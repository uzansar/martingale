#ifndef MARTINGALE_CORE_TRADE_HPP
#define MARTINGALE_CORE_TRADE_HPP

#include <martingale/core/types.hpp>

#include <type_traits>

namespace martingale {

/// Immutable execution record emitted whenever an incoming order matches a resting order.
/// Aligned to a single 64-byte cache line to avoid cache-line splits in ring buffers
/// and sequential event journals.
struct alignas(kCacheLineSize) Trade {
    TradeId trade_id{kInvalidTradeId};
    Price price{kInvalidPrice};
    Quantity quantity{0};
    OrderId buyer_order_id{kInvalidOrderId};
    OrderId seller_order_id{kInvalidOrderId};
    SequenceNumber sequence_number{0};
    Timestamp timestamp{0};
    InstrumentId instrument_id{kInvalidInstrumentId};
    Side aggressor_side{Side::Buy};

    [[nodiscard]] constexpr bool operator==(const Trade&) const noexcept = default;
};

static_assert(sizeof(Trade) == kCacheLineSize);
static_assert(alignof(Trade) == kCacheLineSize);
static_assert(std::is_trivially_copyable_v<Trade>);

}  // namespace martingale

#endif  // MARTINGALE_CORE_TRADE_HPP
