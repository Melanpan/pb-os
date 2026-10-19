// barryglide: glide typing, suggestions and autocorrect for Barry
// Launcher's keyboards, as barry_glide.py does them (the same candidates in
// the same order), in plain C++20.
//
// A pack's words live in a read-only blob (blob.h): built once from its
// words.txt, kept in $XDG_CACHE_HOME and mapped, so its pages are clean
// memory the kernel can drop and read back instead of swapping.
#pragma once

#include "blob.h"
#include "json.h"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace barryglide {

struct Point {
    double x = 0, y = 0;
};

// math.dist / math.hypot as CPython computes them (its vector_norm).
double pyHypot(double dx, double dy);
inline double pyDist(Point a, Point b) { return pyHypot(a.x - b.x, a.y - b.y); }

// A keyboard.json, read as barry_launcher_shelld reads it.
struct Pack {
    std::vector<std::vector<std::string>> rows;  // its rows' string keys
    std::optional<json::Value> grid;             // a grid pack's grid (a nonempty list)
    std::vector<std::pair<std::string, std::vector<std::string>>> more;  // held keys' letters
    std::string words;      // "words", as written (the caller checks the path)
    std::string wordsFrom;  // "wordsFrom"

    static std::optional<Pack> fromJson(const json::Value &v);  // nullopt: not an object

    // Glide typing's fold: a held key's letter ("é") and the key it is on
    // ("e"), in order (shelld's dictionary()).
    std::vector<std::pair<std::u32string, std::u32string>> glideFold() const;
};

// What a Corrector takes from its pack: key positions for typo costs, the
// letters a correctable word is made of, and the accent fold.
struct Layout {
    std::unordered_map<char32_t, Point> positions;
    bool english = true;
    std::unordered_set<char32_t> alphabet;    // not English
    std::unordered_map<char32_t, std::u32string> fold;  // "é" -> "e"

    static Layout fromPack(const Pack &pack, bool english);
    Blob::Fold blobFold() const;  // what the blob's delete index needs
};

class Decoder {
public:
    struct Key {
        std::string name;  // the key's text (a letter)
        Point at;          // its center
    };
    explicit Decoder(std::shared_ptr<const Blob> blob, size_t cacheEntries = 512);

    // Decoder.decode: the likeliest words for a glide, best first.
    std::vector<std::string> decode(const std::vector<Point> &path, const std::vector<Key> &keys, double keySize,
                                    int limit = 4,
                                    const std::vector<std::pair<std::u32string, std::u32string>> &fold = {});

    size_t cacheSize() const { return m_lru.size(); }

private:
    struct Ideal {
        double length;
        Point resampled[32];
        Point normalized[32];
    };
    const Ideal &ideal(std::string_view letters, const std::vector<Point> &pts);

    std::shared_ptr<const Blob> m_blob;
    size_t m_capacity;
    // Ideal paths of the current layout, least recently used last.
    std::vector<std::pair<std::u32string, Point>> m_layout;
    std::list<std::pair<std::string, Ideal>> m_lru;
    std::unordered_map<std::string_view, decltype(m_lru)::iterator> m_lruIndex;
};

class Corrector {
public:
    Corrector(std::shared_ptr<const Blob> blob, Layout layout, std::string personalPath = {});

    std::optional<std::string> correct(const std::string &word) const;
    std::vector<std::string> near(const std::string &word, int limit = 3) const;
    std::vector<std::string> suggest(const std::string &word, int limit = 3) const;
    std::vector<std::string> complete(const std::string &prefix, int limit = 3) const;
    bool known(const std::string &word) const;
    void keep(const std::string &word);    // never correct it again (and save it)
    void addPersonal(const std::string &word);  // another Corrector kept it
    const std::string &personalPath() const { return m_personalPath; }

private:
    std::optional<uint32_t> find(std::u32string_view plain) const;
    bool knownLower(const std::u32string &w) const;
    std::u32string foldPlain(std::u32string_view w) const;
    std::vector<uint32_t> candidates(const std::u32string &plain) const;
    double typoCost(std::u32string_view typed, std::u32string_view word) const;
    double subCost(char32_t a, char32_t b) const;
    std::vector<std::pair<double, uint32_t>> scored(const std::u32string &plain, bool dropSelf) const;

    std::shared_ptr<const Blob> m_blob;
    Layout m_layout;
    std::string m_personalPath;
    std::unordered_set<std::string> m_personal;
};

// barry_glide's tuning, as there.
namespace tune {
constexpr int N = 32;
constexpr double NEAR = 1.1, PASS = 1.0, LOC_SIGMA = 0.45, SHAPE_SIGMA = 0.22;
constexpr double NEIGHBOUR = 1.5;
constexpr double COST_NEAR = 0.45, COST_FAR = 1.0, COST_SWAP = 0.6, COST_EXTRA = 0.9, COST_DOUBLE = 0.4,
                 COST_MISSING = 0.9, COST_ACCENT = 0.15;
constexpr double MAX_COST = 0.9, COST_WEIGHT = 4.0, MARGIN = 2.5;
constexpr int CORRECT_TO_LEVEL = 20;
double levelPrior(int32_t level);
}  // namespace tune

}  // namespace barryglide
