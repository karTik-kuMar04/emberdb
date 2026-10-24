#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

// What to do when memory use goes over the limit.
enum class EvictionPolicy {
    kNoEviction,  // refuse commands that would add data
    kAllKeysLru,  // remove the least recently used keys to make room
};
const char* policy_name(EvictionPolicy policy);
std::optional<EvictionPolicy> parse_eviction_policy(std::string_view text);

// Called once per live key: (key, value, absolute deadline if it has one).
using EntryVisitor =
    std::function<void(const std::string&, const std::string&, std::optional<std::int64_t>)>;

class Store {
public:
    // Most keys one make_room() call will evict, so a huge overshoot cannot freeze the loop.
    static constexpr std::size_t kMaxEvictionsPerCall = 10000;

    explicit Store(Clock clock = unix_now_ms);
    // Entries hold iterators into this store's own LRU list, so copying would break them.
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

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

    // --- memory limit and eviction ---
    void set_maxmemory(std::size_t bytes) noexcept { max_memory_ = bytes; }  // 0 = unlimited
    void set_eviction_policy(EvictionPolicy policy) noexcept { policy_ = policy; }
    [[nodiscard]] std::size_t used_memory() const noexcept { return used_; }
    [[nodiscard]] std::size_t maxmemory() const noexcept { return max_memory_; }
    [[nodiscard]] EvictionPolicy eviction_policy() const noexcept { return policy_; }
    [[nodiscard]] std::size_t evicted_keys() const noexcept { return evicted_keys_; }
    [[nodiscard]] std::size_t expired_keys() const noexcept { return expired_keys_; }
    // Called before a command that adds data. Frees memory if the policy allows it. Returns
    // false when the command must be refused (over the limit and nothing may be evicted).
    // Always true while loading, for followers, or when no limit is set.
    bool make_room();
    // Recounts memory and checks every index against the data. Used by tests.
    [[nodiscard]] bool check_invariants() const;

    // --- persistence support ---
    // While loading (AOF replay) nothing expires or gets evicted, and past deadlines are stored
    // as-is, because replay runs later than the commands originally did.
    void set_loading(bool on) noexcept { loading_ = on; }
    // When on, keys removed by expiry or eviction are remembered so the server can announce them
    // as DEL.
    void set_track_expired(bool on) noexcept { track_expired_ = on; }
    std::vector<std::string> take_expired();

    // --- replication support ---
    // Passive mode (used by followers): an expired key is hidden from reads but never deleted
    // by this store, and nothing is evicted. Only an explicit DEL from the leader removes keys.
    void set_passive_expiry(bool on) noexcept { passive_expiry_ = on; }

    // Calls `visit` for every key that has not expired. Does not modify the store.
    void for_each_live(const EntryVisitor& visit) const;

private:
    using LruList = std::list<const std::string*>;  // front = most recently used

    struct Entry {
        std::string value;
        std::optional<std::int64_t> expire_at;  // absolute time in clock milliseconds
        LruList::iterator lru_it;               // this key's place in `lru_`
    };
    using Map = std::unordered_map<std::string, Entry>;

    // Fixed per-item costs for the memory estimate (list node, hash node, string headers...).
    static constexpr std::size_t kEntryOverhead = 160;
    static constexpr std::size_t kExpiryOverhead = 80;
    static std::size_t entry_cost(const std::string& key, const Entry& entry);
    static std::size_t expiry_cost(const std::string& key);

    Entry* find_live(const std::string& key, std::int64_t now);  // hides or deletes expired keys
    Entry& acquire(const std::string& key);  // finds or creates the entry, marks it most recent
    void touch(Entry& entry);
    void replace_value(Entry& entry, std::string value);
    void drop_expiry(const std::string& key, Entry& entry);
    // The one place a key is removed. `announce` queues it for take_expired().
    void erase_entry(Map::iterator it, bool announce);
    bool evict_one();

    Clock clock_;
    Map data_;
    LruList lru_;
    // (deadline, key) pairs sorted by deadline: the front is always the next key to expire.
    std::set<std::pair<std::int64_t, std::string>> expirations_;
    std::vector<std::string> expired_log_;
    std::size_t used_ = 0;
    std::size_t max_memory_ = 0;
    std::size_t evicted_keys_ = 0;
    std::size_t expired_keys_ = 0;
    EvictionPolicy policy_ = EvictionPolicy::kNoEviction;
    bool loading_ = false;
    bool track_expired_ = false;
    bool passive_expiry_ = false;
};

}  // namespace emberdb
