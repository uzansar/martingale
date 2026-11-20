# ADR-006: Seeded Event-Driven Market Simulation and Decoupled Analytics Pipeline

- **Status:** Accepted (Milestone 5 — Weeks 9–10)
- **Implementation:** [`include/martingale/simulator/agents.hpp`](../include/martingale/simulator/agents.hpp), [`include/martingale/simulator/market_simulator.hpp`](../include/martingale/simulator/market_simulator.hpp), [`include/martingale/research/microstructure_metrics.hpp`](../include/martingale/research/microstructure_metrics.hpp), [`src/run_experiments.cpp`](../src/run_experiments.cpp)
- **Empirical Report:** [`microstructure_experiments.md`](microstructure_experiments.md)

---

## Context

Evaluating market quality, spread formation, endogenous volatility clustering, and institutional market impact requires driving heterogeneous synthetic order flow against the real exchange matching path while ensuring quantitative research and interactive visualization never add runtime coupling or nondeterminism to C++ matching internals.

---

## Decision

1. **Seeded Multi-Agent Simulation Harness (`MarketSimulator`):**
   - Orchestrate `MarketMakerAgent`, `RandomTraderAgent`, `MomentumTraderAgent`, and `InstitutionalTraderAgent` using an explicit `std::mt19937_64` PRNG seed through the real `OrderGateway` and `MatchingEngine`.
   - Enforce exact zero-sum inventory (`sum == 0`) and cash-flow (`sum == 0`) accounting invariants across all exchange participants on every simulation run.
2. **Offline Microstructure Engine & CSV Telemetry (`MicrostructureAnalyzer`):**
   - Compute spreads, microprice, L1/multi-level OBI, VWAP, realized volatility (bps), trade intensity, liquidity depth distributions, and permanent/temporary impact offline from immutable `SimulationResult` records, exporting normalized CSV tables (`sim_steps.csv`, `sim_trades.csv`, `sim_l2_depth.csv`, `experiment_sweeps.csv`) via `martingale_experiments`.
3. **Decoupled Python Visualization (`python/`):**
   - Keep `Polars` and `Plotly` analytics strictly downstream of immutable CSV/JSON artifacts.

---

## Consequences

- **Benefits:** Identical seeds guarantee bit-for-bit reproducible research sweeps (`Institutional Order Size`, `Market Maker Spread`, `Momentum Participation`), and Python visualization upgrades (`Polars`/`Plotly`) require zero changes or runtime overhead inside C++ matching libraries.
- **Trade-off:** Disk-serialized CSV/JSON interchange introduces an offline export step (`martingale_experiments`, `martingale_benchmarks`) rather than live in-memory IPC streaming.
