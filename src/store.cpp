#include "emberdb/store.hpp"

#include <chrono>

#include "emberdb/int_parse.hpp"

namespace emberdb {

std::int64_t unix_now_ms() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    using std::chrono::system_clock;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* policy_name(EvictionPolicy policy) {
    return policy == EvictionPolicy::kAllKeysLru ? "allkeys-lru" : "noeviction";
}

std::optional<EvictionPolicy> parse_eviction_policy(std::string_view text) {
    if (text == "noeviction") {
        return EvictionPolicy::kNoEviction;
    }
    if (text == "allkeys-lru") {
        return EvictionPolicy::kAllKeysLru;
    }
    return std::nullopt;
}

Store::Store(Clock clock) : clock_(std::move(clock)) {}

std::size_t Store::entry_cost(const std::string& key, const Entry& entry) {
    return kEntryOverhead + key.size() + entry.value.size();
}

std::size_t Store::expiry_cost(const std::string& key) {
    return kExpiryOverhead + key.size();
}

void Store::touch(Entry& entry) {
    lru_.splice(lru_.begin(), lru_, entry.lru_it);  // O(1), and no iterator is invalidated
}

// Finds the entry or creates an empty one, and marks it most recently used.
Store::Entry& Store::acquire(const std::string& key) {
    auto [it, inserted] = data_.try_emplace(key);
    Entry& entry = it->second;
    if (inserted) {
        lru_.push_front(&it->first);  // map nodes never move, so this pointer stays valid
        entry.lru_it = lru_.begin();
        used_ += kEntryOverhead + key.size();
    } else {
        touch(entry);
    }
    return entry;
}

void Store::replace_value(Entry& entry, std::string value) {
    used_ -= entry.value.size();
    entry.value = std::move(value);
    used_ += entry.value.size();
}

void Store::drop_expiry(const std::string& key, Entry& entry) {
    if (entry.expire_at) {
        expirations_.erase(std::make_pair(*entry.expire_at, key));
        used_ -= expiry_cost(key);
        entry.expire_at.reset();
    }
}

void Store::erase_entry(Map::iterator it, bool announce) {
    if (announce && track_expired_) {
        expired_log_.push_back(it->first);
    }
    drop_expiry(it->first, it->second);
    lru_.erase(it->second.lru_it);
    used_ -= entry_cost(it->first, it->second);
    data_.erase(it);
}

Store::Entry* Store::find_live(const std::string& key, std::int64_t now) {
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return nullptr;
    }
    Entry& entry = it->second;
    if (!loading_ && entry.expire_at && *entry.expire_at <= now) {
        if (passive_expiry_) {
            return nullptr;  // follower: hide the key, but wait for the leader's DEL
        }
        ++expired_keys_;
        erase_entry(it, true);  // lazy expiry: the deadline passed, so the key is gone
        return nullptr;
    }
    touch(entry);  // every read or write of a live key counts as a use
    return &entry;
}

void Store::set(const std::string& key, std::string value) {
    Entry& entry = acquire(key);
    drop_expiry(key, entry);
    replace_value(entry, std::move(value));
}

std::optional<std::string> Store::get(const std::string& key) {
    const Entry* entry = find_live(key, clock_());
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->value;
}

bool Store::del(const std::string& key) {
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }
    const bool expired =
        !loading_ && !passive_expiry_ && it->second.expire_at && *it->second.expire_at <= clock_();
    if (expired) {
        ++expired_keys_;
    }
    erase_entry(it, expired);  // an expired key is announced, a normal delete is not
    return !expired;
}

bool Store::exists(const std::string& key) {
    return find_live(key, clock_()) != nullptr;
}

void Store::clear() {
    data_.clear();
    expirations_.clear();
    lru_.clear();
    used_ = 0;
}

std::size_t Store::size() const noexcept {
    return data_.size();
}

bool Store::expire_in(const std::string& key, long long ms) {
    return expire_at(key, saturating_add(clock_(), ms)) != ExpireResult::kNoSuchKey;
}

ExpireResult Store::expire_at(const std::string& key, std::int64_t at_ms) {
    const std::int64_t now = clock_();
    Entry* entry = find_live(key, now);
    if (entry == nullptr) {
        return ExpireResult::kNoSuchKey;
    }
    if (!loading_ && !passive_expiry_ && at_ms <= now) {
        del(key);
        return ExpireResult::kKeyDeleted;
    }
    drop_expiry(key, *entry);
    entry->expire_at = at_ms;
    expirations_.emplace(at_ms, key);
    used_ += expiry_cost(key);
    return ExpireResult::kDeadlineSet;
}

long long Store::ttl_ms(const std::string& key) {
    const std::int64_t now = clock_();
    const Entry* entry = find_live(key, now);
    if (entry == nullptr) {
        return -2;
    }
    if (!entry->expire_at) {
        return -1;
    }
    return *entry->expire_at - now;  // always > 0 here: find_live removed or hid expired keys
}

bool Store::persist(const std::string& key) {
    Entry* entry = find_live(key, clock_());
    if (entry == nullptr || !entry->expire_at) {
        return false;
    }
    drop_expiry(key, *entry);
    return true;
}

std::size_t Store::purge_expired(std::size_t max_keys) {
    if (passive_expiry_) {
        return 0;  // a follower waits for the leader to say which keys are gone
    }
    const std::int64_t now = clock_();
    std::size_t removed = 0;
    while (removed < max_keys && !expirations_.empty()) {
        const auto first = expirations_.begin();
        if (first->first > now) {
            break;  // the soonest deadline is still in the future, so nothing else is due
        }
        const std::string key = first->second;  // copy: erasing below invalidates `first`
        const auto it = data_.find(key);
        if (it == data_.end()) {
            expirations_.erase(first);  // defensive: index and map should never disagree
            continue;
        }
        erase_entry(it, true);
        ++removed;
        ++expired_keys_;
    }
    return removed;
}

IncrResult Store::incr_by(const std::string& key, long long delta) {
    Entry* entry = find_live(key, clock_());
    long long current = 0;
    if (entry != nullptr) {
        const auto parsed = parse_int64(entry->value);
        if (!parsed) {
            return {IncrResult::Status::kNotInteger, 0};
        }
        current = *parsed;
    }
    long long next = 0;
    if (__builtin_add_overflow(current, delta, &next)) {
        return {IncrResult::Status::kOverflow, 0};
    }
    if (entry == nullptr) {
        entry = &acquire(key);  // new key, no deadline
    }
    replace_value(*entry, std::to_string(next));  // an existing deadline is kept
    return {IncrResult::Status::kOk, next};
}

std::vector<std::string> Store::take_expired() {
    std::vector<std::string> out;
    out.swap(expired_log_);
    return out;
}

void Store::for_each_live(const EntryVisitor& visit) const {
    const std::int64_t now = clock_();
    for (const auto& [key, entry] : data_) {
        if (entry.expire_at && *entry.expire_at <= now) {
            continue;  // expired but not purged yet: leave it out of the snapshot
        }
        visit(key, entry.value, entry.expire_at);
    }
}

bool Store::evict_one() {
    if (lru_.empty()) {
        return false;
    }
    const auto it = data_.find(*lru_.back());  // the least recently used key
    if (it == data_.end()) {
        lru_.pop_back();  // cannot happen: the list and the map always agree
        return true;
    }
    erase_entry(it, true);  // announced, so the AOF and followers see a DEL
    ++evicted_keys_;
    return true;
}

bool Store::make_room() {
    if (max_memory_ == 0 || loading_ || passive_expiry_ || used_ <= max_memory_) {
        return true;
    }
    purge_expired(kMaxEvictionsPerCall);  // expired keys are free to remove, so take them first
    if (policy_ == EvictionPolicy::kNoEviction) {
        return used_ <= max_memory_;
    }
    std::size_t evicted = 0;
    while (used_ > max_memory_ && evicted < kMaxEvictionsPerCall) {
        if (!evict_one()) {
            break;
        }
        ++evicted;
    }
    // Hitting the cap is not a failure: the command goes ahead and the next one keeps evicting.
    return used_ <= max_memory_ || evicted >= kMaxEvictionsPerCall;
}

bool Store::check_invariants() const {
    std::size_t memory = 0;
    std::size_t with_deadline = 0;
    for (const auto& [key, entry] : data_) {
        memory += entry_cost(key, entry);
        if (*entry.lru_it != &key) {
            return false;  // the list entry must point at this very key
        }
        if (entry.expire_at) {
            memory += expiry_cost(key);
            ++with_deadline;
            if (expirations_.count(std::make_pair(*entry.expire_at, key)) == 0) {
                return false;
            }
        }
    }
    return memory == used_ && with_deadline == expirations_.size() && lru_.size() == data_.size();
}

}  // namespace emberdb
