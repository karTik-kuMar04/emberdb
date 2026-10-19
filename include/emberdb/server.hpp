#pragma once

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "emberdb/resp_parser.hpp"
#include "emberdb/socket.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

class Aof;

// Single-threaded epoll event loop serving many clients (Linux only).
// Can also follow another emberdb server (replication).
class Server {
public:
    // Pass port 0 to let the OS pick a free port (used by tests).
    // `aof` is optional: when set, every state change is logged to it.
    Server(Store& store, std::uint16_t port, Aof* aof = nullptr);

    void run();                                          // blocks until stop()
    void stop() noexcept { stopping_.store(true); }      // safe from a signal handler
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Makes this server follow host:port (IPv4 address or "localhost"). Returns an error message,
    // or an empty string on success. The connection is made from inside run().
    // Call before run() or from the loop thread (the REPLICAOF command does the latter).
    std::string replicate_from(const std::string& host, std::uint16_t port);
    // Stops following and becomes a leader again.
    void stop_replication();

private:
    enum class Role { kClient, kFollower, kLeaderLink };
    enum class LinkState { kDown, kConnecting, kWaiting, kStreaming };

    struct Connection {
        Socket sock;
        Role role = Role::kClient;
        std::string in;            // bytes received, not yet parsed
        std::string out;           // reply bytes not yet fully sent
        std::size_t out_pos = 0;   // how much of `out` is already sent
        std::uint32_t events = 0;  // epoll events we are currently watching
        bool closing = false;      // close once `out` has drained
    };

    void accept_clients();
    void handle_event(int fd, std::uint32_t events);
    bool read_some(Connection& c);
    bool process_input(Connection& c);
    bool flush(Connection& c);
    bool update_interest(int fd, Connection& c);
    void close_connection(int fd);

    std::string handle_command(Connection& c, const Command& cmd);
    void apply_from_leader(const Command& cmd);
    void propagate(const Command& effect);
    void flush_followers();
    void maybe_rewrite_aof();

    // Commands that need server state, so the dispatcher can't run them.
    std::optional<std::string> server_command(Connection& c, const Command& cmd);
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
