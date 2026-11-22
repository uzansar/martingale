#include <martingale/research/microstructure_metrics.hpp>
#include <martingale/simulator/market_simulator.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace martingale;

struct CliOptions {
    std::filesystem::path export_dir{};
    bool single_run{false};
    std::uint64_t seed{2026ULL};
    std::size_t total_steps{500U};
    Price mm_half_spread_ticks{2};
    Price mm_inventory_skew_divisor{50};
    std::size_t num_random_traders{5U};
    std::size_t num_momentum_traders{2U};
    Side inst_side{Side::Buy};
    Quantity inst_total_qty{500U};
    Quantity inst_child_slice{50U};
};

SimulationConfig make_baseline_config() {
    SimulationConfig cfg;
    cfg.seed = 2026ULL;
    cfg.instrument_id = 1U;
    cfg.total_steps = 500U;
    cfg.snapshot_interval = 10U;
    cfg.snapshot_depth = 5U;
    cfg.initial_mid_price = 10000;
    cfg.step_duration_ns = 1'000'000ULL;
    cfg.fundamental_step_sigma_ticks = 2;
    cfg.market_maker_config.half_spread_ticks = 2;
    cfg.market_maker_config.num_levels = 5U;
    cfg.market_maker_config.base_quantity = 60U;
    cfg.market_maker_config.level_quantity_step = 40U;
    cfg.market_maker_config.inventory_skew_divisor = 50;
    cfg.num_random_traders = 5U;
    cfg.num_momentum_traders = 2U;
    cfg.include_institutional_trader = true;
    cfg.institutional_config.side = Side::Buy;
    cfg.institutional_config.total_parent_quantity = 500U;
    cfg.institutional_config.child_slice_quantity = 50U;
    cfg.institutional_config.start_step = 50U;
    cfg.institutional_config.execution_interval_steps = 5U;
    return cfg;
}

template <typename IntType>
IntType parse_integer_arg(std::string_view flag, std::string_view value_str) {
    IntType value{};
    const char* begin = value_str.data();
    const char* end = begin + value_str.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument("Invalid integer value for " + std::string{flag} + ": " +
                                    std::string{value_str});
    }
    return value;
}

std::string_view next_arg_value(std::span<const char* const> args, std::size_t& idx,
                                std::string_view flag) {
    if (idx + 1U >= args.size()) {
        throw std::invalid_argument(std::string{flag} + " requires a value argument");
    }
    return std::string_view{args[++idx]};
}

Side parse_side_arg(std::string_view side_str) {
    if (side_str == "BUY" || side_str == "buy") {
        return Side::Buy;
    }
    if (side_str == "SELL" || side_str == "sell") {
        return Side::Sell;
    }
    throw std::invalid_argument("Invalid --inst-side (expected BUY or SELL): " +
                                std::string{side_str});
}

void print_usage() {
    std::cout
        << "Usage: martingale_experiments [options]\n"
        << "Options:\n"
        << "  --export-dir <directory>      Directory for exported CSV datasets\n"
        << "  --single-run                  Execute a single configurable simulation run\n"
        << "  --seed <uint64>               PRNG seed (default: 2026)\n"
        << "  --steps <1..5000>             Simulation steps (default: 500)\n"
        << "  --mm-half-spread <int64>      Market maker half-spread ticks >= 1 (default: 2)\n"
        << "  --mm-skew-divisor <int64>     Market maker inventory skew divisor >= 1 (default: 50)\n"
        << "  --random-traders <0..50>      Random/noise trader count (default: 5)\n"
        << "  --momentum-traders <0..50>    Momentum trader count (default: 2)\n"
        << "  --inst-side <BUY|SELL>        Institutional parent order side (default: BUY)\n"
        << "  --inst-qty <uint64>           Institutional total quantity, 0 disables (default: 500)\n"
        << "  --inst-slice <uint64>         Institutional child slice size >= 1 (default: 50)\n";
}

CliOptions parse_cli_options(std::span<const char* const> args) {
    CliOptions opts;
    for (std::size_t i = 1U; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--export-dir") {
            opts.export_dir = next_arg_value(args, i, arg);
        } else if (arg == "--single-run") {
            opts.single_run = true;
        } else if (arg == "--seed") {
            opts.seed = parse_integer_arg<std::uint64_t>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--steps") {
            opts.total_steps = parse_integer_arg<std::size_t>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--mm-half-spread") {
            opts.mm_half_spread_ticks = parse_integer_arg<Price>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--mm-skew-divisor") {
            opts.mm_inventory_skew_divisor =
                parse_integer_arg<Price>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--random-traders") {
            opts.num_random_traders =
                parse_integer_arg<std::size_t>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--momentum-traders") {
            opts.num_momentum_traders =
                parse_integer_arg<std::size_t>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--inst-side") {
            opts.inst_side = parse_side_arg(next_arg_value(args, i, arg));
        } else if (arg == "--inst-qty") {
            opts.inst_total_qty = parse_integer_arg<Quantity>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--inst-slice") {
            opts.inst_child_slice = parse_integer_arg<Quantity>(arg, next_arg_value(args, i, arg));
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown argument: " + std::string{arg});
        }
    }

    if (opts.total_steps < 1U || opts.total_steps > 5000U) {
        throw std::invalid_argument("--steps must be between 1 and 5000");
    }
    if (opts.mm_half_spread_ticks < 1) {
        throw std::invalid_argument("--mm-half-spread must be >= 1");
    }
    if (opts.mm_inventory_skew_divisor < 1) {
        throw std::invalid_argument("--mm-skew-divisor must be >= 1");
    }
    if (opts.num_random_traders > 50U) {
        throw std::invalid_argument("--random-traders must be <= 50");
    }
    if (opts.num_momentum_traders > 50U) {
        throw std::invalid_argument("--momentum-traders must be <= 50");
    }
    if (opts.inst_total_qty > 0U && opts.inst_child_slice < 1U) {
        throw std::invalid_argument("--inst-slice must be >= 1 when --inst-qty > 0");
    }
    return opts;
}

SimulationConfig make_single_run_config(const CliOptions& opts) {
    SimulationConfig cfg = make_baseline_config();
    cfg.seed = opts.seed;
    cfg.total_steps = opts.total_steps;
    cfg.market_maker_config.half_spread_ticks = opts.mm_half_spread_ticks;
    cfg.market_maker_config.inventory_skew_divisor = opts.mm_inventory_skew_divisor;
    cfg.num_random_traders = opts.num_random_traders;
    cfg.num_momentum_traders = opts.num_momentum_traders;

    if (opts.inst_total_qty == 0U) {
        cfg.include_institutional_trader = false;
    } else {
        cfg.include_institutional_trader = true;
        cfg.institutional_config.side = opts.inst_side;
        cfg.institutional_config.total_parent_quantity = opts.inst_total_qty;
        cfg.institutional_config.child_slice_quantity =
            std::min(opts.inst_child_slice, opts.inst_total_qty);
        cfg.institutional_config.start_step = std::min<std::size_t>(50U, opts.total_steps / 5U);
    }
    return cfg;
}

void write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to open output file: " + path.string());
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) {
        throw std::runtime_error("Failed while writing output file: " + path.string());
    }
}

void export_telemetry_csvs(const std::filesystem::path& export_dir,
                           const SimulationResult& result) {
    write_text_file(export_dir / "sim_steps.csv", MicrostructureAnalyzer::steps_to_csv(result));
    write_text_file(export_dir / "sim_trades.csv",
                    MicrostructureAnalyzer::trades_to_csv(result.trades));
    write_text_file(export_dir / "sim_l2_depth.csv",
                    MicrostructureAnalyzer::l2_snapshots_to_csv(result.l2_snapshots));
}

void print_sweep_header(std::string_view title) {
    std::cout << "\n=== " << title << " ===\n";
    std::cout << std::left << std::setw(22) << "Experiment" << std::right << std::setw(10)
              << "Param" << std::setw(9) << "Trades" << std::setw(10) << "Volume"
              << std::setw(13) << "Spread(bps)" << std::setw(12) << "RVol(bps)"
              << std::setw(10) << "L1_OBI" << std::setw(15) << "Slippage(bps)"
              << std::setw(12) << "Perm(bps)" << std::setw(12) << "Temp(bps)" << '\n';
    std::cout << std::string(125U, '-') << '\n';
}

void print_sweep_row(std::string_view name, double param_val, const MicrostructureSummary& s) {
    const double slip = s.institutional_impact.has_value() ? s.institutional_impact->slippage_bps : 0.0;
    const double perm =
        s.institutional_impact.has_value() ? s.institutional_impact->permanent_impact_bps : 0.0;
    const double temp =
        s.institutional_impact.has_value() ? s.institutional_impact->temporary_impact_bps : 0.0;

    std::cout << std::left << std::setw(22) << name << std::right << std::fixed
              << std::setprecision(2) << std::setw(10) << param_val << std::setw(9)
              << s.trade_intensity.trade_count << std::setw(10) << s.trade_intensity.total_volume
              << std::setw(13) << s.mean_spread_bps << std::setw(12) << s.realized_volatility_bps
              << std::setprecision(4) << std::setw(10) << s.mean_l1_imbalance
              << std::setprecision(2) << std::setw(15) << slip << std::setw(12) << perm
              << std::setw(12) << temp << '\n';
}

template <typename ConfigureFn>
void execute_sweep(std::string_view title, std::string_view experiment_name,
                   std::string_view param_name, std::span<const double> values,
                   std::ostringstream& sweep_csv, ConfigureFn&& configure) {
    print_sweep_header(title);
    for (const double value : values) {
        SimulationConfig cfg = make_baseline_config();
        configure(cfg, value);
        const SimulationResult result = MarketSimulator(cfg).run();
        const MicrostructureSummary summary = MicrostructureAnalyzer::analyze(result);
        print_sweep_row(experiment_name, value, summary);
        sweep_csv << MicrostructureAnalyzer::summary_to_csv_row(experiment_name, param_name, value,
                                                                summary);
    }
}

void run_single_simulation(const CliOptions& opts) {
    const SimulationConfig cfg = make_single_run_config(opts);
    const SimulationResult result = MarketSimulator(cfg).run();
    const MicrostructureSummary summary = MicrostructureAnalyzer::analyze(result);

    print_sweep_header("Single Simulation Run");
    print_sweep_row("single_run", static_cast<double>(cfg.seed), summary);

    if (!opts.export_dir.empty()) {
        std::filesystem::create_directories(opts.export_dir);
        export_telemetry_csvs(opts.export_dir, result);

        std::ostringstream summary_csv;
        summary_csv << MicrostructureAnalyzer::summary_to_csv_header();
        summary_csv << MicrostructureAnalyzer::summary_to_csv_row(
            "single_run", "seed", static_cast<double>(cfg.seed), summary);
        write_text_file(opts.export_dir / "sim_summary.csv", summary_csv.str());

        std::cout << "\nExported single-run CSV datasets to: " << opts.export_dir.string() << '\n';
    }
}

void run_full_sweeps(const std::filesystem::path& export_dir) {
    std::ostringstream sweep_csv;
    sweep_csv << MicrostructureAnalyzer::summary_to_csv_header();

    // Sweep 1: Institutional parent order size (holding participation window proportional)
    constexpr std::array<double, 4> kInstitutionalSizes{100.0, 300.0, 750.0, 1500.0};
    execute_sweep("Sweep 1: Institutional Order Size (Buy Parent Order)", "institutional_size",
                  "total_parent_quantity", kInstitutionalSizes, sweep_csv,
                  [](SimulationConfig& cfg, double qty_val) {
                      const auto total_qty = static_cast<Quantity>(qty_val);
                      cfg.institutional_config.total_parent_quantity = total_qty;
                      cfg.institutional_config.child_slice_quantity =
                          std::max<Quantity>(10U, total_qty / 10U);
                  });

    // Sweep 2: Market maker quoted half-spread sensitivity
    constexpr std::array<double, 3> kHalfSpreads{1.0, 2.0, 4.0};
    execute_sweep("Sweep 2: Market Maker Half-Spread Sensitivity", "market_maker_spread",
                  "half_spread_ticks", kHalfSpreads, sweep_csv,
                  [](SimulationConfig& cfg, double half_spread_val) {
                      cfg.market_maker_config.half_spread_ticks =
                          static_cast<Price>(half_spread_val);
                  });

    // Sweep 3: Momentum trader participation impact on volatility and flow
    constexpr std::array<double, 4> kMomentumCounts{0.0, 1.0, 3.0, 5.0};
    execute_sweep("Sweep 3: Momentum Trader Participation", "momentum_traders",
                  "num_momentum_traders", kMomentumCounts, sweep_csv,
                  [](SimulationConfig& cfg, double count_val) {
                      cfg.num_momentum_traders = static_cast<std::size_t>(count_val);
                  });

    if (!export_dir.empty()) {
        std::filesystem::create_directories(export_dir);

        // Baseline high-resolution simulation for time-series and L2 heatmap CSV exports
        const SimulationConfig baseline_cfg = make_baseline_config();
        const SimulationResult baseline_result = MarketSimulator(baseline_cfg).run();

        export_telemetry_csvs(export_dir, baseline_result);
        write_text_file(export_dir / "experiment_sweeps.csv", sweep_csv.str());

        std::cout << "\nExported CSV datasets to: " << export_dir.string() << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
        const CliOptions opts = parse_cli_options(args);

        if (opts.single_run) {
            run_single_simulation(opts);
        } else {
            run_full_sweeps(opts.export_dir);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
