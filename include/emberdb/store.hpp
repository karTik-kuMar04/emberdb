#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace emberdb {

class Store {
public:
    void set(const std::string& key, std::string value);
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
    bool del(const std::string& key);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, std::string> data_;
};

}  // namespace emberdb
