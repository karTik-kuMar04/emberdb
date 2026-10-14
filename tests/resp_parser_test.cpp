#include <gtest/gtest.h>

#include "emberdb/resp_parser.hpp"

using emberdb::ParseStatus;
using emberdb::parse_command;

TEST(RespParser, ParsesCompleteCommand) {
    const std::string input = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nember\r\n";
    auto r = parse_command(input);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.command, (std::vector<std::string>{"SET", "name", "ember"}));
    EXPECT_EQ(r.consumed, input.size());
}

TEST(RespParser, EmptyBufferIsIncomplete) {
    EXPECT_EQ(parse_command("").status, ParseStatus::kIncomplete);
}

TEST(RespParser, EveryTruncationIsIncomplete) {
    const std::string input = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    for (std::size_t len = 0; len < input.size(); ++len) {
        auto r = parse_command(std::string_view(input).substr(0, len));
        EXPECT_EQ(r.status, ParseStatus::kIncomplete) << "failed at prefix length " << len;
    }
    EXPECT_EQ(parse_command(input).status, ParseStatus::kComplete);
}

TEST(RespParser, ParsesOnlyFirstOfTwoPipelinedCommands) {
    const std::string first = "*1\r\n$4\r\nPING\r\n";
    const std::string second = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    auto r = parse_command(first + second);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.command, (std::vector<std::string>{"PING"}));
    EXPECT_EQ(r.consumed, first.size());

    auto r2 = parse_command(std::string_view(first + second).substr(r.consumed));
    ASSERT_EQ(r2.status, ParseStatus::kComplete);
    EXPECT_EQ(r2.command, (std::vector<std::string>{"GET", "k"}));
}

TEST(RespParser, ZeroLengthBulkString) {
    const std::string input = "*2\r\n$3\r\nSET\r\n$0\r\n\r\n";
    auto r = parse_command(input);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.command[1], "");
}

TEST(RespParser, PayloadMayContainCrlf) {
    const std::string payload = "a\r\nb";
    const std::string input = "*2\r\n$3\r\nSET\r\n$4\r\n" + payload + "\r\n";
    auto r = parse_command(input);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.command[1], payload);
}

TEST(RespParser, RejectsBadFirstByte) {
    EXPECT_EQ(parse_command("PING\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsNonNumericLength) {
    EXPECT_EQ(parse_command("*abc\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_command("*1\r\n$x\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsNegativeAndZeroArrayLength) {
    EXPECT_EQ(parse_command("*0\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_command("*-1\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsOversizedBulkLength) {
    EXPECT_EQ(parse_command("*1\r\n$999999999999\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsMissingTrailingCrlf) {
    EXPECT_EQ(parse_command("*1\r\n$4\r\nPINGxx").status, ParseStatus::kError);
}
