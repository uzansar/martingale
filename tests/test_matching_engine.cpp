#include <gtest/gtest.h>
#include <martingale/engine/matching_engine.hpp>

#include <cstddef>
#include <optional>
#include <random>
#include <vector>

namespace martingale::tests {

TEST(MatchingEngineTest, RestingOrdersWhenBookDoesNotCross) {
    MatchingEngine engine(1U);

    const OrderResult buy_res =
        engine.submit_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10000, 100U, 10U));
    const OrderResult sell_res =
        engine.submit_order(Order(2U, 1U, Side::Sell, OrderType::Limit, 10050, 200U, 20U));

    EXPECT_TRUE(buy_res.trades.empty());
    EXPECT_TRUE(sell_res.trades.empty());
    EXPECT_EQ(buy_res.final_order.status, OrderStatus::New);
    EXPECT_EQ(sell_res.final_order.status, OrderStatus::New);
    EXPECT_TRUE(engine.trades().empty());

    EXPECT_EQ(engine.book().best_bid(), 10000);
    EXPECT_EQ(engine.book().best_ask(), 10050);
    EXPECT_EQ(engine.book().spread(), 50);
    EXPECT_EQ(engine.book().active_order_count(), 2U);
}

TEST(MatchingEngineTest, ExactFullFillSingleLevel) {
    MatchingEngine engine(1U);

    const OrderResult sell_res =
        engine.submit_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U));
    EXPECT_TRUE(sell_res.trades.empty());

    const OrderResult buy_res =
        engine.submit_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 100U, 20U));

    ASSERT_EQ(buy_res.trades.size(), 1U);
    const Trade& trade = buy_res.trades[0];
    EXPECT_EQ(trade.trade_id, 1U);
    EXPECT_EQ(trade.instrument_id, 1U);
    EXPECT_EQ(trade.price, 10050);
    EXPECT_EQ(trade.quantity, 100U);
    EXPECT_EQ(trade.buyer_order_id, 2U);
    EXPECT_EQ(trade.seller_order_id, 1U);
    EXPECT_EQ(trade.aggressor_side, Side::Buy);
    EXPECT_EQ(trade.timestamp, 20U);

    EXPECT_EQ(buy_res.final_order.status, OrderStatus::Filled);
    EXPECT_EQ(buy_res.final_order.remaining_quantity, 0U);
    EXPECT_TRUE(engine.book().empty());
    EXPECT_EQ(engine.trades().size(), 1U);
}

TEST(MatchingEngineTest, PriceImprovementExecutesAtRestingOrderPrice) {
    MatchingEngine engine(1U);

    // Aggressive Buy crosses resting Ask at 10050 -> executes at 10050 (not 10100)
    static_cast<void>(
        engine.submit_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 50U, 10U)));
    const OrderResult buy_res =
        engine.submit_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10100, 50U, 20U));
    ASSERT_EQ(buy_res.trades.size(), 1U);
    EXPECT_EQ(buy_res.trades[0].price, 10050);
    EXPECT_EQ(buy_res.trades[0].aggressor_side, Side::Buy);

    // Aggressive Sell crosses resting Bid at 10050 -> executes at 10050 (not 10000)
    static_cast<void>(
        engine.submit_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10050, 75U, 30U)));
    const OrderResult sell_res =
        engine.submit_order(Order(4U, 1U, Side::Sell, OrderType::Limit, 10000, 75U, 40U));
    ASSERT_EQ(sell_res.trades.size(), 1U);
    EXPECT_EQ(sell_res.trades[0].price, 10050);
    EXPECT_EQ(sell_res.trades[0].buyer_order_id, 3U);
    EXPECT_EQ(sell_res.trades[0].seller_order_id, 4U);
    EXPECT_EQ(sell_res.trades[0].aggressor_side, Side::Sell);
    EXPECT_TRUE(engine.book().empty());
}

TEST(MatchingEngineTest, StrictFifoPriorityWithinSinglePriceLevel) {
    MatchingEngine engine(1U);

    static_cast<void>(
        engine.submit_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U)));
    static_cast<void>(
        engine.submit_order(Order(2U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 20U)));
    static_cast<void>(
        engine.submit_order(Order(3U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 30U)));

    const OrderResult buy_res =
        engine.submit_order(Order(4U, 1U, Side::Buy, OrderType::Limit, 10050, 250U, 40U));

    ASSERT_EQ(buy_res.trades.size(), 3U);
    EXPECT_EQ(buy_res.trades[0].seller_order_id, 1U);
    EXPECT_EQ(buy_res.trades[0].quantity, 100U);
    EXPECT_EQ(buy_res.trades[1].seller_order_id, 2U);
    EXPECT_EQ(buy_res.trades[1].quantity, 100U);
    EXPECT_EQ(buy_res.trades[2].seller_order_id, 3U);
    EXPECT_EQ(buy_res.trades[2].quantity, 50U);

    EXPECT_EQ(buy_res.final_order.status, OrderStatus::Filled);
    EXPECT_EQ(engine.book().active_order_count(), 1U);
    ASSERT_NE(engine.book().find_order(3U), nullptr);
    EXPECT_EQ(engine.book().find_order(3U)->remaining_quantity, 50U);
    EXPECT_EQ(engine.book().find_order(3U)->status, OrderStatus::PartiallyFilled);
}

TEST(MatchingEngineTest, MultiLevelPriceSweepAndRemainderResting) {
    MatchingEngine engine(1U);

    static_cast<void>(
        engine.submit_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U)));
    static_cast<void>(
        engine.submit_order(Order(2U, 1U, Side::Sell, OrderType::Limit, 10060, 150U, 20U)));
    static_cast<void>(
        engine.submit_order(Order(3U, 1U, Side::Sell, OrderType::Limit, 10070, 200U, 30U)));

    const OrderResult buy_res =
        engine.submit_order(Order(4U, 1U, Side::Buy, OrderType::Limit, 10065, 300U, 40U));

    ASSERT_EQ(buy_res.trades.size(), 2U);
    EXPECT_EQ(buy_res.trades[0].price, 10050);
    EXPECT_EQ(buy_res.trades[0].quantity, 100U);
    EXPECT_EQ(buy_res.trades[1].price, 10060);
    EXPECT_EQ(buy_res.trades[1].quantity, 150U);

    EXPECT_EQ(buy_res.final_order.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(buy_res.final_order.filled_quantity(), 250U);
    EXPECT_EQ(buy_res.final_order.remaining_quantity, 50U);

    EXPECT_EQ(engine.book().best_bid(), 10065);
    EXPECT_EQ(engine.book().best_bid_quantity(), 50U);
    EXPECT_EQ(engine.book().best_ask(), 10070);
    EXPECT_EQ(engine.book().best_ask_quantity(), 200U);
}

TEST(MatchingEngineTest, MarketOrdersSweepLiquidityAndNeverRest) {
    MatchingEngine engine(1U);

    // Market Buy on empty book cancels immediately with zero trades
    const OrderResult empty_mkt = engine.submit_order(
        Order(1U, 1U, Side::Buy, OrderType::Market, kMarketOrderPrice, 100U, 10U));
    EXPECT_TRUE(empty_mkt.trades.empty());
    EXPECT_EQ(empty_mkt.final_order.status, OrderStatus::Cancelled);
    EXPECT_TRUE(engine.book().empty());

    // Resting Bids across 2 levels totaling 300
    static_cast<void>(
        engine.submit_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 100U, 20U)));
    static_cast<void>(
        engine.submit_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10040, 200U, 30U)));

    // Market Sell for 500 sweeps all 300 and cancels remaining 200 without resting
    const OrderResult mkt_sell = engine.submit_order(
        Order(4U, 1U, Side::Sell, OrderType::Market, kMarketOrderPrice, 500U, 40U));
    ASSERT_EQ(mkt_sell.trades.size(), 2U);
    EXPECT_EQ(mkt_sell.trades[0].price, 10050);
    EXPECT_EQ(mkt_sell.trades[1].price, 10040);
    EXPECT_EQ(mkt_sell.final_order.status, OrderStatus::Cancelled);
    EXPECT_EQ(mkt_sell.final_order.filled_quantity(), 300U);
    EXPECT_EQ(mkt_sell.final_order.remaining_quantity, 200U);
    EXPECT_TRUE(engine.book().empty());

    // Market Buy fully filled against resting Ask
    static_cast<void>(
        engine.submit_order(Order(5U, 1U, Side::Sell, OrderType::Limit, 10060, 200U, 50U)));
    const OrderResult mkt_buy = engine.submit_order(
        Order(6U, 1U, Side::Buy, OrderType::Market, kMarketOrderPrice, 80U, 60U));
    ASSERT_EQ(mkt_buy.trades.size(), 1U);
    EXPECT_EQ(mkt_buy.final_order.status, OrderStatus::Filled);
    EXPECT_EQ(engine.book().best_ask_quantity(), 120U);
}

TEST(MatchingEngineTest, CancellationAndRejectEdgeCases) {
    MatchingEngine engine(1U);

    static_cast<void>(
        engine.submit_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U)));
    static_cast<void>(
        engine.submit_order(Order(2U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 20U)));
    static_cast<void>(
        engine.submit_order(Order(3U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 30U)));

    const auto cancelled_mid = engine.cancel_order(2U);
    ASSERT_TRUE(cancelled_mid.has_value());
    EXPECT_EQ(cancelled_mid->id, 2U);
    EXPECT_EQ(cancelled_mid->status, OrderStatus::Cancelled);

    EXPECT_EQ(engine.cancel_order(2U), std::nullopt);
    EXPECT_EQ(engine.cancel_order(999U), std::nullopt);

    const OrderResult buy_res =
        engine.submit_order(Order(4U, 1U, Side::Buy, OrderType::Limit, 10050, 150U, 40U));
    ASSERT_EQ(buy_res.trades.size(), 2U);
    EXPECT_EQ(buy_res.trades[0].seller_order_id, 1U);
    EXPECT_EQ(buy_res.trades[1].seller_order_id, 3U);
    EXPECT_EQ(engine.cancel_order(1U), std::nullopt);

    // Invalid / duplicate order rejections do not mutate book or produce trades
    const auto reject_wrong_inst =
        engine.submit_order(Order(10U, 2U, Side::Buy, OrderType::Limit, 10000, 10U, 50U));
    const auto reject_zero_id = engine.submit_order(
        Order(kInvalidOrderId, 1U, Side::Buy, OrderType::Limit, 10000, 10U, 51U));
    const auto reject_dup_id =
        engine.submit_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10000, 10U, 52U));
    const auto reject_nonpos_price =
        engine.submit_order(Order(11U, 1U, Side::Buy, OrderType::Limit, 0, 10U, 53U));
    const auto reject_zero_qty =
        engine.submit_order(Order(12U, 1U, Side::Buy, OrderType::Limit, 10000, 0U, 54U));

    EXPECT_EQ(reject_wrong_inst.final_order.status, OrderStatus::Rejected);
    EXPECT_EQ(reject_zero_id.final_order.status, OrderStatus::Rejected);
    EXPECT_EQ(reject_dup_id.final_order.status, OrderStatus::Rejected);
    EXPECT_EQ(reject_nonpos_price.final_order.status, OrderStatus::Rejected);
    EXPECT_EQ(reject_zero_qty.final_order.status, OrderStatus::Rejected);
    EXPECT_EQ(engine.book().active_order_count(), 1U);
    EXPECT_EQ(engine.book().best_ask_quantity(), 50U);
}

TEST(MatchingEngineTest, DeterministicTenThousandCommandSimulation) {
    struct Command {
        bool is_cancel{false};
        Order order{};
        OrderId cancel_id{kInvalidOrderId};
    };

    constexpr std::size_t kNumCommands = 10000U;
    std::mt19937_64 rng(42ULL);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<Price> price_dist(9990, 10010);
    std::uniform_int_distribution<Quantity> qty_dist(1U, 100U);

    std::vector<Command> commands;
    commands.reserve(kNumCommands);
    std::vector<OrderId> submitted_ids;
    submitted_ids.reserve(kNumCommands);

    OrderId next_id = 1U;
    for (std::size_t i = 0; i < kNumCommands; ++i) {
        const int action = action_dist(rng);
        if (action < 10 && !submitted_ids.empty()) {
            std::uniform_int_distribution<std::size_t> id_dist(0U, submitted_ids.size() - 1U);
            commands.push_back(Command{true, Order{}, submitted_ids[id_dist(rng)]});
            continue;
        }

        const Side side = (action_dist(rng) < 50) ? Side::Buy : Side::Sell;
        const OrderType type = (action < 25) ? OrderType::Market : OrderType::Limit;
        const Price price = (type == OrderType::Market) ? kMarketOrderPrice : price_dist(rng);
        const Quantity qty = qty_dist(rng);
        const Timestamp ts = static_cast<Timestamp>(i + 1U) * 1000U;

        const OrderId oid = next_id++;
        submitted_ids.push_back(oid);
        commands.push_back(Command{false, Order(oid, 1U, side, type, price, qty, ts), kInvalidOrderId});
    }

    MatchingEngine engine_a(1U);
    MatchingEngine engine_b(1U);

    for (const auto& cmd : commands) {
        if (cmd.is_cancel) {
            EXPECT_EQ(engine_a.cancel_order(cmd.cancel_id), engine_b.cancel_order(cmd.cancel_id));
        } else {
            const auto res_a = engine_a.submit_order(cmd.order);
            const auto res_b = engine_b.submit_order(cmd.order);
            EXPECT_EQ(res_a.final_order, res_b.final_order);
            EXPECT_EQ(res_a.trades, res_b.trades);
            EXPECT_EQ(res_a.sequence_number, res_b.sequence_number);
        }
    }

    EXPECT_GT(engine_a.trades().size(), 1000U);
    EXPECT_EQ(engine_a.trades(), engine_b.trades());
    EXPECT_EQ(engine_a.next_sequence_number(), engine_b.next_sequence_number());
    EXPECT_EQ(engine_a.next_trade_id(), engine_b.next_trade_id());
    EXPECT_EQ(engine_a.book().active_order_count(), engine_b.book().active_order_count());
    EXPECT_EQ(engine_a.book().get_depth(Side::Buy, 1000U),
              engine_b.book().get_depth(Side::Buy, 1000U));
    EXPECT_EQ(engine_a.book().get_depth(Side::Sell, 1000U),
              engine_b.book().get_depth(Side::Sell, 1000U));
}

}  // namespace martingale::tests
