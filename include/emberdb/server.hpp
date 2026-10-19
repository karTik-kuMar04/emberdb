#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "emberdb/resp_parser.hpp"
#include "emberdb/socket.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

class Aof;

// Single-threaded epoll event loop serving many clients (Linux only).
class Server {
public:
    // Pass port 0 to let the OS pick a free port (used by tests).
    // `aof` is optional: when set, every state change is logged to it.
    Server(Store& store, std::uint16_t port, Aof* aof = nullptr);

    void run();                                          // blocks until stop()
    void stop() noexcept { stopping_.store(true); }      // safe from a signal handler
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    struct Connection {
        Socket sock;
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
    void log_effects(const Command& effect);
    std::string run_rewrite(const Command& cmd);

    Store& store_;
    Aof* aof_;
    Socket listener_;
    Socket epoll_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::unordered_map<int, Connection> conns_;
    std::array<char, 16384> read_buf_{};
};

}  // namespace emberdb
