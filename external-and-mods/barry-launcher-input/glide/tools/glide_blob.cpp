// glide-blob: builds a pack's barryglide blob ahead of time (a build step;
// Barry builds it itself on first use otherwise).
//
//   glide-blob <pack folder> <out.blob> [--english]
//
// The pack's keyboard.json gives the words file and the accent fold;
// --english for English's words (shelld: the words' language is "en").
#include "../glide.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace barryglide;

static std::string slurp(const std::string &path, bool &ok)
{
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    std::ostringstream ss;
    if (ok)
        ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: glide-blob <pack folder> <out.blob> [--english]\n");
        return 2;
    }
    const std::string folder = argv[1], outPath = argv[2];
    const bool english = argc > 3 && std::string(argv[3]) == "--english";
    bool ok;
    json::Value v;
    const std::string kj = slurp(folder + "/keyboard.json", ok);
    std::string err;
    if (!ok || !json::parse(kj, v, &err)) {
        std::fprintf(stderr, "%s/keyboard.json: %s\n", folder.c_str(), ok ? err.c_str() : "cannot read");
        return 1;
    }
    auto pack = Pack::fromJson(v);
    if (!pack || pack->words.empty()) {
        std::fprintf(stderr, "%s: no words file\n", folder.c_str());
        return 1;
    }
    const std::string words = slurp(folder + "/" + pack->words, ok);
    if (!ok) {
        std::fprintf(stderr, "%s/%s: cannot read\n", folder.c_str(), pack->words.c_str());
        return 1;
    }
    const Layout layout = Layout::fromPack(*pack, english);
    std::string blob;
    if (!Blob::build(words, english, layout.blobFold(), blob, &err)) {
        std::fprintf(stderr, "%s: %s\n", folder.c_str(), err.c_str());
        return 1;
    }
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    out.write(blob.data(), std::streamsize(blob.size()));
    if (!out) {
        std::fprintf(stderr, "%s: cannot write\n", outPath.c_str());
        return 1;
    }
    auto b = Blob::fromBytes(blob, &err);
    std::printf("%s: %zu bytes, %u words, %u lex, %llu index keys\n", outPath.c_str(), blob.size(),
                b ? b->wordCount() : 0, b ? b->lexCount() : 0,
                b ? static_cast<unsigned long long>(b->header().nIndex) : 0ULL);
    return 0;
}
