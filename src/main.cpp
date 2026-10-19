#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "emberdb/aof.hpp"
#include "emberdb/int_parse.hpp"
#include "emberdb/server.hpp"

namespace {

emberdb::Server* g_server = nullptr;

// Signal handlers may only do async-signal-safe things: setting an atomic flag is one.
void on_signal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

struct Options {
    std::uint16_t port = 6380;
    std::string aof_path;  // empty = persistence off
    emberdb::Aof::Fsync fsync = emberdb::Aof::Fsync::kEverySecond;
};

bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            return false;  // every flag takes a value
        }
        const std::string_view value = argv[++i];
        if (flag == "--port") {
            const auto port = emberdb::parse_int64(value);
            if (!port || *port < 1 || *port > 65535) {
                return false;
            }
            opts.port = static_cast<std::uint16_t>(*port);
        } else if (flag == "--aof") {
            opts.aof_path = std::string(value);
        } else if (flag == "--fsync") {
            const auto policy = emberdb::Aof::parse_fsync(value);
            if (!policy) {
                return false;
            }
            opts.fsync = *policy;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        std::cerr << "usage: emberdb [--port N] [--aof PATH] [--fsync always|everysec|no]\n";
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);

    try {
        // Declaration order matters: the server is destroyed first, then the AOF (which
        // flushes its last records), then the store.
        emberdb::Store store;
        std::unique_ptr<emberdb::Aof> aof;
        if (!opts.aof_path.empty()) {
            emberdb::Aof::LoadStats stats;
            aof = emberdb::Aof::open(opts.aof_path, opts.fsync, store, &stats);
            std::cout << "AOF: replayed " << stats.commands << " commands, " << store.size()
                      << " keys in memory" << std::endl;
            if (stats.tail_bytes > 0) {
                std::cerr << "AOF: cut " << stats.tail_bytes
                          << " bytes of an incomplete final command (crash during a write)"
                          << std::endl;
            }
        }
        emberdb::Server server(store, opts.port, aof.get());
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
