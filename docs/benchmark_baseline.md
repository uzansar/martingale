# Baseline Performance & Profiling Report (STL OrderBook & MatchingEngine)

**Milestone:** 4 (Issue #15)  
**Environment:** WSL2 Ubuntu Linux x86_64, 12 × 2496 MHz CPU (`L1d: 48 KiB`, `L1i: 32 KiB`, `L2: 1280 KiB`, `L3: 12288 KiB`), GCC 15.2.0 (`-O3 -DNDEBUG -std=c++20`).

---

## 1. Google Benchmark & Latency Percentile Baseline

Command:
```bash
./build/gcc-release/benchmarks/martingale_benchmarks --benchmark_min_time=0.2s
```

| Benchmark | Throughput (`items/s`) | Batch CPU | Mean (ns) | P50 (ns) | P90 (ns) | P95 (ns) | P99 (ns) | P99.9 (ns) | Max (ns) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `BM_OrderBook_AddAndCancel` | `19.46 M/s` | `51,395 ns` | `55.99` | `55` | `56` | `56` | `94` | `202` | `613` |
| `BM_MatchingEngine_RestingLimitOrders` | `9.48 M/s` | `105,513 ns` | `97.99` | `89` | `118` | `142` | `204` | `1,639` | `3,332` |
| `BM_MatchingEngine_CrossingLimitOrders` | `8.49 M/s` | `117,848 ns` | `117.63` | `90` | `119` | `131` | `191` | `459` | `24,155` |
| `BM_MatchingEngine_MixedWorkload_10k` | `6.78 M/s` | `1,474,450 ns` | `157.63` | `107` | `217` | `269` | `804` | `2,910` | `102,784` |

**Key Observation:** While median (`P50`) latency is `55–107 ns`, tail latency degrades sharply at `P99.9` (`2,910 ns`) and `Max` (`102,784 ns`, ~103 µs) under mixed trading workloads.

---

## 2. Heaptrack Memory & Allocation Profile

Command:
```bash
heaptrack ./build/gcc-release/benchmarks/martingale_benchmarks \
  --benchmark_filter=BM_MatchingEngine_MixedWorkload_10k --benchmark_min_time=0.1s
```

- **Total runtime (instrumented):** `0.37 s`
- **Total allocation calls:** `836,635` (`2,267,303 allocations/sec`)
- **Temporary allocations:** `54,891` (`148,756/sec`)
- **Peak heap consumption:** `2.46 MB` (`7.78 MB` RSS including Heaptrack overhead)
- **Primary allocation hotspots:**
  - `258,816` aligned `operator new(unsigned long, std::align_val_t)` calls (`234,117` from `MatchingEngine::submit_order` creating/growing `alignas(64)` `Trade` vectors and `OrderResult` payloads).
  - Node-based heap allocations per resting limit order in `std::list<Order>`, `std::map<Price, PriceLevel>`, and two `std::unordered_map` registries (`OrderBook::order_lookup_` and `MatchingEngine::active_order_instruments_`).

---

## 3. Linux `perf` & Valgrind Callgrind (Cache & Branch Simulation)

> **Environment Note:** Inside WSL2 Hyper-V, hardware PMU events (`cpu_atom` / `cycles`) are not exposed by the virtualized kernel (`No supported events found` on default `perf stat`). We therefore measured kernel software counters via `perf stat` alongside full instruction, L1/LL cache, and branch-prediction simulation via `valgrind --tool=callgrind --cache-sim=yes --branch-sim=yes`.

### `perf stat` Software Counters (`--benchmark_min_time=0.2s`)
- **Task clock:** `449.08 msec` (`0.460 s` wall elapsed)
- **Context switches:** `0`
- **CPU migrations:** `0`
- **Minor page faults:** `100,238` (driven by repeated container allocation/deallocation churn)

### Callgrind Cache & Branch Simulation (`--benchmark_min_time=0.05s`)
- **Instructions executed (`Ir`):** `26,069,440` (`I1 miss rate: 0.03%`, `LLi miss rate: 0.02%`)
- **Data memory references (`D`):** `11,252,767` (`6,906,082` reads + `4,346,685` writes)
- **D1 misses:** `181,242` (`1.6%` miss rate) | **LLd misses:** `68,612` (`0.6%` miss rate)
- **Branches:** `3,914,127` (`3,540,714` conditional + `373,413` indirect)
- **Branch mispredictions:** `323,235` (`8.3%` total: `7.9%` conditional, `11.7%` indirect)
- **Top STL overhead hotspots:** `std::_Rb_tree_insert_and_rebalance` and duplicate `std::_Hashtable::erase` paths across `OrderBook::order_lookup_` and `MatchingEngine::active_order_instruments_`.

---

## 4. Optimization Hypotheses for Issues #16 & #17

1. **Issue #16 (Object Pool / Arena Allocator):** Eliminate per-order `std::list<Order>` node allocations and dynamic `new`/`delete` churn from the hot path by pre-allocating a fixed-stride contiguous `ObjectPool` / slab for order nodes.
2. **Issue #17 (Intrusive Queues & Flat Price Levels):** Replace `std::list` + `std::map` red-black tree pointer chasing with intrusive doubly-linked FIFO hooks inside pooled order nodes and contiguous/flat price-level structures, reducing `D1`/`LLd` cache misses, branch mispredictions, and `P99.9`/`Max` tail spikes.
