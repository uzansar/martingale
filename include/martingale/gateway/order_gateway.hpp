#ifndef MARTINGALE_GATEWAY_ORDER_GATEWAY_HPP
#define MARTINGALE_GATEWAY_ORDER_GATEWAY_HPP

#include <martingale/core/order.hpp>
#include <martingale/core/types.hpp>
#include <martingale/engine/matching_engine.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace martingale {

/// Structured reason reported when the OrderGateway rejects an incoming command.
enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidOrderId = 1,
    UnknownInstrument = 2,
    InvalidSide = 3,
    InvalidOrderType = 4,
    InvalidOrderState = 5,
    InvalidQuantity = 6,
    InvalidPrice = 7,
    DuplicateOrderId = 8,
    OrderNotFound = 9,
    InstrumentMismatch = 10,
};

/// Converts RejectReason to a non-owning human-readable string view.
[[nodiscard]] constexpr std::string_view to_string_view(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:
            return "None";
        case RejectReason::InvalidOrderId:
            return "InvalidOrderId";
        case RejectReason::UnknownInstrument:
            return "UnknownInstrument";
        case RejectReason::InvalidSide:
            return "InvalidSide";
        case RejectReason::InvalidOrderType:
            return "InvalidOrderType";
        case RejectReason::InvalidOrderState:
            return "InvalidOrderState";
        case RejectReason::InvalidQuantity:
            return "InvalidQuantity";
        case RejectReason::InvalidPrice:
            return "InvalidPrice";
        case RejectReason::DuplicateOrderId:
            return "DuplicateOrderId";
        case RejectReason::OrderNotFound:
            return "OrderNotFound";
        case RejectReason::InstrumentMismatch:
            return "InstrumentMismatch";
    }
    return "Unknown";
}

/// Result returned by OrderGateway::validate_and_submit().
struct GatewayOrderResult {
    bool accepted{false};
    RejectReason reject_reason{RejectReason::None};
    OrderResult engine_result{};
};

/// Result returned by OrderGateway::validate_and_cancel().
struct GatewayCancelResult {
    bool accepted{false};
    RejectReason reject_reason{RejectReason::None};
    std::optional<Order> cancelled_order{};
};

/// Exchange ingress boundary layer validating order fields, instrument registration,
/// and order identifiers before dispatching valid commands to MatchingEngine.
class OrderGateway {
public:
    explicit OrderGateway(MatchingEngine& engine) noexcept : engine_(engine) {}

    [[nodiscard]] std::uint64_t accepted_order_count() const noexcept {
        return accepted_order_count_;
    }

    [[nodiscard]] std::uint64_t rejected_order_count() const noexcept {
        return rejected_order_count_;
    }

    /// Validates an incoming order and forwards valid commands to MatchingEngine.
    /// Rejected orders never mutate matching engine state or advance sequence numbers.
    [[nodiscard]] GatewayOrderResult validate_and_submit(Order order) {
        const RejectReason reason = validate_order(order);
        if (reason != RejectReason::None) {
            ++rejected_order_count_;
            order.status = OrderStatus::Rejected;
            return GatewayOrderResult{false, reason, OrderResult{order, {}, 0U}};
        }

        ++accepted_order_count_;
        return GatewayOrderResult{true, RejectReason::None, engine_.submit_order(order)};
    }

    /// Validates a cancellation request against instrument registration and active ownership.
    [[nodiscard]] GatewayCancelResult validate_and_cancel(InstrumentId instrument_id,
                                                          OrderId order_id) {
        if (order_id == kInvalidOrderId) {
            return GatewayCancelResult{false, RejectReason::InvalidOrderId, std::nullopt};
        }
        if (!engine_.has_instrument(instrument_id)) {
            return GatewayCancelResult{false, RejectReason::UnknownInstrument, std::nullopt};
        }

        const auto active_inst = engine_.find_order_instrument(order_id);
        if (!active_inst.has_value()) {
            return GatewayCancelResult{false, RejectReason::OrderNotFound, std::nullopt};
        }
        if (*active_inst != instrument_id) {
            return GatewayCancelResult{false, RejectReason::InstrumentMismatch, std::nullopt};
        }

        return GatewayCancelResult{true, RejectReason::None,
                                   engine_.cancel_order(instrument_id, order_id)};
    }

private:
    [[nodiscard]] RejectReason validate_order(const Order& order) const noexcept {
        if (order.id == kInvalidOrderId) {
            return RejectReason::InvalidOrderId;
        }
        if (!engine_.has_instrument(order.instrument_id)) {
            return RejectReason::UnknownInstrument;
        }
        if (!is_valid_side(order.side)) {
            return RejectReason::InvalidSide;
        }
        if (!is_valid_order_type(order.type)) {
            return RejectReason::InvalidOrderType;
        }
        if (order.status != OrderStatus::New) {
            return RejectReason::InvalidOrderState;
        }
        if (order.initial_quantity == 0U || order.remaining_quantity != order.initial_quantity) {
            return RejectReason::InvalidQuantity;
        }
        if ((order.type == OrderType::Limit && order.price <= 0) ||
            (order.type == OrderType::Market && order.price < 0)) {
            return RejectReason::InvalidPrice;
        }
        if (engine_.has_active_order(order.id)) {
            return RejectReason::DuplicateOrderId;
        }
        return RejectReason::None;
    }

    MatchingEngine& engine_;
    std::uint64_t accepted_order_count_{0};
    std::uint64_t rejected_order_count_{0};
};

}  // namespace martingale

#endif  // MARTINGALE_GATEWAY_ORDER_GATEWAY_HPP
