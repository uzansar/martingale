#include <gtest/gtest.h>
#include <martingale/book/order_book.hpp>

#include <optional>
#include <stdexcept>
#include <vector>

namespace martingale::tests {

TEST(OrderBookTest, EmptyBookQueries) {
    const OrderBook book(1U);

    EXPECT_EQ(book.instrument_id(), 1U);
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.active_order_count(), 0U);
    EXPECT_EQ(book.bid_level_count(), 0U);
    EXPECT_EQ(book.ask_level_count(), 0U);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.spread(), std::nullopt);
    EXPECT_EQ(book.mid_price_ticks(), std::nullopt);
    EXPECT_EQ(book.best_bid_quantity(), 0U);
    EXPECT_EQ(book.best_ask_quantity(), 0U);
    EXPECT_EQ(book.best_order(Side::Buy), nullptr);
    EXPECT_EQ(book.best_order(Side::Sell), nullptr);
    EXPECT_EQ(book.find_order(999U), nullptr);
    EXPECT_EQ(book.quantity_at_price(Side::Buy, 10000), 0U);
    EXPECT_TRUE(book.get_depth(Side::Buy, 5U).empty());
    EXPECT_TRUE(book.get_depth(Side::Sell, 5U).empty());
}

TEST(OrderBookTest, BidAndAskSortingAndBboQueries) {
    OrderBook book(1U);

    EXPECT_TRUE(book.add_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10000, 100U, 10U)));
    EXPECT_TRUE(book.add_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 200U, 20U)));
    EXPECT_TRUE(book.add_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10025, 150U, 30U)));
    EXPECT_TRUE(book.add_order(Order(4U, 1U, Side::Buy, OrderType::Limit, 10050, 50U, 40U)));

    EXPECT_TRUE(book.add_order(Order(5U, 1U, Side::Sell, OrderType::Limit, 10100, 300U, 50U)));
    EXPECT_TRUE(book.add_order(Order(6U, 1U, Side::Sell, OrderType::Limit, 10075, 120U, 60U)));
    EXPECT_TRUE(book.add_order(Order(7U, 1U, Side::Sell, OrderType::Limit, 10125, 80U, 70U)));

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.active_order_count(), 7U);
    EXPECT_EQ(book.bid_level_count(), 3U);
    EXPECT_EQ(book.ask_level_count(), 3U);

    EXPECT_EQ(book.best_bid(), 10050);
    EXPECT_EQ(book.best_ask(), 10075);
    EXPECT_EQ(book.best_bid_quantity(), 250U);
    EXPECT_EQ(book.best_ask_quantity(), 120U);
    EXPECT_EQ(book.spread(), 25);
    EXPECT_EQ(book.mid_price_ticks(), 10062);

    EXPECT_EQ(book.quantity_at_price(Side::Buy, 10050), 250U);
    EXPECT_EQ(book.quantity_at_price(Side::Buy, 10025), 150U);
    EXPECT_EQ(book.quantity_at_price(Side::Sell, 10100), 300U);
    EXPECT_EQ(book.quantity_at_price(Side::Sell, 10999), 0U);

    ASSERT_NE(book.best_order(Side::Buy), nullptr);
    EXPECT_EQ(book.best_order(Side::Buy)->id, 2U);
    ASSERT_NE(book.best_order(Side::Sell), nullptr);
    EXPECT_EQ(book.best_order(Side::Sell)->id, 6U);
}

TEST(OrderBookTest, RejectsDuplicateOrderIdAndInvalidInstrumentOrType) {
    OrderBook book(1U);

    EXPECT_TRUE(book.add_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10000, 100U, 10U)));
    EXPECT_FALSE(book.add_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10100, 50U, 20U)));
    EXPECT_EQ(book.active_order_count(), 1U);
    EXPECT_EQ(book.best_ask(), std::nullopt);

    EXPECT_THROW(
        static_cast<void>(book.add_order(Order(2U, 2U, Side::Buy, OrderType::Limit, 10000, 10U, 30U))),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(
                     book.add_order(Order(3U, 1U, Side::Buy, OrderType::Market, 10000, 10U, 40U))),
                 std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(book.add_order(Order(4U, 1U, Side::Buy, OrderType::Limit, 0, 10U, 50U))),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(book.add_order(Order(5U, 1U, Side::Buy, OrderType::Limit, -5, 10U, 60U))),
        std::invalid_argument);
}

TEST(OrderBookTest, ConstantTimeCancelAndAutomaticEmptyPriceLevelCleanup) {
    OrderBook book(1U);

    ASSERT_TRUE(book.add_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10050, 100U, 10U)));
    ASSERT_TRUE(book.add_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10050, 200U, 20U)));
    ASSERT_TRUE(book.add_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10000, 300U, 30U)));

    EXPECT_EQ(book.cancel_order(999U), std::nullopt);

    const auto c1 = book.cancel_order(1U);
    ASSERT_TRUE(c1.has_value());
    EXPECT_EQ(c1->id, 1U);
    EXPECT_EQ(c1->status, OrderStatus::Cancelled);
    EXPECT_EQ(book.find_order(1U), nullptr);
    EXPECT_EQ(book.best_bid(), 10050);
    EXPECT_EQ(book.best_bid_quantity(), 200U);
    EXPECT_EQ(book.bid_level_count(), 2U);

    const auto c2 = book.cancel_order(2U);
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(c2->id, 2U);
    EXPECT_EQ(c2->status, OrderStatus::Cancelled);
    EXPECT_EQ(book.bid_level_count(), 1U);
    EXPECT_EQ(book.best_bid(), 10000);
    EXPECT_EQ(book.best_bid_quantity(), 300U);
    EXPECT_EQ(book.active_order_count(), 1U);
}

TEST(OrderBookTest, FillBestOrderUpdatesBookAndRemovesCompletedOrders) {
    OrderBook book(1U);

    ASSERT_TRUE(book.add_order(Order(10U, 1U, Side::Sell, OrderType::Limit, 10075, 100U, 10U)));
    ASSERT_TRUE(book.add_order(Order(11U, 1U, Side::Sell, OrderType::Limit, 10075, 50U, 20U)));
    ASSERT_TRUE(book.add_order(Order(12U, 1U, Side::Sell, OrderType::Limit, 10100, 200U, 30U)));

    book.fill_best_order(Side::Sell, 40U);
    ASSERT_NE(book.find_order(10U), nullptr);
    EXPECT_EQ(book.find_order(10U)->remaining_quantity, 60U);
    EXPECT_EQ(book.find_order(10U)->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(book.best_ask_quantity(), 110U);
    EXPECT_EQ(book.active_order_count(), 3U);

    book.fill_best_order(Side::Sell, 60U);
    EXPECT_EQ(book.find_order(10U), nullptr);
    EXPECT_EQ(book.best_ask(), 10075);
    EXPECT_EQ(book.best_ask_quantity(), 50U);
    ASSERT_NE(book.best_order(Side::Sell), nullptr);
    EXPECT_EQ(book.best_order(Side::Sell)->id, 11U);
    EXPECT_EQ(book.active_order_count(), 2U);

    book.fill_best_order(Side::Sell, 50U);
    EXPECT_EQ(book.find_order(11U), nullptr);
    EXPECT_EQ(book.best_ask(), 10100);
    EXPECT_EQ(book.best_ask_quantity(), 200U);
    EXPECT_EQ(book.ask_level_count(), 1U);
    EXPECT_EQ(book.active_order_count(), 1U);
}

TEST(OrderBookTest, Level2DepthSnapshotOrderingAndTruncation) {
    OrderBook book(1U);

    ASSERT_TRUE(book.add_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10010, 10U, 1U)));
    ASSERT_TRUE(book.add_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 10040, 20U, 2U)));
    ASSERT_TRUE(book.add_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10040, 30U, 3U)));
    ASSERT_TRUE(book.add_order(Order(4U, 1U, Side::Buy, OrderType::Limit, 10030, 40U, 4U)));
    ASSERT_TRUE(book.add_order(Order(5U, 1U, Side::Buy, OrderType::Limit, 10020, 50U, 5U)));

    ASSERT_TRUE(book.add_order(Order(6U, 1U, Side::Sell, OrderType::Limit, 10050, 15U, 6U)));
    ASSERT_TRUE(book.add_order(Order(7U, 1U, Side::Sell, OrderType::Limit, 10060, 25U, 7U)));
    ASSERT_TRUE(book.add_order(Order(8U, 1U, Side::Sell, OrderType::Limit, 10060, 35U, 8U)));
    ASSERT_TRUE(book.add_order(Order(9U, 1U, Side::Sell, OrderType::Limit, 10070, 45U, 9U)));
    ASSERT_TRUE(book.add_order(Order(10U, 1U, Side::Sell, OrderType::Limit, 10080, 55U, 10U)));

    const std::vector<DepthLevel> top_bids = book.get_depth(Side::Buy, 2U);
    ASSERT_EQ(top_bids.size(), 2U);
    EXPECT_EQ(top_bids[0], (DepthLevel{10040, 50U, 2U}));
    EXPECT_EQ(top_bids[1], (DepthLevel{10030, 40U, 1U}));

    const std::vector<DepthLevel> top_asks = book.get_depth(Side::Sell, 3U);
    ASSERT_EQ(top_asks.size(), 3U);
    EXPECT_EQ(top_asks[0], (DepthLevel{10050, 15U, 1U}));
    EXPECT_EQ(top_asks[1], (DepthLevel{10060, 60U, 2U}));
    EXPECT_EQ(top_asks[2], (DepthLevel{10070, 45U, 1U}));
}

}  // namespace martingale::tests
