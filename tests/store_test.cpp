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

TEST(StorePassive, ExpiredKeysAreHiddenButNotDeleted) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 10);
    s.set_passive_expiry(true);
    clk.now += 10;
    EXPECT_FALSE(s.get("a").has_value());
    EXPECT_FALSE(s.exists("a"));
    EXPECT_EQ(s.ttl_ms("a"), -2);
    EXPECT_EQ(s.size(), 1U);             // still in memory
    EXPECT_EQ(s.purge_expired(10), 0U);  // and the sweep leaves it alone
    EXPECT_TRUE(s.del("a"));             // only the leader's DEL removes it
    EXPECT_EQ(s.size(), 0U);
}

TEST(StorePassive, PastDeadlineFromLeaderIsStoredNotApplied) {
    FakeClock clk;  // starts at 1000
    emberdb::Store s(clk.fn());
    s.set_passive_expiry(true);
    s.set("a", "1");
    EXPECT_EQ(s.expire_at("a", 500), emberdb::ExpireResult::kDeadlineSet);  // already in the past
    EXPECT_EQ(s.size(), 1U);
    EXPECT_FALSE(s.get("a").has_value());
}

TEST(StorePassive, LeavingPassiveModeResumesNormalExpiry) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set_passive_expiry(true);
    s.set("a", "1");
    s.expire_in("a", 10);
    clk.now += 10;
    s.set_passive_expiry(false);
    EXPECT_EQ(s.purge_expired(10), 1U);
    EXPECT_EQ(s.size(), 0U);
}

TEST(StoreClear, RemovesKeysAndDeadlines) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 100);
    s.set("b", "2");
    s.clear();
    EXPECT_EQ(s.size(), 0U);
    EXPECT_EQ(s.purge_expired(10), 0U);
    EXPECT_FALSE(s.exists("a"));
}

#include <random>
#include <vector>

#include "emberdb/int_parse.hpp"

namespace {
const std::string kTenChars = "0123456789";
}

TEST(StoreMemory, UsedMemoryTracksDataAndReturnsToZero) {
    emberdb::Store s;
    EXPECT_EQ(s.used_memory(), 0U);
    s.set("a", "1");
    const std::size_t small = s.used_memory();
    EXPECT_GT(small, 0U);
    s.set("b", std::string(1000, 'x'));
    EXPECT_GE(s.used_memory(), small + 1000);
    EXPECT_TRUE(s.check_invariants());
    EXPECT_TRUE(s.del("a"));
    EXPECT_TRUE(s.del("b"));
    EXPECT_EQ(s.used_memory(), 0U);
    EXPECT_TRUE(s.check_invariants());
}

TEST(StoreMemory, OverwriteAdjustsByValueSizeOnly) {
    emberdb::Store s;
    s.set("k", "x");
    const std::size_t before = s.used_memory();
    s.set("k", std::string(100, 'x'));
    EXPECT_EQ(s.used_memory(), before + 99);
    EXPECT_TRUE(s.check_invariants());
}

TEST(StoreMemory, DeadlineIndexIsCounted) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("k", "v");
    const std::size_t plain = s.used_memory();
    s.expire_in("k", 1000);
    EXPECT_GT(s.used_memory(), plain);
    EXPECT_TRUE(s.check_invariants());
    EXPECT_TRUE(s.persist("k"));
    EXPECT_EQ(s.used_memory(), plain);
    s.expire_in("k", 1000);
    clk.now += 1000;
    EXPECT_EQ(s.purge_expired(10), 1U);
    EXPECT_EQ(s.used_memory(), 0U);
    EXPECT_TRUE(s.check_invariants());
}

TEST(StoreMemory, ClearResetsEverything) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 100);
    s.set("b", "2");
    s.clear();
    EXPECT_EQ(s.used_memory(), 0U);
    EXPECT_EQ(s.size(), 0U);
    EXPECT_TRUE(s.check_invariants());
    s.set("c", "3");  // the store is still usable
    EXPECT_TRUE(s.check_invariants());
}

TEST(StoreMemory, InvariantsHoldThroughRandomOperations) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set_track_expired(true);
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set_maxmemory(3000);
    std::mt19937 rng(12345);
    auto key = [&] { return "k" + std::to_string(rng() % 50); };
    for (int i = 0; i < 5000; ++i) {
        switch (rng() % 9) {
            case 0: s.set(key(), std::string(rng() % 60, 'x')); break;
            case 1: s.get(key()); break;
            case 2: s.del(key()); break;
            case 3: s.expire_in(key(), static_cast<long long>(rng() % 100)); break;
            case 4: s.persist(key()); break;
            case 5: s.incr_by(key(), 1); break;
            case 6: clk.now += static_cast<std::int64_t>(rng() % 40); break;
            case 7: s.purge_expired(rng() % 5 + 1); break;
            default: s.make_room(); break;
        }
        if (i % 25 == 0) {
            ASSERT_TRUE(s.check_invariants()) << "after step " << i;
        }
        s.take_expired();  // keep the announcement log from growing
    }
    EXPECT_TRUE(s.check_invariants());
    EXPECT_GT(s.evicted_keys(), 0U);
}

TEST(StoreEviction, EvictsTheLeastRecentlyUsedKeyFirst) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);
    s.set("b", kTenChars);
    s.set("c", kTenChars);
    s.set_maxmemory(s.used_memory() - 1);  // one byte over: exactly one key must go
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.size(), 2U);
    EXPECT_EQ(s.evicted_keys(), 1U);
    EXPECT_FALSE(s.get("a").has_value());  // "a" was set first and never used again
    EXPECT_TRUE(s.get("b").has_value());
    EXPECT_TRUE(s.get("c").has_value());
    EXPECT_TRUE(s.check_invariants());
}

TEST(StoreEviction, ReadsRefreshRecency) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);
    s.set("b", kTenChars);
    s.set("c", kTenChars);
    EXPECT_TRUE(s.get("a").has_value());  // "a" is now the newest, so "b" is the oldest
    s.set_maxmemory(s.used_memory() - 1);
    EXPECT_TRUE(s.make_room());
    EXPECT_FALSE(s.get("b").has_value());
    EXPECT_TRUE(s.get("a").has_value());
    EXPECT_TRUE(s.get("c").has_value());
}

TEST(StoreEviction, WritesRefreshRecency) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);
    s.set("b", kTenChars);
    s.set("c", kTenChars);
    s.set("a", kTenChars);  // overwriting counts as a use
    s.set_maxmemory(s.used_memory() - 1);
    EXPECT_TRUE(s.make_room());
    EXPECT_FALSE(s.get("b").has_value());
    EXPECT_TRUE(s.get("a").has_value());
}

TEST(StoreEviction, NoEvictionPolicyRefusesInsteadOfEvicting) {
    emberdb::Store s;  // the default policy is noeviction
    s.set("a", kTenChars);
    s.set("b", kTenChars);
    s.set_maxmemory(s.used_memory() - 1);
    EXPECT_FALSE(s.make_room());
    EXPECT_EQ(s.size(), 2U);
    EXPECT_EQ(s.evicted_keys(), 0U);
    s.set_maxmemory(s.used_memory());  // exactly at the limit is fine
    EXPECT_TRUE(s.make_room());
}

TEST(StoreEviction, PurgesExpiredKeysBeforeEvictingLiveOnes) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);  // the least recently used key
    s.set("x", kTenChars);
    s.expire_in("x", 10);
    s.set("b", kTenChars);
    clk.now += 10;  // "x" has expired but is still in memory
    s.set_maxmemory(s.used_memory() - 1);
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.evicted_keys(), 0U);  // removing the expired key was enough
    EXPECT_EQ(s.size(), 2U);
    EXPECT_TRUE(s.get("a").has_value());
    EXPECT_TRUE(s.get("b").has_value());
}

TEST(StoreEviction, EvictionsAreAnnouncedSoTheyReachTheLogAndFollowers) {
    emberdb::Store s;
    s.set_track_expired(true);
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);
    s.set("b", kTenChars);
    s.set_maxmemory(s.used_memory() - 1);
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.take_expired(), (std::vector<std::string>{"a"}));
}

TEST(StoreEviction, FollowersNeverEvict) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);
    s.set("b", kTenChars);
    s.set_maxmemory(1);
    s.set_passive_expiry(true);
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.size(), 2U);
    EXPECT_EQ(s.evicted_keys(), 0U);
}

TEST(StoreEviction, LoadingNeverEvicts) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", kTenChars);
    s.set_maxmemory(1);
    s.set_loading(true);
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.size(), 1U);
}

TEST(StoreEviction, ZeroMeansUnlimited) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", std::string(5000, 'x'));
    EXPECT_EQ(s.maxmemory(), 0U);
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.size(), 1U);
}

TEST(StoreEviction, WorkPerCallIsBounded) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    for (int i = 0; i < 10100; ++i) {
        s.set("k" + std::to_string(i), "v");
    }
    s.set_maxmemory(1);
    EXPECT_TRUE(s.make_room());  // the cap was hit, so the command is allowed to proceed
    EXPECT_EQ(s.evicted_keys(), emberdb::Store::kMaxEvictionsPerCall);
    EXPECT_EQ(s.size(), 100U);
    EXPECT_TRUE(s.make_room());  // the next call finishes the job
    EXPECT_EQ(s.size(), 0U);
    EXPECT_EQ(s.used_memory(), 0U);
    EXPECT_TRUE(s.check_invariants());
}

TEST(ParseBytes, AcceptsPlainNumbersAndUnits) {
    using emberdb::parse_bytes;
    EXPECT_EQ(parse_bytes("0").value(), 0U);
    EXPECT_EQ(parse_bytes("1024").value(), 1024U);
    EXPECT_EQ(parse_bytes("10k").value(), 10240U);
    EXPECT_EQ(parse_bytes("10KB").value(), 10240U);
    EXPECT_EQ(parse_bytes("5mb").value(), 5ULL * 1024 * 1024);
    EXPECT_EQ(parse_bytes("2G").value(), 2ULL * 1024 * 1024 * 1024);
}

TEST(ParseBytes, RejectsGarbageAndOverflow) {
    using emberdb::parse_bytes;
    EXPECT_FALSE(parse_bytes("").has_value());
    EXPECT_FALSE(parse_bytes("mb").has_value());
    EXPECT_FALSE(parse_bytes("-5").has_value());
    EXPECT_FALSE(parse_bytes("5 mb").has_value());
    EXPECT_FALSE(parse_bytes("5tb").has_value());
    EXPECT_FALSE(parse_bytes("1.5g").has_value());
    EXPECT_FALSE(parse_bytes("99999999999999999999").has_value());
    EXPECT_FALSE(parse_bytes("18446744073709551615k").has_value());
}

TEST(StoreCounters, ExpiredKeysAreCountedByEachPath) {
    FakeClock clk;
    emberdb::Store s(clk.fn());
    s.set("lazy", "1");
    s.expire_in("lazy", 10);
    s.set("swept", "1");
    s.expire_in("swept", 10);
    s.set("deleted", "1");
    s.expire_in("deleted", 10);
    s.set("kept", "1");
    clk.now += 10;
    EXPECT_EQ(s.expired_keys(), 0U);
    EXPECT_FALSE(s.exists("lazy"));  // found by a read
    EXPECT_EQ(s.expired_keys(), 1U);
    EXPECT_FALSE(s.del("deleted"));  // an already-expired key: the DEL finds it dead
    EXPECT_EQ(s.expired_keys(), 2U);
    EXPECT_EQ(s.purge_expired(10), 1U);  // found by the sweep
    EXPECT_EQ(s.expired_keys(), 3U);
    EXPECT_TRUE(s.exists("kept"));
}

TEST(StoreCounters, ExplicitDeletesAndEvictionsAreNotExpiries) {
    emberdb::Store s;
    s.set_eviction_policy(emberdb::EvictionPolicy::kAllKeysLru);
    s.set("a", "1");
    s.set("b", "1");
    s.set("c", "1");
    EXPECT_TRUE(s.del("a"));
    EXPECT_EQ(s.expired_keys(), 0U);
    s.set_maxmemory(s.used_memory() - 1);
    EXPECT_TRUE(s.make_room());
    EXPECT_EQ(s.evicted_keys(), 1U);
    EXPECT_EQ(s.expired_keys(), 0U);  // evictions have their own counter
}
