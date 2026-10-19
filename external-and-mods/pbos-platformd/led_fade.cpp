// led-fade (was the python fade in sm8550-sleep): pbos-platformd led-fade
// out|in SAVE. Every LED in SAVE ("led brightness" lines, written by the
// sleep hook) from its saved brightness to 0, or back up to it, over 0.3 s.
// Each LED steps through every level it passes, on a gamma curve (x ** 2.2)
// so the fade looks even to the eye; pbos-qamd's stick lights fade back in
// after a sleep on the same curve and time (lights.cpp FADE_IN, scale()).
//
// The kernel scales each color channel by the brightness and rounds down,
// so near 0 a pink would show red alone (blue already 0): below the level
// where every main channel of its color (a quarter of the strongest or
// more) still lights, the LED is off.
//
// The sleep hook ran this as python3 twice per sleep, a Python start-up on
// the way into the sleep and another on the way out.
//
// As the Python did: everything is read and every brightness opened before
// the first write, so a bad line or an LED that is gone writes nothing and
// fails (the hook goes on); "in" is anything but "out".

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

constexpr double T = 0.3;
constexpr double GAMMA = 2.2;
constexpr double STEP = 0.01;  // seconds between frames

// Python's str.split() whitespace (ASCII).
bool py_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f); }

std::vector<std::string> py_split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && py_space(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !py_space(s[j])) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// Python's int(text): whitespace round it, a sign, digits with single
// underscores between them.
std::optional<long long> py_int(const std::string& text) {
    size_t i = 0, e = text.size();
    while (i < e && py_space(text[i])) ++i;
    while (e > i && py_space(text[e - 1])) --e;
    bool neg = false;
    if (i < e && (text[i] == '+' || text[i] == '-')) neg = text[i++] == '-';
    if (i == e) return std::nullopt;
    long long v = 0;
    bool digit_before = false;
    for (; i < e; ++i) {
        char c = text[i];
        if (c == '_' && digit_before && i + 1 < e && text[i + 1] >= '0' && text[i + 1] <= '9') {
            digit_before = false;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        if (v > (LLONG_MAX - (c - '0')) / 10) return std::nullopt;
        v = v * 10 + (c - '0');
        digit_before = true;
    }
    return neg ? -v : v;
}

// The brightness below which some main channel of the LED's color is
// already 0; 0 when its color can't be read.
long long lowest(const std::string& d) {
    auto mi = pq::read_file(d + "/multi_intensity");
    if (!mi) return 0;
    std::vector<long long> ch;
    for (const auto& t : py_split(*mi)) {
        auto c = py_int(t);
        if (!c) return 0;
        ch.push_back(*c);
    }
    auto mb = pq::read_file(d + "/max_brightness");
    if (!mb) return 0;
    auto top = py_int(*mb);
    if (!top) return 0;
    if (ch.empty()) return 0;
    long long strongest = *std::max_element(ch.begin(), ch.end());
    std::optional<long long> weakest;
    for (long long c : ch)
        if (c && static_cast<double>(c) >= static_cast<double>(strongest) / 4)
            weakest = weakest ? std::min(*weakest, c) : c;
    if (!weakest) return 0;
    return static_cast<long long>(std::ceil(static_cast<double>(*top) / static_cast<double>(*weakest)));
}

struct Led {
    std::string path;
    int fd;
    long long b;
    long long low;
    std::optional<long long> last;
};

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int led_fade_main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pbos-platformd led-fade out|in SAVE\n");
        return 2;
    }
    bool out = !std::strcmp(argv[1], "out");
    auto text = pq::read_file(argv[2]);
    if (!text) {
        pq::log("led-fade: cannot read %s", argv[2]);
        return 1;
    }
    // Lines as Python reads a text file: \n, \r\n or \r end one.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i < text->size(); ++i) {
            char c = (*text)[i];
            cur += c;
            if (c == '\n' || c == '\r') {
                if (c == '\r' && i + 1 < text->size() && (*text)[i + 1] == '\n') cur += (*text)[++i];
                lines.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    std::vector<Led> leds;
    auto give_up = [&](const char* fmt, const std::string& what) {
        pq::log(fmt, what.c_str());
        for (auto& l : leds) close(l.fd);
        return 1;
    };
    for (const auto& line : lines) {
        auto f = py_split(line);
        if (f.empty()) continue;
        if (f.size() != 2) return give_up("led-fade: bad line in the save file: %s", pq::trim(line));
        // Opened as Python's open(..., 'w') did.
        int fd = open((f[0] + "/brightness").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) return give_up("led-fade: cannot open %s/brightness", f[0]);
        leds.push_back({f[0], fd, 0, 0, std::nullopt});
        auto b = py_int(f[1]);
        if (!b) return give_up("led-fade: bad brightness for %s", f[0]);
        leds.back().b = *b;
        leds.back().low = lowest(f[0]);
    }
    int rc = 0;
    double start = now();
    for (;;) {
        double x = std::min((now() - start) / T, 1.0);
        double k = out ? std::pow(1 - x, GAMMA) : std::pow(x, GAMMA);
        for (auto& l : leds) {
            // Python's round(): halves to even, as nearbyint does.
            long long v = static_cast<long long>(std::nearbyint(static_cast<double>(l.b) * k));
            if (v < l.low) v = 0;
            if (l.last != v) {
                std::string s = std::to_string(v);
                if (write(l.fd, s.data(), s.size()) != static_cast<ssize_t>(s.size())) {
                    // Python stopped here, the fade half done.
                    pq::log("led-fade: cannot write %s/brightness", l.path.c_str());
                    rc = 1;
                    goto done;
                }
                l.last = v;
            }
        }
        if (x >= 1) break;
        timespec ts{0, static_cast<long>(STEP * 1e9)};
        nanosleep(&ts, nullptr);
    }
done:
    for (auto& l : leds) close(l.fd);
    return rc;
}

}  // namespace pp
