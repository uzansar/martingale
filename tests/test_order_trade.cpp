#include <gtest/gtest.h>
#include <martingale/core/order.hpp>
#include <martingale/core/trade.hpp>

#include <stdexcept>
#include <type_traits>

namespace martingale::tests {

TEST(OrderAndTradeTest, MemoryLayoutAndCacheLineFit) {
    static_assert(sizeof(Order) == 48U);
    static_assert(sizeof(Order) <= kCacheLineSize);
    static_assert(alignof(Order) == 8U);
    static_assert(std::is_trivially_copyable_v<Order>);

    static_assert(sizeof(Trade) == kCacheLineSize);
    static_assert(alignof(Trade) == kCacheLineSize);
    static_assert(std::is_trivially_copyable_v<Trade>);

    EXPECT_EQ(sizeof(Order), 48U);
    EXPECT_LE(sizeof(Order), kCacheLineSize);
    EXPECT_EQ(alignof(Order), 8U);
    EXPECT_EQ(sizeof(Trade), 64U);
    EXPECT_EQ(alignof(Trade), 64U);
    EXPECT_TRUE(std::is_trivially_copyable_v<Order>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Trade>);
}

TEST(OrderAndTradeTest, OrderConstructionAndInitialInvariants) {
    const Order order(1001U, 1U, Side::Buy, OrderType::Limit, 15025, 250U, 999000U);

    EXPECT_EQ(order.id, 1001U);
    EXPECT_EQ(order.instrument_id, 1U);
    EXPECT_EQ(order.side, Side::Buy);
    EXPECT_EQ(order.type, OrderType::Limit);
    EXPECT_EQ(order.status, OrderStatus::New);
    EXPECT_EQ(order.price, 15025);
    EXPECT_EQ(order.initial_quantity, 250U);
    EXPECT_EQ(order.remaining_quantity, 250U);
    EXPECT_EQ(order.filled_quantity(), 0U);
    EXPECT_EQ(order.timestamp, 999000U);
    EXPECT_TRUE(order.is_active());
    EXPECT_FALSE(order.is_filled());
}

TEST(OrderAndTradeTest, OrderPartialAndFullFillTransitions) {
    Order order(1002U, 7U, Side::Sell, OrderType::Limit, 20000, 100U, 123456U);

    order.fill(40U);
    EXPECT_EQ(order.remaining_quantity, 60U);
    EXPECT_EQ(order.filled_quantity(), 40U);
    EXPECT_EQ(order.status, OrderStatus::PartiallyFilled);
    EXPECT_TRUE(order.is_active());
    EXPECT_FALSE(order.is_filled());

    order.fill(60U);
    EXPECT_EQ(order.remaining_quantity, 0U);
    EXPECT_EQ(order.filled_quantity(), 100U);
    EXPECT_EQ(order.status, OrderStatus::Filled);
    EXPECT_FALSE(order.is_active());
    EXPECT_TRUE(order.is_filled());
}

TEST(OrderAndTradeTest, OrderFillRejectsInvalidQuantities) {
    Order order(1003U, 2U, Side::Buy, OrderType::Limit, 5000, 50U, 500U);

    EXPECT_THROW(order.fill(0U), std::invalid_argument);
    EXPECT_EQ(order.remaining_quantity, 50U);
    EXPECT_EQ(order.status, OrderStatus::New);

    EXPECT_THROW(order.fill(51U), std::invalid_argument);
    EXPECT_EQ(order.remaining_quantity, 50U);
    EXPECT_EQ(order.status, OrderStatus::New);

    order.fill(50U);
    EXPECT_THROW(order.fill(1U), std::logic_error);
    EXPECT_EQ(order.remaining_quantity, 0U);
    EXPECT_EQ(order.status, OrderStatus::Filled);
}

TEST(OrderAndTradeTest, OrderCancellationPreservesFilledQuantity) {
    Order order(1004U, 3U, Side::Sell, OrderType::Limit, 12500, 100U, 777U);
    order.fill(35U);
    order.cancel();

    EXPECT_EQ(order.status, OrderStatus::Cancelled);
    EXPECT_FALSE(order.is_active());
    EXPECT_FALSE(order.is_filled());
    EXPECT_EQ(order.remaining_quantity, 65U);
    EXPECT_EQ(order.filled_quantity(), 35U);

    EXPECT_THROW(order.cancel(), std::logic_error);
    EXPECT_THROW(order.fill(10U), std::logic_error);
}

TEST(OrderAndTradeTest, TradeConstructionAndEquality) {
    const Trade t1{
        .trade_id = 500U,
        .price = 15025,
        .quantity = 40U,
        .buyer_order_id = 1001U,
        .seller_order_id = 1002U,
        .sequence_number = 42U,
        .timestamp = 999500U,
        .instrument_id = 1U,
        .aggressor_side = Side::Buy,
    };
    const Trade t2 = t1;

    EXPECT_EQ(t1, t2);
    EXPECT_EQ(t1.trade_id, 500U);
    EXPECT_EQ(t1.instrument_id, 1U);
    EXPECT_EQ(t1.price, 15025);
    EXPECT_EQ(t1.quantity, 40U);
    EXPECT_EQ(t1.buyer_order_id, 1001U);
    EXPECT_EQ(t1.seller_order_id, 1002U);
    EXPECT_EQ(t1.aggressor_side, Side::Buy);
    EXPECT_EQ(t1.sequence_number, 42U);
    EXPECT_EQ(t1.timestamp, 999500U);
}

}  // namespace martingale::tests
