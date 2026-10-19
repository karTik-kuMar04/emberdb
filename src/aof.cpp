#include "emberdb/aof.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "emberdb/dispatcher.hpp"
#include "emberdb/resp_parser.hpp"
#include "emberdb/snapshot.hpp"

namespace emberdb {
namespace {

constexpr const char* kRewriteSuffix = ".rewrite.tmp";

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

void write_all(int fd, std::string_view data, const char* what) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail(what);
        }
        offset += static_cast<std::size_t>(n);
    }
}

// A rename only survives a power cut once the directory that holds it has been fsynced.
void sync_directory(const std::string& file_path) {
    std::filesystem::path dir = std::filesystem::path(file_path).parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    Socket handle(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!handle.valid()) {
        fail("open AOF directory");
    }
    if (::fsync(handle.fd()) < 0) {
        fail("fsync AOF directory");
    }
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

    // A crash in the middle of a rewrite can leave a half-written temp file behind. The real
    // log was never touched by it, so the leftover is just garbage.
    static_cast<void>(::unlink((path + kRewriteSuffix).c_str()));

    Socket file(::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600));
    if (!file.valid()) {
        fail("open AOF for appending");
    }
    return std::unique_ptr<Aof>(new Aof(std::move(file), policy, path, result.valid_bytes));
}

Aof::Aof(Socket file, Fsync policy, std::string path, std::size_t size)
    : file_(std::move(file)),
      policy_(policy),
      path_(std::move(path)),
      last_sync_(std::chrono::steady_clock::now()),
      size_(size),
      base_size_(size) {}

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
    if (pending_.empty()) {
        return;
    }
    write_all(file_.fd(), pending_, "AOF write failed");  // better to stop than to lose durability
    size_ += pending_.size();
    pending_.clear();
    unsynced_ = true;
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

bool Aof::rewrite_due() const {
    if (size_ < rewrite_policy_.min_bytes || std::chrono::steady_clock::now() < retry_after_) {
        return false;
    }
    const std::size_t base = std::max<std::size_t>(base_size_, 1);
    return size_ > base && (size_ - base) * 100 >= base * rewrite_policy_.growth_percent;
}

void Aof::rewrite(const Store& store) {
    try {
        rewrite_impl(store);
    } catch (...) {
        retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        throw;
    }
}

void Aof::rewrite_impl(const Store& store) {
    const std::string tmp = path_ + kRewriteSuffix;
    Socket out(::open(tmp.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!out.valid()) {
        fail("open AOF rewrite file");
    }

    std::size_t written = 0;
    try {
        write_snapshot(store, [&](std::string_view chunk) {
            write_all(out.fd(), chunk, "AOF rewrite write failed");
            written += chunk.size();
        });
        if (::fdatasync(out.fd()) < 0) {
            fail("AOF rewrite fsync failed");
        }
        if (::rename(tmp.c_str(), path_.c_str()) < 0) {
            fail("AOF rewrite rename failed");
        }
    } catch (...) {
        ::unlink(tmp.c_str());  // the old log was never touched, so just drop the attempt
        throw;
    }

    // Committed: the new file now sits at the log's path, and `out` is already open on it.
    // Swap it in before anything that can still fail.
    file_ = std::move(out);
    pending_.clear();  // those records are already inside the snapshot
    unsynced_ = false;
    size_ = written;
    base_size_ = written;
    last_sync_ = std::chrono::steady_clock::now();
    sync_directory(path_);
}

}  // namespace emberdb
