#include "loop.h"

#include <linux/netlink.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include "util.h"

namespace pq {

namespace {

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

std::string unit_object_path(const std::string& unit) {
    // sd_bus_path_encode's escaping: [A-Za-z0-9] stay, the rest is _xx.
    std::string out = "/org/freedesktop/systemd1/unit/";
    for (unsigned char c : unit) {
        if (std::isalnum(c)) {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "_%02x", c);
            out += buf;
        }
    }
    return out;
}

Loop::Loop() {
    ep_ = epoll_create1(EPOLL_CLOEXEC);
    wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    add_fd(wake_, [this] {
        uint64_t n;
        while (::read(wake_, &n, sizeof n) > 0) {}
        std::vector<Fn> todo;
        {
            std::lock_guard<std::mutex> g(posted_lock_);
            todo.swap(posted_);
        }
        for (auto& fn : todo) fn();
    });
    timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    add_fd(timer_, [this] {
        uint64_t n;
        while (::read(timer_, &n, sizeof n) > 0) {}
        double now = now_s();
        while (!timers_.empty() && timers_.begin()->first <= now + 0.0005) {
            auto fn = std::move(timers_.begin()->second.second);
            timers_.erase(timers_.begin());
            fn();
        }
        arm_timer();
    });
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    sig_ = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
    add_fd(sig_, [this] {
        signalfd_siginfo si;
        while (::read(sig_, &si, sizeof si) == sizeof si) {
            if (si.ssi_signo == SIGPIPE) continue;
            if (signal_cb_) signal_cb_(static_cast<int>(si.ssi_signo));
        }
    });
}

Loop::~Loop() {
    for (auto* s : slots_) sd_bus_slot_unref(s);
    if (bus_) sd_bus_flush_close_unref(bus_);
    for (int fd : {wake_, timer_, inotify_, uevent_, sig_, ep_})
        if (fd >= 0) close(fd);
}

void Loop::add_fd(int fd, std::function<void()> cb) {
    fds_[fd] = std::move(cb);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
}

void Loop::del_fd(int fd) {
    epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    fds_.erase(fd);
}

void Loop::run() {
    while (running_) {
        epoll_event evs[16];
        int timeout = -1;
        if (bus_) {
            uint64_t usec = 0;
            if (sd_bus_get_timeout(bus_, &usec) >= 0 && usec != UINT64_MAX) {
                timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
                timeout = usec <= now ? 0 : static_cast<int>((usec - now + 999) / 1000);
            }
        }
        int n = epoll_wait(ep_, evs, 16, timeout);
        if (n < 0 && errno != EINTR) break;
        for (int i = 0; i < n; ++i) {
            auto it = fds_.find(evs[i].data.fd);
            if (it != fds_.end()) {
                auto cb = it->second;  // the callback may remove itself
                cb();
            }
        }
        if (bus_ && n == 0) bus_process();
    }
}

void Loop::quit() {
    running_ = false;
    post([] {});
}

void Loop::post(Fn fn) {
    {
        std::lock_guard<std::mutex> g(posted_lock_);
        posted_.push_back(std::move(fn));
    }
    uint64_t one = 1;
    ssize_t r = ::write(wake_, &one, sizeof one);
    (void)r;
}

void Loop::work(Fn fn) {
    // Small stacks: the work is a helper call or a file read, and the
    // daemon's footprint is the point of it.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto* heap = new Fn(std::move(fn));
    pthread_t t;
    int rc = pthread_create(
        &t, &attr,
        [](void* p) -> void* {
            auto* f = static_cast<Fn*>(p);
            (*f)();
            delete f;
            return nullptr;
        },
        heap);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        // No thread to be had: run it here rather than drop it.
        (*heap)();
        delete heap;
    }
}

uint64_t Loop::after(double seconds, Fn fn) {
    uint64_t id = next_id_++;
    timers_.emplace(now_s() + seconds, std::make_pair(id, std::move(fn)));
    arm_timer();
    return id;
}

void Loop::cancel(uint64_t id) {
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.first == id) {
            timers_.erase(it);
            break;
        }
    }
    arm_timer();
}

void Loop::arm_timer() {
    itimerspec its{};
    if (!timers_.empty()) {
        double at = timers_.begin()->first;
        if (at <= now_s()) at = now_s() + 0.000001;
        its.it_value.tv_sec = static_cast<time_t>(at);
        its.it_value.tv_nsec = static_cast<long>((at - static_cast<double>(its.it_value.tv_sec)) * 1e9);
        timerfd_settime(timer_, TFD_TIMER_ABSTIME, &its, nullptr);
    } else {
        timerfd_settime(timer_, 0, &its, nullptr);
    }
}

int Loop::watch(const std::string& path, uint32_t mask, WatchFn cb) {
    if (inotify_ < 0) {
        inotify_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (inotify_ < 0) return 0;
        add_fd(inotify_, [this] { drain_inotify(); });
    }
    int wd = inotify_add_watch(inotify_, path.c_str(), mask | IN_MASK_ADD);
    if (wd < 0) return 0;
    int id = next_watch_++;
    watches_[wd].push_back({id, mask, std::move(cb)});
    watch_ids_[id] = wd;
    return id;
}

void Loop::unwatch(int id) {
    auto it = watch_ids_.find(id);
    if (it == watch_ids_.end()) return;
    int wd = it->second;
    watch_ids_.erase(it);
    auto& list = watches_[wd];
    for (auto w = list.begin(); w != list.end(); ++w) {
        if (w->id == id) {
            list.erase(w);
            break;
        }
    }
    // The kernel's mask stays the union while others watch the path; their
    // own masks filter what they hear.
    if (list.empty()) {
        watches_.erase(wd);
        inotify_rm_watch(inotify_, wd);
    }
}

void Loop::drain_inotify() {
    alignas(inotify_event) char buf[8192];
    while (true) {
        ssize_t n = ::read(inotify_, buf, sizeof buf);
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            std::string name = ev->len ? std::string(ev->name) : std::string();
            if (ev->mask & IN_Q_OVERFLOW) {
                // Events were lost: every watcher that asked hears it.
                auto all = watches_;  // a callback may change the watches
                for (auto& [wd, list] : all)
                    for (auto& w : list)
                        if (w.mask & IN_Q_OVERFLOW) w.cb(IN_Q_OVERFLOW, "");
                continue;
            }
            auto it = watches_.find(ev->wd);
            if (it == watches_.end()) continue;
            if (ev->mask & IN_IGNORED) {
                // The path went away: its watchers hear of it once (IN_IGNORED).
                auto list = it->second;
                watches_.erase(it);
                for (auto& w : list) {
                    watch_ids_.erase(w.id);
                    w.cb(IN_IGNORED, name);
                }
                continue;
            }
            auto list = it->second;  // a callback may change the watches
            for (auto& w : list)
                if (ev->mask & w.mask) w.cb(ev->mask, name);
        }
    }
}

void Loop::on_uevent(std::function<void(const std::string&, const std::string&)> cb) {
    if (uevent_ < 0) {
        uevent_ = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
        if (uevent_ < 0) {
            log("uevents: %s", std::strerror(errno));
            return;
        }
        sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        addr.nl_groups = 1;  // the kernel's own events
        int rcvbuf = 1 << 20;  // room for a resume's burst (needs root; else the default stays)
        setsockopt(uevent_, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof rcvbuf);
        if (bind(uevent_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            log("uevents: %s", std::strerror(errno));
            close(uevent_);
            uevent_ = -1;
            return;
        }
        add_fd(uevent_, [this] { drain_uevents(); });
    }
    uevent_cbs_.push_back(std::move(cb));
}

void Loop::drain_uevents() {
    char buf[8192];
    while (true) {
        ssize_t n = recv(uevent_, buf, sizeof buf - 1, 0);
        if (n < 0 && errno == ENOBUFS) {
            // Some were dropped (a burst, as at resume): an empty subsystem.
            for (auto& cb : uevent_cbs_) cb("", "");
            continue;
        }
        if (n <= 0) break;
        buf[n] = 0;
        std::string subsystem, action;
        for (char* p = buf; p < buf + n; p += std::strlen(p) + 1) {
            if (!std::strncmp(p, "SUBSYSTEM=", 10)) subsystem = p + 10;
            else if (!std::strncmp(p, "ACTION=", 7)) action = p + 7;
        }
        for (auto& cb : uevent_cbs_) cb(subsystem, action);
    }
}

void Loop::bus_open() {
    if (bus_) return;
    if (sd_bus_open_system(&bus_) < 0) {
        bus_ = nullptr;
        log("D-Bus: cannot open the system bus");
        return;
    }
    // Units' PropertiesChanged signals only go to subscribers.
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_call_method(bus_, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                           "org.freedesktop.systemd1.Manager", "Subscribe", &err, nullptr, "") < 0) {
        log("D-Bus: systemd Subscribe failed: %s", err.message ? err.message : "?");
    }
    sd_bus_error_free(&err);
    bus_fd_ = sd_bus_get_fd(bus_);
    add_fd(bus_fd_, [this] { bus_process(); });
}

void Loop::bus_process() {
    while (bus_ && sd_bus_process(bus_, nullptr) > 0) {}
}

struct BusSignal {
    static int handle(sd_bus_message* m, void* userdata, sd_bus_error*) {
        auto* self = static_cast<Loop*>(userdata);
        const char* path = sd_bus_message_get_path(m);
        if (!path) return 0;
        auto it = self->unit_cbs_.find(path);
        if (it == self->unit_cbs_.end()) return 0;
        auto cbs = it->second;
        for (auto& cb : cbs) cb();
        return 0;
    }
};

void Loop::on_unit_change(const std::string& unit, Fn cb) {
    bus_open();
    if (!bus_) return;
    std::string path = unit_object_path(unit);
    bool first = unit_cbs_.find(path) == unit_cbs_.end();
    unit_cbs_[path].push_back(std::move(cb));
    if (!first) return;
    sd_bus_slot* slot = nullptr;
    if (sd_bus_match_signal(bus_, &slot, "org.freedesktop.systemd1", path.c_str(), "org.freedesktop.DBus.Properties",
                            "PropertiesChanged", &BusSignal::handle, this) >= 0) {
        slots_.push_back(slot);
    }
    bus_process();
}

bool Loop::on_exit(pid_t pid, Fn cb) {
    int fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (fd < 0) return false;
    add_fd(fd, [this, fd, cb = std::move(cb)] {
        del_fd(fd);
        close(fd);
        cb();
    });
    return true;
}

}  // namespace pq
