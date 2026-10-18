// power-button (was sm8550-powerbuttond): the power button on SM8550
// handhelds (Retroid Pocket 6, Retroid Pocket Nova, AYN Thor).
//
// The Frame image sets logind HandlePowerKey=ignore because its
// steamos-powerbuttond handles the key, but that daemon is
// Requisite=steamvr.service (VR only, masked here). On the Pocket FIT pbosd
// takes over; nothing did on SM8550, so the button did nothing.
//
// Steam Deck behavior (same as pbosd's power_press):
//   Game Mode     short press -> Steam's sleep, long press -> Steam's power menu
//   Desktop Mode  short press -> suspend, long press -> Plasma's shut down prompt
// The wake press from sleep is not seen here: the kernel (pm8941-pwrkey patch
// 0018) drops key events from suspend until 3 s after resume. Resting on the
// charger (sm8550-charge-rest, /run/sm8550-charge-rest) is not a suspend, so
// presses while it rests, and for WAKE_GRACE_S after, are ignored here: that
// press wakes it, and Steam, frozen until then, would read a sleep request on
// waking.
//
// Events, not polling: the pmic_pwrkey evdev device (found when its input
// uevent arrives, not by a 2 s retry), a timer for the long press, inotify on
// /run for the charge-rest flag, and the resting process's exit (pidfd) for
// when the rest ends. The commands run on a thread of their own, so the loop
// never waits for pgrep, Steam or systemctl.

#include <fcntl.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

constexpr double LONG_PRESS_S = 1.0;
constexpr double WAKE_GRACE_S = 2.5;
const char* const USER = "steamos";
const char* const RUN_DIR = "/run";
const char* const CHARGE_REST_NAME = "sm8550-charge-rest";
const char* const CHARGE_REST = "/run/sm8550-charge-rest";
const char* const KEY_NAME = "pmic_pwrkey";

double monotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// The process is alive: it exists and is not a zombie (a resting process
// that died but was not reaped yet is not resting).
bool alive(const std::string& pid) {
    auto stat = pq::read_file("/proc/" + pid + "/stat");
    if (!stat) return false;
    size_t paren = stat->rfind(')');
    if (paren == std::string::npos || paren + 2 >= stat->size()) return false;
    char state = (*stat)[paren + 2];
    return state != 'Z' && state != 'X';
}

// The PID in /run/sm8550-charge-rest while sm8550-charge-rest is resting
// (the flag counts only while that process lives); empty when not resting.
std::string resting_pid() {
    std::string pid = pq::rd(CHARGE_REST);
    if (!pq::is_digits(pid) || pid.size() > 9 || !alive(pid)) return "";
    return pid;
}

// /dev/input/eventN of the PMIC's power key, or empty.
std::string find_pwrkey() {
    for (const auto& ev : pq::glob("/sys/class/input/event*"))
        if (pq::rd(ev + "/device/name") == KEY_NAME) return "/dev/input/" + pq::basename(ev);
    return "";
}

std::vector<std::string> user_env(uid_t uid) {
    std::string u = std::to_string(uid);
    return {"HOME=/home/" + std::string(USER),
            "USER=" + std::string(USER),
            "PATH=/usr/bin:/bin",
            "DISPLAY=:0",
            "XDG_RUNTIME_DIR=/run/user/" + u,
            "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" + u + "/bus"};
}

// Started, and waited for on this (worker) thread; nothing is read back.
pq::RunResult run(const std::vector<std::string>& argv, std::vector<std::string> env = {}) {
    pq::RunOpts o;
    o.capture = false;
    o.timeout = 300;
    o.env = std::move(env);
    return pq::run(argv, o);
}

void run_as_user(std::vector<std::string> cmd) {
    pq::User u = pq::user_named(USER);
    if (!u.ok) return;
    std::vector<std::string> argv = {"runuser", "-u", USER, "--"};
    argv.insert(argv.end(), cmd.begin(), cmd.end());
    run(argv, user_env(u.uid));
}

bool desktop_running() { return run({"pgrep", "-x", "plasmashell"}).code == 0; }

// On a worker thread: pgrep and the command it picks.
void power_press(bool long_press) {
    if (!desktop_running()) {
        pq::log("power-button: Game Mode: %s press", long_press ? "long" : "short");
        run_as_user({"/usr/bin/steam", "-ifrunning", long_press ? "steam://longpowerpress" : "steam://shortpowerpress"});
        return;
    }
    if (!long_press) {
        pq::log("power-button: Desktop: suspend");
        run({"systemctl", "suspend"});
        return;
    }
    pq::log("power-button: Desktop: shut down prompt");
    run_as_user({"qdbus6", "org.kde.LogoutPrompt", "/LogoutPrompt", "org.kde.LogoutPrompt.promptShutDown"});
}

class PowerButton : public Module {
public:
    const char* name() const override { return "power-button"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        loop.watch(RUN_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_Q_OVERFLOW,
                   [this](uint32_t mask, const std::string& name) {
                       if (name == CHARGE_REST_NAME || (mask & IN_Q_OVERFLOW)) check_rest();
                   });
        check_rest();
        loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) {
            if (fd_ >= 0) return;
            auto sub = env.find("SUBSYSTEM"), act = env.find("ACTION"), dev = env.find("DEVNAME");
            bool event_added = sub != env.end() && sub->second == "input" && act != env.end() &&
                               act->second == "add" && dev != env.end() && dev->second.rfind("input/event", 0) == 0;
            if (env.empty() || event_added) open_key();  // empty: some were dropped, look anyway
        });
        open_key();
        if (fd_ < 0) pq::log("power-button: waiting for %s", KEY_NAME);
    }

    void stop() override {
        if (long_timer_) loop_->cancel(long_timer_);
        long_timer_ = 0;
    }

private:
    void open_key() {
        if (fd_ >= 0) return;
        std::string path = find_pwrkey();
        if (path.empty()) return;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            // Found but not openable yet: once more in a moment.
            pq::log("power-button: open %s: %s", path.c_str(), std::strerror(errno));
            if (!retry_) retry_ = loop_->after(2.0, [this] {
                retry_ = 0;
                open_key();
            });
            return;
        }
        fd_ = fd;
        pq::log("power-button: watch %s", path.c_str());
        loop_->on_readable(fd_, [this] { read_key(); });
    }

    void read_key() {
        input_event evs[16];
        while (fd_ >= 0) {
            ssize_t n = read(fd_, evs, sizeof evs);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (n <= 0) {
                // The device went away (ENODEV): closing it also takes it
                // out of the loop's epoll; its uevent brings it back.
                pq::log("power-button: lost the power key (%s)", n < 0 ? std::strerror(errno) : "EOF");
                close(fd_);
                fd_ = -1;
                cancel_press();
                open_key();
                return;
            }
            double now = monotonic();
            check_rest();  // as the Python version read the flag on each event
            for (ssize_t i = 0; i < n / static_cast<ssize_t>(sizeof(input_event)); ++i) {
                const auto& e = evs[i];
                if (e.type != EV_KEY || e.code != KEY_POWER || (e.value != 0 && e.value != 1)) continue;
                if (now < grace_until_) {
                    cancel_press();
                } else if (e.value == 1) {
                    cancel_press();
                    down_ = true;
                    long_timer_ = loop_->after(LONG_PRESS_S, [this] {
                        long_timer_ = 0;
                        if (!down_ || long_sent_) return;
                        long_sent_ = true;
                        pq::Loop::work([] { power_press(true); });
                    });
                } else if (down_) {
                    if (!long_sent_) pq::Loop::work([] { power_press(false); });
                    cancel_press();
                }
            }
        }
    }

    void cancel_press() {
        down_ = false;
        long_sent_ = false;
        if (long_timer_) loop_->cancel(long_timer_);
        long_timer_ = 0;
    }

    // Is sm8550-charge-rest resting? While it is, presses are ignored, and
    // for WAKE_GRACE_S after it stops (its process ends or the flag goes).
    void check_rest() {
        std::string pid = resting_pid();
        double now = monotonic();
        if (!pid.empty()) {
            if (!resting_) cancel_press();
            resting_ = true;
            grace_until_ = std::max(grace_until_, now + WAKE_GRACE_S);
            if (pid != exit_watched_) {
                exit_watched_ = pid;
                if (!loop_->on_exit(static_cast<pid_t>(std::stol(pid)), [this, pid] {
                        if (exit_watched_ == pid) exit_watched_.clear();
                        check_rest();
                    }))
                    exit_watched_.clear();
            }
            return;
        }
        if (resting_) {
            resting_ = false;
            grace_until_ = now + WAKE_GRACE_S;
        }
    }

    pq::Loop* loop_ = nullptr;
    int fd_ = -1;
    uint64_t long_timer_ = 0, retry_ = 0;
    bool down_ = false, long_sent_ = false, resting_ = false;
    double grace_until_ = 0;
    std::string exit_watched_;  // the resting PID whose exit we wait for
};

}  // namespace

std::unique_ptr<Module> make_power_button() { return std::make_unique<PowerButton>(); }

}  // namespace pp
