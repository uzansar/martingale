#ifndef MARTINGALE_BOOK_PRICE_LEVEL_HPP
#define MARTINGALE_BOOK_PRICE_LEVEL_HPP

#include <martingale/core/order.hpp>
#include <martingale/core/types.hpp>

#include <cstddef>
#include <iterator>
#include <list>
#include <stdexcept>

namespace martingale {

/// FIFO queue of active resting limit orders at a single discrete tick price.
/// Maintains strict price-time ordering and O(1) aggregate volume tracking.
class PriceLevel {
public:
    using OrderQueue = std::list<Order>;
    using OrderIterator = OrderQueue::iterator;
    using ConstOrderIterator = OrderQueue::const_iterator;

    explicit PriceLevel(Price level_price) noexcept : price_(level_price) {}

    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }
    [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }

    [[nodiscard]] const Order& front() const {
        if (orders_.empty()) {
            throw std::out_of_range("Cannot inspect front of an empty PriceLevel");
        }
        return orders_.front();
    }

    [[nodiscard]] ConstOrderIterator begin() const noexcept { return orders_.cbegin(); }
    [[nodiscard]] ConstOrderIterator end() const noexcept { return orders_.cend(); }

    /// Appends an active limit order to the back of the FIFO queue in O(1) time.
    /// Returns a stable list iterator used by OrderBook for O(1) cancellation.
    OrderIterator add_order(const Order& order) {
        if (order.price != price_) {
            throw std::invalid_argument("Order price does not match PriceLevel price");
        }
        if (!order.is_active() || order.remaining_quantity == 0U) {
            throw std::invalid_argument("Order must be active with non-zero remaining quantity");
        }
        orders_.push_back(order);
        total_quantity_ += order.remaining_quantity;
        return std::prev(orders_.end());
    }

    /// Executes an amount against the oldest (front) order in O(1) time.
    void fill_front(Quantity exec_qty) {
        if (orders_.empty()) {
            throw std::out_of_range("Cannot fill an empty PriceLevel");
        }
        orders_.front().fill(exec_qty);
        total_quantity_ -= exec_qty;
    }

    /// Removes an order at any position in O(1) time via its stable list iterator.
    Order remove_order(OrderIterator it) {
        Order removed = *it;
        total_quantity_ -= removed.remaining_quantity;
        orders_.erase(it);
        return removed;
    }

    /// Removes and returns the oldest (front) order in O(1) time.
    Order pop_front() {
        if (orders_.empty()) {
            throw std::out_of_range("Cannot pop front of an empty PriceLevel");
        }
        return remove_order(orders_.begin());
    }

private:
    Price price_{kInvalidPrice};
    Quantity total_quantity_{0};
    OrderQueue orders_{};
};

}  // namespace martingale

#endif  // MARTINGALE_BOOK_PRICE_LEVEL_HPP
