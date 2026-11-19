#include <gtest/gtest.h>
#include <martingale/book/flat_order_book.hpp>
#include <martingale/engine/matching_engine.hpp>
#include <martingale/performance/object_pool.hpp>

#include <cstddef>
#include <new>
#include <optional>
#include <random>
#include <vector>

namespace martingale::tests {

TEST(ObjectPoolAndFlatBookTest, ObjectPoolAcquireReleaseAndLifoReuse) {
    ObjectPool<IntrusiveOrderNode> pool(4U);

    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.active_count(), 0U);
    EXPECT_EQ(pool.available_count(), 4U);

    IntrusiveOrderNode* n1 = pool.acquire();
    IntrusiveOrderNode* n2 = pool.acquire();
    IntrusiveOrderNode* n3 = pool.acquire();
    ASSERT_NE(n1, nullptr);
    ASSERT_NE(n2, nullptr);
    ASSERT_NE(n3, nullptr);
    EXPECT_EQ(pool.active_count(), 3U);
    EXPECT_EQ(pool.available_count(), 1U);

    pool.release(n2);
    EXPECT_EQ(pool.active_count(), 2U);
    EXPECT_EQ(pool.available_count(), 2U);

    // LIFO free-list returns the most recently released cache-hot slot first
    IntrusiveOrderNode* n4 = pool.acquire();
    EXPECT_EQ(n4, n2);
    EXPECT_EQ(pool.active_count(), 3U);

    IntrusiveOrderNode* n5 = pool.acquire();
    ASSERT_NE(n5, nullptr);
    EXPECT_EQ(pool.active_count(), 4U);
    EXPECT_EQ(pool.available_count(), 0U);

    EXPECT_THROW(static_cast<void>(pool.acquire()), std::bad_alloc);

    pool.reset();
    EXPECT_EQ(pool.active_count(), 0U);
    EXPECT_EQ(pool.available_count(), 4U);
}

TEST(ObjectPoolAndFlatBookTest, IntrusiveOrderNodeCacheLineLayoutAndQueueOperations) {
    static_assert(sizeof(IntrusiveOrderNode) == kCacheLineSize);
    static_assert(alignof(IntrusiveOrderNode) == kCacheLineSize);

    ObjectPool<IntrusiveOrderNode> pool(8U);
    IntrusivePriceLevel level(10050);

    IntrusiveOrderNode* n1 =
        pool.acquire(Order(1U, 1U, Side::Buy, OrderType::Limit, 10050, 100U, 10U));
    IntrusiveOrderNode* n2 =
        pool.acquire(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 200U, 20U));
    IntrusiveOrderNode* n3 =
        pool.acquire(Order(3U, 1U, Side::Buy, OrderType::Limit, 10050, 50U, 30U));

    level.push_back(n1);
    level.push_back(n2);
    level.push_back(n3);

    EXPECT_EQ(level.price(), 10050);
    EXPECT_EQ(level.order_count(), 3U);
    EXPECT_EQ(level.total_quantity(), 350U);
    EXPECT_EQ(level.front().id, 1U);

    // Unlink middle node n2 in O(1)
    level.unlink(n2);
    pool.release(n2);
    EXPECT_EQ(level.order_count(), 2U);
    EXPECT_EQ(level.total_quantity(), 150U);

    // Partial fill on front node n1
    level.fill_front(40U);
    EXPECT_EQ(level.front().id, 1U);
    EXPECT_EQ(level.front().remaining_quantity, 60U);
    EXPECT_EQ(level.total_quantity(), 110U);

    // Pop front node n1 -> n3 becomes front
    IntrusiveOrderNode* popped = level.pop_front();
    EXPECT_EQ(popped, n1);
    pool.release(popped);
    EXPECT_EQ(level.order_count(), 1U);
    EXPECT_EQ(level.front().id, 3U);
    EXPECT_EQ(level.total_quantity(), 50U);
}

TEST(ObjectPoolAndFlatBookTest, FlatOrderBookFunctionalAndSparseOrderIdCoverage) {
    FlatOrderBook book(1U);

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);

    ASSERT_TRUE(book.add_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10000, 100U, 10U)));
    ASSERT_TRUE(book.add_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 200U, 20U)));
    ASSERT_TRUE(book.add_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10025, 150U, 30U)));
    // Sparse large OrderId beyond direct_lookup_ size (16384)
    ASSERT_TRUE(
        book.add_order(Order(10'000'000U, 1U, Side::Sell, OrderType::Limit, 10075, 80U, 40U)));
    EXPECT_FALSE(book.add_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 50U, 50U)));

    EXPECT_EQ(book.best_bid(), 10050);
    EXPECT_EQ(book.best_bid_quantity(), 200U);
    EXPECT_EQ(book.best_ask(), 10075);
    EXPECT_EQ(book.best_ask_quantity(), 80U);
    EXPECT_EQ(book.spread(), 25);

    ASSERT_NE(book.find_order(10'000'000U), nullptr);
    EXPECT_EQ(book.find_order(10'000'000U)->remaining_quantity, 80U);

    // Cancel middle level (10025) and verify remaining bids
    const auto c3 = book.cancel_order(3U);
    ASSERT_TRUE(c3.has_value());
    EXPECT_EQ(book.bid_level_count(), 2U);

    // Cancel top bid (10050) -> best_bid pops back to 10000
    const auto c2 = book.cancel_order(2U);
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(book.best_bid(), 10000);

    // Fill sparse ask order completely
    book.fill_best_order(Side::Sell, 80U);
    EXPECT_EQ(book.find_order(10'000'000U), nullptr);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(ObjectPoolAndFlatBookTest, FastMatchingEngineBitForBitEquivalenceWithStlBaselineOn10kCommands) {
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
    for (std::size_t i = 0U; i < kNumCommands; ++i) {
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

    MatchingEngine stl_engine(1U);
    FastMatchingEngine fast_engine(1U);

    for (const auto& cmd : commands) {
        if (cmd.is_cancel) {
            EXPECT_EQ(fast_engine.cancel_order(cmd.cancel_id),
                      stl_engine.cancel_order(cmd.cancel_id));
        } else {
            const auto fast_res = fast_engine.submit_order(cmd.order);
            const auto stl_res = stl_engine.submit_order(cmd.order);
            EXPECT_EQ(fast_res.final_order, stl_res.final_order);
            EXPECT_EQ(fast_res.trades, stl_res.trades);
            EXPECT_EQ(fast_res.sequence_number, stl_res.sequence_number);
        }
    }

    EXPECT_GT(fast_engine.trades().size(), 1000U);
    EXPECT_EQ(fast_engine.trades(), stl_engine.trades());
    EXPECT_EQ(fast_engine.next_sequence_number(), stl_engine.next_sequence_number());
    EXPECT_EQ(fast_engine.next_trade_id(), stl_engine.next_trade_id());
    EXPECT_EQ(fast_engine.book().active_order_count(), stl_engine.book().active_order_count());
    EXPECT_EQ(fast_engine.book().get_depth(Side::Buy, 1000U),
              stl_engine.book().get_depth(Side::Buy, 1000U));
    EXPECT_EQ(fast_engine.book().get_depth(Side::Sell, 1000U),
              stl_engine.book().get_depth(Side::Sell, 1000U));
}

}  // namespace martingale::tests
