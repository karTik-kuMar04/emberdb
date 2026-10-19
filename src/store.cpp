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

Store::Store(Clock clock) : clock_(std::move(clock)) {}

void Store::drop_expiry(const std::string& key, Entry& entry) {
    if (entry.expire_at) {
        expirations_.erase(std::make_pair(*entry.expire_at, key));
        entry.expire_at.reset();
    }
}

// The one place a key is removed. `expired` says it died of old age rather than by command.
void Store::erase_entry(Map::iterator it, bool expired) {
    if (expired && track_expired_) {
        expired_log_.push_back(it->first);
    }
    drop_expiry(it->first, it->second);
    data_.erase(it);
}

Store::Entry* Store::find_live(const std::string& key, std::int64_t now) {
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return nullptr;
    }
    Entry& entry = it->second;
    if (!loading_ && entry.expire_at && *entry.expire_at <= now) {
        erase_entry(it, true);  // lazy expiry: the deadline passed, so the key is gone
        return nullptr;
    }
    return &entry;
}

void Store::set(const std::string& key, std::string value) {
    Entry& entry = data_[key];
    drop_expiry(key, entry);
    entry.value = std::move(value);
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
    const bool expired = !loading_ && it->second.expire_at && *it->second.expire_at <= clock_();
    erase_entry(it, expired);
    return !expired;
}

bool Store::exists(const std::string& key) {
    return find_live(key, clock_()) != nullptr;
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
    if (!loading_ && at_ms <= now) {
        del(key);
        return ExpireResult::kKeyDeleted;
    }
    drop_expiry(key, *entry);
    entry->expire_at = at_ms;
    expirations_.emplace(at_ms, key);
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
    return *entry->expire_at - now;  // always > 0 here: find_live removed expired keys
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
        entry = &data_[key];  // new key, no deadline
    }
    entry->value = std::to_string(next);  // an existing deadline is kept
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

}  // namespace emberdb
