#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emberdb/metrics.hpp"
#include "prom_check.hpp"

using emberdb::DropReason;
using emberdb::Histogram;
using emberdb::kLatencyBoundsNs;
using emberdb::Metrics;
using emberdb::MetricsSnapshot;

namespace {

bool has_line(const std::string& text, const std::string& line) {
    return ("\n" + text).find("\n" + line + "\n") != std::string::npos;
}

Metrics sample_metrics() {
    const std::vector<std::string> names = {"SET", "GET", "PING"};
    Metrics m(names);
    m.record_command("GET", 400, false);
    m.record_command("GET", 500, false);
    m.record_command("SET", 501, false);
    m.record_command("FOO", 2'000'000, true);
    m.record_command("BAR", 5'000'000'000ULL, true);
    m.connections_accepted = 9;
    m.bytes_read = 100;
    m.connection_dropped(DropReason::kAuthTimeout);
    return m;
}

MetricsSnapshot sample_snapshot() {
    MetricsSnapshot s;
    s.uptime_seconds = 12.5;
    s.start_time_unix = 1'700'000'000;
    s.keys = 42;
    s.used_memory = 4096;
    s.max_memory = 8192;
    s.aof_enabled = true;
    s.aof_size_bytes = 1000;
    s.auth_required = true;
    return s;
}

}  // namespace

TEST(HistogramBuckets, PutsValuesInTheRightBuckets) {
    Histogram h;
    h.observe(400);                // below the first bound
    h.observe(500);                // exactly the first bound: still counts as <= 500
    h.observe(501);                // just above it
    h.observe(2'000'000);          // 2 ms
    h.observe(5'000'000'000ULL);   // beyond every bound: only the +Inf bucket gets it
    EXPECT_EQ(h.cumulative(0), 2U);
    EXPECT_EQ(h.cumulative(1), 3U);
    EXPECT_EQ(h.cumulative(kLatencyBoundsNs.size() - 1), 4U);
    EXPECT_EQ(h.count(), 5U);
}

TEST(HistogramBuckets, TracksTheSum) {
    Histogram h;
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.sum_ns(), 0U);
    h.observe(100);
    h.observe(250);
    EXPECT_EQ(h.sum_ns(), 350U);
}

TEST(MetricsCounting, KnownAndUnknownCommands) {
    const std::vector<std::string> names = {"GET", "SET"};
    Metrics m(names);
    m.record_command("GET", 10, false);
    m.record_command("GET", 10, false);
    m.record_command("NOPE", 10, true);
    ASSERT_EQ(m.commands().size(), 2U);
    EXPECT_EQ(m.commands()[0].first, "GET");
    EXPECT_EQ(m.commands()[0].second, 2U);
    EXPECT_EQ(m.commands()[1].second, 0U);
    EXPECT_EQ(m.other_commands(), 1U);
    EXPECT_EQ(m.command_errors, 1U);
    EXPECT_EQ(m.command_latency().count(), 3U);
}

TEST(MetricsCounting, NamesAreSortedAndDeduplicated) {
    const std::vector<std::string> names = {"SET", "GET", "SET", "DEL"};
    Metrics m(names);
    ASSERT_EQ(m.commands().size(), 3U);
    EXPECT_EQ(m.commands()[0].first, "DEL");
    EXPECT_EQ(m.commands()[1].first, "GET");
    EXPECT_EQ(m.commands()[2].first, "SET");
}

TEST(MetricsRender, OutputIsValidExposition) {
    EXPECT_EQ(check_exposition(emberdb::render_prometheus(sample_metrics(), sample_snapshot())), "");
    // Even a server that has done nothing must produce a valid scrape.
    const std::vector<std::string> none;
    EXPECT_EQ(check_exposition(emberdb::render_prometheus(Metrics(none), MetricsSnapshot{})), "");
}

TEST(MetricsRender, ShowsCountersAndGauges) {
    const std::string out = emberdb::render_prometheus(sample_metrics(), sample_snapshot());
    EXPECT_TRUE(has_line(out, "emberdb_build_info{version=\"" + std::string(emberdb::kVersion) + "\"} 1"));
    EXPECT_TRUE(has_line(out, "emberdb_start_time_seconds 1700000000"));
    EXPECT_TRUE(has_line(out, "emberdb_uptime_seconds 12.500"));
    EXPECT_TRUE(has_line(out, "emberdb_commands_processed_total{command=\"GET\"} 2"));
    EXPECT_TRUE(has_line(out, "emberdb_commands_processed_total{command=\"SET\"} 1"));
    EXPECT_TRUE(has_line(out, "emberdb_commands_processed_total{command=\"PING\"} 0"));
    EXPECT_TRUE(has_line(out, "emberdb_commands_processed_total{command=\"other\"} 2"));
    EXPECT_TRUE(has_line(out, "emberdb_command_errors_total 2"));
    EXPECT_TRUE(has_line(out, "emberdb_connections_accepted_total 9"));
    EXPECT_TRUE(has_line(out, "emberdb_connections_dropped_total{reason=\"auth_timeout\"} 1"));
    EXPECT_TRUE(has_line(out, "emberdb_network_read_bytes_total 100"));
    EXPECT_TRUE(has_line(out, "emberdb_keys 42"));
    EXPECT_TRUE(has_line(out, "emberdb_memory_used_bytes 4096"));
    EXPECT_TRUE(has_line(out, "emberdb_memory_limit_bytes 8192"));
    EXPECT_TRUE(has_line(out, "emberdb_aof_enabled 1"));
    EXPECT_TRUE(has_line(out, "emberdb_aof_size_bytes 1000"));
    EXPECT_TRUE(has_line(out, "emberdb_auth_required 1"));
    EXPECT_TRUE(has_line(out, "emberdb_replication_follower 0"));
}

TEST(MetricsRender, HistogramBucketsAreCumulative) {
    const std::string out = emberdb::render_prometheus(sample_metrics(), sample_snapshot());
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_bucket{le=\"5e-07\"} 2"));
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_bucket{le=\"1e-06\"} 3"));
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_bucket{le=\"0.0025\"} 4"));
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_bucket{le=\"1\"} 4"));
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_bucket{le=\"+Inf\"} 5"));
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_count 5"));
    EXPECT_TRUE(has_line(out, "emberdb_command_duration_seconds_sum 5.002001401"));
}

TEST(MetricsRender, UnknownCommandsDoNotCreateSeries) {
    const std::vector<std::string> names = {"GET"};
    Metrics few(names);
    Metrics many(names);
    few.record_command("JUNK", 100, true);
    for (int i = 0; i < 10000; ++i) {
        many.record_command("JUNK" + std::to_string(i), 100, true);
    }
    const std::string a = emberdb::render_prometheus(few, MetricsSnapshot{});
    const std::string b = emberdb::render_prometheus(many, MetricsSnapshot{});
    EXPECT_EQ(b.find("JUNK"), std::string::npos);
    EXPECT_LT(b.size(), a.size() + 200);  // only the digits of the counters grew
}

TEST(MetricsRender, EveryDropReasonHasASeries) {
    const std::string out = emberdb::render_prometheus(sample_metrics(), sample_snapshot());
    for (std::size_t i = 0; i < emberdb::kDropReasonCount; ++i) {
        const std::string reason = emberdb::drop_reason_name(static_cast<DropReason>(i));
        EXPECT_NE(reason, "unknown");
        EXPECT_NE(out.find("emberdb_connections_dropped_total{reason=\"" + reason + "\"}"),
                  std::string::npos)
            << reason;
    }
}
