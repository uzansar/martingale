# ADR-005: Pre-Allocated `ObjectPool` and Intrusive `FlatOrderBook` Optimization

- **Status:** Accepted (Milestone 4 — Weeks 7–8)
- **Headers:** [`include/martingale/performance/object_pool.hpp`](../include/martingale/performance/object_pool.hpp), [`include/martingale/book/flat_order_book.hpp`](../include/martingale/book/flat_order_book.hpp), [`include/martingale/engine/matching_engine.hpp`](../include/martingale/engine/matching_engine.hpp)
- **Evidence:** [`benchmark_baseline.md`](benchmark_baseline.md), [`benchmark_comparison.md`](benchmark_comparison.md)

---

## Context

Milestone 4 profiling (Heaptrack and Valgrind Callgrind) of the STL baseline `OrderBook` (`std::map`, `std::list`, `std::unordered_map`) revealed `836,635` heap allocation calls during `MixedWorkload_10k` alongside `323,235` branch mispredictions (`8.3%` overall, `11.7%` indirect branches) driven by pointer chasing across red-black tree nodes and doubly-linked list nodes.

---

## Decision

1. **Contiguous Intrusive Slab Allocation (`ObjectPool<IntrusiveOrderNode>`):**
   - Pre-allocate `64`-byte cache-line-aligned intrusive order slots in an $O(1)$ free-list pool, embedding doubly-linked FIFO prev/next indices directly inside `IntrusiveOrderNode`.
2. **Flat Price-Level Storage & Direct Index Lookup (`FlatOrderBook`):**
   - Replace per-order heap `std::list` allocations and node-based maps with contiguous sorted active price-level arrays paired with a flat `direct_lookup_` table for $O(1)$ cancel/modify resolution by `OrderId`.
3. **Compile-Time Engine Specialization (`BasicMatchingEngine<BookType>`):**
   - Parameterize matching logic on `BookType`, exposing both `MatchingEngine` (STL oracle) and `FastMatchingEngine` (`FlatOrderBook`) behind an identical API so every optimization is verified against the STL reference oracle.

---

## Consequences

- **Measured Gains (`GCC 15.2.0 -O3`):**
  - `AddAndCancel`: `21.17 M/s` → **`86.71 M/s`** (**`4.10x` speedup**, P50 `57 ns` → **`29 ns`**, P99 `81 ns` → **`40 ns`**).
  - `RestingLimitOrders`: **`1.92x`** speedup (Max latency `1,668 ns` → **`132 ns`**).
  - `CrossingLimitOrders`: **`1.86x`** speedup (`13.02 M/s` → **`24.26 M/s`**).
  - Callgrind (`MixedWorkload_10k`): **`-27.0%`** instructions, **`-28.0%`** D1 read misses, **`-42.7%`** branch mispredictions, and **`-85.8%`** indirect branch mispredictions.
- **Trade-off:** Higher initial capacity reservation and linear sorted-vector shifts on extreme price dispersion outside tight spreads; future optimizations target `OrderResult::trades` vector reuse to remove remaining trade-return allocations.
