#include <gtest/gtest.h>

#include "emberdb/store.hpp"
#include "fake_clock.hpp"

TEST(StoreTest, SetThenGetReturnsValue) {
    emberdb::Store s;
    s.set("a", "1");
    ASSERT_TRUE(s.get("a").has_value());
    EXPECT_EQ(s.get("a").value(), "1");
}

TEST(StoreTest, MissingKeyReturnsNullopt) {
    emberdb::Store s;
    EXPECT_FALSE(s.get("nope").has_value());
}

TEST(StoreTest, SetOverwritesExistingValue) {
    emberdb::Store s;
    s.set("a", "1");
    s.set("a", "2");
    EXPECT_EQ(s.get("a").value(), "2");
    EXPECT_EQ(s.size(), 1U);
}

TEST(StoreTest, DelRemovesKeyAndReportsResult) {
    emberdb::Store s;
    s.set("a", "1");
    EXPECT_TRUE(s.del("a"));
    EXPECT_FALSE(s.del("a"));
    EXPECT_FALSE(s.get("a").has_value());
}

TEST(StoreExpiry, KeyDisappearsAfterTtl) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    EXPECT_TRUE(s.expire_in("a", 500));
    clk.now += 499;
    EXPECT_TRUE(s.get("a").has_value());
    clk.now += 1;
    EXPECT_FALSE(s.get("a").has_value());
}

TEST(StoreExpiry, SetClearsTtl) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 100);
    s.set("a", "2");
    clk.now += 1000;
    ASSERT_TRUE(s.get("a").has_value());
    EXPECT_EQ(s.ttl_ms("a"), -1);
}

TEST(StoreExpiry, TtlReportsRemainingMissingAndNoExpiry) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    EXPECT_EQ(s.ttl_ms("nope"), -2);
    s.set("a", "1");
    EXPECT_EQ(s.ttl_ms("a"), -1);
    s.expire_in("a", 1000);
    clk.now += 300;
    EXPECT_EQ(s.ttl_ms("a"), 700);
}

TEST(StoreExpiry, PersistRemovesTtl) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 100);
    EXPECT_TRUE(s.persist("a"));
    EXPECT_FALSE(s.persist("a"));
    clk.now += 1000;
    EXPECT_TRUE(s.exists("a"));
}

TEST(StoreExpiry, NonPositiveExpireDeletesKey) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.set("b", "2");
    EXPECT_TRUE(s.expire_in("a", 0));
    EXPECT_FALSE(s.exists("a"));
    EXPECT_TRUE(s.expire_in("b", -5));
    EXPECT_FALSE(s.exists("b"));
    EXPECT_FALSE(s.expire_in("missing", 10));
}

TEST(StoreExpiry, PurgeRemovesDueKeysAndRespectsBudget) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    for (int i = 0; i < 5; ++i) {
        const std::string k = "k" + std::to_string(i);
        s.set(k, "v");
        s.expire_in(k, 100);
    }
    s.set("live", "v");
    s.expire_in("live", 10000);
    clk.now += 100;
    EXPECT_EQ(s.purge_expired(3), 3U);
    EXPECT_EQ(s.size(), 3U);
    EXPECT_EQ(s.purge_expired(100), 2U);
    EXPECT_EQ(s.size(), 1U);
    EXPECT_TRUE(s.exists("live"));
}

TEST(StoreExpiry, DelOfExpiredKeyReportsFalse) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 10);
    clk.now += 10;
    EXPECT_FALSE(s.del("a"));
    EXPECT_EQ(s.size(), 0U);
}

TEST(StoreExpiry, ReExpireReplacesOldDeadline) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 100);
    s.expire_in("a", 1000);
    clk.now += 500;
    EXPECT_TRUE(s.exists("a"));
    EXPECT_EQ(s.purge_expired(10), 0U);
    clk.now += 500;
    EXPECT_FALSE(s.exists("a"));
}

TEST(StoreIncr, MissingKeyStartsAtZero) {
    emberdb::Store s;
    auto r = s.incr_by("n", 1);
    EXPECT_EQ(r.status, emberdb::IncrResult::Status::kOk);
    EXPECT_EQ(r.value, 1);
    EXPECT_EQ(s.incr_by("n", 1).value, 2);
}

TEST(StoreIncr, KeepsTtl) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "5");
    s.expire_in("a", 1000);
    EXPECT_EQ(s.incr_by("a", 1).value, 6);
    EXPECT_EQ(s.get("a").value(), "6");
    EXPECT_EQ(s.ttl_ms("a"), 1000);
}

TEST(StoreIncr, RejectsNonIntegerAndLeavesValueAlone) {
    emberdb::Store s;
    s.set("a", "abc");
    EXPECT_EQ(s.incr_by("a", 1).status, emberdb::IncrResult::Status::kNotInteger);
    EXPECT_EQ(s.get("a").value(), "abc");
    s.set("b", "007");
    EXPECT_EQ(s.incr_by("b", 1).status, emberdb::IncrResult::Status::kNotInteger);
}

TEST(StoreIncr, DetectsOverflowBothWays) {
    emberdb::Store s;
    s.set("max", "9223372036854775807");
    EXPECT_EQ(s.incr_by("max", 1).status, emberdb::IncrResult::Status::kOverflow);
    EXPECT_EQ(s.get("max").value(), "9223372036854775807");
    s.set("min", "-9223372036854775808");
    EXPECT_EQ(s.incr_by("min", -1).status, emberdb::IncrResult::Status::kOverflow);
}

TEST(StoreIncr, DecrementWorks) {
    emberdb::Store s;
    s.set("a", "10");
    EXPECT_EQ(s.incr_by("a", -3).value, 7);
}

TEST(StoreTracking, LazyAndActiveExpiryAreReported) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set_track_expired(true);
    s.set("a", "1");
    s.expire_in("a", 10);
    s.set("b", "1");
    s.expire_in("b", 10);
    clk.now += 10;
    EXPECT_FALSE(s.exists("a"));          // lazy: discovered by a read
    EXPECT_EQ(s.purge_expired(10), 1U);   // active: found by the sweep
    const auto keys = s.take_expired();
    ASSERT_EQ(keys.size(), 2U);
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "b");
    EXPECT_TRUE(s.take_expired().empty());
}

TEST(StoreTracking, ExplicitDeleteIsNotReportedAsExpiry) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set_track_expired(true);
    s.set("a", "1");
    EXPECT_TRUE(s.del("a"));
    EXPECT_TRUE(s.take_expired().empty());
}

TEST(StoreLoading, KeepsExpiredKeysUntilLoadingEnds) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set_loading(true);
    s.set("a", "1");
    EXPECT_EQ(s.expire_at("a", 500), emberdb::ExpireResult::kDeadlineSet);  // 500 is in the past
    EXPECT_EQ(s.get("a").value(), "1");
    s.set_loading(false);
    EXPECT_FALSE(s.get("a").has_value());
}

TEST(StoreExpireAt, ReportsWhatHappened) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    EXPECT_EQ(s.expire_at("nope", 5000), emberdb::ExpireResult::kNoSuchKey);
    s.set("a", "1");
    EXPECT_EQ(s.expire_at("a", 5000), emberdb::ExpireResult::kDeadlineSet);
    EXPECT_EQ(s.ttl_ms("a"), 4000);
    EXPECT_EQ(s.expire_at("a", 1000), emberdb::ExpireResult::kKeyDeleted);
    EXPECT_FALSE(s.exists("a"));
}
