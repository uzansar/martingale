#include <gtest/gtest.h>
#include <martingale/engine/matching_engine.hpp>
#include <martingale/journal/event_journal.hpp>
#include <martingale/market_data/market_data_publisher.hpp>

#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace martingale::tests {

TEST(JournalAndMarketDataTest, EventJournalRecordsCommandsTradesAndLifecycleEvents) {
    static_assert(std::is_trivially_copyable_v<JournalRecord>);

    EventJournal journal;
    EXPECT_TRUE(journal.empty());
    EXPECT_EQ(journal.size(), 0U);

    MatchingEngine engine(1U);
    ASSERT_TRUE(engine.register_instrument(2U));
    journal.record_register_instrument(2U, 100U);

    const Order o1(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 200U);
    const auto r1 = engine.submit_order(o1);
    journal.record_submission(o1, r1);

    const Order o2(2U, 1U, Side::Buy, OrderType::Limit, 10050, 40U, 300U);
    const auto r2 = engine.submit_order(o2);
    journal.record_submission(o2, r2);

    const auto c1 = engine.cancel_order(1U, 1U);
    journal.record_cancellation(1U, 1U, c1, engine.next_sequence_number() - 1U, 400U);

    ASSERT_EQ(journal.size(), 8U);
    EXPECT_EQ(journal.records()[0].type, JournalEventType::RegisterInstrument);
    EXPECT_EQ(journal.records()[1].type, JournalEventType::SubmitOrder);
    EXPECT_EQ(journal.records()[2].type, JournalEventType::OrderAccepted);
    EXPECT_EQ(journal.records()[3].type, JournalEventType::SubmitOrder);
    EXPECT_EQ(journal.records()[4].type, JournalEventType::OrderAccepted);
    EXPECT_EQ(journal.records()[5].type, JournalEventType::TradeExecuted);
    EXPECT_EQ(journal.records()[6].type, JournalEventType::CancelOrder);
    EXPECT_EQ(journal.records()[7].type, JournalEventType::OrderCancelled);

    for (std::size_t i = 0; i < journal.size(); ++i) {
        EXPECT_EQ(journal.records()[i].journal_sequence, i + 1U);
    }
}

TEST(JournalAndMarketDataTest, EventJournalBinarySerializationRoundTrip) {
    EventJournal original;
    original.record_register_instrument(2U, 50U);

    const Order o1(10U, 1U, Side::Buy, OrderType::Limit, 9950, 75U, 100U);
    MatchingEngine engine(1U);
    const auto res = engine.submit_order(o1);
    original.record_submission(o1, res);

    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    original.save_to_stream(ss);

    ss.seekg(0);
    const EventJournal loaded = EventJournal::load_from_stream(ss);
    ASSERT_EQ(loaded.size(), original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_EQ(loaded.records()[i], original.records()[i]);
    }

    std::stringstream bad_stream("BADMAGIC12345678", std::ios::in | std::ios::binary);
    EXPECT_THROW(static_cast<void>(EventJournal::load_from_stream(bad_stream)), std::runtime_error);
}

TEST(JournalAndMarketDataTest, DeterministicStateReconstructionViaReplayEngine) {
    MatchingEngine live_engine(1U);
    EventJournal journal;

    ASSERT_TRUE(live_engine.register_instrument(2U));
    journal.record_register_instrument(2U, 10U);

    const std::vector<Order> orders = {
        Order(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 100U),
        Order(2U, 1U, Side::Sell, OrderType::Limit, 10060, 150U, 200U),
        Order(3U, 1U, Side::Buy, OrderType::Limit, 10055, 120U, 300U),
        Order(4U, 2U, Side::Buy, OrderType::Limit, 5000, 80U, 400U),
        Order(5U, 2U, Side::Sell, OrderType::Market, kMarketOrderPrice, 50U, 500U),
        Order(6U, 1U, Side::Buy, OrderType::Market, kMarketOrderPrice, 50U, 600U),
    };

    for (const auto& order : orders) {
        const auto res = live_engine.submit_order(order);
        journal.record_submission(order, res);
    }

    const auto cancel_res = live_engine.cancel_order(2U, 4U);
    journal.record_cancellation(2U, 4U, cancel_res, live_engine.next_sequence_number() - 1U, 700U);

    // Serialize + reload before replaying to verify full disk/stream reconstruction
    std::stringstream wire(std::ios::in | std::ios::out | std::ios::binary);
    journal.save_to_stream(wire);
    wire.seekg(0);
    const EventJournal reloaded = EventJournal::load_from_stream(wire);

    MatchingEngine replayed_engine(1U);
    const ReplayVerificationResult replay_status = ReplayEngine::replay_into(reloaded, replayed_engine);

    EXPECT_TRUE(replay_status.matches_recorded_events);
    EXPECT_EQ(replay_status.commands_replayed, 8U);
    EXPECT_EQ(replay_status.trades_replayed, live_engine.trades().size());
    EXPECT_EQ(replayed_engine.trades(), live_engine.trades());
    EXPECT_EQ(replayed_engine.next_sequence_number(), live_engine.next_sequence_number());
    EXPECT_EQ(replayed_engine.next_trade_id(), live_engine.next_trade_id());
    EXPECT_EQ(replayed_engine.book(1U).get_depth(Side::Buy, 100U),
              live_engine.book(1U).get_depth(Side::Buy, 100U));
    EXPECT_EQ(replayed_engine.book(1U).get_depth(Side::Sell, 100U),
              live_engine.book(1U).get_depth(Side::Sell, 100U));
    EXPECT_EQ(replayed_engine.book(2U).get_depth(Side::Buy, 100U),
              live_engine.book(2U).get_depth(Side::Buy, 100U));
    EXPECT_EQ(replayed_engine.book(2U).get_depth(Side::Sell, 100U),
              live_engine.book(2U).get_depth(Side::Sell, 100U));
}

TEST(JournalAndMarketDataTest, MarketDataPublisherEmitsOrderTradeAndBboDeltaEvents) {
    MatchingEngine engine(1U);
    MarketDataPublisher publisher;

    // 1. Best ask set at 10050 -> emits OrderAccepted + BestBidOfferUpdate
    const Order o1(1U, 1U, Side::Sell, OrderType::Limit, 10050, 100U, 10U);
    publisher.on_order_processed(engine.submit_order(o1), engine.book(1U));

    ASSERT_EQ(publisher.events().size(), 2U);
    EXPECT_EQ(publisher.events()[0].type, MarketDataEventType::OrderAccepted);
    EXPECT_EQ(publisher.events()[1].type, MarketDataEventType::BestBidOfferUpdate);
    EXPECT_EQ(publisher.events()[1].bbo.best_ask, 10050);
    EXPECT_EQ(publisher.events()[1].bbo.best_ask_quantity, 100U);

    // 2. Deeper ask at 10075 does NOT change BBO -> emits OrderAccepted only
    const Order o2(2U, 1U, Side::Sell, OrderType::Limit, 10075, 50U, 20U);
    publisher.on_order_processed(engine.submit_order(o2), engine.book(1U));

    ASSERT_EQ(publisher.events().size(), 3U);
    EXPECT_EQ(publisher.events()[2].type, MarketDataEventType::OrderAccepted);

    // 3. Aggressive buy fills o1 completely -> emits OrderAccepted + TradeExecuted + BboUpdate (10075)
    const Order o3(3U, 1U, Side::Buy, OrderType::Limit, 10050, 100U, 30U);
    publisher.on_order_processed(engine.submit_order(o3), engine.book(1U));

    ASSERT_EQ(publisher.events().size(), 6U);
    EXPECT_EQ(publisher.events()[3].type, MarketDataEventType::OrderAccepted);
    EXPECT_EQ(publisher.events()[4].type, MarketDataEventType::TradeExecuted);
    EXPECT_EQ(publisher.events()[4].trade.price, 10050);
    EXPECT_EQ(publisher.events()[5].type, MarketDataEventType::BestBidOfferUpdate);
    EXPECT_EQ(publisher.events()[5].bbo.best_ask, 10075);
    EXPECT_EQ(publisher.events()[5].bbo.best_ask_quantity, 50U);

    ASSERT_TRUE(publisher.last_bbo(1U).has_value());
    EXPECT_EQ(publisher.last_bbo(1U)->best_ask, 10075);
}

TEST(JournalAndMarketDataTest, MarketDataPublisherCancellationUpdatesBbo) {
    MatchingEngine engine(1U);
    MarketDataPublisher publisher;

    const Order o1(1U, 1U, Side::Buy, OrderType::Limit, 10000, 100U, 10U);
    const Order o2(2U, 1U, Side::Buy, OrderType::Limit, 9990, 200U, 20U);
    publisher.on_order_processed(engine.submit_order(o1), engine.book(1U));
    publisher.on_order_processed(engine.submit_order(o2), engine.book(1U));
    const std::size_t count_before = publisher.events().size();

    // Cancelling deeper level o2 emits OrderCancelled without BestBidOfferUpdate
    const auto c2 = engine.cancel_order(1U, 2U);
    ASSERT_TRUE(c2.has_value());
    publisher.on_order_cancelled(*c2, engine.book(1U), 30U);
    ASSERT_EQ(publisher.events().size(), count_before + 1U);
    EXPECT_EQ(publisher.events().back().type, MarketDataEventType::OrderCancelled);

    // Cancelling best bid o1 emits OrderCancelled + BestBidOfferUpdate (empty bid)
    const auto c1 = engine.cancel_order(1U, 1U);
    ASSERT_TRUE(c1.has_value());
    publisher.on_order_cancelled(*c1, engine.book(1U), 40U);
    ASSERT_EQ(publisher.events().size(), count_before + 3U);
    EXPECT_EQ(publisher.events()[count_before + 1U].type, MarketDataEventType::OrderCancelled);
    EXPECT_EQ(publisher.events()[count_before + 2U].type, MarketDataEventType::BestBidOfferUpdate);
    EXPECT_EQ(publisher.events()[count_before + 2U].bbo.best_bid, std::nullopt);
}

TEST(JournalAndMarketDataTest, MarketDataPublisherGeneratesConfigurableL2Snapshots) {
    MatchingEngine engine(1U);
    std::vector<MarketDataEvent> callback_log;
    MarketDataPublisher publisher(
        [&callback_log](const MarketDataEvent& event) { callback_log.push_back(event); });

    static_cast<void>(engine.submit_order(Order(1U, 1U, Side::Buy, OrderType::Limit, 10000, 10U, 1U)));
    static_cast<void>(engine.submit_order(Order(2U, 1U, Side::Buy, OrderType::Limit, 9990, 20U, 2U)));
    static_cast<void>(engine.submit_order(Order(3U, 1U, Side::Buy, OrderType::Limit, 9980, 30U, 3U)));
    static_cast<void>(engine.submit_order(Order(4U, 1U, Side::Sell, OrderType::Limit, 10010, 15U, 4U)));
    static_cast<void>(engine.submit_order(Order(5U, 1U, Side::Sell, OrderType::Limit, 10020, 25U, 5U)));

    const OrderBookSnapshot snap = publisher.capture_snapshot(engine.book(1U), 2U, 9999U);

    EXPECT_EQ(snap.instrument_id, 1U);
    EXPECT_EQ(snap.timestamp, 9999U);
    EXPECT_EQ(snap.bids.size(), 2U);
    EXPECT_EQ(snap.asks.size(), 2U);
    EXPECT_EQ(snap.bids[0], (DepthLevel{10000, 10U, 1U}));
    EXPECT_EQ(snap.bids[1], (DepthLevel{9990, 20U, 1U}));
    EXPECT_EQ(snap.asks[0], (DepthLevel{10010, 15U, 1U}));
    EXPECT_EQ(snap.asks[1], (DepthLevel{10020, 25U, 1U}));

    ASSERT_EQ(callback_log.size(), 1U);
    EXPECT_EQ(callback_log[0].type, MarketDataEventType::BookSnapshot);
    EXPECT_EQ(callback_log[0].md_sequence, snap.md_sequence);
}

}  // namespace martingale::tests
