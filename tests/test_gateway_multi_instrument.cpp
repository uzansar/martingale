#include <gtest/gtest.h>
#include <martingale/engine/matching_engine.hpp>
#include <martingale/gateway/order_gateway.hpp>

#include <optional>
#include <stdexcept>

namespace martingale::tests {

TEST(GatewayAndMultiInstrumentTest, MultiInstrumentRegistrationAndBookIsolation) {
    MatchingEngine engine(1U);

    EXPECT_TRUE(engine.has_instrument(1U));
    EXPECT_FALSE(engine.has_instrument(2U));
    EXPECT_TRUE(engine.register_instrument(2U));
    EXPECT_FALSE(engine.register_instrument(2U));
    EXPECT_TRUE(engine.register_instrument(3U));
    EXPECT_EQ(engine.instrument_count(), 3U);

    EXPECT_THROW(static_cast<void>(engine.register_instrument(kInvalidInstrumentId)),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(engine.book(999U)), std::out_of_range);

    // Non-interacting resting orders at same price on separate instruments
    const auto r1 = engine.submit_order(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U));
    const auto r2 = engine.submit_order(Order(2U, 2U, Side::Buy, OrderType::Limit, 10050, 100U, 20U));

    EXPECT_TRUE(r1.trades.empty());
    EXPECT_TRUE(r2.trades.empty());
    EXPECT_TRUE(engine.trades().empty());
    EXPECT_EQ(engine.book(1U).best_ask(), 10050);
    EXPECT_EQ(engine.book(1U).best_bid(), std::nullopt);
    EXPECT_EQ(engine.book(2U).best_bid(), 10050);
    EXPECT_EQ(engine.book(2U).best_ask(), std::nullopt);

    // Crossing each instrument independently
    const auto r3 = engine.submit_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 10050, 60U, 30U));
    const auto r4 = engine.submit_order(Order(4U, 2U, Side::Sell, OrderType::Limit, 10050, 40U, 40U));

    ASSERT_EQ(r3.trades.size(), 1U);
    EXPECT_EQ(r3.trades[0].trade_id, 1U);
    EXPECT_EQ(r3.trades[0].instrument_id, 1U);
    EXPECT_EQ(r3.trades[0].quantity, 60U);

    ASSERT_EQ(r4.trades.size(), 1U);
    EXPECT_EQ(r4.trades[0].trade_id, 2U);
    EXPECT_EQ(r4.trades[0].instrument_id, 2U);
    EXPECT_EQ(r4.trades[0].quantity, 40U);

    EXPECT_EQ(engine.trades().size(), 2U);
    EXPECT_EQ(engine.book(1U).best_ask_quantity(), 40U);
    EXPECT_EQ(engine.book(2U).best_bid_quantity(), 60U);
    EXPECT_TRUE(engine.book(3U).empty());
}

TEST(GatewayAndMultiInstrumentTest, MultiInstrumentCrossInstrumentOrderIdUniquenessAndCancellation) {
    MatchingEngine engine(1U);
    ASSERT_TRUE(engine.register_instrument(2U));

    const auto r1 = engine.submit_order(Order(100U, 1U, Side::Buy, OrderType::Limit, 10000, 50U, 10U));
    EXPECT_EQ(r1.final_order.status, OrderStatus::New);
    EXPECT_TRUE(engine.has_active_order(100U));
    EXPECT_EQ(engine.find_order_instrument(100U), 1U);

    // Same OrderId cannot be submitted on a different instrument while active
    const auto dup =
        engine.submit_order(Order(100U, 2U, Side::Sell, OrderType::Limit, 10050, 50U, 20U));
    EXPECT_EQ(dup.final_order.status, OrderStatus::Rejected);
    EXPECT_TRUE(engine.book(2U).empty());

    // Instrument-scoped cancellation rejects mismatched instrument
    EXPECT_EQ(engine.cancel_order(2U, 100U), std::nullopt);
    EXPECT_TRUE(engine.has_active_order(100U));

    // Correct instrument succeeds and clears active-order lookup
    const auto cancelled = engine.cancel_order(1U, 100U);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->status, OrderStatus::Cancelled);
    EXPECT_FALSE(engine.has_active_order(100U));
    EXPECT_EQ(engine.find_order_instrument(100U), std::nullopt);
}

TEST(GatewayAndMultiInstrumentTest, GatewayAcceptsValidLimitAndMarketOrders) {
    MatchingEngine engine(1U);
    ASSERT_TRUE(engine.register_instrument(2U));
    OrderGateway gateway(engine);

    const auto res1 =
        gateway.validate_and_submit(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U));
    EXPECT_TRUE(res1.accepted);
    EXPECT_EQ(res1.reject_reason, RejectReason::None);
    EXPECT_EQ(res1.engine_result.final_order.status, OrderStatus::New);

    const auto res2 = gateway.validate_and_submit(
        Order(2U, 1U, Side::Buy, OrderType::Market, kMarketOrderPrice, 40U, 20U));
    EXPECT_TRUE(res2.accepted);
    EXPECT_EQ(res2.reject_reason, RejectReason::None);
    ASSERT_EQ(res2.engine_result.trades.size(), 1U);
    EXPECT_EQ(res2.engine_result.trades[0].price, 10050);
    EXPECT_EQ(res2.engine_result.trades[0].quantity, 40U);

    const auto res3 =
        gateway.validate_and_submit(Order(3U, 2U, Side::Buy, OrderType::Limit, 5000, 25U, 30U));
    EXPECT_TRUE(res3.accepted);
    EXPECT_EQ(res3.reject_reason, RejectReason::None);

    EXPECT_EQ(gateway.accepted_order_count(), 3U);
    EXPECT_EQ(gateway.rejected_order_count(), 0U);
}

TEST(GatewayAndMultiInstrumentTest, GatewayRejectsMalformedOrdersWithoutAdvancingEngineSequence) {
    MatchingEngine engine(1U);
    OrderGateway gateway(engine);

    // Seed one active order so DuplicateOrderId can be tested
    ASSERT_TRUE(
        gateway.validate_and_submit(Order(1U, 1U, Side::Buy, OrderType::Limit, 10000, 50U, 10U))
            .accepted);
    const SequenceNumber seq_before = engine.next_sequence_number();

    const auto r_zero_id = gateway.validate_and_submit(
        Order(kInvalidOrderId, 1U, Side::Buy, OrderType::Limit, 10000, 10U, 11U));
    const auto r_unknown_inst =
        gateway.validate_and_submit(Order(2U, 99U, Side::Buy, OrderType::Limit, 10000, 10U, 12U));
    const auto r_bad_side = gateway.validate_and_submit(
        Order(3U, 1U, static_cast<Side>(9), OrderType::Limit, 10000, 10U, 13U));
    const auto r_bad_type = gateway.validate_and_submit(
        Order(4U, 1U, Side::Buy, static_cast<OrderType>(9), 10000, 10U, 14U));

    Order non_new_order(5U, 1U, Side::Buy, OrderType::Limit, 10000, 10U, 15U);
    non_new_order.cancel();
    const auto r_bad_state = gateway.validate_and_submit(non_new_order);

    const auto r_zero_qty =
        gateway.validate_and_submit(Order(6U, 1U, Side::Buy, OrderType::Limit, 10000, 0U, 16U));
    const auto r_bad_limit_price =
        gateway.validate_and_submit(Order(7U, 1U, Side::Buy, OrderType::Limit, 0, 10U, 17U));
    const auto r_bad_mkt_price =
        gateway.validate_and_submit(Order(8U, 1U, Side::Buy, OrderType::Market, -1, 10U, 18U));
    const auto r_dup_id =
        gateway.validate_and_submit(Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 10U, 19U));

    EXPECT_FALSE(r_zero_id.accepted);
    EXPECT_EQ(r_zero_id.reject_reason, RejectReason::InvalidOrderId);
    EXPECT_FALSE(r_unknown_inst.accepted);
    EXPECT_EQ(r_unknown_inst.reject_reason, RejectReason::UnknownInstrument);
    EXPECT_FALSE(r_bad_side.accepted);
    EXPECT_EQ(r_bad_side.reject_reason, RejectReason::InvalidSide);
    EXPECT_FALSE(r_bad_type.accepted);
    EXPECT_EQ(r_bad_type.reject_reason, RejectReason::InvalidOrderType);
    EXPECT_FALSE(r_bad_state.accepted);
    EXPECT_EQ(r_bad_state.reject_reason, RejectReason::InvalidOrderState);
    EXPECT_FALSE(r_zero_qty.accepted);
    EXPECT_EQ(r_zero_qty.reject_reason, RejectReason::InvalidQuantity);
    EXPECT_FALSE(r_bad_limit_price.accepted);
    EXPECT_EQ(r_bad_limit_price.reject_reason, RejectReason::InvalidPrice);
    EXPECT_FALSE(r_bad_mkt_price.accepted);
    EXPECT_EQ(r_bad_mkt_price.reject_reason, RejectReason::InvalidPrice);
    EXPECT_FALSE(r_dup_id.accepted);
    EXPECT_EQ(r_dup_id.reject_reason, RejectReason::DuplicateOrderId);

    EXPECT_EQ(engine.next_sequence_number(), seq_before);
    EXPECT_EQ(gateway.accepted_order_count(), 1U);
    EXPECT_EQ(gateway.rejected_order_count(), 9U);

    EXPECT_FALSE(to_string_view(RejectReason::None).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InvalidOrderId).empty());
    EXPECT_FALSE(to_string_view(RejectReason::UnknownInstrument).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InvalidSide).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InvalidOrderType).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InvalidOrderState).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InvalidQuantity).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InvalidPrice).empty());
    EXPECT_FALSE(to_string_view(RejectReason::DuplicateOrderId).empty());
    EXPECT_FALSE(to_string_view(RejectReason::OrderNotFound).empty());
    EXPECT_FALSE(to_string_view(RejectReason::InstrumentMismatch).empty());
}

TEST(GatewayAndMultiInstrumentTest, GatewayValidatesCancellationRequests) {
    MatchingEngine engine(1U);
    ASSERT_TRUE(engine.register_instrument(2U));
    OrderGateway gateway(engine);

    ASSERT_TRUE(
        gateway.validate_and_submit(Order(10U, 1U, Side::Buy, OrderType::Limit, 10000, 50U, 10U))
            .accepted);

    const auto bad_id = gateway.validate_and_cancel(1U, kInvalidOrderId);
    EXPECT_FALSE(bad_id.accepted);
    EXPECT_EQ(bad_id.reject_reason, RejectReason::InvalidOrderId);

    const auto bad_inst = gateway.validate_and_cancel(99U, 10U);
    EXPECT_FALSE(bad_inst.accepted);
    EXPECT_EQ(bad_inst.reject_reason, RejectReason::UnknownInstrument);

    const auto mismatch = gateway.validate_and_cancel(2U, 10U);
    EXPECT_FALSE(mismatch.accepted);
    EXPECT_EQ(mismatch.reject_reason, RejectReason::InstrumentMismatch);

    const auto not_found = gateway.validate_and_cancel(1U, 777U);
    EXPECT_FALSE(not_found.accepted);
    EXPECT_EQ(not_found.reject_reason, RejectReason::OrderNotFound);

    const auto ok = gateway.validate_and_cancel(1U, 10U);
    EXPECT_TRUE(ok.accepted);
    EXPECT_EQ(ok.reject_reason, RejectReason::None);
    ASSERT_TRUE(ok.cancelled_order.has_value());
    EXPECT_EQ(ok.cancelled_order->status, OrderStatus::Cancelled);
    EXPECT_TRUE(engine.book(1U).empty());
}

}  // namespace martingale::tests
