#include "emberdb/store.hpp"

#include <utility>

namespace emberdb {

void Store::set(const std::string& key, std::string value) {
    data_[key] = std::move(value);
}

std::optional<std::string> Store::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Store::del(const std::string& key) {
    return data_.erase(key) > 0;
}

std::size_t Store::size() const noexcept {
    return data_.size();
}

}  // namespace emberdb
