#include "barrylink.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

const std::string LINK_SOCKET = "/run/pbos-qam/barry.sock";
const std::map<std::string, std::set<std::string>> LINK_METHODS = {
    {"barry", {"get_state", "set_brightness"}},
    {"tools", {"get_charge_control", "set_charge_control", "get_game_profiles", "set_game_profile"}},
};
constexpr size_t kMaxLine = 64 * 1024;
constexpr size_t kMaxClients = 8;

struct Client {
    int fd;
    std::mutex write_lock;
    bool closed = false;
    std::string buf;
};

int listen_fd = -1;
int stop_fd = -1;
std::thread thread;
std::function<Section*(const std::string&)> find_section;
Host* host = nullptr;

void send_line(const std::shared_ptr<Client>& c, const Value& v) {
    std::string line = v.dump() + "\n";
    std::lock_guard<std::mutex> l(c->write_lock);
    if (c->closed) return;
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = send(c->fd, line.data() + off, line.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void reply(const std::shared_ptr<Client>& c, long long id, bool ok, const Value& v) {
    Value out = Value::object();
    out["id"] = id;
    out["ok"] = ok;
    out[ok ? "v" : "error"] = v;
    send_line(c, out);
}

void handle(const std::shared_ptr<Client>& c, const std::string& line) {
    auto v = Value::parse(line);
    if (!v || !v->is_object()) return;
    const long long id = (*v)["id"].as_int(0);
    const std::string s = (*v)["s"].str(), m = (*v)["m"].str();
    const Value args = (*v)["a"].is_array() ? (*v)["a"] : Value::array();
    auto allowed = LINK_METHODS.find(s);
    Section* section = allowed != LINK_METHODS.end() && allowed->second.count(m) ? find_section(s) : nullptr;
    if (!section) {
        reply(c, id, false, "Barry Launcher may not call " + s + "." + m);
        return;
    }
    Loop::work([c, id, section, s, m, args] {
        try {
            const Value answer = section->call(m, args);
            if (s == "tools" && m == "set_game_profile" && answer["ok"].truthy()) {
                // The game's launch options are Steam's: the PB-OS tab in
                // Steam puts the profile's wrapper in or takes it out.
                Value ev = Value::array();
                ev.push(args.at(0).str());
                ev.push(!answer["profile"].is_null());
                host->emit("tools", "barry_game_profile", ev);
            }
            reply(c, id, true, answer);
        } catch (const std::exception& e) {
            reply(c, id, false, e.what());
        }
    });
}

bool peer_allowed(int fd) {
    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    const User& u = desktop_user();
    return cred.uid == 0 || (u.ok && cred.uid == u.uid);
}

void run_link() {
    std::vector<std::shared_ptr<Client>> clients;
    auto drop = [](const std::shared_ptr<Client>& c) {
        std::lock_guard<std::mutex> l(c->write_lock);
        c->closed = true;
        close(c->fd);
    };
    for (;;) {
        std::vector<pollfd> fds = {{stop_fd, POLLIN, 0}, {listen_fd, POLLIN, 0}};
        for (const auto& c : clients) fds.push_back({c->fd, POLLIN, 0});
        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents) break;
        if (fds[1].revents & POLLIN) {
            int fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) {
                if (!peer_allowed(fd) || clients.size() >= kMaxClients) {
                    close(fd);
                } else {
                    auto c = std::make_shared<Client>();
                    c->fd = fd;
                    clients.push_back(c);
                }
            }
        }
        for (size_t i = 2; i < fds.size(); i++) {
            if (!fds[i].revents) continue;
            auto c = clients[i - 2];
            char buf[4096];
            ssize_t n = recv(c->fd, buf, sizeof buf, 0);
            if (n <= 0 && !(n < 0 && errno == EINTR)) {
                drop(c);
                continue;
            }
            if (n < 0) continue;
            c->buf.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = c->buf.find('\n')) != std::string::npos) {
                std::string line = c->buf.substr(0, nl);
                c->buf.erase(0, nl + 1);
                handle(c, line);
            }
            if (c->buf.size() > kMaxLine) drop(c);
        }
        std::erase_if(clients, [](const std::shared_ptr<Client>& c) {
            std::lock_guard<std::mutex> l(c->write_lock);
            return c->closed;
        });
    }
    for (const auto& c : clients) drop(c);
}

}  // namespace

void barry_link_start(std::function<Section*(const std::string&)> find, Host& h) {
    find_section = std::move(find);
    host = &h;
    makedirs(dirname(LINK_SOCKET));
    chmod(dirname(LINK_SOCKET).c_str(), 0755);
    unlink(LINK_SOCKET.c_str());
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, LINK_SOCKET.c_str(), sizeof addr.sun_path - 1);
    if (listen_fd < 0 || bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(listen_fd, 8) != 0) {
        log("pbos-qamd: cannot listen on %s: %s", LINK_SOCKET.c_str(), std::strerror(errno));
        if (listen_fd >= 0) close(listen_fd);
        listen_fd = -1;
        return;
    }
    // Anyone may reach it; peer_allowed() decides who is answered.
    chmod(LINK_SOCKET.c_str(), 0666);
    stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    thread = std::thread(run_link);
    log("pbos-qamd: Barry Launcher's link on %s", LINK_SOCKET.c_str());
}

void barry_link_stop() {
    if (listen_fd < 0) return;
    uint64_t one = 1;
    ssize_t r = write(stop_fd, &one, sizeof one);
    (void)r;
    if (thread.joinable()) thread.join();
    close(listen_fd);
    close(stop_fd);
    unlink(LINK_SOCKET.c_str());
    listen_fd = stop_fd = -1;
}

}  // namespace pq
