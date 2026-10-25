#include <sys/stat.h>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "emberdb/aof.hpp"
#include "emberdb/config.hpp"
#include "emberdb/int_parse.hpp"
#include "emberdb/security.hpp"
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
    std::string bind_address = "127.0.0.1";  // only this machine, unless told otherwise
    std::string aof_path;                    // empty = persistence off
    emberdb::Aof::Fsync fsync = emberdb::Aof::Fsync::kEverySecond;
    std::size_t aof_rewrite_min_bytes = 64UL * 1024 * 1024;
    std::string leader_host;  // empty = run as a leader
    std::uint16_t leader_port = 0;
    std::size_t maxmemory = 0;  // 0 = unlimited
    emberdb::EvictionPolicy policy = emberdb::EvictionPolicy::kNoEviction;
    std::string password;         // empty = no authentication
    std::string leader_password;  // presented to our leader when following it
    bool protected_mode = true;
    std::string metrics_bind = "127.0.0.1";
    std::uint16_t metrics_port = 0;  // 0 = no metrics endpoint
};

std::string read_small_file(const std::string& path) {
    constexpr std::size_t kMax = 1UL << 20;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path + "'");
    }
    std::string data(kMax + 1, '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(in.gcount()));
    if (data.size() > kMax) {
        throw std::runtime_error("'" + path + "' is larger than 1 MB");
    }
    return data;
}

void warn_if_group_or_world_readable(const std::string& path, const char* what) {
    // Container orchestrators mount secrets here and decide the permissions themselves.
    if (path.starts_with("/run/secrets/")) {
        return;
    }
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        std::cerr << "warning: " << path << " holds " << what
                  << " but other users can read it; run: chmod 600 " << path << std::endl;
    }
}

// Reads a password from the first line of a file. Returns an error message, or "".
std::string read_password_file(const std::string& path, std::string& out) {
    try {
        std::string text = read_small_file(path);
        const std::size_t newline = text.find('\n');
        if (newline != std::string::npos) {
            text.resize(newline);
        }
        while (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }
        if (text.empty()) {
            return "the password file is empty";
        }
        warn_if_group_or_world_readable(path, "a password");
        out = std::move(text);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

// Applies one option, given by name without leading dashes. Returns an error message, or "".
// Messages never contain the option's value, because it may be a password.
std::string apply_option(Options& opts, std::string_view name, std::string_view value) {
    if (name == "port") {
        const auto port = emberdb::parse_int64(value);
        if (!port || *port < 1 || *port > 65535) {
            return "port must be a number from 1 to 65535";
        }
        opts.port = static_cast<std::uint16_t>(*port);
    } else if (name == "bind") {
        opts.bind_address = std::string(value);
    } else if (name == "aof") {
        opts.aof_path = std::string(value);
    } else if (name == "fsync") {
        const auto policy = emberdb::Aof::parse_fsync(value);
        if (!policy) {
            return "fsync must be always, everysec or no";
        }
        opts.fsync = *policy;
    } else if (name == "aof-rewrite-min-bytes") {
        const auto bytes = emberdb::parse_int64(value);
        if (!bytes || *bytes < 1) {
            return "aof-rewrite-min-bytes must be a positive number";
        }
        opts.aof_rewrite_min_bytes = static_cast<std::size_t>(*bytes);
    } else if (name == "replicaof") {
        const std::size_t colon = value.rfind(':');
        if (colon == std::string_view::npos || colon == 0) {
            return "replicaof must look like HOST:PORT";
        }
        const auto port = emberdb::parse_int64(value.substr(colon + 1));
        if (!port || *port < 1 || *port > 65535) {
            return "replicaof has an invalid port";
        }
        opts.leader_host = std::string(value.substr(0, colon));
        opts.leader_port = static_cast<std::uint16_t>(*port);
    } else if (name == "maxmemory") {
        const auto bytes = emberdb::parse_bytes(value);
        if (!bytes) {
            return "maxmemory must be a size such as 100mb";
        }
        opts.maxmemory = static_cast<std::size_t>(*bytes);
    } else if (name == "maxmemory-policy") {
        const auto policy = emberdb::parse_eviction_policy(value);
        if (!policy) {
            return "maxmemory-policy must be noeviction or allkeys-lru";
        }
        opts.policy = *policy;
    } else if (name == "requirepass") {
        opts.password = std::string(value);
    } else if (name == "requirepass-file") {
        return read_password_file(std::string(value), opts.password);
    } else if (name == "masterauth") {
        opts.leader_password = std::string(value);
    } else if (name == "masterauth-file") {
        return read_password_file(std::string(value), opts.leader_password);
    } else if (name == "metrics-port") {
        const auto port = emberdb::parse_int64(value);
        if (!port || *port < 1 || *port > 65535) {
            return "metrics-port must be a number from 1 to 65535";
        }
        opts.metrics_port = static_cast<std::uint16_t>(*port);
    } else if (name == "metrics-bind") {
        opts.metrics_bind = std::string(value);
    } else if (name == "protected-mode") {
        if (value == "yes") {
            opts.protected_mode = true;
        } else if (value == "no") {
            opts.protected_mode = false;
        } else {
            return "protected-mode must be yes or no";
        }
    } else {
        return "unknown option '" + std::string(name) + "'";
    }
    return {};
}

std::string load_config(Options& opts, const std::string& path) {
    try {
        const std::string text = read_small_file(path);
        bool has_secret = false;
        for (const emberdb::ConfigEntry& entry : emberdb::parse_config(text)) {
            if (entry.key == "config") {
                return "line " + std::to_string(entry.line) +
                       ": 'config' cannot be used inside a config file";
            }
            const std::string error = apply_option(opts, entry.key, entry.value);
            if (!error.empty()) {
                return "line " + std::to_string(entry.line) + ": " + error;
            }
            if (entry.key == "requirepass" || entry.key == "masterauth") {
                has_secret = true;
            }
        }
        if (has_secret) {
            warn_if_group_or_world_readable(path, "a password");
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

// Returns an error message, or "" if every argument was understood.
std::string parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (!flag.starts_with("--") || flag.size() == 2) {
            return "unexpected argument '" + std::string(flag) + "'";
        }
        if (i + 1 >= argc) {
            return "option " + std::string(flag) + " needs a value";
        }
        const std::string_view name = flag.substr(2);
        char* value_arg = argv[++i];
        const std::string error = name == "config" ? load_config(opts, value_arg)
                                                   : apply_option(opts, name, value_arg);
        if (!error.empty()) {
            return "--" + std::string(name) + ": " + error;
        }
        if (name == "requirepass" || name == "masterauth") {
            // The value is copied, so hide it from `ps` and /proc/<pid>/cmdline.
            std::memset(value_arg, '*', std::strlen(value_arg));
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    const std::string problem = parse_args(argc, argv, opts);
    if (!problem.empty()) {
        std::cerr << "error: " << problem << "\n"
                  << "usage: emberdb [--config FILE] [--port N] [--bind ADDR]\n"
                     "               [--requirepass-file FILE | --requirepass PASSWORD]\n"
                     "               [--protected-mode yes|no] [--aof PATH]\n"
                     "               [--metrics-port N] [--metrics-bind ADDR]\n"
                     "               [--fsync always|everysec|no] [--aof-rewrite-min-bytes N]\n"
                     "               [--replicaof HOST:PORT] [--masterauth-file FILE]\n"
                     "               [--maxmemory SIZE] [--maxmemory-policy noeviction|allkeys-lru]\n"
                     "  Put --config first: options after it override the file.\n";
        return 1;
    }

    const bool exposed = !emberdb::is_loopback_address(opts.bind_address);
    if (opts.protected_mode && exposed && opts.password.empty()) {
        std::cerr << "refusing to start: --bind " << opts.bind_address
                  << " lets other machines connect, but no password is set.\n"
                     "  Set one with --requirepass-file FILE, bind to 127.0.0.1,\n"
                     "  or pass --protected-mode no if you really want an open server.\n";
        return 1;
    }
    if (!opts.password.empty() && opts.password.size() < 12) {
        std::cerr << "warning: the password is shorter than 12 characters; use a long random one\n";
    }
    if (exposed && !opts.password.empty()) {
        std::cerr << "warning: traffic, including the password, is not encrypted; use a private "
                     "network, a VPN or a TLS proxy\n";
    }
    if (opts.metrics_port != 0 && !emberdb::is_loopback_address(opts.metrics_bind)) {
        std::cerr << "warning: the metrics endpoint on " << opts.metrics_bind << ":"
                  << opts.metrics_port
                  << " has no authentication or encryption; keep it on a private network\n";
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
            aof->set_rewrite_policy(emberdb::Aof::RewritePolicy{opts.aof_rewrite_min_bytes, 100});
            std::cout << "AOF: replayed " << stats.commands << " commands, " << store.size()
                      << " keys in memory" << std::endl;
            if (stats.tail_bytes > 0) {
                std::cerr << "AOF: cut " << stats.tail_bytes
                          << " bytes of an incomplete final command (crash during a write)"
                          << std::endl;
            }
        }

        // Applied after the log is loaded, so replaying it can never evict anything.
        store.set_eviction_policy(opts.policy);
        store.set_maxmemory(opts.maxmemory);
        if (opts.maxmemory > 0) {
            std::cout << "maxmemory: " << opts.maxmemory << " bytes, policy "
                      << emberdb::policy_name(opts.policy) << std::endl;
        }

        emberdb::Server server(store, opts.port, aof.get(), opts.bind_address);
        server.set_password(opts.password);
        server.set_leader_password(opts.leader_password);
        if (opts.metrics_port != 0) {
            server.enable_metrics(opts.metrics_bind, opts.metrics_port);
            std::cout << "metrics: http://" << opts.metrics_bind << ":" << server.metrics_port()
                      << "/metrics" << std::endl;
        }
        g_server = &server;

        if (!opts.leader_host.empty()) {
            const std::string error = server.replicate_from(opts.leader_host, opts.leader_port);
            if (!error.empty()) {
                throw std::runtime_error("--replicaof: " + error);
            }
            std::cout << "following " << opts.leader_host << ":" << opts.leader_port << std::endl;
        }

        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        std::cout << "emberdb listening on " << opts.bind_address << ":" << server.port()
                  << (opts.password.empty() ? "" : " (password required)") << std::endl;
        server.run();
        g_server = nullptr;
        std::cout << "emberdb shutting down" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
