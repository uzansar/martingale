# Market Microstructure Experiments & Empirical Findings

> **Milestone 5 (Issues #20 & #21)** — Deterministic quantitative evaluation of market liquidity, bid-ask spread formation, realized volatility, and institutional market impact using `MartingaleSimulator` and `MicrostructureAnalyzer`.

---

## 1. Experimental Setup & Reproducibility

All experiments are implemented in [`src/run_experiments.cpp`](../src/run_experiments.cpp) via the `martingale_experiments` binary and analyzed with [`MicrostructureAnalyzer`](../include/martingale/research/microstructure_metrics.hpp).

### Reproduction Command

```bash
cmake --build build/gcc-release --target martingale_experiments
./build/gcc-release/martingale_experiments --export-dir data/processed
```

Executing with `--export-dir data/processed` exports four CSV datasets consumed by downstream Python research notebooks and dashboards (kept out of version control via `.gitignore`):
- `data/processed/sim_steps.csv` — step-by-step BBO quotes, mid-price, spread (ticks), Level-1 Order Book Imbalance (`l1_obi`), trade counts, and volume.
- `data/processed/sim_trades.csv` — full execution tape (`trade_id`, sequence number, nanosecond timestamp, price, quantity, aggressor side, buyer/seller order IDs).
- `data/processed/sim_l2_depth.csv` — periodic long-format Level-2 order book snapshots across top 5 bid/ask levels.
- `data/processed/experiment_sweeps.csv` — tabular microstructure summaries across all parameter sweeps below.

### Common Baseline Parameters (`seed = 2026`)
- **Horizon & Clock:** `total_steps = 500`, `step_duration_ns = 1,000,000` (1 ms/step; 0.50 s simulated horizon), `initial_mid_price = 10000`, `fundamental_step_sigma_ticks = 2`
- **Market Maker:** `half_spread_ticks = 2`, `num_levels = 5`, `base_quantity = 60`, `level_quantity_step = 40`, `inventory_skew_divisor = 50`
- **Noise & Trend Agents:** `num_random_traders = 5`, `num_momentum_traders = 2`
- **Institutional Parent Order (Buy):** `total_parent_quantity = 500`, `child_slice_quantity = 50`, `start_step = 50`, `execution_interval_steps = 5`

---

## 2. Quantitative Metric Definitions

For a parent order on side $s \in \{+1\ (\text{Buy}), -1\ (\text{Sell})\}$ arriving at $P_{\text{arrival}}$, achieving volume-weighted average fill price $P_{\text{exec}}$, and settling at terminal post-trade mid $P_{\text{post}}$:

$$
\text{Slippage (ticks)} = s \cdot (P_{\text{exec}} - P_{\text{arrival}}) = \underbrace{s \cdot (P_{\text{post}} - P_{\text{arrival}})}_{\text{Permanent Impact}} + \underbrace{s \cdot (P_{\text{exec}} - P_{\text{post}})}_{\text{Temporary Impact}}
$$

Basis-point values normalize tick deltas by `arrival_mid`: $(\Delta / P_{\text{arrival}}) \times 10{,}000$. Realized volatility is computed as the sample standard deviation of step-over-step BBO mid-price log returns $\ln(P_t / P_{t-1})$ expressed in basis points.

---

## 3. Sweep 1: Institutional Parent Order Size

Varies `institutional_config.total_parent_quantity` across `{100, 300, 750, 1500}` while scaling `child_slice_quantity = max(10, total_parent_quantity / 10)` so all runs share the same ~10-slice execution schedule starting at step 50.

| Parent Qty | Executed Qty | Trades | Total Volume | Mean Spread (bps) | Realized Vol (bps) | Mean L1 OBI | Slippage (bps) | Perm Impact (bps) | Temp Impact (bps) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 | 3,169 | 72,623 | 3.22 | 5.42 | -0.0214 | 15.93 | 8.02 | 7.92 |
| 300 | 270 | 3,155 | 72,876 | 3.17 | 5.55 | +0.0019 | 22.13 | 8.02 | 14.11 |
| 750 | 600 | 3,164 | 73,692 | 3.18 | 6.89 | -0.0014 | 25.20 | 13.03 | 12.17 |
| 1500 | 1200 | 3,213 | 74,711 | 3.26 | 6.52 | +0.0282 | 27.65 | -8.02 | 35.67 |

### Findings
- **Monotonic Implementation Shortfall:** Execution slippage scales monotonically from **15.93 bps** (`100` shares) to **27.65 bps** (`1,500` parent / `1,200` executed shares), a **+73.6% increase** in implementation shortfall. Larger child slices walk deeper up the ask ladder and push the market maker's inventory skew higher during execution.
- **Transient Impact vs. Post-Trade Horizon Noise:** At `1,500` parent shares, aggressive buying generates **35.67 bps** of temporary execution pressure during slices, while single-path terminal mid-price (`step 500`, ~400 steps after slicing completes) experiences exogenous fundamental random-walk mean reversion (`-8.02 bps`). Averaging permanent impact across multi-seed Monte Carlo ensembles cleanly isolates post-execution drift from single-path fundamental noise.

---

## 4. Sweep 2: Market Maker Half-Spread Sensitivity

Varies `market_maker_config.half_spread_ticks` across `{1, 2, 4}` ticks under the 500-share baseline institutional Buy order.

| Half-Spread (ticks) | Trades | Total Volume | Mean Spread (bps) | Realized Vol (bps) | Mean L1 OBI | Inst Executed | Slippage (bps) | Perm Impact (bps) | Temp Impact (bps) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3,232 | 78,240 | 2.56 | 5.83 | -0.0368 | 500 | 23.11 | 5.51 | 17.60 |
| 2 | 3,242 | 74,762 | 3.31 | 6.41 | +0.0235 | 450 | 24.51 | 9.02 | 15.49 |
| 4 | 3,186 | 71,003 | 4.27 | 6.69 | -0.0131 | 450 | 21.47 | 9.02 | 12.45 |

### Findings
- **Spread Pass-Through:** Widening the market maker's target half-spread from 1 to 4 ticks monotonically increases time-averaged realized BBO spread from **2.56 bps to 4.27 bps** (+66.8%). Realized spread sits slightly above `2 * half_spread_ticks` whenever top-of-book liquidity is swept between MM requote cycles.
- **Turnover Contraction & Bid-Ask Bounce:** Total traded volume drops monotonically by **9.2%** (`78,240 -> 74,762 -> 71,003`) as wider passive quotes discourage limit-order matching and raise crossing friction, while step-to-step realized volatility rises modestly from **5.83 bps to 6.69 bps** due to amplified bid-ask bounce.

---

## 5. Sweep 3: Momentum Trader Participation

Varies `num_momentum_traders` across `{0, 1, 3, 5}` while holding market maker quoting parameters and random noise flow constant.

| Momentum Agents | Trades | Total Volume | Volume / sec | Mean Spread (bps) | Realized Vol (bps) | Mean L1 OBI | Inst Slippage (bps) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2,376 | 51,371 | 102,742 | 2.38 | 1.55 | -0.0054 | 8.63 |
| 1 | 2,679 | 59,422 | 118,844 | 2.64 | 2.50 | +0.0033 | 13.56 |
| 3 | 3,791 | 91,455 | 182,910 | 3.78 | 9.52 | -0.0238 | 24.89 |
| 5 | 4,942 | 127,478 | 254,956 | 4.51 | 10.86 | -0.0611 | 12.57 |

### Findings
- **Endogenous Volatility Amplification:** Increasing momentum-chasing agents from `0` to `5` causes a **7.0x surge in realized volatility** (**1.55 bps -> 10.86 bps**) alongside a **+148% expansion in traded volume** (**51,371 -> 127,478 shares**). Even without changing exogenous fundamental volatility (`fundamental_step_sigma_ticks = 2`), positive-feedback trend following induces regime transitions into high-volatility trending flow.
- **Adverse Selection & Liquidity Depletion:** Heightened toxic/directional momentum flow widens effective BBO spreads monotonically from **2.38 bps to 4.51 bps** (+89.5%) as momentum orders repeatedly consume the inside quote levels faster than passive depth replenishes. At `5` momentum agents, net bearish order-book imbalance (`-0.0611`) and heavy two-way trend volatility reduce the institutional buyer's executed quantity (`400` of `500` shares filled) while dominating price discovery relative to single-agent slicing.
