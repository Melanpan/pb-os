// konkr-volume (was /usr/lib/konkr/konkr-volume): usable, clean volume for
// the KONKR Pocket FIT and AYANEO Pocket S2 speakers.
//
// Steam's volume bar (like every PulseAudio client) is cubic: 20 % on the bar
// is 0.2^3 of full amplitude, -42 dB, and the lower third of the bar was
// silent on these small speakers.
//
// This keeps Steam's value (channelVolumes) untouched (the bar never jumps)
// and applies a gentler curve in HARDWARE, on the WSA macro's digital volume
// (WSA_RX0/RX1 Digital Volume: 1 dB steps, -84..-3 dB, processed at the
// codec's internal precision). The speaker PCM only takes S16_LE, so
// attenuating in software instead (softVolumes) threw away bits and added an
// audible crackle/grit that got worse the lower it went. softVolumes stay at
// 1.0 (or 0 when muted).
//
//     applied dB = -34 * (1 - v) ** 1.35       (relative to the stage maximum)
//
//     bar   1 %   5 %  10 %  20 %  30 %  50 %  75 %  100 %
//     dB  -33.5 -31.7 -29.5 -25.2 -21.0 -13.3 -5.2    0
//
// The amplifier (PA) is held at 17 = the chip's G_0_DB step (the control's
// TLV label says +16.5 dB, but the register is inverted over 0..31 while the
// gain table runs 0 = +21 dB .. 18 = -6 dB; the old "12" was about -7.5 dB and
// users found the speakers far too quiet). 17 is the cap the sc8280xp machine
// driver enforces. Something (UCM/state restore on a PipeWire restart) can put
// it back, so it is put right again whenever it changes.
//
// The Python version followed `pactl subscribe`, ran pactl, amixer, pw-cli
// and pw-dump on each event, and re-checked everything every 3 s (about six
// programs every 3 s, forever). Here each part tells us when it changes:
//   - the bar (volume, mute): the sound server's events (libpulse);
//   - the WSA gains and the PA: the card's control events (libasound), so a
//     reset is put right at once (the Python only saw a PA reset, up to 3 s
//     late, and never a reset of the digital stage alone);
//   - softVolumes (ACP puts them back to the cubic value on node resume and
//     route changes): the speaker node's Props events (libpipewire).
// Writes happen only where a value differs from what it should be.
//
// One thread owns all of the state: the loop's. PipeWire's loop and the
// card's events run on it; the PulseAudio thread only posts what it read.
// The Python crashed every few minutes this way: its 3 s thread cleared the
// node id while apply() was using it on the main thread, apply() passed None
// to pw-cli, the daemon died, and every restart re-forced the amp gains.
// Here nothing is shared between threads, a missing node or card only means
// waiting for it to come back, and a fight with something that keeps undoing
// our values is slowed down (a burst, then 2 writes a second) rather than
// looping.

#include <alsa/asoundlib.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <sys/epoll.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

constexpr const char* SINK = "alsa_output.platform-sound.HiFi__Speaker__sink";
constexpr const char* CARD = "hw:0";
constexpr const char* DIGITAL[] = {"WSA_RX0 Digital Volume", "WSA_RX1 Digital Volume"};
constexpr const char* PA = "Speakers Volume";
constexpr long PA_LEVEL = 17;
constexpr long DIGITAL_MAX_DEFAULT = 81;  // when the card cannot tell
constexpr pa_usec_t PULSE_RECONNECT_US = 2 * 1000 * 1000;
constexpr double PW_RECONNECT_S = 2.0;

double mono_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Hardware register value for bar position v (1 dB per step, 0 = mute).
// Python's round() rounds halves to even, as nearbyint() does.
long target_steps(double v, long vmax) {
    if (v <= 0.0) return 0;
    v = std::min(v, 1.0);
    double db = -34.0 * std::pow(1.0 - v, 1.35);
    return std::max(1L, std::min(vmax, vmax + static_cast<long>(std::nearbyint(db))));
}

// A burst of writes goes through at once (a slider being dragged), then at
// most 2 a second: something undoing every write cannot make this spin.
class Gate {
public:
    // 0 when a write may go now, else the seconds until one may.
    double wait() {
        refill();
        return tokens_ >= 1 ? 0 : (1 - tokens_) / RATE;
    }
    void take() {
        refill();
        tokens_ -= 1;
    }

private:
    static constexpr double BURST = 40, RATE = 2;
    void refill() {
        double now = mono_now();
        tokens_ = std::min(BURST, tokens_ + (now - last_) * RATE);
        last_ = now;
    }
    double tokens_ = BURST;
    double last_ = mono_now();
};

class KonkrVolume : public Module {
public:
    const char* name() const override { return "konkr-volume"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        ep_ = epoll_create1(EPOLL_CLOEXEC);
        loop.on_readable(ep_, [this] { card_events(); });
        // The card comes back (or shows up late): its control device's uevent.
        loop.on_uevent([this](const std::string& subsystem, const std::string& action) {
            if (!ctl_ && (subsystem.empty() || (subsystem == "sound" && action == "add"))) open_card();
        });
        open_card();

        pw_init(nullptr, nullptr);
        pw_loop_ = pw_loop_new(nullptr);
        pw_loop_enter(pw_loop_);
        pw_context_ = pw_context_new(pw_loop_, nullptr, 0);
        loop.on_readable(pw_loop_get_fd(pw_loop_), [this] { pw_loop_iterate(pw_loop_, 0); });
        pw_connect();

        ml_ = pa_threaded_mainloop_new();
        api_ = pa_threaded_mainloop_get_api(ml_);
        pa_threaded_mainloop_lock(ml_);
        pa_connect();
        pa_threaded_mainloop_unlock(ml_);
        pa_threaded_mainloop_start(ml_);
    }

    void stop() override {
        if (ml_) {
            pa_threaded_mainloop_stop(ml_);
            if (ctx_) {
                // Disconnecting calls the state callback (TERMINATED) right
                // away, which would unref the context and connect again.
                pa_context_set_state_callback(ctx_, nullptr, nullptr);
                pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
                pa_context_disconnect(ctx_);
                pa_context_unref(ctx_);
            }
            pa_threaded_mainloop_free(ml_);
            ml_ = nullptr;
        }
        pw_disconnect();
        if (pw_context_) pw_context_destroy(pw_context_);
        if (pw_loop_) {
            pw_loop_leave(pw_loop_);
            pw_loop_destroy(pw_loop_);
        }
        pw_context_ = nullptr;
        pw_loop_ = nullptr;
        close_card();
    }

private:
    // ---- What the bar asks for (the loop thread) ----

    struct Bar {
        bool present = false;  // the speaker sink exists and was read
        int percent = 0;       // as pactl prints the first channel
        bool muted = false;
    };

    void bar(const Bar& b) {
        bar_ = b;
        if (!b.present) return;  // no sink (or no sound server): nothing to apply
        steps_ = b.muted ? 0 : target_steps(b.percent / 100.0, vmax_);
        soft_ = (b.muted || b.percent <= 0) ? 0.0f : 1.0f;
        fix_card();
        fix_soft();
    }

    // ---- The card: WSA digital volume and the PA (libasound) ----

    struct Control {
        const char* name;
        snd_ctl_elem_id_t* id = nullptr;
        unsigned count = 0;
        long max = 0;
        bool ok = false;
    };

    void open_card() {
        if (ctl_) return;
        if (snd_ctl_open(&ctl_, CARD, SND_CTL_NONBLOCK) < 0) {
            ctl_ = nullptr;
            if (!card_missing_logged_) pq::log("konkr-volume: no sound card %s yet", CARD);
            card_missing_logged_ = true;
            return;
        }
        card_missing_logged_ = false;
        snd_ctl_subscribe_events(ctl_, 1);
        int n = snd_ctl_poll_descriptors_count(ctl_);
        card_fds_.resize(std::max(n, 0));
        snd_ctl_poll_descriptors(ctl_, card_fds_.data(), card_fds_.size());
        for (auto& p : card_fds_) {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = p.fd;
            epoll_ctl(ep_, EPOLL_CTL_ADD, p.fd, &ev);
        }
        find_controls();
        fix_card();
    }

    void close_card() {
        for (auto& p : card_fds_) epoll_ctl(ep_, EPOLL_CTL_DEL, p.fd, nullptr);
        card_fds_.clear();
        for (auto* c : controls_) {
            if (c->id) snd_ctl_elem_id_free(c->id);
            c->id = nullptr;
            c->ok = false;
        }
        if (ctl_) snd_ctl_close(ctl_);
        ctl_ = nullptr;
    }

    void find_controls() {
        snd_ctl_elem_info_t* info;
        snd_ctl_elem_info_alloca(&info);
        for (auto* c : controls_) {
            if (!c->id) snd_ctl_elem_id_malloc(&c->id);
            snd_ctl_elem_id_clear(c->id);
            snd_ctl_elem_id_set_interface(c->id, SND_CTL_ELEM_IFACE_MIXER);
            snd_ctl_elem_id_set_name(c->id, c->name);
            snd_ctl_elem_info_set_id(info, c->id);
            c->ok = snd_ctl_elem_info(ctl_, info) >= 0 && snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_INTEGER;
            if (!c->ok) continue;
            snd_ctl_elem_info_get_id(info, c->id);
            c->count = snd_ctl_elem_info_get_count(info);
            c->max = snd_ctl_elem_info_get_max(info);
        }
        long vmax = digital_[0].ok ? digital_[0].max : DIGITAL_MAX_DEFAULT;
        if (vmax != vmax_) {
            vmax_ = vmax;
            if (bar_.present) bar(bar_);
        }
    }

    bool ours(const char* name) {
        for (auto* c : controls_)
            if (!std::strcmp(name, c->name)) return true;
        return false;
    }

    void card_events() {
        epoll_event evs[4];
        epoll_wait(ep_, evs, 4, 0);  // only to clear its readiness; the card is read below
        if (!ctl_) return;
        snd_ctl_event_t* ev;
        snd_ctl_event_alloca(&ev);
        bool changed = false, refind = false;
        for (;;) {
            int r = snd_ctl_read(ctl_, ev);
            if (r == 0 || r == -EAGAIN) break;
            if (r < 0) {  // the card went away
                pq::log("konkr-volume: sound card %s gone (%s)", CARD, snd_strerror(r));
                close_card();
                return;
            }
            if (snd_ctl_event_get_type(ev) != SND_CTL_EVENT_ELEM) continue;
            if (!ours(snd_ctl_event_elem_get_name(ev))) continue;
            unsigned mask = snd_ctl_event_elem_get_mask(ev);
            if (mask == SND_CTL_EVENT_MASK_REMOVE || (mask & SND_CTL_EVENT_MASK_ADD)) refind = true;
            else if (mask & SND_CTL_EVENT_MASK_VALUE) changed = true;
        }
        if (refind) find_controls();
        if (changed || refind) fix_card();
    }

    // Writes value to every channel of c unless it is there already;
    // true when it wrote.
    bool put(Control& c, long value) {
        if (!c.ok) return false;
        snd_ctl_elem_value_t* v;
        snd_ctl_elem_value_alloca(&v);
        snd_ctl_elem_value_set_id(v, c.id);
        bool same = snd_ctl_elem_read(ctl_, v) >= 0;
        for (unsigned i = 0; same && i < c.count; ++i) same = snd_ctl_elem_value_get_integer(v, i) == value;
        if (same) return false;
        for (unsigned i = 0; i < c.count; ++i) snd_ctl_elem_value_set_integer(v, i, value);
        int r = snd_ctl_elem_write(ctl_, v);
        if (r < 0) pq::log("konkr-volume: %s: %s", c.name, snd_strerror(r));
        return true;
    }

    bool card_right() {
        auto right = [this](Control& c, long value) {
            if (!c.ok) return true;
            snd_ctl_elem_value_t* v;
            snd_ctl_elem_value_alloca(&v);
            snd_ctl_elem_value_set_id(v, c.id);
            if (snd_ctl_elem_read(ctl_, v) < 0) return false;
            for (unsigned i = 0; i < c.count; ++i)
                if (snd_ctl_elem_value_get_integer(v, i) != value) return false;
            return true;
        };
        return right(pa_, PA_LEVEL) && right(digital_[0], steps_) && right(digital_[1], steps_);
    }

    // The PA at 17 and the digital stage at the bar's step, as the Python's
    // amixer calls set them (PA first).
    void fix_card() {
        if (!ctl_ || !bar_.present || card_retry_ || card_right()) return;
        if (double w = card_gate_.wait(); w > 0) {
            if (!card_slowed_logged_) pq::log("konkr-volume: the speaker gains keep being reset; slowing down");
            card_slowed_logged_ = true;
            card_retry_ = loop_->after(w, [this] {
                card_retry_ = 0;
                fix_card();
            });
            return;
        }
        card_gate_.take();
        put(pa_, PA_LEVEL);
        put(digital_[0], steps_);
        put(digital_[1], steps_);
    }

    // ---- softVolumes on the speaker node (libpipewire, on the loop thread) ----

    void pw_connect() {
        pw_core_ = pw_context_connect(pw_context_, nullptr, 0);
        if (!pw_core_) {
            pw_retry();
            return;
        }
        static const pw_core_events core_events = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = [](void* p, uint32_t id, int seq) {
                auto* self = static_cast<KonkrVolume*>(p);
                if (id == PW_ID_CORE && seq == self->props_seq_) {
                    self->props_known_ = true;
                    self->fix_soft();
                }
            };
            e.error = [](void* p, uint32_t id, int, int res, const char*) {
                // PipeWire went away (restart): connect again a bit later,
                // outside of its own callback.
                if (id == PW_ID_CORE && res == -EPIPE) static_cast<KonkrVolume*>(p)->pw_lost();
            };
            return e;
        }();
        static const pw_registry_events registry_events = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = [](void* p, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
                auto* self = static_cast<KonkrVolume*>(p);
                if (self->node_ || std::strcmp(type, PW_TYPE_INTERFACE_Node) || !props) return;
                const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
                if (name && !std::strcmp(name, SINK)) self->bind_node(id);
            };
            e.global_remove = [](void* p, uint32_t id) {
                auto* self = static_cast<KonkrVolume*>(p);
                if (self->node_ && id == self->node_id_) self->unbind_node();
            };
            return e;
        }();
        pw_core_add_listener(pw_core_, &core_hook_, &core_events, this);
        pw_registry_ = pw_core_get_registry(pw_core_, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(pw_registry_, &registry_hook_, &registry_events, this);
    }

    void pw_lost() {
        if (pw_lost_) return;
        pw_lost_ = true;
        loop_->post([this] {
            pw_disconnect();
            pw_retry();
        });
    }

    void pw_retry() {
        loop_->after(PW_RECONNECT_S, [this] {
            pw_lost_ = false;
            pw_connect();
        });
    }

    void pw_disconnect() {
        unbind_node();
        if (pw_registry_) {
            spa_hook_remove(&registry_hook_);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(pw_registry_));
            pw_registry_ = nullptr;
        }
        if (pw_core_) {
            spa_hook_remove(&core_hook_);
            pw_core_disconnect(pw_core_);
            pw_core_ = nullptr;
        }
    }

    void bind_node(uint32_t id) {
        static const pw_node_events node_events = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.param = [](void* p, int, uint32_t pid, uint32_t, uint32_t, const spa_pod* param) {
                if (pid == SPA_PARAM_Props) static_cast<KonkrVolume*>(p)->props(param);
            };
            return e;
        }();
        node_ = static_cast<pw_node*>(pw_registry_bind(pw_registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!node_) return;
        node_id_ = id;
        soft_now_.reset();
        props_known_ = false;
        pw_node_add_listener(node_, &node_hook_, &node_events, this);
        // Its Props now, then each time they change.
        uint32_t ids[] = {SPA_PARAM_Props};
        pw_node_subscribe_params(node_, ids, 1);
        props_seq_ = pw_core_sync(pw_core_, PW_ID_CORE, 0);
    }

    void unbind_node() {
        if (!node_) return;
        spa_hook_remove(&node_hook_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node_));
        node_ = nullptr;
        soft_now_.reset();
        props_known_ = false;
    }

    // A Props param: the first channel of its softVolumes, as pw-dump read it.
    void props(const spa_pod* param) {
        if (!param || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props)) return;
        const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes);
        if (!prop) return;
        float v[64];  // SPA_AUDIO_MAX_CHANNELS
        if (spa_pod_copy_array(&prop->value, SPA_TYPE_Float, v, 64) == 0) return;
        soft_now_ = v[0];
        fix_soft();
    }

    void fix_soft() {
        if (!node_ || !bar_.present || soft_retry_) return;
        if (soft_now_ ? std::fabs(*soft_now_ - soft_) <= 1e-3 : !props_known_) return;
        if (double w = soft_gate_.wait(); w > 0) {
            if (!soft_slowed_logged_) pq::log("konkr-volume: softVolumes keep being reset; slowing down");
            soft_slowed_logged_ = true;
            soft_retry_ = loop_->after(w, [this] {
                soft_retry_ = 0;
                fix_soft();
            });
            return;
        }
        soft_gate_.take();
        // pw-cli set-param <node> Props '{ softVolumes: [ s, s ] }'
        uint8_t buf[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        float vals[2] = {soft_, soft_};
        auto* pod = static_cast<spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_softVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, vals)));
        pw_node_set_param(node_, SPA_PARAM_Props, 0, pod);
        soft_now_ = soft_;  // its Props event brings the real value back
    }

    // ---- The bar (libpulse; its own thread, which only posts) ----

    void pa_connect() {
        ctx_ = pa_context_new(api_, "pbos-platformd konkr-volume");
        pa_context_set_state_callback(ctx_, &KonkrVolume::on_state, this);
        pa_context_set_subscribe_callback(ctx_, &KonkrVolume::on_event, this);
        // A refused connection may already have failed through on_state
        // (which retries and clears ctx_): only one retry either way.
        if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0 && ctx_) pa_retry();
    }

    void pa_retry() {
        if (ctx_) {
            pa_context_set_state_callback(ctx_, nullptr, nullptr);
            pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
            pa_context_disconnect(ctx_);
            pa_context_unref(ctx_);
            ctx_ = nullptr;
        }
        loop_->post([this] { bar(Bar{}); });  // nothing is applied without the server
        pa_gettimeofday(&tv_);
        pa_timeval_add(&tv_, PULSE_RECONNECT_US);
        api_->time_new(api_, &tv_,
                       [](pa_mainloop_api* a, pa_time_event* e, const timeval*, void* self) {
                           a->time_free(e);
                           static_cast<KonkrVolume*>(self)->pa_connect();
                       },
                       this);
    }

    static void on_state(pa_context* c, void* p) {
        auto* self = static_cast<KonkrVolume*>(p);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa_operation_unref(pa_context_subscribe(
                c,
                static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER |
                                                    PA_SUBSCRIPTION_MASK_CARD),
                nullptr, nullptr));
            self->ask();
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            self->pa_retry();  // PipeWire restarted, or not up yet
            break;
        default:
            break;
        }
    }

    // "on sink", "on server" or "on card" in pactl subscribe's words.
    static void on_event(pa_context*, pa_subscription_event_type_t, uint32_t, void* p) {
        static_cast<KonkrVolume*>(p)->ask();
    }

    void ask() {
        pa_operation_unref(pa_context_get_sink_info_by_name(
            ctx_, SINK,
            [](pa_context*, const pa_sink_info* i, int eol, void* p) {
                auto* self = static_cast<KonkrVolume*>(p);
                Bar b;
                if (eol < 0) {
                    // No such sink (yet): Python's bar_position() was None.
                } else if (eol || !i) {
                    return;
                } else {
                    b.present = true;
                    // pactl's "/ 42%" of the first channel.
                    b.percent = static_cast<int>(
                        (static_cast<uint64_t>(i->volume.values[0]) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
                    b.muted = i->mute;
                }
                self->loop_->post([self, b] { self->bar(b); });
            },
            this));
    }

    pq::Loop* loop_ = nullptr;

    // The bar and what it asks for.
    Bar bar_;
    long vmax_ = DIGITAL_MAX_DEFAULT;
    long steps_ = 0;
    float soft_ = 1.0f;

    // The card.
    snd_ctl_t* ctl_ = nullptr;
    int ep_ = -1;
    std::vector<pollfd> card_fds_;
    Control pa_{PA}, digital_[2] = {{DIGITAL[0]}, {DIGITAL[1]}};
    Control* const controls_[3] = {&pa_, &digital_[0], &digital_[1]};
    Gate card_gate_;
    uint64_t card_retry_ = 0;
    bool card_slowed_logged_ = false, card_missing_logged_ = false;

    // PipeWire.
    pw_loop* pw_loop_ = nullptr;
    pw_context* pw_context_ = nullptr;
    pw_core* pw_core_ = nullptr;
    pw_registry* pw_registry_ = nullptr;
    pw_node* node_ = nullptr;
    uint32_t node_id_ = SPA_ID_INVALID;
    spa_hook core_hook_{}, registry_hook_{}, node_hook_{};
    int props_seq_ = -1;
    bool props_known_ = false;  // the node's Props have been read once
    std::optional<float> soft_now_;
    Gate soft_gate_;
    uint64_t soft_retry_ = 0;
    bool soft_slowed_logged_ = false, pw_lost_ = false;

    // PulseAudio.
    pa_threaded_mainloop* ml_ = nullptr;
    pa_mainloop_api* api_ = nullptr;
    pa_context* ctx_ = nullptr;
    timeval tv_{};
};

}  // namespace

bool konkr_volume_here() {
    std::string model = pq::dt_model();
    return model.find("KONKR Pocket FIT") != std::string::npos || model.find("AYANEO Pocket S2") != std::string::npos;
}

std::unique_ptr<Module> make_konkr_volume() { return std::make_unique<KonkrVolume>(); }

}  // namespace pp
