// barryglide: a pack's dictionary as one read-only blob.
//
// Built from words.txt (as barry_glide.Decoder and Corrector read it) and
// mapped from a cache file, keyed by a hash of the words, whether they are
// English and the accent fold, so a changed pack builds a new one:
//
//   header | strings | words | ends | lex | contractions | hashes | ids
//
//   words         Decoder.words in file order: word, its letter keys, level
//   ends          (first letter, last letter, word) sorted: Decoder.by_ends
//   lex           Corrector.level/typed, sorted by the lower-case word
//                 (Corrector.sorted): word, as typed, level, length
//   contractions  "dont" -> "don't", sorted
//   hashes, ids   the delete index: a 64-bit hash of each key (the word,
//                 its accent-folded form, the word less one letter) sorted,
//                 and the lex entry it leads to. Lookups check the key
//                 against the word, so a hash collision changes nothing.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace barryglide {

class Blob {
public:
    static constexpr uint32_t VERSION = 1;

    struct Header {
        char magic[8];
        uint32_t version;
        uint32_t english;
        uint64_t key[2];
        uint64_t size;
        uint32_t nWords, nEnds, nLex, nContractions;
        uint64_t nIndex;
        uint64_t strings, stringsLen, words, ends, lex, contractions, hashes, ids;
    };
    struct Str {
        uint32_t off, len;
    };
    struct Word {
        Str word, letters;
        int32_t level;
    };
    struct End {
        uint32_t first, last, word;
    };
    enum : uint32_t { APOSTROPHE = 1 };
    struct Lex {
        Str plain, typed;
        int32_t level;
        uint32_t chars;  // len() of plain: code points
        uint32_t flags;
    };
    struct Contraction {
        Str bare, typed;
    };
    using Fold = std::unordered_map<char32_t, std::u32string>;

    // The blob for words.txt's bytes; false and why on a file barry_glide
    // would not load (bad UTF-8, a line without one word and one level).
    static bool build(std::string_view wordsFile, bool english, const Fold &fold, std::string &out,
                      std::string *error = nullptr);
    static void key(std::string_view wordsFile, bool english, const Fold &fold, uint64_t out[2]);

    // A blob from memory (kept) or a mapped file; null when it is not one.
    static std::shared_ptr<Blob> fromBytes(std::string bytes, std::string *error = nullptr);
    static std::shared_ptr<Blob> map(const std::string &path, std::string *error = nullptr);

    // words.txt's blob from cacheDir (built and saved there first if need
    // be); in memory when it cannot be saved. cacheDir empty: the default
    // ($XDG_CACHE_HOME or ~/.cache)/barry_launcher/glide.
    static std::shared_ptr<Blob> load(const std::string &wordsPath, bool english, const Fold &fold,
                                      const std::string &cacheDir = {}, std::string *error = nullptr);
    static std::string defaultCacheDir();

    ~Blob();
    Blob(const Blob &) = delete;
    Blob &operator=(const Blob &) = delete;

    const Header &header() const { return *m_header; }
    bool english() const { return m_header->english != 0; }
    bool mapped() const { return m_map != nullptr; }
    size_t bytes() const { return m_size; }
    // A string in the blob; empty when the reference is out of bounds.
    std::string_view str(Str s) const
    {
        const uint64_t n = m_header->stringsLen;
        return s.off <= n && s.len <= n - s.off ? std::string_view(m_strings + s.off, s.len) : std::string_view();
    }

    const Word *words() const { return m_words; }
    uint32_t wordCount() const { return m_header->nWords; }
    // by_ends[(first, last)]
    std::pair<const End *, const End *> ends(char32_t first, char32_t last) const;
    const Lex *lex() const { return m_lex; }
    uint32_t lexCount() const { return m_header->nLex; }
    const Lex *findLex(std::string_view plain) const;
    const Contraction *findContraction(std::string_view bare) const;
    // Lex ids whose keys hash as key does (check them).
    std::pair<const uint32_t *, const uint32_t *> index(std::string_view key) const;

    static uint64_t hash(std::string_view s, uint64_t seed = 0);

private:
    Blob() = default;
    bool attach(const char *data, size_t size, std::string *error);

    std::string m_owned;
    void *m_map = nullptr;
    size_t m_size = 0;
    const Header *m_header = nullptr;
    const char *m_strings = nullptr;
    const Word *m_words = nullptr;
    const End *m_ends = nullptr;
    const Lex *m_lex = nullptr;
    const Contraction *m_contractions = nullptr;
    const uint64_t *m_hashes = nullptr;
    const uint32_t *m_ids = nullptr;
};

}  // namespace barryglide
