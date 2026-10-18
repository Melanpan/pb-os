// pbos-qamd's event loop (one thread, epoll): posted work, timers, inotify,
// kernel uevents, systemd unit changes over D-Bus, process exits and
// signals. Sections react to these; nothing here wakes up on a clock unless
// a section asked for a timer.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sd_bus;
struct sd_bus_slot;

namespace pq {

class Loop {
public:
    using Fn = std::function<void()>;

    Loop();
    ~Loop();
    void run();
    void quit();

    // From any thread: fn runs on the loop thread.
    void post(Fn fn);
    // Runs fn on a thread of its own (the sections' blocking work).
    static void work(Fn fn);

    // Loop thread only, from here on.
    uint64_t after(double seconds, Fn fn);
    void cancel(uint64_t id);

    // inotify: cb(mask, name) for each event on path (a folder's entries
    // carry their name). 0 when the path cannot be watched. A watcher whose
    // mask has IN_Q_OVERFLOW hears when the queue overflowed (name empty).
    using WatchFn = std::function<void(uint32_t mask, const std::string& name)>;
    int watch(const std::string& path, uint32_t mask, WatchFn cb);
    void unwatch(int id);

    // Kernel uevents (block devices coming and going): cb(subsystem, action);
    // an empty subsystem when some were dropped (ENOBUFS).
    void on_uevent(std::function<void(const std::string&, const std::string&)> cb);
    // A systemd unit changed state (started, stopped, failed).
    void on_unit_change(const std::string& unit, Fn cb);
    // The process exited (pidfd); false when it is already gone.
    bool on_exit(pid_t pid, Fn cb);
    // SIGTERM, SIGINT and SIGHUP.
    void on_signal(std::function<void(int)> cb) { signal_cb_ = std::move(cb); }

private:
    void add_fd(int fd, std::function<void()> cb);
    void del_fd(int fd);
    void arm_timer();
    void drain_inotify();
    void drain_uevents();
    void bus_open();
    void bus_process();
    friend struct BusSignal;

    int ep_ = -1, wake_ = -1, timer_ = -1, inotify_ = -1, uevent_ = -1, sig_ = -1, bus_fd_ = -1;
    bool running_ = true;
    std::mutex posted_lock_;
    std::vector<Fn> posted_;
    std::map<int, std::function<void()>> fds_;
    uint64_t next_id_ = 1;
    std::multimap<double, std::pair<uint64_t, Fn>> timers_;
    struct Watch {
        int id;
        uint32_t mask;
        WatchFn cb;
    };
    std::map<int, std::vector<Watch>> watches_;  // wd -> its watchers
    std::map<int, int> watch_ids_;               // id -> wd
    int next_watch_ = 1;
    std::vector<std::function<void(const std::string&, const std::string&)>> uevent_cbs_;
    sd_bus* bus_ = nullptr;
    std::vector<sd_bus_slot*> slots_;
    std::map<std::string, std::vector<Fn>> unit_cbs_;  // D-Bus object path -> cbs
    std::function<void(int)> signal_cb_;
};

// systemd's object path for a unit name.
std::string unit_object_path(const std::string& unit);

}  // namespace pq
