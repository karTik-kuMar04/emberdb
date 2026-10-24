#pragma once

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "emberdb/http.hpp"
#include "emberdb/metrics.hpp"
#include "emberdb/resp_parser.hpp"
#include "emberdb/security.hpp"
#include "emberdb/socket.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

class Aof;

// Single-threaded epoll event loop serving many clients (Linux only).
// Can also follow another emberdb server (replication) and expose Prometheus metrics.
class Server {
public:
    // Pass port 0 to let the OS pick a free port (used by tests).
    // `aof` is optional: when set, every state change is logged to it.
    // `bind_address` is an IPv4 address or "localhost". The default only accepts connections
    // from this machine; use "0.0.0.0" to accept them from anywhere.
    Server(Store& store, std::uint16_t port, Aof* aof = nullptr,
           const std::string& bind_address = "127.0.0.1");

    void run();                                          // blocks until stop()
    void stop() noexcept { stopping_.store(true); }      // safe from a signal handler
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Clients must send AUTH <password> before anything else. Empty turns it off.
    // Call before run().
    void set_password(std::string password) { auth_ = Authenticator(std::move(password)); }
    // The password to present to our leader when following it. Call before run().
    void set_leader_password(std::string password) { leader_password_ = std::move(password); }
    // How long a connection may stay unauthenticated (default 10 seconds).
    void set_auth_timeout(std::chrono::milliseconds timeout) noexcept { auth_timeout_ = timeout; }

    // Starts an HTTP endpoint (/metrics, /healthz, /readyz) on its own port. Pass port 0 to let
    // the OS pick one. Throws std::runtime_error on failure. Call once, before run().
    void enable_metrics(const std::string& bind_address, std::uint16_t port);
    [[nodiscard]] std::uint16_t metrics_port() const noexcept { return metrics_port_; }
    // How long an HTTP client has to finish its request (default 5 seconds).
    void set_http_timeout(std::chrono::milliseconds timeout) noexcept { http_timeout_ = timeout; }

    // Makes this server follow host:port (IPv4 address or "localhost"). Returns an error message,
    // or an empty string on success. The connection is made from inside run().
    // Call before run() or from the loop thread (the REPLICAOF command does the latter).
    std::string replicate_from(const std::string& host, std::uint16_t port);
    // Stops following and becomes a leader again.
    void stop_replication();

private:
    enum class Role { kClient, kFollower, kLeaderLink, kHttp };
    enum class LinkState { kDown, kConnecting, kWaiting, kStreaming };

    struct Connection {
        Socket sock;
        Role role = Role::kClient;
        std::string peer;          // remote IP, for the log
        std::string in;            // bytes received, not yet parsed
        std::string out;           // reply bytes not yet fully sent
        std::size_t out_pos = 0;   // how much of `out` is already sent
        std::uint32_t events = 0;  // epoll events we are currently watching
        bool closing = false;      // close once `out` has drained
        bool authenticated = false;
        int auth_failures = 0;
        std::chrono::steady_clock::time_point accepted_at{};
    };

    void accept_clients();
    void accept_http_clients();
    void handle_event(int fd, std::uint32_t events);
    void handle_http_event(int fd, Connection& c, std::uint32_t events);
    bool read_some(Connection& c);
    bool read_http(Connection& c);
    bool process_input(Connection& c);
    bool flush(Connection& c);
    bool update_interest(int fd, Connection& c);
    void close_connection(int fd);

    std::string handle_command(Connection& c, const Command& cmd);
    std::string dispatch_command(Connection& c, const Command& cmd, const std::string& name);
    void apply_from_leader(const Command& cmd);
    void propagate(const Command& effect);
    void flush_followers();
    void maybe_rewrite_aof();
    void close_stale_connections();

    std::string http_reply(const HttpRequest& request);
    std::string render_metrics() const;

    // Commands that need server state, so the dispatcher can't run them.
    std::optional<std::string> server_command(Connection& c, const Command& cmd,
                                              const std::string& name);
    std::string cmd_auth(Connection& c, const Command& cmd);
    std::string try_authenticate(Connection& c, std::string_view password);
    std::string cmd_rewriteaof(const Command& cmd);
    std::string cmd_sync(Connection& c, const Command& cmd);
    std::string cmd_replicaof(const Command& cmd);
    std::string cmd_info() const;

    // Follower side.
    void replication_cron();
    void connect_to_leader();
    void finish_connect(int fd, Connection& c, std::uint32_t events);
    void drop_leader_link();
    void log_link_failure(const std::string& why);

    Store& store_;
    Aof* aof_;
    Socket listener_;
    Socket epoll_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::unordered_map<int, Connection> conns_;
    std::array<char, 16384> read_buf_{};

    Authenticator auth_;
    std::chrono::milliseconds auth_timeout_{10000};
    std::chrono::steady_clock::time_point next_sweep_{};
    std::string leader_password_;

    Metrics metrics_;
    Socket metrics_listener_;
    std::uint16_t metrics_port_ = 0;
    std::size_t http_clients_ = 0;
    std::chrono::milliseconds http_timeout_{5000};
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
    std::int64_t started_unix_ = unix_now_ms() / 1000;

    std::vector<int> followers_;  // fds of connections that follow us
    std::vector<int> doomed_;     // fds to close at the end of the current loop pass

    std::string repl_host_;  // empty = we are a leader
    std::uint16_t repl_port_ = 0;
    sockaddr_in leader_addr_{};
    int leader_fd_ = -1;
    LinkState link_ = LinkState::kDown;
    bool failure_logged_ = false;
    std::chrono::steady_clock::time_point retry_at_{};
};

}  // namespace emberdb
