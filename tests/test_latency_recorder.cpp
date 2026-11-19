#include <gtest/gtest.h>
#include <martingale/performance/latency_recorder.hpp>

#include <cstdint>
#include <string>

namespace martingale::tests {

TEST(LatencyRecorderTest, EmptyRecorderReturnsZeroedStats) {
    const LatencyRecorder recorder;
    EXPECT_TRUE(recorder.empty());
    EXPECT_EQ(recorder.size(), 0U);

    const LatencyStats stats = recorder.compute_stats();
    EXPECT_EQ(stats.count, 0U);
    EXPECT_EQ(stats.min_ns, 0U);
    EXPECT_DOUBLE_EQ(stats.mean_ns, 0.0);
    EXPECT_EQ(stats.p50_ns, 0U);
    EXPECT_EQ(stats.p90_ns, 0U);
    EXPECT_EQ(stats.p95_ns, 0U);
    EXPECT_EQ(stats.p99_ns, 0U);
    EXPECT_EQ(stats.p99_9_ns, 0U);
    EXPECT_EQ(stats.max_ns, 0U);
    EXPECT_DOUBLE_EQ(stats.throughput_ops_per_sec, 0.0);
}

TEST(LatencyRecorderTest, ComputesExactPercentilesAndMeanOnDeterministicDistribution) {
    LatencyRecorder recorder;
    recorder.reserve(1000U);
    for (std::uint64_t i = 1U; i <= 1000U; ++i) {
        recorder.record_ns(i);
    }

    EXPECT_FALSE(recorder.empty());
    EXPECT_EQ(recorder.size(), 1000U);

    const LatencyStats stats = recorder.compute_stats(0.001);
    EXPECT_EQ(stats.count, 1000U);
    EXPECT_EQ(stats.min_ns, 1U);
    EXPECT_DOUBLE_EQ(stats.mean_ns, 500.5);
    EXPECT_EQ(stats.p50_ns, 500U);
    EXPECT_EQ(stats.p90_ns, 900U);
    EXPECT_EQ(stats.p95_ns, 950U);
    EXPECT_EQ(stats.p99_ns, 990U);
    EXPECT_EQ(stats.p99_9_ns, 999U);
    EXPECT_EQ(stats.max_ns, 1000U);
    EXPECT_DOUBLE_EQ(stats.throughput_ops_per_sec, 1000000.0);
}

TEST(LatencyRecorderTest, HandlesUnsortedSamplesAndClear) {
    LatencyRecorder recorder;
    recorder.record_ns(500U);
    recorder.record_ns(10U);
    recorder.record_ns(100U);
    recorder.record_ns(50U);
    recorder.record_ns(200U);

    const LatencyStats stats = recorder.compute_stats();
    EXPECT_EQ(stats.count, 5U);
    EXPECT_EQ(stats.min_ns, 10U);
    EXPECT_EQ(stats.max_ns, 500U);
    EXPECT_EQ(stats.p50_ns, 100U);

    recorder.clear();
    EXPECT_TRUE(recorder.empty());
    EXPECT_EQ(recorder.size(), 0U);
}

TEST(LatencyRecorderTest, MeasureBlockAndCsvFormatting) {
    LatencyRecorder recorder;
    volatile std::uint64_t sink = 0U;
    recorder.measure([&]() {
        for (std::uint64_t i = 0U; i < 64U; ++i) {
            sink += i;
        }
    });
    static_cast<void>(sink);

    EXPECT_EQ(recorder.size(), 1U);
    const LatencyStats stats = recorder.compute_stats(0.0001);

    const std::string header = LatencyRecorder::to_csv_header();
    const std::string row = LatencyRecorder::to_csv_row("baseline", stats);
    EXPECT_NE(header.find("p50_ns"), std::string::npos);
    EXPECT_NE(header.find("p99_9_ns"), std::string::npos);
    EXPECT_NE(row.find("baseline,"), std::string::npos);
}

}  // namespace martingale::tests
