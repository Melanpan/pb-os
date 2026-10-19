#include "glide.h"

#include "text.h"
#include "unicode.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <sstream>

namespace barryglide {

// --- arithmetic as CPython does it --------------------------------------------------

namespace {

struct DoubleLength {
    double hi, lo;
};
DoubleLength dlFastSum(double a, double b)
{
    const double x = a + b;
    const double z = x - a;
    return {x, b - z};
}
DoubleLength dlMul(double x, double y)
{
    const double z = x * y;
    return {z, std::fma(x, y, -z)};
}

// CPython's vector_norm for two values (math.dist, math.hypot).
double vectorNorm(double a, double b, double max)
{
    if (std::isinf(max))
        return max;
    if (std::isnan(a) || std::isnan(b))
        return NAN;
    if (max == 0.0)
        return max;
    int maxE;
    std::frexp(max, &maxE);
    if (maxE < -1023)
        return DBL_MIN * vectorNorm(a / DBL_MIN, b / DBL_MIN, max / DBL_MIN);
    const double scale = std::ldexp(1.0, -maxE);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    for (double x : {a, b}) {
        x *= scale;
        const DoubleLength pr = dlMul(x, x);
        const DoubleLength sm = dlFastSum(csum, pr.hi);
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    const DoubleLength pr = dlMul(-h, h);
    const DoubleLength sm = dlFastSum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    const double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);
    return h / scale;
}

// sum() of floats: Neumaier's compensated sum, as CPython 3.12 does.
struct Sum {
    double f = 0.0, c = 0.0;
    void add(double x)
    {
        const double t = f + x;
        if (std::fabs(f) >= std::fabs(x))
            c += (f - t) + x;
        else
            c += (x - t) + f;
        f = t;
    }
    double value() const { return c != 0.0 && std::isfinite(c) ? f + c : f; }
};

// x ** 2 goes through libm's pow, which the compiler would fold to x * x.
double square(double x)
{
    static volatile double two = 2.0;
    return std::pow(x, two);
}

}  // namespace

double pyHypot(double dx, double dy)
{
    dx = std::fabs(dx);
    dy = std::fabs(dy);
    double max = 0.0;
    if (dx > max)
        max = dx;
    if (dy > max)
        max = dy;
    return vectorNorm(dx, dy, max);
}

double tune::levelPrior(int32_t level)
{
    switch (level) {
    case 10: return 0.0;
    case 20: return -0.75;
    case 35: return -1.65;
    case 40: return -2.25;
    default: return -3.0;
    }
}

// --- the pack ---------------------------------------------------------------------

std::optional<Pack> Pack::fromJson(const json::Value &v)
{
    if (v.type != json::Value::Object)
        return std::nullopt;
    Pack p;
    if (const auto *rows = v.get("rows"); rows && rows->type == json::Value::Array) {
        for (const auto &r : rows->arr) {
            if (r.type != json::Value::Array)
                continue;
            std::vector<std::string> row;
            for (const auto &k : r.arr)
                if (k.type == json::Value::String)
                    row.push_back(k.str);
            p.rows.push_back(std::move(row));
        }
    }
    if (const auto *grid = v.get("grid"); grid && grid->type == json::Value::Array && !grid->arr.empty())
        p.grid = *grid;
    if (const auto *more = v.get("more"); more && more->type == json::Value::Object) {
        for (const auto &[k, alts] : more->obj) {
            if (alts.type != json::Value::Array)
                continue;
            std::vector<std::string> list;
            for (const auto &a : alts.arr)
                if (a.type == json::Value::String)
                    list.push_back(a.str);
            p.more.emplace_back(k, std::move(list));
        }
    }
    if (const auto *w = v.get("words"); w && w->type == json::Value::String)
        p.words = w->str;
    if (const auto *w = v.get("wordsFrom"); w && w->type == json::Value::String)
        p.wordsFrom = w->str;
    return p;
}

std::vector<std::pair<std::u32string, std::u32string>> Pack::glideFold() const
{
    // {a.lower(): base.lower() for base, alts in more if len(base) == 1 for a in alts if len(a) == 1}
    std::vector<std::pair<std::u32string, std::u32string>> out;
    for (const auto &[base, alts] : more) {
        const std::u32string b = uni::decode(base);
        if (b.size() != 1)
            continue;
        for (const auto &alt : alts) {
            const std::u32string a = uni::decode(alt);
            if (a.size() != 1)
                continue;
            std::u32string from = uni::lower(a), to = uni::lower(b);
            auto it = std::find_if(out.begin(), out.end(), [&](const auto &kv) { return kv.first == from; });
            if (it != out.end())
                it->second = to;
            else
                out.emplace_back(std::move(from), std::move(to));
        }
    }
    return out;
}

Layout Layout::fromPack(const Pack &pack, bool english)
{
    Layout l;
    l.english = english;
    // positions or (key_positions(rows) if rows else QWERTY), positions
    // being a grid pack's grid_positions.
    bool gridAny = false;
    if (pack.grid) {
        static const std::pair<const char *, std::pair<int, int>> DIRECTIONS[] = {
            {"top", {0, -1}},   {"topRight", {1, -1}},   {"right", {1, 0}}, {"bottomRight", {1, 1}},
            {"bottom", {0, 1}}, {"bottomLeft", {-1, 1}}, {"left", {-1, 0}}, {"topLeft", {-1, -1}}};
        std::unordered_set<std::u32string> seen;
        for (size_t y = 0; y < pack.grid->arr.size(); ++y) {
            const auto &row = pack.grid->arr[y];
            if (row.type != json::Value::Array)
                continue;
            for (size_t x = 0; x < row.arr.size(); ++x) {
                const auto &key = row.arr[x];
                if (key.type != json::Value::Object)
                    continue;
                const double cx = double(x) * 4.0, cy = double(y) * 4.0;
                for (const auto &[name, t] : key.obj) {
                    if (t.type != json::Value::String)
                        continue;
                    const std::u32string text = uni::decode(t.str);
                    if (text.size() != 1)
                        continue;
                    Point at;
                    if (name == "center") {
                        at = {cx, cy};
                    } else {
                        auto d = std::find_if(std::begin(DIRECTIONS), std::end(DIRECTIONS),
                                              [&](const auto &e) { return name == e.first; });
                        if (d == std::end(DIRECTIONS))
                            continue;
                        const double dx = d->second.first, dy = d->second.second;
                        const double n = pyHypot(dx, dy);
                        at = {cx + dx / n, cy + dy / n};
                    }
                    const std::u32string lowered = uni::lower(text);
                    if (!seen.insert(lowered).second)
                        continue;  // setdefault
                    gridAny = true;
                    if (lowered.size() == 1)
                        l.positions.emplace(lowered[0], at);
                }
            }
        }
    }
    if (!gridAny) {
        if (!pack.rows.empty()) {
            size_t widest = 0;
            for (const auto &r : pack.rows)
                widest = std::max(widest, r.size());
            for (size_t y = 0; y < pack.rows.size(); ++y) {
                const auto &row = pack.rows[y];
                for (size_t x = 0; x < row.size(); ++x) {
                    const std::u32string key = uni::decode(row[x]);
                    if (key.size() != 1)
                        continue;
                    l.positions[key[0]] = {double(x) + double(widest - row.size()) / 2.0, double(y)};
                }
            }
        } else {
            static const std::pair<const char *, double> QWERTY[] = {{"qwertyuiop", 0.0}, {"asdfghjkl", 0.5},
                                                                    {"zxcvbnm", 1.5}};
            for (size_t y = 0; y < 3; ++y)
                for (size_t x = 0; QWERTY[y].first[x]; ++x)
                    l.positions[char32_t(QWERTY[y].first[x])] = {double(x) + QWERTY[y].second, double(y)};
        }
    }
    if (!english) {
        for (const auto &[c, at] : l.positions)
            if (uni::isAlpha(c))
                l.alphabet.insert(c);
        for (const auto &[base, alts] : pack.more)
            for (const auto &alt : alts)
                for (char32_t c : uni::decode(alt))
                    if (uni::isAlpha(c))
                        if (const auto lc = uni::lower(std::u32string(1, c)); lc.size() == 1)
                            l.alphabet.insert(lc[0]);
        for (const auto &[base, alts] : pack.more) {
            const std::u32string b = uni::decode(base);
            if (b.size() != 1 || !uni::isAlpha(b[0]))
                continue;
            for (const auto &alt : alts) {
                const std::u32string a = uni::decode(alt);
                if (a.size() != 1 || !uni::isAlpha(a[0]) || a == b)
                    continue;
                const std::u32string from = uni::lower(a);
                if (from.size() == 1)
                    l.fold.emplace(from[0], uni::lower(b));  // setdefault
            }
        }
    }
    return l;
}

Blob::Fold Layout::blobFold() const { return Blob::Fold(fold.begin(), fold.end()); }

// --- glide typing -------------------------------------------------------------------

namespace {

double pathLength(const Point *p, size_t n)
{
    Sum s;
    for (size_t i = 1; i < n; ++i)
        s.add(pyDist(p[i - 1], p[i]));
    return s.value();
}

void resample(const Point *pts, size_t count, Point *out, int n)
{
    if (count == 1) {
        std::fill(out, out + n, pts[0]);
        return;
    }
    const double total = pathLength(pts, count);
    if (total == 0) {
        std::fill(out, out + n, pts[0]);
        return;
    }
    const double step = total / double(n - 1);
    int k = 0;
    out[k++] = pts[0];
    size_t i = 1;
    double carry = 0.0;
    Point a = pts[0];
    while (k < n - 1 && i < count) {
        const Point b = pts[i];
        const double seg = pyDist(a, b);
        if (carry + seg >= step && seg > 0) {
            const double t = (step - carry) / seg;
            a = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            out[k++] = a;
            carry = 0.0;
        } else {
            carry += seg;
            a = b;
            ++i;
        }
    }
    while (k < n)
        out[k++] = pts[count - 1];
}

void normalized(const Point *pts, int n, Point *out)
{
    Sum sx, sy;
    double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (int i = 0; i < n; ++i) {
        sx.add(pts[i].x);
        sy.add(pts[i].y);
        minX = std::min(minX, pts[i].x), maxX = std::max(maxX, pts[i].x);
        minY = std::min(minY, pts[i].y), maxY = std::max(maxY, pts[i].y);
    }
    const double cx = sx.value() / n, cy = sy.value() / n;
    const double w = maxX - minX, h = maxY - minY;
    double size = std::max(w, h);
    if (size == 0)
        size = 1.0;
    for (int i = 0; i < n; ++i)
        out[i] = {(pts[i].x - cx) / size, (pts[i].y - cy) / size};
}

double meanDistance(const Point *a, const Point *b, int n)
{
    Sum s;
    for (int i = 0; i < n; ++i)
        s.add(pyDist(a[i], b[i]));
    return s.value() / n;
}

std::string display(std::string_view word)
{
    // "i'm" typed as "I'm".
    if (word == "i" || word.substr(0, 2) == "i'")
        return "I" + std::string(word.substr(1));
    return std::string(word);
}

}  // namespace

Decoder::Decoder(std::shared_ptr<const Blob> blob, size_t cacheEntries)
    : m_blob(std::move(blob)), m_capacity(std::max<size_t>(cacheEntries, 1))
{
}

const Decoder::Ideal &Decoder::ideal(std::string_view letters, const std::vector<Point> &pts)
{
    if (auto it = m_lruIndex.find(letters); it != m_lruIndex.end()) {
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        return it->second->second;
    }
    if (m_lru.size() >= m_capacity) {
        m_lruIndex.erase(m_lru.back().first);
        m_lru.pop_back();
    }
    m_lru.emplace_front(std::string(letters), Ideal{});
    Ideal &d = m_lru.front().second;
    d.length = pathLength(pts.data(), pts.size());
    resample(pts.data(), pts.size(), d.resampled, tune::N);
    normalized(d.resampled, tune::N, d.normalized);
    m_lruIndex.emplace(m_lru.front().first, m_lru.begin());
    return d;
}

std::vector<std::string> Decoder::decode(const std::vector<Point> &path, const std::vector<Key> &keysIn,
                                         double keySize, int limit,
                                         const std::vector<std::pair<std::u32string, std::u32string>> &fold)
{
    using namespace tune;
    if (path.size() < 2 || pathLength(path.data(), path.size()) < 0.8 * keySize)
        return {};
    // The letter keys, in order; a held key's letters on their key.
    std::vector<std::pair<std::u32string, Point>> keys;
    auto findKey = [&](const std::u32string &k) {
        return std::find_if(keys.begin(), keys.end(), [&](const auto &kv) { return kv.first == k; });
    };
    for (const auto &k : keysIn) {
        std::u32string name = uni::decode(k.name);
        if (name.size() != 1 || !uni::isAlpha(name[0]))
            continue;
        if (auto it = findKey(name); it != keys.end())
            it->second = k.at;
        else
            keys.emplace_back(std::move(name), k.at);
    }
    for (const auto &[alt, base] : fold) {
        auto b = findKey(base);
        if (b != keys.end() && findKey(alt) == keys.end()) {
            const Point at = b->second;
            keys.emplace_back(alt, at);
        }
    }
    // Ideal paths are the layout's: another layout, another cache.
    auto sameLayout = [&] {
        if (keys.size() != m_layout.size())
            return false;
        auto sorted = keys;
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (size_t i = 0; i < sorted.size(); ++i)
            if (sorted[i].first != m_layout[i].first || sorted[i].second.x != m_layout[i].second.x
                || sorted[i].second.y != m_layout[i].second.y)
                return false;
        return true;
    };
    if (!sameLayout()) {
        m_layout = keys;
        std::sort(m_layout.begin(), m_layout.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        m_lru.clear();
        m_lruIndex.clear();
    }
    if (keys.empty())
        return {};

    // Single letters' keys: code point -> index (most are ASCII).
    std::vector<Point> at;
    std::unordered_map<char32_t, int> single;
    for (size_t i = 0; i < keys.size(); ++i) {
        at.push_back(keys[i].second);
        if (keys[i].first.size() == 1)
            single.emplace(keys[i].first[0], int(i));
    }
    std::vector<int> keyOf(128, -1);  // ASCII fast path
    auto keyIndex = [&](char32_t c) -> int {
        if (c < 128)
            return keyOf[c];
        auto it = single.find(c);
        return it == single.end() ? -1 : it->second;
    };
    for (const auto &[c, i] : single)
        if (c < 128)
            keyOf[c] = i;

    auto near = [&](Point pt) {
        std::vector<size_t> close;
        for (size_t i = 0; i < keys.size(); ++i)
            if (pyDist(pt, keys[i].second) <= NEAR * keySize)
                close.push_back(i);
        if (close.empty()) {
            size_t best = 0;
            double bestD = pyDist(pt, keys[0].second);
            for (size_t i = 1; i < keys.size(); ++i) {
                const double d = pyDist(pt, keys[i].second);
                if (d < bestD)
                    best = i, bestD = d;
            }
            close.push_back(best);
        }
        return close;
    };
    const auto starts = near(path.front()), ends = near(path.back());
    Point rp[N], np[N];
    resample(path.data(), path.size(), rp, N);
    normalized(rp, N, np);
    const double lp = pathLength(path.data(), path.size());
    Point dense[64];
    resample(path.data(), path.size(), dense, 64);
    std::vector<double> gap(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        double m = pyDist(keys[i].second, dense[0]);
        for (int j = 1; j < 64; ++j)
            m = std::min(m, pyDist(keys[i].second, dense[j]));
        gap[i] = m / keySize;
    }

    std::vector<Point> pts;
    std::vector<std::pair<double, std::string_view>> scored;
    std::u32string letters;
    for (size_t s : starts) {
        if (keys[s].first.size() != 1)
            continue;
        for (size_t e : ends) {
            if (keys[e].first.size() != 1)
                continue;
            auto [lo, hi] = m_blob->ends(keys[s].first[0], keys[e].first[0]);
            for (const Blob::End *it = lo; it != hi; ++it) {
                if (it->word >= m_blob->wordCount())
                    continue;
                const Blob::Word &w = m_blob->words()[it->word];
                const std::string_view lettersUtf8 = m_blob->str(w.letters);
                if (!uni::decode(lettersUtf8, letters) || letters.empty())
                    continue;
                // Every letter must lie near the glide.
                bool far = false;
                pts.clear();
                for (char32_t c : letters) {
                    const int k = keyIndex(c);
                    if ((k < 0 ? PASS + 1 : gap[size_t(k)]) > PASS) {
                        far = true;
                        break;
                    }
                    pts.push_back(at[size_t(k)]);
                }
                if (far)
                    continue;
                const Ideal &id = ideal(lettersUtf8, pts);
                if (id.length > 0 && !(0.5 <= lp / id.length && lp / id.length <= 2.5))
                    continue;
                const double loc = meanDistance(rp, id.resampled, N) / keySize;
                const double shape = meanDistance(np, id.normalized, N);
                const double score = -square(loc / LOC_SIGMA) - square(shape / SHAPE_SIGMA) + levelPrior(w.level);
                scored.emplace_back(score, m_blob->str(w.word));
            }
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a > b; });
    std::vector<std::string> out;
    for (const auto &[score, word] : scored) {
        std::string w = m_blob->english() ? display(word) : std::string(word);
        if (std::find(out.begin(), out.end(), w) == out.end())
            out.push_back(std::move(w));
        if (int(out.size()) == limit)
            break;
    }
    return out;
}

// --- autocorrect ----------------------------------------------------------------------

namespace {

struct Fixed {
    const char *typo, *word;
};
// Classic slips a tie between two words would otherwise leave alone (sorted).
constexpr Fixed FIXED[] = {
    {"adn", "and"},         {"alot", "a lot"},        {"beacuse", "because"},   {"becuase", "because"},
    {"definately", "definitely"}, {"freind", "friend"}, {"freinds", "friends"}, {"hte", "the"},
    {"im", "I'm"},          {"nad", "and"},           {"occured", "occurred"},  {"recieve", "receive"},
    {"recieved", "received"}, {"seperate", "separate"}, {"taht", "that"},       {"teh", "the"},
    {"thier", "their"},     {"thta", "that"},         {"untill", "until"},      {"waht", "what"},
    {"wierd", "weird"},     {"wiht", "with"},
};

const char *fixedFor(std::string_view plain)
{
    for (const auto &f : FIXED)
        if (plain == f.typo)
            return f.word;
    return nullptr;
}

// Endings that make a known word into another real one, and what the stem may have lost.
struct Inflection {
    const char32_t *end;
    std::vector<const char32_t *> back;
};
const std::vector<Inflection> &inflections()
{
    static const std::vector<Inflection> v = {
        {U"s", {U""}},          {U"es", {U"", U"e"}},  {U"ies", {U"y"}},       {U"ied", {U"y"}},
        {U"ed", {U"", U"e"}},   {U"ing", {U"", U"e"}}, {U"er", {U"", U"e"}},   {U"ers", {U"", U"e"}},
        {U"ly", {U""}},         {U"ness", {U""}},      {U"able", {U"", U"e"}},
    };
    return v;
}

std::u32string capitalized(std::u32string_view t)
{
    if (t.empty())
        return {};
    return uni::upper(t[0]) + std::u32string(t.substr(1));
}

std::string readFile(const std::string &path, bool &ok)
{
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    if (!ok)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

Corrector::Corrector(std::shared_ptr<const Blob> blob, Layout layout, std::string personalPath)
    : m_blob(std::move(blob)), m_layout(std::move(layout)), m_personalPath(std::move(personalPath))
{
    if (m_personalPath.empty())
        return;
    bool ok;
    const std::string bytes = readFile(m_personalPath, ok);
    std::u32string file;
    if (!ok || !uni::decode(bytes, file))
        return;
    for (auto line : text::lines(file)) {
        const auto w = text::strip(line);
        if (!w.empty())
            m_personal.insert(uni::encode(uni::lower(w)));
    }
}

std::optional<uint32_t> Corrector::find(std::u32string_view plain) const
{
    const Blob::Lex *l = m_blob->findLex(uni::encode(plain));
    if (!l)
        return std::nullopt;
    return uint32_t(l - m_blob->lex());
}

std::u32string Corrector::foldPlain(std::u32string_view w) const
{
    std::u32string out;
    for (char32_t c : w) {
        auto it = m_layout.fold.find(c);
        if (it != m_layout.fold.end())
            out += it->second;
        else
            out.push_back(c);
    }
    return out;
}

bool Corrector::knownLower(const std::u32string &w) const
{
    if (find(w) || find(text::without(w, U'\'')) || m_personal.count(uni::encode(w)))
        return true;
    if (!m_blob->english())
        return false;
    for (const auto &inf : inflections()) {
        const std::u32string_view end(inf.end);
        if (w.size() >= end.size() && std::u32string_view(w).substr(w.size() - end.size()) == end
            && w.size() - end.size() >= 3) {
            const std::u32string stem = w.substr(0, w.size() - end.size());
            for (const char32_t *b : inf.back)
                if (find(stem + b))
                    return true;
        }
    }
    return false;
}

bool Corrector::known(const std::string &word) const
{
    std::u32string w;
    if (!uni::decode(word, w))
        return false;
    return knownLower(uni::lower(w));
}

void Corrector::addPersonal(const std::string &word)
{
    std::u32string w;
    if (uni::decode(word, w))
        m_personal.insert(uni::encode(uni::lower(w)));
}

void Corrector::keep(const std::string &word)
{
    std::u32string w;
    if (!uni::decode(word, w))
        return;
    const std::string plain = uni::encode(uni::lower(w));
    if (m_personal.count(plain) || m_personalPath.empty())
        return;
    m_personal.insert(plain);
    std::ofstream out(m_personalPath, std::ios::app | std::ios::binary);
    if (out)
        out << plain << '\n';
}

double Corrector::subCost(char32_t a, char32_t b) const
{
    using namespace tune;
    if (!m_layout.fold.empty()) {
        auto fa = m_layout.fold.find(a), fb = m_layout.fold.find(b);
        const std::u32string ka = fa != m_layout.fold.end() ? fa->second : std::u32string(1, a);
        const std::u32string kb = fb != m_layout.fold.end() ? fb->second : std::u32string(1, b);
        if (ka == kb)
            return COST_ACCENT;
    }
    auto pa = m_layout.positions.find(a), pb = m_layout.positions.find(b);
    if (pa != m_layout.positions.end() && pb != m_layout.positions.end()
        && pyDist(pa->second, pb->second) <= NEIGHBOUR)
        return COST_NEAR;
    return COST_FAR;
}

double Corrector::typoCost(std::u32string_view typed, std::u32string_view word) const
{
    using namespace tune;
    const size_t n = typed.size(), m = word.size();
    std::vector<double> d((n + 1) * (m + 1), 0.0);
    auto at = [&](size_t i, size_t j) -> double & { return d[i * (m + 1) + j]; };
    for (size_t i = 1; i <= n; ++i)
        at(i, 0) = double(i) * COST_EXTRA;
    for (size_t j = 1; j <= m; ++j)
        at(0, j) = double(j) * COST_MISSING;
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            const char32_t a = typed[i - 1], b = word[j - 1];
            double best = at(i - 1, j - 1) + (a == b ? 0.0 : subCost(a, b));
            // An extra letter: a doubled one, or a slip that hit two keys.
            const double extra = (i > 1 && typed[i - 2] == a) || word[j - 1] == a ? COST_DOUBLE : COST_EXTRA;
            best = std::min(best, at(i - 1, j) + extra);
            const double missing = j > 1 && word[j - 2] == b ? COST_DOUBLE : COST_MISSING;
            best = std::min(best, at(i, j - 1) + missing);
            if (i > 1 && j > 1 && a == word[j - 2] && typed[i - 2] == b)
                best = std::min(best, at(i - 2, j - 2) + COST_SWAP);
            at(i, j) = best;
        }
    }
    return at(n, m);
}

std::vector<uint32_t> Corrector::candidates(const std::u32string &plain) const
{
    std::vector<std::u32string> keys;
    for (size_t i = 0; i < plain.size(); ++i)
        keys.push_back(plain.substr(0, i) + plain.substr(i + 1));
    keys.push_back(plain);
    keys.push_back(foldPlain(plain));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<uint32_t> out;
    std::u32string cand;
    for (const auto &key : keys) {
        auto [lo, hi] = m_blob->index(uni::encode(key));
        for (const uint32_t *id = lo; id != hi; ++id) {
            // The hash matched; is the key really one of the word's?
            if (*id >= m_blob->lexCount() || !uni::decode(m_blob->str(m_blob->lex()[*id].plain), cand))
                continue;
            bool has = key == cand || key == foldPlain(cand);
            if (!has && key.size() + 1 == cand.size()) {
                size_t i = 0;
                while (i < key.size() && key[i] == cand[i])
                    ++i;
                has = std::u32string_view(key).substr(i) == std::u32string_view(cand).substr(i + 1);
            }
            if (has)
                out.push_back(*id);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::pair<double, uint32_t>> Corrector::scored(const std::u32string &plain, bool dropSelf) const
{
    using namespace tune;
    std::vector<std::pair<double, uint32_t>> out;
    std::u32string cand;
    for (uint32_t id : candidates(plain)) {
        const Blob::Lex &l = m_blob->lex()[id];
        uni::decode(m_blob->str(l.plain), cand);
        if (dropSelf && cand == plain)
            continue;
        const double cost = typoCost(plain, cand);
        if (cost <= MAX_COST)
            out.emplace_back(-COST_WEIGHT * cost + levelPrior(l.level), id);
    }
    // Best first; a tie goes to the later word, as Python's reverse sort of
    // (score, word) does. Lex ids are in word order.
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a > b; });
    return out;
}

std::optional<std::string> Corrector::correct(const std::string &wordUtf8) const
{
    using namespace tune;
    std::u32string word;
    if (!uni::decode(wordUtf8, word) || !uni::isAlpha(word))
        return std::nullopt;
    for (size_t i = 1; i < word.size(); ++i)
        if (uni::isUpper(word[i]))
            return std::nullopt;
    const std::u32string plain = uni::lower(word);
    // Only words of the keyboard's own letters (English: plain ASCII).
    if (m_layout.english) {
        if (!uni::isAscii(word))
            return std::nullopt;
    } else {
        for (char32_t c : plain)
            if (!m_layout.alphabet.count(c))
                return std::nullopt;
    }
    const std::string plainUtf8 = uni::encode(plain);
    if (m_personal.count(plainUtf8))
        return std::nullopt;
    std::u32string fixed;
    if (const char *f = m_blob->english() ? fixedFor(plainUtf8) : nullptr)
        fixed = uni::decode(f);
    else if (const Blob::Contraction *c = m_blob->findContraction(plainUtf8))
        fixed = uni::decode(m_blob->str(c->typed));
    if (!fixed.empty())
        return uni::encode(uni::isUpper(word[0]) ? capitalized(fixed) : fixed);
    if (word.size() < 3 || knownLower(plain))
        return std::nullopt;
    const auto s = scored(plain, false);
    if (s.empty())
        return std::nullopt;
    if (s.size() > 1 && s[0].first - s[1].first < MARGIN)
        return std::nullopt;
    const Blob::Lex &best = m_blob->lex()[s[0].second];
    if (best.level > CORRECT_TO_LEVEL)
        return std::nullopt;
    // Keep a capital the user typed ("Teh" -> "The").
    const std::u32string typed = uni::decode(m_blob->str(best.typed));
    return uni::encode(uni::isUpper(word[0]) ? capitalized(typed) : typed);
}

std::vector<std::string> Corrector::near(const std::string &wordUtf8, int limit) const
{
    std::u32string word;
    if (!uni::decode(wordUtf8, word))
        return {};
    const std::u32string plain = uni::lower(word);
    if (plain.size() < 2 || !uni::isAlpha(plain))
        return {};
    const auto s = scored(plain, true);
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size() && int(i) < limit; ++i) {
        const std::u32string t = uni::decode(m_blob->str(m_blob->lex()[s[i].second].typed));
        out.push_back(uni::encode(uni::isUpper(word[0]) ? capitalized(t) : t));
    }
    return out;
}

std::vector<std::string> Corrector::complete(const std::string &prefixUtf8, int limit) const
{
    std::u32string prefix;
    if (!uni::decode(prefixUtf8, prefix))
        return {};
    const std::u32string p = uni::lower(prefix);
    if (p.size() < 2 || !uni::isAlpha(p))
        return {};
    const std::string lo = uni::encode(p), hi = lo + "\xEF\xBF\xBF";  // p + U+FFFF
    const Blob::Lex *b = m_blob->lex(), *e = b + m_blob->lexCount();
    auto cmp = [this](const Blob::Lex &x, const std::string &v) { return m_blob->str(x.plain) < v; };
    const Blob::Lex *from = std::lower_bound(b, e, lo, cmp), *to = std::lower_bound(b, e, hi, cmp);
    std::vector<const Blob::Lex *> found;
    for (const Blob::Lex *it = from; it < to; ++it)
        found.push_back(it);
    std::stable_sort(found.begin(), found.end(), [](const Blob::Lex *x, const Blob::Lex *y) {
        return std::pair(x->level, x->chars) < std::pair(y->level, y->chars);
    });
    std::vector<std::string> out;
    for (const Blob::Lex *l : found) {
        if (m_blob->str(l->plain) == lo || (l->flags & Blob::APOSTROPHE))
            continue;
        const std::u32string t = uni::decode(m_blob->str(l->typed));
        if (t.empty())
            continue;  // a damaged blob
        out.push_back(uni::encode(uni::isUpper(prefix[0]) ? prefix.substr(0, 1) + t.substr(1) : t));
        if (int(out.size()) == limit)
            break;
    }
    return out;
}

std::vector<std::string> Corrector::suggest(const std::string &word, int limit) const
{
    const auto nearWords = near(word, limit), more = complete(word, limit);
    const bool nearFirst = known(word) || more.empty();
    const auto &first = nearFirst ? nearWords : more;
    const auto &then = nearFirst ? more : nearWords;
    // Two of the first kind and one of the other, when there are both.
    std::vector<std::string> order;
    const size_t head = size_t(std::max(limit - 1, 0));
    for (size_t i = 0; i < first.size() && i < head; ++i)
        order.push_back(first[i]);
    if (!then.empty())
        order.push_back(then[0]);
    for (size_t i = head; i < first.size(); ++i)
        order.push_back(first[i]);
    for (size_t i = 1; i < then.size(); ++i)
        order.push_back(then[i]);
    std::u32string w32;
    uni::decode(word, w32);
    const std::u32string wl = uni::lower(w32);
    std::vector<std::string> out;
    for (const auto &w : order) {
        if (uni::lower(uni::decode(w)) != wl && std::find(out.begin(), out.end(), w) == out.end())
            out.push_back(w);
    }
    if (int(out.size()) > limit)
        out.resize(size_t(std::max(limit, 0)));
    return out;
}

}  // namespace barryglide
