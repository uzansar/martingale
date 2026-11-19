#ifndef MARTINGALE_PERFORMANCE_LATENCY_RECORDER_HPP
#define MARTINGALE_PERFORMANCE_LATENCY_RECORDER_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace martingale {

/// Summary statistics and tail-latency percentiles computed from recorded samples.
struct LatencyStats {
    std::size_t count{0};
    std::uint64_t min_ns{0};
    double mean_ns{0.0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p95_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p99_9_ns{0};
    std::uint64_t max_ns{0};
    double throughput_ops_per_sec{0.0};
};

/// Collects nanosecond execution latency samples and computes exact nearest-rank
/// percentiles (P50, P90, P95, P99, P99.9, Max) plus CSV exports for offline analysis.
class LatencyRecorder {
public:
    void reserve(std::size_t capacity) { samples_ns_.reserve(capacity); }

    void record_ns(std::uint64_t elapsed_ns) { samples_ns_.push_back(elapsed_ns); }

    template <typename Func>
    auto measure(Func&& func) -> decltype(std::forward<Func>(func)()) {
        const auto start = std::chrono::steady_clock::now();
        if constexpr (std::is_void_v<decltype(std::forward<Func>(func)())>) {
            std::forward<Func>(func)();
            const auto end = std::chrono::steady_clock::now();
            record_ns(elapsed_nanoseconds(start, end));
        } else {
            auto result = std::forward<Func>(func)();
            const auto end = std::chrono::steady_clock::now();
            record_ns(elapsed_nanoseconds(start, end));
            return result;
        }
    }

    [[nodiscard]] LatencyStats compute_stats(double total_elapsed_seconds = 0.0) const {
        if (samples_ns_.empty()) {
            return LatencyStats{};
        }

        std::vector<std::uint64_t> sorted = samples_ns_;
        std::ranges::sort(sorted);

        const std::size_t n = sorted.size();
        const std::uint64_t sum = std::accumulate(sorted.begin(), sorted.end(), std::uint64_t{0});
        const double mean = static_cast<double>(sum) / static_cast<double>(n);
        const double throughput =
            (total_elapsed_seconds > 0.0) ? (static_cast<double>(n) / total_elapsed_seconds) : 0.0;

        return LatencyStats{
            .count = n,
            .min_ns = sorted.front(),
            .mean_ns = mean,
            .p50_ns = nearest_rank_percentile(sorted, 500U),
            .p90_ns = nearest_rank_percentile(sorted, 900U),
            .p95_ns = nearest_rank_percentile(sorted, 950U),
            .p99_ns = nearest_rank_percentile(sorted, 990U),
            .p99_9_ns = nearest_rank_percentile(sorted, 999U),
            .max_ns = sorted.back(),
            .throughput_ops_per_sec = throughput,
        };
    }

    void clear() noexcept { samples_ns_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return samples_ns_.size(); }
    [[nodiscard]] bool empty() const noexcept { return samples_ns_.empty(); }

    [[nodiscard]] static std::string to_csv_header() {
        return "label,count,min_ns,mean_ns,p50_ns,p90_ns,p95_ns,p99_ns,p99_9_ns,max_ns,"
               "throughput_ops_per_sec";
    }

    [[nodiscard]] static std::string to_csv_row(std::string_view label,
                                                const LatencyStats& stats) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << label << ',' << stats.count << ',' << stats.min_ns << ',' << stats.mean_ns << ','
            << stats.p50_ns << ',' << stats.p90_ns << ',' << stats.p95_ns << ',' << stats.p99_ns
            << ',' << stats.p99_9_ns << ',' << stats.max_ns << ',' << stats.throughput_ops_per_sec;
        return oss.str();
    }

private:
    [[nodiscard]] static std::uint64_t elapsed_nanoseconds(
        std::chrono::steady_clock::time_point start,
        std::chrono::steady_clock::time_point end) noexcept {
        const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        return delta > 0 ? static_cast<std::uint64_t>(delta) : 0U;
    }

    [[nodiscard]] static std::uint64_t nearest_rank_percentile(
        const std::vector<std::uint64_t>& sorted, std::size_t per_mille) noexcept {
        // Integer ceiling division avoids IEEE-754 drift such as 99.9 / 100.0 * 1000 = 999.0000000000001.
        const std::size_t rank = (per_mille * sorted.size() + 999U) / 1000U;
        const std::size_t idx = (rank == 0U) ? 0U : std::min(rank - 1U, sorted.size() - 1U);
        return sorted[idx];
    }

    std::vector<std::uint64_t> samples_ns_{};
};

}  // namespace martingale

#endif  // MARTINGALE_PERFORMANCE_LATENCY_RECORDER_HPP
