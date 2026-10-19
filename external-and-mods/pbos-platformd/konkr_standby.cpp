// konkr-standby (was /usr/lib/konkr/konkr-standby): safe "sleep" for the
// KONKR Pocket FIT and the AYANEO Pocket S2. Run as pbos-platformd
// konkr-standby by konkr-suspend, systemd-suspend.service's ExecStart.
//
// Kernel suspend (s2idle and deep) does not reliably resume on the 7.1.2
// kernel: the device looked powered off and needed a forced restart. Steam's
// Sleep goes through logind → systemd-suspend.service, which the port
// overrides to run this instead. It:
//
//   - pauses the running game (SIGSTOP on the Steam-launched process tree),
//   - suspends audio, detaches the touch panel, unloads Wi-Fi and has
//     gamescope power the panel off (konkr-sleep standby-pre),
//   - freezes the user session (Steam, gamescope, games, Android), so nothing
//     in userspace wakes the CPUs. PipeWire, WirePlumber and D-Bus keep
//     running (idle, outputs suspended): frozen along with the apps, PipeWire
//     woke hours out of date and audio lagged and crackled for minutes,
//   - turns the panel backlight and the LEDs off,
//   - takes the big cores offline, drops the little ones to their lowest clock
//     and caps the GPU,
//   - blocks until the power button is pressed again (or SIGUSR1; for testing
//     over SSH run it with KONKR_STANDBY_KEEP_WIFI=1), then restores it all:
//     CPUs and audio first, then the apps, then the screen.
//
// Steam sees a normal suspend/resume cycle (PrepareForSleep false → true →
// false). Nothing enters kernel suspend, so there is nothing to fail to wake.
//
// The Python version's start and exit (~0.1-0.2 s of interpreter on the
// little cores) were added to every standby entry and exit. Here the steps,
// their order, every write, command and signal are the Python version's; the
// one change is the freeze wait: the kernel announces each cgroup's "frozen 1"
// as a modification of cgroup.events, so it waits on inotify (still up to
// 2 s) instead of reading the files every 50 ms.

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "module.h"
#include "util.h"

extern char** environ;

namespace pp {

namespace {

const char* const FLAG = "/run/konkr-standby";
const char* const KONKR_SLEEP = "/usr/lib/konkr/konkr-sleep";
const char* const CPU = "/sys/devices/system/cpu";

double monotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// time.sleep(): the whole time, signals or not.
void sleep_s(double s) {
    timespec ts{static_cast<time_t>(s), static_cast<long>((s - std::floor(s)) * 1e9)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void log(const std::string& msg) {
    std::printf("konkr-standby: %s\n", msg.c_str());
    std::fflush(stdout);
}

// Python's open(p, "w").write(str(v)): created if missing, truncated, one
// write; false when the open or the write is refused.
bool wr_ok(const std::string& p, const std::string& v) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    bool ok = true;
    if (!v.empty()) {
        ssize_t n;
        do n = write(fd, v.data(), v.size());
        while (n < 0 && errno == EINTR);
        ok = n == static_cast<ssize_t>(v.size());
    }
    if (close(fd) != 0 && errno != EINTR) ok = false;
    return ok;
}

void wr(const std::string& p, const std::string& v) { wr_ok(p, v); }

// glob.glob(dir + "/" + pat) with the wildcard in the last part only: in the
// folder's own order (as Python's, unsorted), hidden names skipped.
std::vector<std::string> pyglob(const std::string& dir, const char* pat) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d))
        if (e->d_name[0] != '.' && fnmatch(pat, e->d_name, 0) == 0) out.push_back(dir + "/" + e->d_name);
    closedir(d);
    return out;
}

// The names in a folder in its own order (os.listdir), or empty.
std::vector<std::string> listdir_raw(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d))
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) out.emplace_back(e->d_name);
    closedir(d);
    return out;
}

bool to_long(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtol(s.c_str(), &end, 10);
    return errno == 0 && end && *end == 0;
}

// subprocess.run(cmd, timeout=20, stdout=DEVNULL): stderr to the journal as
// before; killed after the timeout (timeout < 0: none, as os.system).
int run(const std::vector<std::string>& argv, double timeout, bool quiet_err = false) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (quiet_err) posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, dfl;
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGXFSZ);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return 1;
    int status = 0;
    if (timeout >= 0) {
        int pfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
        double end = monotonic() + timeout;
        bool done = false;
        while (pfd >= 0) {
            double left = end - monotonic();
            if (left <= 0) break;
            pollfd p{pfd, POLLIN, 0};
            int r = poll(&p, 1, static_cast<int>(std::ceil(left * 1000)));
            if (r > 0) {
                done = true;
                break;
            }
            if (r < 0 && errno != EINTR) break;
        }
        if (pfd >= 0) close(pfd);
        if (!done && pfd >= 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return 1;
        }
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

// CPUs above the lowest capacity (the A520s stay online).
std::vector<int> big_cpus() {
    std::map<int, long> caps;
    for (const auto& c : pyglob(CPU, "cpu[0-9]*")) {
        long n = 0;
        if (!to_long(pq::basename(c).substr(3), n)) continue;
        std::string v = pq::rd(c + "/cpu_capacity", "0");
        long cap = 0;
        caps[static_cast<int>(n)] = pq::is_digits(v) && to_long(v, cap) ? cap : 0;
    }
    std::vector<int> out;
    if (caps.empty()) return out;
    long low = caps.begin()->second;
    for (auto& [n, v] : caps) low = std::min(low, v);
    for (auto& [n, v] : caps)
        if (v > low && n != 0) out.push_back(n);
    return out;  // std::map: sorted
}

// The processes under Steam's reaper (SteamLaunch), parents before children.
std::vector<pid_t> game_pids() {
    static const std::string MARK("reaper\0SteamLaunch\0", 19);
    std::map<long, std::vector<long>> children;
    std::vector<long> roots;
    for (const auto& p : listdir_raw("/proc")) {
        if (!pq::is_digits(p)) continue;
        auto st = pq::read_file("/proc/" + p + "/stat");
        if (!st) continue;
        size_t paren = st->rfind(')');
        if (paren == std::string::npos) continue;
        std::vector<std::string> f;
        size_t i = paren + 2;
        while (i < st->size() && f.size() < 2) {
            while (i < st->size() && std::isspace(static_cast<unsigned char>((*st)[i]))) ++i;
            size_t j = i;
            while (j < st->size() && !std::isspace(static_cast<unsigned char>((*st)[j]))) ++j;
            if (j > i) f.push_back(st->substr(i, j - i));
            i = j;
        }
        long ppid = 0, pid = 0;
        if (f.size() < 2 || !to_long(f[1], ppid) || !to_long(p, pid)) continue;
        auto cmd = pq::read_file("/proc/" + p + "/cmdline");
        if (!cmd) continue;
        children[ppid].push_back(pid);
        if (cmd->find(MARK) != std::string::npos) roots.push_back(pid);
    }
    std::vector<pid_t> out;
    std::vector<long> stack = roots;
    while (!stack.empty()) {
        long pid = stack.back();
        stack.pop_back();
        auto it = children.find(pid);
        if (it == children.end()) continue;
        for (long c : it->second) {
            out.push_back(static_cast<pid_t>(c));
            stack.push_back(c);
        }
    }
    return out;
}

int power_key_fd() {
    for (const auto& e : pyglob("/sys/class/input", "event*"))
        if (pq::rd(e + "/device/name") == "pmic_pwrkey")
            return open(("/dev/input/" + pq::basename(e)).c_str(), O_RDONLY | O_CLOEXEC);
    return -2;  // none (-1: there, but it cannot be opened)
}

// Keep audio and D-Bus running while freezing the graphical user.
bool keep_running(const std::string& unit) {
    static const char* const KEEP[] = {"pipewire.service",   "pipewire-pulse.service",  "wireplumber.service",
                                       "dbus-broker.service", "at-spi-dbus-bus.service", "init.scope"};
    for (const char* k : KEEP)
        if (unit == k) return true;
    return false;
}

// posixpath.normpath for an absolute path.
std::string normpath(const std::string& path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out;
    for (auto& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

bool lexists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

// cgroups to freeze: each session scope, every user-manager slice but
// session.slice, and every session.slice unit but the ones kept running.
std::vector<std::string> freeze_targets() {
    std::vector<std::string> out;
    pq::User u = pq::user_named("steamos");
    if (!u.ok) return {};
    std::string own;
    bool found = false;
    if (auto cg = pq::read_file("/proc/self/cgroup")) {
        for (const auto& line : pq::split_lines(*cg)) {
            if (line.rfind("0::", 0) != 0) continue;
            own = pq::trim(std::string_view(line).substr(3));
            found = true;
            break;
        }
    }
    if (!found) return {};
    own = normpath("/sys/fs/cgroup" + own);
    std::string user = "/sys/fs/cgroup/user.slice/user-" + std::to_string(u.uid) + ".slice";
    if (lexists(user)) {
        for (const auto& child : pq::listdir(user)) {
            std::string path = user + "/" + child;
            if (!pq::is_dir(path)) continue;
            bool manager = child.rfind("user@", 0) == 0 && child.size() >= 13 &&
                           child.compare(child.size() - 8, 8, ".service") == 0;
            if (!manager) {
                out.push_back(path);  // session-N.scope
                continue;
            }
            for (const auto& sub : pq::listdir(path)) {
                std::string spath = path + "/" + sub;
                if (!pq::is_dir(spath) || keep_running(sub)) continue;
                if (sub != "session.slice") {
                    out.push_back(spath);  // app.slice, …
                    continue;
                }
                for (const auto& unit : pq::listdir(spath)) {
                    std::string upath = spath + "/" + unit;
                    if (pq::is_dir(upath) && !keep_running(unit)) out.push_back(upath);
                }
            }
        }
    }
    // A caller inside a graphical session must never freeze itself or the
    // session coordinating its wake-up. Root SSH sessions are not selected.
    std::vector<std::string> keep;
    for (auto& path : out)
        if (own != path && own.rfind(path + "/", 0) != 0) keep.push_back(path);
    return keep;
}

bool all_frozen(const std::vector<std::string>& cgroups) {
    for (const auto& cg : cgroups)
        if (pq::rd(cg + "/cgroup.events").find("frozen 1") == std::string::npos) return false;
    return true;
}

// Write cgroup.freeze; returns the cgroups that took it. Freezing waits up
// to 2 s for the tasks to reach the freezer: on inotify of each
// cgroup.events (the kernel's "file modified" when frozen changes), or
// every 50 ms if one cannot be watched.
std::vector<std::string> set_frozen(const std::vector<std::string>& cgroups, bool frozen) {
    std::vector<std::string> done;
    for (const auto& cg : cgroups)
        if (wr_ok(cg + "/cgroup.freeze", frozen ? "1" : "0")) done.push_back(cg);
    if (!frozen) return done;
    double end = monotonic() + 2.0;
    int in = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    bool events = in >= 0;
    for (const auto& cg : done)
        if (events && inotify_add_watch(in, (cg + "/cgroup.events").c_str(), IN_MODIFY) < 0) events = false;
    while (monotonic() < end && !all_frozen(done)) {
        if (!events) {
            sleep_s(0.05);
            continue;
        }
        double left = end - monotonic();
        pollfd p{in, POLLIN, 0};
        if (left > 0 && poll(&p, 1, static_cast<int>(std::ceil(left * 1000))) > 0) {
            char buf[4096];
            while (read(in, buf, sizeof buf) > 0) {
            }
        }
    }
    if (in >= 0) close(in);
    return done;
}

int wake_r = -1, wake_w = -1;

void on_usr1(int) {
    int e = errno;
    [[maybe_unused]] ssize_t n = write(wake_w, "x", 1);
    errno = e;
}

// Block until a full press+release of the power key, or SIGUSR1. False
// when the key cannot be read (the Python version raised there).
bool wait_power_press(int fd) {
    bool pressed = false;
    input_event ev[64];
    for (;;) {
        pollfd p[2] = {{fd, POLLIN, 0}, {wake_r, POLLIN, 0}};
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (p[1].revents) {
            log("wake requested (SIGUSR1)");
            return true;
        }
        if (!p[0].revents) continue;
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            std::fprintf(stderr, "konkr-standby: reading the power key: %s\n", n < 0 ? std::strerror(errno) : "end of file");
            return false;
        }
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i) {
            if (ev[i].type != EV_KEY || ev[i].code != KEY_POWER) continue;
            if (ev[i].value == 1) {
                pressed = true;
            } else if (ev[i].value == 0 && pressed) {
                log("wake: power key");
                return true;
            }
        }
    }
}

// Saved values, in the order they were first saved (a Python dict).
struct Saved {
    std::vector<std::pair<std::string, std::string>> items;
    void set(const std::string& k, std::string v) {
        for (auto& [key, val] : items)
            if (key == k) {
                val = std::move(v);
                return;
            }
        items.emplace_back(k, std::move(v));
    }
};

bool is_clock(const std::string& path) {
    return path.find("/cpufreq/") != std::string::npos || path.find("/devfreq/") != std::string::npos;
}

}  // namespace

int konkr_standby_main(int, char**) {
    // The name the script had (its comm), so `pkill -USR1 konkr-standby` still wakes it.
    prctl(PR_SET_NAME, "konkr-standby", 0, 0, 0);
    int fd = power_key_fd();
    if (fd == -2) {
        log("no power key device; not entering standby");
        return 1;
    }
    if (fd < 0) {
        std::fprintf(stderr, "konkr-standby: cannot open the power key: %s\n", std::strerror(errno));
        return 1;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return 1;
    wake_r = pipefd[0];
    wake_w = pipefd[1];
    fcntl(wake_w, F_SETFL, O_NONBLOCK);
    if (!wr_ok(FLAG, "")) {
        std::fprintf(stderr, "konkr-standby: cannot create %s: %s\n", FLAG, std::strerror(errno));
        return 1;
    }
    Saved saved;
    std::vector<pid_t> paused;
    std::vector<int> offlined;
    std::vector<std::string> frozen;
    bool ok = true;

    // Drain the press that triggered sleep, so its release is not the wake.
    sleep_s(0.6);
    {
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        char buf[sizeof(input_event) * 64];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)) {
        }
        if (n < 0 && errno != EAGAIN) ok = false;  // the Python version raised here
        fcntl(fd, F_SETFL, fl);
    }

    if (ok) {
        struct sigaction sa {};
        sa.sa_handler = on_usr1;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, nullptr);
        for (pid_t pid : game_pids())
            if (kill(pid, SIGSTOP) == 0) paused.push_back(pid);
        for (const auto& bl : pyglob("/sys/class/backlight", "*")) {
            saved.set(bl + "/brightness", pq::rd(bl + "/brightness"));
            wr(bl + "/brightness", "0");
        }
        for (const auto& led : pyglob("/sys/class/leds", "*")) {
            if (led.find("joystick") != std::string::npos || led.find("power-led") != std::string::npos) {
                saved.set(led + "/brightness", pq::rd(led + "/brightness"));
                wr(led + "/brightness", "0");
            }
        }
        run({KONKR_SLEEP, "standby-pre"}, 20);
        sleep_s(0.3);  // let gamescope commit the panel-off
        frozen = set_frozen(freeze_targets(), true);
        for (const auto& pol : pyglob(std::string(CPU) + "/cpufreq", "policy*")) {
            // pbosd's touch boost may still hold the floor up (it drops it
            // within 0.5 s of this flag); a max below the min would be refused.
            wr(pol + "/scaling_min_freq", pq::rd(pol + "/cpuinfo_min_freq"));
            saved.set(pol + "/scaling_max_freq", pq::rd(pol + "/scaling_max_freq"));
            wr(pol + "/scaling_max_freq", pq::rd(pol + "/cpuinfo_min_freq"));
        }
        for (int cpu : big_cpus())
            if (wr_ok(std::string(CPU) + "/cpu" + std::to_string(cpu) + "/online", "0")) offlined.push_back(cpu);
        for (const auto& gpu : pyglob("/sys/class/devfreq", "*gpu*")) {
            saved.set(gpu + "/max_freq", pq::rd(gpu + "/max_freq"));
            std::vector<unsigned long long> freqs;
            for (const auto& f : pq::split_lines(pq::rd(gpu + "/available_frequencies"))) {
                size_t i = 0;
                while (i < f.size()) {
                    size_t j = f.find_first_of(" \t", i);
                    if (j == std::string::npos) j = f.size();
                    std::string w = f.substr(i, j - i);
                    if (pq::is_digits(w)) freqs.push_back(std::strtoull(w.c_str(), nullptr, 10));
                    i = j + 1;
                }
            }
            if (!freqs.empty()) wr(gpu + "/max_freq", std::to_string(*std::min_element(freqs.begin(), freqs.end())));
        }
        std::string how = frozen.empty()
                              ? "session NOT frozen"
                              : std::to_string(frozen.size()) + " session cgroups frozen (audio kept running)";
        std::string cpus = "[";
        for (size_t i = 0; i < offlined.size(); ++i) cpus += (i ? ", " : "") + std::to_string(offlined[i]);
        cpus += "]";
        log("standby: paused " + std::to_string(paused.size()) + " game processes, " + how + ", cpus " + cpus +
            " offline, screen off");
        ok = wait_power_press(fd);
        if (ok) log("wake");
    }

    // CPUs and clocks first, then audio (PipeWire is still running), then
    // the apps, so they wake to a ready speaker and a current clock.
    for (int cpu : offlined) wr(std::string(CPU) + "/cpu" + std::to_string(cpu) + "/online", "1");
    for (const auto& [path, val] : saved.items)
        if (!val.empty() && is_clock(path)) wr(path, val);
    run({KONKR_SLEEP, "standby-audio-on"}, 20);
    if (!frozen.empty()) set_frozen(frozen, false);
    run({KONKR_SLEEP, "standby-screen-on"}, 20);
    for (const auto& [path, val] : saved.items)
        if (!val.empty() && !is_clock(path)) wr(path, val);
    run({KONKR_SLEEP, "standby-post"}, 20);
    for (pid_t pid : paused) kill(pid, SIGCONT);
    sleep_s(0.5);  // let the wake release settle before pbosd sees keys
    unlink(FLAG);
    close(fd);
    // pbosd re-applies profile clocks/LEDs on SIGHUP
    run({"systemctl", "kill", "-s", "HUP", "pbosd.service"}, -1, true);
    return ok ? 0 : 1;
}

}  // namespace pp
