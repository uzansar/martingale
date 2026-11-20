"""Pytest verification suite for Martingale Exchange Polars pipeline and Plotly dashboard."""

from __future__ import annotations

import sys
from pathlib import Path

import polars as pl
import pytest

PYTHON_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = PYTHON_DIR.parent
DATA_DIR = PROJECT_ROOT / "data" / "processed"

if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

from dashboard import create_dashboard_figure, export_dashboard_html  # noqa: E402
from data_pipeline import MartingaleDataPipeline  # noqa: E402


@pytest.fixture(scope="module")
def pipeline() -> MartingaleDataPipeline:
    required = [
        "sim_steps.csv",
        "sim_trades.csv",
        "sim_l2_depth.csv",
        "experiment_sweeps.csv",
        "benchmarks.json",
    ]
    missing = [name for name in required if not (DATA_DIR / name).exists()]
    if missing:
        pytest.fail(f"Missing required exported datasets in {DATA_DIR}: {missing}")
    return MartingaleDataPipeline(data_dir=DATA_DIR)


def test_polars_loads_steps_and_derives_microstructure_invariants(
    pipeline: MartingaleDataPipeline,
) -> None:
    steps = pipeline.load_steps()
    assert steps.height == 500
    for col in ("spread_bps", "microprice", "rolling_l1_obi", "rolling_mid_price", "time_ms"):
        assert col in steps.columns

    valid_bbo = steps.filter(pl.col("best_bid").is_not_null() & pl.col("best_ask").is_not_null())
    assert valid_bbo.height > 0
    assert (valid_bbo["best_ask"] >= valid_bbo["best_bid"]).all()
    assert (steps["l1_obi"] >= -1.0).all() and (steps["l1_obi"] <= 1.0).all()
    assert (valid_bbo["spread_bps"] >= 0.0).all()


def test_polars_loads_trades_and_l2_depth_profile(pipeline: MartingaleDataPipeline) -> None:
    trades = pipeline.load_trades()
    assert trades.height > 1000
    assert "running_vwap" in trades.columns
    assert trades["cum_volume"].is_sorted()
    assert (trades["running_vwap"] > 0.0).all()

    profile = pipeline.summarize_l2_depth_profile()
    assert profile.height == 5
    total_share = profile["share_of_book_liquidity"].sum()
    assert total_share == pytest.approx(1.0, abs=1e-6)


def test_polars_verifies_experiment_sweeps_and_benchmark_speedups(
    pipeline: MartingaleDataPipeline,
) -> None:
    sweeps = pipeline.load_experiment_sweeps()
    inst = sweeps.filter(pl.col("experiment") == "institutional_size").sort("sweep_value")
    assert inst["sweep_value"].to_list() == [100.0, 300.0, 750.0, 1500.0]
    slippages = inst["inst_slippage_bps"].to_list()
    assert all(b > a for a, b in zip(slippages, slippages[1:])), (
        f"Expected monotonic institutional slippage, got {slippages}"
    )

    bench = pipeline.load_benchmarks()
    assert bench.height == 8
    fast = bench.filter(pl.col("variant") == "FlatOrderBook + ObjectPool")
    assert fast.height == 4
    # FlatOrderBook + ObjectPool outperforms STL Baseline across all 4 workloads
    assert (fast["speedup"] > 1.25).all(), f"Unexpected speedups: {fast['speedup'].to_list()}"
    add_cancel_speedup = fast.filter(pl.col("workload") == "AddAndCancel")["speedup"].item()
    assert add_cancel_speedup > 2.5


def test_plotly_dashboard_builds_and_exports_html(
    pipeline: MartingaleDataPipeline, tmp_path: Path
) -> None:
    fig = create_dashboard_figure(pipeline)
    assert len(fig.data) >= 15

    html_out = tmp_path / "martingale_dashboard.html"
    exported = export_dashboard_html(data_dir=DATA_DIR, output_path=html_out)
    assert exported.exists()
    content = exported.read_text(encoding="utf-8")
    assert "Martingale Exchange" in content
    assert len(content) > 10_000
