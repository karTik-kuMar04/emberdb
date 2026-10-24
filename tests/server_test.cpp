#include "emberdb/dispatcher.hpp"
#include "prom_check.hpp"
#include <vector>
#include "emberdb/resp_writer.hpp"
#include <filesystem>
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "emberdb/aof.hpp"
#include "emberdb/server.hpp"

namespace {

int dial(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    timeval tv{5, 0};  // never let a broken test hang forever
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

void send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::string recv_n(int fd, std::size_t n) {
    std::string out;
    char buf[4096];
    while (out.size() < n) {
        const ssize_t r = ::recv(fd, buf, std::min(sizeof(buf), n - out.size()), 0);
        if (r <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

std::string recv_until_eof(int fd) {
    std::string out;
    char buf[256];
    for (;;) {
        const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<emberdb::Server>(store_, 0);
        port_ = server_->port();
        thread_ = std::thread([this] { server_->run(); });
    }
    void TearDown() override {
        server_->stop();
        thread_.join();
    }

    emberdb::Store store_;
    std::unique_ptr<emberdb::Server> server_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

const std::string kPing = "*1\r\n$4\r\nPING\r\n";

}  // namespace

TEST_F(ServerTest, PingReturnsPong) {
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), kPing);
    EXPECT_EQ(recv_n(c.fd(), 7), "+PONG\r\n");
}

TEST_F(ServerTest, IdleClientDoesNotBlockOthers) {
    emberdb::Socket idle(dial(port_));  // connects and says nothing
    ASSERT_TRUE(idle.valid());
    emberdb::Socket busy(dial(port_));
    ASSERT_TRUE(busy.valid());
    send_all(busy.fd(), kPing);
    EXPECT_EQ(recv_n(busy.fd(), 7), "+PONG\r\n");  // the blocking server hung here
    send_all(idle.fd(), kPing);
    EXPECT_EQ(recv_n(idle.fd(), 7), "+PONG\r\n");
}

TEST_F(ServerTest, CommandSplitAcrossPackets) {
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "*1\r\n$4\r\nPI");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_all(c.fd(), "NG\r\n");
    EXPECT_EQ(recv_n(c.fd(), 7), "+PONG\r\n");
}

TEST_F(ServerTest, PipelinedCommandsAnswerInOrder) {
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    EXPECT_EQ(recv_n(c.fd(), 12), "+OK\r\n$1\r\nv\r\n");
}

TEST_F(ServerTest, ProtocolErrorGetsReplyThenClose) {
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "garbage\r\n");
    EXPECT_EQ(recv_until_eof(c.fd()), "-ERR Protocol error: expected '*' at start of command\r\n");
}

TEST_F(ServerTest, DataIsSharedBetweenConnections) {
    {
        emberdb::Socket a(dial(port_));
        send_all(a.fd(), "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nember\r\n");
        EXPECT_EQ(recv_n(a.fd(), 5), "+OK\r\n");
    }
    emberdb::Socket b(dial(port_));
    send_all(b.fd(), "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");
    EXPECT_EQ(recv_n(b.fd(), 11), "$5\r\nember\r\n");
}

TEST_F(ServerTest, LargeValueRoundTripExercisesPartialWrites) {
    const std::string value(8 * 1024 * 1024, 'x');
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" + std::to_string(value.size()) + "\r\n" +
                         value + "\r\n");
    EXPECT_EQ(recv_n(c.fd(), 5), "+OK\r\n");

    send_all(c.fd(), "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    const std::string expected = "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
    EXPECT_TRUE(recv_n(c.fd(), expected.size()) == expected);
}

TEST_F(ServerTest, KeyExpiresInRealTime) {
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    EXPECT_EQ(recv_n(c.fd(), 5), "+OK\r\n");
    send_all(c.fd(), "*3\r\n$7\r\nPEXPIRE\r\n$1\r\nk\r\n$2\r\n50\r\n");
    EXPECT_EQ(recv_n(c.fd(), 4), ":1\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    send_all(c.fd(), "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    EXPECT_EQ(recv_n(c.fd(), 5), "$-1\r\n");
}

TEST(ServerPersistence, DataSurvivesRestart) {
    char tpl[] = "/tmp/emberdb_srv_XXXXXX";
    const int tmp_fd = ::mkstemp(tpl);
    ASSERT_GE(tmp_fd, 0);
    ::close(tmp_fd);
    const std::string path = tpl;

    {
        emberdb::Store store;
        auto aof = emberdb::Aof::open(path, emberdb::Aof::Fsync::kAlways, store);
        emberdb::Server server(store, 0, aof.get());
        std::thread t([&] { server.run(); });
        emberdb::Socket c(dial(server.port()));
        ASSERT_TRUE(c.valid());
        send_all(c.fd(), "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nember\r\n");
        EXPECT_EQ(recv_n(c.fd(), 5), "+OK\r\n");
        server.stop();
        t.join();
    }
    {
        emberdb::Store store;
        auto aof = emberdb::Aof::open(path, emberdb::Aof::Fsync::kAlways, store);
        emberdb::Server server(store, 0, aof.get());
        std::thread t([&] { server.run(); });
        emberdb::Socket c(dial(server.port()));
        ASSERT_TRUE(c.valid());
        send_all(c.fd(), "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");
        EXPECT_EQ(recv_n(c.fd(), 11), "$5\r\nember\r\n");
        server.stop();
        t.join();
    }
    ::unlink(path.c_str());
}

TEST_F(ServerTest, RewriteAofWithoutAofIsAnError) {
    emberdb::Socket c(dial(port_));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "*1\r\n$10\r\nREWRITEAOF\r\n");
    const std::string expected = "-ERR AOF is not enabled\r\n";
    EXPECT_EQ(recv_n(c.fd(), expected.size()), expected);
}

TEST(ServerPersistence, RewriteCompactsLogAndKeepsData) {
    char tpl[] = "/tmp/emberdb_rw_XXXXXX";
    const int tmp_fd = ::mkstemp(tpl);
    ASSERT_GE(tmp_fd, 0);
    ::close(tmp_fd);
    const std::string path = tpl;

    std::uintmax_t before = 0;
    std::uintmax_t after = 0;
    {
        emberdb::Store store;
        auto aof = emberdb::Aof::open(path, emberdb::Aof::Fsync::kAlways, store);
        emberdb::Server server(store, 0, aof.get());
        std::thread t([&] { server.run(); });
        emberdb::Socket c(dial(server.port()));
        EXPECT_TRUE(c.valid());

        std::string batch;
        std::string expected_replies;
        for (int i = 0; i < 100; ++i) {
            batch += emberdb::resp::command({"SET", "k", "v" + std::to_string(i)});
            expected_replies += "+OK\r\n";
        }
        send_all(c.fd(), batch);
        EXPECT_EQ(recv_n(c.fd(), expected_replies.size()), expected_replies);
        before = std::filesystem::file_size(path);

        send_all(c.fd(), "*1\r\n$10\r\nREWRITEAOF\r\n");
        EXPECT_EQ(recv_n(c.fd(), 5), "+OK\r\n");
        after = std::filesystem::file_size(path);

        server.stop();
        t.join();
    }
    EXPECT_LT(after, before);

    {
        emberdb::Store store;
        auto aof = emberdb::Aof::open(path, emberdb::Aof::Fsync::kAlways, store);
        emberdb::Server server(store, 0, aof.get());
        std::thread t([&] { server.run(); });
        emberdb::Socket c(dial(server.port()));
        EXPECT_TRUE(c.valid());
        send_all(c.fd(), "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
        EXPECT_EQ(recv_n(c.fd(), 9), "$3\r\nv99\r\n");
        server.stop();
        t.join();
    }
    ::unlink(path.c_str());
}

namespace {

// Sends one command on a fresh connection and returns the first chunk of the reply.
std::string ask(std::uint16_t port, const emberdb::Command& cmd) {
    emberdb::Socket c(dial(port));
    if (!c.valid()) {
        return "";
    }
    send_all(c.fd(), emberdb::resp::command(cmd));
    char buf[4096];
    const ssize_t n = ::recv(c.fd(), buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

// Polls until `predicate` is true. Replication is asynchronous, so tests wait for it.
template <typename Predicate>
bool eventually(Predicate predicate, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

class ReplicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        leader_ = std::make_unique<emberdb::Server>(leader_store_, 0);
        follower_ = std::make_unique<emberdb::Server>(follower_store_, 0);
        leader_thread_ = std::thread([this] { leader_->run(); });
        follower_thread_ = std::thread([this] { follower_->run(); });
    }
    void TearDown() override {
        if (leader_) {
            leader_->stop();
        }
        if (follower_) {
            follower_->stop();
        }
        if (leader_thread_.joinable()) {
            leader_thread_.join();
        }
        if (follower_thread_.joinable()) {
            follower_thread_.join();
        }
    }

    std::uint16_t leader_port() const { return leader_->port(); }
    std::uint16_t follower_port() const { return follower_->port(); }

    // Points the follower at the leader and waits until data is flowing.
    void link() {
        ASSERT_EQ(ask(follower_port(), {"REPLICAOF", "127.0.0.1", std::to_string(leader_port())}),
                  "+OK\r\n");
        ASSERT_TRUE(eventually([&] {
            return contains(ask(follower_port(), {"INFO"}), "master_link_status:up");
        }));
    }

    emberdb::Store leader_store_;
    emberdb::Store follower_store_;
    std::unique_ptr<emberdb::Server> leader_;
    std::unique_ptr<emberdb::Server> follower_;
    std::thread leader_thread_;
    std::thread follower_thread_;
};

}  // namespace

TEST_F(ReplicationTest, FollowerReceivesExistingData) {
    EXPECT_EQ(ask(leader_port(), {"SET", "a", "1"}), "+OK\r\n");
    EXPECT_EQ(ask(leader_port(), {"SET", "b", "2"}), "+OK\r\n");
    EXPECT_EQ(ask(leader_port(), {"SET", "t", "x"}), "+OK\r\n");
    EXPECT_EQ(ask(leader_port(), {"EXPIRE", "t", "1000"}), ":1\r\n");
    link();
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "a"}) == "$1\r\n1\r\n"; }));
    EXPECT_EQ(ask(follower_port(), {"GET", "b"}), "$1\r\n2\r\n");
    const std::string ttl = ask(follower_port(), {"TTL", "t"});
    EXPECT_NE(ttl, ":-1\r\n");  // the deadline was carried over
    EXPECT_NE(ttl, ":-2\r\n");
}

TEST_F(ReplicationTest, LiveWritesAreStreamed) {
    link();
    EXPECT_EQ(ask(leader_port(), {"SET", "k", "v"}), "+OK\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "k"}) == "$1\r\nv\r\n"; }));
    EXPECT_EQ(ask(leader_port(), {"INCR", "n"}), ":1\r\n");
    EXPECT_EQ(ask(leader_port(), {"INCR", "n"}), ":2\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "n"}) == "$1\r\n2\r\n"; }));
    EXPECT_EQ(ask(leader_port(), {"DEL", "k"}), ":1\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "k"}) == "$-1\r\n"; }));
}

TEST_F(ReplicationTest, FollowerIsReadOnlyForClients) {
    link();
    EXPECT_EQ(ask(follower_port(), {"SET", "x", "1"}),
              "-ERR You can't write against a read only replica.\r\n");
    EXPECT_EQ(ask(follower_port(), {"GET", "x"}), "$-1\r\n");
    EXPECT_EQ(ask(follower_port(), {"PING"}), "+PONG\r\n");
}

TEST_F(ReplicationTest, LeaderExpiryRemovesKeyOnFollower) {
    link();
    EXPECT_EQ(ask(leader_port(), {"SET", "k", "v"}), "+OK\r\n");
    EXPECT_EQ(ask(leader_port(), {"PEXPIRE", "k", "500"}), ":1\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"DBSIZE"}) == ":1\r\n"; }));
    // The follower never purges on its own: only the leader's DEL brings the count to zero.
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"DBSIZE"}) == ":0\r\n"; }));
    EXPECT_EQ(ask(follower_port(), {"GET", "k"}), "$-1\r\n");
}

TEST_F(ReplicationTest, ResyncReplacesStaleLocalData) {
    EXPECT_EQ(ask(follower_port(), {"SET", "stale", "1"}), "+OK\r\n");  // not a follower yet
    EXPECT_EQ(ask(leader_port(), {"SET", "fresh", "1"}), "+OK\r\n");
    link();
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "fresh"}) == "$1\r\n1\r\n"; }));
    EXPECT_EQ(ask(follower_port(), {"GET", "stale"}), "$-1\r\n");
}

TEST_F(ReplicationTest, FollowerReconnectsAfterLeaderRestart) {
    link();
    EXPECT_EQ(ask(leader_port(), {"SET", "a", "1"}), "+OK\r\n");
    ASSERT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "a"}) == "$1\r\n1\r\n"; }));

    const std::uint16_t port = leader_port();
    leader_->stop();
    leader_thread_.join();
    leader_.reset();

    EXPECT_TRUE(eventually([&] {
        return contains(ask(follower_port(), {"INFO"}), "master_link_status:down");
    }));
    EXPECT_EQ(ask(follower_port(), {"GET", "a"}), "$1\r\n1\r\n");  // stale reads keep working

    leader_ = std::make_unique<emberdb::Server>(leader_store_, port);
    leader_thread_ = std::thread([this] { leader_->run(); });
    EXPECT_EQ(ask(port, {"SET", "b", "2"}), "+OK\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "b"}) == "$1\r\n2\r\n"; }, 10000));
    EXPECT_EQ(ask(follower_port(), {"GET", "a"}), "$1\r\n1\r\n");
}

TEST_F(ReplicationTest, ReplicaofNoOnePromotesFollower) {
    link();
    EXPECT_EQ(ask(leader_port(), {"SET", "a", "1"}), "+OK\r\n");
    ASSERT_TRUE(eventually([&] { return ask(follower_port(), {"GET", "a"}) == "$1\r\n1\r\n"; }));

    EXPECT_EQ(ask(follower_port(), {"REPLICAOF", "NO", "ONE"}), "+OK\r\n");
    EXPECT_EQ(ask(follower_port(), {"SET", "x", "1"}), "+OK\r\n");  // writable again
    EXPECT_TRUE(contains(ask(follower_port(), {"INFO"}), "role:master"));
    EXPECT_EQ(ask(follower_port(), {"GET", "a"}), "$1\r\n1\r\n");  // keeps its data
    EXPECT_TRUE(eventually([&] {
        return contains(ask(leader_port(), {"INFO"}), "connected_slaves:0");
    }));

    EXPECT_EQ(ask(leader_port(), {"SET", "b", "2"}), "+OK\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(ask(follower_port(), {"GET", "b"}), "$-1\r\n");  // no longer follows the old leader
}

TEST_F(ReplicationTest, ReplicaofRejectsBadArguments) {
    EXPECT_EQ(ask(follower_port(), {"REPLICAOF", "127.0.0.1", "abc"}), "-ERR invalid port\r\n");
    EXPECT_EQ(ask(follower_port(), {"REPLICAOF", "not-an-ip", "6379"}),
              "-ERR invalid host (use an IPv4 address or localhost)\r\n");
    EXPECT_EQ(ask(leader_port(), {"REPLICAOF", "127.0.0.1", std::to_string(leader_port())}),
              "-ERR a server cannot replicate from itself\r\n");
    EXPECT_EQ(ask(leader_port(), {"REPLICAOF", "127.0.0.1"}),
              "-ERR wrong number of arguments for 'REPLICAOF' command\r\n");
}

TEST_F(ReplicationTest, InfoShowsRolesAndFollowers) {
    const std::string before = ask(leader_port(), {"INFO"});
    EXPECT_TRUE(contains(before, "role:master"));
    EXPECT_TRUE(contains(before, "connected_slaves:0"));

    link();
    EXPECT_TRUE(eventually([&] {
        return contains(ask(leader_port(), {"INFO"}), "connected_slaves:1");
    }));
    EXPECT_TRUE(eventually([&] {
        return contains(ask(leader_port(), {"INFO"}), "slave0:pending_bytes=0");
    }));
    const std::string info = ask(follower_port(), {"INFO"});
    EXPECT_TRUE(contains(info, "role:slave"));
    EXPECT_TRUE(contains(info, "master_host:127.0.0.1"));
    EXPECT_TRUE(contains(info, "master_port:" + std::to_string(leader_port())));
}

TEST(ServerEviction, LruKeepsMemoryBoundedAndInfoReportsIt) {
    constexpr std::size_t kLimit = 20 * 1024;
    emberdb::Store store;
    store.set_maxmemory(kLimit);
    store.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    emberdb::Server server(store, 0);
    std::thread t([&] { server.run(); });

    const std::string value(1000, 'x');
    emberdb::Socket c(dial(server.port()));
    EXPECT_TRUE(c.valid());
    std::string batch;
    std::string expected;
    for (int i = 0; i < 200; ++i) {
        batch += emberdb::resp::command({"SET", "key" + std::to_string(i), value});
        expected += "+OK\r\n";
    }
    send_all(c.fd(), batch);
    EXPECT_EQ(recv_n(c.fd(), expected.size()), expected);

    send_all(c.fd(), emberdb::resp::command({"GET", "key199"}));
    const std::string newest = "$1000\r\n" + value + "\r\n";
    EXPECT_EQ(recv_n(c.fd(), newest.size()), newest);  // the newest key survived
    send_all(c.fd(), emberdb::resp::command({"GET", "key0"}));
    EXPECT_EQ(recv_n(c.fd(), 5), "$-1\r\n");  // the oldest was evicted

    const std::string info = ask(server.port(), {"INFO"});
    EXPECT_TRUE(contains(info, "maxmemory:20480"));
    EXPECT_TRUE(contains(info, "maxmemory_policy:allkeys-lru"));
    EXPECT_FALSE(contains(info, "evicted_keys:0\r\n"));

    server.stop();
    t.join();
    EXPECT_GT(store.evicted_keys(), 100U);
    EXPECT_LE(store.used_memory(), kLimit + 2048);  // at most one command over the limit
    EXPECT_TRUE(store.check_invariants());
}

TEST(ServerEviction, NoEvictionRepliesOomWhenFull) {
    emberdb::Store store;
    store.set_maxmemory(100);  // default policy: noeviction
    emberdb::Server server(store, 0);
    std::thread t([&] { server.run(); });
    const std::uint16_t port = server.port();

    const std::string value(1000, 'x');
    EXPECT_EQ(ask(port, {"SET", "a", value}), "+OK\r\n");  // the store was empty, so nothing to refuse yet
    EXPECT_EQ(ask(port, {"SET", "b", value}),
              "-OOM command not allowed when used memory > 'maxmemory'.\r\n");
    EXPECT_EQ(ask(port, {"GET", "a"}), "$1000\r\n" + value + "\r\n");  // reads still work
    EXPECT_EQ(ask(port, {"DEL", "a"}), ":1\r\n");
    EXPECT_EQ(ask(port, {"SET", "b", value}), "+OK\r\n");  // space was freed

    server.stop();
    t.join();
}

#include <cerrno>

#include "emberdb/security.hpp"

namespace {

// Sends one command on an existing connection and returns the first chunk of the reply.
std::string call(int fd, const emberdb::Command& cmd) {
    send_all(fd, emberdb::resp::command(cmd));
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

// Opens a connection and authenticates it.
emberdb::Socket login(std::uint16_t port, const std::string& password) {
    emberdb::Socket c(dial(port));
    if (c.valid()) {
        call(c.fd(), {"AUTH", password});
    }
    return c;
}

const std::string kNoAuth = "-NOAUTH Authentication required.\r\n";
const std::string kWrongPass = "-WRONGPASS invalid password\r\n";

class AuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<emberdb::Server>(store_, 0);
        server_->set_password("s3cret");
        server_->set_auth_timeout(std::chrono::milliseconds(600));
        thread_ = std::thread([this] { server_->run(); });
    }
    void TearDown() override {
        server_->stop();
        thread_.join();
    }
    std::uint16_t port() const { return server_->port(); }

    emberdb::Store store_;
    std::unique_ptr<emberdb::Server> server_;
    std::thread thread_;
};

}  // namespace

TEST_F(AuthTest, NoCommandWorksBeforeAuth) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(call(c.fd(), {"PING"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"GET", "k"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"SET", "k", "v"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"INFO"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"FLUSHALL"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"REPLICAOF", "NO", "ONE"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"REWRITEAOF"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"SYNC"}), kNoAuth);
}

TEST_F(AuthTest, WrongPasswordIsRejected) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(call(c.fd(), {"AUTH", "nope"}), kWrongPass);
    EXPECT_EQ(call(c.fd(), {"PING"}), kNoAuth);
}

TEST_F(AuthTest, CorrectPasswordUnlocksTheConnection) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(call(c.fd(), {"SET", "k", "v"}), kNoAuth);
    EXPECT_EQ(call(c.fd(), {"AUTH", "s3cret"}), "+OK\r\n");
    EXPECT_EQ(call(c.fd(), {"PING"}), "+PONG\r\n");
    EXPECT_EQ(call(c.fd(), {"GET", "k"}), "$-1\r\n");  // the refused SET never ran
    EXPECT_EQ(call(c.fd(), {"SET", "k", "v"}), "+OK\r\n");
    EXPECT_EQ(call(c.fd(), {"GET", "k"}), "$1\r\nv\r\n");
}

TEST_F(AuthTest, AuthDoesNotCarryOverToOtherConnections) {
    emberdb::Socket a = login(port(), "s3cret");
    emberdb::Socket b(dial(port()));
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_EQ(call(a.fd(), {"PING"}), "+PONG\r\n");
    EXPECT_EQ(call(b.fd(), {"PING"}), kNoAuth);
}

TEST_F(AuthTest, ThreeWrongPasswordsCloseTheConnection) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(call(c.fd(), {"AUTH", "a"}), kWrongPass);
    EXPECT_EQ(call(c.fd(), {"AUTH", "b"}), kWrongPass);
    EXPECT_EQ(call(c.fd(), {"AUTH", "c"}), kWrongPass);
    char byte = 0;
    EXPECT_EQ(::recv(c.fd(), &byte, 1, 0), 0);  // the server hung up
}

TEST_F(AuthTest, AuthChecksArgumentCount) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    const std::string expected = "-ERR wrong number of arguments for 'AUTH' command\r\n";
    EXPECT_EQ(call(c.fd(), {"AUTH"}), expected);
    EXPECT_EQ(call(c.fd(), {"AUTH", "a", "b"}), expected);
}

TEST_F(AuthTest, GlobalThrottleBlocksGuessing) {
    // Ten wrong guesses (three per connection) use up the budget for this one-second window.
    int guesses = 0;
    while (guesses < 10) {
        emberdb::Socket c(dial(port()));
        ASSERT_TRUE(c.valid());
        for (int i = 0; i < 3 && guesses < 10; ++i, ++guesses) {
            EXPECT_EQ(call(c.fd(), {"AUTH", "bad"}), kWrongPass);
        }
    }
    emberdb::Socket blocked(dial(port()));
    ASSERT_TRUE(blocked.valid());
    // Even the right password is refused during the lockout.
    EXPECT_EQ(call(blocked.fd(), {"AUTH", "s3cret"}),
              "-ERR too many authentication failures, try again later\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    emberdb::Socket later(dial(port()));
    ASSERT_TRUE(later.valid());
    EXPECT_EQ(call(later.fd(), {"AUTH", "s3cret"}), "+OK\r\n");
}

TEST_F(AuthTest, IdleUnauthenticatedConnectionIsClosed) {
    emberdb::Socket idle(dial(port()));
    ASSERT_TRUE(idle.valid());
    emberdb::Socket authed = login(port(), "s3cret");
    ASSERT_TRUE(authed.valid());
    char byte = 0;
    EXPECT_EQ(::recv(idle.fd(), &byte, 1, 0), 0);  // closed by the server after the timeout
    EXPECT_EQ(call(authed.fd(), {"PING"}), "+PONG\r\n");  // the authenticated one is left alone
}

TEST_F(AuthTest, OversizedPreAuthInputIsRejected) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    send_all(c.fd(), "*1\r\n$100000\r\n" + std::string(20000, 'x'));
    std::string got;
    char buf[256];
    ssize_t n = 0;
    while ((n = ::recv(c.fd(), buf, sizeof(buf), 0)) > 0) {
        got.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_TRUE(n == 0 || errno == ECONNRESET);  // closed, cleanly or by a reset
    EXPECT_TRUE(got.empty() || got == "-ERR too much data before authentication\r\n");
}

TEST_F(AuthTest, SyncCanCarryThePassword) {
    emberdb::Socket bad(dial(port()));
    ASSERT_TRUE(bad.valid());
    EXPECT_EQ(call(bad.fd(), {"SYNC", "wrong"}), kWrongPass);

    emberdb::Socket good(dial(port()));
    ASSERT_TRUE(good.valid());
    send_all(good.fd(), emberdb::resp::command({"SYNC", "s3cret"}));
    EXPECT_EQ(recv_n(good.fd(), 18), "*1\r\n$8\r\nFLUSHALL\r\n");  // the stream starts
}

TEST_F(AuthTest, InfoReportsAuthState) {
    emberdb::Socket c(dial(port()));
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(call(c.fd(), {"AUTH", "wrong"}), kWrongPass);
    EXPECT_EQ(call(c.fd(), {"AUTH", "s3cret"}), "+OK\r\n");
    const std::string info = call(c.fd(), {"INFO"});
    EXPECT_TRUE(contains(info, "auth_required:yes"));
    EXPECT_TRUE(contains(info, "auth_failures_total:1"));
}

TEST(ServerSecurity, AuthWithoutPasswordConfiguredIsAnError) {
    emberdb::Store store;
    emberdb::Server server(store, 0);
    std::thread t([&] { server.run(); });
    EXPECT_EQ(ask(server.port(), {"AUTH", "x"}),
              "-ERR AUTH called without any password configured\r\n");
    EXPECT_EQ(ask(server.port(), {"PING"}), "+PONG\r\n");  // no password means no gate
    server.stop();
    t.join();
}

TEST(ServerSecurity, InvalidBindAddressThrows) {
    emberdb::Store store;
    EXPECT_THROW({ emberdb::Server server(store, 0, nullptr, "not-an-ip"); }, std::runtime_error);
}

TEST(ServerSecurity, BindAddressIsHonored) {
    emberdb::Store store;
    emberdb::Server server(store, 0, nullptr, "127.0.0.2");
    std::thread t([&] { server.run(); });

    EXPECT_LT(dial(server.port()), 0);  // 127.0.0.1 is not the address we listen on

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.port());
    ::inet_pton(AF_INET, "127.0.0.2", &addr.sin_addr);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ::close(fd);

    server.stop();
    t.join();
}

namespace {

class ReplicationAuthTest : public ::testing::Test {
protected:
    void start(const std::string& leader_password, const std::string& masterauth) {
        leader_ = std::make_unique<emberdb::Server>(leader_store_, 0);
        leader_->set_password(leader_password);
        follower_ = std::make_unique<emberdb::Server>(follower_store_, 0);
        follower_->set_leader_password(masterauth);
        leader_thread_ = std::thread([this] { leader_->run(); });
        follower_thread_ = std::thread([this] { follower_->run(); });
    }
    void TearDown() override {
        if (leader_) {
            leader_->stop();
        }
        if (follower_) {
            follower_->stop();
        }
        if (leader_thread_.joinable()) {
            leader_thread_.join();
        }
        if (follower_thread_.joinable()) {
            follower_thread_.join();
        }
    }
    std::string follow() {
        return ask(follower_->port(),
                   {"REPLICAOF", "127.0.0.1", std::to_string(leader_->port())});
    }

    emberdb::Store leader_store_;
    emberdb::Store follower_store_;
    std::unique_ptr<emberdb::Server> leader_;
    std::unique_ptr<emberdb::Server> follower_;
    std::thread leader_thread_;
    std::thread follower_thread_;
};

}  // namespace

TEST_F(ReplicationAuthTest, FollowerWithTheRightPasswordSyncs) {
    start("lead-pw", "lead-pw");
    emberdb::Socket l = login(leader_->port(), "lead-pw");
    ASSERT_TRUE(l.valid());
    EXPECT_EQ(call(l.fd(), {"SET", "a", "1"}), "+OK\r\n");

    EXPECT_EQ(follow(), "+OK\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_->port(), {"GET", "a"}) == "$1\r\n1\r\n"; }));
    EXPECT_EQ(call(l.fd(), {"SET", "b", "2"}), "+OK\r\n");
    EXPECT_TRUE(eventually([&] { return ask(follower_->port(), {"GET", "b"}) == "$1\r\n2\r\n"; }));
}

TEST_F(ReplicationAuthTest, FollowerWithTheWrongPasswordIsRefused) {
    start("lead-pw", "not-the-password");
    EXPECT_EQ(follow(), "+OK\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // long enough for a retry

    EXPECT_TRUE(contains(ask(follower_->port(), {"INFO"}), "master_link_status:down"));
    emberdb::Socket l = login(leader_->port(), "lead-pw");
    ASSERT_TRUE(l.valid());
    EXPECT_TRUE(contains(call(l.fd(), {"INFO"}), "connected_slaves:0"));
}

namespace {

// A server running on its own thread, with its own store.
class Node {
public:
    Node() : server_(store_, 0) {}
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() { stop(); }

    emberdb::Store& store() { return store_; }
    emberdb::Server& server() { return server_; }
    void start() { thread_ = std::thread([this] { server_.run(); }); }
    void stop() {
        if (thread_.joinable()) {
            server_.stop();
            thread_.join();
        }
    }

private:
    emberdb::Store store_;
    emberdb::Server server_;
    std::thread thread_;
};

std::string http_exchange(std::uint16_t port, const std::string& request) {
    emberdb::Socket c(dial(port));
    if (!c.valid()) {
        return "";
    }
    send_all(c.fd(), request);
    return recv_until_eof(c.fd());
}

std::string http_get(std::uint16_t port, const std::string& path) {
    return http_exchange(port, "GET " + path + " HTTP/1.1\r\nHost: test\r\n\r\n");
}

std::string body_of(const std::string& response) {
    const std::size_t pos = response.find("\r\n\r\n");
    return pos == std::string::npos ? std::string() : response.substr(pos + 4);
}

// The value of one series in a scrape, or -1 if it is not there.
double metric_value(const std::string& body, const std::string& series) {
    const std::string prefix = series + " ";
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with(prefix)) {
            return std::strtod(line.c_str() + prefix.size(), nullptr);
        }
    }
    return -1.0;
}

std::string scrape(std::uint16_t metrics_port) {
    return body_of(http_get(metrics_port, "/metrics"));
}

}  // namespace

TEST(MetricsEndpoint, ServesHealthAndMetrics) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::uint16_t port = n.server().metrics_port();

    const std::string health = http_get(port, "/healthz");
    EXPECT_TRUE(health.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(body_of(health), "ok\n");
    EXPECT_TRUE(http_get(port, "/readyz").starts_with("HTTP/1.1 200 OK\r\n"));

    const std::string response = http_get(port, "/metrics");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(contains(response, "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"));
    const std::string body = body_of(response);
    EXPECT_TRUE(contains(response, "Content-Length: " + std::to_string(body.size()) + "\r\n"));
    EXPECT_EQ(check_exposition(body), "");
    EXPECT_EQ(metric_value(body, "emberdb_build_info{version=\"" + std::string(emberdb::kVersion) + "\"}"), 1.0);
    EXPECT_EQ(metric_value(body, "emberdb_metrics_scrapes_total"), 1.0);
}

TEST(MetricsEndpoint, CountsCommandsByName) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    EXPECT_EQ(ask(n.server().port(), {"SET", "a", "1"}), "+OK\r\n");
    EXPECT_EQ(ask(n.server().port(), {"GET", "a"}), "$1\r\n1\r\n");
    EXPECT_EQ(ask(n.server().port(), {"GET", "a"}), "$1\r\n1\r\n");
    EXPECT_EQ(ask(n.server().port(), {"FOO"}), "-ERR unknown command 'FOO'\r\n");

    const std::string body = scrape(n.server().metrics_port());
    EXPECT_EQ(check_exposition(body), "");
    EXPECT_EQ(metric_value(body, "emberdb_commands_processed_total{command=\"SET\"}"), 1.0);
    EXPECT_EQ(metric_value(body, "emberdb_commands_processed_total{command=\"GET\"}"), 2.0);
    EXPECT_EQ(metric_value(body, "emberdb_commands_processed_total{command=\"other\"}"), 1.0);
    EXPECT_EQ(metric_value(body, "emberdb_command_errors_total"), 1.0);
    EXPECT_EQ(metric_value(body, "emberdb_connections_accepted_total"), 4.0);
    EXPECT_EQ(metric_value(body, "emberdb_command_duration_seconds_count"), 4.0);
    EXPECT_EQ(metric_value(body, "emberdb_keys"), 1.0);
    EXPECT_GT(metric_value(body, "emberdb_network_read_bytes_total"), 0.0);
    EXPECT_GT(metric_value(body, "emberdb_network_written_bytes_total"), 0.0);
    EXPECT_GT(metric_value(body, "emberdb_event_loop_busy_seconds_total"), 0.0);
}

TEST(MetricsEndpoint, EveryKnownCommandHasItsOwnSeries) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::string body = scrape(n.server().metrics_port());
    std::vector<std::string> names = emberdb::command_names();
    for (const char* extra : {"AUTH", "SYNC", "REPLICAOF", "INFO", "REWRITEAOF"}) {
        names.emplace_back(extra);
    }
    for (const std::string& name : names) {
        EXPECT_GE(metric_value(body, "emberdb_commands_processed_total{command=\"" + name + "\"}"),
                  0.0)
            << name;
    }
}

TEST(MetricsEndpoint, UnknownCommandsDoNotCreateSeries) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::string before = scrape(n.server().metrics_port());

    emberdb::Socket c(dial(n.server().port()));
    ASSERT_TRUE(c.valid());
    for (int i = 0; i < 50; ++i) {
        call(c.fd(), {"JUNKCOMMAND" + std::to_string(i)});
    }
    const std::string after = scrape(n.server().metrics_port());
    EXPECT_FALSE(contains(after, "JUNKCOMMAND"));
    EXPECT_EQ(metric_value(after, "emberdb_commands_processed_total{command=\"other\"}"), 50.0);
    EXPECT_EQ(metric_value(after, "emberdb_command_errors_total"), 50.0);
    const auto count_series = [](const std::string& text) {
        std::size_t lines = 0;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            lines += line.starts_with("emberdb_commands_processed_total{") ? 1 : 0;
        }
        return lines;
    };
    EXPECT_EQ(count_series(before), count_series(after));
}

TEST(MetricsEndpoint, LatencyHistogramCountsEveryCommand) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    emberdb::Socket c(dial(n.server().port()));
    ASSERT_TRUE(c.valid());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(call(c.fd(), {"PING"}), "+PONG\r\n");
    }
    const std::string body = scrape(n.server().metrics_port());
    EXPECT_EQ(check_exposition(body), "");  // buckets are cumulative and match _count
    EXPECT_EQ(metric_value(body, "emberdb_command_duration_seconds_count"), 10.0);
    EXPECT_EQ(metric_value(body, "emberdb_command_duration_seconds_bucket{le=\"+Inf\"}"), 10.0);
    EXPECT_EQ(metric_value(body, "emberdb_commands_processed_total{command=\"PING\"}"), 10.0);
}

TEST(MetricsEndpoint, AnswersErrorsAndIgnoresQueryStrings) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::uint16_t port = n.server().metrics_port();

    EXPECT_TRUE(http_get(port, "/nope").starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(http_get(port, "/healthz?probe=1").starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(http_get(port, "/metrics?x=1").starts_with("HTTP/1.1 200 OK\r\n"));

    const std::string post = http_exchange(port, "POST /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(post.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_TRUE(contains(post, "Allow: GET\r\n"));

    EXPECT_TRUE(http_exchange(port, "garbage\r\n\r\n").starts_with("HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_TRUE(http_exchange(port, "GET /metrics HTTP/1.1\r\nX-Pad: " + std::string(9000, 'a'))
                    .starts_with("HTTP/1.1 431 "));
}

TEST(MetricsEndpoint, SlowHttpClientsAreCutOff) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.server().set_http_timeout(std::chrono::milliseconds(300));
    n.start();
    emberdb::Socket slow(dial(n.server().metrics_port()));
    ASSERT_TRUE(slow.valid());
    send_all(slow.fd(), "GET /metrics HTT");  // never finishes its request
    char byte = 0;
    EXPECT_EQ(::recv(slow.fd(), &byte, 1, 0), 0);  // the server hung up on it
    EXPECT_EQ(ask(n.server().port(), {"PING"}), "+PONG\r\n");  // and the database never noticed
}

TEST(MetricsEndpoint, TooManyHttpConnectionsAreRefused) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::uint16_t port = n.server().metrics_port();

    std::vector<emberdb::Socket> idle;
    for (int i = 0; i < 16; ++i) {
        idle.push_back(emberdb::Socket(dial(port)));
        ASSERT_TRUE(idle.back().valid());
    }
    emberdb::Socket extra(dial(port));
    ASSERT_TRUE(extra.valid());
    char byte = 0;
    EXPECT_EQ(::recv(extra.fd(), &byte, 1, 0), 0);  // the 17th is closed straight away

    idle.pop_back();  // free one slot
    EXPECT_TRUE(eventually([&] { return http_get(port, "/healthz").starts_with("HTTP/1.1 200"); }));
    EXPECT_EQ(ask(n.server().port(), {"PING"}), "+PONG\r\n");
}

TEST(MetricsEndpoint, ReportsAuthActivityAndNeedsNoPassword) {
    Node n;
    n.server().set_password("s3cret");
    n.server().set_auth_timeout(std::chrono::milliseconds(300));
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::uint16_t mport = n.server().metrics_port();

    emberdb::Socket c(dial(n.server().port()));
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(call(c.fd(), {"AUTH", "wrong"}), kWrongPass);
    EXPECT_EQ(call(c.fd(), {"AUTH", "s3cret"}), "+OK\r\n");  // this one logs in, so it survives
    emberdb::Socket idle(dial(n.server().port()));            // this one never does
    ASSERT_TRUE(idle.valid());

    ASSERT_TRUE(eventually([&] {
        return metric_value(scrape(mport), "emberdb_connections_dropped_total{reason=\"auth_timeout\"}") == 1.0;
    }));
    const std::string body = scrape(mport);  // no password was needed for any of these
    EXPECT_EQ(metric_value(body, "emberdb_auth_required"), 1.0);
    EXPECT_EQ(metric_value(body, "emberdb_auth_failures_total"), 1.0);
    EXPECT_EQ(call(c.fd(), {"PING"}), "+PONG\r\n");
}

TEST(MetricsEndpoint, CountsExpiredAndEvictedKeys) {
    Node n;
    n.store().set_maxmemory(3000);
    n.store().set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::uint16_t mport = n.server().metrics_port();

    EXPECT_EQ(ask(n.server().port(), {"SET", "ttl", "v"}), "+OK\r\n");
    EXPECT_EQ(ask(n.server().port(), {"PEXPIRE", "ttl", "50"}), ":1\r\n");
    ASSERT_TRUE(eventually([&] { return metric_value(scrape(mport), "emberdb_keys_expired_total") == 1.0; }));

    const std::string big(1000, 'x');
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(ask(n.server().port(), {"SET", "k" + std::to_string(i), big}), "+OK\r\n");
    }
    const std::string body = scrape(mport);
    EXPECT_GT(metric_value(body, "emberdb_keys_evicted_total"), 0.0);
    EXPECT_EQ(metric_value(body, "emberdb_memory_limit_bytes"), 3000.0);
    EXPECT_GT(metric_value(body, "emberdb_memory_used_bytes"), 0.0);
}

TEST(MetricsEndpoint, ReportsReplication) {
    Node leader;
    Node follower;
    leader.server().enable_metrics("127.0.0.1", 0);
    follower.server().enable_metrics("127.0.0.1", 0);
    leader.start();
    follower.start();
    const std::uint16_t lm = leader.server().metrics_port();
    const std::uint16_t fm = follower.server().metrics_port();

    EXPECT_EQ(metric_value(scrape(fm), "emberdb_replication_follower"), 0.0);
    ASSERT_EQ(ask(follower.server().port(),
                  {"REPLICAOF", "127.0.0.1", std::to_string(leader.server().port())}),
              "+OK\r\n");
    ASSERT_TRUE(eventually([&] { return metric_value(scrape(fm), "emberdb_replication_link_up") == 1.0; }));
    EXPECT_TRUE(http_get(fm, "/readyz").starts_with("HTTP/1.1 200 OK\r\n"));

    EXPECT_EQ(ask(leader.server().port(), {"SET", "a", "1"}), "+OK\r\n");
    ASSERT_TRUE(eventually([&] {
        return metric_value(scrape(fm), "emberdb_replication_commands_applied_total") >= 2.0;
    }));  // the FLUSHALL that starts every sync, plus the SET

    const std::string lbody = scrape(lm);
    EXPECT_EQ(metric_value(lbody, "emberdb_replication_connected_followers"), 1.0);
    EXPECT_EQ(metric_value(lbody, "emberdb_replication_full_syncs_sent_total"), 1.0);
    EXPECT_EQ(metric_value(scrape(fm), "emberdb_replication_follower"), 1.0);
}

TEST(MetricsEndpoint, FollowerIsNotReadyWithoutALeader) {
    Node n;
    n.server().enable_metrics("127.0.0.1", 0);
    n.start();
    const std::uint16_t mport = n.server().metrics_port();

    EXPECT_EQ(ask(n.server().port(), {"REPLICAOF", "127.0.0.1", "1"}), "+OK\r\n");  // nobody listens there
    EXPECT_TRUE(http_get(mport, "/readyz").starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_TRUE(http_get(mport, "/healthz").starts_with("HTTP/1.1 200 OK\r\n"));  // alive, just not ready

    EXPECT_EQ(ask(n.server().port(), {"REPLICAOF", "NO", "ONE"}), "+OK\r\n");
    EXPECT_TRUE(eventually([&] { return http_get(mport, "/readyz").starts_with("HTTP/1.1 200 OK"); }));
}

TEST(MetricsEndpoint, EnableMetricsValidatesItsArguments) {
    emberdb::Store store;
    emberdb::Server server(store, 0);
    EXPECT_THROW(server.enable_metrics("not-an-ip", 0), std::runtime_error);
    EXPECT_THROW(server.enable_metrics("127.0.0.1", server.port()), std::runtime_error);  // taken
    server.enable_metrics("127.0.0.1", 0);
    EXPECT_NE(server.metrics_port(), 0);
    EXPECT_THROW(server.enable_metrics("127.0.0.1", 0), std::runtime_error);  // only once
}
