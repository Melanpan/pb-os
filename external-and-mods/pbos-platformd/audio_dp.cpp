// audio-dp (was sm8550-audio-pipewire --watch's loop): run the SM8550 sink
// policy again when an HDMI/DisplayPort output is plugged in or pulled.
//
// The policy itself stays in /usr/lib/steamos/sm8550-audio-pipewire (pactl
// and amixer, a few seconds, only when something changed). That script
// applies it once and then execs this module, with the state it applied in
// PBOS_AUDIO_DP_LAST (on/off). The bash loop it replaces read every DRM
// connector's status every 2 s, forever; this reads them when the kernel
// sends a DRM uevent (hotplug), 0.5 s after the last one of a burst so a
// connector that bounces settles first, and when uevents were dropped.

#include <cstdlib>
#include <string>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

constexpr const char* POLICY = "/usr/lib/steamos/sm8550-audio-pipewire";
constexpr double SETTLE = 0.5;
constexpr double POLICY_TIMEOUT = 120;

// The script's drm_dp: any DP or HDMI connector reports "connected".
bool drm_dp() {
    for (const char* pat : {"/sys/class/drm/card*-DP-*/status", "/sys/class/drm/card*-HDMI-A-*/status"})
        for (const auto& f : pq::glob(pat)) {
            auto s = pq::read_file(f);
            if (s && pq::trim(*s) == "connected") return true;
        }
    return false;
}

class AudioDp : public Module {
public:
    const char* name() const override { return "audio-dp"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        const char* last = std::getenv("PBOS_AUDIO_DP_LAST");
        bool now = drm_dp();
        last_ = last ? std::string(last) == "on" : now;
        pq::log("audio-dp: watching, last_dp=%s", last_ ? "on" : "off");
        loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) {
            if (!env.empty()) {
                auto it = env.find("SUBSYSTEM");
                if (it == env.end() || it->second != "drm") return;
            }
            settle();
        });
        // A flip between the script's look and this start.
        if (now != last_) check();
    }

private:
    void settle() {
        if (timer_) loop_->cancel(timer_);
        timer_ = loop_->after(SETTLE, [this] {
            timer_ = 0;
            check();
        });
    }

    void check() {
        if (running_) {
            again_ = true;
            return;
        }
        bool cur = drm_dp();
        if (cur == last_) return;
        pq::log("audio-dp: DP flip %s -> %s", last_ ? "on" : "off", cur ? "on" : "off");
        last_ = cur;
        running_ = true;
        pq::Loop::work([this] {
            pq::RunOpts o;
            o.timeout = POLICY_TIMEOUT;
            auto r = pq::run({POLICY}, o);
            loop_->post([this, r] {
                for (const auto& line : pq::split_lines(r.err))
                    if (!line.empty()) pq::log("%s", line.c_str());
                if (!r.started) pq::log("audio-dp: %s did not start", POLICY);
                else if (r.timed_out) pq::log("audio-dp: %s timed out", POLICY);
                running_ = false;
                // Something changed while it ran: look again.
                if (again_) {
                    again_ = false;
                    check();
                }
            });
        });
    }

    pq::Loop* loop_ = nullptr;
    bool last_ = false;
    bool running_ = false;
    bool again_ = false;
    uint64_t timer_ = 0;
};

}  // namespace

std::unique_ptr<Module> make_audio_dp() { return std::make_unique<AudioDp>(); }

}  // namespace pp
