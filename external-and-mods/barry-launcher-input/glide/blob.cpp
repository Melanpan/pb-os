#include "blob.h"

#include "text.h"
#include "unicode.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <malloc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace barryglide {
namespace {

constexpr char MAGIC[8] = {'B', 'G', 'L', 'I', 'D', 'E', '\0', '\1'};

bool fail(std::string *error, std::string what)
{
    if (error)
        *error = std::move(what);
    return false;
}

uint64_t mix(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

struct Writer {
    std::string strings;
    std::unordered_map<std::string, Blob::Str> seen;
    Blob::Str add(const std::string &s)
    {
        auto it = seen.find(s);
        if (it != seen.end())
            return it->second;
        Blob::Str r{uint32_t(strings.size()), uint32_t(s.size())};
        strings += s;
        seen.emplace(s, r);
        return r;
    }
};

template <typename T>
void put(std::string &out, uint64_t &off, const std::vector<T> &v)
{
    while (out.size() % 8)
        out += '\0';
    off = out.size();
    out.append(reinterpret_cast<const char *>(v.data()), v.size() * sizeof(T));
}

std::string foldString(const Blob::Fold &fold)
{
    std::map<char32_t, std::u32string> sorted(fold.begin(), fold.end());
    std::string out;
    for (const auto &[from, to] : sorted) {
        uni::append(out, from);
        out += '\x01';
        out += uni::encode(to);
        out += '\x02';
    }
    return out;
}

}  // namespace

uint64_t Blob::hash(std::string_view s, uint64_t seed)
{
    // FNV-1a, then a splitmix finish so nearby keys spread out.
    uint64_t h = 0xcbf29ce484222325ULL ^ mix(seed);
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return mix(h ^ s.size());
}

void Blob::key(std::string_view wordsFile, bool english, const Fold &fold, uint64_t out[2])
{
    std::string all(wordsFile);
    all += '\0';
    all += english ? 'E' : 'N';
    all += foldString(fold);
    all += "v" + std::to_string(VERSION);
    out[0] = hash(all, 1);
    out[1] = hash(all, 2);
}

bool Blob::build(std::string_view wordsFile, bool english, const Fold &fold, std::string &out, std::string *error)
{
    std::u32string file;
    if (!uni::decode(wordsFile, file))
        return fail(error, "not UTF-8");

    struct W {
        std::string word, letters;
        int32_t level;
    };
    std::vector<W> words;
    for (std::u32string_view line : text::lines(file)) {
        if ((!line.empty() && line[0] == U'#') || text::strip(line).empty())
            continue;
        const auto parts = text::split(line);
        if (parts.size() != 2)
            return fail(error, "a line without one word and its level: " + uni::encode(line));
        const auto level = text::toInt(parts[1]);
        if (!level)
            return fail(error, "not a level: " + uni::encode(parts[1]));
        // Matched on its lower-case letters, typed as written.
        const std::u32string plain = uni::lower(text::without(parts[0], U'\''));
        std::u32string letters;
        for (size_t i = 0; i < plain.size(); ++i)
            if (i == 0 || plain[i] != plain[i - 1])
                letters.push_back(plain[i]);
        if (letters.size() < 2)
            continue;
        words.push_back({uni::encode(parts[0]), uni::encode(letters), *level});
    }
    if (words.size() >= UINT32_MAX / 2)
        return fail(error, "too many words");

    // Corrector.level and .typed, in their insertion order.
    struct L {
        std::string plain, typed;
        int32_t level;
        uint32_t chars;
        bool apostrophe;
    };
    std::vector<L> lex;
    std::unordered_map<std::string, size_t> lexAt;
    auto setdefault = [&](const std::u32string &plain, const std::string &typed, int32_t level) {
        std::string p = uni::encode(plain);
        if (lexAt.count(p))
            return;
        lexAt.emplace(p, lex.size());
        const bool apostrophe = plain.find(U'\'') != std::u32string::npos;
        lex.push_back({std::move(p), typed, level, uint32_t(plain.size()), apostrophe});
    };
    for (const W &w : words)
        setdefault(uni::lower(uni::decode(w.word)), w.word, w.level);
    if (english) {
        setdefault(U"a", "a", 10);
        setdefault(U"i", "I", 10);
    }
    // Contractions typed without the apostrophe, unless that is a word.
    std::map<std::string, std::string> contractions;  // the last one wins, as there
    if (english) {
        for (const L &l : lex) {
            if (!l.apostrophe)
                continue;
            const std::string bare = uni::encode(text::without(uni::decode(l.plain), U'\''));
            if (!lexAt.count(bare))
                contractions[bare] = l.typed;
        }
    }

    // Sorted as Corrector.sorted (code point order = UTF-8 byte order).
    std::vector<uint32_t> order(lex.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return lex[a].plain < lex[b].plain; });

    // The delete index, over the sorted ids.
    std::vector<std::pair<uint64_t, uint32_t>> index;
    for (uint32_t id = 0; id < order.size(); ++id) {
        const L &l = lex[order[id]];
        if (l.apostrophe)
            continue;
        const std::u32string plain = uni::decode(l.plain);
        std::set<std::u32string> keys;
        for (size_t i = 0; i < plain.size(); ++i)
            keys.insert(plain.substr(0, i) + plain.substr(i + 1));
        keys.insert(plain);
        std::u32string folded;
        for (char32_t c : plain) {
            auto it = fold.find(c);
            if (it != fold.end())
                folded += it->second;
            else
                folded.push_back(c);
        }
        keys.insert(folded);
        for (const auto &k : keys)
            index.emplace_back(hash(uni::encode(k)), id);
    }
    std::sort(index.begin(), index.end());

    Writer sw;
    std::vector<Word> wordRecs;
    wordRecs.reserve(words.size());
    std::vector<End> ends;
    for (uint32_t i = 0; i < words.size(); ++i) {
        wordRecs.push_back({sw.add(words[i].word), sw.add(words[i].letters), words[i].level});
        const std::u32string letters = uni::decode(words[i].letters);
        ends.push_back({uint32_t(letters.front()), uint32_t(letters.back()), i});
    }
    std::sort(ends.begin(), ends.end(), [](const End &a, const End &b) {
        return std::tie(a.first, a.last, a.word) < std::tie(b.first, b.last, b.word);
    });
    std::vector<Lex> lexRecs;
    for (uint32_t i : order) {
        const L &l = lex[i];
        lexRecs.push_back({sw.add(l.plain), sw.add(l.typed), l.level, l.chars, l.apostrophe ? APOSTROPHE : 0u});
    }
    std::vector<Contraction> contrRecs;
    for (const auto &[bare, typed] : contractions)
        contrRecs.push_back({sw.add(bare), sw.add(typed)});
    std::vector<uint64_t> hashes;
    std::vector<uint32_t> ids;
    hashes.reserve(index.size());
    ids.reserve(index.size());
    for (const auto &[h, id] : index) {
        hashes.push_back(h);
        ids.push_back(id);
    }

    Header h{};
    std::memcpy(h.magic, MAGIC, 8);
    h.version = VERSION;
    h.english = english;
    key(wordsFile, english, fold, h.key);
    h.nWords = uint32_t(wordRecs.size());
    h.nEnds = uint32_t(ends.size());
    h.nLex = uint32_t(lexRecs.size());
    h.nContractions = uint32_t(contrRecs.size());
    h.nIndex = index.size();
    out.assign(sizeof(Header), '\0');
    h.strings = out.size();
    h.stringsLen = sw.strings.size();
    out += sw.strings;
    put(out, h.words, wordRecs);
    put(out, h.ends, ends);
    put(out, h.lex, lexRecs);
    put(out, h.contractions, contrRecs);
    put(out, h.hashes, hashes);
    put(out, h.ids, ids);
    while (out.size() % 8)
        out += '\0';
    h.size = out.size();
    std::memcpy(out.data(), &h, sizeof h);
    return true;
}

bool Blob::attach(const char *data, size_t size, std::string *error)
{
    if (size < sizeof(Header))
        return fail(error, "too small");
    if (reinterpret_cast<uintptr_t>(data) % 8)
        return fail(error, "misaligned");
    m_header = reinterpret_cast<const Header *>(data);
    const Header &h = *m_header;
    if (std::memcmp(h.magic, MAGIC, 8) != 0 || h.version != VERSION)
        return fail(error, "not a barryglide blob of this version");
    if (h.size != size)
        return fail(error, "truncated");
    auto section = [&](uint64_t off, uint64_t count, size_t each) {
        return off % 8 == 0 && off <= size && count <= (size - off) / each;
    };
    if (!(h.strings <= size && h.stringsLen <= size - h.strings && section(h.words, h.nWords, sizeof(Word))
          && section(h.ends, h.nEnds, sizeof(End)) && section(h.lex, h.nLex, sizeof(Lex))
          && section(h.contractions, h.nContractions, sizeof(Contraction))
          && section(h.hashes, h.nIndex, sizeof(uint64_t)) && section(h.ids, h.nIndex, sizeof(uint32_t))))
        return fail(error, "bad sections");
    m_strings = data + h.strings;
    m_words = reinterpret_cast<const Word *>(data + h.words);
    m_ends = reinterpret_cast<const End *>(data + h.ends);
    m_lex = reinterpret_cast<const Lex *>(data + h.lex);
    m_contractions = reinterpret_cast<const Contraction *>(data + h.contractions);
    m_hashes = reinterpret_cast<const uint64_t *>(data + h.hashes);
    m_ids = reinterpret_cast<const uint32_t *>(data + h.ids);
    // References inside are checked where they are read (str(), the
    // callers' id checks): mapping touches no page it does not need.
    m_size = size;
    return true;
}

std::shared_ptr<Blob> Blob::fromBytes(std::string bytes, std::string *error)
{
    std::shared_ptr<Blob> b(new Blob);
    b->m_owned = std::move(bytes);
    // std::string's buffer is new-allocated, so 16-byte aligned.
    if (!b->attach(b->m_owned.data(), b->m_owned.size(), error))
        return nullptr;
    return b;
}

std::shared_ptr<Blob> Blob::map(const std::string &path, std::string *error)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fail(error, path + ": " + std::strerror(errno));
        return nullptr;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < off_t(sizeof(Header))) {
        ::close(fd);
        fail(error, path + ": too small");
        return nullptr;
    }
    void *m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        fail(error, path + ": mmap: " + std::strerror(errno));
        return nullptr;
    }
    std::shared_ptr<Blob> b(new Blob);
    b->m_map = m;
    b->m_size = size_t(st.st_size);
    if (!b->attach(static_cast<const char *>(m), size_t(st.st_size), error))
        return nullptr;  // ~Blob unmaps
    // Lookups jump about: read ahead no more than a page asks for.
    madvise(m, size_t(st.st_size), MADV_RANDOM);
    return b;
}

Blob::~Blob()
{
    if (m_map)
        munmap(m_map, m_size);
}

std::string Blob::defaultCacheDir()
{
    std::string base;
    if (const char *x = std::getenv("XDG_CACHE_HOME"); x && *x == '/')
        base = x;
    else if (const char *home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.cache";
    else
        return {};
    return base + "/barry_launcher/glide";
}

std::shared_ptr<Blob> Blob::load(const std::string &wordsPath, bool english, const Fold &fold,
                                 const std::string &cacheDir, std::string *error)
{
    std::string words;
    {
        std::ifstream in(wordsPath, std::ios::binary);
        if (!in) {
            fail(error, wordsPath + ": cannot read");
            return nullptr;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        words = ss.str();
    }
    uint64_t k[2];
    key(words, english, fold, k);
    const std::string dir = cacheDir.empty() ? defaultCacheDir() : cacheDir;
    char name[64];
    std::snprintf(name, sizeof name, "%016llx%016llx.blob", static_cast<unsigned long long>(k[0]),
                  static_cast<unsigned long long>(k[1]));
    const std::string path = dir.empty() ? std::string() : dir + "/" + name;
    if (!path.empty()) {
        if (auto b = map(path); b && b->m_header->key[0] == k[0] && b->m_header->key[1] == k[1])
            return b;
    }
    std::string bytes;
    const bool built = build(words, english, fold, bytes, error);
    // The build's scratch (a few times the blob) goes back to the system.
    malloc_trim(0);
    if (!built)
        return nullptr;
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string tmp = path + ".tmp" + std::to_string(getpid());
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        bool written = false;
        if (fd >= 0) {
            size_t done = 0;
            while (done < bytes.size()) {
                const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
                if (n <= 0)
                    break;
                done += size_t(n);
            }
            written = ::close(fd) == 0 && done == bytes.size();
        }
        if (written && std::rename(tmp.c_str(), path.c_str()) == 0) {
            // Old packs' blobs: keep the newest few.
            std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> blobs;
            for (const auto &e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".blob")
                    blobs.emplace_back(e.last_write_time(ec), e.path());
            std::sort(blobs.rbegin(), blobs.rend());
            for (size_t i = 8; i < blobs.size(); ++i)
                std::filesystem::remove(blobs[i].second, ec);
            if (auto b = map(path))
                return b;
        } else {
            std::remove(tmp.c_str());
        }
    }
    return fromBytes(std::move(bytes), error);
}

std::pair<const Blob::End *, const Blob::End *> Blob::ends(char32_t first, char32_t last) const
{
    const End *b = m_ends, *e = m_ends + m_header->nEnds;
    auto lo = std::lower_bound(b, e, std::pair<uint32_t, uint32_t>(first, last),
                               [](const End &x, const std::pair<uint32_t, uint32_t> &v) {
                                   return std::tie(x.first, x.last) < std::tie(v.first, v.second);
                               });
    auto hi = lo;
    while (hi != e && hi->first == first && hi->last == last)
        ++hi;
    return {lo, hi};
}

const Blob::Lex *Blob::findLex(std::string_view plain) const
{
    const Lex *b = m_lex, *e = m_lex + m_header->nLex;
    auto it = std::lower_bound(b, e, plain, [this](const Lex &x, std::string_view v) { return str(x.plain) < v; });
    return it != e && str(it->plain) == plain ? it : nullptr;
}

const Blob::Contraction *Blob::findContraction(std::string_view bare) const
{
    const Contraction *b = m_contractions, *e = m_contractions + m_header->nContractions;
    auto it = std::lower_bound(b, e, bare,
                               [this](const Contraction &x, std::string_view v) { return str(x.bare) < v; });
    return it != e && str(it->bare) == bare ? it : nullptr;
}

std::pair<const uint32_t *, const uint32_t *> Blob::index(std::string_view key) const
{
    const uint64_t h = hash(key);
    const uint64_t *b = m_hashes, *e = m_hashes + m_header->nIndex;
    auto [lo, hi] = std::equal_range(b, e, h);
    return {m_ids + (lo - b), m_ids + (hi - b)};
}

}  // namespace barryglide
