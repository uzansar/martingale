#ifndef MARTINGALE_ENGINE_MATCHING_ENGINE_HPP
#define MARTINGALE_ENGINE_MATCHING_ENGINE_HPP

#include <martingale/book/order_book.hpp>
#include <martingale/core/order.hpp>
#include <martingale/core/trade.hpp>
#include <martingale/core/types.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace martingale {

/// Outcome returned synchronously for every submitted order command.
struct OrderResult {
    Order final_order{};
    std::vector<Trade> trades{};
    SequenceNumber sequence_number{0};
};

/// Single-threaded, deterministic price-time priority matching engine managing
/// one or more independent tradable instruments.
class MatchingEngine {
public:
    explicit MatchingEngine(InstrumentId default_instrument_id = 1U)
        : default_instrument_id_(default_instrument_id) {
        if (default_instrument_id_ == kInvalidInstrumentId) {
            throw std::invalid_argument("Default instrument_id must be non-zero");
        }
        books_.try_emplace(default_instrument_id_, default_instrument_id_);
    }

    /// Registers an additional instrument book. Returns false if already registered.
    bool register_instrument(InstrumentId instrument_id) {
        if (instrument_id == kInvalidInstrumentId) {
            throw std::invalid_argument("InstrumentId must be non-zero");
        }
        return books_.try_emplace(instrument_id, instrument_id).second;
    }

    [[nodiscard]] bool has_instrument(InstrumentId instrument_id) const noexcept {
        return books_.contains(instrument_id);
    }

    [[nodiscard]] std::size_t instrument_count() const noexcept { return books_.size(); }

    [[nodiscard]] InstrumentId instrument_id() const noexcept { return default_instrument_id_; }

    [[nodiscard]] bool has_active_order(OrderId order_id) const noexcept {
        return active_order_instruments_.contains(order_id);
    }

    [[nodiscard]] std::optional<InstrumentId> find_order_instrument(
        OrderId order_id) const noexcept {
        const auto it = active_order_instruments_.find(order_id);
        if (it == active_order_instruments_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] const OrderBook& book(InstrumentId instrument_id) const {
        const auto it = books_.find(instrument_id);
        if (it == books_.end()) {
            throw std::out_of_range("Unknown InstrumentId requested from MatchingEngine");
        }
        return it->second;
    }

    [[nodiscard]] const OrderBook& book() const { return book(default_instrument_id_); }

    [[nodiscard]] const std::vector<Trade>& trades() const noexcept { return trades_; }
    [[nodiscard]] SequenceNumber next_sequence_number() const noexcept {
        return next_sequence_number_;
    }
    [[nodiscard]] TradeId next_trade_id() const noexcept { return next_trade_id_; }

    /// Processes an incoming Limit or Market order under strict price-time priority.
    /// Rejects malformed or duplicate-ID orders without mutating any book state.
    [[nodiscard]] OrderResult submit_order(Order order) {
        const SequenceNumber cmd_seq = next_sequence_number_++;
        const auto book_it = books_.find(order.instrument_id);
        if (book_it == books_.end() || !is_valid_new_order(order)) {
            order.reject();
            return OrderResult{order, {}, cmd_seq};
        }

        OrderBook& target_book = book_it->second;
        const Side resting_side = opposite_side(order.side);
        OrderResult result{order, {}, cmd_seq};

        while (order.remaining_quantity > 0U) {
            const Order* resting = target_book.best_order(resting_side);
            if (resting == nullptr || !can_cross(order, *resting)) {
                break;
            }

            // Capture resting fields before fill_best_order potentially pops and erases it.
            const Quantity resting_remaining = resting->remaining_quantity;
            const Quantity exec_qty = std::min(order.remaining_quantity, resting_remaining);
            const Price exec_price = resting->price;
            const OrderId resting_id = resting->id;
            const OrderId buyer_id = (order.side == Side::Buy) ? order.id : resting_id;
            const OrderId seller_id = (order.side == Side::Sell) ? order.id : resting_id;

            order.fill(exec_qty);
            target_book.fill_best_order(resting_side, exec_qty);
            if (resting_remaining == exec_qty) {
                active_order_instruments_.erase(resting_id);
            }

            const Trade trade{
                .trade_id = next_trade_id_++,
                .price = exec_price,
                .quantity = exec_qty,
                .buyer_order_id = buyer_id,
                .seller_order_id = seller_id,
                .sequence_number = next_sequence_number_++,
                .timestamp = order.timestamp,
                .instrument_id = target_book.instrument_id(),
                .aggressor_side = order.side,
            };
            result.trades.push_back(trade);
            trades_.push_back(trade);
        }

        if (order.remaining_quantity > 0U) {
            if (order.type == OrderType::Limit) {
                if (target_book.add_order(order)) {
                    active_order_instruments_.emplace(order.id, order.instrument_id);
                }
            } else {
                // Unfilled Market order remainder never rests on the order book (IOC semantics).
                order.cancel();
            }
        }

        result.final_order = order;
        return result;
    }

    /// Cancels an active order scoped to a specific InstrumentId.
    [[nodiscard]] std::optional<Order> cancel_order(InstrumentId instrument_id, OrderId order_id) {
        ++next_sequence_number_;
        const auto book_it = books_.find(instrument_id);
        if (book_it == books_.end()) {
            return std::nullopt;
        }
        auto cancelled = book_it->second.cancel_order(order_id);
        if (cancelled.has_value()) {
            active_order_instruments_.erase(order_id);
        }
        return cancelled;
    }

    /// Cancels an active resting order by OrderId across registered instruments.
    [[nodiscard]] std::optional<Order> cancel_order(OrderId order_id) {
        ++next_sequence_number_;
        const auto inst_it = active_order_instruments_.find(order_id);
        if (inst_it == active_order_instruments_.end()) {
            return std::nullopt;
        }
        const auto book_it = books_.find(inst_it->second);
        if (book_it == books_.end()) {
            return std::nullopt;
        }
        auto cancelled = book_it->second.cancel_order(order_id);
        if (cancelled.has_value()) {
            active_order_instruments_.erase(inst_it);
        }
        return cancelled;
    }

private:
    [[nodiscard]] bool is_valid_new_order(const Order& order) const noexcept {
        if (order.id == kInvalidOrderId) {
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
        if (order.type == OrderType::Market && order.price < 0) {
            return false;
        }
        return !active_order_instruments_.contains(order.id);
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

    InstrumentId default_instrument_id_{1U};
    std::unordered_map<InstrumentId, OrderBook> books_{};
    std::unordered_map<OrderId, InstrumentId> active_order_instruments_{};
    std::vector<Trade> trades_{};
    SequenceNumber next_sequence_number_{1};
    TradeId next_trade_id_{1};
};

}  // namespace martingale

#endif  // MARTINGALE_ENGINE_MATCHING_ENGINE_HPP
