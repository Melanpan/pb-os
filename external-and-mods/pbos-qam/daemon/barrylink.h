// Barry Launcher's way in (AYN Thor): a local socket, LINK_SOCKET, for its
// quick settings page (Barry.Input's PbosQam). Only the desktop user (and
// root) may connect, and only a few methods may be called (LINK_METHODS):
// the screens' brightness, charging and game profiles, the same settings
// the PB-OS tab shows.
//
// One JSON object a line each way: {"id", "s": section, "m": method, "a":
// [args]} in, {"id", "ok", "v" | "error"} out. Calls run on threads of
// their own, as the frontend's do.
#pragma once

#include <functional>
#include <string>

namespace pq {

class Host;
class Section;

// find(name): the section, or nullptr.
void barry_link_start(std::function<Section*(const std::string&)> find, Host& host);
void barry_link_stop();

}  // namespace pq
