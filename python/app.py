"""Live Streamlit web application for Martingale Exchange C++20 simulations."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any

import plotly.graph_objects as go
from plotly.subplots import make_subplots
import polars as pl
import streamlit as st

from data_pipeline import MartingaleDataPipeline


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DATA_DIR = Path(
    os.environ.get("MARTINGALE_DATA_DIR", str(PROJECT_ROOT / "data" / "processed"))
)

_TERMINAL_CSS = """
<style>
.block-container {
    padding-top: 1.35rem;
    padding-bottom: 1.5rem;
}
[data-testid="stSidebar"] > div:first-child {
    padding-top: 1.1rem;
}
.mtgl-header {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    justify-content: space-between;
    gap: 0.75rem;
    padding-bottom: 0.65rem;
    margin-bottom: 0.75rem;
    border-bottom: 1px solid #1f2937;
}
.mtgl-title {
    margin: 0;
    font-size: 1.45rem;
    font-weight: 700;
    letter-spacing: -0.015em;
    color: #f8fafc;
}
.mtgl-subtitle {
    margin: 0.15rem 0 0 0;
    font-size: 0.85rem;
    color: #94a3b8;
}
.mtgl-badges {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
}
.mtgl-badge {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 0.75rem;
    color: #22d3ee;
    background: #111827;
    border: 1px solid #1f2937;
    border-radius: 6px;
    padding: 0.2rem 0.55rem;
}
[data-testid="stMetric"] {
    background: #111827;
    border: 1px solid #1f2937;
    border-radius: 8px;
    padding: 12px 14px;
}
[data-testid="stMetricLabel"] {
    font-size: 0.8rem;
    color: #94a3b8;
}
[data-testid="stMetricValue"] {
    font-variant-numeric: tabular-nums;
    font-size: 1.5rem;
    color: #f8fafc;
}
[data-testid="stMetricDelta"] {
    font-variant-numeric: tabular-nums;
    font-size: 0.78rem;
    color: #94a3b8;
}
</style>
"""


def _apply_terminal_layout(
    fig: go.Figure,
    title: str,
    height: int = 410,
    barmode: str | None = None,
) -> go.Figure:
    """Applies unified dark trading-terminal styling to any Plotly figure."""
    layout_kwargs: dict[str, Any] = {
        "template": "plotly_dark",
        "title": dict(text=title, font=dict(size=15, color="#f8fafc"), x=0.0, xanchor="left"),
        "paper_bgcolor": "rgba(0,0,0,0)",
        "plot_bgcolor": "rgba(17,24,39,0.55)",
        "font": dict(color="#e2e8f0", size=12),
        "hovermode": "x unified",
        "height": height,
        "margin": dict(l=52, r=36, t=64, b=46),
        "legend": dict(
            orientation="h",
            yanchor="bottom",
            y=1.02,
            xanchor="right",
            x=1.0,
            bgcolor="rgba(0,0,0,0)",
        ),
    }
    if barmode is not None:
        layout_kwargs["barmode"] = barmode
    fig.update_layout(**layout_kwargs)
    fig.update_xaxes(gridcolor="#1f2937", zerolinecolor="#374151")
    fig.update_yaxes(gridcolor="#1f2937", zerolinecolor="#374151")
    return fig


def _windows_to_wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive = resolved.drive.rstrip(":").lower()
    if not drive:
        return resolved.as_posix()
    tail = resolved.as_posix().split(":", 1)[1]
    return f"/mnt/{drive}{tail}"


def resolve_engine_command() -> tuple[list[str], bool]:
    """Resolves the martingale_experiments command prefix and whether WSL translation is needed."""
    env_bin = os.environ.get("MARTINGALE_EXPERIMENTS_BIN")
    if env_bin:
        bin_path = Path(env_bin)
        if not bin_path.exists():
            raise FileNotFoundError(
                f"MARTINGALE_EXPERIMENTS_BIN points to missing file: {bin_path}"
            )
        return ([str(bin_path)], False)

    native_bin = PROJECT_ROOT / "build" / "gcc-release" / "martingale_experiments"
    if native_bin.exists():
        if os.name != "nt":
            return ([str(native_bin)], False)
        if shutil.which("wsl"):
            return (["wsl", "-e", _windows_to_wsl_path(native_bin)], True)

    exe_bin = native_bin.with_suffix(".exe")
    if exe_bin.exists():
        return ([str(exe_bin)], False)

    raise FileNotFoundError(
        "Could not locate compiled 'martingale_experiments' binary. "
        "Build with CMake/Ninja or set MARTINGALE_EXPERIMENTS_BIN."
    )


def run_live_simulation(
    params: dict[str, Any],
) -> tuple[pl.DataFrame, pl.DataFrame, pl.DataFrame, pl.DataFrame]:
    """Executes the C++20 matching engine in an isolated directory and returns Polars frames."""
    cmd_prefix, use_wsl = resolve_engine_command()

    with tempfile.TemporaryDirectory() as tmp_dir_str:
        tmp_dir = Path(tmp_dir_str)
        export_arg = _windows_to_wsl_path(tmp_dir) if use_wsl else str(tmp_dir)

        cmd = [
            *cmd_prefix,
            "--single-run",
            "--seed",
            str(int(params["seed"])),
            "--steps",
            str(int(params["steps"])),
            "--mm-half-spread",
            str(int(params["mm_half_spread"])),
            "--mm-skew-divisor",
            str(int(params["mm_skew_divisor"])),
            "--random-traders",
            str(int(params["random_traders"])),
            "--momentum-traders",
            str(int(params["momentum_traders"])),
            "--inst-side",
            str(params["inst_side"]),
            "--inst-qty",
            str(int(params["inst_qty"])),
            "--inst-slice",
            str(int(params["inst_slice"])),
            "--export-dir",
            export_arg,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            raise RuntimeError(
                f"C++ simulator failed (exit {proc.returncode}):\n{proc.stderr or proc.stdout}"
            )

        pipeline = MartingaleDataPipeline(data_dir=tmp_dir)
        steps_df = pipeline.load_steps()
        trades_df = pipeline.load_trades()
        l2_profile_df = pipeline.summarize_l2_depth_profile()
        summary_df = pl.read_csv(tmp_dir / "sim_summary.csv")
        return steps_df, trades_df, l2_profile_df, summary_df


def build_price_tape_figure(steps_df: pl.DataFrame, trades_df: pl.DataFrame) -> go.Figure:
    """Builds BBO quotes, mid-price, running VWAP, and buy/sell trade tape figure."""
    fig = go.Figure()
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["best_ask"].to_list(),
            name="Best Ask",
            line=dict(color="rgba(248, 113, 113, 0.55)", width=1.1),
            mode="lines",
        )
    )
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["best_bid"].to_list(),
            name="Best Bid",
            line=dict(color="rgba(52, 211, 153, 0.55)", width=1.1),
            mode="lines",
        )
    )
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["mid_price"].to_list(),
            name="Mid-Price",
            line=dict(color="#38bdf8", width=2.2),
            mode="lines",
        )
    )
    if trades_df.height > 0:
        fig.add_trace(
            go.Scatter(
                x=trades_df["time_ms"].to_list(),
                y=trades_df["running_vwap"].to_list(),
                name="Running VWAP",
                line=dict(color="#facc15", width=2.0, dash="dot"),
                mode="lines",
            )
        )
        buy_trades = trades_df.filter(pl.col("aggressor_side") == "BUY")
        sell_trades = trades_df.filter(pl.col("aggressor_side") == "SELL")
        fig.add_trace(
            go.Scatter(
                x=buy_trades["time_ms"].to_list(),
                y=buy_trades["price"].to_list(),
                name="Buy Prints",
                mode="markers",
                marker=dict(color="#10b981", size=4, opacity=0.38),
            )
        )
        fig.add_trace(
            go.Scatter(
                x=sell_trades["time_ms"].to_list(),
                y=sell_trades["price"].to_list(),
                name="Sell Prints",
                mode="markers",
                marker=dict(color="#f43f5e", size=4, opacity=0.38),
            )
        )

    _apply_terminal_layout(
        fig, "BBO Quotes, Mid-Price, Running VWAP & Execution Tape", height=430
    )
    fig.update_xaxes(title_text="Simulated Time (ms)")
    fig.update_yaxes(title_text="Price (ticks)")
    return fig


def build_spread_obi_figure(steps_df: pl.DataFrame) -> go.Figure:
    """Builds dual-axis chart of realized spread (bps) and rolling L1 OBI."""
    fig = make_subplots(specs=[[{"secondary_y": True}]])
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["spread_bps"].to_list(),
            name="Spread (bps)",
            line=dict(color="#c084fc", width=1.8),
            mode="lines",
        ),
        secondary_y=False,
    )
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["rolling_l1_obi"].to_list(),
            name="20-Step Rolling L1 OBI",
            line=dict(color="#22d3ee", width=2.0),
            mode="lines",
        ),
        secondary_y=True,
    )
    _apply_terminal_layout(
        fig, "Realized Spread (bps) & Rolling Level-1 Imbalance", height=380
    )
    fig.update_xaxes(title_text="Simulated Time (ms)")
    fig.update_yaxes(title_text="Spread (bps)", secondary_y=False)
    fig.update_yaxes(title_text="Rolling L1 OBI", range=[-1.0, 1.0], secondary_y=True)
    return fig


def build_l2_depth_figure(l2_profile: pl.DataFrame) -> go.Figure:
    """Builds bar chart comparing mean bid and ask resting quantities per level."""
    levels = [f"Level {lvl + 1}" for lvl in l2_profile["level"].to_list()]
    fig = go.Figure()
    fig.add_trace(
        go.Bar(
            x=levels,
            y=l2_profile["mean_bid_qty"].to_list(),
            name="Mean Bid Depth",
            marker_color="#10b981",
        )
    )
    fig.add_trace(
        go.Bar(
            x=levels,
            y=l2_profile["mean_ask_qty"].to_list(),
            name="Mean Ask Depth",
            marker_color="#f43f5e",
        )
    )
    _apply_terminal_layout(
        fig, "Mean Level-2 Resting Liquidity Profile (Top 5 Levels)", height=380, barmode="group"
    )
    fig.update_xaxes(title_text="Book Depth Level")
    fig.update_yaxes(title_text="Mean Resting Quantity")
    return fig


def build_sweeps_figures(sweeps_df: pl.DataFrame) -> tuple[go.Figure, go.Figure, go.Figure]:
    """Builds charts for Institutional Size, MM Half-Spread, and Momentum sweeps."""
    inst = sweeps_df.filter(pl.col("experiment") == "institutional_size").sort("sweep_value")
    fig_inst = go.Figure()
    fig_inst.add_trace(
        go.Scatter(
            x=inst["sweep_value"].to_list(),
            y=inst["inst_slippage_bps"].to_list(),
            name="Total Slippage (bps)",
            mode="lines+markers",
            line=dict(color="#facc15", width=2.5),
        )
    )
    fig_inst.add_trace(
        go.Bar(
            x=inst["sweep_value"].to_list(),
            y=inst["inst_permanent_impact_bps"].to_list(),
            name="Permanent Impact (bps)",
            marker_color="#38bdf8",
        )
    )
    fig_inst.add_trace(
        go.Bar(
            x=inst["sweep_value"].to_list(),
            y=inst["inst_temporary_impact_bps"].to_list(),
            name="Temporary Impact (bps)",
            marker_color="#fb923c",
        )
    )
    _apply_terminal_layout(
        fig_inst,
        "Sweep 1: Institutional Parent Order Size vs. Slippage & Impact",
        height=390,
        barmode="group",
    )
    fig_inst.update_xaxes(title_text="Total Parent Quantity (shares)")
    fig_inst.update_yaxes(title_text="Basis Points (bps)")

    spread_df = sweeps_df.filter(pl.col("experiment") == "market_maker_spread").sort("sweep_value")
    fig_spread = make_subplots(specs=[[{"secondary_y": True}]])
    fig_spread.add_trace(
        go.Bar(
            x=spread_df["sweep_value"].to_list(),
            y=spread_df["mean_spread_bps"].to_list(),
            name="Mean Spread (bps)",
            marker_color="#c084fc",
        ),
        secondary_y=False,
    )
    fig_spread.add_trace(
        go.Scatter(
            x=spread_df["sweep_value"].to_list(),
            y=spread_df["total_volume"].to_list(),
            name="Total Volume",
            mode="lines+markers",
            line=dict(color="#22d3ee", width=2.5),
        ),
        secondary_y=True,
    )
    _apply_terminal_layout(
        fig_spread, "Sweep 2: Market Maker Half-Spread vs. Effective Spread & Volume", height=390
    )
    fig_spread.update_xaxes(title_text="MM Half-Spread (ticks)")
    fig_spread.update_yaxes(title_text="Mean Spread (bps)", secondary_y=False)
    fig_spread.update_yaxes(title_text="Total Traded Volume", secondary_y=True)

    mom_df = sweeps_df.filter(pl.col("experiment") == "momentum_traders").sort("sweep_value")
    fig_mom = make_subplots(specs=[[{"secondary_y": True}]])
    fig_mom.add_trace(
        go.Scatter(
            x=mom_df["sweep_value"].to_list(),
            y=mom_df["realized_volatility_bps"].to_list(),
            name="Realized Volatility (bps)",
            mode="lines+markers",
            line=dict(color="#f97316", width=2.5),
        ),
        secondary_y=False,
    )
    fig_mom.add_trace(
        go.Scatter(
            x=mom_df["sweep_value"].to_list(),
            y=mom_df["mean_spread_bps"].to_list(),
            name="Mean Spread (bps)",
            mode="lines+markers",
            line=dict(color="#4ade80", width=2.5, dash="dash"),
        ),
        secondary_y=True,
    )
    _apply_terminal_layout(
        fig_mom, "Sweep 3: Momentum Trader Participation vs. Volatility & Spread", height=390
    )
    fig_mom.update_xaxes(title_text="Momentum Traders (count)")
    fig_mom.update_yaxes(title_text="Realized Volatility (bps)", secondary_y=False)
    fig_mom.update_yaxes(title_text="Mean Spread (bps)", secondary_y=True)

    return fig_inst, fig_spread, fig_mom


def build_benchmarks_figure(bench_df: pl.DataFrame) -> go.Figure:
    """Builds STL Baseline vs. FlatOrderBook + ObjectPool throughput and tail latency figure."""
    stl = bench_df.filter(pl.col("variant") == "STL Baseline")
    fast = bench_df.filter(pl.col("variant") == "FlatOrderBook + ObjectPool")
    workloads = stl["workload"].to_list()

    fig = make_subplots(
        rows=1,
        cols=2,
        subplot_titles=(
            "Throughput (M ops/sec) & Speedup",
            "Tail Latency Percentiles (P50 & P99 ns)",
        ),
        specs=[[{"secondary_y": True}, {"secondary_y": False}]],
    )
    fig.add_trace(
        go.Bar(
            x=workloads,
            y=stl["throughput_mops"].to_list(),
            name="STL Throughput (M/s)",
            marker_color="#64748b",
        ),
        row=1,
        col=1,
        secondary_y=False,
    )
    fig.add_trace(
        go.Bar(
            x=workloads,
            y=fast["throughput_mops"].to_list(),
            name="FlatOrderBook Throughput (M/s)",
            marker_color="#22d3ee",
        ),
        row=1,
        col=1,
        secondary_y=False,
    )
    fig.add_trace(
        go.Scatter(
            x=workloads,
            y=fast["speedup"].to_list(),
            name="Speedup (x)",
            mode="lines+markers+text",
            text=[f"{val:.2f}x" for val in fast["speedup"].to_list()],
            textposition="top center",
            line=dict(color="#facc15", width=2.5),
        ),
        row=1,
        col=1,
        secondary_y=True,
    )

    fig.add_trace(
        go.Bar(x=workloads, y=stl["P50_ns"].to_list(), name="STL P50 (ns)", marker_color="#94a3b8"),
        row=1,
        col=2,
    )
    fig.add_trace(
        go.Bar(
            x=workloads,
            y=fast["P50_ns"].to_list(),
            name="FlatOrderBook P50 (ns)",
            marker_color="#10b981",
        ),
        row=1,
        col=2,
    )
    fig.add_trace(
        go.Bar(
            x=workloads,
            y=fast["P99_ns"].to_list(),
            name="FlatOrderBook P99 (ns)",
            marker_color="#fb923c",
        ),
        row=1,
        col=2,
    )

    _apply_terminal_layout(
        fig,
        "Google Benchmark Engine Performance (STL Baseline vs. FlatOrderBook + ObjectPool)",
        height=440,
        barmode="group",
    )
    fig.update_yaxes(title_text="Throughput (M ops/sec)", row=1, col=1, secondary_y=False)
    fig.update_yaxes(title_text="Speedup (x)", row=1, col=1, secondary_y=True)
    fig.update_yaxes(title_text="Latency (ns)", row=1, col=2)
    return fig


@st.cache_data(show_spinner="Running C++20 Martingale Matching Engine simulation...")
def _cached_live_simulation(
    seed: int,
    steps: int,
    mm_half_spread: int,
    mm_skew_divisor: int,
    random_traders: int,
    momentum_traders: int,
    inst_side: str,
    inst_qty: int,
    inst_slice: int,
) -> tuple[pl.DataFrame, pl.DataFrame, pl.DataFrame, pl.DataFrame]:
    return run_live_simulation(
        {
            "seed": seed,
            "steps": steps,
            "mm_half_spread": mm_half_spread,
            "mm_skew_divisor": mm_skew_divisor,
            "random_traders": random_traders,
            "momentum_traders": momentum_traders,
            "inst_side": inst_side,
            "inst_qty": inst_qty,
            "inst_slice": inst_slice,
        }
    )


def render_app() -> None:
    st.set_page_config(
        page_title="Martingale Exchange — Live Microstructure Simulator",
        layout="wide",
    )
    st.markdown(_TERMINAL_CSS, unsafe_allow_html=True)

    with st.sidebar:
        st.markdown("### Simulation Controls")
        with st.form("simulation_params"):
            c_seed, c_steps = st.columns(2)
            with c_seed:
                seed = st.number_input("Seed", min_value=1, max_value=999_999, value=2026, step=1)
            with c_steps:
                steps = st.number_input(
                    "Steps", min_value=100, max_value=1500, value=500, step=50
                )

            st.form_submit_button("Run Simulation", type="primary", use_container_width=True)

            st.caption("Order Flow Agents")
            random_traders = st.slider("Random Traders", min_value=1, max_value=10, value=5)
            momentum_traders = st.slider("Momentum Traders", min_value=0, max_value=8, value=2)

            st.caption("Institutional Parent Order")
            inst_side = st.selectbox("Execution Side", options=["BUY", "SELL"], index=0)
            inst_qty = st.slider(
                "Parent Qty (0 = disabled)", min_value=0, max_value=2000, value=500, step=50
            )

            with st.expander("Market Maker & Slice Tuning", expanded=False):
                mm_half_spread = st.slider("MM Half-Spread (ticks)", min_value=1, max_value=6, value=2)
                mm_skew_divisor = st.slider(
                    "Inventory Skew Divisor", min_value=10, max_value=200, value=50, step=10
                )
                inst_slice = st.slider(
                    "Child Slice Qty", min_value=10, max_value=200, value=50, step=10
                )

    st.markdown(
        f"""
        <div class="mtgl-header">
            <div>
                <h1 class="mtgl-title">Martingale Exchange</h1>
                <p class="mtgl-subtitle">Deterministic C++20 Price-Time Priority Matching Engine · Polars &amp; Plotly Terminal</p>
            </div>
            <div class="mtgl-badges">
                <span class="mtgl-badge">C++20 Engine</span>
                <span class="mtgl-badge">Seed {int(seed)}</span>
                <span class="mtgl-badge">{int(steps)} steps</span>
                <span class="mtgl-badge">1 ms/step</span>
            </div>
        </div>
        """,
        unsafe_allow_html=True,
    )

    tab_live, tab_sweeps, tab_bench = st.tabs(
        [
            "Live C++20 Simulation",
            "Parameter Sweeps (Milestone 5)",
            "Engine Benchmarks (STL vs. FlatOrderBook)",
        ]
    )

    with tab_live:
        try:
            steps_df, trades_df, l2_profile, summary_df = _cached_live_simulation(
                int(seed),
                int(steps),
                int(mm_half_spread),
                int(mm_skew_divisor),
                int(random_traders),
                int(momentum_traders),
                str(inst_side),
                int(inst_qty),
                int(inst_slice),
            )
        except Exception as exc:  # pragma: no cover - surfaced interactively in UI
            st.error(f"Unable to run live simulation: {exc}")
        else:
            row = summary_df.row(0, named=True)
            k1, k2, k3, k4 = st.columns(4)
            k1.metric(
                "Mean Spread",
                f"{row['mean_spread_bps']:.2f} bps",
                delta=f"RVol {row['realized_volatility_bps']:.2f} bps",
                delta_color="off",
            )
            k2.metric(
                "Session VWAP",
                f"{row['vwap']:,.2f}",
                delta="Trade-weighted mid",
                delta_color="off",
            )
            k3.metric(
                "Order Flow",
                f"{int(row['trade_count']):,} trades",
                delta=f"{int(row['total_volume']):,} vol",
                delta_color="off",
            )
            if int(row["inst_executed_qty"]) > 0:
                k4.metric(
                    "Inst. Slippage",
                    f"{row['inst_slippage_bps']:.2f} bps",
                    delta=(
                        f"Perm {row['inst_permanent_impact_bps']:.1f} · "
                        f"Temp {row['inst_temporary_impact_bps']:.1f} bps"
                    ),
                    delta_color="off",
                )
            else:
                k4.metric(
                    "Inst. Slippage",
                    "Disabled" if int(inst_qty) == 0 else "0 fills",
                    delta="Institutional order",
                    delta_color="off",
                )

            st.plotly_chart(
                build_price_tape_figure(steps_df, trades_df),
                theme=None,
                use_container_width=True,
            )
            col_left, col_right = st.columns(2)
            with col_left:
                st.plotly_chart(
                    build_spread_obi_figure(steps_df), theme=None, use_container_width=True
                )
            with col_right:
                st.plotly_chart(
                    build_l2_depth_figure(l2_profile), theme=None, use_container_width=True
                )

            with st.expander("View Raw Polars Summary & Trade Tape Sample"):
                st.dataframe(summary_df.to_pandas(), use_container_width=True)
                st.dataframe(trades_df.head(50).to_pandas(), use_container_width=True)

    prebaked_pipeline = MartingaleDataPipeline(data_dir=DEFAULT_DATA_DIR)

    with tab_sweeps:
        sweeps_path = DEFAULT_DATA_DIR / "experiment_sweeps.csv"
        if sweeps_path.exists():
            sweeps_df = prebaked_pipeline.load_experiment_sweeps()
            fig_inst, fig_spread, fig_mom = build_sweeps_figures(sweeps_df)
            st.plotly_chart(fig_inst, theme=None, use_container_width=True)
            c1, c2 = st.columns(2)
            with c1:
                st.plotly_chart(fig_spread, theme=None, use_container_width=True)
            with c2:
                st.plotly_chart(fig_mom, theme=None, use_container_width=True)
            st.dataframe(sweeps_df.to_pandas(), use_container_width=True)
        else:
            st.info(f"Pre-computed sweep file not found at {sweeps_path}.")

    with tab_bench:
        bench_path = DEFAULT_DATA_DIR / "benchmarks.json"
        if bench_path.exists():
            bench_df = prebaked_pipeline.load_benchmarks()
            st.plotly_chart(
                build_benchmarks_figure(bench_df), theme=None, use_container_width=True
            )
            st.dataframe(bench_df.to_pandas(), use_container_width=True)
        else:
            st.info(f"Benchmark JSON file not found at {bench_path}.")


if __name__ == "__main__":
    render_app()
