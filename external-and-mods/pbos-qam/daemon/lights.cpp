// The stick light engine: the Retroid Pocket 6 / Nova and AYN Odin 2 /
// Odin 2 Portal stick lights (was pbos_lights.py on the system's Python).
//
// pbos-qamd --lights runs as the transient unit pbos-utils-lights, started
// by the tools section (tools.cpp keeps its own copy of the definitions and
// the saved settings' reading; keep the two in step). It shows what
// LIGHTS_STATE says, again on SIGHUP (the tools section sends one after each
// change), and fades the lights back in when their devices come back after a
// sleep.
//
// Nothing runs on a clock but an effect's frames: the LED devices coming
// back are heard as kernel uevents (subsystem leds), the charging indicator
// taking and giving back the lights with inotify on /run and its owner's
// pidfd. Python polled both every 0.5 s and drew Breathing at 4.6 % of a
// core on the Nova.

#include "lights.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <thread>
#include <vector>

#include "json.h"
#include "loop.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;
using RGB = std::array<double, 3>;

const std::string LIGHTS_STATE = "/var/lib/pbos-utils/lights.json";
// While this exists the stick lights are the charging indicator's
// (sm8550-charge-rest: resting on the charger, or the Power tab's preview):
// the engine writes nothing until it is gone.
const std::string INDICATOR_DIR = "/run";
const std::string INDICATOR_NAME = "pbos-charge-indicator";
const std::string INDICATOR = INDICATOR_DIR + "/" + INDICATOR_NAME;
const std::string LEDS = "/sys/class/leds";
// AYN Odin 2: per stick a ring and a strip on the grip's side, each one RGB
// LED (pwm-leds-multicolor on two SN3112s), named by their DT labels.
const char* const ODIN2_ZONES[] = {"left-joystick", "left-side", "right-joystick", "right-side"};

const char* const DEFAULT_EFFECT = "breathing";
const char* const DEFAULT_COLOR = "8c00ff";
const char* const DEFAULT_COLOR2 = "ffffff";  // the audio effects' intensity color

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Python's float %: the result takes the divisor's sign.
double pymod(double a, double b) { return a - b * std::floor(a / b); }

// The indicator holds its owner's PID; a leftover from a crash counts for
// nothing, and nor does an owner that has ended but not yet been reaped
// (its pidfd has fired, its /proc entry is still there). The PID, or 0.
pid_t indicator_owner() {
    std::string pid = rd(INDICATOR);
    if (!is_digits(pid)) return 0;
    std::string stat = rd("/proc/" + pid + "/stat");
    auto close = stat.rfind(')');
    if (close == std::string::npos || close + 2 >= stat.size() || stat[close + 2] == 'Z') return 0;
    return static_cast<pid_t>(std::atol(pid.c_str()));
}

// The stick LEDs found, by zone name (l1..r4, or the Odin 2's labels).
// Retroid Pocket 6 / Nova, AYN Odin 2 Portal: four RGB groups around each
// stick (leds-group-multicolor, functions l1..l4 and r1..r4).
std::map<std::string, std::string> led_paths() {
    std::map<std::string, std::string> found;
    for (const auto& p : glob(LEDS + "/rgb:[lr][1-4]")) found[p.substr(p.rfind(':') + 1)] = p;
    for (const char* name : ODIN2_ZONES)
        if (exists(LEDS + "/" + name + "/multi_intensity")) found[name] = LEDS + "/" + name;
    return found;
}

bool multicolor_here() {
    if (dt_compatible().find("ayn,thor") != std::string::npos) return false;  // Barry Launcher's Lights tab
    static const std::regex pbosd("KONKR Pocket FIT|AYANEO Pocket S2");
    if (std::regex_search(rd("/sys/firmware/devicetree/base/model"), pbosd)) return false;  // pbosd's
    return !led_paths().empty();
}

// Each ring clockwise from the top, left stick then right, by device model.
// The zones' numbers don't follow the rings the same way on every stick or
// device, though both share a device tree (mapped by eye 2026-10-07: on the
// RP6 the left ring sits a quarter turn on from the Nova's and the right is
// numbered the other way round).
struct RingOrder {
    const char* model;
    std::vector<std::string> order;
};
const std::vector<RingOrder>& ring_orders() {
    static const std::vector<RingOrder> orders = {
        {"Retroid Pocket Nova", {"l3", "l2", "l1", "l4", "r4", "r1", "r2", "r3"}},
        {"Retroid Pocket 6", {"l2", "l1", "l4", "l3", "r3", "r2", "r1", "r4"}},
        // Same HTR3212 groups as the Retroids (ROCKNIX DT); the Nova's order
        // until someone maps it by eye. Before "AYN Odin 2", which its model
        // starts with.
        {"AYN Odin 2 Portal", {"l3", "l2", "l1", "l4", "r4", "r1", "r2", "r3"}},
        // Two places per stick: the ring (place 0) and the side strip.
        {"AYN Odin 2", {"left-joystick", "left-side", "right-joystick", "right-side"}},
    };
    return orders;
}

const std::vector<std::string>& ring_order() {
    std::string model = dt_model();
    while (!model.empty() && (model.back() == '\0' || model.back() == ' ')) model.pop_back();
    for (const auto& o : ring_orders())
        if (model.rfind(o.model, 0) == 0) return o.order;
    return ring_orders()[0].order;
}

// The stick LEDs in ring order, so Spin and Rainbow Wave go round each
// stick, both sticks in step.
std::vector<std::string> zones() {
    auto found = led_paths();
    const auto& order = ring_order();
    std::set<std::string> have, want(order.begin(), order.end());
    for (const auto& [name, path] : found) have.insert(name);
    std::vector<std::string> out;
    if (have == want) {
        for (const auto& z : order) out.push_back(found[z]);
    } else {
        for (const auto& [name, path] : found) out.push_back(path);  // sorted by name
    }
    return out;
}

// Changes when the stick LED devices were made again, which loses what was
// set (white and off): the SM8550 sleep test unbinds their controllers for
// sleep. A plain sleep keeps the LEDs; the sleep hook fades them.
std::vector<ino_t> lights_signature() {
    std::vector<ino_t> out;
    for (const auto& z : zones()) {
        struct stat st;
        out.push_back(::stat(z.c_str(), &st) == 0 ? st.st_ino : 0);
    }
    return out;
}

std::string clean_color(const Value& color) {
    std::string c = lower(color.str());
    while (!c.empty() && c[0] == '#') c.erase(0, 1);
    c = c.substr(0, 6);
    static const std::regex hex("^[0-9a-f]{6}$");
    return std::regex_match(c, hex) ? c : DEFAULT_COLOR;
}

RGB rgb_of(const std::string& c) {
    return {double(std::stoi(c.substr(0, 2), nullptr, 16)), double(std::stoi(c.substr(2, 2), nullptr, 16)),
            double(std::stoi(c.substr(4, 2), nullptr, 16))};
}

// The saved settings, as tools.cpp's multicolor_lights() reads them.
struct Settings {
    bool on = true;
    std::string effect = "static";
    RGB rgb{}, rgb2{};
    int brightness = 160;
    int speed = 5;
    std::array<bool, 2> reverse{false, false};
};

Settings load_settings() {
    Value st = read_json(LIGHTS_STATE);
    Settings s;
    // Before effects there was a mode: static or off.
    s.on = st.has("on") ? st["on"].truthy() : st["mode"].str() != "off";
    bool audio = st["experimental_audio"].truthy();
    s.effect = st.has("effect") ? st["effect"].str() : DEFAULT_EFFECT;
    static const std::set<std::string> base = {"static", "breathing", "cycle", "wave", "spin", "starlight"};
    if (!base.count(s.effect) && !(audio && (s.effect == "audio" || s.effect == "spectrum"))) s.effect = "static";
    s.rgb = rgb_of(clean_color(st.has("color") ? st["color"] : Value(DEFAULT_COLOR)));
    s.rgb2 = rgb_of(clean_color(st.has("color2") ? st["color2"] : Value(DEFAULT_COLOR2)));
    s.brightness = static_cast<int>(std::clamp<long long>(st.has("brightness") ? st["brightness"].as_int(160) : 160, 0, 255));
    s.speed = static_cast<int>(std::clamp<long long>(st.has("speed") ? st["speed"].as_int(5) : 5, 1, 10));
    s.reverse = {st["reverse_left"].truthy(), st["reverse_right"].truthy()};
    return s;
}

// ----------------------------------------------------------------- effects --
constexpr double GAMMA = 2.2;
// Audio effects, as the eye sees it: the base color rests at a dim
// background (or its dimmest true level, if higher); loudness lifts a zone
// AUDIO_LEVEL_SHARE of the way from there to the brightness setting, and
// only hits go all the way, so they stand out over loud sound.
constexpr double AUDIO_BACKGROUND = 0.2;
constexpr double AUDIO_LEVEL_SHARE = 0.6;
// Audio Meter: the share of the level at which each place round a ring (0
// top, clockwise) fills: the bottom first, then both sides, then the top.
constexpr double METER_FILL[4][2] = {{2.0 / 3, 1.0}, {1.0 / 3, 2.0 / 3}, {0.0, 1.0 / 3}, {1.0 / 3, 2.0 / 3}};
// Audio Spectrum: which band (0 bass, 1 low mids, 2 high mids, 3 treble)
// each place round a ring shows (0 top, clockwise), per stick: bass at the
// bottom, treble at the top, low mids on the outer side, high mids inner.
constexpr int SPECTRUM_PLACES[2][4] = {{3, 2, 0, 1}, {3, 1, 0, 2}};
// With two places per stick (AYN Odin 2): bass in the ring, high mids (a
// snare's snap) on the side strip.
constexpr int SPECTRUM_PAIR[2] = {0, 2};
// Seconds the stick lights take to come back after a sleep: the sleep hook's
// fade-out (sm8550-sleep), the other way round.
constexpr double FADE_IN = 0.3;
// How long the LED devices settle after their last uevent before they are
// looked at.
constexpr double SETTLE = 0.3;
// A color's main channels: those at least this share of its strongest one.
constexpr double MAIN = 0.25;
// How much light each channel gives the eye (Rec. 709 luma: red, green, blue).
constexpr double LUMA[3] = {0.2126, 0.7152, 0.0722};
// Breathing goes no dimmer than where the weakest main channel has this many
// power steps left.
constexpr double FLOOR_STEPS = 4;
// How far (in zones) a zone's light reaches during Spin: 1 = only two zones
// share the light, each handing over as the other takes it.
constexpr double SPIN_WIDTH = 1.5;

// A full color of hue h (0-1) at the same total power for every hue: red to
// green to blue and back, each pair crossing over linearly. The usual
// rainbow lights two channels fully for yellow, cyan and magenta, twice the
// power of red, green or blue, and looked brighter there.
RGB even_hue(double h) {
    h = pymod(h, 1.0) * 3;
    int i = static_cast<int>(h) % 3;
    double f = h - std::floor(h);
    RGB ch{0, 0, 0};
    ch[i] = 255 * (1 - f);
    ch[(i + 1) % 3] = 255 * f;
    return ch;
}

// v is how bright it should look; LEDs look bright at low power, so the
// power is v ** 2.2 (as the sleep hook's fade), or fades would seem to snap
// on and hang before going off. Rounded once, in Animator.
RGB scale(const RGB& c, double v) {
    double k = v > 0 ? std::pow(v, GAMMA) : 0.0;
    return {c[0] * k, c[1] * k, c[2] * k};
}

RGB mix(const RGB& a, const RGB& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// Starlight: a zone's current or next twinkle; times in seconds.
struct Star {
    bool set = false;
    double start = 0, length = 0, peak = 0;
};

struct Levels {
    std::vector<double> levels, accents;  // 0-1 per channel
};

std::mt19937_64& rng() {
    static std::mt19937_64 r{std::random_device{}()};
    return r;
}
double expovariate(double lambda) { return std::exponential_distribution<double>(lambda)(rng()); }
double uniform(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng()); }

// Each zone's color at time t (seconds). Zones go round each ring of n / 2;
// both sticks show the same.
std::vector<RGB> frame(const std::string& effect, double t, const RGB& rgb, int speed, int n,
                       std::vector<Star>& stars, double floor, std::array<bool, 2> reverse, const Levels& level,
                       const RGB& rgb2) {
    int ring = std::max(1, n / 2);
    double fast = (speed - 1) / 9.0;  // 0 slowest .. 1 fastest
    // Zone i's place round its ring (0 = top, clockwise); a reversed stick is
    // mirrored, so its light starts at the top and turns the other way.
    auto place = [&](int i) {
        int p = i % ring;
        return reverse[std::min(i / ring, 1)] ? (ring - p) % ring : p;
    };
    std::vector<RGB> out;
    out.reserve(n);

    if (effect == "breathing") {
        // Breathes between floor (Animator::floor(): the dimmest level that
        // still shows the color) and full. Steady through the dim end and
        // slowing only towards full: a sine lingered at the bottom, where one
        // power step is a visible jump, so it held there and then jumped
        // (Nova, brightness 70).
        double period = 8 - 6.5 * fast;
        double w = 1 - std::abs(2 * pymod(t / period, 1) - 1);  // 0 -> 1 -> 0, straight
        double v = floor + (1 - floor) * std::sin(M_PI / 2 * w);
        out.assign(n, scale(rgb, v));
        return out;
    }
    if (effect == "cycle") {
        out.assign(n, even_hue(t / (24 - 21 * fast)));
        return out;
    }
    if (effect == "wave") {
        double period = 6 - 5 * fast;
        // Hues laid round the ring the other way from its places, so the
        // rainbow turns clockwise like Spin.
        for (int i = 0; i < n; ++i) out.push_back(even_hue(t / period + double((ring - place(i)) % ring) / ring));
        return out;
    }
    if (effect == "spin") {
        // One light going round. Each zone fades up as the light comes within
        // SPIN_WIDTH zones of it and down as it leaves, so the next zone is
        // already rising while this one peaks and the last is still fading:
        // through the Nova's clear shell, no gap between zones. The raised
        // cosines add up to the same total wherever the light is. One turn
        // takes 6 s at speed 1 down to 0.5 s at 10, each notch the same
        // factor faster.
        double head = (t / (6 * std::pow(0.5 / 6, fast))) * ring;
        for (int i = 0; i < n; ++i) {
            double d = std::abs(pymod(head - place(i) + ring / 2.0, ring) - ring / 2.0);  // distance round the ring
            double c = std::cos(M_PI * d / (2 * SPIN_WIDTH));
            out.push_back(scale(rgb, d < SPIN_WIDTH ? c * c : 0.0));
        }
        return out;
    }
    if (effect == "audio" || effect == "spectrum") {
        // level (AudioMeter::current()): levels and accents, 0-1 per channel.
        // The zone rests at the background in the base color; loudness lifts
        // it, a hit (accent) takes it towards rgb2, the intensity color, and
        // up to full: the brightness setting is the peak.
        const auto& lv = level.levels;
        const auto& ac = level.accents;
        double bg = std::max(floor, AUDIO_BACKGROUND);
        for (int i = 0; i < n; ++i) {
            int side = std::min(i / ring, 1);
            if (effect == "audio") {
                // Audio Meter: each stick its side, filling its ring like a
                // VU meter, bottom, then both sides, then the top (the peak,
                // in the intensity color); a beat kicks it up and turns the
                // lit LEDs towards the intensity color.
                size_t c = lv.size() > size_t(side) ? side : 0;
                double lvl = c < lv.size() ? std::max(lv[c], ac[c]) : 0.0;
                double a = c < ac.size() ? ac[c] : 0.0;
                int at = i % ring;
                double lo = ring == 4 ? METER_FILL[at][0] : 0.0, hi = ring == 4 ? METER_FILL[at][1] : 1.0;
                double f = std::min(1.0, std::max(0.0, (lvl - lo) / (hi - lo)));
                double m = (at == 0 && ring == 4) ? 1.0 : a;
                RGB col = mix(rgb, rgb2, m);
                out.push_back(scale(f != 0 ? col : rgb, bg + (1 - bg) * f));
                continue;
            }
            // Audio Spectrum: each stick its side's four bands.
            int band = ring == 4 ? SPECTRUM_PLACES[side][i % ring] : SPECTRUM_PAIR[i % ring % 2];
            int c = lv.size() >= 8 ? side * 4 + band : -1;
            double v_level = c >= 0 ? lv[c] : 0.0, a = c >= 0 ? ac[c] : 0.0;
            double v = bg + (1 - bg) * std::max(AUDIO_LEVEL_SHARE * v_level, a);
            out.push_back(scale(mix(rgb, rgb2, a), v));
        }
        return out;
    }
    if (effect == "starlight") {
        // Each zone twinkles on its own: every twinkle has its own length
        // (around life) and peak, and the wait before the next is drawn like
        // natural random events (exponential), so no rhythm forms. The times
        // are seconds, so the frame rate doesn't matter.
        double life = 2.4 - 1.8 * fast;
        if (stars.size() < size_t(n)) stars.resize(n);
        for (int i = 0; i < n; ++i) {
            Star& st = stars[i];
            if (!st.set) st = {true, t + expovariate(1 / life), 0.0, 0.0};
            if (st.length == 0.0 || t > st.start + st.length) {  // new twinkle after a random wait
                st.start = std::max(t, st.start + st.length) + expovariate(1 / (0.8 * life));
                st.length = life * uniform(0.6, 1.4);
                st.peak = uniform(0.45, 1.0);
            }
            double age = t - st.start;
            double v = (age >= 0 && age <= st.length) ? st.peak * std::sin(M_PI * age / st.length) : 0.0;
            out.push_back(scale(rgb, v));
        }
        return out;
    }
    out.assign(n, rgb);
    return out;
}

// ------------------------------------------------------------------- audio --
// Audio Spectrum's bands come from plain listeners at falling sample rates:
// PipeWire's resampler drops everything above half the rate, in native code
// and steeply, so each listener holds the energy below one edge and the
// bands are differences (energy in separate ranges adds up). A tone lands in
// one band, the others 17-150 dB down. A filter-chain process did this
// first, but with it the first sound after the speaker suspended could not
// start for 5 s or more (every other sound, Nova, 15 tries); these: none.
//
// The edges, Hz: below the first nothing counts; then bass, low mids, high
// mids (a snare's snap), treble (hi-hats). Through the built-in speakers
// only what they can play counts: small handheld drivers give little below
// about SPEAKER_LOW_HZ (an estimate, not measured on these devices).
constexpr int SPEAKER_LOW_HZ = 300;
const std::vector<int> EDGES_FULL = {0, 250, 1000, 5000, 12000};
const std::vector<int> EDGES_SPEAKER = {SPEAKER_LOW_HZ, 600, 1500, 5000, 12000};

// The sound goes out of the built-in speakers: the default output is the
// device's own card and nothing is in the headphone jack or video out.
bool on_speakers(const std::string& sink) {
    if (sink.rfind("alsa_output.platform-sound", 0) != 0) return false;  // Bluetooth, USB, ...
    for (const char* jack : {"Headphone Jack", "DP0 Jack"}) {
        RunOpts o;
        o.timeout = 5;
        auto r = run({"amixer", "-c", "0", "cget", std::string("iface=CARD,name=") + jack}, o);
        if (r.out.find("values=on") != std::string::npos) return false;
    }
    return true;
}

// How the device's sound output moves, for the audio effects, as the user
// that owns the sound session; never a microphone.
//
// "pulse" (Audio Meter): the default output's monitor in stereo at 24 kHz
// (through the speakers less a listener at the lowest edge); channels [left,
// right]. "spectrum" (Audio Spectrum): listeners at twice each non-zero edge
// (EDGES_SPEAKER or EDGES_FULL, by on_speakers()); channels [left bass, low
// mids, high mids, treble, right bass, ...].
//
// Per channel it gives a level (loudness in dB against its own loudest of
// the last few seconds, rising at once, falling over 0.3 s; in "spectrum" a
// band far below its side's loudest stays dark) and an accent: a hit, a
// sudden rise against its own recent average, which flashes and fades. The
// high mids and treble (snare, hi-hats) hit easiest and fastest and take
// their full level from a narrower loudness range, so they show most.
//
// Every listener is passive (node.passive): it never keeps the sound card
// running, so with nothing playing it gets no data and the light rests at
// its glow; and can't be moved (node.dont-move), so the sleep hook's shuffle
// of streams onto its silent stand-in can't land it on another source after
// a wake. They run only while an audio effect shows, and start again if one
// ends, or the output or its kind (speakers or not) changes. pw-record runs
// unbuffered: it writes a pipe in 4 kB blocks, and measured on the Nova the
// light came 0.9 s after the sound; unbuffered about 0.1 s.
//
// One run (MeterRun) per start: its thread owns it with the meter, so a stop
// that the thread can't hear at once (it waits on pactl or amixer, up to 5 s
// each) leaves it to end by itself instead of holding up the loop.
class MeterRun : public std::enable_shared_from_this<MeterRun> {
public:
    explicit MeterRun(const std::string& mode) : mode_(mode == "spectrum" ? "spectrum" : "pulse") { reset(); }
    ~MeterRun() {
        if (stop_fd_ >= 0) close(stop_fd_);
    }

    bool begin() {
        stop_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (stop_fd_ < 0) {
            log("Audio effects: eventfd: %s", std::strerror(errno));
            return false;
        }
        std::thread([self = shared_from_this()] {
            self->supervise();
            std::lock_guard<std::mutex> g(self->lock_);
            self->done_ = true;
            self->ended_.notify_all();
        }).detach();
        return true;
    }

    // Asks the thread to stop and waits up to wait seconds for it.
    void end(double wait) {
        uint64_t one = 1;
        ssize_t r = ::write(stop_fd_, &one, sizeof one);
        (void)r;
        std::unique_lock<std::mutex> g(lock_);
        if (!ended_.wait_for(g, std::chrono::duration<double>(wait), [this] { return done_; }))
            log("Audio effects: listener still busy, left to end by itself");
    }

    const std::string& mode() const { return mode_; }

    // Levels and accents now; they fall away when no sound has come.
    Levels current() {
        std::lock_guard<std::mutex> g(lock_);
        return current_unlocked(now_s());
    }

private:
    // Per band (spectrum) or for the meter (pulse): the quietest that lights,
    // below its loudest; the rise over its recent average that is a hit; how
    // long a hit fades; how long a level falls. The meter falls fast, so it
    // drops between beats instead of trailing them.
    static constexpr double RANGE_DB[4] = {30.0, 30.0, 22.0, 22.0};
    static constexpr double METER_RANGE_DB = 24.0;
    static constexpr double METER_HIT_DB = 6.0;
    static constexpr double METER_HIT_FADE = 0.15;
    static constexpr double METER_FALL = 0.12;
    static constexpr double SPECTRUM_FALL = 0.15;
    static constexpr double GATE_DB = 15.0;  // spectrum: a band this far below its side's loudest is dark
    static constexpr double HIT_DB[4] = {9.0, 9.0, 5.0, 5.0};     // per band: rise over its recent average that is a hit
    static constexpr double HIT_FADE[4] = {0.30, 0.30, 0.15, 0.12};  // per band: seconds a hit fades over
    // A hit is a rise over the band's average of about the last HIT_AVG
    // seconds (floored at -60 dB): short, so a held note stops counting
    // within a few frames (over 0.5 s, a tone after silence held a zone at
    // full for a second on the Nova).
    static constexpr double HIT_AVG = 0.12;
    static constexpr double WINDOW = 1.0 / 30;  // seconds of sound each energy is measured over

    bool pulse() const { return mode_ == "pulse"; }
    double range(size_t c) const { return pulse() ? METER_RANGE_DB : RANGE_DB[c % 4]; }
    double hit_db(size_t c) const { return pulse() ? METER_HIT_DB : HIT_DB[c % 4]; }
    double hit_fade(size_t c) const { return pulse() ? METER_HIT_FADE : HIT_FADE[c % 4]; }
    double fall() const { return pulse() ? METER_FALL : SPECTRUM_FALL; }

    void reset() {
        size_t n = pulse() ? 2 : 8;
        levels_.assign(n, 0.0);
        accents_.assign(n, 0.0);
        peaks_.assign(n, -60.0);
        averages_.assign(n, -60.0);
        at_ = 0.0;  // when the levels were last measured
    }

    Levels current_unlocked(double now) const {
        Levels out{std::vector<double>(levels_.size(), 0.0), std::vector<double>(accents_.size(), 0.0)};
        if (!at_) return out;
        double idle = std::max(0.0, now - at_);
        double k = std::exp(-std::max(0.0, idle - 0.05) / fall());
        for (size_t c = 0; c < levels_.size(); ++c) out.levels[c] = levels_[c] * k;
        for (size_t c = 0; c < accents_.size(); ++c) out.accents[c] = accents_[c] * std::exp(-idle / hit_fade(c));
        return out;
    }

    // True when stopped within seconds.
    bool wait(double seconds) {
        pollfd p{stop_fd_, POLLIN, 0};
        return poll(&p, 1, static_cast<int>(seconds * 1000)) > 0;
    }

    struct Listener {
        pid_t pid = -1;
        int fd = -1;
        int rate = 0;
        int error = 0;
    };

    static bool session(std::string& user, std::string& rt) {
        // The user with the sound session (Game Mode's steamos) and its runtime dir.
        passwd* pw = getpwnam("steamos");
        if (!pw) return false;
        rt = "/run/user/" + std::to_string(pw->pw_uid);
        if (!exists(rt + "/pipewire-0")) return false;
        user = "steamos";
        return true;
    }

    static std::string default_sink(const std::string& user, const std::string& rt) {
        RunOpts o;
        o.timeout = 5;
        return trim(run({"runuser", "-u", user, "--", "env", "XDG_RUNTIME_DIR=" + rt, "pactl", "get-default-sink"}, o).out);
    }

    std::vector<int> rates() const {
        if (pulse()) {
            // Up to 12 kHz, the hi-hats too (at 8 kHz, so below 4 kHz, they
            // never showed); through the speakers less what they can't play
            // (a listener at the lowest edge, subtracted).
            int top = 2 * edges_.back();
            return edges_[0] ? std::vector<int>{2 * edges_[0], top} : std::vector<int>{top};
        }
        std::vector<int> out;
        for (int e : edges_)
            if (e) out.push_back(2 * e);
        return out;
    }

    static Listener listen(const std::string& user, const std::string& rt, int rate) {
        std::vector<std::string> args = {
            "/usr/bin/runuser", "-u", user, "--", "env", "XDG_RUNTIME_DIR=" + rt, "stdbuf", "-o0", "pw-record",
            "--raw", "--rate", std::to_string(rate), "--channels", "2", "--format", "s16", "--latency", "16ms", "-P",
            "{ stream.capture.sink=true node.passive=true node.dont-move=true node.name=pbos-utils-lighting-" +
                std::to_string(rate) + " media.name=\"PB-OS Utils lighting\" }",
            "-"};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        Listener l;
        l.rate = rate;
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            l.error = errno;
            return l;
        }
        pid_t parent = getpid();
        pid_t pid = fork();
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() != parent) _exit(0);  // the engine already ended
            int null = open("/dev/null", O_RDWR);
            dup2(null, 0);
            dup2(fds[1], 1);
            dup2(null, 2);
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
        close(fds[1]);
        if (pid < 0) {
            l.error = errno;
            close(fds[0]);
            return l;
        }
        l.pid = pid;
        l.fd = fds[0];
        return l;
    }

    static void end_all(std::vector<Listener>& ls) {
        for (auto& l : ls)
            if (l.pid > 0) kill(l.pid, SIGTERM);
        for (auto& l : ls) {
            if (l.pid > 0) {
                bool gone = false;
                for (int i = 0; i < 100 && !gone; ++i) {
                    gone = waitpid(l.pid, nullptr, WNOHANG) == l.pid;
                    if (!gone) usleep(10000);
                }
                if (!gone) {
                    kill(l.pid, SIGKILL);
                    waitpid(l.pid, nullptr, 0);
                }
            }
            if (l.fd >= 0) close(l.fd);
        }
        ls.clear();
    }

    void supervise() {
        while (true) {
            std::string user, rt;
            if (!session(user, rt)) {
                if (wait(5)) return;
                continue;
            }
            std::string sink = default_sink(user, rt);
            bool speakers = on_speakers(sink);
            edges_ = speakers ? EDGES_SPEAKER : EDGES_FULL;
            std::vector<Listener> ls;
            int error = 0;
            for (int rate : rates()) {
                ls.push_back(listen(user, rt, rate));
                if ((error = ls.back().error)) break;
            }
            if (error) {
                log("Audio effects: can't listen: %s", std::strerror(error));
                end_all(ls);
                if (wait(5)) return;
                continue;
            }
            bool stopped = read(ls, user, rt, sink, speakers);
            end_all(ls);
            if (stopped || wait(1)) return;
        }
    }

    // Mean square per side of stereo s16 sound.
    static std::array<double, 2> mean_squares(const std::vector<int16_t>& s) {
        std::array<double, 2> out{0.0, 0.0};
        size_t frames = s.size() / 2;
        if (!frames) return out;
        for (size_t i = 0; i < frames; ++i) {
            out[0] += double(s[2 * i]) * s[2 * i];
            out[1] += double(s[2 * i + 1]) * s[2 * i + 1];
        }
        out[0] /= frames;
        out[1] /= frames;
        return out;
    }

    // Until a listener ends, the output changes (false) or it is stopped (true).
    bool read(std::vector<Listener>& ls, const std::string& user, const std::string& rt, const std::string& sink,
              bool speakers) {
        size_t n = ls.size();
        // Each listener: its last WINDOW of sound (stereo s16 samples), and
        // its energy (mean square) per side.
        std::vector<std::vector<int16_t>> windows(n);
        std::vector<std::string> bufs(n);
        std::vector<std::array<double, 2>> energy(n, {0.0, 0.0});
        std::vector<pollfd> pfds;
        for (auto& l : ls) pfds.push_back({l.fd, POLLIN, 0});
        pfds.push_back({stop_fd_, POLLIN, 0});
        double checked = now_s();
        std::vector<char> chunk(1 << 16);
        while (true) {
            int r = poll(pfds.data(), pfds.size(), 1000);
            if (r < 0 && errno != EINTR) return false;
            if (pfds.back().revents) return true;
            for (size_t i = 0; i < n && r > 0; ++i) {
                if (!pfds[i].revents) continue;
                ssize_t got = ::read(pfds[i].fd, chunk.data(), chunk.size());
                if (got <= 0) return false;  // a listener ended: start them all again
                bufs[i].append(chunk.data(), got);
                size_t whole = bufs[i].size() / 4 * 4;
                size_t keep = std::max<size_t>(8, size_t(ls[i].rate * WINDOW)) * 2;  // samples
                auto& w = windows[i];
                size_t had = w.size();
                w.resize(had + whole / 2);
                std::memcpy(w.data() + had, bufs[i].data(), whole);
                if (w.size() > keep) w.erase(w.begin(), w.end() - keep);
                bufs[i].erase(0, whole);
                energy[i] = mean_squares(w);
                if (i == n - 1) measure(energy);  // the fastest listener: once per frame
            }
            if (now_s() - checked > 5) {
                checked = now_s();
                std::string now_sink = default_sink(user, rt);
                if (now_sink != sink || on_speakers(now_sink) != speakers)
                    return false;  // other output, or headphones in or out: listen again
            }
        }
    }

    // Mean-square power per channel from the listeners' energies.
    std::vector<double> band_power(const std::vector<std::array<double, 2>>& energy) const {
        if (pulse()) {
            if (energy.size() == 2)  // the speakers: less what they can't play
                return {std::max(0.0, energy[1][0] - energy[0][0]), std::max(0.0, energy[1][1] - energy[0][1])};
            return {energy[0][0], energy[0][1]};
        }
        // e[k]: energy below the k-th non-zero edge; with a lowest edge (the
        // speakers) everything below it is taken away first.
        std::vector<double> power;
        bool skip = edges_[0] != 0;
        for (int side = 0; side < 2; ++side) {
            std::vector<double> e;
            if (!skip) e.push_back(0.0);
            for (const auto& x : energy) e.push_back(x[side]);
            for (int k = 0; k < 4; ++k) power.push_back(std::max(0.0, e[k + 1] - e[k]));
        }
        return power;
    }

    void measure(const std::vector<std::array<double, 2>>& energy) {
        const double full = 32768.0 * 32768.0;
        std::vector<double> dbs;
        for (double x : band_power(energy)) dbs.push_back(10 * std::log10(std::max(x / full, 1e-12)));
        double now = now_s();
        std::lock_guard<std::mutex> g(lock_);
        double dt = at_ ? now - at_ : 0.0;
        Levels prev = current_unlocked(now);
        size_t n = std::min(dbs.size(), levels_.size());
        size_t side = n > 2 ? n / 2 : 1;
        for (size_t c = 0; c < n; ++c) {
            double db = dbs[c];
            // The loudest of the last few seconds, falling 3 dB a second.
            peaks_[c] = std::max({db, peaks_[c] - 3 * dt, -60.0});
            double p = peaks_[c], rng = range(c);
            double target = p <= -55 ? 0.0 : std::min(1.0, std::max(0.0, (db - (p - rng)) / rng));
            bool gated = false;
            if (n > 2) {
                size_t from = c / side * side;
                double loudest = *std::max_element(dbs.begin() + from, dbs.begin() + from + side);
                gated = db < loudest - GATE_DB;
            }
            if (gated) target = 0.0;
            levels_[c] = target >= prev.levels[c] ? target
                                                  : prev.levels[c] + (target - prev.levels[c]) * std::min(1.0, dt / fall());
            // A hit: a sudden rise over the band's own recent average.
            double rise = db - averages_[c];
            double acc = prev.accents[c];
            if (!gated && p > -55 && rise > hit_db(c)) acc = std::max(acc, std::min(1.0, 0.5 + (rise - hit_db(c)) / 12));
            accents_[c] = acc;
            averages_[c] = std::max(-60.0, averages_[c] + (db - averages_[c]) * std::min(1.0, dt / HIT_AVG));
        }
        at_ = now;
    }

    const std::string mode_;
    std::vector<int> edges_ = EDGES_FULL;  // the supervising thread's
    int stop_fd_ = -1;
    std::mutex lock_;
    std::condition_variable ended_;
    bool done_ = false;
    std::vector<double> levels_, accents_, peaks_, averages_;
    double at_ = 0.0;
};

class AudioMeter {
public:
    ~AudioMeter() { stop(); }

    void start(const std::string& mode) {
        if (run_ && run_->mode() == mode) return;
        stop();
        auto run = std::make_shared<MeterRun>(mode);
        if (run->begin()) run_ = std::move(run);
    }

    // As Python's join(timeout=2): a busy listener thread is not waited for.
    void stop() {
        if (run_) run_->end(2);
        run_.reset();
    }

    Levels current() { return run_ ? run_->current() : Levels{}; }

private:
    std::shared_ptr<MeterRun> run_;
};

// ---------------------------------------------------------------- animator --
// Drives the stick LEDs. Static and off are written once; effects run on the
// loop's timer at FPS, writing only zones whose color changed (each write is
// an I2C transfer to the LED driver).
class Animator {
public:
    explicit Animator(Loop& loop) : loop_(loop) {}

    // Measured on the Nova (Spin, 20 s each, Python): 20 and 30 fps both cost
    // ~1.7 % of one core, 60 fps 3.7 %; a frame's writes take under 1 ms.
    // Battery draw was below what the rest of the system varies by at all three.
    static constexpr int FPS = 30;

    // Shows s; with fade, brightens from off to it over that many seconds
    // (the effect already running underneath). keep_brightness: the zones'
    // brightness is left as it is (after the charging indicator, which put
    // it back, and sm8550-sleep may be fading it in).
    void apply(const Settings& s, double fade = 0.0, bool keep_brightness = false) {
        halt();
        failed_.clear();
        order_.clear();  // read again: the devices may be new
        tops_.clear();
        leds_ = zones();
        // The zones' own brightness stays at full and the set brightness goes
        // into the channels (power()): the kernel would otherwise scale each
        // channel again and round it down, and a fading pink came out as red
        // alone near the end (blue already 0, red still 1).
        level_ = s.brightness / 255.0;
        if (leds_.empty()) return;  // all unbound: shown when they come back
        if (!s.on) {
            if (!keep_brightness)
                for (const auto& led : leds_) write(led, "brightness", "0");
            return;
        }
        int n = static_cast<int>(leds_.size());
        // The color first, then the brightness: LEDs made again (after a
        // sleep) start at full white, and brightness first flashed it.
        std::vector<Star> first_stars;
        std::vector<RGB> first = fade ? std::vector<RGB>(n, RGB{0, 0, 0})
                                      : frame(s.effect, 0.0, s.rgb, s.speed, n, first_stars, 0.0, s.reverse, {}, s.rgb2);
        for (int i = 0; i < n; ++i) {
            write(leds_[i], "multi_intensity", power(leds_[i], first[i]));
            if (!keep_brightness) write(leds_[i], "brightness", rd(leds_[i] + "/max_brightness", "255"));
        }
        if (s.effect == "static" && !fade) return;
        run_ = Run{};
        run_.s = s;
        run_.fade = fade;
        run_.audio = s.effect == "audio" ? "pulse" : s.effect == "spectrum" ? "spectrum" : "";
        run_.floor = n && (s.effect == "breathing" || !run_.audio.empty()) ? floor(leds_[0], s.rgb) : 0.0;
        run_.fine = s.effect == "breathing" || s.effect == "starlight" || !run_.audio.empty();  // fades: power_smooth
        run_.shown.assign(n, "");
        if (!run_.audio.empty()) meter_.start(run_.audio);
        // Frame k is drawn for exactly k / FPS and written at that time, so
        // the steps are even and the rate is FPS, not FPS minus the time the
        // writes take. Behind by more than a frame (busy, or paused): skip to
        // the current one rather than rush to catch up.
        run_.t0 = now_s();
        running_ = true;
        tick();
    }

    void halt() {
        if (timer_) loop_.cancel(timer_);
        timer_ = 0;
        running_ = false;
        meter_.stop();
    }

private:
    struct Run {
        Settings s;
        double fade = 0, floor = 0, t0 = 0;
        long long k = 0;
        std::string audio;
        bool fine = false;
        std::vector<std::string> shown;
        std::vector<Star> stars;
        std::map<std::string, std::pair<std::array<int, 3>, RGB>> smooth;  // Breathing: last pick per zone
    };

    void tick() {
        timer_ = 0;
        if (!running_) return;
        Run& r = run_;
        int n = static_cast<int>(leds_.size());
        // Fading in: the sleep hook's fade-out reversed (x ** 2.2).
        double ramp_r = r.fade ? std::min(1.0, r.k / double(FPS) / r.fade) : 1.0;
        double ramp = std::pow(ramp_r, GAMMA);
        Levels level = r.audio.empty() ? Levels{} : meter_.current();
        auto colors = frame(r.s.effect, r.k / double(FPS), r.s.rgb, r.s.speed, n, r.stars, r.floor, r.s.reverse, level,
                            r.s.rgb2);
        for (int i = 0; i < n; ++i) {
            RGB c = {colors[i][0] * ramp, colors[i][1] * ramp, colors[i][2] * ramp};
            std::string value = r.fine ? power_smooth(leds_[i], c, r.smooth) : power(leds_[i], c);
            if (value != r.shown[i]) {
                write(leds_[i], "multi_intensity", value);
                r.shown[i] = value;
            }
        }
        if (r.s.effect == "static" && ramp_r >= 1) {
            halt();  // faded in; static needs no more frames
            return;
        }
        if (!failed_.empty()) {
            halt();  // LEDs gone (unbound for sleep): the fade-in starts it again
            return;
        }
        r.k += 1;
        double late = now_s() - (r.t0 + r.k / double(FPS));
        if (late > 1.0 / FPS) r.k += static_cast<long long>(late * FPS);
        timer_ = loop_.after(std::max(0.0, r.t0 + r.k / double(FPS) - now_s()), [this] { tick(); });
    }

    // The zone's max_brightness, read once per apply (power() runs 30 times a
    // second per zone).
    int top(const std::string& led) {
        auto it = tops_.find(led);
        if (it != tops_.end()) return it->second;
        std::string v = rd(led + "/max_brightness", "255");
        int t = is_digits(v) ? std::atoi(v.c_str()) : 255;
        tops_[led] = t;
        return t;
    }

    // Where red, green and blue go in multi_intensity: its multi_index,
    // which on the Retroid Pocket 6 / Nova is "blue green red".
    const std::array<int, 3>& channels(const std::string& led) {
        auto it = order_.find(led);
        if (it != order_.end()) return it->second;
        std::array<int, 3> order{0, 1, 2};
        std::vector<std::string> names;
        std::string idx = rd(led + "/multi_index");
        for (size_t i = 0; i < idx.size();) {
            while (i < idx.size() && idx[i] == ' ') ++i;
            size_t j = idx.find(' ', i);
            if (j == std::string::npos) j = idx.size();
            if (j > i) names.push_back(idx.substr(i, j - i));
            i = j;
        }
        auto sorted = names;
        std::sort(sorted.begin(), sorted.end());
        if (sorted == std::vector<std::string>{"blue", "green", "red"}) {
            for (int i = 0; i < 3; ++i) order[i] = names[i] == "red" ? 0 : names[i] == "green" ? 1 : 2;
        }
        return order_[led] = order;
    }

    // The dimmest level (as it looks, 0-1) at which rgb at the set brightness
    // still has FLOOR_STEPS power steps in its weakest main channel. Below
    // that the few whole steps left can't hold the color (a pink at
    // brightness 70 turned red: blue 1, red 3) and every step is a visible
    // jump.
    double floor(const std::string& led, const RGB& rgb) {
        double t = top(led);
        double strongest = std::max({rgb[0], rgb[1], rgb[2]});
        std::vector<double> main;
        for (double c : rgb)
            if (c && c >= MAIN * strongest) main.push_back(c * level_ * t / 255);
        if (main.empty()) return 0.0;
        double weakest = *std::min_element(main.begin(), main.end());
        if (weakest <= FLOOR_STEPS) return 1.0;
        return std::pow(FLOOR_STEPS / weakest, 1 / GAMMA);
    }

    static double light(const RGB& ch) { return LUMA[0] * ch[0] + LUMA[1] * ch[1] + LUMA[2] * ch[2]; }
    static double light(const std::array<int, 3>& ch) { return LUMA[0] * ch[0] + LUMA[1] * ch[1] + LUMA[2] * ch[2]; }

    std::string format(const std::string& led, const std::array<int, 3>& ch) {
        const auto& o = channels(led);
        return std::to_string(ch[o[0]]) + " " + std::to_string(ch[o[1]]) + " " + std::to_string(ch[o[2]]);
    }

    // power() for a slow fade: each channel rounded up or down, picking the
    // mix whose light (LUMA) is nearest the target, which gives about twice
    // as many brightness steps at the dim end as rounding each channel alone
    // (Nova, pink at 50: largest step 25 % -> 13 %). While the target dims
    // the light may only stay or dim, and the other way round, so it never
    // wobbles. No channel is more than one step off.
    std::string power_smooth(const std::string& led, const RGB& rgb,
                             std::map<std::string, std::pair<std::array<int, 3>, RGB>>& last) {
        double t = top(led);
        RGB want = {rgb[0] * level_ * t / 255, rgb[1] * level_ * t / 255, rgb[2] * level_ * t / 255};
        double strongest = std::max({want[0], want[1], want[2]});
        bool main[3];
        for (int i = 0; i < 3; ++i) main[i] = want[i] && want[i] >= MAIN * strongest;
        auto it = last.find(led);
        bool have_prev = it != last.end();
        int going = 0;
        if (have_prev) {
            double a = light(want), b = light(it->second.second);
            going = (a > b) - (a < b);
        }
        bool found = false;
        double best_cost = 0;
        std::array<int, 3> best{};
        // Every mix of each channel rounded down or up, the last channel
        // changing fastest (itertools.product's order: the first best wins).
        for (int m = 0; m < 8; ++m) {
            std::array<int, 3> ch;
            for (int i = 0; i < 3; ++i) {
                int bit = (m >> (2 - i)) & 1;
                ch[i] = static_cast<int>(bit ? std::ceil(want[i]) : std::floor(want[i]));
            }
            bool any = ch[0] || ch[1] || ch[2];
            bool tinted = false;
            for (int i = 0; i < 3; ++i)
                if (main[i] && !ch[i]) tinted = true;
            if (any && tinted) continue;  // a main channel dark: tinted
            if (have_prev && going * (light(ch) - light(it->second.first)) < -1e-9) continue;  // against the fade
            double d = light(ch) - light(want);
            double off = 0;
            for (int i = 0; i < 3; ++i) off += (ch[i] - want[i]) * (ch[i] - want[i]);
            double cost = d * d + 0.02 * off;
            if (!found || cost < best_cost) {
                found = true;
                best_cost = cost;
                best = ch;
            }
        }
        std::array<int, 3> ch = found ? best : have_prev ? it->second.first : std::array<int, 3>{0, 0, 0};
        last[led] = {ch, want};
        return format(led, ch);
    }

    // multi_intensity for rgb (0-255, may be fractional) at the set
    // brightness: each channel's final power, rounded once (half to even, as
    // Python's round()). Near the end of a fade a main channel of the color
    // (MAIN of the strongest or more) would round to 0 while another is still
    // lit, tinting the zone, so the zone is off from there.
    std::string power(const std::string& led, const RGB& rgb) {
        double t = top(led);
        RGB ch = {rgb[0] * level_ * t / 255, rgb[1] * level_ * t / 255, rgb[2] * level_ * t / 255};
        double strongest = std::max({ch[0], ch[1], ch[2]});
        bool dark = false;
        for (double x : ch)
            if (x >= MAIN * strongest && x < 0.5) dark = true;
        std::array<int, 3> out{0, 0, 0};
        if (!dark)
            for (int i = 0; i < 3; ++i) out[i] = static_cast<int>(std::nearbyint(ch[i]));
        return format(led, out);
    }

    void write(const std::string& led, const char* attr, const std::string& value) {
        std::string path = led + "/" + attr;
        int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
        ssize_t n = -1;
        if (fd >= 0) {
            do n = ::write(fd, value.data(), value.size());
            while (n < 0 && errno == EINTR);
            int e = errno;
            close(fd);
            errno = e;
        }
        if (n == static_cast<ssize_t>(value.size())) return;
        // Gone while the controller is unbound for sleep: say so once.
        if (failed_.insert(led).second) log("%s: %s", path.c_str(), std::strerror(errno));
    }

    Loop& loop_;
    AudioMeter meter_;
    std::vector<std::string> leds_;
    std::set<std::string> failed_;
    std::map<std::string, std::array<int, 3>> order_;
    std::map<std::string, int> tops_;
    double level_ = 1.0;
    Run run_;
    bool running_ = false;
    uint64_t timer_ = 0;
};

// ------------------------------------------------------------------ engine --
// Shows LIGHTS_STATE on the stick lights: at start, on SIGHUP, and again,
// fading in, when the LED devices were made again (the SM8550 sleep test
// unbinds their controllers for sleep; they come back white and off, and the
// sleep hook's own fade-in finds none).
class Engine {
public:
    explicit Engine(Loop& loop) : loop_(loop), animator_(loop) {}

    void start() {
        // Listening first, then the first look, so nothing falls in between.
        // An overflowed inotify queue (IN_Q_OVERFLOW) may have lost the
        // indicator's events: looked at again.
        if (!loop_.watch(INDICATOR_DIR,
                         IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_Q_OVERFLOW,
                         [this](uint32_t mask, const std::string& name) {
                             if (name == INDICATOR_NAME || (mask & IN_Q_OVERFLOW)) lend();
                         }))
            log("stick lights: cannot watch %s; the charging indicator is looked at on SIGHUP only",
                INDICATOR_DIR.c_str());
        // An empty subsystem: uevents were dropped (ENOBUFS), maybe the LEDs'.
        loop_.on_uevent([this](const std::string& subsystem, const std::string&) {
            if (subsystem != "leds" && !subsystem.empty()) return;
            if (settle_) loop_.cancel(settle_);
            settle_ = loop_.after(SETTLE, [this] {
                settle_ = 0;
                devices_changed();
            });
        });
        last_ = lights_signature();
        if (!lend()) reload();
    }

    void reload(double fade = 0.0) {
        if (lend()) return;  // while lent, shown when they are back
        animator_.apply(load_settings(), fade);
    }

    void stop() { animator_.halt(); }

private:
    // True while the charging indicator has the lights. When it gives them
    // back, they are shown again, brightness untouched.
    bool lend() {
        pid_t owner = indicator_owner();
        if (owner) {
            if (!lent_) {
                animator_.halt();
                lent_ = true;
                log("stick lights: lent to the charging indicator");
            }
            // Its owner ending without taking the file away gives them back
            // too. Gone before its pidfd could be had: looked at again soon.
            if (owner != owner_) {
                if (loop_.on_exit(owner, [this, owner] {
                        if (owner_ == owner) owner_ = 0;
                        lend();
                    }))
                    owner_ = owner;
                else
                    loop_.after(0.5, [this] { lend(); });
            }
            return true;
        }
        if (lent_) {
            lent_ = false;
            last_ = lights_signature();
            log("stick lights: back from the charging indicator");
            animator_.apply(load_settings(), 0.0, true);
        }
        return false;
    }

    // The LED devices settled after uevents: set again if they were made again.
    void devices_changed() {
        if (lent_) return;
        auto now = lights_signature();
        if (now == last_ || now.size() != last_.size() || std::find(now.begin(), now.end(), 0) != now.end())
            return;  // same devices, or still coming back
        last_ = now;
        log("stick lights: set again after sleep");
        reload(FADE_IN);
    }

    Loop& loop_;
    Animator animator_;
    std::vector<ino_t> last_;
    bool lent_ = false;
    pid_t owner_ = 0;
    uint64_t settle_ = 0;
};

}  // namespace

int run_lights() {
    Loop loop;  // blocks SIGTERM, SIGINT and SIGHUP first, for every thread
    if (!multicolor_here()) {
        log("no stick lights to drive here");
        return 0;
    }
    Engine engine(loop);
    loop.on_signal([&](int sig) {
        if (sig == SIGHUP) {
            engine.reload();
            return;
        }
        engine.stop();
        loop.quit();
    });
    engine.start();
    loop.run();
    engine.stop();
    return 0;
}

}  // namespace pq
