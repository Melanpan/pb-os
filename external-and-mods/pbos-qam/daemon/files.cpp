// The files section: folder listings for the PB-OS tab's file picker (what
// Decky's openFilePicker was for the Barry Launcher plugin: an app, theme or
// language pack from Downloads or a USB drive). Only the user's home and
// removable drives are listed.

#include <sys/stat.h>

#include <algorithm>

#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

std::vector<std::string> roots() {
    return {desktop_user().home, "/run/media", "/media", "/mnt", "/run/barry-launcher/usb"};
}

bool allowed(const std::string& real) {
    for (const auto& r : roots())
        if (real == r || real.rfind(r + "/", 0) == 0) return true;
    return false;
}

std::string ext_of(const std::string& name) {
    auto dot = name.find_last_of('.');
    return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

class Files : public Section {
public:
    std::string name() const override { return "files"; }

    Value call(const std::string& m, const Value& a) override {
        if (m == "home") return desktop_user().home;
        if (m != "list") throw CallError("files has no method " + m);
        // list(path, include_files, include_folders, extensions, show_hidden)
        std::string path = arg(a, 0, desktop_user().home).str();
        bool files = arg(a, 1, true).truthy();
        bool folders = arg(a, 2, true).truthy();
        std::vector<std::string> exts;
        for (const auto& e : a.at(3).items()) exts.push_back(lower(e.str()));
        bool hidden = arg(a, 4, false).truthy();
        std::string real = realpath(path);
        Value out = Value::object();
        if (!allowed(real) || !is_dir(real)) {
            out["ok"] = false;
            out["error"] = "That folder can't be opened here.";
            return out;
        }
        struct Entry {
            std::string name;
            bool dir;
            long long size;
        };
        std::vector<Entry> entries;
        for (const auto& n : listdir(real)) {
            if (!hidden && n[0] == '.') continue;
            std::string p = real + "/" + n;
            struct stat st;
            if (stat(p.c_str(), &st) != 0) continue;
            bool dir = S_ISDIR(st.st_mode);
            if (dir && !folders) continue;
            if (!dir) {
                if (!files || !S_ISREG(st.st_mode)) continue;
                if (!exts.empty() && std::find(exts.begin(), exts.end(), ext_of(n)) == exts.end()) continue;
            }
            entries.push_back({n, dir, static_cast<long long>(st.st_size)});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
            if (x.dir != y.dir) return x.dir;
            return lower(x.name) < lower(y.name);
        });
        Value list = Value::array();
        for (const auto& e : entries) {
            Value v = Value::object();
            v["name"] = e.name;
            v["path"] = real + "/" + e.name;
            v["dir"] = e.dir;
            v["size"] = e.size;
            list.push(v);
        }
        out["ok"] = true;
        out["path"] = real;
        std::string parent = dirname(real);
        out["parent"] = allowed(parent) && parent != real ? Value(parent) : Value();
        out["entries"] = list;
        Value places = Value::array();
        for (const auto& r : roots())
            if (is_dir(r)) places.push(r);
        out["places"] = places;
        return out;
    }
};

}  // namespace

std::unique_ptr<Section> make_files_section() { return std::make_unique<Files>(); }

}  // namespace pq
