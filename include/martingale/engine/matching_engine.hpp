#ifndef MARTINGALE_ENGINE_MATCHING_ENGINE_HPP
#define MARTINGALE_ENGINE_MATCHING_ENGINE_HPP

#include <martingale/book/order_book.hpp>
#include <martingale/core/order.hpp>
#include <martingale/core/trade.hpp>
#include <martingale/core/types.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace martingale {

/// Outcome returned synchronously for every submitted order command.
struct OrderResult {
    Order final_order{};
    std::vector<Trade> trades{};
    SequenceNumber sequence_number{0};
};

/// Single-threaded, deterministic price-time priority matching engine for a single instrument.
class MatchingEngine {
public:
    explicit MatchingEngine(InstrumentId instrument_id = 1U) noexcept : book_(instrument_id) {}

    [[nodiscard]] InstrumentId instrument_id() const noexcept { return book_.instrument_id(); }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const std::vector<Trade>& trades() const noexcept { return trades_; }
    [[nodiscard]] SequenceNumber next_sequence_number() const noexcept {
        return next_sequence_number_;
    }
    [[nodiscard]] TradeId next_trade_id() const noexcept { return next_trade_id_; }

    /// Processes an incoming Limit or Market order under strict price-time priority.
    /// Rejects malformed or duplicate-ID orders without mutating book state.
    [[nodiscard]] OrderResult submit_order(Order order) {
        const SequenceNumber cmd_seq = next_sequence_number_++;
        if (!is_valid_new_order(order)) {
            order.reject();
            return OrderResult{order, {}, cmd_seq};
        }

        const Side resting_side = opposite_side(order.side);
        OrderResult result{order, {}, cmd_seq};

        while (order.remaining_quantity > 0U) {
            const Order* resting = book_.best_order(resting_side);
            if (resting == nullptr || !can_cross(order, *resting)) {
                break;
            }

            // Capture resting fields before fill_best_order potentially pops and erases it.
            const Quantity exec_qty = std::min(order.remaining_quantity, resting->remaining_quantity);
            const Price exec_price = resting->price;
            const OrderId resting_id = resting->id;
            const OrderId buyer_id = (order.side == Side::Buy) ? order.id : resting_id;
            const OrderId seller_id = (order.side == Side::Sell) ? order.id : resting_id;

            order.fill(exec_qty);
            book_.fill_best_order(resting_side, exec_qty);

            const Trade trade{
                .trade_id = next_trade_id_++,
                .price = exec_price,
                .quantity = exec_qty,
                .buyer_order_id = buyer_id,
                .seller_order_id = seller_id,
                .sequence_number = next_sequence_number_++,
                .timestamp = order.timestamp,
                .instrument_id = book_.instrument_id(),
                .aggressor_side = order.side,
            };
            result.trades.push_back(trade);
            trades_.push_back(trade);
        }

        if (order.remaining_quantity > 0U) {
            if (order.type == OrderType::Limit) {
                static_cast<void>(book_.add_order(order));
            } else {
                // Unfilled Market order remainder never rests on the order book (IOC semantics).
                order.cancel();
            }
        }

        result.final_order = order;
        return result;
    }

    /// Cancels an active resting order by OrderId, advancing sequence number deterministically.
    [[nodiscard]] std::optional<Order> cancel_order(OrderId order_id) {
        ++next_sequence_number_;
        return book_.cancel_order(order_id);
    }

private:
    [[nodiscard]] bool is_valid_new_order(const Order& order) const noexcept {
        if (order.id == kInvalidOrderId || order.instrument_id != book_.instrument_id()) {
            return false;
        }
        if (!is_valid_side(order.side) || !is_valid_order_type(order.type)) {
            return false;
        }
        if (order.status != OrderStatus::New || order.initial_quantity == 0U ||
            order.remaining_quantity != order.initial_quantity) {
            return false;
        }
        if (order.type == OrderType::Limit && order.price <= 0) {
            return false;
        }
        return book_.find_order(order.id) == nullptr;
    }

    [[nodiscard]] static constexpr bool can_cross(const Order& incoming,
                                                  const Order& resting) noexcept {
        if (incoming.type == OrderType::Market) {
            return true;
        }
        if (incoming.side == Side::Buy) {
            return incoming.price >= resting.price;
        }
        return incoming.price <= resting.price;
    }

    OrderBook book_;
    std::vector<Trade> trades_{};
    SequenceNumber next_sequence_number_{1};
    TradeId next_trade_id_{1};
};

}  // namespace martingale

#endif  // MARTINGALE_ENGINE_MATCHING_ENGINE_HPP
