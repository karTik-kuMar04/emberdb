#include "emberdb/store.hpp"

#include <chrono>
#include <limits>

#include "emberdb/int_parse.hpp"

namespace emberdb {

std::int64_t steady_now_ms() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    using std::chrono::steady_clock;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Store::Store(Clock clock) : clock_(std::move(clock)) {}

Store::Entry* Store::find_live(const std::string& key, std::int64_t now) {
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return nullptr;
    }
    Entry& entry = it->second;
    if (entry.expire_at && *entry.expire_at <= now) {
        drop_expiry(key, entry);  // lazy expiry: the deadline passed, so the key is gone
        data_.erase(it);
        return nullptr;
    }
    return &entry;
}

void Store::drop_expiry(const std::string& key, Entry& entry) {
    if (entry.expire_at) {
        expirations_.erase(std::make_pair(*entry.expire_at, key));
        entry.expire_at.reset();
    }
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
    const bool was_live = !(it->second.expire_at && *it->second.expire_at <= clock_());
    drop_expiry(key, it->second);
    data_.erase(it);
    return was_live;
}

bool Store::exists(const std::string& key) {
    return find_live(key, clock_()) != nullptr;
}

std::size_t Store::size() const noexcept {
    return data_.size();
}

bool Store::expire_in(const std::string& key, long long ms) {
    const std::int64_t now = clock_();
    Entry* entry = find_live(key, now);
    if (entry == nullptr) {
        return false;
    }
    if (ms <= 0) {
        del(key);
        return true;
    }
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    const std::int64_t deadline = (ms > kMax - now) ? kMax : now + static_cast<std::int64_t>(ms);
    drop_expiry(key, *entry);
    entry->expire_at = deadline;
    expirations_.emplace(deadline, key);
    return true;
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
        data_.erase(first->second);
        expirations_.erase(first);
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

}  // namespace emberdb
