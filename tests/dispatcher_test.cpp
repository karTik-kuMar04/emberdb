#include <gtest/gtest.h>

#include "emberdb/dispatcher.hpp"
#include "fake_clock.hpp"

using emberdb::Command;
using emberdb::execute;
using emberdb::Store;

TEST(Dispatcher, Ping) {
    Store s;
    EXPECT_EQ(execute(s, {"PING"}), "+PONG\r\n");
}

TEST(Dispatcher, SetThenGet) {
    Store s;
    EXPECT_EQ(execute(s, {"SET", "name", "ember"}), "+OK\r\n");
    EXPECT_EQ(execute(s, {"GET", "name"}), "$5\r\nember\r\n");
}

TEST(Dispatcher, GetMissingKeyReturnsNullBulk) {
    Store s;
    EXPECT_EQ(execute(s, {"GET", "nope"}), "$-1\r\n");
}

TEST(Dispatcher, CommandNamesAreCaseInsensitive) {
    Store s;
    EXPECT_EQ(execute(s, {"set", "k", "v"}), "+OK\r\n");
    EXPECT_EQ(execute(s, {"GeT", "k"}), "$1\r\nv\r\n");
}

TEST(Dispatcher, DelCountsRemovedKeys) {
    Store s;
    execute(s, {"SET", "a", "1"});
    execute(s, {"SET", "b", "2"});
    EXPECT_EQ(execute(s, {"DEL", "a", "b", "missing"}), ":2\r\n");
    EXPECT_EQ(execute(s, {"GET", "a"}), "$-1\r\n");
}

TEST(Dispatcher, UnknownCommand) {
    Store s;
    EXPECT_EQ(execute(s, {"FOO"}), "-ERR unknown command 'FOO'\r\n");
}

TEST(Dispatcher, WrongArgumentCount) {
    Store s;
    EXPECT_EQ(execute(s, {"GET"}), "-ERR wrong number of arguments for 'GET' command\r\n");
    EXPECT_EQ(execute(s, {"SET", "k"}), "-ERR wrong number of arguments for 'SET' command\r\n");
    EXPECT_EQ(execute(s, {"DEL"}), "-ERR wrong number of arguments for 'DEL' command\r\n");
}

TEST(Dispatcher, EmptyCommand) {
    Store s;
    EXPECT_EQ(execute(s, Command{}), "-ERR empty command\r\n");
}

TEST(Dispatcher, ErrorRepliesCannotInjectProtocolLines) {
    Store s;
    // The CR/LF inside the command name must not appear in the reply.
    EXPECT_EQ(execute(s, {"BAD\r\n+OK"}), "-ERR unknown command 'BAD  +OK'\r\n");
}

TEST(DispatcherExpiry, ExpireAndTtlCountDown) {
    FakeClock clk;
    Store s(clk.fn());
    execute(s, {"SET", "k", "v"});
    EXPECT_EQ(execute(s, {"TTL", "k"}), ":-1\r\n");
    EXPECT_EQ(execute(s, {"EXPIRE", "k", "10"}), ":1\r\n");
    EXPECT_EQ(execute(s, {"TTL", "k"}), ":10\r\n");
    clk.now += 4000;
    EXPECT_EQ(execute(s, {"TTL", "k"}), ":6\r\n");
    EXPECT_EQ(execute(s, {"PTTL", "k"}), ":6000\r\n");
    clk.now += 6000;
    EXPECT_EQ(execute(s, {"GET", "k"}), "$-1\r\n");
    EXPECT_EQ(execute(s, {"TTL", "k"}), ":-2\r\n");
}

TEST(DispatcherExpiry, ExpireOnMissingKeyReturnsZero) {
    Store s;
    EXPECT_EQ(execute(s, {"EXPIRE", "nope", "10"}), ":0\r\n");
}

TEST(DispatcherExpiry, ExpireRejectsBadArguments) {
    Store s;
    execute(s, {"SET", "k", "v"});
    EXPECT_EQ(execute(s, {"EXPIRE", "k", "abc"}),
              "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(execute(s, {"EXPIRE", "k", "1.5"}),
              "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(execute(s, {"EXPIRE", "k", "9223372036854775807"}),
              "-ERR invalid expire time in 'expire' command\r\n");
}

TEST(DispatcherExpiry, PersistRemovesTtl) {
    FakeClock clk;
    Store s(clk.fn());
    execute(s, {"SET", "k", "v"});
    execute(s, {"EXPIRE", "k", "10"});
    EXPECT_EQ(execute(s, {"PERSIST", "k"}), ":1\r\n");
    EXPECT_EQ(execute(s, {"TTL", "k"}), ":-1\r\n");
    EXPECT_EQ(execute(s, {"PERSIST", "k"}), ":0\r\n");
    EXPECT_EQ(execute(s, {"PERSIST", "nokey"}), ":0\r\n");
}

TEST(DispatcherExpiry, ExistsCountsLiveKeys) {
    FakeClock clk;
    Store s(clk.fn());
    execute(s, {"SET", "a", "1"});
    execute(s, {"SET", "b", "2"});
    EXPECT_EQ(execute(s, {"EXISTS", "a", "b", "missing"}), ":2\r\n");
    EXPECT_EQ(execute(s, {"EXISTS", "a", "a"}), ":2\r\n");
    execute(s, {"EXPIRE", "a", "1"});
    clk.now += 1000;
    EXPECT_EQ(execute(s, {"EXISTS", "a"}), ":0\r\n");
}

TEST(DispatcherCounters, IncrAndDecr) {
    Store s;
    EXPECT_EQ(execute(s, {"INCR", "n"}), ":1\r\n");
    EXPECT_EQ(execute(s, {"INCR", "n"}), ":2\r\n");
    EXPECT_EQ(execute(s, {"DECR", "n"}), ":1\r\n");
    EXPECT_EQ(execute(s, {"GET", "n"}), "$1\r\n1\r\n");
}

TEST(DispatcherCounters, IncrErrors) {
    Store s;
    execute(s, {"SET", "s", "abc"});
    EXPECT_EQ(execute(s, {"INCR", "s"}), "-ERR value is not an integer or out of range\r\n");
    execute(s, {"SET", "big", "9223372036854775807"});
    EXPECT_EQ(execute(s, {"INCR", "big"}), "-ERR increment or decrement would overflow\r\n");
    EXPECT_EQ(execute(s, {"INCR"}), "-ERR wrong number of arguments for 'INCR' command\r\n");
}

TEST(DispatcherPropagation, WritesAreLoggedAndReadsAreNot) {
    Store s;
    Command fx;
    execute(s, {"SET", "k", "v"}, &fx);
    EXPECT_EQ(fx, (Command{"SET", "k", "v"}));
    execute(s, {"GET", "k"}, &fx);
    EXPECT_TRUE(fx.empty());
    execute(s, {"DEL", "k"}, &fx);
    EXPECT_EQ(fx, (Command{"DEL", "k"}));
    execute(s, {"DEL", "k"}, &fx);  // nothing to delete
    EXPECT_TRUE(fx.empty());
}

TEST(DispatcherPropagation, ExpireIsRewrittenToAbsoluteTime) {
    FakeClock clk;  // starts at 1000
    Store s(clk.fn());
    Command fx;
    execute(s, {"SET", "k", "v"}, &fx);
    execute(s, {"EXPIRE", "k", "10"}, &fx);
    EXPECT_EQ(fx, (Command{"PEXPIREAT", "k", "11000"}));
    execute(s, {"PEXPIRE", "k", "500"}, &fx);
    EXPECT_EQ(fx, (Command{"PEXPIREAT", "k", "1500"}));
}

TEST(DispatcherPropagation, NonPositiveExpireBecomesDel) {
    FakeClock clk;
    Store s(clk.fn());
    Command fx;
    execute(s, {"SET", "k", "v"}, &fx);
    EXPECT_EQ(execute(s, {"EXPIRE", "k", "0"}, &fx), ":1\r\n");
    EXPECT_EQ(fx, (Command{"DEL", "k"}));
    EXPECT_EQ(execute(s, {"GET", "k"}), "$-1\r\n");
}

TEST(DispatcherPropagation, FailedAndNoopCommandsAreNotLogged) {
    Store s;
    Command fx;
    execute(s, {"EXPIRE", "missing", "10"}, &fx);
    EXPECT_TRUE(fx.empty());
    execute(s, {"SET", "str", "abc"}, &fx);
    execute(s, {"INCR", "str"}, &fx);  // fails: not an integer
    EXPECT_TRUE(fx.empty());
    execute(s, {"PERSIST", "str"}, &fx);  // no deadline to remove
    EXPECT_TRUE(fx.empty());
    execute(s, {"INCR", "n"}, &fx);
    EXPECT_EQ(fx, (Command{"INCR", "n"}));
}

TEST(DispatcherPropagation, PexpireatSetsAndDeletes) {
    FakeClock clk;
    Store s(clk.fn());
    Command fx;
    execute(s, {"SET", "k", "v"}, &fx);
    EXPECT_EQ(execute(s, {"PEXPIREAT", "k", "5000"}, &fx), ":1\r\n");
    EXPECT_EQ(fx, (Command{"PEXPIREAT", "k", "5000"}));
    EXPECT_EQ(execute(s, {"PTTL", "k"}), ":4000\r\n");
    EXPECT_EQ(execute(s, {"PEXPIREAT", "k", "500"}, &fx), ":1\r\n");  // already in the past
    EXPECT_EQ(fx, (Command{"DEL", "k"}));
    EXPECT_EQ(execute(s, {"EXISTS", "k"}), ":0\r\n");
}
