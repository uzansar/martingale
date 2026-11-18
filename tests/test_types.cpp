#include <gtest/gtest.h>
#include <martingale/core/types.hpp>

#include <limits>
#include <type_traits>

namespace martingale::tests {

TEST(CoreTypesTest, FixedWidthSizesAndSignedness) {
    static_assert(sizeof(OrderId) == 8U);
    static_assert(sizeof(TradeId) == 8U);
    static_assert(sizeof(InstrumentId) == 4U);
    static_assert(sizeof(Price) == 8U);
    static_assert(sizeof(Quantity) == 8U);
    static_assert(sizeof(Timestamp) == 8U);
    static_assert(sizeof(SequenceNumber) == 8U);

    static_assert(std::is_unsigned_v<OrderId>);
    static_assert(std::is_unsigned_v<TradeId>);
    static_assert(std::is_unsigned_v<InstrumentId>);
    static_assert(std::is_signed_v<Price>);
    static_assert(std::is_unsigned_v<Quantity>);
    static_assert(std::is_unsigned_v<Timestamp>);
    static_assert(std::is_unsigned_v<SequenceNumber>);

    EXPECT_EQ(sizeof(OrderId), 8U);
    EXPECT_EQ(sizeof(Price), 8U);
    EXPECT_EQ(sizeof(InstrumentId), 4U);
    EXPECT_EQ(kInvalidOrderId, 0U);
    EXPECT_EQ(kInvalidTradeId, 0U);
    EXPECT_EQ(kInvalidInstrumentId, 0U);
    EXPECT_EQ(kInvalidQuantity, 0U);
    EXPECT_EQ(kMarketOrderPrice, 0);
    EXPECT_EQ(kInvalidPrice, std::numeric_limits<Price>::min());
    EXPECT_EQ(kCacheLineSize, 64U);
}

TEST(CoreTypesTest, SignedPricePreventsCrossedSpreadUnderflow) {
    constexpr Price best_bid = 10050;
    constexpr Price best_ask = 10045;
    constexpr Price spread = best_ask - best_bid;

    static_assert(spread == -5);
    EXPECT_EQ(spread, -5);
    EXPECT_LT(spread, 0);
}

TEST(CoreTypesTest, CompactEnumUnderlyingTypes) {
    static_assert(sizeof(Side) == 1U);
    static_assert(sizeof(OrderType) == 1U);
    static_assert(sizeof(OrderStatus) == 1U);

    EXPECT_EQ(sizeof(Side), 1U);
    EXPECT_EQ(sizeof(OrderType), 1U);
    EXPECT_EQ(sizeof(OrderStatus), 1U);
}

TEST(CoreTypesTest, OppositeSideHelper) {
    static_assert(opposite_side(Side::Buy) == Side::Sell);
    static_assert(opposite_side(Side::Sell) == Side::Buy);

    EXPECT_EQ(opposite_side(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite_side(Side::Sell), Side::Buy);
}

TEST(CoreTypesTest, ValidationAndStatusHelpers) {
    static_assert(is_valid_side(Side::Buy));
    static_assert(is_valid_side(Side::Sell));
    static_assert(is_valid_order_type(OrderType::Limit));
    static_assert(is_valid_order_type(OrderType::Market));

    static_assert(is_active_status(OrderStatus::New));
    static_assert(is_active_status(OrderStatus::PartiallyFilled));
    static_assert(!is_active_status(OrderStatus::Filled));
    static_assert(!is_active_status(OrderStatus::Cancelled));
    static_assert(!is_active_status(OrderStatus::Rejected));

    EXPECT_TRUE(is_valid_side(Side::Buy));
    EXPECT_TRUE(is_valid_side(Side::Sell));
    EXPECT_FALSE(is_valid_side(static_cast<Side>(255)));

    EXPECT_TRUE(is_valid_order_type(OrderType::Limit));
    EXPECT_TRUE(is_valid_order_type(OrderType::Market));
    EXPECT_FALSE(is_valid_order_type(static_cast<OrderType>(99)));

    EXPECT_TRUE(is_active_status(OrderStatus::New));
    EXPECT_TRUE(is_active_status(OrderStatus::PartiallyFilled));
    EXPECT_FALSE(is_active_status(OrderStatus::Filled));
    EXPECT_FALSE(is_active_status(OrderStatus::Cancelled));
    EXPECT_FALSE(is_active_status(OrderStatus::Rejected));

    EXPECT_EQ(to_string_view(Side::Buy), "Buy");
    EXPECT_EQ(to_string_view(Side::Sell), "Sell");
    EXPECT_EQ(to_string_view(OrderType::Limit), "Limit");
    EXPECT_EQ(to_string_view(OrderType::Market), "Market");
    EXPECT_EQ(to_string_view(OrderStatus::New), "New");
    EXPECT_EQ(to_string_view(OrderStatus::PartiallyFilled), "PartiallyFilled");
    EXPECT_EQ(to_string_view(OrderStatus::Filled), "Filled");
    EXPECT_EQ(to_string_view(OrderStatus::Cancelled), "Cancelled");
    EXPECT_EQ(to_string_view(OrderStatus::Rejected), "Rejected");
}

}  // namespace martingale::tests
