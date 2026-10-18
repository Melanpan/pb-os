// pbos-platformd: PB-OS's per-device platform daemons in one process, one
// module each, picked by the device tree. Two instances: --system (root)
// and --user (the session's: PipeWire, the session bus).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "loop.h"

namespace pp {

class Module {
public:
    virtual ~Module() = default;
    virtual const char* name() const = 0;
    // On the loop thread, once.
    virtual void start(pq::Loop& loop) = 0;
    // On the loop thread, at shutdown (put the hardware's safe state back).
    virtual void stop() {}
    // SIGHUP: read the settings again.
    virtual void reload() {}
};

enum class Instance { System, User };

struct ModuleInfo {
    const char* name;
    Instance instance;
    // Whether this device has it (the device tree's compatible and model).
    std::function<bool()> here;
    std::function<std::unique_ptr<Module>()> make;
};

// Every module, in start order.
const std::vector<ModuleInfo>& modules();

// The device tree's compatible strings contain s.
bool dt_has(const char* s);

// Each module's factory (one source file per module).
std::unique_ptr<Module> make_volume_keeper();
bool charge_control_here();
std::unique_ptr<Module> make_charge_control();
std::unique_ptr<Module> make_fan();
std::unique_ptr<Module> make_power_button();
std::unique_ptr<Module> make_volume_keys();

}  // namespace pp
