#include <gtest/gtest.h>
#include <martingale/research/microstructure_metrics.hpp>
#include <martingale/simulator/market_simulator.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace martingale::tests {

namespace {

Trade make_trade(TradeId id, Price price, Quantity qty, Timestamp ts, Side aggressor) {
    Trade t{};
    t.trade_id = id;
    t.price = price;
    t.quantity = qty;
    t.buyer_order_id = id * 2U;
    t.seller_order_id = id * 2U + 1U;
    t.sequence_number = id;
    t.timestamp = ts;
    t.instrument_id = 1U;
    t.aggressor_side = aggressor;
    return t;
}

SimulationStepRecord make_step(std::size_t idx, Timestamp ts, Price ref_mid,
                               Price bid, Quantity bid_qty,
                               Price ask, Quantity ask_qty) {
    SimulationStepRecord rec{};
    rec.step_index = idx;
    rec.timestamp = ts;
    rec.reference_mid_price = ref_mid;
    rec.best_bid = bid;
    rec.best_bid_quantity = bid_qty;
    rec.best_ask = ask;
    rec.best_ask_quantity = ask_qty;
    return rec;
}

std::size_t count_lines(const std::string& csv) {
    if (csv.empty()) {
        return 0U;
    }
    return static_cast<std::size_t>(std::count(csv.begin(), csv.end(), '\n'));
}

}  // namespace

TEST(MicrostructureMetricsTest, SpreadAndOrderBookImbalanceCalculations) {
    // Missing or empty book sides
    EXPECT_FALSE(MicrostructureAnalyzer::bid_ask_spread(std::nullopt, std::optional<Price>{10002})
                     .has_value());
    EXPECT_FALSE(
        MicrostructureAnalyzer::bid_ask_spread(std::optional<Price>{9998}, std::nullopt).has_value());
    EXPECT_FALSE(
        MicrostructureAnalyzer::mid_price(std::optional<Price>{9998}, std::nullopt).has_value());
    EXPECT_FALSE(MicrostructureAnalyzer::microprice(std::optional<Price>{9998}, 0U,
                                                    std::optional<Price>{10002}, 0U)
                     .has_value());

    // Valid BBO quotes
    const std::optional<Price> bid{9998};
    const std::optional<Price> ask{10002};
    const auto spread = MicrostructureAnalyzer::bid_ask_spread(bid, ask);
    const auto mid = MicrostructureAnalyzer::mid_price(bid, ask);
    const auto rel_bps = MicrostructureAnalyzer::relative_spread_bps(bid, ask);
    const auto micro = MicrostructureAnalyzer::microprice(bid, 300U, ask, 100U);

    ASSERT_TRUE(spread.has_value());
    ASSERT_TRUE(mid.has_value());
    ASSERT_TRUE(rel_bps.has_value());
    ASSERT_TRUE(micro.has_value());

    EXPECT_DOUBLE_EQ(*spread, 4.0);
    EXPECT_DOUBLE_EQ(*mid, 10000.0);
    EXPECT_DOUBLE_EQ(*rel_bps, 4.0);  // 4 / 10000 * 10000 = 4.0 bps
    // Microprice = (9998 * 100 + 10002 * 300) / 400 = 10001.0 (leans toward ask)
    EXPECT_DOUBLE_EQ(*micro, 10001.0);

    // L1 Order Book Imbalance
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::order_book_imbalance(0U, 0U), 0.0);
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::order_book_imbalance(100U, 100U), 0.0);
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::order_book_imbalance(300U, 100U), 0.5);
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::order_book_imbalance(100U, 300U), -0.5);

    // Multi-level L2 snapshot OBI
    OrderBookSnapshot snap{};
    snap.bids = {
        DepthLevel{9998, 200U, 2U},
        DepthLevel{9996, 150U, 1U},
        DepthLevel{9994, 50U, 1U},
    };
    snap.asks = {
        DepthLevel{10002, 100U, 1U},
        DepthLevel{10004, 150U, 2U},
        DepthLevel{10006, 550U, 3U},
    };

    // Level 1: (200 - 100) / (200 + 100) = +1/3
    EXPECT_NEAR(MicrostructureAnalyzer::order_book_imbalance(snap, 1U), 1.0 / 3.0, 1e-12);
    // Top 2: (350 - 250) / (350 + 250) = +100/600 = +1/6
    EXPECT_NEAR(MicrostructureAnalyzer::order_book_imbalance(snap, 2U), 1.0 / 6.0, 1e-12);
    // Top 3: (400 - 800) / (400 + 800) = -1/3
    EXPECT_NEAR(MicrostructureAnalyzer::order_book_imbalance(snap, 3U), -1.0 / 3.0, 1e-12);
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::order_book_imbalance(snap, 0U), 0.0);
}

TEST(MicrostructureMetricsTest, VwapAndTradeIntensityCalculations) {
    EXPECT_FALSE(MicrostructureAnalyzer::compute_vwap({}).has_value());

    const std::vector<Trade> trades = {
        make_trade(1U, 10000, 100U, 1'000'000'000ULL, Side::Buy),
        make_trade(2U, 10010, 300U, 1'500'000'000ULL, Side::Buy),
        make_trade(3U, 9990, 100U, 3'000'000'000ULL, Side::Sell),
    };

    // VWAP = (10000*100 + 10010*300 + 9990*100) / 500 = (1000000 + 3003000 + 999000) / 500
    //      = 5002000 / 500 = 10004.0
    const auto vwap = MicrostructureAnalyzer::compute_vwap(trades);
    ASSERT_TRUE(vwap.has_value());
    EXPECT_DOUBLE_EQ(*vwap, 10004.0);

    // 2.0 second window: 3 trades, 500 volume, 400 buy vs 100 sell => OFI = 300 / 500 = 0.6
    const TradeIntensityMetrics intensity = MicrostructureAnalyzer::compute_trade_intensity(
        trades, 1'000'000'000ULL, 3'000'000'000ULL);
    EXPECT_EQ(intensity.trade_count, 3U);
    EXPECT_EQ(intensity.total_volume, 500U);
    EXPECT_EQ(intensity.buy_aggressor_volume, 400U);
    EXPECT_EQ(intensity.sell_aggressor_volume, 100U);
    EXPECT_DOUBLE_EQ(intensity.order_flow_imbalance, 0.6);
    EXPECT_DOUBLE_EQ(intensity.trades_per_second, 1.5);
    EXPECT_DOUBLE_EQ(intensity.volume_per_second, 250.0);
}

TEST(MicrostructureMetricsTest, RealizedVolatilityCalculation) {
    // Degenerate and constant series have zero volatility
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::compute_realized_volatility({}), 0.0);
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::compute_realized_volatility(
                         std::vector<double>{10000.0, 10000.0}),
                     0.0);
    EXPECT_DOUBLE_EQ(MicrostructureAnalyzer::compute_realized_volatility(
                         std::vector<double>{10000.0, 10000.0, 10000.0, 10000.0}),
                     0.0);

    // Alternating series: 10000 -> 10100 -> 10000 -> 10100
    // Log returns: +r, -r, +r where r = ln(1.01)
    // mean = r / 3; deviations = (+2r/3, -4r/3, +2r/3)
    // sum sq dev = (4 + 16 + 4)/9 * r^2 = (24/9) * r^2
    // sample variance (n-1 = 2) = (12/9) * r^2 = (4/3) * r^2 => stddev = (2 / sqrt(3)) * r
    const std::vector<double> alternating{10000.0, 10100.0, 10000.0, 10100.0};
    const double r = std::log(1.01);
    const double expected_std = (2.0 / std::sqrt(3.0)) * r;
    EXPECT_NEAR(MicrostructureAnalyzer::compute_realized_volatility(alternating), expected_std,
                1e-12);
    EXPECT_NEAR(MicrostructureAnalyzer::compute_realized_volatility_bps(alternating),
                expected_std * 10000.0, 1e-8);

    // Series directly from SimulationStepRecords
    const std::vector<SimulationStepRecord> steps = {
        make_step(0U, 0ULL, 10000, 9999, 10U, 10001, 10U),
        make_step(1U, 1ULL, 10100, 10099, 10U, 10101, 10U),
        make_step(2U, 2ULL, 10000, 9999, 10U, 10001, 10U),
        make_step(3U, 3ULL, 10100, 10099, 10U, 10101, 10U),
    };
    EXPECT_NEAR(MicrostructureAnalyzer::compute_realized_volatility_bps(steps),
                expected_std * 10000.0, 1e-8);
}

TEST(MicrostructureMetricsTest, ExecutionSlippageAndMarketImpactBuyAndSell) {
    // Institutional Buy: arrived at 10000, paid avg 10015, market settles at 10009
    // Slippage = +15 ticks (+15 bps)
    // Permanent impact = +9 ticks (+9 bps)
    // Temporary impact = 10015 - 10009 = +6 ticks (+6 bps)
    const ExecutionImpactMetrics buy_impact = MicrostructureAnalyzer::compute_execution_impact(
        Side::Buy, 10000, 10015.0, 10009.0, 500U);
    EXPECT_EQ(buy_impact.executed_quantity, 500U);
    EXPECT_DOUBLE_EQ(buy_impact.slippage_ticks, 15.0);
    EXPECT_DOUBLE_EQ(buy_impact.slippage_bps, 15.0);
    EXPECT_DOUBLE_EQ(buy_impact.permanent_impact_ticks, 9.0);
    EXPECT_DOUBLE_EQ(buy_impact.permanent_impact_bps, 9.0);
    EXPECT_DOUBLE_EQ(buy_impact.temporary_impact_ticks, 6.0);
    EXPECT_DOUBLE_EQ(buy_impact.temporary_impact_bps, 6.0);
    EXPECT_DOUBLE_EQ(buy_impact.permanent_impact_ticks + buy_impact.temporary_impact_ticks,
                     buy_impact.slippage_ticks);

    // Institutional Sell: arrived at 10000, sold at avg 9986, market settles at 9992
    // Cost (signed) = -(9986 - 10000) = +14 ticks (+14 bps)
    // Permanent impact = -(9992 - 10000) = +8 ticks (+8 bps)
    // Temporary impact = -(9986 - 9992) = +6 ticks (+6 bps)
    const ExecutionImpactMetrics sell_impact = MicrostructureAnalyzer::compute_execution_impact(
        Side::Sell, 10000, 9986.0, 9992.0, 500U);
    EXPECT_DOUBLE_EQ(sell_impact.slippage_ticks, 14.0);
    EXPECT_DOUBLE_EQ(sell_impact.slippage_bps, 14.0);
    EXPECT_DOUBLE_EQ(sell_impact.permanent_impact_ticks, 8.0);
    EXPECT_DOUBLE_EQ(sell_impact.permanent_impact_bps, 8.0);
    EXPECT_DOUBLE_EQ(sell_impact.temporary_impact_ticks, 6.0);
    EXPECT_DOUBLE_EQ(sell_impact.temporary_impact_bps, 6.0);
    EXPECT_DOUBLE_EQ(sell_impact.permanent_impact_ticks + sell_impact.temporary_impact_ticks,
                     sell_impact.slippage_ticks);
}

TEST(MicrostructureMetricsTest, FullSimulationAnalysisAndCsvExporters) {
    SimulationConfig cfg;
    cfg.seed = 4242ULL;
    cfg.total_steps = 120U;
    cfg.snapshot_interval = 10U;
    cfg.snapshot_depth = 5U;
    cfg.num_random_traders = 4U;
    cfg.num_momentum_traders = 2U;
    cfg.include_institutional_trader = true;
    cfg.institutional_config.side = Side::Buy;
    cfg.institutional_config.total_parent_quantity = 400U;
    cfg.institutional_config.child_slice_quantity = 40U;
    cfg.institutional_config.start_step = 20U;
    cfg.institutional_config.execution_interval_steps = 5U;

    const SimulationResult result = MarketSimulator(cfg).run();
    const MicrostructureSummary summary = MicrostructureAnalyzer::analyze(result);

    EXPECT_EQ(summary.num_steps, 120U);
    EXPECT_EQ(summary.num_snapshots, 12U);
    EXPECT_GT(summary.mean_spread_ticks, 0.0);
    EXPECT_GT(summary.mean_spread_bps, 0.0);
    EXPECT_GT(summary.vwap, 0.0);
    EXPECT_GE(summary.realized_volatility_bps, 0.0);
    EXPECT_EQ(summary.trade_intensity.trade_count, result.trades.size());
    EXPECT_TRUE(summary.institutional_impact.has_value());
    EXPECT_GT(summary.institutional_impact->executed_quantity, 0U);

    ASSERT_FALSE(summary.liquidity_profile.empty());
    double total_share = 0.0;
    for (const auto& bucket : summary.liquidity_profile) {
        EXPECT_GE(bucket.mean_total_quantity, 0.0);
        total_share += bucket.share_of_book_liquidity;
    }
    EXPECT_NEAR(total_share, 1.0, 1e-9);

    const std::string steps_csv = MicrostructureAnalyzer::steps_to_csv(result);
    const std::string trades_csv = MicrostructureAnalyzer::trades_to_csv(result.trades);
    const std::string l2_csv = MicrostructureAnalyzer::l2_snapshots_to_csv(result.l2_snapshots);
    const std::string sweep_hdr = MicrostructureAnalyzer::summary_to_csv_header();
    const std::string sweep_row =
        MicrostructureAnalyzer::summary_to_csv_row("baseline", "inst_qty", 400.0, summary);

    EXPECT_EQ(count_lines(steps_csv), result.step_records.size() + 1U);
    EXPECT_EQ(count_lines(trades_csv), result.trades.size() + 1U);
    EXPECT_GT(count_lines(l2_csv), 1U);
    EXPECT_EQ(count_lines(sweep_hdr), 1U);
    EXPECT_EQ(count_lines(sweep_row), 1U);
}

TEST(MicrostructureMetricsTest, InstitutionalOrderSizeIncreasesExecutionSlippage) {
    SimulationConfig small_cfg;
    small_cfg.seed = 42ULL;
    small_cfg.total_steps = 400U;
    small_cfg.snapshot_interval = 20U;
    small_cfg.num_random_traders = 4U;
    small_cfg.num_momentum_traders = 1U;
    small_cfg.market_maker_config.half_spread_ticks = 2;
    small_cfg.market_maker_config.inventory_skew_divisor = 50;
    small_cfg.include_institutional_trader = true;
    small_cfg.institutional_config.side = Side::Buy;
    small_cfg.institutional_config.total_parent_quantity = 100U;
    small_cfg.institutional_config.child_slice_quantity = 20U;
    small_cfg.institutional_config.start_step = 40U;
    small_cfg.institutional_config.execution_interval_steps = 5U;

    SimulationConfig large_cfg = small_cfg;
    large_cfg.institutional_config.total_parent_quantity = 1200U;
    large_cfg.institutional_config.child_slice_quantity = 120U;

    const MicrostructureSummary small_summary =
        MicrostructureAnalyzer::analyze(MarketSimulator(small_cfg).run());
    const MicrostructureSummary large_summary =
        MicrostructureAnalyzer::analyze(MarketSimulator(large_cfg).run());

    ASSERT_TRUE(small_summary.institutional_impact.has_value());
    ASSERT_TRUE(large_summary.institutional_impact.has_value());
    EXPECT_GT(large_summary.institutional_impact->slippage_bps,
              small_summary.institutional_impact->slippage_bps);
}

}  // namespace martingale::tests
