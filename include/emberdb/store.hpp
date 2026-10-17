#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace emberdb {

// Milliseconds on a monotonic clock. Injected so tests can control time.
using Clock = std::function<std::int64_t()>;
std::int64_t steady_now_ms();

struct IncrResult {
    enum class Status { kOk, kNotInteger, kOverflow };
    Status status = Status::kOk;
    long long value = 0;
};

class Store {
public:
    explicit Store(Clock clock = steady_now_ms);

    void set(const std::string& key, std::string value);  // like Redis SET: clears any TTL
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);  // true only if a live key was removed
    bool exists(const std::string& key);

    // Counts keys not yet purged, so it can include expired ones.
    [[nodiscard]] std::size_t size() const noexcept;

    // --- expiry ---
    // Sets a deadline `ms` from now. Returns false if the key doesn't exist.
    // ms <= 0 deletes the key immediately (and returns true), as Redis does.
    bool expire_in(const std::string& key, long long ms);
    // Remaining milliseconds; -2 if the key doesn't exist, -1 if it has no deadline.
    long long ttl_ms(const std::string& key);
    bool persist(const std::string& key);  // removes the deadline; true if one existed
    // Deletes up to `max_keys` expired keys. Returns how many were deleted.
    std::size_t purge_expired(std::size_t max_keys);

    // --- counters ---
    IncrResult incr_by(const std::string& key, long long delta);

private:
    struct Entry {
        std::string value;
        std::optional<std::int64_t> expire_at;  // absolute time in clock milliseconds
    };

    Entry* find_live(const std::string& key, std::int64_t now);  // deletes the key if expired
    void drop_expiry(const std::string& key, Entry& entry);

    Clock clock_;
    std::unordered_map<std::string, Entry> data_;
    // (deadline, key) pairs sorted by deadline: the front is always the next key to expire.
    std::set<std::pair<std::int64_t, std::string>> expirations_;
};

}  // namespace emberdb
