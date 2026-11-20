"""Interactive 6-panel Plotly research dashboard for Martingale Exchange."""

from __future__ import annotations

import argparse
from pathlib import Path

import plotly.graph_objects as go
from plotly.subplots import make_subplots

from data_pipeline import MartingaleDataPipeline


def create_dashboard_figure(pipeline: MartingaleDataPipeline) -> go.Figure:
    """Builds the multi-panel interactive microstructure and latency dashboard."""
    steps_df = pipeline.load_steps()
    trades_df = pipeline.load_trades()
    l2_profile_df = pipeline.summarize_l2_depth_profile()
    sweeps_df = pipeline.load_experiment_sweeps()
    bench_df = pipeline.load_benchmarks()

    fig = make_subplots(
        rows=3,
        cols=2,
        subplot_titles=(
            "1. BBO Quotes, Mid-Price, Running VWAP & Aggressor Tape",
            "2. Realized Spread (bps) & Rolling Level-1 Order Book Imbalance",
            "3. Mean Level-2 Resting Liquidity Profile (BUY vs. SELL)",
            "4. Institutional Order Size vs. Slippage & Impact Decomposition",
            "5. Momentum Participation vs. Volatility & Spread Expansion",
            "6. Matching Engine Speedup & Tail-Latency Percentiles (P50–P99.9)",
        ),
        specs=[
            [{"secondary_y": False}, {"secondary_y": True}],
            [{"secondary_y": False}, {"secondary_y": False}],
            [{"secondary_y": True}, {"secondary_y": True}],
        ],
        vertical_spacing=0.11,
        horizontal_spacing=0.09,
    )

    # Panel 1: BBO envelope, mid-price, VWAP, and trade tape markers
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["best_ask"].to_list(),
            name="Best Ask",
            line=dict(color="rgba(239, 83, 80, 0.45)", width=1),
            mode="lines",
        ),
        row=1,
        col=1,
    )
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["best_bid"].to_list(),
            name="Best Bid",
            line=dict(color="rgba(38, 166, 154, 0.45)", width=1),
            fill="tonexty",
            fillcolor="rgba(100, 181, 246, 0.08)",
            mode="lines",
        ),
        row=1,
        col=1,
    )
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["mid_price"].to_list(),
            name="Mid-Price",
            line=dict(color="#64b5f6", width=2),
            mode="lines",
        ),
        row=1,
        col=1,
    )
    fig.add_trace(
        go.Scatter(
            x=trades_df["time_ms"].to_list(),
            y=trades_df["running_vwap"].to_list(),
            name="Running VWAP",
            line=dict(color="#ffd54f", width=2, dash="dot"),
            mode="lines",
        ),
        row=1,
        col=1,
    )
    buy_trades = trades_df.filter(trades_df["aggressor_side"] == "BUY")
    sell_trades = trades_df.filter(trades_df["aggressor_side"] == "SELL")
    fig.add_trace(
        go.Scatter(
            x=buy_trades["time_ms"].to_list(),
            y=buy_trades["price"].to_list(),
            name="Buy Aggressor Prints",
            mode="markers",
            marker=dict(color="#00e676", size=4, opacity=0.45),
        ),
        row=1,
        col=1,
    )
    fig.add_trace(
        go.Scatter(
            x=sell_trades["time_ms"].to_list(),
            y=sell_trades["price"].to_list(),
            name="Sell Aggressor Prints",
            mode="markers",
            marker=dict(color="#ff5252", size=4, opacity=0.45),
        ),
        row=1,
        col=1,
    )

    # Panel 2: Spread (bps) and Rolling L1 OBI
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["spread_bps"].to_list(),
            name="Spread (bps)",
            line=dict(color="#ab47bc", width=1.6),
            mode="lines",
        ),
        row=1,
        col=2,
        secondary_y=False,
    )
    fig.add_trace(
        go.Scatter(
            x=steps_df["time_ms"].to_list(),
            y=steps_df["rolling_l1_obi"].to_list(),
            name="20-Step Rolling L1 OBI",
            line=dict(color="#26c6da", width=2),
            mode="lines",
        ),
        row=1,
        col=2,
        secondary_y=True,
    )

    # Panel 3: L2 Resting Depth Profile (levels 0..4)
    level_labels = [f"L{lvl + 1}" for lvl in l2_profile_df["level"].to_list()]
    fig.add_trace(
        go.Bar(
            x=level_labels,
            y=l2_profile_df["mean_bid_qty"].to_list(),
            name="Mean Bid Depth",
            marker_color="#26a69a",
        ),
        row=2,
        col=1,
    )
    fig.add_trace(
        go.Bar(
            x=level_labels,
            y=l2_profile_df["mean_ask_qty"].to_list(),
            name="Mean Ask Depth",
            marker_color="#ef5350",
        ),
        row=2,
        col=1,
    )

    # Panel 4: Sweep 1 - Institutional Size vs Slippage & Impact
    inst_df = sweeps_df.filter(sweeps_df["experiment"] == "institutional_size").sort("sweep_value")
    inst_x = inst_df["sweep_value"].to_list()
    fig.add_trace(
        go.Scatter(
            x=inst_x,
            y=inst_df["inst_slippage_bps"].to_list(),
            name="Total Slippage (bps)",
            mode="lines+markers",
            line=dict(color="#ffd54f", width=2.5),
        ),
        row=2,
        col=2,
    )
    fig.add_trace(
        go.Bar(
            x=inst_x,
            y=inst_df["inst_permanent_impact_bps"].to_list(),
            name="Permanent Impact (bps)",
            marker_color="#42a5f5",
            opacity=0.75,
        ),
        row=2,
        col=2,
    )
    fig.add_trace(
        go.Bar(
            x=inst_x,
            y=inst_df["inst_temporary_impact_bps"].to_list(),
            name="Temporary Impact (bps)",
            marker_color="#ffa726",
            opacity=0.75,
        ),
        row=2,
        col=2,
    )

    # Panel 5: Sweep 3 - Momentum Participation vs Realized Volatility & Spread
    mom_df = sweeps_df.filter(sweeps_df["experiment"] == "momentum_traders").sort("sweep_value")
    mom_x = mom_df["sweep_value"].to_list()
    fig.add_trace(
        go.Scatter(
            x=mom_x,
            y=mom_df["realized_volatility_bps"].to_list(),
            name="Realized Vol (bps)",
            mode="lines+markers",
            line=dict(color="#ff7043", width=2.5),
        ),
        row=3,
        col=1,
        secondary_y=False,
    )
    fig.add_trace(
        go.Scatter(
            x=mom_x,
            y=mom_df["mean_spread_bps"].to_list(),
            name="Effective Spread (bps)",
            mode="lines+markers",
            line=dict(color="#9ccc65", width=2.5, dash="dash"),
        ),
        row=3,
        col=1,
        secondary_y=True,
    )

    # Panel 6: STL Baseline vs FlatOrderBook + ObjectPool Latency & Speedup
    stl_df = bench_df.filter(bench_df["variant"] == "STL Baseline")
    fast_df = bench_df.filter(bench_df["variant"] == "FlatOrderBook + ObjectPool")
    workloads = stl_df["workload"].to_list()

    fig.add_trace(
        go.Bar(
            x=workloads,
            y=stl_df["P50_ns"].to_list(),
            name="STL P50 (ns)",
            marker_color="#78909c",
        ),
        row=3,
        col=2,
        secondary_y=False,
    )
    fig.add_trace(
        go.Bar(
            x=workloads,
            y=fast_df["P50_ns"].to_list(),
            name="FlatOrderBook P50 (ns)",
            marker_color="#26c6da",
        ),
        row=3,
        col=2,
        secondary_y=False,
    )
    fig.add_trace(
        go.Bar(
            x=workloads,
            y=fast_df["P99_ns"].to_list(),
            name="FlatOrderBook P99 (ns)",
            marker_color="#00acc1",
            opacity=0.65,
        ),
        row=3,
        col=2,
        secondary_y=False,
    )
    fig.add_trace(
        go.Scatter(
            x=workloads,
            y=fast_df["speedup"].to_list(),
            name="Throughput Speedup (x)",
            mode="lines+markers+text",
            text=[f"{val:.2f}x" for val in fast_df["speedup"].to_list()],
            textposition="top center",
            line=dict(color="#ffd54f", width=2.5),
        ),
        row=3,
        col=2,
        secondary_y=True,
    )

    fig.update_layout(
        template="plotly_dark",
        title=dict(
            text="Martingale Exchange — Quantitative Microstructure & Engine Latency Dashboard",
            x=0.5,
            font=dict(size=20),
        ),
        height=1180,
        width=1550,
        barmode="group",
        legend=dict(orientation="h", yanchor="top", y=-0.05, x=0.0),
        margin=dict(l=60, r=60, t=90, b=130),
    )

    fig.update_xaxes(title_text="Time (ms)", row=1, col=1)
    fig.update_yaxes(title_text="Price (ticks)", row=1, col=1)
    fig.update_xaxes(title_text="Time (ms)", row=1, col=2)
    fig.update_yaxes(title_text="Spread (bps)", row=1, col=2, secondary_y=False)
    fig.update_yaxes(title_text="Rolling L1 OBI", range=[-1.0, 1.0], row=1, col=2, secondary_y=True)
    fig.update_xaxes(title_text="Order Book Level", row=2, col=1)
    fig.update_yaxes(title_text="Mean Resting Quantity", row=2, col=1)
    fig.update_xaxes(title_text="Parent Order Quantity (shares)", row=2, col=2)
    fig.update_yaxes(title_text="Impact / Slippage (bps)", row=2, col=2)
    fig.update_xaxes(title_text="Momentum Traders (count)", row=3, col=1)
    fig.update_yaxes(title_text="Realized Volatility (bps)", row=3, col=1, secondary_y=False)
    fig.update_yaxes(title_text="Mean Spread (bps)", row=3, col=1, secondary_y=True)
    fig.update_xaxes(title_text="Benchmark Workload", row=3, col=2)
    fig.update_yaxes(title_text="Latency (ns)", row=3, col=2, secondary_y=False)
    fig.update_yaxes(title_text="Throughput Speedup (x)", row=3, col=2, secondary_y=True)

    return fig


def export_dashboard_html(data_dir: Path, output_path: Path) -> Path:
    """Builds and writes the standalone interactive Plotly dashboard HTML file."""
    pipeline = MartingaleDataPipeline(data_dir=data_dir)
    fig = create_dashboard_figure(pipeline)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.write_html(str(output_path), include_plotlyjs="cdn")

    sweeps = pipeline.load_experiment_sweeps()
    bench = pipeline.load_benchmarks().filter(
        pipeline.load_benchmarks()["variant"] == "FlatOrderBook + ObjectPool"
    )
    print(f"Exported dashboard HTML -> {output_path}")
    print("\n--- Polars Experiment Summary Rows ---")
    print(sweeps.select(["experiment", "sweep_value", "mean_spread_bps", "realized_volatility_bps", "inst_slippage_bps"]))
    print("\n--- Polars FastMatchingEngine Speedups ---")
    print(bench.select(["workload", "throughput_mops", "P50_ns", "P99_ns", "speedup"]))
    return output_path


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build interactive Martingale Exchange Plotly research dashboard."
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path("data/processed"),
        help="Directory containing exported simulation CSVs and benchmarks.json.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("data/processed/martingale_dashboard.html"),
        help="Destination HTML path for the standalone interactive dashboard.",
    )
    args = parser.parse_args()
    export_dashboard_html(data_dir=args.data_dir, output_path=args.output)


if __name__ == "__main__":
    main()
