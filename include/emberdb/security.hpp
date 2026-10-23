#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace emberdb {

// Compares two strings without stopping at the first difference, so the time taken does not
// reveal how many leading characters were right. It still reveals the longer of the two lengths.
bool constant_time_equals(std::string_view a, std::string_view b);

// True for "localhost" and any 127.x.x.x address. Anything else (including "0.0.0.0") is
// reachable from other machines.
bool is_loopback_address(std::string_view address);

enum class AuthResult { kOk, kWrongPassword, kThrottled };

// Holds the password and the global limit on wrong guesses.
class Authenticator {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    static constexpr int kMaxFailuresPerWindow = 10;
    static constexpr std::chrono::milliseconds kWindow{1000};

    // An empty password means authentication is off.
    explicit Authenticator(std::string password = {});

    [[nodiscard]] bool required() const noexcept { return !password_.empty(); }

    // Checks a guess. After kMaxFailuresPerWindow wrong guesses inside one window, every attempt
    // is refused as kThrottled (even a correct one) until the window ends, so a flood of guesses
    // cannot be used to test passwords.
    AuthResult attempt(std::string_view password, TimePoint now);

private:
    std::string password_;
    TimePoint window_start_{};
    int failures_in_window_ = 0;
};

}  // namespace emberdb
