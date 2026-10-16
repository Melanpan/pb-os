// Steam's browser (CEF) DevTools protocol: the list of pages at
// 127.0.0.1:8080/json and one WebSocket to a page. Steam runs with
// -cef-enable-debugging (RUNSTEAM.sh); Decky Loader uses the same port.
#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "json.h"

namespace pq {

constexpr int kCefPort = 8080;

// One WebSocket client connection (RFC 6455, text frames, no TLS).
class WebSocket {
public:
    ~WebSocket() { close(); }
    bool connect(int port, const std::string& path, double timeout);
    // Safe from any thread.
    bool send_text(const std::string& text);
    enum class Read { Message, Timeout, Closed, Woken };
    // Waits up to timeout_ms (-1: no limit) for a whole message; wake_fd,
    // when readable, ends the wait with Woken.
    Read read(std::string& msg, int timeout_ms, int wake_fd);
    void close();
    bool open() const { return fd_ >= 0; }

private:
    bool send_frame(int opcode, const std::string& payload);
    bool fill(int timeout_ms, int wake_fd, Read& why);
    int fd_ = -1;
    std::string in_;      // received, not yet framed
    std::string partial_; // a fragmented message so far
    std::mutex send_lock_;
};

struct CefPage {
    std::string title, url, ws_path;
};
// Steam's Game Mode page (SharedJSContext), found as Decky Loader finds it
// (injector.py tab_is_gamepadui); nullopt when Steam's browser is not up.
std::optional<CefPage> find_gamepadui_page(double timeout);

}  // namespace pq
