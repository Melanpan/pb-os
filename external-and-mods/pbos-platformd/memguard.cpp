// memguard: when memory runs out, background work goes before the game,
// gamescope, Steam and the sound server (as Apple's jetsam does on iOS).
// systemd-oomd stays off: it kills a whole cgroup, the game's included.
//
// Two kernel PSI triggers on /proc/pressure/memory (POLLPRI when they fire;
// nothing runs while memory is fine):
//  - early, "some 150000 2000000" (7.5% of 2 s stalled): pins the OOM
//    priorities that units cannot set statically (below), and keeps a
//    recovery from starting while memory is still tight.
//  - act, "some 500000 2000000" (25% of 2 s): when it fires in two windows
//    in a row (about 4 s of heavy stalls), the next background unit of
//    EXPENDABLE stops, one per streak, but only while MemAvailable is under
//    LOW_PCT of RAM: one process thrashing inside its own memory limit
//    stalls just as hard while the rest of RAM is free. Barry Launcher's shelld closes its
//    frozen apps at "some 300000 2000000" (15%), every window, so it always
//    acts first; this module never touches Barry's (or Flatpak's) app scopes.
// Once no trigger has fired for CALM_S, the units it stopped start again.
//
// OOM priorities (the kernel's last resort): the units' drop-ins
// (sm8650-overlay, NN-pbos-oom.conf) set the system side. User units cannot
// go below the user manager's 100 (user@.service), so this module (root)
// sets the compositor and sound server units' processes to PROTECT_ADJ.
// The Steam client starts every game at 900, so the expendable units sit at
// 1000 (999 for Decky's plugin backends) to go before it.
// Decky's loader gets LOADER_ADJ, above its plugin backends' 999: Decky
// does not restart a backend that dies (and spins on it), while systemd
// restarts the whole loader (Restart=always).
//
// Tests only: PBOS_MEMGUARD_PSI_EARLY / PBOS_MEMGUARD_PSI_ACT (a FIFO in
// place of the PSI file: each write is one firing), PBOS_MEMGUARD_SYSTEMCTL,
// PBOS_MEMGUARD_CALM_S, PBOS_MEMGUARD_GAP_S, PBOS_MEMGUARD_LOW_PCT.

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

constexpr const char* PSI = "/proc/pressure/memory";
constexpr const char* EARLY_TRIGGER = "some 150000 2000000";
constexpr const char* ACT_TRIGGER = "some 500000 2000000";
// Two act firings this close together are one streak (the window is 2 s and
// the kernel fires at most once per window).
constexpr double GAP_S = 3.0;
constexpr double CALM_S = 120;
constexpr double FREED_AFTER_S = 2;
constexpr double PIN_EVERY_S = 10;
constexpr int PROTECT_ADJ = -500;
constexpr int LOADER_ADJ = 1000;
// Act only while MemAvailable is under this share of MemTotal (percent).
constexpr double LOW_PCT = 10;
constexpr const char* LOADER = "plugin_loader.service";

// In order: the first goes first. Each is started again once calm.
const char* const EXPENDABLE[] = {
    "gpu-trace.service",  // Valve's GPU capture daemon (idle unless asked)
    LOADER,               // Decky and its plugin backends
};

// User units whose processes never start a game: safe to protect.
const char* const PROTECTED[] = {
    "/gamescope-session.service", "/sm8550-bottom-session.service",  // gamescope (Thor: both screens)
    "/pipewire.service", "/pipewire-pulse.service", "/wireplumber.service",
};

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string env_or(const char* name, const char* def) {
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}

double env_num(const char* name, double def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atof(v) : def;
}

// A /proc/meminfo value ("MemAvailable:") in MB, -1 when unreadable.
long meminfo_mb(const char* key) {
    auto s = pq::read_file("/proc/meminfo");
    if (!s) return -1;
    size_t n = std::strlen(key);
    for (const auto& line : pq::split_lines(*s))
        if (!line.compare(0, n, key)) return std::atol(line.c_str() + n) / 1024;
    return -1;
}

long mem_available_mb() { return meminfo_mb("MemAvailable:"); }

// /proc/pressure/memory on one line, for the log.
std::string psi_now() {
    auto s = pq::read_file(PSI);
    if (!s) return "?";
    std::string out;
    for (const auto& line : pq::split_lines(*s)) {
        if (line.empty()) continue;
        if (!out.empty()) out += " | ";
        out += line;
    }
    return out;
}

int read_adj(int pid) {
    auto s = pq::read_file("/proc/" + std::to_string(pid) + "/oom_score_adj");
    return s ? std::atoi(s->c_str()) : 0;
}

// Lowers (or, raise=true, raises) a process's oom_score_adj to adj.
void set_adj(int pid, int adj, bool raise, const char* why) {
    int cur = read_adj(pid);
    if (raise ? cur >= adj : cur <= adj) return;
    if (pq::write_sysfs("/proc/" + std::to_string(pid) + "/oom_score_adj", std::to_string(adj)))
        pq::log("memguard: pid %d (%s) oom_score_adj %d -> %d", pid, why, cur, adj);
}

bool ends_with(const std::string& s, const char* tail) {
    size_t n = std::strlen(tail);
    return s.size() >= n && !s.compare(s.size() - n, n, tail);
}

// Opens one trigger: the PSI file (POLLPRI) or a test FIFO (readable).
int open_trigger(const std::string& path, const char* trigger, bool& fifo) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    fifo = S_ISFIFO(st.st_mode);
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);  // a FIFO's own writer too: no HUP
    if (fd < 0) return -1;
    if (!fifo && write(fd, trigger, std::strlen(trigger) + 1) < 0) {
        pq::log("memguard: trigger \"%s\" on %s: %s", trigger, path.c_str(), std::strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

class MemGuard : public Module {
public:
    const char* name() const override { return "memguard"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        systemctl_ = env_or("PBOS_MEMGUARD_SYSTEMCTL", "systemctl");
        calm_s_ = env_num("PBOS_MEMGUARD_CALM_S", CALM_S);
        gap_s_ = env_num("PBOS_MEMGUARD_GAP_S", GAP_S);
        low_pct_ = env_num("PBOS_MEMGUARD_LOW_PCT", LOW_PCT);
        // Our own pages stay in memory: this has to run when nothing else can.
        if (mlockall(MCL_CURRENT) != 0) pq::log("memguard: mlockall: %s", std::strerror(errno));
        early_ = arm(env_or("PBOS_MEMGUARD_PSI_EARLY", PSI), EARLY_TRIGGER, [this] { on_early(); });
        act_ = arm(env_or("PBOS_MEMGUARD_PSI_ACT", PSI), ACT_TRIGGER, [this] { on_act(); });
        if (act_ < 0) pq::log("memguard: no memory pressure trigger; only the static OOM priorities apply");
        pin();
    }

    void stop() override {
        // Leave nothing stopped behind us (unless the system is going down).
        if (!stopped_.empty() && pq::trim(pq::run({systemctl_, "is-system-running"}).out) != "stopping")
            for (auto it = stopped_.rbegin(); it != stopped_.rend(); ++it)
                if (std::find(tried_.begin(), tried_.end(), *it) == tried_.end())
                    pq::run({systemctl_, "start", "--no-block", *it});
        stopped_.clear();
    }

private:
    int arm(const std::string& path, const char* trigger, std::function<void()> fired) {
        bool fifo = false;
        int fd = open_trigger(path, trigger, fifo);
        if (fd < 0) {
            pq::log("memguard: no trigger on %s", path.c_str());
            return -1;
        }
        auto cb = [fd, fifo, fired] {
            if (fifo) {
                char buf[256];
                while (read(fd, buf, sizeof buf) > 0) {}
            }
            fired();
        };
        if (fifo) loop_->on_readable(fd, cb);
        else loop_->on_priority(fd, cb);
        pq::log("memguard: trigger on %s%s", path.c_str(), fifo ? " (FIFO)" : (std::string(" (") + trigger + ")").c_str());
        return fd;
    }

    void on_early() {
        pin();
        recheck_calm();
    }

    void on_act() {
        double now = now_s();
        bool streak = last_act_ > 0 && now - last_act_ <= gap_s_;
        last_act_ = now;
        recheck_calm();
        if (!streak || busy_) return;
        last_act_ = 0;  // the next action needs a streak of its own
        long avail = mem_available_mb(), total = meminfo_mb("MemTotal:");
        if (avail >= 0 && total > 0 && avail * 100.0 >= total * low_pct_) {
            pq::log("memguard: memory pressure (%s), but MemAvailable %ld of %ld MB: nothing stopped", psi_now().c_str(),
                    avail, total);
            return;
        }
        act();
    }

    void act() {
        for (const char* unit : EXPENDABLE) {
            bool done = false;
            for (const auto& s : stopped_) done |= s == unit;
            if (done) continue;
            busy_ = true;
            std::string u = unit, psi = psi_now();
            long before = mem_available_mb();
            pq::Loop::work([this, u, psi, before] {
                bool was = pq::run({systemctl_, "is-active", "--quiet", u}).code == 0;
                bool ok = was && pq::run({systemctl_, "stop", u}).code == 0;
                loop_->post([this, u, psi, before, was, ok] {
                    busy_ = false;
                    if (!was) {
                        // Not running: nothing to stop; the next one may.
                        tried_.push_back(u);
                        stopped_.push_back(u);
                        act();
                        return;
                    }
                    if (!ok) {
                        pq::log("memguard: memory pressure (%s), stopping %s failed", psi.c_str(), u.c_str());
                        return;
                    }
                    stopped_.push_back(u);
                    pq::log("memguard: memory pressure (%s), MemAvailable %ld MB: stopped %s", psi.c_str(), before,
                            u.c_str());
                    loop_->after(FREED_AFTER_S, [u, before] {
                        long after = mem_available_mb();
                        pq::log("memguard: %s stopped, MemAvailable %ld -> %ld MB (%+ld MB)", u.c_str(), before, after,
                                after - before);
                    });
                    recheck_calm();
                });
            });
            return;
        }
        if (!said_nothing_) {
            said_nothing_ = true;
            pq::log("memguard: memory pressure (%s), MemAvailable %ld MB: nothing left to stop; the kernel's OOM "
                    "order applies",
                    psi_now().c_str(), mem_available_mb());
        }
    }

    // Each firing pushes the recovery back by CALM_S.
    void recheck_calm() {
        if (calm_timer_) loop_->cancel(calm_timer_);
        calm_timer_ = 0;
        if (stopped_.empty() && !said_nothing_) return;
        calm_timer_ = loop_->after(calm_s_, [this] {
            calm_timer_ = 0;
            said_nothing_ = false;
            auto units = stopped_;
            auto skip = tried_;
            stopped_.clear();
            tried_.clear();
            pq::Loop::work([this, units, skip] {
                std::vector<std::string> started;
                for (auto it = units.rbegin(); it != units.rend(); ++it) {
                    bool was_off = false;
                    for (const auto& s : skip) was_off |= s == *it;
                    if (was_off) continue;
                    pq::run({systemctl_, "start", *it});
                    started.push_back(*it);
                }
                loop_->post([this, started] {
                    for (const auto& u : started)
                        pq::log("memguard: calm for %.0f s, MemAvailable %ld MB: started %s", calm_s_,
                                mem_available_mb(), u.c_str());
                });
            });
        });
    }

    // The priorities no unit file can set (see the top). At start and on
    // the early trigger, at most every PIN_EVERY_S.
    void pin() {
        double now = now_s();
        if (pinned_at_ > 0 && now - pinned_at_ < PIN_EVERY_S) return;
        pinned_at_ = now;
        DIR* d = opendir("/proc");
        if (!d) return;
        while (dirent* e = readdir(d)) {
            if (!pq::is_digits(e->d_name)) continue;
            int pid = std::atoi(e->d_name);
            auto cg = pq::read_file(std::string("/proc/") + e->d_name + "/cgroup");
            if (!cg) continue;
            for (const auto& line : pq::split_lines(*cg)) {
                if (line.compare(0, 3, "0::")) continue;  // cgroup v2's line
                if (line.find("/user@") == std::string::npos) continue;
                for (const char* unit : PROTECTED)
                    if (ends_with(line, unit)) set_adj(pid, PROTECT_ADJ, false, unit + 1);
            }
        }
        closedir(d);
        if (pinning_loader_) return;
        pinning_loader_ = true;
        pq::Loop::work([this] {
            auto r = pq::run({systemctl_, "show", "-p", "MainPID", "--value", LOADER});
            int pid = r.code == 0 ? std::atoi(pq::trim(r.out).c_str()) : 0;
            loop_->post([this, pid] {
                pinning_loader_ = false;
                if (pid > 1) set_adj(pid, LOADER_ADJ, true, LOADER);
            });
        });
    }

    pq::Loop* loop_ = nullptr;
    std::string systemctl_;
    double calm_s_ = CALM_S, gap_s_ = GAP_S, low_pct_ = LOW_PCT;
    int early_ = -1, act_ = -1;
    double last_act_ = 0, pinned_at_ = 0;
    bool busy_ = false, said_nothing_ = false, pinning_loader_ = false;
    uint64_t calm_timer_ = 0;
    std::vector<std::string> stopped_;  // stopped (or found not running), in order
    std::vector<std::string> tried_;    // of those, found not running: not started again
};

}  // namespace

std::unique_ptr<Module> make_memguard() { return std::make_unique<MemGuard>(); }

}  // namespace pp
