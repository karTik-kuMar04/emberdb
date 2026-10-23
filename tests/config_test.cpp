#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "emberdb/config.hpp"

using emberdb::parse_config;

TEST(Config, ParsesKeyValuePairs) {
    const auto e = parse_config("port 6380\nbind 127.0.0.1\n");
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(e[0].key, "port");
    EXPECT_EQ(e[0].value, "6380");
    EXPECT_EQ(e[0].line, 1U);
    EXPECT_EQ(e[1].key, "bind");
    EXPECT_EQ(e[1].value, "127.0.0.1");
    EXPECT_EQ(e[1].line, 2U);
}

TEST(Config, IgnoresCommentsAndBlankLines) {
    const auto e = parse_config("# a comment\n\n   \n  # indented comment\nport 1\n");
    ASSERT_EQ(e.size(), 1U);
    EXPECT_EQ(e[0].key, "port");
    EXPECT_EQ(e[0].line, 5U);
}

TEST(Config, ValueIsTheRestOfTheLineAndMayContainAHash) {
    const auto e = parse_config("requirepass  my pass#word with spaces  \n");
    ASSERT_EQ(e.size(), 1U);
    EXPECT_EQ(e[0].value, "my pass#word with spaces");
}

TEST(Config, KeysAreLowercasedAndTabsSeparate) {
    const auto e = parse_config("MaxMemory\t100mb\n");
    ASSERT_EQ(e.size(), 1U);
    EXPECT_EQ(e[0].key, "maxmemory");
    EXPECT_EQ(e[0].value, "100mb");
}

TEST(Config, HandlesCrlfAndMissingFinalNewline) {
    const auto e = parse_config("port 1\r\nbind 127.0.0.1");
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(e[0].value, "1");
    EXPECT_EQ(e[1].value, "127.0.0.1");
}

TEST(Config, MissingValueNamesTheLine) {
    try {
        parse_config("port 1\n\nbind\n");
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("line 3"), std::string::npos);
        EXPECT_NE(what.find("bind"), std::string::npos);
    }
}

TEST(Config, EmptyTextGivesNoEntries) {
    EXPECT_TRUE(parse_config("").empty());
    EXPECT_TRUE(parse_config("\n\n# nothing here").empty());
}
