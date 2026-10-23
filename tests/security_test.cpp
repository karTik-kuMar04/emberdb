#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

#include "emberdb/security.hpp"

using emberdb::AuthResult;
using emberdb::Authenticator;
using emberdb::constant_time_equals;
using emberdb::is_loopback_address;

namespace {

Authenticator::TimePoint at(long long ms) {
    return Authenticator::TimePoint{} + std::chrono::milliseconds(ms);
}

}  // namespace

TEST(ConstantTime, EqualStrings) {
    EXPECT_TRUE(constant_time_equals("", ""));
    EXPECT_TRUE(constant_time_equals("hunter2", "hunter2"));
    EXPECT_TRUE(constant_time_equals(std::string_view("a\0b", 3), std::string_view("a\0b", 3)));
}

TEST(ConstantTime, DifferentStrings) {
    EXPECT_FALSE(constant_time_equals("hunter2", "hunter3"));
    EXPECT_FALSE(constant_time_equals("hunter2", "hunter"));
    EXPECT_FALSE(constant_time_equals("hunter", "hunter2"));
    EXPECT_FALSE(constant_time_equals("", "x"));
    EXPECT_FALSE(constant_time_equals("x", ""));
    EXPECT_FALSE(constant_time_equals(std::string_view("a\0b", 3), std::string_view("a\0c", 3)));
}

TEST(Loopback, RecognizesLoopbackAddresses) {
    EXPECT_TRUE(is_loopback_address("localhost"));
    EXPECT_TRUE(is_loopback_address("127.0.0.1"));
    EXPECT_TRUE(is_loopback_address("127.5.6.7"));
}

TEST(Loopback, RejectsEverythingElse) {
    EXPECT_FALSE(is_loopback_address("0.0.0.0"));
    EXPECT_FALSE(is_loopback_address("192.168.1.10"));
    EXPECT_FALSE(is_loopback_address("10.0.0.1"));
    EXPECT_FALSE(is_loopback_address("128.0.0.1"));
    EXPECT_FALSE(is_loopback_address("not-an-ip"));
    EXPECT_FALSE(is_loopback_address(""));
}

TEST(Authenticator, NotRequiredWhenPasswordIsEmpty) {
    EXPECT_FALSE(Authenticator().required());
    EXPECT_FALSE(Authenticator("").required());
    EXPECT_TRUE(Authenticator("pw").required());
}

TEST(Authenticator, AcceptsRightAndRejectsWrongPassword) {
    Authenticator a("pw");
    EXPECT_EQ(a.attempt("pw", at(0)), AuthResult::kOk);
    EXPECT_EQ(a.attempt("PW", at(1)), AuthResult::kWrongPassword);
    EXPECT_EQ(a.attempt("", at(2)), AuthResult::kWrongPassword);
    EXPECT_EQ(a.attempt("pw ", at(3)), AuthResult::kWrongPassword);
}

TEST(Authenticator, ThrottlesAfterTooManyFailures) {
    Authenticator a("pw");
    for (int i = 0; i < Authenticator::kMaxFailuresPerWindow; ++i) {
        EXPECT_EQ(a.attempt("bad", at(100 + i)), AuthResult::kWrongPassword);
    }
    EXPECT_EQ(a.attempt("bad", at(200)), AuthResult::kThrottled);
    EXPECT_EQ(a.attempt("pw", at(300)), AuthResult::kThrottled);  // even the right one has to wait
}

TEST(Authenticator, ResumesWhenTheWindowPasses) {
    Authenticator a("pw");
    for (int i = 0; i < Authenticator::kMaxFailuresPerWindow; ++i) {
        a.attempt("bad", at(100));
    }
    EXPECT_EQ(a.attempt("pw", at(999)), AuthResult::kThrottled);
    EXPECT_EQ(a.attempt("pw", at(1000)), AuthResult::kOk);  // a new window began
    for (int i = 0; i < Authenticator::kMaxFailuresPerWindow; ++i) {
        EXPECT_EQ(a.attempt("bad", at(1100)), AuthResult::kWrongPassword);
    }
    EXPECT_EQ(a.attempt("bad", at(1200)), AuthResult::kThrottled);  // and the budget is per window
}

TEST(Authenticator, SuccessfulLoginsAreNotFailures) {
    Authenticator a("pw");
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(a.attempt("pw", at(i)), AuthResult::kOk);
    }
    EXPECT_EQ(a.attempt("bad", at(60)), AuthResult::kWrongPassword);
}
