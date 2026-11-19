#include <gtest/gtest.h>
#include <martingale/simulator/agents.hpp>
#include <martingale/simulator/market_simulator.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace martingale::tests {

namespace {

struct RecordedSubmission {
    bool is_cancel{false};
    bool is_market{false};
    Side side{Side::Buy};
    Price price{kInvalidPrice};
    Quantity quantity{0};
    OrderId order_id{kInvalidOrderId};
};

class RecordingSink final : public OrderSubmissionSink {
public:
    OrderId submit_limit(std::size_t /*agent_index*/, Side side, Price price,
                         Quantity quantity) override {
        const OrderId oid = next_id_++;
        events.push_back(RecordedSubmission{false, false, side, price, quantity, oid});
        return oid;
    }

    OrderId submit_market(std::size_t /*agent_index*/, Side side, Quantity quantity) override {
        const OrderId oid = next_id_++;
        events.push_back(
            RecordedSubmission{false, true, side, kMarketOrderPrice, quantity, oid});
        return oid;
    }

    void cancel(std::size_t /*agent_index*/, OrderId order_id) override {
        events.push_back(RecordedSubmission{true, false, Side::Buy, kInvalidPrice, 0U, order_id});
    }

    std::vector<RecordedSubmission> events{};

private:
    OrderId next_id_{1};
};

}  // namespace

TEST(MarketSimulatorTest, MarketMakerQuotesTwoSidedLadderAndSkewsOnInventory) {
    MarketMakerConfig cfg;
    cfg.half_spread_ticks = 2;
    cfg.num_levels = 3U;
    cfg.base_quantity = 50U;
    cfg.level_quantity_step = 25U;
    cfg.inventory_skew_divisor = 50;

    MarketMakerAgent mm(0U, cfg);
    RecordingSink sink;

    const SimulationContext ctx{
        .step_index = 0U,
        .instrument_id = 1U,
        .current_time = 1000U,
        .reference_mid_price = 10000,
        .best_bid = std::nullopt,
        .best_bid_quantity = 0U,
        .best_ask = std::nullopt,
        .best_ask_quantity = 0U,
    };

    mm.on_step(ctx, sink);
    ASSERT_EQ(sink.events.size(), 6U);

    Price neutral_best_bid = 0;
    Price neutral_best_ask = 0;
    for (const auto& ev : sink.events) {
        ASSERT_FALSE(ev.is_cancel);
        ASSERT_FALSE(ev.is_market);
        if (ev.side == Side::Buy && ev.price > neutral_best_bid) {
            neutral_best_bid = ev.price;
        }
        if (ev.side == Side::Sell && (neutral_best_ask == 0 || ev.price < neutral_best_ask)) {
            neutral_best_ask = ev.price;
        }
    }
    EXPECT_EQ(neutral_best_bid, 9998);
    EXPECT_EQ(neutral_best_ask, 10002);
    EXPECT_LT(neutral_best_bid, neutral_best_ask);

    // Simulate long inventory fill -> quotes must skew downward
    mm.on_fill(Side::Buy, 10000, 150U);
    sink.events.clear();

    mm.on_step(ctx, sink);
    // 6 cancellations of previous quotes + 6 fresh skewed quotes
    ASSERT_EQ(sink.events.size(), 12U);

    Price skewed_best_bid = 0;
    Price skewed_best_ask = 0;
    for (const auto& ev : sink.events) {
        if (ev.is_cancel) {
            continue;
        }
        if (ev.side == Side::Buy && ev.price > skewed_best_bid) {
            skewed_best_bid = ev.price;
        }
        if (ev.side == Side::Sell && (skewed_best_ask == 0 || ev.price < skewed_best_ask)) {
            skewed_best_ask = ev.price;
        }
    }
    EXPECT_LT(skewed_best_bid, neutral_best_bid);
    EXPECT_LT(skewed_best_ask, neutral_best_ask);
}

TEST(MarketSimulatorTest, MomentumTraderTriggersOnTrendBreakout) {
    MomentumTraderConfig cfg;
    cfg.lookback_window = 3U;
    cfg.breakout_threshold_ticks = 4;
    cfg.order_quantity = 40U;

    MomentumTraderAgent trader(0U, cfg);
    RecordingSink sink;

    const auto make_ctx = [](std::size_t step, Price mid) {
        return SimulationContext{step, 1U, (step + 1U) * 1000U, mid, mid - 1, 100U, mid + 1, 100U};
    };

    // Flat/noisy baseline below threshold -> no orders
    trader.on_step(make_ctx(0U, 10000), sink);
    trader.on_step(make_ctx(1U, 10001), sink);
    trader.on_step(make_ctx(2U, 10000), sink);
    trader.on_step(make_ctx(3U, 10002), sink);
    EXPECT_TRUE(sink.events.empty());

    // +5 ticks vs step 1 (10001 -> 10006) triggers Buy momentum order
    trader.on_step(make_ctx(4U, 10006), sink);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_TRUE(sink.events[0].is_market);
    EXPECT_EQ(sink.events[0].side, Side::Buy);
    EXPECT_EQ(sink.events[0].quantity, 40U);

    // Sharp drop to 9995 triggers Sell momentum order
    sink.events.clear();
    trader.on_step(make_ctx(5U, 9995), sink);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_TRUE(sink.events[0].is_market);
    EXPECT_EQ(sink.events[0].side, Side::Sell);
    EXPECT_EQ(sink.events[0].quantity, 40U);
}

TEST(MarketSimulatorTest, InstitutionalTraderExecutesTwapChildSlicesAndTracksNotional) {
    InstitutionalTraderConfig cfg;
    cfg.side = Side::Buy;
    cfg.total_parent_quantity = 300U;
    cfg.child_slice_quantity = 100U;
    cfg.start_step = 2U;
    cfg.execution_interval_steps = 2U;

    InstitutionalTraderAgent inst(0U, cfg);
    RecordingSink sink;

    const auto make_ctx = [](std::size_t step, Price mid) {
        return SimulationContext{step, 1U, (step + 1U) * 1000U, mid, mid - 1, 100U, mid + 1, 100U};
    };

    // Steps 0 and 1 are before start_step
    inst.on_step(make_ctx(0U, 10000), sink);
    inst.on_step(make_ctx(1U, 10000), sink);
    EXPECT_TRUE(sink.events.empty());
    EXPECT_FALSE(inst.arrival_mid_price().has_value());

    // Step 2: first slice submitted and arrival mid captured
    inst.on_step(make_ctx(2U, 10010), sink);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].quantity, 100U);
    EXPECT_EQ(inst.arrival_mid_price(), 10010);
    inst.on_fill(Side::Buy, 10012, 100U);

    // Step 3: off-interval -> no slice
    inst.on_step(make_ctx(3U, 10012), sink);
    EXPECT_EQ(sink.events.size(), 1U);

    // Step 4 & Step 6: remaining two slices
    inst.on_step(make_ctx(4U, 10014), sink);
    inst.on_fill(Side::Buy, 10015, 100U);
    inst.on_step(make_ctx(5U, 10015), sink);
    inst.on_step(make_ctx(6U, 10016), sink);
    inst.on_fill(Side::Buy, 10018, 100U);

    EXPECT_EQ(sink.events.size(), 3U);
    EXPECT_TRUE(inst.is_completed());
    EXPECT_EQ(inst.submitted_quantity(), 300U);
    EXPECT_EQ(inst.executed_quantity(), 300U);
}

TEST(MarketSimulatorTest, MultiAgentSimulationMaintainsZeroSumInventoryConservation) {
    SimulationConfig cfg;
    cfg.seed = 1337ULL;
    cfg.total_steps = 300U;
    cfg.snapshot_interval = 10U;
    cfg.num_random_traders = 3U;
    cfg.num_momentum_traders = 1U;
    cfg.include_institutional_trader = true;

    MarketSimulator sim(cfg);
    const SimulationResult result = sim.run();

    EXPECT_EQ(result.step_records.size(), 300U);
    EXPECT_EQ(result.l2_snapshots.size(), 30U);
    EXPECT_GT(result.trades.size(), 50U);

    // Zero-sum inventory and cash-flow conservation across all exchange participants
    std::int64_t total_inventory = 0;
    std::int64_t total_cash_flow = 0;
    Quantity sum_agent_volume = 0U;
    for (const auto& metrics : result.agent_metrics) {
        total_inventory += metrics.inventory;
        total_cash_flow += metrics.cash_flow_ticks;
        sum_agent_volume += metrics.traded_volume;
    }

    Quantity total_trade_volume = 0U;
    for (const auto& trade : result.trades) {
        total_trade_volume += trade.quantity;
    }

    EXPECT_EQ(total_inventory, 0);
    EXPECT_EQ(total_cash_flow, 0);
    EXPECT_EQ(sum_agent_volume, 2U * total_trade_volume);
}

TEST(MarketSimulatorTest, SimulatorIsStrictlyDeterministicForIdenticalSeed) {
    SimulationConfig cfg_a;
    cfg_a.seed = 2026ULL;
    cfg_a.total_steps = 400U;
    cfg_a.snapshot_interval = 20U;

    SimulationConfig cfg_b = cfg_a;

    SimulationConfig cfg_diff = cfg_a;
    cfg_diff.seed = 7777ULL;

    const SimulationResult res_a = MarketSimulator(cfg_a).run();
    const SimulationResult res_b = MarketSimulator(cfg_b).run();
    const SimulationResult res_diff = MarketSimulator(cfg_diff).run();

    EXPECT_EQ(res_a.trades, res_b.trades);
    EXPECT_EQ(res_a.step_records, res_b.step_records);
    EXPECT_EQ(res_a.l2_snapshots, res_b.l2_snapshots);
    EXPECT_EQ(res_a.agent_metrics, res_b.agent_metrics);

    EXPECT_NE(res_a.trades, res_diff.trades);
}

}  // namespace martingale::tests
