#include <gtest/gtest.h>
#include <martingale/book/flat_order_book.hpp>
#include <martingale/engine/matching_engine.hpp>
#include <martingale/gateway/order_gateway.hpp>
#include <martingale/journal/event_journal.hpp>
#include <martingale/market_data/market_data_publisher.hpp>
#include <martingale/research/microstructure_metrics.hpp>
#include <martingale/simulator/market_simulator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <vector>

namespace martingale::tests {

TEST(EndToEndStressTest, MultiInstrumentGatewayJournalReplayAndFastEngineParity) {
    constexpr std::size_t kTotalCommands = 25'000U;
    constexpr std::array<InstrumentId, 3> kInstruments{1U, 2U, 3U};

    MatchingEngine stl_engine(1U);
    FastMatchingEngine fast_engine(1U);
    EventJournal journal;

    for (std::size_t i = 1U; i < kInstruments.size(); ++i) {
        ASSERT_TRUE(stl_engine.register_instrument(kInstruments[i]));
        ASSERT_TRUE(fast_engine.register_instrument(kInstruments[i]));
        journal.record_register_instrument(kInstruments[i], static_cast<Timestamp>(i));
    }

    OrderGateway gateway(stl_engine);
    MarketDataPublisher stl_publisher;
    MarketDataPublisher fast_publisher;

    std::mt19937_64 rng(20261120ULL);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<std::size_t> inst_dist(0U, kInstruments.size() - 1U);
    std::uniform_int_distribution<Price> price_dist(9992, 10008);
    std::uniform_int_distribution<Quantity> qty_dist(1U, 100U);

    struct ActiveOrderRef {
        InstrumentId instrument_id;
        OrderId order_id;
    };
    std::vector<ActiveOrderRef> candidate_orders;
    candidate_orders.reserve(kTotalCommands);

    OrderId next_order_id = 1U;
    std::uint64_t expected_rejected = 0U;

    for (std::size_t step = 0U; step < kTotalCommands; ++step) {
        const Timestamp ts = static_cast<Timestamp>(step + 100U) * 1000ULL;
        const int action = action_dist(rng);

        // Inject periodic malformed boundary commands every 500 steps to stress OrderGateway
        if (step % 500U == 250U) {
            const auto bad_res = gateway.validate_and_submit(
                Order(kInvalidOrderId, 999U, Side::Buy, OrderType::Limit, 10000, 10U, ts));
            EXPECT_FALSE(bad_res.accepted);
            ++expected_rejected;
            continue;
        }

        // 20% cancellation attempts against previously submitted orders
        if (action < 20 && !candidate_orders.empty()) {
            std::uniform_int_distribution<std::size_t> idx_dist(0U, candidate_orders.size() - 1U);
            const ActiveOrderRef target = candidate_orders[idx_dist(rng)];

            const auto gw_cancel = gateway.validate_and_cancel(target.instrument_id, target.order_id);
            if (gw_cancel.accepted && gw_cancel.cancelled_order.has_value()) {
                journal.record_cancellation(target.instrument_id, target.order_id,
                                            gw_cancel.cancelled_order,
                                            stl_engine.next_sequence_number() - 1U, ts);
                stl_publisher.on_order_cancelled(*gw_cancel.cancelled_order,
                                                 stl_engine.book(target.instrument_id), ts);

                const auto fast_cancel = fast_engine.cancel_order(target.instrument_id, target.order_id);
                ASSERT_TRUE(fast_cancel.has_value());
                EXPECT_EQ(*fast_cancel, *gw_cancel.cancelled_order);
                fast_publisher.on_order_cancelled(*fast_cancel,
                                                  fast_engine.book(target.instrument_id), ts);
            }
            continue;
        }

        const InstrumentId inst = kInstruments[inst_dist(rng)];
        const Side side = (action_dist(rng) < 50) ? Side::Buy : Side::Sell;
        const OrderType type = (action < 32) ? OrderType::Market : OrderType::Limit;
        const Price price = (type == OrderType::Market) ? kMarketOrderPrice : price_dist(rng);
        const Quantity qty = qty_dist(rng);
        const OrderId oid = next_order_id++;

        const Order order(oid, inst, side, type, price, qty, ts);
        const auto gw_res = gateway.validate_and_submit(order);
        ASSERT_TRUE(gw_res.accepted);
        journal.record_submission(order, gw_res.engine_result);
        stl_publisher.on_order_processed(gw_res.engine_result, stl_engine.book(inst));

        const auto fast_res = fast_engine.submit_order(order);
        fast_publisher.on_order_processed(fast_res, fast_engine.book(inst));

        ASSERT_EQ(fast_res.trades, gw_res.engine_result.trades);
        ASSERT_EQ(fast_res.final_order, gw_res.engine_result.final_order);

        if (gw_res.engine_result.final_order.is_active()) {
            candidate_orders.push_back(ActiveOrderRef{inst, oid});
        }

        // Periodic L2 snapshot parity checks across STL OrderBook and FlatOrderBook
        if (step % 1000U == 999U) {
            for (const InstrumentId id : kInstruments) {
                const auto stl_snap = stl_publisher.capture_snapshot(stl_engine.book(id), 5U, ts);
                const auto fast_snap = fast_publisher.capture_snapshot(fast_engine.book(id), 5U, ts);
                EXPECT_EQ(stl_snap, fast_snap);
            }
        }
    }

    EXPECT_EQ(gateway.rejected_order_count(), expected_rejected);
    EXPECT_GT(stl_engine.trades().size(), 1000U);
    EXPECT_EQ(stl_engine.trades(), fast_engine.trades());
    EXPECT_EQ(stl_engine.next_sequence_number(), fast_engine.next_sequence_number());
    EXPECT_EQ(stl_engine.next_trade_id(), fast_engine.next_trade_id());
    EXPECT_EQ(stl_publisher.events().size(), fast_publisher.events().size());

    for (const InstrumentId id : kInstruments) {
        EXPECT_EQ(stl_publisher.last_bbo(id), fast_publisher.last_bbo(id));
        EXPECT_EQ(stl_engine.book(id).get_depth(Side::Buy, 100U),
                  fast_engine.book(id).get_depth(Side::Buy, 100U));
        EXPECT_EQ(stl_engine.book(id).get_depth(Side::Sell, 100U),
                  fast_engine.book(id).get_depth(Side::Sell, 100U));
    }

    // Binary EventJournal round-trip serialization + ReplayEngine reconstruction
    std::stringstream wire(std::ios::in | std::ios::out | std::ios::binary);
    journal.save_to_stream(wire);
    wire.seekg(0);
    const EventJournal reloaded = EventJournal::load_from_stream(wire);

    MatchingEngine replayed_engine(1U);
    const ReplayVerificationResult replay_status =
        ReplayEngine::replay_into(reloaded, replayed_engine);

    EXPECT_TRUE(replay_status.matches_recorded_events);
    EXPECT_EQ(replay_status.trades_replayed, stl_engine.trades().size());
    EXPECT_EQ(replayed_engine.trades(), stl_engine.trades());
    EXPECT_EQ(replayed_engine.next_sequence_number(), stl_engine.next_sequence_number());
    EXPECT_EQ(replayed_engine.next_trade_id(), stl_engine.next_trade_id());

    for (const InstrumentId id : kInstruments) {
        EXPECT_EQ(replayed_engine.book(id).get_depth(Side::Buy, 100U),
                  stl_engine.book(id).get_depth(Side::Buy, 100U));
        EXPECT_EQ(replayed_engine.book(id).get_depth(Side::Sell, 100U),
                  stl_engine.book(id).get_depth(Side::Sell, 100U));
    }
}

TEST(EndToEndStressTest, MultiSeedSimulationConservationAndMicrostructureSanity) {
    constexpr std::array<std::uint64_t, 3> kSeeds{101ULL, 202ULL, 303ULL};

    for (const std::uint64_t seed : kSeeds) {
        SimulationConfig cfg;
        cfg.seed = seed;
        cfg.total_steps = 300U;
        cfg.snapshot_interval = 10U;
        cfg.snapshot_depth = 5U;
        cfg.num_random_traders = 5U;
        cfg.num_momentum_traders = 2U;
        cfg.include_institutional_trader = true;
        cfg.institutional_config.side = Side::Buy;
        cfg.institutional_config.total_parent_quantity = 400U;
        cfg.institutional_config.child_slice_quantity = 40U;
        cfg.institutional_config.start_step = 30U;
        cfg.institutional_config.execution_interval_steps = 5U;

        const SimulationResult result = MarketSimulator(cfg).run();
        const MicrostructureSummary summary = MicrostructureAnalyzer::analyze(result);

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

        EXPECT_EQ(summary.num_steps, 300U);
        EXPECT_EQ(summary.num_snapshots, 30U);
        EXPECT_GT(summary.mean_spread_bps, 0.0);
        EXPECT_GT(summary.vwap, 0.0);
        EXPECT_GE(summary.realized_volatility_bps, 0.0);
        EXPECT_TRUE(summary.institutional_impact.has_value());
        EXPECT_GT(summary.institutional_impact->executed_quantity, 0U);
    }
}

}  // namespace martingale::tests
