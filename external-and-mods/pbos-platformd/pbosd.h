// pbosd's parts shared between its source files (pbosd.cpp: the daemon;
// pbosd_games.cpp: the game and UI thread boost).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "loop.h"

namespace pp::pbosd {

// Python's int() of a string: surrounding whitespace, a sign, digits with
// single underscores between them (base 10).
std::optional<long long> py_int(std::string_view text);

// CPUs not in the lowest-capacity cluster (drops the Cortex-A520s).
std::set<int> big_cpus();

// Threads of Steam-launched games (descendants of `reaper SteamLaunch`) are
// kept off the Cortex-A520 little cores and get a uclamp.min boost so
// schedutil ramps the big cores immediately; the Steam UI path gets a
// smaller floor; both get a disk priority. What Android's game mode / WALT
// does for GameNative.
//
// The Python version looked through /proc once a second. This follows the
// kernel's process events (the proc connector, as sm8550-boostd does): a new
// thread of a boosted process is boosted as it is made, and /proc is looked
// through again only when a process that matters starts, changes or ends.
// Without the connector it looks once a second, as before.
class GameBoost {
public:
    GameBoost(pq::Loop& loop, bool enabled, bool avoid_little);
    ~GameBoost();
    void start(const std::string& profile);
    // A profile switch: every thread gets the new profile's floor (the
    // Python version cleared what it had done; its next look redid it all).
    void set_profile(const std::string& profile);

private:
    struct Target {
        unsigned uclamp;
        int io;
        bool game;
    };
    void tick();
    void rescan();
    void look();
    void boost(pid_t tid, const Target& t);
    bool matters(pid_t pid) const;
    void drain();
    void event(const void* ev);
    std::map<pid_t, Target> scan(std::set<pid_t>& roots) const;

    pq::Loop& loop_;
    bool enabled_, avoid_little_;
    std::set<int> big_;
    std::string profile_ = "balanced";
    int cn_ = -1;
    uint64_t pending_ = 0;
    std::map<pid_t, Target> targets_;
    std::set<pid_t> roots_;
    std::map<pid_t, unsigned> done_;  // tid -> the uclamp value applied
};

}  // namespace pp::pbosd
