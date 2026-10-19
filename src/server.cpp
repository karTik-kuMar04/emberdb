#include "emberdb/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "emberdb/aof.hpp"
#include "emberdb/dispatcher.hpp"
#include "emberdb/int_parse.hpp"
#include "emberdb/resp_parser.hpp"
#include "emberdb/resp_writer.hpp"
#include "emberdb/snapshot.hpp"

namespace emberdb {
namespace {

constexpr int kBacklog = 511;
constexpr std::size_t kMaxClients = 10000;
constexpr int kEpollTimeoutMs = 100;        // also the cadence of the expiry sweep
constexpr std::size_t kPurgeBudget = 1000;  // max keys deleted per sweep
constexpr std::size_t kMaxInputBytes = 64UL * 1024 * 1024;
constexpr std::size_t kMaxOutputBytes = 64UL * 1024 * 1024;
constexpr std::size_t kOutputHighWater = 1UL * 1024 * 1024;
constexpr std::size_t kCompactThreshold = 64UL * 1024;
constexpr std::size_t kFollowerBufferLimit = 256UL * 1024 * 1024;
constexpr auto kReconnectDelay = std::chrono::seconds(1);

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

std::string upper(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

// Only IPv4 addresses and "localhost": a DNS lookup would block the whole event loop.
bool make_address(const std::string& host, std::uint16_t port, sockaddr_in& out) {
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    const std::string ip = (host == "localhost") ? "127.0.0.1" : host;
    return ::inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

}  // namespace

Server::Server(Store& store, std::uint16_t port, Aof* aof) : store_(store), aof_(aof) {
    // Keys that expire are announced (to the AOF and to followers) as DEL records.
    store_.set_track_expired(true);

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

        propagate(Command{});  // announces keys that just expired
        if (aof_ != nullptr) {
            aof_->tick();  // write it out, and fsync if "everysec" is due
            maybe_rewrite_aof();
        }
        flush_followers();

        // Close everything that was marked during this pass, then (re)connect to the leader.
        // Doing both here, after all events were handled, means a reused fd number can never
        // be confused with a stale event.
        for (const int fd : doomed_) {
            close_connection(fd);
        }
        doomed_.clear();
        replication_cron();
    }
}

void Server::maybe_rewrite_aof() {
    if (!aof_->rewrite_due()) {
        return;
    }
    const std::size_t before = aof_->size();
    try {
        aof_->rewrite(store_);
        std::cout << "AOF rewritten: " << before << " -> " << aof_->size() << " bytes"
                  << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "AOF rewrite failed (retrying in 60s): " << e.what() << std::endl;
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

    if (c.role == Role::kLeaderLink && link_ == LinkState::kConnecting) {
        finish_connect(fd, c, events);
        return;
    }

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
        if (c.role == Role::kLeaderLink) {
            std::cerr << "replication: the leader closed the connection" << std::endl;
            return false;
        }
        c.closing = true;  // client hung up: send any pending reply, then close
        return true;
    }
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    if (c.role == Role::kFollower) {
        return true;  // a follower has nothing to tell us yet: ignore its bytes
    }
    if (c.role == Role::kLeaderLink) {
        link_ = LinkState::kStreaming;
        failure_logged_ = false;
    }
    c.in.append(read_buf_.data(), static_cast<std::size_t>(n));
    return process_input(c);
}

// Runs every complete command in the input buffer. Returns false to drop the connection.
bool Server::process_input(Connection& c) {
    std::size_t pos = 0;
    while (pos < c.in.size()) {
        ParseResult r = parse_command(std::string_view(c.in).substr(pos));
        if (r.status == ParseStatus::kIncomplete) {
            break;  // wait for more bytes
        }
        if (r.status == ParseStatus::kError) {
            if (c.role == Role::kLeaderLink) {
                std::cerr << "replication: protocol error from leader: " << r.error << std::endl;
                return false;
            }
            c.out += resp::error("Protocol error: " + r.error);
            c.closing = true;
            pos = c.in.size();  // discard the rest of the garbage
            break;
        }
        pos += r.consumed;

        if (c.role == Role::kLeaderLink) {
            apply_from_leader(r.command);
            continue;
        }
        c.out += handle_command(c, r.command);
        if (c.role == Role::kFollower) {
            pos = c.in.size();  // it just became a follower: ignore anything else it sent
            break;
        }
        if (c.out.size() > kMaxOutputBytes) {
            return false;  // client pipelines but never reads replies
        }
    }
    c.in.erase(0, pos);  // erase once, not once per command
    if (c.in.size() > kMaxInputBytes) {
        if (c.role == Role::kLeaderLink) {
            return false;
        }
        c.out += resp::error("input buffer too large");
        c.closing = true;
        c.in.clear();
    }
    return true;
}

std::string Server::handle_command(Connection& c, const Command& cmd) {
    if (auto reply = server_command(c, cmd)) {
        return std::move(*reply);
    }
    Command effect;
    // A follower refuses writes from clients: only its leader may change its data.
    std::string reply = execute(store_, cmd, &effect, !repl_host_.empty());
    propagate(effect);
    return reply;
}

// Commands from our leader: run them, send no reply, and pass the effects on.
void Server::apply_from_leader(const Command& cmd) {
    Command effect;
    static_cast<void>(execute(store_, cmd, &effect));
    propagate(effect);
}

// Sends one state change to the AOF and to every follower. Any key that expired while running
// the command is announced as DEL *before* the command's own effect, so a replay or a follower
// sees the same order of events as the original run did.
void Server::propagate(const Command& effect) {
    std::string bytes;
    for (const std::string& key : store_.take_expired()) {
        bytes += resp::command({"DEL", key});
    }
    if (!effect.empty()) {
        bytes += resp::command(effect);
    }
    if (bytes.empty()) {
        return;
    }
    if (aof_ != nullptr) {
        aof_->append(bytes);
    }
    for (const int fd : followers_) {
        const auto it = conns_.find(fd);
        if (it == conns_.end()) {
            continue;
        }
        Connection& f = it->second;
        f.out += bytes;
        if (f.out.size() - f.out_pos > kFollowerBufferLimit) {
            doomed_.push_back(fd);  // too far behind: drop it, it will resync from scratch
        }
    }
}

// Sends pending stream bytes to followers. Done once per loop pass, so a client that pipelines
// 100 writes costs one send per follower instead of 100.
void Server::flush_followers() {
    for (const int fd : followers_) {
        const auto it = conns_.find(fd);
        if (it == conns_.end()) {
            continue;
        }
        Connection& f = it->second;
        if (f.out_pos == f.out.size()) {
            continue;
        }
        if (!flush(f) || !update_interest(fd, f)) {
            doomed_.push_back(fd);
        }
    }
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
    } else if (c.out_pos >= kCompactThreshold && c.out_pos * 2 >= c.out.size()) {
        c.out.erase(0, c.out_pos);  // compact only when half is dead, so the cost stays amortized
        c.out_pos = 0;
    }
    return true;
}

// Decides which events epoll should watch for this connection right now.
bool Server::update_interest(int fd, Connection& c) {
    std::uint32_t want = 0;
    const std::size_t pending = c.out.size() - c.out_pos;
    if (!c.closing && pending < kOutputHighWater) {
        want |= kIn;  // stop reading from peers who are not reading our output
    }
    if (pending > 0) {
        want |= kOut;
    }
    if (want == c.events) {
        return true;
    }
    c.events = want;
    return epoll_update(epoll_.fd(), EPOLL_CTL_MOD, fd, want);
}

void Server::close_connection(int fd) {
    if (fd == leader_fd_) {
        leader_fd_ = -1;
        link_ = LinkState::kDown;
        retry_at_ = std::chrono::steady_clock::now() + kReconnectDelay;
    }
    followers_.erase(std::remove(followers_.begin(), followers_.end(), fd), followers_.end());
    conns_.erase(fd);  // Socket's destructor closes the fd, and the kernel drops it from epoll
}

// ---------------------------------------------------------------------------------------
// Commands that need server state
// ---------------------------------------------------------------------------------------

std::optional<std::string> Server::server_command(Connection& c, const Command& cmd) {
    if (cmd.empty()) {
        return std::nullopt;
    }
    const std::string name = upper(cmd[0]);
    if (name == "REWRITEAOF") {
        return cmd_rewriteaof(cmd);
    }
    if (name == "SYNC") {
        return cmd_sync(c, cmd);
    }
    if (name == "REPLICAOF") {
        return cmd_replicaof(cmd);
    }
    if (name == "INFO") {
        return cmd_info();
    }
    return std::nullopt;
}

std::string Server::cmd_rewriteaof(const Command& cmd) {
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

// A follower asks for our data. The "reply" is FLUSHALL + a full snapshot; from this moment on
// the connection also receives every change we make. Because the loop is single-threaded,
// nothing can change between building the snapshot and registering the follower.
std::string Server::cmd_sync(Connection& c, const Command& cmd) {
    if (cmd.size() != 1) {
        return resp::error("wrong number of arguments for 'SYNC' command");
    }
    std::string payload = resp::command({"FLUSHALL"});
    write_snapshot(store_, [&](std::string_view piece) { payload.append(piece); });
    if (payload.size() > kFollowerBufferLimit) {
        return resp::error("dataset too large to replicate");
    }
    c.role = Role::kFollower;
    followers_.push_back(c.sock.fd());
    return payload;
}

std::string Server::cmd_replicaof(const Command& cmd) {
    if (cmd.size() != 3) {
        return resp::error("wrong number of arguments for 'REPLICAOF' command");
    }
    if (upper(cmd[1]) == "NO" && upper(cmd[2]) == "ONE") {
        stop_replication();
        return resp::simple_string("OK");
    }
    const auto port = parse_int64(cmd[2]);
    if (!port || *port < 1 || *port > 65535) {
        return resp::error("invalid port");
    }
    const std::string error = replicate_from(cmd[1], static_cast<std::uint16_t>(*port));
    if (!error.empty()) {
        return resp::error(error);
    }
    return resp::simple_string("OK");
}

std::string Server::cmd_info() const {
    std::string out = "# Replication\r\n";
    if (repl_host_.empty()) {
        out += "role:master\r\n";
    } else {
        out += "role:slave\r\n";
        out += "master_host:" + repl_host_ + "\r\n";
        out += "master_port:" + std::to_string(repl_port_) + "\r\n";
        out += std::string("master_link_status:") +
               (link_ == LinkState::kStreaming ? "up" : "down") + "\r\n";
    }
    out += "connected_slaves:" + std::to_string(followers_.size()) + "\r\n";
    std::size_t index = 0;
    for (const int fd : followers_) {
        const auto it = conns_.find(fd);
        if (it == conns_.end()) {
            continue;
        }
        const std::size_t pending = it->second.out.size() - it->second.out_pos;
        out += "slave" + std::to_string(index++) + ":pending_bytes=" + std::to_string(pending) +
               "\r\n";
    }
    return resp::bulk_string(out);
}

// ---------------------------------------------------------------------------------------
// Following a leader
// ---------------------------------------------------------------------------------------

std::string Server::replicate_from(const std::string& host, std::uint16_t port) {
    sockaddr_in addr{};
    if (!make_address(host, port, addr)) {
        return "invalid host (use an IPv4 address or localhost)";
    }
    if (port == port_ && (ntohl(addr.sin_addr.s_addr) >> 24) == 127) {
        return "a server cannot replicate from itself";
    }
    drop_leader_link();
    repl_host_ = host;
    repl_port_ = port;
    leader_addr_ = addr;
    link_ = LinkState::kDown;
    failure_logged_ = false;
    retry_at_ = {};  // connect on the next pass of the event loop
    store_.set_passive_expiry(true);
    return {};
}

void Server::stop_replication() {
    repl_host_.clear();
    repl_port_ = 0;
    store_.set_passive_expiry(false);  // we are in charge of expiry again
    drop_leader_link();
    link_ = LinkState::kDown;
}

void Server::drop_leader_link() {
    if (leader_fd_ >= 0) {
        close_connection(leader_fd_);
    }
}

void Server::replication_cron() {
    if (repl_host_.empty() || leader_fd_ >= 0) {
        return;
    }
    if (std::chrono::steady_clock::now() < retry_at_) {
        return;
    }
    connect_to_leader();
}

void Server::log_link_failure(const std::string& why) {
    if (!failure_logged_) {
        std::cerr << "replication: " << why << " (retrying every second)" << std::endl;
        failure_logged_ = true;
    }
}

// Starts a non-blocking connect. Completion shows up as a writable event on the socket.
void Server::connect_to_leader() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        retry_at_ = std::chrono::steady_clock::now() + kReconnectDelay;
        return;
    }
    Socket sock(fd);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&leader_addr_), sizeof(leader_addr_));
    if (rc < 0 && errno != EINPROGRESS) {
        log_link_failure(std::string("cannot connect to leader: ") + std::strerror(errno));
        retry_at_ = std::chrono::steady_clock::now() + kReconnectDelay;
        return;
    }
    if (!epoll_update(epoll_.fd(), EPOLL_CTL_ADD, fd, kOut)) {
        retry_at_ = std::chrono::steady_clock::now() + kReconnectDelay;
        return;
    }
    Connection conn;
    conn.sock = std::move(sock);
    conn.role = Role::kLeaderLink;
    conn.events = kOut;
    conns_.emplace(fd, std::move(conn));
    leader_fd_ = fd;
    link_ = LinkState::kConnecting;
}

// The connect finished (or failed). On success, ask the leader for a full sync.
void Server::finish_connect(int fd, Connection& c, std::uint32_t events) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        err = errno;
    }
    if (err == 0 && (events & (kErr | kHup)) != 0) {
        err = ECONNREFUSED;
    }
    if (err == 0 && (events & kOut) == 0) {
        return;  // still connecting
    }
    if (err != 0) {
        log_link_failure(std::string("cannot connect to leader: ") + std::strerror(err));
        close_connection(fd);  // also schedules the next attempt
        return;
    }

    const int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    // A leader that dies without closing the connection (power loss, network cut) is noticed
    // after roughly 20 seconds of silence.
    const int idle = 10;
    const int interval = 3;
    const int count = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

    c.out = resp::command({"SYNC"});
    c.out_pos = 0;
    link_ = LinkState::kWaiting;
    if (!flush(c) || !update_interest(fd, c)) {
        close_connection(fd);
    }
}

}  // namespace emberdb
