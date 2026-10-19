// pbosd's game and UI thread boost (see pbosd.h): uclamp.min, the big cores
// for game threads, and BFQ disk priorities, per profile.

#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pbosd.h"
#include "util.h"

namespace pp::pbosd {

namespace {

constexpr double TICK_S = 1;
// Process events come in bursts (a game starting forks and execs dozens of
// processes): one look through /proc this long after the first.
constexpr double RESCAN_DELAY_S = 0.1;

// per-thread uclamp.min for game threads (0..1024)
unsigned game_uclamp(const std::string& p) { return p == "lowpower" ? 0 : 256; }
// The Steam UI path. Mainline EAS parks these bursty, light threads on the
// Cortex-A520s (capacity 216 vs 855 on the big cores) — gamescope and
// Xwayland were measured on cpu1 — which makes every menu feel laggy.
// Android keeps the foreground app on big cores; do the same.
unsigned ui_uclamp(const std::string& p) { return p == "lowpower" ? 128 : 200; }
const std::set<std::string> UI_COMMS = {
    "gamescope-wl", "Xwayland", "steam", "steamwebhelper", "gamescopereaper", "mangoapp", "inputplumber",
    // Desktop Mode: same problem, compositor + shell on the A520s
    "kwin_wayland", "kwin_x11", "plasmashell", "krunner", "konsole", "dolphin", "plasma-discover",
    "systemsettings",
    // Discover installs: the pull, the checksumming and the deploy
    // (comm is truncated to 15 chars)
    "flatpak", "flatpak-system-", "flatpak-session", "ostree"};

// Disk priority (BFQ honors it). The microSD does ~1.6 MB/s of 4K writes;
// a Steam download from the `steam` client process must not queue ahead of
// the UI's and the game's reads.
constexpr int IOPRIO_CLASS_BE = 2;
constexpr int IO_UI = 0, IO_GAME = 2, IO_DOWNLOAD = 7;  // best-effort levels, 0 = highest
const char* const IO_LOW_COMM = "steam";                // does the downloading/installing
constexpr int IOPRIO_WHO_PROCESS = 1;

void set_ioprio(pid_t tid, int level) {
    syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, tid, (IOPRIO_CLASS_BE << 13) | level);
}

// The kernel's struct sched_attr (with uclamp, SCHED_ATTR_SIZE_VER1).
struct SchedAttr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};
constexpr uint64_t SCHED_FLAG_KEEP_ALL = 0x08 | 0x10;
constexpr uint64_t SCHED_FLAG_UTIL_CLAMP_MIN = 0x20;

bool set_uclamp_min(pid_t tid, unsigned value) {
    SchedAttr attr{};
    attr.size = sizeof attr;
    attr.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN;
    attr.sched_util_min = value;
    return syscall(SYS_sched_setattr, tid, &attr, 0) == 0;
}

std::vector<pid_t> tasks(pid_t pid) {
    std::vector<pid_t> out;
    for (const auto& t : pq::listdir("/proc/" + std::to_string(pid) + "/task"))
        if (pq::is_digits(t)) out.push_back(static_cast<pid_t>(std::atol(t.c_str())));
    return out;
}

// The kernel's process events over netlink; -1 when this kernel has none.
int open_proc_events() {
    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_CONNECTOR);
    if (fd < 0) return -1;
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    addr.nl_pid = 0;  // the kernel picks
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    int rcvbuf = 4 << 20;  // a game starting is a burst
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof rcvbuf);
    // nlmsghdr, cn_msg, then the request (cn_msg ends in a flexible array).
    alignas(nlmsghdr) char msg[NLMSG_SPACE(sizeof(cn_msg) + sizeof(proc_cn_mcast_op))]{};
    auto* nl = reinterpret_cast<nlmsghdr*>(msg);
    nl->nlmsg_len = NLMSG_LENGTH(sizeof(cn_msg) + sizeof(proc_cn_mcast_op));
    nl->nlmsg_type = NLMSG_DONE;
    nl->nlmsg_pid = static_cast<uint32_t>(getpid());
    auto* cn = static_cast<cn_msg*>(NLMSG_DATA(nl));
    cn->id.idx = CN_IDX_PROC;
    cn->id.val = CN_VAL_PROC;
    cn->len = sizeof(proc_cn_mcast_op);
    proc_cn_mcast_op op = PROC_CN_MCAST_LISTEN;
    std::memcpy(cn->data, &op, sizeof op);
    if (send(fd, msg, nl->nlmsg_len, 0) != static_cast<ssize_t>(nl->nlmsg_len)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

GameBoost::GameBoost(pq::Loop& loop, bool enabled, bool avoid_little)
    : loop_(loop), enabled_(enabled), avoid_little_(avoid_little), big_(big_cpus()) {}

GameBoost::~GameBoost() {
    if (cn_ >= 0) close(cn_);
}

void GameBoost::start(const std::string& profile) {
    profile_ = profile;
    if (!enabled_) return;
    // Test only (tests/pbosd): the once-a-second look even with the connector.
    if (!env_set("PBOS_PLATFORMD_TEST_PBOSD_SCAN")) {
        cn_ = open_proc_events();
        if (cn_ < 0)
            pq::log("pbosd: games: no process events from this kernel (%s), looking once a second",
                    std::strerror(errno));
    }
    if (cn_ >= 0) {
        loop_.on_readable(cn_, [this] { drain(); });
        look();
    } else {
        tick();
    }
}

void GameBoost::set_profile(const std::string& profile) {
    profile_ = profile;
    done_.clear();
    if (enabled_ && cn_ >= 0) look();  // the scan's own tick redoes it within a second
}

void GameBoost::tick() {
    look();
    loop_.after(TICK_S, [this] { tick(); });
}

void GameBoost::rescan() {
    if (pending_) return;
    pending_ = loop_.after(RESCAN_DELAY_S, [this] {
        pending_ = 0;
        look();
    });
}

// One /proc pass: what each game and UI process gets. Descendants of Steam's
// `reaper SteamLaunch` are the game; UI_COMMS outside it are the UI. comm
// comes from stat, and cmdline is read only for `reaper` (reading three
// files per process cost ~5 % CPU idle in the once-a-second version).
std::map<pid_t, GameBoost::Target> GameBoost::scan(std::set<pid_t>& roots) const {
    std::map<pid_t, std::vector<pid_t>> children;
    std::vector<std::pair<pid_t, std::string>> ui;
    roots.clear();
    for (const auto& p : pq::listdir("/proc")) {
        if (!pq::is_digits(p)) continue;
        auto stat = pq::read_file("/proc/" + p + "/stat");
        if (!stat) continue;
        auto open = stat->find('('), close = stat->rfind(')');
        if (open == std::string::npos || close == std::string::npos || close + 2 >= stat->size()) continue;
        std::string comm = stat->substr(open + 1, close - open - 1);
        // After ") ": the state, then the parent.
        const char* rest = stat->c_str() + close + 2;
        const char* sp = std::strchr(rest, ' ');
        if (!sp) continue;
        pid_t pid = static_cast<pid_t>(std::atol(p.c_str()));
        pid_t ppid = static_cast<pid_t>(std::atol(sp + 1));
        children[ppid].push_back(pid);
        if (UI_COMMS.count(comm)) {
            ui.emplace_back(pid, comm);
        } else if (comm == "reaper") {
            // argv: /…/reaper SteamLaunch AppId=<id> -- <game>
            auto cmdline = pq::read_file("/proc/" + p + "/cmdline");
            static const std::string launch("reaper\0SteamLaunch\0", 19);
            if (cmdline && cmdline->find(launch) != std::string::npos) roots.insert(pid);
        }
    }
    std::map<pid_t, Target> out;
    Target game{game_uclamp(profile_), IO_GAME, true};
    std::vector<pid_t> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        pid_t p = stack.back();
        stack.pop_back();
        auto it = children.find(p);
        if (it == children.end()) continue;
        for (pid_t c : it->second)
            if (out.emplace(c, game).second) stack.push_back(c);
    }
    for (const auto& [pid, comm] : ui)  // a game's own stays the game's
        out.emplace(pid, Target{ui_uclamp(profile_), comm == IO_LOW_COMM ? IO_DOWNLOAD : IO_UI, false});
    return out;
}

void GameBoost::look() {
    targets_ = scan(roots_);
    std::set<pid_t> live;
    for (const auto& [pid, t] : targets_)
        for (pid_t tid : tasks(pid)) {
            live.insert(tid);
            boost(tid, t);
        }
    for (auto it = done_.begin(); it != done_.end();)
        it = live.count(it->first) ? std::next(it) : done_.erase(it);
}

// A thread gets its process's treatment once (again when the floor changed:
// a profile switch, or a UI process that became part of a game).
void GameBoost::boost(pid_t tid, const Target& t) {
    auto it = done_.find(tid);
    if (it != done_.end() && it->second == t.uclamp) return;
    set_ioprio(tid, t.io);
    if (avoid_little_ && !big_.empty() && t.game) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c : big_)
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
        sched_setaffinity(tid, sizeof set, &set);  // a thread already gone: nothing to do
    }
    set_uclamp_min(tid, t.uclamp);
    done_[tid] = t.uclamp;
}

bool GameBoost::matters(pid_t pid) const { return targets_.count(pid) || roots_.count(pid); }

void GameBoost::drain() {
    alignas(nlmsghdr) char buf[16384];
    while (true) {
        ssize_t n = recv(cn_, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == ENOBUFS) {
                rescan();  // events were dropped
                continue;
            }
            break;
        }
        if (n == 0) break;
        for (auto* nl = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(nl, static_cast<unsigned>(n));
             nl = NLMSG_NEXT(nl, n)) {
            if (nl->nlmsg_type == NLMSG_ERROR || nl->nlmsg_type == NLMSG_NOOP) continue;
            auto* cn = static_cast<cn_msg*>(NLMSG_DATA(nl));
            if (cn->id.idx != CN_IDX_PROC || cn->id.val != CN_VAL_PROC) continue;
            event(cn->data);
        }
    }
}

void GameBoost::event(const void* data) {
    const auto& ev = *static_cast<const proc_event*>(data);
    switch (ev.what) {
    case PROC_EVENT_FORK: {
        pid_t ptgid = ev.event_data.fork.parent_tgid;
        pid_t cpid = ev.event_data.fork.child_pid, ctgid = ev.event_data.fork.child_tgid;
        if (cpid != ctgid) {
            // A new thread: its process's treatment, now.
            auto it = targets_.find(ctgid);
            if (it != targets_.end()) boost(cpid, it->second);
        } else if (matters(ptgid)) {
            // A child of a game, a launch reaper or a UI process (same comm).
            rescan();
        }
        break;
    }
    case PROC_EVENT_EXEC: {
        pid_t pid = ev.event_data.exec.process_tgid;
        if (ev.event_data.exec.process_pid != pid) break;
        if (matters(pid)) {
            rescan();
            break;
        }
        std::string comm = pq::rd("/proc/" + std::to_string(pid) + "/comm");
        if (UI_COMMS.count(comm) || comm == "reaper") rescan();
        break;
    }
    case PROC_EVENT_COMM: {
        pid_t pid = ev.event_data.comm.process_tgid;
        if (ev.event_data.comm.process_pid != pid) break;
        std::string comm(ev.event_data.comm.comm, strnlen(ev.event_data.comm.comm, sizeof ev.event_data.comm.comm));
        if (matters(pid) || UI_COMMS.count(comm) || comm == "reaper") rescan();
        break;
    }
    case PROC_EVENT_EXIT: {
        pid_t pid = ev.event_data.exit.process_pid, tgid = ev.event_data.exit.process_tgid;
        done_.erase(pid);
        // A launch reaper ending can leave its game's daemons elsewhere.
        if (pid == tgid && matters(pid)) rescan();
        break;
    }
    default:
        break;
    }
}

}  // namespace pp::pbosd
