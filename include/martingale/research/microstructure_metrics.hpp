#ifndef MARTINGALE_RESEARCH_MICROSTRUCTURE_METRICS_HPP
#define MARTINGALE_RESEARCH_MICROSTRUCTURE_METRICS_HPP

#include <martingale/book/order_book.hpp>
#include <martingale/core/trade.hpp>
#include <martingale/core/types.hpp>
#include <martingale/market_data/market_data_publisher.hpp>
#include <martingale/simulator/market_simulator.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace martingale {

/// Execution cost and permanent/temporary market impact decomposition for a parent order.
/// All signed quantities are positive when moving adversely against the parent order side.
/// Identity: slippage_ticks == permanent_impact_ticks + temporary_impact_ticks.
struct ExecutionImpactMetrics {
    Side side{Side::Buy};
    Quantity executed_quantity{0};
    Price arrival_mid{kInvalidPrice};
    double avg_execution_price{0.0};
    double post_trade_mid{0.0};
    double slippage_ticks{0.0};
    double slippage_bps{0.0};
    double permanent_impact_ticks{0.0};
    double permanent_impact_bps{0.0};
    double temporary_impact_ticks{0.0};
    double temporary_impact_bps{0.0};
};

/// Order flow and trade arrival rate metrics computed over a time window.
struct TradeIntensityMetrics {
    std::size_t trade_count{0};
    Quantity total_volume{0};
    Quantity buy_aggressor_volume{0};
    Quantity sell_aggressor_volume{0};
    double order_flow_imbalance{0.0};
    double trades_per_second{0.0};
    double volume_per_second{0.0};
};

/// Aggregated resting liquidity profile at a given depth level across L2 snapshots.
struct LiquidityDepthBucket {
    std::size_t level_index{0};
    double mean_bid_quantity{0.0};
    double mean_ask_quantity{0.0};
    double mean_total_quantity{0.0};
    double share_of_book_liquidity{0.0};
};

/// End-to-end microstructure diagnostic summary for a simulated market session.
struct MicrostructureSummary {
    std::size_t num_steps{0};
    std::size_t num_snapshots{0};
    double mean_spread_ticks{0.0};
    double mean_spread_bps{0.0};
    double mean_l1_imbalance{0.0};
    double mean_top3_imbalance{0.0};
    double vwap{0.0};
    double realized_volatility_bps{0.0};
    TradeIntensityMetrics trade_intensity{};
    std::optional<ExecutionImpactMetrics> institutional_impact{};
    std::vector<LiquidityDepthBucket> liquidity_profile{};
};

/// Stateless quantitative analytics engine and CSV exporter for exchange simulation telemetry.
class MicrostructureAnalyzer {
public:
    /// Computes the raw bid-ask spread in price ticks, or nullopt if either side is empty.
    [[nodiscard]] static constexpr std::optional<double> bid_ask_spread(
        std::optional<Price> best_bid, std::optional<Price> best_ask) noexcept {
        if (!is_valid_price(best_bid) || !is_valid_price(best_ask)) {
            return std::nullopt;
        }
        return static_cast<double>(*best_ask - *best_bid);
    }

    /// Computes the arithmetic mid-price (best_bid + best_ask) / 2.
    [[nodiscard]] static constexpr std::optional<double> mid_price(
        std::optional<Price> best_bid, std::optional<Price> best_ask) noexcept {
        if (!is_valid_price(best_bid) || !is_valid_price(best_ask)) {
            return std::nullopt;
        }
        return 0.5 * (static_cast<double>(*best_bid) + static_cast<double>(*best_ask));
    }

    /// Computes relative bid-ask spread in basis points: (spread / mid) * 10,000.
    [[nodiscard]] static constexpr std::optional<double> relative_spread_bps(
        std::optional<Price> best_bid, std::optional<Price> best_ask) noexcept {
        const auto spread = bid_ask_spread(best_bid, best_ask);
        const auto mid = mid_price(best_bid, best_ask);
        if (!spread.has_value() || !mid.has_value() || *mid <= 0.0) {
            return std::nullopt;
        }
        return (*spread / *mid) * 10000.0;
    }

    /// Computes the BBO volume-weighted microprice:
    /// (best_bid * ask_qty + best_ask * bid_qty) / (bid_qty + ask_qty).
    [[nodiscard]] static constexpr std::optional<double> microprice(
        std::optional<Price> best_bid, Quantity best_bid_qty,
        std::optional<Price> best_ask, Quantity best_ask_qty) noexcept {
        if (!is_valid_price(best_bid) || !is_valid_price(best_ask)) {
            return std::nullopt;
        }
        const Quantity total_qty = best_bid_qty + best_ask_qty;
        if (total_qty == 0U) {
            return std::nullopt;
        }
        const double bid_px = static_cast<double>(*best_bid);
        const double ask_px = static_cast<double>(*best_ask);
        const double q_bid = static_cast<double>(best_bid_qty);
        const double q_ask = static_cast<double>(best_ask_qty);
        return (bid_px * q_ask + ask_px * q_bid) / static_cast<double>(total_qty);
    }

    /// Computes Level-1 Order Book Imbalance: (bid_qty - ask_qty) / (bid_qty + ask_qty) in [-1, 1].
    [[nodiscard]] static constexpr double order_book_imbalance(Quantity bid_qty,
                                                               Quantity ask_qty) noexcept {
        const Quantity total_qty = bid_qty + ask_qty;
        if (total_qty == 0U) {
            return 0.0;
        }
        const double bid_d = static_cast<double>(bid_qty);
        const double ask_d = static_cast<double>(ask_qty);
        return (bid_d - ask_d) / static_cast<double>(total_qty);
    }

    /// Computes multi-level Order Book Imbalance aggregated over the top `depth_levels` book levels.
    [[nodiscard]] static double order_book_imbalance(const OrderBookSnapshot& snapshot,
                                                     std::size_t depth_levels = 1U) noexcept {
        if (depth_levels == 0U) {
            return 0.0;
        }
        const std::size_t bid_limit = std::min(depth_levels, snapshot.bids.size());
        const std::size_t ask_limit = std::min(depth_levels, snapshot.asks.size());

        Quantity bid_vol = 0U;
        for (std::size_t i = 0U; i < bid_limit; ++i) {
            bid_vol += snapshot.bids[i].quantity;
        }

        Quantity ask_vol = 0U;
        for (std::size_t i = 0U; i < ask_limit; ++i) {
            ask_vol += snapshot.asks[i].quantity;
        }

        return order_book_imbalance(bid_vol, ask_vol);
    }

    /// Computes Volume-Weighted Average Price across executed trades, or nullopt if empty.
    [[nodiscard]] static std::optional<double> compute_vwap(std::span<const Trade> trades) noexcept {
        long double notional_sum = 0.0L;
        Quantity volume_sum = 0U;
        for (const Trade& trade : trades) {
            notional_sum += static_cast<long double>(trade.price) *
                            static_cast<long double>(trade.quantity);
            volume_sum += trade.quantity;
        }
        if (volume_sum == 0U) {
            return std::nullopt;
        }
        return static_cast<double>(notional_sum / static_cast<long double>(volume_sum));
    }

    /// Computes sample standard deviation of log returns ln(P_t / P_{t-1}) for a price series.
    /// Returns 0.0 when fewer than two consecutive valid returns exist.
    [[nodiscard]] static double compute_realized_volatility(
        std::span<const double> prices) noexcept {
        std::vector<double> returns;
        if (prices.size() < 3U) {
            return 0.0;
        }
        returns.reserve(prices.size() - 1U);

        for (std::size_t i = 1U; i < prices.size(); ++i) {
            if (prices[i - 1U] > 0.0 && prices[i] > 0.0) {
                returns.push_back(std::log(prices[i] / prices[i - 1U]));
            }
        }
        if (returns.size() < 2U) {
            return 0.0;
        }

        double mean = 0.0;
        for (const double r : returns) {
            mean += r;
        }
        mean /= static_cast<double>(returns.size());

        double sq_diff_sum = 0.0;
        for (const double r : returns) {
            const double diff = r - mean;
            sq_diff_sum += diff * diff;
        }
        const double variance = sq_diff_sum / static_cast<double>(returns.size() - 1U);
        return std::sqrt(variance);
    }

    /// Computes realized volatility in basis points from a price series.
    [[nodiscard]] static double compute_realized_volatility_bps(
        std::span<const double> prices) noexcept {
        return compute_realized_volatility(prices) * 10000.0;
    }

    /// Computes realized volatility in basis points from step-by-step mid-price observations.
    [[nodiscard]] static double compute_realized_volatility_bps(
        std::span<const SimulationStepRecord> steps) {
        std::vector<double> mids;
        mids.reserve(steps.size());
        for (const auto& step : steps) {
            const auto bbo_mid = mid_price(extract_price(step.best_bid), extract_price(step.best_ask));
            if (bbo_mid.has_value()) {
                mids.push_back(*bbo_mid);
            } else if (step.reference_mid_price > 0 && step.reference_mid_price != kInvalidPrice) {
                mids.push_back(static_cast<double>(step.reference_mid_price));
            }
        }
        return compute_realized_volatility_bps(mids);
    }

    /// Computes trade arrival rate, volume rate, and aggressive order flow imbalance.
    /// When `window_start_ns == 0 && window_end_ns == 0`, infers the horizon from first/last trades.
    [[nodiscard]] static TradeIntensityMetrics compute_trade_intensity(
        std::span<const Trade> trades, Timestamp window_start_ns = 0ULL,
        Timestamp window_end_ns = 0ULL) noexcept {
        TradeIntensityMetrics metrics;
        metrics.trade_count = trades.size();

        for (const Trade& trade : trades) {
            metrics.total_volume += trade.quantity;
            if (trade.aggressor_side == Side::Buy) {
                metrics.buy_aggressor_volume += trade.quantity;
            } else {
                metrics.sell_aggressor_volume += trade.quantity;
            }
        }

        if (metrics.total_volume > 0U) {
            const double buy_d = static_cast<double>(metrics.buy_aggressor_volume);
            const double sell_d = static_cast<double>(metrics.sell_aggressor_volume);
            metrics.order_flow_imbalance =
                (buy_d - sell_d) / static_cast<double>(metrics.total_volume);
        }

        double duration_seconds = 0.0;
        if (window_end_ns > window_start_ns) {
            duration_seconds = static_cast<double>(window_end_ns - window_start_ns) * 1e-9;
        } else if (window_start_ns == 0ULL && window_end_ns == 0ULL && trades.size() >= 2U &&
                   trades.back().timestamp > trades.front().timestamp) {
            duration_seconds =
                static_cast<double>(trades.back().timestamp - trades.front().timestamp) * 1e-9;
        }

        if (duration_seconds > 0.0) {
            metrics.trades_per_second = static_cast<double>(metrics.trade_count) / duration_seconds;
            metrics.volume_per_second = static_cast<double>(metrics.total_volume) / duration_seconds;
        }

        return metrics;
    }

    /// Computes side-signed execution slippage decomposed into permanent and temporary market impact.
    [[nodiscard]] static constexpr ExecutionImpactMetrics compute_execution_impact(
        Side side, Price arrival_mid, double avg_execution_price, double post_trade_mid,
        Quantity executed_quantity) noexcept {
        ExecutionImpactMetrics impact;
        impact.side = side;
        impact.executed_quantity = executed_quantity;
        impact.arrival_mid = arrival_mid;
        impact.avg_execution_price = avg_execution_price;
        impact.post_trade_mid = post_trade_mid;

        const double sign = (side == Side::Buy) ? 1.0 : -1.0;
        const double arrival_d = static_cast<double>(arrival_mid);

        impact.slippage_ticks = sign * (avg_execution_price - arrival_d);
        impact.permanent_impact_ticks = sign * (post_trade_mid - arrival_d);
        impact.temporary_impact_ticks = impact.slippage_ticks - impact.permanent_impact_ticks;

        if (arrival_mid > 0) {
            const double to_bps = 10000.0 / arrival_d;
            impact.slippage_bps = impact.slippage_ticks * to_bps;
            impact.permanent_impact_bps = impact.permanent_impact_ticks * to_bps;
            impact.temporary_impact_bps = impact.temporary_impact_ticks * to_bps;
        }

        return impact;
    }

    /// Computes average depth quantity and share of resting liquidity per level across L2 snapshots.
    [[nodiscard]] static std::vector<LiquidityDepthBucket> compute_liquidity_distribution(
        std::span<const OrderBookSnapshot> snapshots) {
        if (snapshots.empty()) {
            return {};
        }

        std::size_t max_levels = 0U;
        for (const auto& snap : snapshots) {
            max_levels = std::max(max_levels, std::max(snap.bids.size(), snap.asks.size()));
        }
        if (max_levels == 0U) {
            return {};
        }

        std::vector<double> sum_bids(max_levels, 0.0);
        std::vector<double> sum_asks(max_levels, 0.0);
        for (const auto& snap : snapshots) {
            for (std::size_t lvl = 0U; lvl < snap.bids.size(); ++lvl) {
                sum_bids[lvl] += static_cast<double>(snap.bids[lvl].quantity);
            }
            for (std::size_t lvl = 0U; lvl < snap.asks.size(); ++lvl) {
                sum_asks[lvl] += static_cast<double>(snap.asks[lvl].quantity);
            }
        }

        const double inv_snaps = 1.0 / static_cast<double>(snapshots.size());
        std::vector<LiquidityDepthBucket> buckets(max_levels);
        double grand_total = 0.0;
        for (std::size_t lvl = 0U; lvl < max_levels; ++lvl) {
            buckets[lvl].level_index = lvl;
            buckets[lvl].mean_bid_quantity = sum_bids[lvl] * inv_snaps;
            buckets[lvl].mean_ask_quantity = sum_asks[lvl] * inv_snaps;
            buckets[lvl].mean_total_quantity =
                buckets[lvl].mean_bid_quantity + buckets[lvl].mean_ask_quantity;
            grand_total += buckets[lvl].mean_total_quantity;
        }

        if (grand_total > 0.0) {
            for (auto& bucket : buckets) {
                bucket.share_of_book_liquidity = bucket.mean_total_quantity / grand_total;
            }
        }
        return buckets;
    }

    /// Computes the complete microstructure summary for a finished SimulationResult.
    [[nodiscard]] static MicrostructureSummary analyze(const SimulationResult& result) {
        MicrostructureSummary summary;
        summary.num_steps = result.step_records.size();
        summary.num_snapshots = result.l2_snapshots.size();

        double spread_ticks_sum = 0.0;
        double spread_bps_sum = 0.0;
        std::size_t valid_spread_count = 0U;

        double l1_obi_sum = 0.0;
        std::size_t valid_l1_obi_count = 0U;

        for (const auto& step : result.step_records) {
            const auto bid = extract_price(step.best_bid);
            const auto ask = extract_price(step.best_ask);
            const auto spread = bid_ask_spread(bid, ask);
            const auto rel_bps = relative_spread_bps(bid, ask);
            if (spread.has_value() && rel_bps.has_value()) {
                spread_ticks_sum += *spread;
                spread_bps_sum += *rel_bps;
                ++valid_spread_count;
            }
            if (step.best_bid_quantity > 0U || step.best_ask_quantity > 0U) {
                l1_obi_sum +=
                    order_book_imbalance(step.best_bid_quantity, step.best_ask_quantity);
                ++valid_l1_obi_count;
            }
        }

        if (valid_spread_count > 0U) {
            const double inv = 1.0 / static_cast<double>(valid_spread_count);
            summary.mean_spread_ticks = spread_ticks_sum * inv;
            summary.mean_spread_bps = spread_bps_sum * inv;
        }
        if (valid_l1_obi_count > 0U) {
            summary.mean_l1_imbalance = l1_obi_sum / static_cast<double>(valid_l1_obi_count);
        }

        if (!result.l2_snapshots.empty()) {
            double top3_sum = 0.0;
            for (const auto& snap : result.l2_snapshots) {
                top3_sum += order_book_imbalance(snap, 3U);
            }
            summary.mean_top3_imbalance =
                top3_sum / static_cast<double>(result.l2_snapshots.size());
            summary.liquidity_profile = compute_liquidity_distribution(result.l2_snapshots);
        }

        summary.vwap = compute_vwap(result.trades).value_or(0.0);
        summary.realized_volatility_bps = compute_realized_volatility_bps(result.step_records);

        Timestamp window_start = 0ULL;
        Timestamp window_end = 0ULL;
        if (result.step_records.size() >= 2U) {
            window_start = result.step_records.front().timestamp;
            const Timestamp last_ts = result.step_records.back().timestamp;
            const Timestamp step_dt =
                (result.step_records[1].timestamp >= result.step_records[0].timestamp)
                    ? (result.step_records[1].timestamp - result.step_records[0].timestamp)
                    : 0ULL;
            window_end = last_ts + step_dt;
        }
        summary.trade_intensity = compute_trade_intensity(result.trades, window_start, window_end);

        if (result.institutional_arrival_mid.has_value() &&
            result.institutional_avg_exec_price.has_value() &&
            result.institutional_executed_qty > 0U) {
            double post_mid = static_cast<double>(*result.institutional_arrival_mid);
            if (!result.step_records.empty()) {
                const auto& last = result.step_records.back();
                const auto terminal_bbo_mid =
                    mid_price(extract_price(last.best_bid), extract_price(last.best_ask));
                if (terminal_bbo_mid.has_value()) {
                    post_mid = *terminal_bbo_mid;
                } else if (last.reference_mid_price > 0 &&
                           last.reference_mid_price != kInvalidPrice) {
                    post_mid = static_cast<double>(last.reference_mid_price);
                }
            }
            summary.institutional_impact = compute_execution_impact(
                result.institutional_side, *result.institutional_arrival_mid,
                *result.institutional_avg_exec_price, post_mid, result.institutional_executed_qty);
        }

        return summary;
    }

    /// Exports step-level BBO, spread, imbalance, and volume telemetry as CSV.
    [[nodiscard]] static std::string steps_to_csv(const SimulationResult& result) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << "step_index,timestamp_ns,reference_mid_price,best_bid,best_bid_qty,"
               "best_ask,best_ask_qty,mid_price,spread_ticks,l1_obi,trade_count,step_volume\n";

        for (const auto& step : result.step_records) {
            const auto bid = extract_price(step.best_bid);
            const auto ask = extract_price(step.best_ask);
            const auto mid = mid_price(bid, ask);
            const auto spread = bid_ask_spread(bid, ask);
            const double obi = order_book_imbalance(step.best_bid_quantity, step.best_ask_quantity);

            oss << step.step_index << ',' << step.timestamp << ',' << step.reference_mid_price
                << ',';
            if (bid.has_value()) {
                oss << *bid;
            }
            oss << ',' << step.best_bid_quantity << ',';
            if (ask.has_value()) {
                oss << *ask;
            }
            oss << ',' << step.best_ask_quantity << ',';
            if (mid.has_value()) {
                oss << *mid;
            }
            oss << ',';
            if (spread.has_value()) {
                oss << *spread;
            }
            oss << ',' << obi << ',' << step.trade_count << ',' << step.step_volume << '\n';
        }
        return oss.str();
    }

    /// Exports executed trade records as CSV.
    [[nodiscard]] static std::string trades_to_csv(std::span<const Trade> trades) {
        std::ostringstream oss;
        oss << "trade_id,sequence_number,timestamp_ns,instrument_id,price,quantity,"
               "aggressor_side,buyer_order_id,seller_order_id\n";

        for (const Trade& t : trades) {
            oss << t.trade_id << ',' << t.sequence_number << ',' << t.timestamp << ','
                << t.instrument_id << ',' << t.price << ',' << t.quantity << ','
                << side_label(t.aggressor_side) << ',' << t.buyer_order_id << ','
                << t.seller_order_id << '\n';
        }
        return oss.str();
    }

    /// Exports periodic L2 depth snapshots in long tidy format (one row per book level/side).
    [[nodiscard]] static std::string l2_snapshots_to_csv(
        std::span<const OrderBookSnapshot> snapshots) {
        std::ostringstream oss;
        oss << "md_sequence,timestamp_ns,instrument_id,side,level,price,quantity,order_count\n";

        for (const auto& snap : snapshots) {
            for (std::size_t lvl = 0U; lvl < snap.bids.size(); ++lvl) {
                const auto& d = snap.bids[lvl];
                oss << snap.md_sequence << ',' << snap.timestamp << ',' << snap.instrument_id
                    << ",BUY," << lvl << ',' << d.price << ',' << d.quantity << ','
                    << d.order_count << '\n';
            }
            for (std::size_t lvl = 0U; lvl < snap.asks.size(); ++lvl) {
                const auto& d = snap.asks[lvl];
                oss << snap.md_sequence << ',' << snap.timestamp << ',' << snap.instrument_id
                    << ",SELL," << lvl << ',' << d.price << ',' << d.quantity << ','
                    << d.order_count << '\n';
            }
        }
        return oss.str();
    }

    /// Returns the CSV header corresponding to `summary_to_csv_row`.
    [[nodiscard]] static std::string summary_to_csv_header() {
        return "experiment,sweep_param,sweep_value,num_steps,trade_count,total_volume,"
               "mean_spread_ticks,mean_spread_bps,mean_l1_imbalance,mean_top3_imbalance,"
               "vwap,realized_volatility_bps,trades_per_second,volume_per_second,"
               "order_flow_imbalance,inst_executed_qty,inst_slippage_bps,"
               "inst_permanent_impact_bps,inst_temporary_impact_bps\n";
    }

    /// Formats a single MicrostructureSummary row for experiment sweep CSV output.
    [[nodiscard]] static std::string summary_to_csv_row(std::string_view experiment_name,
                                                        std::string_view sweep_param,
                                                        double sweep_value,
                                                        const MicrostructureSummary& summary) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);

        const Quantity inst_qty =
            summary.institutional_impact.has_value() ? summary.institutional_impact->executed_quantity
                                                     : 0U;
        const double inst_slippage =
            summary.institutional_impact.has_value() ? summary.institutional_impact->slippage_bps
                                                     : 0.0;
        const double inst_perm = summary.institutional_impact.has_value()
                                     ? summary.institutional_impact->permanent_impact_bps
                                     : 0.0;
        const double inst_temp = summary.institutional_impact.has_value()
                                     ? summary.institutional_impact->temporary_impact_bps
                                     : 0.0;

        oss << experiment_name << ',' << sweep_param << ',' << sweep_value << ','
            << summary.num_steps << ',' << summary.trade_intensity.trade_count << ','
            << summary.trade_intensity.total_volume << ',' << summary.mean_spread_ticks << ','
            << summary.mean_spread_bps << ',' << summary.mean_l1_imbalance << ','
            << summary.mean_top3_imbalance << ',' << summary.vwap << ','
            << summary.realized_volatility_bps << ',' << summary.trade_intensity.trades_per_second
            << ',' << summary.trade_intensity.volume_per_second << ','
            << summary.trade_intensity.order_flow_imbalance << ',' << inst_qty << ','
            << inst_slippage << ',' << inst_perm << ',' << inst_temp << '\n';
        return oss.str();
    }

private:
    [[nodiscard]] static constexpr bool is_valid_price(std::optional<Price> price) noexcept {
        return price.has_value() && *price != kInvalidPrice;
    }

    [[nodiscard]] static constexpr std::optional<Price> extract_price(
        std::optional<Price> price) noexcept {
        return price;
    }

    [[nodiscard]] static constexpr std::optional<Price> extract_price(Price price) noexcept {
        if (price == kInvalidPrice) {
            return std::nullopt;
        }
        return price;
    }

    [[nodiscard]] static constexpr std::string_view side_label(Side side) noexcept {
        return (side == Side::Buy) ? "BUY" : "SELL";
    }
};

}  // namespace martingale

#endif  // MARTINGALE_RESEARCH_MICROSTRUCTURE_METRICS_HPP
