#ifndef MARTINGALE_JOURNAL_EVENT_JOURNAL_HPP
#define MARTINGALE_JOURNAL_EVENT_JOURNAL_HPP

#include <martingale/core/order.hpp>
#include <martingale/core/trade.hpp>
#include <martingale/core/types.hpp>
#include <martingale/engine/matching_engine.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace martingale {

/// Discriminator for entries stored in the sequential EventJournal.
enum class JournalEventType : std::uint8_t {
    RegisterInstrument = 0,
    SubmitOrder = 1,
    CancelOrder = 2,
    OrderAccepted = 3,
    OrderCancelled = 4,
    TradeExecuted = 5,
};

/// Fixed-layout, trivially copyable record stored sequentially in EventJournal.
struct alignas(kCacheLineSize) JournalRecord {
    SequenceNumber journal_sequence{0};
    SequenceNumber engine_sequence{0};
    Timestamp timestamp{0};
    InstrumentId instrument_id{kInvalidInstrumentId};
    JournalEventType type{JournalEventType::SubmitOrder};
    Order order{};
    Trade trade{};

    [[nodiscard]] constexpr bool operator==(const JournalRecord&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<JournalRecord>);

/// Append-only sequential command and event journal supporting binary stream
/// serialization and deterministic replay verification.
class EventJournal {
public:
    void record_register_instrument(InstrumentId instrument_id, Timestamp ts = 0U) {
        JournalRecord rec{};
        rec.journal_sequence = next_journal_sequence_++;
        rec.timestamp = ts;
        rec.instrument_id = instrument_id;
        rec.type = JournalEventType::RegisterInstrument;
        records_.push_back(rec);
    }

    void record_submit_order(const Order& order) {
        JournalRecord rec{};
        rec.journal_sequence = next_journal_sequence_++;
        rec.timestamp = order.timestamp;
        rec.instrument_id = order.instrument_id;
        rec.type = JournalEventType::SubmitOrder;
        rec.order = order;
        records_.push_back(rec);
    }

    void record_cancel_command(InstrumentId instrument_id, OrderId order_id, Timestamp ts = 0U) {
        JournalRecord rec{};
        rec.journal_sequence = next_journal_sequence_++;
        rec.timestamp = ts;
        rec.instrument_id = instrument_id;
        rec.type = JournalEventType::CancelOrder;
        rec.order.id = order_id;
        rec.order.instrument_id = instrument_id;
        records_.push_back(rec);
    }

    void record_order_accepted(const Order& final_order, SequenceNumber engine_seq) {
        JournalRecord rec{};
        rec.journal_sequence = next_journal_sequence_++;
        rec.engine_sequence = engine_seq;
        rec.timestamp = final_order.timestamp;
        rec.instrument_id = final_order.instrument_id;
        rec.type = JournalEventType::OrderAccepted;
        rec.order = final_order;
        records_.push_back(rec);
    }

    void record_order_cancelled(const Order& cancelled_order, SequenceNumber engine_seq,
                                Timestamp ts = 0U) {
        JournalRecord rec{};
        rec.journal_sequence = next_journal_sequence_++;
        rec.engine_sequence = engine_seq;
        rec.timestamp = ts;
        rec.instrument_id = cancelled_order.instrument_id;
        rec.type = JournalEventType::OrderCancelled;
        rec.order = cancelled_order;
        records_.push_back(rec);
    }

    void record_trade(const Trade& trade) {
        JournalRecord rec{};
        rec.journal_sequence = next_journal_sequence_++;
        rec.engine_sequence = trade.sequence_number;
        rec.timestamp = trade.timestamp;
        rec.instrument_id = trade.instrument_id;
        rec.type = JournalEventType::TradeExecuted;
        rec.trade = trade;
        records_.push_back(rec);
    }

    /// Convenience helper recording an incoming order command plus resulting accepted/trade events.
    void record_submission(const Order& raw_order, const OrderResult& result) {
        record_submit_order(raw_order);
        if (result.final_order.status != OrderStatus::Rejected) {
            record_order_accepted(result.final_order, result.sequence_number);
            for (const auto& trade : result.trades) {
                record_trade(trade);
            }
        }
    }

    /// Convenience helper recording a cancel command plus optional OrderCancelled outcome.
    void record_cancellation(InstrumentId instrument_id, OrderId order_id,
                             const std::optional<Order>& cancelled, SequenceNumber engine_seq,
                             Timestamp ts = 0U) {
        record_cancel_command(instrument_id, order_id, ts);
        if (cancelled.has_value()) {
            record_order_cancelled(*cancelled, engine_seq, ts);
        }
    }

    void save_to_stream(std::ostream& out) const {
        out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
        const std::uint64_t count = records_.size();
        out.write(reinterpret_cast<const char*>(&count),
                  static_cast<std::streamsize>(sizeof(count)));
        if (count > 0U) {
            const auto byte_count =
                static_cast<std::streamsize>(count * sizeof(JournalRecord));
            out.write(reinterpret_cast<const char*>(records_.data()), byte_count);
        }
        if (!out) {
            throw std::runtime_error("Failed writing EventJournal to binary stream");
        }
    }

    [[nodiscard]] static EventJournal load_from_stream(std::istream& in) {
        std::array<char, 8> magic{};
        in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!in || magic != kMagic) {
            throw std::runtime_error("Invalid EventJournal magic header");
        }

        std::uint64_t count{0};
        in.read(reinterpret_cast<char*>(&count), static_cast<std::streamsize>(sizeof(count)));
        if (!in) {
            throw std::runtime_error("Failed reading EventJournal record count");
        }

        EventJournal journal;
        if (count > 0U) {
            journal.records_.resize(static_cast<std::size_t>(count));
            const auto byte_count =
                static_cast<std::streamsize>(count * sizeof(JournalRecord));
            in.read(reinterpret_cast<char*>(journal.records_.data()), byte_count);
            if (!in) {
                throw std::runtime_error("Truncated EventJournal binary stream");
            }
            journal.next_journal_sequence_ = journal.records_.back().journal_sequence + 1U;
        }
        return journal;
    }

    [[nodiscard]] const std::vector<JournalRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

    void clear() noexcept {
        records_.clear();
        next_journal_sequence_ = 1U;
    }

private:
    static constexpr std::array<char, 8> kMagic{'M', 'T', 'G', 'L', 'J', 'R', 'N', '1'};

    std::vector<JournalRecord> records_{};
    SequenceNumber next_journal_sequence_{1};
};

/// Summary returned by ReplayEngine::replay_into().
struct ReplayVerificationResult {
    std::size_t commands_replayed{0};
    std::size_t trades_replayed{0};
    bool matches_recorded_events{true};
};

/// Reconstructs exchange state deterministically from an EventJournal and verifies
/// that replayed outputs match all recorded lifecycle and trade events.
class ReplayEngine {
public:
    [[nodiscard]] static ReplayVerificationResult replay_into(const EventJournal& journal,
                                                              MatchingEngine& target_engine) {
        ReplayVerificationResult verification;
        std::optional<OrderResult> last_order_result;
        std::size_t trade_cursor{0};
        std::optional<Order> last_cancel_result;
        SequenceNumber last_cancel_seq{0};

        for (const auto& rec : journal.records()) {
            switch (rec.type) {
                case JournalEventType::RegisterInstrument:
                    if (!target_engine.has_instrument(rec.instrument_id)) {
                        static_cast<void>(target_engine.register_instrument(rec.instrument_id));
                    }
                    ++verification.commands_replayed;
                    last_order_result.reset();
                    last_cancel_result.reset();
                    break;

                case JournalEventType::SubmitOrder:
                    last_order_result = target_engine.submit_order(rec.order);
                    trade_cursor = 0U;
                    last_cancel_result.reset();
                    ++verification.commands_replayed;
                    break;

                case JournalEventType::CancelOrder:
                    last_cancel_seq = target_engine.next_sequence_number();
                    last_cancel_result =
                        target_engine.cancel_order(rec.instrument_id, rec.order.id);
                    last_order_result.reset();
                    ++verification.commands_replayed;
                    break;

                case JournalEventType::OrderAccepted:
                    if (!last_order_result.has_value() ||
                        last_order_result->final_order != rec.order ||
                        last_order_result->sequence_number != rec.engine_sequence) {
                        verification.matches_recorded_events = false;
                    }
                    break;

                case JournalEventType::TradeExecuted:
                    if (!last_order_result.has_value() ||
                        trade_cursor >= last_order_result->trades.size() ||
                        last_order_result->trades[trade_cursor] != rec.trade) {
                        verification.matches_recorded_events = false;
                    } else {
                        ++trade_cursor;
                        ++verification.trades_replayed;
                    }
                    break;

                case JournalEventType::OrderCancelled:
                    if (!last_cancel_result.has_value() ||
                        *last_cancel_result != rec.order ||
                        last_cancel_seq != rec.engine_sequence) {
                        verification.matches_recorded_events = false;
                    }
                    break;
            }
        }
        return verification;
    }
};

}  // namespace martingale

#endif  // MARTINGALE_JOURNAL_EVENT_JOURNAL_HPP
