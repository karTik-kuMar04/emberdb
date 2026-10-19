#include "emberdb/aof.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "emberdb/dispatcher.hpp"
#include "emberdb/resp_parser.hpp"

namespace emberdb {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

// Puts the store into loading mode and always takes it back out, even on an exception.
class LoadingGuard {
public:
    explicit LoadingGuard(Store& store) : store_(store) { store_.set_loading(true); }
    ~LoadingGuard() { store_.set_loading(false); }
    LoadingGuard(const LoadingGuard&) = delete;
    LoadingGuard& operator=(const LoadingGuard&) = delete;

private:
    Store& store_;
};

Aof::LoadStats replay(const std::string& path, Store& store) {
    Aof::LoadStats stats;
    const int raw = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (raw < 0) {
        if (errno == ENOENT) {
            return stats;  // no file yet: a fresh database
        }
        fail("open AOF for reading");
    }
    Socket file(raw);
    const LoadingGuard loading(store);

    std::string buffer;
    std::array<char, 65536> chunk{};
    for (;;) {
        const ssize_t n = ::read(file.fd(), chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("read AOF");
        }
        if (n == 0) {
            break;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(n));

        std::size_t pos = 0;
        while (pos < buffer.size()) {
            ParseResult r = parse_command(std::string_view(buffer).substr(pos));
            if (r.status == ParseStatus::kIncomplete) {
                break;  // the rest arrives with the next read, or this is a torn tail
            }
            if (r.status == ParseStatus::kError) {
                throw std::runtime_error("AOF is corrupt at byte " +
                                         std::to_string(stats.valid_bytes + pos) + ": " + r.error);
            }
            const std::string reply = execute(store, r.command);
            if (reply.starts_with('-')) {
                // A healthy log only holds commands that succeeded. Fail loudly.
                throw std::runtime_error("AOF command " + std::to_string(stats.commands + 1) +
                                         " failed to replay: " + reply);
            }
            pos += r.consumed;
            ++stats.commands;
        }
        stats.valid_bytes += pos;
        buffer.erase(0, pos);
    }
    stats.tail_bytes = buffer.size();  // whatever is left over is an incomplete final command
    return stats;
}

}  // namespace

std::optional<Aof::Fsync> Aof::parse_fsync(std::string_view text) {
    if (text == "always") {
        return Fsync::kAlways;
    }
    if (text == "everysec") {
        return Fsync::kEverySecond;
    }
    if (text == "no") {
        return Fsync::kNever;
    }
    return std::nullopt;
}

std::unique_ptr<Aof> Aof::open(const std::string& path, Fsync policy, Store& store,
                               LoadStats* stats) {
    LoadStats local;
    LoadStats& result = stats != nullptr ? *stats : local;
    result = replay(path, store);

    if (result.tail_bytes > 0) {
        // A crash during a write left half a command at the end. Cut it off so new records
        // start on a clean boundary.
        if (::truncate(path.c_str(), static_cast<off_t>(result.valid_bytes)) < 0) {
            fail("truncate AOF");
        }
    }

    Socket file(::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600));
    if (!file.valid()) {
        fail("open AOF for appending");
    }
    return std::unique_ptr<Aof>(new Aof(std::move(file), policy));
}

Aof::Aof(Socket file, Fsync policy)
    : file_(std::move(file)), policy_(policy), last_sync_(std::chrono::steady_clock::now()) {}

Aof::~Aof() {
    try {
        write_pending();
        if (unsynced_ && policy_ != Fsync::kNever) {
            sync();
        }
    } catch (...) {
        // Nothing sensible can be done about an I/O error while shutting down.
    }
}

void Aof::append(std::string_view bytes) {
    pending_.append(bytes);
}

void Aof::write_pending() {
    std::size_t offset = 0;
    while (offset < pending_.size()) {
        const ssize_t n = ::write(file_.fd(), pending_.data() + offset, pending_.size() - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("AOF write failed");  // better to stop than to keep running without durability
        }
        offset += static_cast<std::size_t>(n);
    }
    if (!pending_.empty()) {
        pending_.clear();
        unsynced_ = true;
    }
}

void Aof::sync() {
    if (::fdatasync(file_.fd()) < 0) {
        fail("AOF fsync failed");
    }
    unsynced_ = false;
    last_sync_ = std::chrono::steady_clock::now();
}

void Aof::commit() {
    write_pending();
    if (policy_ == Fsync::kAlways && unsynced_) {
        sync();
    }
}

void Aof::tick() {
    commit();
    if (policy_ == Fsync::kEverySecond && unsynced_ &&
        std::chrono::steady_clock::now() - last_sync_ >= std::chrono::seconds(1)) {
        sync();
    }
}

}  // namespace emberdb
