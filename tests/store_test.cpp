#include <gtest/gtest.h>

#include "emberdb/store.hpp"

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
