#include <gtest/gtest.h>
#include <martingale/book/price_level.hpp>

#include <stdexcept>
#include <vector>

namespace martingale::tests {

TEST(PriceLevelTest, InitialEmptyState) {
    const PriceLevel level(10500);

    EXPECT_EQ(level.price(), 10500);
    EXPECT_EQ(level.total_quantity(), 0U);
    EXPECT_EQ(level.order_count(), 0U);
    EXPECT_TRUE(level.empty());
    EXPECT_THROW(static_cast<void>(level.front()), std::out_of_range);
}

TEST(PriceLevelTest, MaintainsFifoOrderAndAggregateQuantity) {
    PriceLevel level(10500);

    const Order o1(1U, 1U, Side::Buy, OrderType::Limit, 10500, 100U, 1000U);
    const Order o2(2U, 1U, Side::Buy, OrderType::Limit, 10500, 250U, 2000U);
    const Order o3(3U, 1U, Side::Buy, OrderType::Limit, 10500, 50U, 3000U);

    static_cast<void>(level.add_order(o1));
    static_cast<void>(level.add_order(o2));
    static_cast<void>(level.add_order(o3));

    EXPECT_FALSE(level.empty());
    EXPECT_EQ(level.order_count(), 3U);
    EXPECT_EQ(level.total_quantity(), 400U);
    EXPECT_EQ(level.front().id, 1U);

    std::vector<OrderId> ids;
    for (const auto& order : level) {
        ids.push_back(order.id);
    }
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids[0], 1U);
    EXPECT_EQ(ids[1], 2U);
    EXPECT_EQ(ids[2], 3U);
}

TEST(PriceLevelTest, RejectsMismatchedPriceOrInactiveOrders) {
    PriceLevel level(10500);

    const Order wrong_price(10U, 1U, Side::Buy, OrderType::Limit, 10501, 100U, 1000U);
    EXPECT_THROW(static_cast<void>(level.add_order(wrong_price)), std::invalid_argument);

    const Order zero_qty(11U, 1U, Side::Buy, OrderType::Limit, 10500, 0U, 1000U);
    EXPECT_THROW(static_cast<void>(level.add_order(zero_qty)), std::invalid_argument);

    Order cancelled(12U, 1U, Side::Buy, OrderType::Limit, 10500, 100U, 1000U);
    cancelled.cancel();
    EXPECT_THROW(static_cast<void>(level.add_order(cancelled)), std::invalid_argument);

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.total_quantity(), 0U);
}

TEST(PriceLevelTest, PartialAndFullFrontFillsUpdateAggregateQuantity) {
    PriceLevel level(10500);

    const auto it1 = level.add_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10500, 100U, 10U));
    static_cast<void>(level.add_order(Order(2U, 1U, Side::Sell, OrderType::Limit, 10500, 300U, 20U)));

    EXPECT_EQ(level.total_quantity(), 400U);

    level.fill_front(40U);
    EXPECT_EQ(level.front().id, 1U);
    EXPECT_EQ(level.front().remaining_quantity, 60U);
    EXPECT_EQ(level.front().status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(level.total_quantity(), 360U);

    level.fill_front(60U);
    EXPECT_TRUE(level.front().is_filled());
    EXPECT_EQ(level.total_quantity(), 300U);

    const Order removed = level.remove_order(it1);
    EXPECT_EQ(removed.id, 1U);
    EXPECT_TRUE(removed.is_filled());
    EXPECT_EQ(level.order_count(), 1U);
    EXPECT_EQ(level.front().id, 2U);
    EXPECT_EQ(level.total_quantity(), 300U);
}

TEST(PriceLevelTest, ArbitraryMiddleOrderRemovalInConstantTime) {
    PriceLevel level(10500);

    const auto it1 = level.add_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10500, 100U, 10U));
    const auto it2 = level.add_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10500, 250U, 20U));
    const auto it3 = level.add_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10500, 50U, 30U));

    const Order removed = level.remove_order(it2);
    EXPECT_EQ(removed.id, 2U);
    EXPECT_EQ(removed.remaining_quantity, 250U);

    EXPECT_EQ(level.order_count(), 2U);
    EXPECT_EQ(level.total_quantity(), 150U);
    EXPECT_EQ(it1->id, 1U);
    EXPECT_EQ(it3->id, 3U);

    std::vector<OrderId> ids;
    for (const auto& order : level) {
        ids.push_back(order.id);
    }
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ids[0], 1U);
    EXPECT_EQ(ids[1], 3U);
}

}  // namespace martingale::tests
