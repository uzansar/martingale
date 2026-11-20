# ADR-004: Binary Sequential Event Journal (`MTGLJRN1`) and Deterministic Replay Engine

- **Status:** Accepted (Milestone 3 — Weeks 5–6)
- **Header:** [`include/martingale/journal/event_journal.hpp`](../include/martingale/journal/event_journal.hpp)
- **Related:** [`include/martingale/market_data/market_data_publisher.hpp`](../include/martingale/market_data/market_data_publisher.hpp)

---

## Context

In production electronic exchanges, post-trade auditing, crash recovery, and bug reproduction require the ability to replay every accepted order and cancellation command and recover the exact order book state and trade sequence bit-for-bit. Embedding JSON or text formatting inside the matching loop adds unacceptable allocation and latency overhead.

---

## Decision

1. **Sequential Binary Event Journal (`EventJournal`):**
   - Persist every validated input command (`NewOrder`, `CancelOrder`) and emitted exchange event (`OrderAccepted`, `OrderCancelled`, `TradeExecuted`) as fixed-size records serialized into an explicit little-endian binary container prefixed by an `8`-byte magic header (`MTGLJRN1`).
2. **Stateless Deterministic Replay (`ReplayEngine`):**
   - Deserializes the command stream cleanly into a fresh `OrderGateway` + `MatchingEngine` instance, re-executes commands in original sequence order, and asserts exact parity of emitted trades and resting L2 book state.
3. **Concept-Constrained Outbound Feeds (`OrderBookLike`):**
   - `MarketDataPublisher` reads BBO and L2 depth through the C++20 `OrderBookLike` concept so both the STL baseline `OrderBook` and the optimized `FlatOrderBook` emit identical market data without virtual dispatch overhead.

---

## Consequences

- **Benefits:** Guaranteed deterministic crash recovery and regression reproduction verified by GoogleTest binary round-trip tests (`JournalAndMarketDataTest`), plus zero virtual-call penalty for market data publication.
- **Trade-off:** Binary journal records are architecture/schema versioned; breaking field layout changes require bumping the `MTGLJRN1` format header.
