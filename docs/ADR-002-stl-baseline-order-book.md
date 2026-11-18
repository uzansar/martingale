# ADR-002: STL Baseline Order Book Architecture

## Status
Accepted (Milestone 2 — Weeks 3–4)

## Context
A Limit Order Book (LOB) must support four core operations accurately and deterministically:
1. Insert a resting limit order at its discrete tick `Price` in strict FIFO arrival order.
2. Inspect and match against the Best Bid / Best Ask (`best_bid()`, `best_ask()`) in $O(1)$ time.
3. Cancel an arbitrary resting order by `OrderId` without linearly scanning price levels.
4. Expose ordered L2 depth snapshots across top price levels.

## Decision
For the initial Milestone 2 reference implementation, `OrderBook` uses standard STL containers:
- **Price Levels (`BidMap` / `AskMap`):** `std::map<Price, PriceLevel, std::greater<Price>>` for Bids and `std::map<Price, PriceLevel, std::less<Price>>` for Asks, so `levels.begin()` always resolves to the best price in $O(1)$ amortized time.
- **FIFO Queue (`PriceLevel`):** `std::list<Order>` storing orders by arrival time with an incremental `total_quantity_` counter.
- **Order Lookup Registry:** `std::unordered_map<OrderId, OrderLocation>` storing `{side, price, std::list<Order>::iterator}` for $O(1)$ expected-time cancellation and filled-order cleanup, relying on `std::list`'s iterator-stability guarantee across insertions and unrelated erasures.
- **Eager Level Cleanup:** Whenever the last order at a `PriceLevel` is cancelled or fully filled, the empty `PriceLevel` node is erased from the `std::map` immediately so `levels.begin()` always represents a non-empty best price.

## Consequences
- **Benefits:** Clean, memory-safe, easy-to-audit baseline establishing exact deterministic semantics for Milestone 2 & 3 tests, and acting as the ground-truth oracle for Milestone 4 optimizations.
- **Trade-offs (Measured Later in Milestone 4):** Each `std::list<Order>` insertion and `std::map` node allocation incurs a dynamic heap allocation and non-contiguous pointer chasing, which we will quantify using Google Benchmark, `perf`, and Heaptrack before introducing pre-allocated object pools and intrusive/flat structures.
