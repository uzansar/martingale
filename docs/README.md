# Architecture Decision Records (ADRs) & Technical Documentation

This directory houses design documentation, Architecture Decision Records (ADRs), performance benchmark reports, quantitative microstructure research findings, and Doxygen API configuration for **Martingale Exchange**.

## Architecture Decision Records

- [ADR-001: Fixed-Point Arithmetic](ADR-001-fixed-point-arithmetic.md) — Discrete 64-bit integer tick pricing and quantities across matching internals.
- [ADR-002: STL Baseline Order Book](ADR-002-stl-baseline-order-book.md) — Reference `OrderBook` design (`std::map`, `std::list`, `std::unordered_map`) prioritizing correctness before optimization.
- [ADR-003: Single-Threaded Deterministic Matching](ADR-003-single-threaded-deterministic-matching.md) — Lock-free single-threaded matching loop decoupled from I/O, journaling, and offline analytics.
- [ADR-004: Binary Sequential Event Journal & Replay](ADR-004-binary-event-journal-and-deterministic-replay.md) — `MTGLJRN1` binary event journal, `ReplayEngine`, and concept-constrained `MarketDataPublisher`.
- [ADR-005: Pre-Allocated ObjectPool & FlatOrderBook](ADR-005-object-pool-and-flat-order-book.md) — Intrusive FIFO node pooling and contiguous flat book optimization (`4.10x` OrderBook throughput gain).
- [ADR-006: Seeded Market Simulator & Decoupled Analytics](ADR-006-seeded-market-simulator-and-decoupled-analytics.md) — `std::mt19937_64` multi-agent simulation harness and downstream CSV/Polars/Plotly research pipeline.

## Performance & Research Reports

- [Performance Benchmark Baseline](benchmark_baseline.md) — Milestone 4 Google Benchmark throughput, latency percentiles (`P50`–`Max`), Callgrind, and Heaptrack baseline measurements.
- [STL Baseline vs. ObjectPool + FlatOrderBook Comparison](benchmark_comparison.md) — Head-to-head Milestone 4 optimization results (`86.71 M ops/sec` OrderBook, `4.10x` speedup, cache/branch deltas).
- [Market Microstructure Experiments & Findings](microstructure_experiments.md) — Milestone 5 empirical parameter sweeps (Institutional order size, Market Maker half-spread, Momentum participation) and CSV dataset guide.

## Building Doxygen API Documentation

Generate browsable HTML documentation (rendered to `docs/html/`, excluded from Git via `.gitignore`) using Doxygen `>= 1.15`:

```bash
doxygen Doxyfile
```
