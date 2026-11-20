"""Polars quantitative data ingestion and feature pipeline for Martingale Exchange."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import polars as pl


_WORKLOAD_LABELS: dict[str, tuple[str, str]] = {
    "BM_OrderBook_AddAndCancel": ("AddAndCancel", "STL Baseline"),
    "BM_FlatOrderBook_AddAndCancel": ("AddAndCancel", "FlatOrderBook + ObjectPool"),
    "BM_MatchingEngine_RestingLimitOrders": ("RestingLimitOrders", "STL Baseline"),
    "BM_FastMatchingEngine_RestingLimitOrders": (
        "RestingLimitOrders",
        "FlatOrderBook + ObjectPool",
    ),
    "BM_MatchingEngine_CrossingLimitOrders": ("CrossingLimitOrders", "STL Baseline"),
    "BM_FastMatchingEngine_CrossingLimitOrders": (
        "CrossingLimitOrders",
        "FlatOrderBook + ObjectPool",
    ),
    "BM_MatchingEngine_MixedWorkload_10k": ("MixedWorkload_10k", "STL Baseline"),
    "BM_FastMatchingEngine_MixedWorkload_10k": (
        "MixedWorkload_10k",
        "FlatOrderBook + ObjectPool",
    ),
}


def _rolling_mean_expr(column: str, window_size: int) -> pl.Expr:
    """Computes rolling mean across Polars 1.x versions."""
    expr = pl.col(column)
    try:
        return expr.rolling_mean(window_size=window_size, min_samples=1)
    except TypeError:  # pragma: no cover - fallback for Polars < 1.21
        return expr.rolling_mean(window_size=window_size, min_periods=1)


@dataclass(frozen=True)
class MartingaleDataPipeline:
    """Loads exported C++20 simulation CSVs and benchmark JSON using Polars."""

    data_dir: Path

    def __post_init__(self) -> None:
        object.__setattr__(self, "data_dir", Path(self.data_dir))

    def load_steps(self) -> pl.DataFrame:
        """Loads step telemetry and computes spread_bps, microprice, and rolling features."""
        df = pl.read_csv(self.data_dir / "sim_steps.csv", null_values=[""])
        qty_sum = (pl.col("best_bid_qty") + pl.col("best_ask_qty")).cast(pl.Float64)

        return (
            df.with_columns(
                pl.when(pl.col("mid_price") > 0.0)
                .then((pl.col("spread_ticks") / pl.col("mid_price")) * 10_000.0)
                .otherwise(None)
                .alias("spread_bps"),
                pl.when(qty_sum > 0.0)
                .then(
                    (
                        pl.col("best_bid").cast(pl.Float64)
                        * pl.col("best_ask_qty").cast(pl.Float64)
                        + pl.col("best_ask").cast(pl.Float64)
                        * pl.col("best_bid_qty").cast(pl.Float64)
                    )
                    / qty_sum
                )
                .otherwise(pl.col("mid_price"))
                .alias("microprice"),
                (pl.col("timestamp_ns").cast(pl.Float64) / 1e6).alias("time_ms"),
            )
            .with_columns(
                _rolling_mean_expr("l1_obi", 20).alias("rolling_l1_obi"),
                _rolling_mean_expr("mid_price", 20).alias("rolling_mid_price"),
            )
            .sort("step_index")
        )

    def load_trades(self) -> pl.DataFrame:
        """Loads execution tape and computes cumulative volume and running VWAP."""
        df = pl.read_csv(self.data_dir / "sim_trades.csv").sort("sequence_number")
        return (
            df.with_columns(
                (pl.col("price").cast(pl.Float64) * pl.col("quantity").cast(pl.Float64))
                .alias("notional"),
                (pl.col("timestamp_ns").cast(pl.Float64) / 1e6).alias("time_ms"),
            )
            .with_columns(
                pl.col("notional").cum_sum().alias("cum_notional"),
                pl.col("quantity").cast(pl.Float64).cum_sum().alias("cum_volume"),
            )
            .with_columns(
                pl.when(pl.col("cum_volume") > 0.0)
                .then(pl.col("cum_notional") / pl.col("cum_volume"))
                .otherwise(None)
                .alias("running_vwap")
            )
        )

    def load_l2_depth(self) -> pl.DataFrame:
        """Loads tidy long-format L2 order book depth snapshots."""
        return pl.read_csv(self.data_dir / "sim_l2_depth.csv").with_columns(
            (pl.col("timestamp_ns").cast(pl.Float64) / 1e6).alias("time_ms")
        )

    def summarize_l2_depth_profile(self, l2_df: pl.DataFrame | None = None) -> pl.DataFrame:
        """Aggregates mean resting liquidity and book share by depth level and side."""
        df = self.load_l2_depth() if l2_df is None else l2_df
        num_snapshots = max(df.select(pl.col("md_sequence").n_unique()).item(), 1)

        by_side_level = (
            df.group_by(["level", "side"])
            .agg(
                (pl.col("quantity").cast(pl.Float64).sum() / float(num_snapshots)).alias(
                    "mean_qty"
                ),
                (pl.col("order_count").cast(pl.Float64).sum() / float(num_snapshots)).alias(
                    "mean_orders"
                ),
            )
        )

        bids = (
            by_side_level.filter(pl.col("side") == "BUY")
            .select(
                pl.col("level"),
                pl.col("mean_qty").alias("mean_bid_qty"),
                pl.col("mean_orders").alias("mean_bid_orders"),
            )
        )
        asks = (
            by_side_level.filter(pl.col("side") == "SELL")
            .select(
                pl.col("level"),
                pl.col("mean_qty").alias("mean_ask_qty"),
                pl.col("mean_orders").alias("mean_ask_orders"),
            )
        )

        levels = (
            df.select("level")
            .unique()
            .sort("level")
            .join(bids, on="level", how="left")
            .join(asks, on="level", how="left")
            .with_columns(
                pl.col("mean_bid_qty").fill_null(0.0),
                pl.col("mean_ask_qty").fill_null(0.0),
                pl.col("mean_bid_orders").fill_null(0.0),
                pl.col("mean_ask_orders").fill_null(0.0),
            )
            .with_columns(
                (pl.col("mean_bid_qty") + pl.col("mean_ask_qty")).alias("mean_total_qty")
            )
        )
        grand_total = levels.select(pl.col("mean_total_qty").sum()).item() or 1.0
        return levels.with_columns(
            (pl.col("mean_total_qty") / float(grand_total)).alias("share_of_book_liquidity")
        )

    def load_experiment_sweeps(self) -> pl.DataFrame:
        """Loads Milestone 5 quantitative parameter sweep summaries."""
        return pl.read_csv(self.data_dir / "experiment_sweeps.csv")

    def load_benchmarks(self) -> pl.DataFrame:
        """Parses Google Benchmark JSON telemetry and calculates throughput speedup ratios."""
        raw = json.loads((self.data_dir / "benchmarks.json").read_text(encoding="utf-8"))
        rows: list[dict[str, object]] = []
        for entry in raw.get("benchmarks", []):
            name = str(entry["name"])
            workload, variant = _WORKLOAD_LABELS.get(name, (name, "Unknown"))
            throughput_mops = float(entry.get("items_per_second", 0.0)) / 1e6
            rows.append(
                {
                    "name": name,
                    "workload": workload,
                    "variant": variant,
                    "iterations": int(entry.get("iterations", 0)),
                    "throughput_mops": throughput_mops,
                    "Mean_ns": float(entry.get("Mean_ns", 0.0)),
                    "P50_ns": float(entry.get("P50_ns", 0.0)),
                    "P90_ns": float(entry.get("P90_ns", 0.0)),
                    "P95_ns": float(entry.get("P95_ns", 0.0)),
                    "P99_ns": float(entry.get("P99_ns", 0.0)),
                    "P99_9_ns": float(entry.get("P99_9_ns", 0.0)),
                    "Max_ns": float(entry.get("Max_ns", 0.0)),
                }
            )

        df = pl.DataFrame(rows)
        baseline = (
            df.filter(pl.col("variant") == "STL Baseline")
            .select(
                pl.col("workload"),
                pl.col("throughput_mops").alias("baseline_mops"),
            )
        )
        return df.join(baseline, on="workload", how="left").with_columns(
            pl.when(pl.col("baseline_mops") > 0.0)
            .then(pl.col("throughput_mops") / pl.col("baseline_mops"))
            .otherwise(1.0)
            .alias("speedup")
        )
