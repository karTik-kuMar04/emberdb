#include "emberdb/security.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <utility>

namespace emberdb {

bool constant_time_equals(std::string_view a, std::string_view b) {
    const std::size_t n = std::max(a.size(), b.size());
    // volatile stops the compiler from turning this back into an early-exit comparison.
    volatile unsigned char diff = static_cast<unsigned char>(a.size() == b.size() ? 0 : 1);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff = static_cast<unsigned char>(diff | (x ^ y));
    }
    return diff == 0;
}

bool is_loopback_address(std::string_view address) {
    if (address == "localhost") {
        return true;
    }
    const std::string text(address);
    in_addr parsed{};
    if (::inet_pton(AF_INET, text.c_str(), &parsed) != 1) {
        return false;
    }
    return (ntohl(parsed.s_addr) >> 24) == 127;
}

Authenticator::Authenticator(std::string password) : password_(std::move(password)) {}

AuthResult Authenticator::attempt(std::string_view password, TimePoint now) {
    if (now - window_start_ >= kWindow) {
        window_start_ = now;  // a new window: forget the old failures
        failures_in_window_ = 0;
    }
    if (failures_in_window_ >= kMaxFailuresPerWindow) {
        return AuthResult::kThrottled;  // do not even look at the guess
    }
    if (constant_time_equals(password, password_)) {
        return AuthResult::kOk;
    }
    ++failures_in_window_;
    return AuthResult::kWrongPassword;
}

}  // namespace emberdb
