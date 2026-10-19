#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "emberdb/socket.hpp"
#include "emberdb/store.hpp"

namespace emberdb {

// Append-only file: every state change is appended as a RESP command and replayed on startup.
class Aof {
public:
    enum class Fsync { kAlways, kEverySecond, kNever };

    struct LoadStats {
        std::size_t commands = 0;     // commands replayed
        std::size_t valid_bytes = 0;  // length of the good part of the file
        std::size_t tail_bytes = 0;   // incomplete trailing bytes that were cut off
    };

    // When the log is worth compacting automatically.
    struct RewritePolicy {
        std::size_t min_bytes = 64UL * 1024 * 1024;  // never rewrite a smaller log...
        std::size_t growth_percent = 100;            // ...and only after it has grown this much
    };

    // Replays `path` into `store` (if it exists), repairs a torn tail, then opens the file
    // for appending. Throws std::runtime_error if the file is corrupt or I/O fails.
    static std::unique_ptr<Aof> open(const std::string& path, Fsync policy, Store& store,
                                     LoadStats* stats = nullptr);

    static std::optional<Fsync> parse_fsync(std::string_view text);

    ~Aof();
    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    void append(std::string_view bytes);  // buffer a record in memory
    void commit();                        // write buffered records; fsync if policy is "always"
    void tick();                          // commit + the once-per-second fsync for "everysec"

    // Replaces the log with a minimal one that recreates `store`'s current contents.
    // Throws on I/O failure; in that case the old log is left exactly as it was.
    void rewrite(const Store& store);
    // True when the log is big enough and has grown enough to be worth rewriting.
    [[nodiscard]] bool rewrite_due() const;
    void set_rewrite_policy(const RewritePolicy& policy) { rewrite_policy_ = policy; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }  // current file size

private:
    Aof(Socket file, Fsync policy, std::string path, std::size_t size);
    void write_pending();
    void sync();
    void rewrite_impl(const Store& store);

    Socket file_;
    Fsync policy_;
    std::string path_;
    std::string pending_;
    bool unsynced_ = false;
    std::chrono::steady_clock::time_point last_sync_;
    std::size_t size_;       // bytes in the file (not counting `pending_`)
    std::size_t base_size_;  // size right after the last load or rewrite
    RewritePolicy rewrite_policy_;
    std::chrono::steady_clock::time_point retry_after_{};  // backoff after a failed rewrite
};

}  // namespace emberdb
