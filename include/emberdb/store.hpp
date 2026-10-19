#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace emberdb {

// Milliseconds since the Unix epoch. Injected so tests can control time.
using Clock = std::function<std::int64_t()>;
std::int64_t unix_now_ms();

struct IncrResult {
    enum class Status { kOk, kNotInteger, kOverflow };
    Status status = Status::kOk;
    long long value = 0;
};

enum class ExpireResult { kNoSuchKey, kDeadlineSet, kKeyDeleted };

// Called once per live key: (key, value, absolute deadline if it has one).
using EntryVisitor =
    std::function<void(const std::string&, const std::string&, std::optional<std::int64_t>)>;

class Store {
public:
    explicit Store(Clock clock = unix_now_ms);

    void set(const std::string& key, std::string value);  // like Redis SET: clears any TTL
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);  // true only if a live key was removed
    bool exists(const std::string& key);
    void clear();  // FLUSHALL

    // Counts keys not yet purged, so it can include expired ones.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::int64_t now_ms() const { return clock_(); }

    // --- expiry ---
    // Deadline relative to now. Returns false if the key doesn't exist.
    bool expire_in(const std::string& key, long long ms);
    // Deadline as an absolute time. A time that is not in the future deletes the key
    // (except while loading or in passive mode, see below).
    ExpireResult expire_at(const std::string& key, std::int64_t at_ms);
    // Remaining milliseconds; -2 if the key doesn't exist, -1 if it has no deadline.
    long long ttl_ms(const std::string& key);
    bool persist(const std::string& key);  // removes the deadline; true if one existed
    // Deletes up to `max_keys` expired keys. Returns how many were deleted.
    std::size_t purge_expired(std::size_t max_keys);

    // --- counters ---
    IncrResult incr_by(const std::string& key, long long delta);

    // --- persistence support ---
    // While loading (AOF replay) nothing expires and past deadlines are stored as-is,
    // because replay runs later than the commands originally did.
    void set_loading(bool on) noexcept { loading_ = on; }
    // When on, keys removed by expiry are remembered so the server can announce them as DEL.
    void set_track_expired(bool on) noexcept { track_expired_ = on; }
    std::vector<std::string> take_expired();

    // --- replication support ---
    // Passive mode (used by followers): an expired key is hidden from reads but never deleted
    // by this store. Only an explicit DEL from the leader removes it.
    void set_passive_expiry(bool on) noexcept { passive_expiry_ = on; }

    // Calls `visit` for every key that has not expired. Does not modify the store.
    void for_each_live(const EntryVisitor& visit) const;

private:
    struct Entry {
        std::string value;
        std::optional<std::int64_t> expire_at;  // absolute time in clock milliseconds
    };
    using Map = std::unordered_map<std::string, Entry>;

    Entry* find_live(const std::string& key, std::int64_t now);  // deletes the key if expired
    void drop_expiry(const std::string& key, Entry& entry);
    void erase_entry(Map::iterator it, bool expired);

    Clock clock_;
    Map data_;
    // (deadline, key) pairs sorted by deadline: the front is always the next key to expire.
    std::set<std::pair<std::int64_t, std::string>> expirations_;
    std::vector<std::string> expired_log_;
    bool loading_ = false;
    bool track_expired_ = false;
    bool passive_expiry_ = false;
};

}  // namespace emberdb
