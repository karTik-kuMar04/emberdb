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
