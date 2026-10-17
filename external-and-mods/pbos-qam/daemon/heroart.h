// The bottom screen's hero art (AYN Thor): the game selected on the top
// screen, with its hero and logo images, for Barry Launcher (Barry.Input's
// SteamGame reads GAME_FILE).
//
// The frontend (sections/barry.js) follows Steam's focus and tells which
// game; this finds its art on disk, in this order:
//   1. the user's own art for it: Steam's custom artwork, which is also
//      where the SteamGridDB Decky plugin puts what the user picks
//      (userdata/*/config/grid/APPID_hero.*, APPID_logo.*);
//   2. Steam's own (appcache/librarycache/APPID/...);
//   3. SteamGridDB, fetched with the user's own API key (set in the PB-OS
//      tab; none: nothing is fetched) and kept in SGDB_CACHE. A game it
//      has nothing for is not asked about again for a week.
#pragma once

#include <string>

#include "json.h"

namespace pq {

// {"appid", "name", "shortcut": bool, "running": bool, "profile": {"appid",
// "name"}} from the frontend; writes GAME_FILE, and fetches from
// SteamGridDB in the background when the art is missing.
pj::Value hero_set_game(const pj::Value& game);
// {"hasKey": bool}
pj::Value hero_sgdb_state();
// An empty key removes it.
pj::Value hero_set_sgdb_key(const std::string& key);

}  // namespace pq
