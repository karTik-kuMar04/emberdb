#pragma once

#include <cstdint>

#include "emberdb/socket.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

// Single-threaded blocking TCP server (first version; epoll comes later).
class Server {
public:
    // Creates the listening socket. Throws std::runtime_error on failure.
    Server(Store& store, std::uint16_t port);

    // Accepts clients forever, serving one at a time.
    void run();

private:
    void handle_client(Socket client);

    Store& store_;
    Socket listener_;
};

}  // namespace emberdb
