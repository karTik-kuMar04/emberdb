#include "emberdb/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "emberdb/dispatcher.hpp"
#include "emberdb/resp_parser.hpp"
#include "emberdb/resp_writer.hpp"
#include "emberdb/aof.hpp"

namespace emberdb {
namespace {

constexpr int kBacklog = 511;
constexpr std::size_t kMaxClients = 10000;
constexpr int kEpollTimeoutMs = 1000;  // wake up now and then to notice stop()
constexpr std::size_t kPurgeBudget = 1000;    // max keys deleted per sweep
constexpr std::size_t kMaxInputBytes = 64UL * 1024 * 1024;
constexpr std::size_t kMaxOutputBytes = 64UL * 1024 * 1024;
constexpr std::size_t kOutputHighWater = 1UL * 1024 * 1024;
constexpr std::size_t kCompactThreshold = 64UL * 1024;

constexpr std::uint32_t kIn = EPOLLIN;
constexpr std::uint32_t kOut = EPOLLOUT;
constexpr std::uint32_t kErr = EPOLLERR;
constexpr std::uint32_t kHup = EPOLLHUP;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

bool epoll_update(int ep, int op, int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return ::epoll_ctl(ep, op, fd, &ev) == 0;
}
// REWRITEAOF needs the Aof, which the dispatcher doesn't know about, so the server handles it.
bool is_rewrite_command(const Command& cmd) {
    constexpr std::string_view kName = "REWRITEAOF";
    if (cmd.empty() || cmd[0].size() != kName.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kName.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(cmd[0][i])) != kName[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

Server::Server(Store& store, std::uint16_t port, Aof* aof) : store_(store), aof_(aof) {
    store_.set_track_expired(aof_ != nullptr);
    listener_ = Socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener_.valid()) {
        fail("socket");
    }
    const int yes = 1;
    ::setsockopt(listener_.fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fail("bind");
    }
    if (::listen(listener_.fd(), kBacklog) < 0) {
        fail("listen");
    }

    // Ask the OS which port we actually got (matters when port == 0).
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listener_.fd(), reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
        fail("getsockname");
    }
    port_ = ntohs(bound.sin_port);

    epoll_ = Socket(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_.valid()) {
        fail("epoll_create1");
    }
    if (!epoll_update(epoll_.fd(), EPOLL_CTL_ADD, listener_.fd(), kIn)) {
        fail("epoll_ctl(listener)");
    }
}

void Server::run() {
    std::array<epoll_event, 128> events{};
    int timeout_ms = kEpollTimeoutMs;
    while (!stopping_.load()) {
        const int n =
            ::epoll_wait(epoll_.fd(), events.data(), static_cast<int>(events.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("epoll_wait");
        }
        for (int i = 0; i < n; ++i) {
            const epoll_event& ev = events[static_cast<std::size_t>(i)];
            if (ev.data.fd == listener_.fd()) {
                accept_clients();
            } else {
                handle_event(ev.data.fd, ev.events);
            }
        }

        // Active expiry: delete a bounded batch of due keys. If the batch was full there is
        // probably more due, so come back immediately instead of sleeping.
        const std::size_t removed = store_.purge_expired(kPurgeBudget);
        timeout_ms = (removed == kPurgeBudget) ? 0 : kEpollTimeoutMs;
        if (aof_ != nullptr) {
            log_effects(Command{});  // records a DEL for every key that just expired
            aof_->tick();            // write it out, and fsync if "everysec" is due
            if (aof_->rewrite_due()) {
                const std::size_t before = aof_->size();
                try {
                    aof_->rewrite(store_);
                    std::cout << "AOF rewritten: " << before << " -> " << aof_->size()
                              << " bytes" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "AOF rewrite failed (retrying in 60s): " << e.what() << std::endl;
                }
            }
        }
    }
}

void Server::accept_clients() {
    // Several clients may be waiting, so accept until there are none left.
    for (;;) {
        const int fd = ::accept4(listener_.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            return;
        }
        Socket sock(fd);  // from here on, closed automatically on any early exit
        if (conns_.size() >= kMaxClients) {
            continue;  // too many clients: drop this one
        }
        const int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        if (!epoll_update(epoll_.fd(), EPOLL_CTL_ADD, fd, kIn)) {
            continue;
        }
        Connection conn;
        conn.sock = std::move(sock);
        conn.events = kIn;
        conns_.emplace(fd, std::move(conn));
    }
}

void Server::handle_event(int fd, std::uint32_t events) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;  // already closed earlier in this batch
    }
    Connection& c = it->second;

    bool ok = (events & kErr) == 0;
    if (ok && !c.closing && (events & (kIn | kHup)) != 0) {
        ok = read_some(c);
    }
    if (aof_ != nullptr) {
        aof_->commit();  // the log must be on its way to disk before any reply is sent
    }
    if (ok) {
        ok = flush(c);
    }
    if (!ok || (c.closing && c.out.empty())) {
        close_connection(fd);
        return;
    }
    if (!update_interest(fd, c)) {
        close_connection(fd);
    }
}

bool Server::read_some(Connection& c) {
    const ssize_t n = ::recv(c.sock.fd(), read_buf_.data(), read_buf_.size(), 0);
    if (n == 0) {
        c.closing = true;  // client hung up: send any pending reply, then close
        return true;
    }
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    c.in.append(read_buf_.data(), static_cast<std::size_t>(n));
    return process_input(c);
}

// Runs every complete command in the input buffer. Returns false to drop the client.
bool Server::process_input(Connection& c) {
    std::size_t pos = 0;
    while (pos < c.in.size()) {
        ParseResult r = parse_command(std::string_view(c.in).substr(pos));
        if (r.status == ParseStatus::kIncomplete) {
            break;  // wait for more bytes
        }
        if (r.status == ParseStatus::kError) {
            c.out += resp::error("Protocol error: " + r.error);
            c.closing = true;
            pos = c.in.size();  // discard the rest of the garbage
            break;
        }
        if (is_rewrite_command(r.command)) {
            c.out += run_rewrite(r.command);
        } else {
            Command effect;
            c.out += execute(store_, r.command, aof_ != nullptr ? &effect : nullptr);
            if (aof_ != nullptr) {
                log_effects(effect);
            }
        }
        pos += r.consumed;
        if (c.out.size() > kMaxOutputBytes) {
            return false;  // client pipelines but never reads replies
        }
    }
    c.in.erase(0, pos);  // erase once, not once per command
    if (c.in.size() > kMaxInputBytes) {
        c.out += resp::error("input buffer too large");
        c.closing = true;
        c.in.clear();
    }
    return true;
}

// Sends as much of `out` as the socket accepts right now. Returns false on a real error.
bool Server::flush(Connection& c) {
    while (c.out_pos < c.out.size()) {
        const ssize_t n =
            ::send(c.sock.fd(), c.out.data() + c.out_pos, c.out.size() - c.out_pos, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // socket buffer full: EPOLLOUT will tell us when to continue
            }
            return false;
        }
        c.out_pos += static_cast<std::size_t>(n);
    }
    if (c.out_pos == c.out.size()) {
        c.out.clear();
        c.out_pos = 0;
    } else if (c.out_pos >= kCompactThreshold) {
        c.out.erase(0, c.out_pos);
        c.out_pos = 0;
    }
    return true;
}

// Decides which events epoll should watch for this client right now.
bool Server::update_interest(int fd, Connection& c) {
    std::uint32_t want = 0;
    if (!c.closing && c.out.size() < kOutputHighWater) {
        want |= kIn;  // stop reading from clients who are not reading our replies
    }
    if (!c.out.empty()) {
        want |= kOut;
    }
    if (want == c.events) {
        return true;
    }
    c.events = want;
    return epoll_update(epoll_.fd(), EPOLL_CTL_MOD, fd, want);
}

void Server::close_connection(int fd) {
    conns_.erase(fd);  // Socket's destructor closes the fd, and the kernel drops it from epoll
}

// Any key that expired while running the command is logged as DEL *before* the command's own
// effect, so replay sees the same order of events as the original run did.
void Server::log_effects(const Command& effect) {
    for (const std::string& key : store_.take_expired()) {
        aof_->append(resp::command({"DEL", key}));
    }
    if (!effect.empty()) {
        aof_->append(resp::command(effect));
    }
}

std::string Server::run_rewrite(const Command& cmd) {
    if (cmd.size() != 1) {
        return resp::error("wrong number of arguments for 'REWRITEAOF' command");
    }
    if (aof_ == nullptr) {
        return resp::error("AOF is not enabled");
    }
    try {
        aof_->rewrite(store_);
    } catch (const std::exception& e) {
        return resp::error(std::string("AOF rewrite failed: ") + e.what());
    }
    return resp::simple_string("OK");
}

}  // namespace emberdb
