#ifndef MARTINGALE_ENGINE_MATCHING_ENGINE_HPP
#define MARTINGALE_ENGINE_MATCHING_ENGINE_HPP

#include <martingale/book/flat_order_book.hpp>
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

/// Single-threaded, deterministic price-time priority matching engine parameterized
/// by order book implementation (OrderBook STL baseline or FlatOrderBook pooled engine).
template <typename BookType = OrderBook>
class BasicMatchingEngine {
public:
    explicit BasicMatchingEngine(InstrumentId default_instrument_id = 1U)
        : default_instrument_id_(validate_instrument_id(default_instrument_id)),
          default_book_(default_instrument_id_) {}

    /// Registers an additional instrument book. Returns false if already registered.
    bool register_instrument(InstrumentId instrument_id) {
        if (instrument_id == kInvalidInstrumentId) {
            throw std::invalid_argument("InstrumentId must be non-zero");
        }
        if (instrument_id == default_instrument_id_) {
            return false;
        }
        return extra_books_.try_emplace(instrument_id, instrument_id).second;
    }

    [[nodiscard]] bool has_instrument(InstrumentId instrument_id) const noexcept {
        return instrument_id == default_instrument_id_ || extra_books_.contains(instrument_id);
    }

    [[nodiscard]] std::size_t instrument_count() const noexcept {
        return 1U + extra_books_.size();
    }

    [[nodiscard]] InstrumentId instrument_id() const noexcept { return default_instrument_id_; }

    [[nodiscard]] bool has_active_order(OrderId order_id) const noexcept {
        return default_book_.find_order(order_id) != nullptr ||
               (!extra_books_.empty() && active_extra_order_instruments_.contains(order_id));
    }

    [[nodiscard]] std::optional<InstrumentId> find_order_instrument(
        OrderId order_id) const noexcept {
        if (default_book_.find_order(order_id) != nullptr) {
            return default_instrument_id_;
        }
        if (!extra_books_.empty()) {
            const auto it = active_extra_order_instruments_.find(order_id);
            if (it != active_extra_order_instruments_.end()) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const BookType& book(InstrumentId instrument_id) const {
        if (instrument_id == default_instrument_id_) {
            return default_book_;
        }
        const auto it = extra_books_.find(instrument_id);
        if (it == extra_books_.end()) {
            throw std::out_of_range("Unknown InstrumentId requested from MatchingEngine");
        }
        return it->second;
    }

    [[nodiscard]] const BookType& book() const noexcept { return default_book_; }

    [[nodiscard]] const std::vector<Trade>& trades() const noexcept { return trades_; }
    [[nodiscard]] SequenceNumber next_sequence_number() const noexcept {
        return next_sequence_number_;
    }
    [[nodiscard]] TradeId next_trade_id() const noexcept { return next_trade_id_; }

    /// Processes an incoming Limit or Market order under strict price-time priority.
    /// Rejects malformed or duplicate-ID orders without mutating any book state.
    [[nodiscard]] OrderResult submit_order(Order order) {
        const SequenceNumber cmd_seq = next_sequence_number_++;
        if (order.instrument_id == default_instrument_id_) {
            return process_order_on_book(order, default_book_, true, cmd_seq);
        }

        const auto book_it = extra_books_.find(order.instrument_id);
        if (book_it == extra_books_.end()) {
            order.reject();
            return OrderResult{order, {}, cmd_seq};
        }
        return process_order_on_book(order, book_it->second, false, cmd_seq);
    }

    /// Cancels an active order scoped to a specific InstrumentId.
    [[nodiscard]] std::optional<Order> cancel_order(InstrumentId instrument_id, OrderId order_id) {
        ++next_sequence_number_;
        if (instrument_id == default_instrument_id_) {
            return default_book_.cancel_order(order_id);
        }
        const auto book_it = extra_books_.find(instrument_id);
        if (book_it == extra_books_.end()) {
            return std::nullopt;
        }
        auto cancelled = book_it->second.cancel_order(order_id);
        if (cancelled.has_value()) {
            active_extra_order_instruments_.erase(order_id);
        }
        return cancelled;
    }

    /// Cancels an active resting order by OrderId across registered instruments.
    [[nodiscard]] std::optional<Order> cancel_order(OrderId order_id) {
        ++next_sequence_number_;
        auto default_cancelled = default_book_.cancel_order(order_id);
        if (default_cancelled.has_value() || extra_books_.empty()) {
            return default_cancelled;
        }
        const auto inst_it = active_extra_order_instruments_.find(order_id);
        if (inst_it == active_extra_order_instruments_.end()) {
            return std::nullopt;
        }
        const auto book_it = extra_books_.find(inst_it->second);
        if (book_it == extra_books_.end()) {
            return std::nullopt;
        }
        auto cancelled = book_it->second.cancel_order(order_id);
        if (cancelled.has_value()) {
            active_extra_order_instruments_.erase(inst_it);
        }
        return cancelled;
    }

private:
    [[nodiscard]] static InstrumentId validate_instrument_id(InstrumentId instrument_id) {
        if (instrument_id == kInvalidInstrumentId) {
            throw std::invalid_argument("Default instrument_id must be non-zero");
        }
        return instrument_id;
    }

    [[nodiscard]] OrderResult process_order_on_book(Order order, BookType& target_book,
                                                    bool is_default_book,
                                                    SequenceNumber cmd_seq) {
        if (!is_valid_new_order(order)) {
            order.reject();
            return OrderResult{order, {}, cmd_seq};
        }

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
            if (!is_default_book && resting_remaining == exec_qty) {
                active_extra_order_instruments_.erase(resting_id);
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
                if (target_book.add_order(order) && !is_default_book) {
                    active_extra_order_instruments_.emplace(order.id, order.instrument_id);
                }
            } else {
                // Unfilled Market order remainder never rests on the order book (IOC semantics).
                order.cancel();
            }
        }

        result.final_order = order;
        return result;
    }

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
        return !has_active_order(order.id);
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
    BookType default_book_;
    std::unordered_map<InstrumentId, BookType> extra_books_{};
    std::unordered_map<OrderId, InstrumentId> active_extra_order_instruments_{};
    std::vector<Trade> trades_{};
    SequenceNumber next_sequence_number_{1};
    TradeId next_trade_id_{1};
};

using MatchingEngine = BasicMatchingEngine<OrderBook>;
using FastMatchingEngine = BasicMatchingEngine<FlatOrderBook>;

}  // namespace martingale

#endif  // MARTINGALE_ENGINE_MATCHING_ENGINE_HPP
