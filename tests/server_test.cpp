#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

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
