#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>

#include "emberdb/server.hpp"

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

    // Writing to a closed socket would otherwise kill the whole process.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        emberdb::Store store;
        emberdb::Server server(store, port);
        std::cout << "emberdb listening on port " << port << std::endl;
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
