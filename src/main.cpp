#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>

#include "emberdb/server.hpp"

namespace {

emberdb::Server* g_server = nullptr;

// Signal handlers may only do async-signal-safe things: setting an atomic flag is one.
void on_signal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 6380;
    if (argc > 1) {
        const int parsed = std::atoi(argv[1]);
        if (parsed < 1 || parsed > 65535) {
            std::cerr << "usage: emberdb [port]\n";
            return 1;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    std::signal(SIGPIPE, SIG_IGN);

    try {
        emberdb::Store store;
        emberdb::Server server(store, port);
        g_server = &server;

        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        std::cout << "emberdb listening on port " << server.port() << std::endl;
        server.run();
        g_server = nullptr;
        std::cout << "emberdb shutting down" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
