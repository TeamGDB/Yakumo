#include "lobby/reporter.hpp"

#include "adhoc/client.hpp"
#include "adhoc/protocol.hpp"
#include "adhoc/sockets.hpp"
#include "game/guest_ram.hpp"
#include "lobby/report.hpp"
#include "lobby/snapshot.hpp"
#include "psprecomp/runtime.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace mhp3rd::lobby {
namespace {

// The socket helpers live beside the ad hoc client.
namespace net = mhp3rd::adhoc::net;

using net::Socket;
using net::kNoSocket;
using std::chrono::steady_clock;

// The dashboard's report port; the client only needs the host when it is this.
constexpr std::uint16_t kDefaultLobbyPort = 8090;
// How often the game thread looks at the state.
constexpr auto kTick = std::chrono::milliseconds(1000);
// How long to wait before trying a dashboard that refused or dropped us.
constexpr auto kReconnectDelay = std::chrono::milliseconds(5000);
// Lines queued towards a dashboard that is not reading; the oldest are dropped.
constexpr std::size_t kMaxQueuedLines = 64;

std::int64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool zero_mac(const adhoc::Mac &mac) {
    return std::all_of(mac.begin(), mac.end(), [](std::uint8_t byte) { return byte == 0u; });
}

// "host", "host:port" or "[v6]:port"; the port defaults to the dashboard's.
void split_server(const std::string &server, std::string &host, std::uint16_t &port) {
    host = server;
    port = kDefaultLobbyPort;
    std::string port_text;
    if (!server.empty() && server.front() == '[') {
        const auto close = server.find(']');
        if (close != std::string::npos) {
            host = server.substr(1u, close - 1u);
            if (close + 1u < server.size() && server[close + 1u] == ':') port_text = server.substr(close + 2u);
        }
    } else if (std::count(server.begin(), server.end(), ':') == 1) {
        const auto colon = server.find(':');
        host = server.substr(0, colon);
        port_text = server.substr(colon + 1u);
    }
    if (!port_text.empty()) {
        const unsigned long value = std::strtoul(port_text.c_str(), nullptr, 10);
        if (value > 0u && value < 65536u) port = static_cast<std::uint16_t>(value);
    }
}

// A non-blocking connect with a five second wait, as the ad hoc client does.
Socket connect_to(const std::string &host, std::uint16_t port, std::string &error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *list = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0) {
        error = "cannot resolve " + host;
        return kNoSocket;
    }
    Socket result = kNoSocket;
    for (addrinfo *entry = list; entry != nullptr; entry = entry->ai_next) {
        const Socket candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (candidate == kNoSocket) continue;
        net::set_nonblocking(candidate);
        net::no_sigpipe(candidate);
        if (::connect(candidate, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0) {
            result = candidate;
            break;
        }
        if (!net::connect_pending(net::socket_error())) {
            net::close_socket(candidate);
            continue;
        }
        net::PollEntry poll{};
        poll.fd = candidate;
        poll.events = POLLOUT;
        if (net::poll_sockets(&poll, 1u, 5000) <= 0) {
            net::close_socket(candidate);
            continue;
        }
        int failure = 0;
        socklen_t length = sizeof(failure);
        if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&failure), &length) != 0 ||
            failure != 0) {
            net::close_socket(candidate);
            continue;
        }
        result = candidate;
        break;
    }
    freeaddrinfo(list);
    if (result == kNoSocket) error = "cannot connect to " + host + ":" + service;
    return result;
}

// Writes one whole line, waiting for room when the socket is full.
bool send_all(Socket socket, const std::string &line) {
    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto count =
            ::send(socket, line.data() + offset, static_cast<int>(line.size() - offset), net::kSendFlags);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && net::would_block(net::socket_error())) {
            net::PollEntry poll{};
            poll.fd = socket;
            poll.events = POLLOUT;
            if (net::poll_sockets(&poll, 1u, 5000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// The reporter's state. One instance, its thread started the first time the
// lobby is enabled and stopped at exit.
class Reporter {
public:
    static Reporter &get() {
        static Reporter reporter;
        return reporter;
    }
    ~Reporter() { shutdown(); }
    Reporter(const Reporter &) = delete;
    Reporter &operator=(const Reporter &) = delete;

    void frame(psprecomp::Runtime &runtime);
    [[nodiscard]] Status status() const;
    void shutdown();

private:
    Reporter() = default;
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    std::thread thread_;
    bool thread_started_{};
    bool stop_{};
    bool enabled_{};
    std::string server_;
    std::string token_;
    Status status_;
    std::chrono::steady_clock::time_point last_tick_{};
    Planner planner_;
};

void Reporter::frame(psprecomp::Runtime &runtime) {
    const settings::Settings &settings = settings::current();
    const bool enabled = settings.lobby && !settings.lobby_server.empty();
    std::string token;
    {
        std::lock_guard lock(mutex_);
        enabled_ = enabled;
        server_ = settings.lobby_server;
        token_ = settings.lobby_token;
        status_.enabled = enabled;
        status_.server = settings.lobby_server;
        token = token_;
    }
    if (!enabled) return;

    const auto now = steady_clock::now();
    if (last_tick_ != steady_clock::time_point{} && now - last_tick_ < kTick) return;
    last_tick_ = now;
    if (!thread_started_) {
        thread_started_ = true;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }

    const adhoc::Diagnostics diagnostics = adhoc::Client::get().diagnostics();
    if (zero_mac(diagnostics.mac)) return; // the game has not gone on line yet
    Snapshot snapshot;
    snapshot.mac = adhoc::format_mac(diagnostics.mac);
    snapshot.nickname = diagnostics.nickname;
    snapshot.product = diagnostics.product;
    snapshot.server = diagnostics.server;
    snapshot.token = token;
    if (diagnostics.group) snapshot.hall = *diagnostics.group;
    if (!snapshot.hall.empty()) {
        game::GuestRam ram(runtime.memory());
        read_hunter(ram, snapshot);
    }

    const std::vector<std::string> lines = planner_.update(snapshot, unix_seconds());
    if (lines.empty()) return;
    {
        std::lock_guard lock(mutex_);
        for (const std::string &line : lines) {
            if (queue_.size() >= kMaxQueuedLines) queue_.pop_front();
            queue_.push_back(line);
        }
    }
    wake_.notify_all();
}

void Reporter::run() {
    Socket socket = kNoSocket;
    std::string connected_server;
    steady_clock::time_point next_attempt{};
    for (;;) {
        std::deque<std::string> lines;
        bool enabled = false;
        std::string server;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(
                lock, std::chrono::milliseconds(250), [this] { return stop_ || (!queue_.empty() && enabled_); });
            if (stop_) break;
            enabled = enabled_;
            server = server_;
            if (enabled && !queue_.empty()) lines.swap(queue_);
        }
        if (!enabled || server.empty()) {
            if (socket != kNoSocket) {
                net::close_socket(socket);
                socket = kNoSocket;
                connected_server.clear();
            }
            std::lock_guard lock(mutex_);
            status_.connected = false;
            continue;
        }
        if (socket != kNoSocket && connected_server != server) {
            net::close_socket(socket);
            socket = kNoSocket;
            connected_server.clear();
        }
        if (socket == kNoSocket && lines.empty()) continue;
        if (socket == kNoSocket) {
            if (steady_clock::now() < next_attempt) continue;
            std::string error;
            std::string host;
            std::uint16_t port = 0;
            split_server(server, host, port);
            socket = connect_to(host, port, error);
            if (socket == kNoSocket) {
                next_attempt = steady_clock::now() + kReconnectDelay;
                {
                    std::lock_guard lock(mutex_);
                    status_.connected = false;
                    status_.last_error = error;
                    for (auto it = lines.rbegin(); it != lines.rend(); ++it) queue_.push_front(*it);
                    if (queue_.size() > kMaxQueuedLines) queue_.resize(kMaxQueuedLines);
                }
                continue;
            }
            connected_server = server;
            std::lock_guard lock(mutex_);
            status_.connected = true;
            status_.last_error.clear();
        }
        bool lost = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!send_all(socket, lines[i])) {
                lost = true;
                net::close_socket(socket);
                socket = kNoSocket;
                connected_server.clear();
                next_attempt = steady_clock::now() + kReconnectDelay;
                std::lock_guard lock(mutex_);
                status_.connected = false;
                status_.last_error = "the connection was lost";
                for (auto it = lines.rbegin() + static_cast<std::ptrdiff_t>(i); it != lines.rend(); ++it)
                    queue_.push_front(*it);
                if (queue_.size() > kMaxQueuedLines) queue_.resize(kMaxQueuedLines);
                break;
            }
            std::lock_guard lock(mutex_);
            ++status_.reports_sent;
        }
        if (lost) continue;
    }
    if (socket != kNoSocket) net::close_socket(socket);
}

Status Reporter::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void Reporter::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!thread_started_) return;
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    thread_started_ = false;
}

} // namespace

void frame(psprecomp::Runtime &runtime) {
    Reporter::get().frame(runtime);
}

Status status() {
    return Reporter::get().status();
}

void shutdown() {
    Reporter::get().shutdown();
}

} // namespace mhp3rd::lobby
