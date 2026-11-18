#ifndef MARTINGALE_BOOK_ORDER_BOOK_HPP
#define MARTINGALE_BOOK_ORDER_BOOK_HPP

#include <martingale/book/price_level.hpp>
#include <martingale/core/order.hpp>
#include <martingale/core/types.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace martingale {

/// Aggregated L2 market data snapshot entry for a single price level.
struct DepthLevel {
    Price price{kInvalidPrice};
    Quantity quantity{0};
    std::size_t order_count{0};

    [[nodiscard]] constexpr bool operator==(const DepthLevel&) const noexcept = default;
};

/// Single-instrument Limit Order Book using STL associative/list containers as a
/// verified correctness baseline prior to custom pool/intrusive optimization.
class OrderBook {
public:
    explicit OrderBook(InstrumentId instrument_id) noexcept : instrument_id_(instrument_id) {}

    [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_id_; }
    [[nodiscard]] bool empty() const noexcept { return order_lookup_.empty(); }
    [[nodiscard]] std::size_t active_order_count() const noexcept { return order_lookup_.size(); }
    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids_.empty()) {
            return std::nullopt;
        }
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->first;
    }

    [[nodiscard]] Quantity best_bid_quantity() const noexcept {
        return bids_.empty() ? 0U : bids_.begin()->second.total_quantity();
    }

    [[nodiscard]] Quantity best_ask_quantity() const noexcept {
        return asks_.empty() ? 0U : asks_.begin()->second.total_quantity();
    }

    [[nodiscard]] std::optional<Price> spread() const noexcept {
        const auto bid = best_bid();
        const auto ask = best_ask();
        if (!bid.has_value() || !ask.has_value()) {
            return std::nullopt;
        }
        return *ask - *bid;
    }

    [[nodiscard]] std::optional<Price> mid_price_ticks() const noexcept {
        const auto bid = best_bid();
        const auto ask = best_ask();
        if (!bid.has_value() || !ask.has_value()) {
            return std::nullopt;
        }
        // Avoid signed overflow on (bid + ask) / 2.
        return *bid + (*ask - *bid) / 2;
    }

    [[nodiscard]] Quantity quantity_at_price(Side side, Price price) const noexcept {
        return side == Side::Buy ? quantity_at_price_impl(bids_, price)
                                 : quantity_at_price_impl(asks_, price);
    }

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept {
        const auto it = order_lookup_.find(id);
        if (it == order_lookup_.end()) {
            return nullptr;
        }
        return &(*it->second.iterator);
    }

    [[nodiscard]] const Order* best_order(Side side) const noexcept {
        return side == Side::Buy ? best_order_impl(bids_) : best_order_impl(asks_);
    }

    /// Inserts an active limit order into the book.
    /// Returns false if OrderId already exists; throws std::invalid_argument on invalid fields.
    [[nodiscard]] bool add_order(const Order& order) {
        if (order.instrument_id != instrument_id_) {
            throw std::invalid_argument("Order instrument_id does not match OrderBook");
        }
        if (order.type != OrderType::Limit) {
            throw std::invalid_argument("Only Limit orders can rest on the OrderBook");
        }
        if (order.price <= 0) {
            throw std::invalid_argument("Limit order price must be positive");
        }
        if (!is_valid_side(order.side) || !order.is_active() || order.remaining_quantity == 0U) {
            throw std::invalid_argument("Invalid or inactive order");
        }
        if (order_lookup_.contains(order.id)) {
            return false;
        }

        const auto order_it = (order.side == Side::Buy) ? insert_into_map(bids_, order)
                                                        : insert_into_map(asks_, order);
        order_lookup_.emplace(order.id, OrderLocation{order.side, order.price, order_it});
        return true;
    }

    /// Cancels an active order in O(1) expected time and erases empty price levels.
    [[nodiscard]] std::optional<Order> cancel_order(OrderId id) {
        const auto lookup_it = order_lookup_.find(id);
        if (lookup_it == order_lookup_.end()) {
            return std::nullopt;
        }

        const OrderLocation loc = lookup_it->second;
        order_lookup_.erase(lookup_it);

        Order removed = (loc.side == Side::Buy) ? remove_from_map(bids_, loc.price, loc.iterator)
                                                : remove_from_map(asks_, loc.price, loc.iterator);
        removed.cancel();
        return removed;
    }

    /// Applies a trade execution against the best resting order on the specified side.
    /// Automatically removes filled orders from both PriceLevel and order_lookup_.
    void fill_best_order(Side side, Quantity exec_qty) {
        if (side == Side::Buy) {
            fill_best_impl(bids_, exec_qty);
        } else {
            fill_best_impl(asks_, exec_qty);
        }
    }

    /// Returns up to max_levels L2 depth entries ordered best-to-worst.
    [[nodiscard]] std::vector<DepthLevel> get_depth(Side side, std::size_t max_levels) const {
        return side == Side::Buy ? get_depth_impl(bids_, max_levels)
                                 : get_depth_impl(asks_, max_levels);
    }

private:
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    struct OrderLocation {
        Side side{Side::Buy};
        Price price{kInvalidPrice};
        PriceLevel::OrderIterator iterator{};
    };

    using OrderLookupMap = std::unordered_map<OrderId, OrderLocation>;

    template <typename BookMap>
    static Quantity quantity_at_price_impl(const BookMap& levels, Price price) noexcept {
        const auto it = levels.find(price);
        return (it == levels.end()) ? 0U : it->second.total_quantity();
    }

    template <typename BookMap>
    static const Order* best_order_impl(const BookMap& levels) noexcept {
        if (levels.empty()) {
            return nullptr;
        }
        return &levels.begin()->second.front();
    }

    template <typename BookMap>
    static PriceLevel::OrderIterator insert_into_map(BookMap& levels, const Order& order) {
        auto [level_it, inserted] = levels.try_emplace(order.price, order.price);
        static_cast<void>(inserted);
        return level_it->second.add_order(order);
    }

    template <typename BookMap>
    static Order remove_from_map(BookMap& levels, Price price, PriceLevel::OrderIterator order_it) {
        const auto level_it = levels.find(price);
        if (level_it == levels.end()) {
            throw std::logic_error("OrderBook price-level index invariant violated");
        }
        Order removed = level_it->second.remove_order(order_it);
        if (level_it->second.empty()) {
            levels.erase(level_it);
        }
        return removed;
    }

    template <typename BookMap>
    void fill_best_impl(BookMap& levels, Quantity exec_qty) {
        if (levels.empty()) {
            throw std::out_of_range("Cannot fill best order on empty side of OrderBook");
        }
        const auto level_it = levels.begin();
        PriceLevel& level = level_it->second;
        const OrderId best_id = level.front().id;

        level.fill_front(exec_qty);

        if (level.front().is_filled()) {
            static_cast<void>(level.pop_front());
            order_lookup_.erase(best_id);
            if (level.empty()) {
                levels.erase(level_it);
            }
        }
    }

    template <typename BookMap>
    static std::vector<DepthLevel> get_depth_impl(const BookMap& levels, std::size_t max_levels) {
        std::vector<DepthLevel> depth;
        const std::size_t count = std::min(levels.size(), max_levels);
        depth.reserve(count);

        for (const auto& [price, level] : levels) {
            if (depth.size() >= max_levels) {
                break;
            }
            depth.push_back(DepthLevel{price, level.total_quantity(), level.order_count()});
        }
        return depth;
    }

    InstrumentId instrument_id_{kInvalidInstrumentId};
    BidMap bids_{};
    AskMap asks_{};
    OrderLookupMap order_lookup_{};
};

}  // namespace martingale

#endif  // MARTINGALE_BOOK_ORDER_BOOK_HPP
