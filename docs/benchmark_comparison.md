# Performance Optimization Comparison: STL Baseline vs. ObjectPool + FlatOrderBook

**Milestone:** 4 (Issues #16 & #17)  
**Environment:** WSL2 Ubuntu Linux x86_64, 12 × 2496 MHz CPU (`L1d: 48 KiB`, `L1i: 32 KiB`, `L2: 1280 KiB`, `L3: 12288 KiB`), GCC 15.2.0 (`-O3 -DNDEBUG -std=c++20`).

---

## 1. Google Benchmark & Tail-Latency Head-to-Head (`--benchmark_min_time=0.2s`)

| Workload | Variant | Throughput | Batch CPU | P50 (ns) | P90 (ns) | P95 (ns) | P99 (ns) | P99.9 (ns) | Max (ns) | Speedup |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Add & Cancel** | STL `OrderBook` | `21.17 M/s` | `47,236 ns` | `57` | `58` | `69` | `81` | `91` | `556` | `1.00x` |
| **Add & Cancel** | `FlatOrderBook` + `ObjectPool` | **`86.71 M/s`** | **`11,533 ns`** | **`29`** | **`30`** | **`30`** | **`40`** | `123` | **`356`** | **`4.10x`** |
| **Resting Limits** | STL `MatchingEngine` | `10.56 M/s` | `94,691 ns` | `62` | `97` | `121` | `175` | `828` | `1,668` | `1.00x` |
| **Resting Limits** | `FastMatchingEngine` | **`20.26 M/s`** | **`49,354 ns`** | **`39`** | **`53`** | **`59`** | **`70`** | **`95`** | **`132`** | **`1.92x`** |
| **Crossing Limits** | STL `MatchingEngine` | `13.02 M/s` | `76,786 ns` | `77` | `108` | `136` | `230` | `857` | `1,237` | `1.00x` |
| **Crossing Limits** | `FastMatchingEngine` | **`24.26 M/s`** | **`41,224 ns`** | **`54`** | **`62`** | **`68`** | **`92`** | **`499`** | **`1,019`** | **`1.86x`** |
| **Mixed 10k Sim** | STL `MatchingEngine` | `7.80 M/s` | `1,282,160 ns` | `95` | `189` | `233` | `801` | `1,789` | `110,160` | `1.00x` |
| **Mixed 10k Sim** | `FastMatchingEngine` | **`11.90 M/s`** | **`840,180 ns`** | **`62`** | **`106`** | **`126`** | **`774`** | `2,129` | `117,662` | **`1.53x`** |

---

## 2. Callgrind Cache & Branch Simulation (`MixedWorkload_10k`)

| Metric | STL Baseline | `ObjectPool` + `FlatOrderBook` | Delta |
| :--- | ---: | ---: | ---: |
| **Instructions Executed (`Ir`)** | `26,069,440` | **`19,025,592`** | **`-27.0%`** |
| **Data Reads (`Dr`)** | `6,906,082` | **`4,849,136`** | **`-29.8%`** |
| **D1 Read Misses (`D1mr`)** | `107,846` | **`77,652`** | **`-28.0%`** |
| **Total Branches** | `3,914,127` | **`2,833,110`** | **`-27.6%`** |
| **Branch Mispredictions** | `323,235` (`8.3%`) | **`185,326` (`6.5%`)** | **`-42.7%`** |
| **Indirect Branch Mispredicts (`Bim`)** | `43,548` (`11.7%`) | **`6,175` (`4.4%`)** | **`-85.8%`** |

*(Note: Callgrind `D1mw` / `LLd` write misses rose slightly from `73,396`/`68,612` to `106,980`/`104,231` due to contiguous `64B` `IntrusiveOrderNode` / `direct_lookup_` writes over a freshly constructed book per iteration.)*

---

## 3. Heaptrack Allocation Findings & Remaining Tail-Latency Root Cause

- **Zero Order-Node Allocations:** `FlatOrderBook` eliminates all per-order `std::list<Order>` and `std::map` node heap allocations via `ObjectPool<IntrusiveOrderNode>` (`64B` cache-line-aligned intrusive slots) and `direct_lookup_` array indexing—driving `AddAndCancel` to **`86.71 M ops/sec` (`4.10x`)** and cutting `RestingLimitOrders` `Max` latency from **`1,668 ns` down to `132 ns` (`12.6x`)**.
- **Remaining `MixedWorkload_10k` Allocations (`494,209` calls in `0.28s`):** Heaptrack confirms that the remaining `operator new(std::align_val_t)` calls in `BasicMatchingEngine::process_order_on_book` come from constructing `OrderResult::trades` (`std::vector<Trade>`) and growing `MatchingEngine::trades_` (`alignas(64)` `Trade` buffer) during trades—explaining why `Max_ns` on `MixedWorkload_10k` retains a `~110–118 µs` allocator resize spike while `P50`–`P95` improved by **35–46%**.
