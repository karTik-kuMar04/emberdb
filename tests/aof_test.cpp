#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "emberdb/aof.hpp"
#include "emberdb/dispatcher.hpp"
#include "emberdb/resp_writer.hpp"
#include "fake_clock.hpp"

using emberdb::Aof;
using emberdb::Command;
using emberdb::Store;
using emberdb::execute;
namespace resp = emberdb::resp;

namespace {

// Does what the server does around each command: run it, then log expiries and the effect.
std::string run(Store& s, Aof& aof, const Command& cmd) {
    Command effect;
    std::string reply = execute(s, cmd, &effect);
    for (const std::string& key : s.take_expired()) {
        aof.append(resp::command({"DEL", key}));
    }
    if (!effect.empty()) {
        aof.append(resp::command(effect));
    }
    aof.commit();
    return reply;
}

class AofTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tpl[] = "/tmp/emberdb_aof_XXXXXX";
        const int fd = ::mkstemp(tpl);
        ASSERT_GE(fd, 0);
        ::close(fd);
        path_ = tpl;
    }
    void TearDown() override { ::unlink(path_.c_str()); }

    std::string path_;
};

}  // namespace

TEST_F(AofTest, RestoresDataAfterRestart) {
    {
        Store s;
        auto aof = Aof::open(path_, Aof::Fsync::kAlways, s);
        run(s, *aof, {"SET", "a", "1"});
        run(s, *aof, {"SET", "b", "2"});
        run(s, *aof, {"DEL", "a"});
        run(s, *aof, {"INCR", "n"});
        run(s, *aof, {"INCR", "n"});
    }
    Store s2;
    Aof::LoadStats stats;
    auto aof2 = Aof::open(path_, Aof::Fsync::kNever, s2, &stats);
    EXPECT_EQ(stats.commands, 5U);
    EXPECT_EQ(stats.tail_bytes, 0U);
    EXPECT_FALSE(s2.get("a").has_value());
    EXPECT_EQ(s2.get("b").value(), "2");
    EXPECT_EQ(s2.get("n").value(), "2");
}

TEST_F(AofTest, TornTailIsRepaired) {
    {
        Store s;
        auto aof = Aof::open(path_, Aof::Fsync::kNever, s);
        run(s, *aof, {"SET", "a", "1"});
    }
    const auto good = std::filesystem::file_size(path_);
    std::ofstream(path_, std::ios::app | std::ios::binary) << "*3\r\n$3\r\nSET\r\n$1\r\nb";

    Store s2;
    Aof::LoadStats stats;
    auto aof2 = Aof::open(path_, Aof::Fsync::kNever, s2, &stats);
    EXPECT_GT(stats.tail_bytes, 0U);
    EXPECT_EQ(stats.valid_bytes, static_cast<std::size_t>(good));
    EXPECT_EQ(std::filesystem::file_size(path_), good);  // the half command is gone
    EXPECT_EQ(s2.get("a").value(), "1");
    EXPECT_FALSE(s2.get("b").has_value());

    run(s2, *aof2, {"SET", "c", "3"});  // new records start on a clean boundary
    aof2.reset();
    Store s3;
    auto aof3 = Aof::open(path_, Aof::Fsync::kNever, s3);
    EXPECT_EQ(s3.get("a").value(), "1");
    EXPECT_EQ(s3.get("c").value(), "3");
}

TEST_F(AofTest, CorruptFileRefusesToStart) {
    std::ofstream(path_, std::ios::trunc | std::ios::binary) << "garbage\r\n";
    Store s;
    EXPECT_THROW(Aof::open(path_, Aof::Fsync::kNever, s), std::runtime_error);
}

TEST_F(AofTest, UnknownCommandInFileRefusesToStart) {
    std::ofstream(path_, std::ios::trunc | std::ios::binary) << "*1\r\n$3\r\nFOO\r\n";
    Store s;
    EXPECT_THROW(Aof::open(path_, Aof::Fsync::kNever, s), std::runtime_error);
}

TEST_F(AofTest, ExpiryUsesAbsoluteTimeAcrossRestarts) {
    FakeClock clk;  // starts at 1000
    {
        Store s(clk.fn());
        auto aof = Aof::open(path_, Aof::Fsync::kNever, s);
        run(s, *aof, {"SET", "k", "v"});
        run(s, *aof, {"EXPIRE", "k", "10"});  // deadline = 11000
    }
    FakeClock later;
    later.now = 6000;  // restarted 5 seconds later: 5 seconds should remain
    {
        Store s2(later.fn());
        auto aof2 = Aof::open(path_, Aof::Fsync::kNever, s2);
        EXPECT_EQ(s2.ttl_ms("k"), 5000);
    }
    FakeClock much_later;
    much_later.now = 20000;  // restarted after the deadline: the key must be gone
    Store s3(much_later.fn());
    auto aof3 = Aof::open(path_, Aof::Fsync::kNever, s3);
    EXPECT_FALSE(s3.exists("k"));
}

TEST_F(AofTest, ExpiredKeyDoesNotResurrectOnReplay) {
    FakeClock clk;
    {
        Store s(clk.fn());
        s.set_track_expired(true);
        auto aof = Aof::open(path_, Aof::Fsync::kNever, s);
        run(s, *aof, {"SET", "k", "5"});
        run(s, *aof, {"EXPIRE", "k", "10"});
        clk.now += 20000;  // the key expires...
        // ...so INCR starts a brand new counter at 1, not 6.
        EXPECT_EQ(run(s, *aof, {"INCR", "k"}), ":1\r\n");
    }
    FakeClock again;
    again.now = clk.now;
    Store s2(again.fn());
    auto aof2 = Aof::open(path_, Aof::Fsync::kNever, s2);
    EXPECT_EQ(s2.get("k").value(), "1");  // without the logged DEL this would be "6"
    EXPECT_EQ(s2.ttl_ms("k"), -1);
}

TEST_F(AofTest, AlwaysPolicyWritesBeforeReturning) {
    Store s;
    auto aof = Aof::open(path_, Aof::Fsync::kAlways, s);
    run(s, *aof, {"SET", "a", "1"});
    EXPECT_GT(std::filesystem::file_size(path_), 0U);  // on disk already, aof still open
}
