#include "heroart.h"

#include <sys/stat.h>
#include <time.h>

#include <cctype>
#include <cstdio>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <utility>

#include "util.h"

namespace pq {

namespace {

using pj::Value;

const std::string GAME_DIR = "/run/pbos-qam";
const std::string GAME_FILE = GAME_DIR + "/steam-game.json";
const std::string SGDB_KEY = "/var/lib/pbos-qam/sgdb.json";
const std::string SGDB_CACHE = "/var/cache/pbos-qam/sgdb";
const std::string SGDB_API = "https://www.steamgriddb.com/api/v2";
// A game SteamGridDB had nothing for is asked about again after this.
constexpr long long SGDB_RETRY_S = 7 * 24 * 3600;

std::mutex lock;
Value current = Value::object();  // the game last told, as written
std::set<std::string> fetching;   // appids being fetched

std::string steam_dir() {
    const std::string home = desktop_user().home;
    std::string real = realpath(home + "/.steam/steam");
    return is_dir(real) ? real : home + "/.local/share/Steam";
}

// The first file matching any of the patterns.
std::string first(const std::vector<std::string>& patterns) {
    for (const auto& p : patterns)
        for (const auto& f : glob(p))
            if (is_file(f)) return f;
    return "";
}

std::string own_art(const std::string& appid, const std::string& kind) {
    return first({steam_dir() + "/userdata/*/config/grid/" + appid + "_" + kind + ".*"});
}

std::string steam_art(const std::string& appid, const std::string& file) {
    // Per game folder (newer Steam, sometimes one folder deeper), or the
    // older flat names.
    const std::string cache = steam_dir() + "/appcache/librarycache/";
    return first({cache + appid + "/" + file + ".*", cache + appid + "/*/" + file + ".*",
                  cache + appid + "_" + file + ".*"});
}

std::string sgdb_art(const std::string& appid, const std::string& kind) {
    std::string f = first({SGDB_CACHE + "/" + appid + "_" + kind + ".png", SGDB_CACHE + "/" + appid + "_" + kind + ".jpg"});
    return f;
}

bool sgdb_gave_up(const std::string& appid) {
    struct stat st;
    if (stat((SGDB_CACHE + "/" + appid + ".none").c_str(), &st) != 0) return false;
    return time(nullptr) - st.st_mtime < SGDB_RETRY_S;
}

std::string sgdb_key() { return read_json(SGDB_KEY)["key"].str(); }

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// One request to SteamGridDB's API: its JSON, or {} on any failure. The key
// goes to curl on stdin (a config line), never on its command line.
Value sgdb_get(const std::string& key, const std::string& path) {
    RunOpts o;
    o.timeout = 25;
    o.input = "header = \"Authorization: Bearer " + key + "\"\n";
    auto r = run({"curl", "-fsS", "--max-time", "20", "-K", "-", "-H", "Accept: application/json", SGDB_API + path}, o);
    if (r.code != 0) {
        log("heroart: SteamGridDB %s failed: %s", path.c_str(), trim(r.err).c_str());
        return Value::object();
    }
    auto v = Value::parse(r.out);
    return v && v->is_object() ? *v : Value::object();
}

// The first image's URL of a SteamGridDB list answer, "" when none.
std::string first_url(const Value& answer) {
    if (!answer["success"].truthy()) return "";
    for (const auto& item : answer["data"].items())
        if (!item["url"].str().empty()) return item["url"].str();
    return "";
}

bool download(const std::string& url, const std::string& to) {
    // Only SteamGridDB's own image hosts.
    static const std::regex ok(R"(^https://[a-z0-9.-]*steamgriddb\.com/[^\s]+$)");
    if (!std::regex_match(url, ok)) {
        log("heroart: not fetching %s", url.c_str());
        return false;
    }
    makedirs(SGDB_CACHE);
    const std::string tmp = to + ".part";
    RunOpts o;
    o.timeout = 40;
    auto r = run({"curl", "-fsSL", "--max-time", "35", "--max-filesize", "26214400", "-o", tmp, url}, o);
    if (r.code != 0) {
        log("heroart: download %s failed: %s", url.c_str(), trim(r.err).c_str());
        remove_file(tmp);
        return false;
    }
    chmod(tmp.c_str(), 0644);
    return rename(tmp.c_str(), to.c_str()) == 0;
}

std::string ext_of(const std::string& url) {
    auto dot = url.rfind('.');
    std::string e = dot == std::string::npos ? "" : lower(url.substr(dot + 1));
    return e == "jpeg" ? "jpg" : e;
}

// Hero and logo for the game from SteamGridDB, into SGDB_CACHE.
void sgdb_fetch(std::string appid, std::string name, bool shortcut, bool want_hero, bool want_logo) {
    const std::string key = sgdb_key();
    if (key.empty()) return;
    std::string where = "steam/" + appid;
    if (shortcut) {
        // A non-Steam game: found by its name (encoded twice, as the
        // SteamGridDB plugin does).
        auto found = sgdb_get(key, "/search/autocomplete/" + url_encode(url_encode(name)));
        long long id = 0;
        for (const auto& g : found["data"].items()) {
            id = g["id"].as_int(0);
            if (id) break;
        }
        if (!id) {
            log("heroart: SteamGridDB has no game called %s", name.c_str());
            write_text(SGDB_CACHE + "/" + appid + ".none", name);
            return;
        }
        where = "game/" + std::to_string(id);
    }
    bool got = false;
    if (want_hero) {
        std::string url = first_url(sgdb_get(key, "/heroes/" + where + "?types=static&nsfw=false&humor=false&mimes=image/png,image/jpeg"));
        std::string ext = ext_of(url);
        if (!url.empty() && (ext == "png" || ext == "jpg"))
            got = download(url, SGDB_CACHE + "/" + appid + "_hero." + ext) || got;
    }
    if (want_logo) {
        std::string url = first_url(sgdb_get(key, "/logos/" + where + "?types=static&nsfw=false&humor=false&mimes=image/png"));
        if (!url.empty() && ext_of(url) == "png") got = download(url, SGDB_CACHE + "/" + appid + "_logo.png") || got;
    }
    if (!got) write_text(SGDB_CACHE + "/" + appid + ".none", name);
    log("heroart: SteamGridDB for %s (%s): %s", appid.c_str(), name.c_str(), got ? "art fetched" : "nothing");
}

// The game's art, as found now.
void resolve(Value& g) {
    const std::string appid = g["appid"].str();
    std::string hero = own_art(appid, "hero"), logo = own_art(appid, "logo");
    if (hero.empty()) hero = steam_art(appid, "library_hero");
    if (logo.empty()) logo = steam_art(appid, "logo");
    if (hero.empty()) hero = sgdb_art(appid, "hero");
    if (logo.empty()) logo = sgdb_art(appid, "logo");
    g["hero"] = hero;
    g["logo"] = logo;
}

void write_game(const Value& g) {
    makedirs(GAME_DIR);
    chmod(GAME_DIR.c_str(), 0755);
    write_json(GAME_FILE, g);
}

}  // namespace

Value hero_set_game(const Value& game) {
    static const std::regex id_re("^[0-9]{1,10}$");
    Value g = Value::object();
    const std::string appid = game["appid"].str();
    if (!std::regex_match(appid, id_re)) return false;
    g["appid"] = appid;
    g["name"] = game["name"].str();
    g["running"] = game["running"].truthy();
    const bool shortcut = game["shortcut"].truthy();
    Value profile = Value::object();
    if (std::regex_match(game["profile"]["appid"].str(), id_re)) {
        profile["appid"] = game["profile"]["appid"].str();
        profile["name"] = game["profile"]["name"].str();
    }
    g["profile"] = profile;
    resolve(g);

    bool fetch = false;
    {
        std::lock_guard<std::mutex> l(lock);
        if (g.dump() == current.dump()) return true;
        current = g;
        write_game(g);
        if ((g["hero"].str().empty() || g["logo"].str().empty()) && !fetching.count(appid) && !sgdb_gave_up(appid) &&
            !sgdb_key().empty()) {
            fetching.insert(appid);
            fetch = true;
        }
    }
    if (fetch) {
        const std::string name = g["name"].str();
        const bool want_hero = g["hero"].str().empty(), want_logo = g["logo"].str().empty();
        std::thread([appid, name, shortcut, want_hero, want_logo] {
            sgdb_fetch(appid, name, shortcut, want_hero, want_logo);
            std::lock_guard<std::mutex> l(lock);
            fetching.erase(appid);
            // Still the game shown: its new art goes up.
            if (std::as_const(current)["appid"].str() == appid) {
                Value g = current;
                resolve(g);
                if (g.dump() != current.dump()) {
                    current = g;
                    write_game(g);
                }
            }
        }).detach();
    }
    return true;
}

Value hero_sgdb_state() {
    Value out = Value::object();
    out["hasKey"] = !sgdb_key().empty();
    return out;
}

Value hero_set_sgdb_key(const std::string& key) {
    static const std::regex key_re("^[A-Za-z0-9]{16,128}$");
    const std::string k = trim(key);
    if (k.empty()) {
        remove_file(SGDB_KEY);
        log("heroart: SteamGridDB key removed");
    } else if (!std::regex_match(k, key_re)) {
        Value e = Value::object();
        e["ok"] = false;
        e["error"] = "That doesn't look like a SteamGridDB API key (letters and digits only).";
        return e;
    } else {
        Value v = Value::object();
        v["key"] = k;
        makedirs(dirname(SGDB_KEY), 0700);
        chmod(dirname(SGDB_KEY).c_str(), 0700);  // the key is the user's: root only
        write_json(SGDB_KEY, v);
        chmod(SGDB_KEY.c_str(), 0600);
        // Games it found nothing for before get another try.
        for (const auto& f : glob(SGDB_CACHE + "/*.none")) remove_file(f);
        log("heroart: SteamGridDB key set");
    }
    Value out = hero_sgdb_state();
    out["ok"] = true;
    return out;
}

}  // namespace pq
