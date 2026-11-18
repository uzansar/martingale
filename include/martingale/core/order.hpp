#ifndef MARTINGALE_CORE_ORDER_HPP
#define MARTINGALE_CORE_ORDER_HPP

#include <martingale/core/types.hpp>

#include <stdexcept>
#include <type_traits>

namespace martingale {

/// Represents a single exchange order with strict lifecycle invariants.
/// Fields are ordered from largest alignment (8B) to smallest (1B) so the struct
/// occupies 48 bytes—leaving 16 bytes for std::list / intrusive queue pointers
/// to fit cleanly inside a single 64-byte x86_64 cache line.
struct Order {
    OrderId id{kInvalidOrderId};
    Price price{kInvalidPrice};
    Quantity initial_quantity{0};
    Quantity remaining_quantity{0};
    Timestamp timestamp{0};
    InstrumentId instrument_id{kInvalidInstrumentId};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    OrderStatus status{OrderStatus::New};

    constexpr Order() noexcept = default;

    constexpr Order(OrderId order_id, InstrumentId inst_id, Side order_side, OrderType order_type,
                    Price order_price, Quantity quantity, Timestamp ts) noexcept
        : id(order_id),
          price(order_price),
          initial_quantity(quantity),
          remaining_quantity(quantity),
          timestamp(ts),
          instrument_id(inst_id),
          side(order_side),
          type(order_type),
          status(OrderStatus::New) {}

    [[nodiscard]] constexpr Quantity filled_quantity() const noexcept {
        return initial_quantity - remaining_quantity;
    }

    [[nodiscard]] constexpr bool is_filled() const noexcept {
        return status == OrderStatus::Filled;
    }

    [[nodiscard]] constexpr bool is_active() const noexcept { return is_active_status(status); }

    /// Applies an execution fill against this order, updating remaining quantity and status.
    constexpr void fill(Quantity exec_qty) {
        if (!is_active()) {
            throw std::logic_error("Cannot fill an inactive order");
        }
        if (exec_qty == 0U || exec_qty > remaining_quantity) {
            throw std::invalid_argument("Invalid execution quantity");
        }
        remaining_quantity -= exec_qty;
        status = (remaining_quantity == 0U) ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
    }

    /// Cancels an active order, preserving its filled_quantity() history.
    constexpr void cancel() {
        if (!is_active()) {
            throw std::logic_error("Cannot cancel an inactive order");
        }
        status = OrderStatus::Cancelled;
    }

    /// Marks a new order as rejected without modifying filled/remaining quantities.
    constexpr void reject() {
        if (status != OrderStatus::New) {
            throw std::logic_error("Only newly created orders can be rejected");
        }
        status = OrderStatus::Rejected;
    }

    [[nodiscard]] constexpr bool operator==(const Order&) const noexcept = default;
};

static_assert(sizeof(Order) == 48U);
static_assert(sizeof(Order) <= kCacheLineSize);
static_assert(alignof(Order) == 8U);
static_assert(std::is_trivially_copyable_v<Order>);

}  // namespace martingale

#endif  // MARTINGALE_CORE_ORDER_HPP
