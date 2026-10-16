#include "emberdb/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "emberdb/dispatcher.hpp"
#include "emberdb/resp_parser.hpp"
#include "emberdb/resp_writer.hpp"

namespace emberdb {
namespace {

constexpr int kBacklog = 128;
constexpr std::size_t kMaxBufferBytes = 64UL * 1024 * 1024;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

// send() may write only part of the data, so loop until all of it is out.
bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

Server::Server(Store& store, std::uint16_t port) : store_(store) {
    listener_ = Socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listener_.valid()) {
        fail("socket");
    }

    // Lets us restart the server immediately without "address already in use".
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
}

void Server::run() {
    for (;;) {
        Socket client(::accept(listener_.fd(), nullptr, nullptr));
        if (!client.valid()) {
            if (errno != EINTR) {
                std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            }
            continue;
        }
        handle_client(std::move(client));  // closed automatically on return
    }
}

void Server::handle_client(Socket client) {
    const int fd = client.fd();
    std::string buffer;
    std::array<char, 4096> chunk{};

    for (;;) {
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {
            return;  // client closed the connection
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(n));

        if (buffer.size() > kMaxBufferBytes) {
            static_cast<void>(send_all(fd, resp::error("input buffer too large")));
            return;
        }

        // The buffer may hold 0, 1, or many commands: drain every complete one.
        while (!buffer.empty()) {
            ParseResult result = parse_command(buffer);
            if (result.status == ParseStatus::kIncomplete) {
                break;  // wait for more bytes
            }
            if (result.status == ParseStatus::kError) {
                static_cast<void>(send_all(fd, resp::error("Protocol error: " + result.error)));
                return;
            }
            if (!send_all(fd, execute(store_, result.command))) {
                return;
            }
            buffer.erase(0, result.consumed);
        }
    }
}

}  // namespace emberdb