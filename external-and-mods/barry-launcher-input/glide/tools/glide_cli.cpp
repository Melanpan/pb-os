// glide-cli: barryglide over JSONL cases, as barry_launcher_shelld answers
// them (its /glide, /suggest and /type "correct" requests), for the
// equivalence harness (tests/) and for measuring.
//
//   glide-cli [--dirs D1:D2] [--data DIR] [--cache DIR] [--cache-entries N]
//             [--repeat N] [--load LANG,...] [--rss] [cases.jsonl]
//
// --dirs: keyboard pack folders, later ones first (shelld's
// BARRY_KEYBOARDS_DIRS); --data: where personal-words*.txt are (shelld's
// DATA). Each case line: {"id", "lang", "op", ...}; op is glide (path,
// keys, size), correct (word), suggest (prefix), near, complete, known
// (word). Each answer line: {"id", "result"}. --repeat times each case
// (stderr: per-op microseconds); --load and --rss print memory use.
#include "../glide.h"
#include "../unicode.h"

#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unistd.h>

using namespace barryglide;

namespace {

std::vector<std::string> dirs;
std::string dataDir, cacheDir;
size_t cacheEntries = 512;

bool validLang(const std::string &s)
{
    if (s.empty() || s.size() > 32)
        return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_' || c == '-'))
            return false;
    return true;
}

std::string readFile(const std::string &path, bool &ok)
{
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    std::ostringstream ss;
    if (ok)
        ss << in.rdbuf();
    return ss.str();
}

std::string realPath(const std::string &p)
{
    char buf[PATH_MAX];
    return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

struct Found {
    std::string folder, words, wordsLang;
    Pack pack;
};

// shelld's keyboard_pack().
std::optional<Found> keyboardPack(const std::string &lang, bool borrow = true)
{
    if (!validLang(lang))
        return std::nullopt;
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        const std::string folder = *it + "/" + lang;
        bool ok;
        const std::string text = readFile(folder + "/keyboard.json", ok);
        json::Value v;
        if (!ok || !json::parse(text, v))
            continue;
        auto pack = Pack::fromJson(v);
        if (!pack)
            continue;
        Found f{folder, {}, lang, *pack};
        if (!pack->words.empty()) {
            const std::string path = realPath(folder + "/" + pack->words);
            const std::string base = realPath(folder);
            if (!path.empty() && !base.empty() && path.rfind(base + "/", 0) == 0) {
                std::ifstream probe(path);
                if (probe)
                    f.words = path;
            }
        }
        if (f.words.empty() && borrow && !pack->wordsFrom.empty() && pack->wordsFrom != lang) {
            if (auto lender = keyboardPack(pack->wordsFrom, false); lender && !lender->words.empty())
                f.words = lender->words, f.wordsLang = pack->wordsFrom;
        }
        return f;
    }
    return std::nullopt;
}

struct Dict {
    std::shared_ptr<Decoder> decoder;
    std::unique_ptr<Corrector> corrector;
    std::vector<std::pair<std::u32string, std::u32string>> fold;
};
std::map<std::string, std::unique_ptr<Dict>> packs;
std::map<std::string, std::shared_ptr<Decoder>> decoders;  // words path -> its decoder

// shelld's dictionary() (no reload on change: the harness's packs stay put).
Dict *dictionary(const std::string &lang)
{
    if (auto it = packs.find(lang); it != packs.end())
        return it->second.get();
    auto f = keyboardPack(lang);
    if (!f || f->words.empty())
        return nullptr;
    const bool english = f->wordsLang == "en";
    const Layout layout = Layout::fromPack(f->pack, english);
    std::string err;
    auto blob = Blob::load(f->words, english, layout.blobFold(), cacheDir, &err);
    if (!blob) {
        std::fprintf(stderr, "keyboard %s: cannot read %s: %s\n", lang.c_str(), f->words.c_str(), err.c_str());
        return nullptr;
    }
    auto d = std::make_unique<Dict>();
    // Two layouts of one language with the same fold share one decoder.
    const std::string decKey = f->words + "\n" + std::to_string(blob->header().key[0]);
    auto &dec = decoders[decKey];
    if (!dec)
        dec = std::make_shared<Decoder>(blob, cacheEntries);
    d->decoder = dec;
    const std::string personal = dataDir.empty() ? std::string()
        : dataDir + (english ? "/personal-words.txt" : "/personal-words-" + f->wordsLang + ".txt");
    d->corrector = std::make_unique<Corrector>(blob, layout, personal);
    d->fold = f->pack.glideFold();
    return packs.emplace(lang, std::move(d)).first->second.get();
}

std::string list(const std::vector<std::string> &v)
{
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i)
        out += (i ? "," : "") + json::quote(v[i]);
    return out + "]";
}

bool point(const json::Value &v, Point &p)
{
    if (v.type != json::Value::Array || v.arr.size() != 2 || !v.arr[0].isNumber() || !v.arr[1].isNumber())
        return false;
    p = {v.arr[0].num, v.arr[1].num};
    return true;
}

// The answer to one case, as JSON; empty for a bad case.
std::string answer(const json::Value &c)
{
    const json::Value *op = c.get("op"), *lang = c.get("lang");
    if (!op || op->type != json::Value::String)
        return {};
    const std::string l = lang && lang->type == json::Value::String ? lang->str : "en";
    Dict *d = dictionary(l);
    auto str = [&](const char *name) -> const std::string * {
        const json::Value *v = c.get(name);
        return v && v->type == json::Value::String ? &v->str : nullptr;
    };
    if (op->str == "glide") {
        const json::Value *path = c.get("path"), *keys = c.get("keys"), *size = c.get("size");
        if (!path || path->type != json::Value::Array || !keys || keys->type != json::Value::Object || !size
            || !size->isNumber())
            return {};
        std::vector<Point> pts;
        for (const auto &p : path->arr) {
            Point q;
            if (!point(p, q))
                return {};
            pts.push_back(q);
        }
        std::vector<Decoder::Key> ks;
        for (const auto &[name, at] : keys->obj) {
            Point q;
            if (!point(at, q))
                return {};
            ks.push_back({name, q});
        }
        return list(d ? d->decoder->decode(pts, ks, size->num, 4, d->fold) : std::vector<std::string>());
    }
    if (op->str == "correct") {
        const std::string *w = str("word");
        if (!w)
            return {};
        auto r = d ? d->corrector->correct(*w) : std::nullopt;
        return r ? json::quote(*r) : "null";
    }
    if (op->str == "suggest" || op->str == "near" || op->str == "complete") {
        const std::string *w = str(op->str == "suggest" ? "prefix" : "word");
        if (!w || !d)
            return w ? "[]" : std::string();
        return list(op->str == "suggest" ? d->corrector->suggest(*w)
                    : op->str == "near"  ? d->corrector->near(*w)
                                         : d->corrector->complete(*w));
    }
    if (op->str == "known") {
        const std::string *w = str("word");
        if (!w)
            return {};
        return d && d->corrector->known(*w) ? "true" : "false";
    }
    return {};
}

void printMemory(const char *when)
{
    std::ifstream in("/proc/self/status");
    std::string line;
    std::fprintf(stderr, "memory %s:", when);
    while (std::getline(in, line))
        if (line.rfind("VmRSS", 0) == 0 || line.rfind("RssAnon", 0) == 0 || line.rfind("RssFile", 0) == 0)
            std::fprintf(stderr, " %s", line.c_str());
    std::fprintf(stderr, "\n");
}

}  // namespace

int main(int argc, char **argv)
{
    std::string casesPath, load;
    int repeat = 0;
    bool rss = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--dirs") {
            std::stringstream ss(next());
            for (std::string d; std::getline(ss, d, ':');)
                if (!d.empty())
                    dirs.push_back(d);
        } else if (a == "--data") {
            dataDir = next();
        } else if (a == "--cache") {
            cacheDir = next();
        } else if (a == "--cache-entries") {
            cacheEntries = std::stoul(next());
        } else if (a == "--repeat") {
            repeat = std::stoi(next());
        } else if (a == "--load") {
            load = next();
        } else if (a == "--rss") {
            rss = true;
        } else {
            casesPath = a;
        }
    }
    if (dirs.empty())
        dirs = {"/usr/share/barry_launcher/keyboards"};
    if (rss)
        printMemory("start");
    if (!load.empty()) {
        std::stringstream ss(load);
        for (std::string l; std::getline(ss, l, ',');)
            dictionary(l);
        if (rss)
            printMemory("loaded");
    }
    if (casesPath.empty())
        return 0;
    std::ifstream in(casesPath);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", casesPath.c_str());
        return 2;
    }
    std::map<std::string, std::pair<double, long>> times;  // op -> (µs, calls)
    std::string line, out;
    long bad = 0;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        json::Value c;
        if (!json::parse(line, c)) {
            ++bad;
            continue;
        }
        std::string r = answer(c);
        for (int k = 0; k < repeat; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            r = answer(c);
            const auto t1 = std::chrono::steady_clock::now();
            auto &t = times[c.get("op")->str];
            t.first += std::chrono::duration<double, std::micro>(t1 - t0).count();
            ++t.second;
        }
        const json::Value *id = c.get("id");
        out = "{\"id\":" + (id && id->isNumber() ? std::to_string(long(id->num)) : std::string("null"))
            + ",\"result\":" + (r.empty() ? std::string("\"<bad case>\"") : r) + "}\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    for (const auto &[op, t] : times)
        std::fprintf(stderr, "time %s: %.1f us/call over %ld calls\n", op.c_str(), t.first / double(t.second), t.second);
    if (bad)
        std::fprintf(stderr, "%ld unreadable lines\n", bad);
    if (rss)
        printMemory("end");
    return 0;
}
