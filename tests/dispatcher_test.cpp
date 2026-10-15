#include <gtest/gtest.h>

#include "emberdb/dispatcher.hpp"

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
