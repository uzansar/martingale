#ifndef MARTINGALE_MARKET_DATA_MARKET_DATA_PUBLISHER_HPP
#define MARTINGALE_MARKET_DATA_MARKET_DATA_PUBLISHER_HPP

#include <martingale/book/order_book.hpp>
#include <martingale/core/order.hpp>
#include <martingale/core/trade.hpp>
#include <martingale/core/types.hpp>
#include <martingale/engine/matching_engine.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace martingale {

/// Types of outbound market data events generated off the matching critical path.
enum class MarketDataEventType : std::uint8_t {
    OrderAccepted = 0,
    OrderCancelled = 1,
    TradeExecuted = 2,
    BestBidOfferUpdate = 3,
    BookSnapshot = 4,
};

/// Level-1 Best Bid and Offer (BBO) quote state for a single instrument.
struct BboQuote {
    InstrumentId instrument_id{kInvalidInstrumentId};
    std::optional<Price> best_bid{};
    Quantity best_bid_quantity{0};
    std::optional<Price> best_ask{};
    Quantity best_ask_quantity{0};

    [[nodiscard]] constexpr bool operator==(const BboQuote&) const noexcept = default;
};

/// Level-2 aggregated order book snapshot for a single instrument.
struct OrderBookSnapshot {
    InstrumentId instrument_id{kInvalidInstrumentId};
    Timestamp timestamp{0};
    SequenceNumber md_sequence{0};
    std::vector<DepthLevel> bids{};
    std::vector<DepthLevel> asks{};

    [[nodiscard]] bool operator==(const OrderBookSnapshot&) const noexcept = default;
};

/// Outbound market data message emitted by MarketDataPublisher.
struct MarketDataEvent {
    SequenceNumber md_sequence{0};
    Timestamp timestamp{0};
    InstrumentId instrument_id{kInvalidInstrumentId};
    MarketDataEventType type{MarketDataEventType::OrderAccepted};
    Order order{};
    Trade trade{};
    BboQuote bbo{};
};

/// Decoupled publisher translating matching outcomes and book state into
/// sequenced L1/L2 market data events and snapshots.
class MarketDataPublisher {
public:
    using EventCallback = std::function<void(const MarketDataEvent&)>;

    MarketDataPublisher() = default;
    explicit MarketDataPublisher(EventCallback callback) : callback_(std::move(callback)) {}

    /// Publishes OrderAccepted, any resulting TradeExecuted prints, and an L1 BBO
    /// delta update if the top-of-book changed for the instrument.
    void on_order_processed(const OrderResult& result, const OrderBook& book) {
        if (result.final_order.status == OrderStatus::Rejected) {
            return;
        }

        const Timestamp ts = result.final_order.timestamp;
        const InstrumentId inst = book.instrument_id();

        emit_event(MarketDataEvent{0U, ts, inst, MarketDataEventType::OrderAccepted,
                                   result.final_order, Trade{}, BboQuote{}});

        for (const auto& trade : result.trades) {
            emit_event(MarketDataEvent{0U, trade.timestamp, inst,
                                       MarketDataEventType::TradeExecuted, Order{}, trade,
                                       BboQuote{}});
        }

        maybe_emit_bbo_update(book, ts);
    }

    /// Publishes OrderCancelled and emits a BestBidOfferUpdate if top-of-book changed.
    void on_order_cancelled(const Order& cancelled_order, const OrderBook& book, Timestamp ts) {
        const InstrumentId inst = book.instrument_id();
        emit_event(MarketDataEvent{0U, ts, inst, MarketDataEventType::OrderCancelled,
                                   cancelled_order, Trade{}, BboQuote{}});
        maybe_emit_bbo_update(book, ts);
    }

    /// Captures an L2 depth snapshot up to max_levels and emits a BookSnapshot event.
    [[nodiscard]] OrderBookSnapshot capture_snapshot(const OrderBook& book, std::size_t max_levels,
                                                     Timestamp ts) {
        const InstrumentId inst = book.instrument_id();
        emit_event(MarketDataEvent{0U, ts, inst, MarketDataEventType::BookSnapshot, Order{},
                                   Trade{}, BboQuote{}});
        return OrderBookSnapshot{
            .instrument_id = inst,
            .timestamp = ts,
            .md_sequence = events_.back().md_sequence,
            .bids = book.get_depth(Side::Buy, max_levels),
            .asks = book.get_depth(Side::Sell, max_levels),
        };
    }

    [[nodiscard]] const std::vector<MarketDataEvent>& events() const noexcept { return events_; }

    [[nodiscard]] std::optional<BboQuote> last_bbo(InstrumentId instrument_id) const noexcept {
        const auto it = last_bbo_.find(instrument_id);
        if (it == last_bbo_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    void emit_event(MarketDataEvent&& event) {
        event.md_sequence = next_sequence_++;
        events_.push_back(std::move(event));
        if (callback_) {
            callback_(events_.back());
        }
    }

    void maybe_emit_bbo_update(const OrderBook& book, Timestamp ts) {
        const InstrumentId inst = book.instrument_id();
        const BboQuote current{
            .instrument_id = inst,
            .best_bid = book.best_bid(),
            .best_bid_quantity = book.best_bid_quantity(),
            .best_ask = book.best_ask(),
            .best_ask_quantity = book.best_ask_quantity(),
        };

        const auto it = last_bbo_.find(inst);
        if (it == last_bbo_.end() || it->second != current) {
            last_bbo_.insert_or_assign(inst, current);
            emit_event(MarketDataEvent{0U, ts, inst, MarketDataEventType::BestBidOfferUpdate,
                                       Order{}, Trade{}, current});
        }
    }

    EventCallback callback_{};
    std::vector<MarketDataEvent> events_{};
    std::unordered_map<InstrumentId, BboQuote> last_bbo_{};
    SequenceNumber next_sequence_{1};
};

}  // namespace martingale

#endif  // MARTINGALE_MARKET_DATA_MARKET_DATA_PUBLISHER_HPP
