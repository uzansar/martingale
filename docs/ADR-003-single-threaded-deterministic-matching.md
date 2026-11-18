# ADR-003: Single-Threaded Deterministic Matching Core

## Status
Accepted (Milestone 2 — Weeks 3–4)

## Context
An electronic exchange matching engine must guarantee **strict determinism**: given two independent instances fed the exact same ordered sequence of incoming commands (`NewOrder`, `CancelOrder`), every resulting trade, sequence number, and final L2 order book state must be bit-for-bit identical.

## Decision
1. **Single-Threaded Critical Path:** The matching engine executes sequentially on a single thread without mutexes, condition variables, or non-deterministic thread scheduling on the hot path. Multi-instrument parallelism (sharding independent instruments onto dedicated threads) will be explored only after single-threaded baselines are measured in Milestone 4.
2. **Passive-Price Execution Rule:** When an incoming aggressive order crosses a resting limit order, execution always occurs at the **resting order's price** (`resting->price`), guaranteeing price improvement for the aggressor.
3. **Market Order Semantics:** Market orders sweep available opposite-side liquidity in strict price-time order; any unfilled remainder cancels immediately (`OrderStatus::Cancelled`) rather than resting on the book.
4. **Monotonic Deterministic Sequencing:** Every accepted command and emitted trade advances deterministic `SequenceNumber` and `TradeId` counters without relying on wall-clock time.

## Consequences
- **Benefits:** Zero lock contention, zero tail-latency jitter from OS mutexes, trivial deterministic replay for event journaling (Milestone 3), and bit-for-bit reproducible simulations (Milestone 5).
- **Trade-offs:** Throughput for a single instrument is bounded by single-core IPC and cache efficiency, which directly motivates Milestone 4's memory-pool and intrusive-queue optimization loop.
