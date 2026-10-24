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
#include <vector>

#include "emberdb/aof.hpp"
#include "emberdb/dispatcher.hpp"
#include "emberdb/http.hpp"
#include "emberdb/int_parse.hpp"
#include "emberdb/metrics.hpp"
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

// An anonymous client may buffer this much before it has to log in, enough for any AUTH command.
constexpr std::size_t kMaxUnauthInputBytes = 16UL * 1024;
constexpr int kMaxAuthFailures = 3;  // wrong guesses allowed on one connection

// The metrics endpoint is for one scraper, so it gets very few connections.
constexpr std::size_t kMaxHttpClients = 16;
constexpr int kMetricsBacklog = 16;
constexpr const char* kTextPlain = "text/plain; charset=utf-8";
constexpr const char* kPrometheusType = "text/plain; version=0.0.4; charset=utf-8";

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

std::uint64_t nanos_since(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return ns > 0 ? static_cast<std::uint64_t>(ns) : 0;
}

// The names that get their own metrics series: everything the dispatcher knows, plus the commands
// the server handles itself. Anything else is counted as "other".
std::vector<std::string> all_command_names() {
    std::vector<std::string> names = command_names();
    for (const char* extra : {"AUTH", "SYNC", "REPLICAOF", "INFO", "REWRITEAOF"}) {
        names.emplace_back(extra);
    }
    return names;
}

}  // namespace

Server::Server(Store& store, std::uint16_t port, Aof* aof, const std::string& bind_address)
    : store_(store), aof_(aof), metrics_(all_command_names()) {
    // Keys that expire or get evicted are announced (to the AOF and to followers) as DEL records.
    store_.set_track_expired(true);

    sockaddr_in addr{};
    if (!make_address(bind_address, port, addr)) {
        throw std::runtime_error("invalid bind address '" + bind_address +
                                 "' (use an IPv4 address or localhost)");
    }

    listener_ = Socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener_.valid()) {
        fail("socket");
    }
    const int yes = 1;
    ::setsockopt(listener_.fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

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

void Server::enable_metrics(const std::string& bind_address, std::uint16_t port) {
    if (metrics_listener_.valid()) {
        throw std::runtime_error("the metrics endpoint is already enabled");
    }
    sockaddr_in addr{};
    if (!make_address(bind_address, port, addr)) {
        throw std::runtime_error("invalid metrics bind address '" + bind_address +
                                 "' (use an IPv4 address or localhost)");
    }
    Socket sock(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!sock.valid()) {
        fail("metrics socket");
    }
    const int yes = 1;
    ::setsockopt(sock.fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (::bind(sock.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fail("metrics bind");
    }
    if (::listen(sock.fd(), kMetricsBacklog) < 0) {
        fail("metrics listen");
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
        fail("metrics getsockname");
    }
    if (!epoll_update(epoll_.fd(), EPOLL_CTL_ADD, sock.fd(), kIn)) {
        fail("epoll_ctl(metrics listener)");
    }
    metrics_port_ = ntohs(bound.sin_port);
    metrics_listener_ = std::move(sock);
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
        const auto busy_from = std::chrono::steady_clock::now();  // from here on we are working
        for (int i = 0; i < n; ++i) {
            const epoll_event& ev = events[static_cast<std::size_t>(i)];
            if (ev.data.fd == listener_.fd()) {
                accept_clients();
            } else if (metrics_listener_.valid() && ev.data.fd == metrics_listener_.fd()) {
                accept_http_clients();
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
        close_stale_connections();

        // Close everything that was marked during this pass, then (re)connect to the leader.
        // Doing both here, after all events were handled, means a reused fd number can never
        // be confused with a stale event.
        for (const int fd : doomed_) {
            close_connection(fd);
        }
        doomed_.clear();
        replication_cron();

        metrics_.loop_busy_ns += nanos_since(busy_from);
    }
}

void Server::maybe_rewrite_aof() {
    if (!aof_->rewrite_due()) {
        return;
    }
    const std::size_t before = aof_->size();
    try {
        aof_->rewrite(store_);
        ++metrics_.aof_rewrites;
        std::cout << "AOF rewritten: " << before << " -> " << aof_->size() << " bytes"
                  << std::endl;
    } catch (const std::exception& e) {
        ++metrics_.aof_rewrite_failures;
        std::cerr << "AOF rewrite failed (retrying in 60s): " << e.what() << std::endl;
    }
}

// Anonymous connections cost a file descriptor and some memory. Without a deadline, anyone could
// open thousands of silent ones and fill kMaxClients, locking real clients out. The same goes for
// HTTP clients that never finish their request.
void Server::close_stale_connections() {
    if (!auth_.required() && http_clients_ == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_sweep_) {
        return;
    }
    next_sweep_ = now + std::chrono::milliseconds(kEpollTimeoutMs);
    for (const auto& [fd, conn] : conns_) {
        if (conn.role == Role::kHttp) {
            if (now - conn.accepted_at >= http_timeout_) {
                doomed_.push_back(fd);
            }
        } else if (auth_.required() && conn.role == Role::kClient && !conn.authenticated &&
                   now - conn.accepted_at >= auth_timeout_) {
            doomed_.push_back(fd);
            metrics_.connection_dropped(DropReason::kAuthTimeout);
        }
    }
}

void Server::accept_clients() {
    // Several clients may be waiting, so accept until there are none left.
    for (;;) {
        sockaddr_in peer_addr{};
        socklen_t peer_len = sizeof(peer_addr);
        const int fd = ::accept4(listener_.fd(), reinterpret_cast<sockaddr*>(&peer_addr),
                                 &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
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
            metrics_.connection_dropped(DropReason::kMaxClients);
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
        conn.accepted_at = std::chrono::steady_clock::now();
        char text[INET_ADDRSTRLEN] = {};
        conn.peer = ::inet_ntop(AF_INET, &peer_addr.sin_addr, text, sizeof(text)) != nullptr
                        ? text
                        : "?";
        conns_.emplace(fd, std::move(conn));
        ++metrics_.connections_accepted;
    }
}

void Server::accept_http_clients() {
    for (;;) {
        const int fd = ::accept4(metrics_listener_.fd(), nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            std::cerr << "metrics accept failed: " << std::strerror(errno) << "\n";
            return;
        }
        Socket sock(fd);
        if (http_clients_ >= kMaxHttpClients) {
            continue;  // the extra connection is closed right away
        }
        if (!epoll_update(epoll_.fd(), EPOLL_CTL_ADD, fd, kIn)) {
            continue;
        }
        Connection conn;
        conn.sock = std::move(sock);
        conn.role = Role::kHttp;
        conn.events = kIn;
        conn.accepted_at = std::chrono::steady_clock::now();
        conns_.emplace(fd, std::move(conn));
        ++http_clients_;
    }
}

void Server::handle_event(int fd, std::uint32_t events) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;  // already closed earlier in this batch
    }
    Connection& c = it->second;

    if (c.role == Role::kHttp) {
        handle_http_event(fd, c, events);
        return;
    }
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

void Server::handle_http_event(int fd, Connection& c, std::uint32_t events) {
    bool ok = (events & kErr) == 0;
    if (ok && !c.closing && (events & (kIn | kHup)) != 0) {
        ok = read_http(c);
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

// Reads an HTTP request and queues the answer. Every connection serves exactly one request.
bool Server::read_http(Connection& c) {
    const ssize_t n = ::recv(c.sock.fd(), read_buf_.data(), read_buf_.size(), 0);
    if (n == 0) {
        return false;  // the client left before finishing its request
    }
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    c.in.append(read_buf_.data(), static_cast<std::size_t>(n));

    const HttpParseResult parsed = parse_http_request(c.in);
    if (parsed.status == HttpParse::kIncomplete) {
        if (c.in.size() > kMaxHttpRequestBytes) {
            c.out = http_response(431, kTextPlain, "request too large\n");
            c.closing = true;
            c.in.clear();
        }
        return true;
    }
    c.in.clear();
    c.out = parsed.status == HttpParse::kBad ? http_response(400, kTextPlain, "bad request\n")
                                             : http_reply(parsed.request);
    c.closing = true;  // one request per connection: close once the answer is sent
    return true;
}

std::string Server::http_reply(const HttpRequest& request) {
    if (request.method != "GET") {
        return http_response(405, kTextPlain, "method not allowed\n", "Allow: GET\r\n");
    }
    if (request.path == "/metrics") {
        ++metrics_.scrapes;
        return http_response(200, kPrometheusType, render_metrics());
    }
    if (request.path == "/healthz") {
        return http_response(200, kTextPlain, "ok\n");  // the loop is alive, since it answered
    }
    if (request.path == "/readyz") {
        const bool ready = repl_host_.empty() || link_ == LinkState::kStreaming;
        return ready ? http_response(200, kTextPlain, "ready\n")
                     : http_response(503, kTextPlain, "not ready: the replication link is down\n");
    }
    return http_response(404, kTextPlain, "not found\n");
}

std::string Server::render_metrics() const {
    MetricsSnapshot s;
    s.uptime_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count();
    s.start_time_unix = started_unix_;
    for (const auto& entry : conns_) {
        if (entry.second.role == Role::kClient) {
            ++s.connected_clients;
        }
    }
    s.connected_followers = followers_.size();
    for (const int fd : followers_) {
        const auto it = conns_.find(fd);
        if (it != conns_.end()) {
            s.follower_max_pending_bytes =
                std::max(s.follower_max_pending_bytes, it->second.out.size() - it->second.out_pos);
        }
    }
    s.keys = store_.size();
    s.used_memory = store_.used_memory();
    s.max_memory = store_.maxmemory();
    s.expired_keys = store_.expired_keys();
    s.evicted_keys = store_.evicted_keys();
    s.aof_enabled = aof_ != nullptr;
    s.aof_size_bytes = aof_ != nullptr ? aof_->size() : 0;
    s.auth_required = auth_.required();
    s.is_follower = !repl_host_.empty();
    s.replication_link_up = link_ == LinkState::kStreaming;
    return render_prometheus(metrics_, s);
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
    metrics_.bytes_read += static_cast<std::uint64_t>(n);
    if (c.role == Role::kFollower) {
        return true;  // a follower has nothing to tell us yet: ignore its bytes
    }
    if (c.role == Role::kLeaderLink) {
        if (link_ == LinkState::kWaiting && read_buf_[0] == '-') {
            // The leader answered our SYNC with an error (wrong or missing password, say).
            // Show its message instead of a confusing parse error.
            std::string line(read_buf_.data(), static_cast<std::size_t>(n));
            const std::size_t end = line.find("\r\n");
            if (end != std::string::npos) {
                line.resize(end);
            }
            if (!failure_logged_) {
                std::cerr << "replication: the leader refused the sync: " << line
                          << " (retrying every second)" << std::endl;
                failure_logged_ = true;
            }
            return false;
        }
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
            metrics_.connection_dropped(DropReason::kProtocolError);
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
        if (c.role == Role::kFollower || c.closing) {
            pos = c.in.size();  // it became a follower, or we are hanging up: ignore the rest
            break;
        }
        if (c.out.size() > kMaxOutputBytes) {
            metrics_.connection_dropped(DropReason::kOutputLimit);
            return false;  // client pipelines but never reads replies
        }
    }
    c.in.erase(0, pos);  // erase once, not once per command

    if (c.role == Role::kClient && auth_.required() && !c.authenticated &&
        c.in.size() > kMaxUnauthInputBytes) {
        c.out += resp::error("too much data before authentication");
        c.closing = true;
        c.in.clear();
        return true;
    }
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

// Times the command and counts it. The time is what the server spent, so network time is
// not included.
std::string Server::handle_command(Connection& c, const Command& cmd) {
    const auto started = std::chrono::steady_clock::now();
    const std::string name = cmd.empty() ? std::string() : upper(cmd[0]);
    std::string reply = dispatch_command(c, cmd, name);
    metrics_.record_command(name, nanos_since(started), !reply.empty() && reply.front() == '-');
    return reply;
}

std::string Server::dispatch_command(Connection& c, const Command& cmd, const std::string& name) {
    if (name == "AUTH") {
        return cmd_auth(c, cmd);
    }
    if (auth_.required() && !c.authenticated) {
        if (name == "SYNC" && cmd.size() == 2) {
            // A follower presents its password together with the sync request.
            std::string refusal = try_authenticate(c, cmd[1]);
            if (!refusal.empty()) {
                return refusal;
            }
            return cmd_sync(c, Command{"SYNC"});
        }
        return resp::error_code("NOAUTH", "Authentication required.");
    }
    if (auto reply = server_command(c, cmd, name)) {
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
    ++metrics_.replicated_commands_applied;
}

// Sends one state change to the AOF and to every follower. Any key that expired or was evicted
// while running the command is announced as DEL *before* the command's own effect, so a replay
// or a follower sees the same order of events as the original run did.
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
        if (f.out.size() - f.out_pos > kFollowerBufferLimit &&
            std::find(doomed_.begin(), doomed_.end(), fd) == doomed_.end()) {
            doomed_.push_back(fd);  // too far behind: drop it, it will resync from scratch
            metrics_.connection_dropped(DropReason::kFollowerLag);
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
        if (c.role != Role::kHttp) {
            metrics_.bytes_written += static_cast<std::uint64_t>(n);
        }
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
    const auto it = conns_.find(fd);
    if (it != conns_.end()) {
        if (it->second.role == Role::kHttp) {
            --http_clients_;
        }
        conns_.erase(it);  // Socket's destructor closes the fd, and the kernel drops it from epoll
    }
}

// ---------------------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------------------

// Checks a password guess for this connection. Returns "" on success, otherwise the error reply.
std::string Server::try_authenticate(Connection& c, std::string_view password) {
    switch (auth_.attempt(password, std::chrono::steady_clock::now())) {
        case AuthResult::kOk:
            c.authenticated = true;
            return {};
        case AuthResult::kThrottled:
            c.closing = true;
            return resp::error("too many authentication failures, try again later");
        case AuthResult::kWrongPassword:
            break;
    }
    ++metrics_.auth_failures;
    std::cerr << "auth: failed attempt from " << c.peer << std::endl;  // never log the guess
    if (++c.auth_failures >= kMaxAuthFailures) {
        c.closing = true;
    }
    return resp::error_code("WRONGPASS", "invalid password");
}

std::string Server::cmd_auth(Connection& c, const Command& cmd) {
    if (cmd.size() != 2) {
        return resp::error("wrong number of arguments for 'AUTH' command");
    }
    if (!auth_.required()) {
        return resp::error("AUTH called without any password configured");
    }
    std::string refusal = try_authenticate(c, cmd[1]);
    return refusal.empty() ? resp::simple_string("OK") : refusal;
}

// ---------------------------------------------------------------------------------------
// Commands that need server state
// ---------------------------------------------------------------------------------------

std::optional<std::string> Server::server_command(Connection& c, const Command& cmd,
                                                  const std::string& name) {
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
        ++metrics_.aof_rewrites;
    } catch (const std::exception& e) {
        ++metrics_.aof_rewrite_failures;
        return resp::error(std::string("AOF rewrite failed: ") + e.what());
    }
    return resp::simple_string("OK");
}

// A follower asks for our data. The "reply" is FLUSHALL + a full snapshot; from this moment on
// the connection also receives every change we make. Because the loop is single-threaded,
// nothing can change between building the snapshot and registering the follower.
// `SYNC <password>` is accepted too: an unauthenticated follower presents its password there.
std::string Server::cmd_sync(Connection& c, const Command& cmd) {
    if (cmd.size() > 2) {
        return resp::error("wrong number of arguments for 'SYNC' command");
    }
    std::string payload = resp::command({"FLUSHALL"});
    write_snapshot(store_, [&](std::string_view piece) { payload.append(piece); });
    if (payload.size() > kFollowerBufferLimit) {
        return resp::error("dataset too large to replicate");
    }
    c.role = Role::kFollower;
    followers_.push_back(c.sock.fd());
    ++metrics_.full_syncs_sent;
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
    out += "\r\n# Memory\r\n";
    out += "used_memory:" + std::to_string(store_.used_memory()) + "\r\n";
    out += "maxmemory:" + std::to_string(store_.maxmemory()) + "\r\n";
    out += std::string("maxmemory_policy:") + policy_name(store_.eviction_policy()) + "\r\n";
    out += "evicted_keys:" + std::to_string(store_.evicted_keys()) + "\r\n";
    out += "\r\n# Security\r\n";
    out += std::string("auth_required:") + (auth_.required() ? "yes" : "no") + "\r\n";
    out += "auth_failures_total:" + std::to_string(metrics_.auth_failures) + "\r\n";
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
    conn.accepted_at = std::chrono::steady_clock::now();
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

    c.out = leader_password_.empty() ? resp::command({"SYNC"})
                                     : resp::command({"SYNC", leader_password_});
    c.out_pos = 0;
    link_ = LinkState::kWaiting;
    if (!flush(c) || !update_interest(fd, c)) {
        close_connection(fd);
    }
}

}  // namespace emberdb
