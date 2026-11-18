#ifndef MARTINGALE_CORE_TYPES_HPP
#define MARTINGALE_CORE_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace martingale {

/// Unique identifier assigned to each submitted order.
using OrderId = std::uint64_t;

/// Unique monotonically increasing identifier assigned to each executed trade.
using TradeId = std::uint64_t;

/// Compact identifier for a tradable financial instrument.
using InstrumentId = std::uint32_t;

/// Signed discrete integer price in minimum instrument ticks.
/// Signedness avoids unsigned wrap-around on spread calculations or price deltas.
using Price = std::int64_t;

/// Unsigned integer quantity of shares/contracts.
using Quantity = std::uint64_t;

/// Monotonic simulation or exchange timestamp in nanoseconds.
using Timestamp = std::uint64_t;

/// Deterministic global or per-instrument event sequence number.
using SequenceNumber = std::uint64_t;

/// Sentinel constants for uninitialized or special-purpose domain values.
inline constexpr OrderId kInvalidOrderId{0};
inline constexpr TradeId kInvalidTradeId{0};
inline constexpr InstrumentId kInvalidInstrumentId{0};
inline constexpr Price kInvalidPrice{std::numeric_limits<Price>::min()};
inline constexpr Price kMarketOrderPrice{0};
inline constexpr Quantity kInvalidQuantity{0};
inline constexpr std::size_t kCacheLineSize{64};

static_assert(sizeof(OrderId) == 8U && std::is_unsigned_v<OrderId>);
static_assert(sizeof(TradeId) == 8U && std::is_unsigned_v<TradeId>);
static_assert(sizeof(InstrumentId) == 4U && std::is_unsigned_v<InstrumentId>);
static_assert(sizeof(Price) == 8U && std::is_signed_v<Price>);
static_assert(sizeof(Quantity) == 8U && std::is_unsigned_v<Quantity>);
static_assert(sizeof(Timestamp) == 8U && std::is_unsigned_v<Timestamp>);
static_assert(sizeof(SequenceNumber) == 8U && std::is_unsigned_v<SequenceNumber>);

/// Order direction in the market.
enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

/// Order execution instruction type.
enum class OrderType : std::uint8_t {
    Limit = 0,
    Market = 1,
};

/// Lifecycle state of an order within the exchange.
enum class OrderStatus : std::uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4,
};

static_assert(sizeof(Side) == 1U);
static_assert(sizeof(OrderType) == 1U);
static_assert(sizeof(OrderStatus) == 1U);

/// Returns the opposite market side (Buy <-> Sell).
[[nodiscard]] constexpr Side opposite_side(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

/// Checks whether a Side value matches one of the defined enumerators.
[[nodiscard]] constexpr bool is_valid_side(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

/// Checks whether an OrderType value matches one of the defined enumerators.
[[nodiscard]] constexpr bool is_valid_order_type(OrderType type) noexcept {
    return type == OrderType::Limit || type == OrderType::Market;
}

/// Checks whether an order in the given status can still rest or execute on the book.
[[nodiscard]] constexpr bool is_active_status(OrderStatus status) noexcept {
    return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
}

/// Converts Side to a non-owning human-readable string view.
[[nodiscard]] constexpr std::string_view to_string_view(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "Buy";
        case Side::Sell:
            return "Sell";
    }
    return "Unknown";
}

/// Converts OrderType to a non-owning human-readable string view.
[[nodiscard]] constexpr std::string_view to_string_view(OrderType type) noexcept {
    switch (type) {
        case OrderType::Limit:
            return "Limit";
        case OrderType::Market:
            return "Market";
    }
    return "Unknown";
}

/// Converts OrderStatus to a non-owning human-readable string view.
[[nodiscard]] constexpr std::string_view to_string_view(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::New:
            return "New";
        case OrderStatus::PartiallyFilled:
            return "PartiallyFilled";
        case OrderStatus::Filled:
            return "Filled";
        case OrderStatus::Cancelled:
            return "Cancelled";
        case OrderStatus::Rejected:
            return "Rejected";
    }
    return "Unknown";
}

}  // namespace martingale

#endif  // MARTINGALE_CORE_TYPES_HPP
