#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "emberdb/dispatcher.hpp"
#include "emberdb/resp_parser.hpp"
#include "emberdb/snapshot.hpp"
#include "fake_clock.hpp"

using emberdb::ParseResult;
using emberdb::ParseStatus;
using emberdb::Store;
using emberdb::write_snapshot;

namespace {

std::string dump(const Store& s, std::size_t chunk = 64 * 1024) {
    std::string out;
    write_snapshot(s, [&](std::string_view piece) { out.append(piece); }, chunk);
    return out;
}

// Replays a snapshot the same way the AOF loader does: parse a command, execute it.
void load(Store& s, const std::string& data) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        ParseResult r = emberdb::parse_command(std::string_view(data).substr(pos));
        ASSERT_EQ(r.status, ParseStatus::kComplete);
        emberdb::execute(s, r.command);
        pos += r.consumed;
    }
}

}  // namespace

TEST(StoreIteration, VisitsLiveKeysWithTheirDeadlines) {
    FakeClock clk;  // starts at 1000
    Store s(clk.fn());
    s.set("plain", "1");
    s.set("timed", "2");
    s.expire_in("timed", 500);
    s.set("old", "3");
    s.expire_in("old", 10);
    clk.now += 10;  // "old" has now expired

    std::map<std::string, std::string> values;
    std::map<std::string, std::optional<std::int64_t>> deadlines;
    s.for_each_live([&](const std::string& k, const std::string& v, std::optional<std::int64_t> at) {
        values[k] = v;
        deadlines[k] = at;
    });
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values["plain"], "1");
    EXPECT_FALSE(deadlines["plain"].has_value());
    EXPECT_EQ(deadlines["timed"].value(), 1500);
}

TEST(StoreIteration, DoesNotPurgeAnything) {
    FakeClock clk;
    Store s(clk.fn());
    s.set("a", "1");
    s.expire_in("a", 5);
    clk.now += 5;
    s.for_each_live([](const std::string&, const std::string&, std::optional<std::int64_t>) {});
    EXPECT_EQ(s.size(), 1U);  // still in memory: purging is the expiry code's job
}

TEST(Snapshot, EmptyStoreProducesNothing) {
    Store s;
    EXPECT_TRUE(dump(s).empty());
}

TEST(Snapshot, RoundTripKeepsValuesAndDeadlines) {
    FakeClock clk;
    Store a(clk.fn());
    a.set("plain", "1");
    a.set("timed", "2");
    a.expire_in("timed", 10000);

    Store b(clk.fn());
    load(b, dump(a));
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b.get("plain").value(), "1");
    EXPECT_EQ(b.get("timed").value(), "2");
    EXPECT_EQ(b.ttl_ms("plain"), -1);
    EXPECT_EQ(b.ttl_ms("timed"), 10000);
}

TEST(Snapshot, SkipsExpiredKeys) {
    FakeClock clk;
    Store a(clk.fn());
    a.set("old", "1");
    a.expire_in("old", 10);
    a.set("live", "2");
    clk.now += 10;

    EXPECT_EQ(write_snapshot(a, [](std::string_view) {}), 1U);
    Store b(clk.fn());
    load(b, dump(a));
    EXPECT_EQ(b.size(), 1U);
    EXPECT_FALSE(b.exists("old"));
}

TEST(Snapshot, ValuesWithCrlfAndBinaryBytesSurvive) {
    const std::string weird("x\r\n\0y\xff", 6);
    Store a;
    a.set("bin", weird);
    Store b;
    load(b, dump(a));
    EXPECT_EQ(b.get("bin").value(), weird);
}

TEST(Snapshot, ChunkSizeControlsHowOftenTheSinkIsCalled) {
    Store s;
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    std::size_t calls = 0;
    std::string joined;
    const std::size_t keys = write_snapshot(
        s,
        [&](std::string_view piece) {
            ++calls;
            joined.append(piece);
        },
        1);
    EXPECT_EQ(keys, 3U);
    EXPECT_EQ(calls, 3U);          // every key crossed the 1-byte threshold on its own
    EXPECT_EQ(joined, dump(s));    // and the pieces add up to the same bytes
}
