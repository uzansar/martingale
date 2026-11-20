#include <martingale/research/microstructure_metrics.hpp>
#include <martingale/simulator/market_simulator.hpp>

#include <algorithm>
#include <array>
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

std::filesystem::path parse_export_dir(std::span<const char* const> args) {
    std::filesystem::path export_dir;
    for (std::size_t i = 1U; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--export-dir") {
            if (i + 1U >= args.size()) {
                throw std::invalid_argument("--export-dir requires a directory path argument");
            }
            export_dir = args[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: martingale_experiments [--export-dir <directory>]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown argument: " + std::string{arg});
        }
    }
    return export_dir;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
        const std::filesystem::path export_dir = parse_export_dir(args);

        std::ostringstream sweep_csv;
        sweep_csv << MicrostructureAnalyzer::summary_to_csv_header();

        // Sweep 1: Institutional parent order size (holding participation window proportional)
        constexpr std::array<double, 4> kInstitutionalSizes{100.0, 300.0, 750.0, 1500.0};
        execute_sweep("Sweep 1: Institutional Order Size (Buy Parent Order)",
                      "institutional_size", "total_parent_quantity", kInstitutionalSizes,
                      sweep_csv, [](SimulationConfig& cfg, double qty_val) {
                          const auto total_qty = static_cast<martingale::Quantity>(qty_val);
                          cfg.institutional_config.total_parent_quantity = total_qty;
                          cfg.institutional_config.child_slice_quantity =
                              std::max<martingale::Quantity>(10U, total_qty / 10U);
                      });

        // Sweep 2: Market maker quoted half-spread sensitivity
        constexpr std::array<double, 3> kHalfSpreads{1.0, 2.0, 4.0};
        execute_sweep("Sweep 2: Market Maker Half-Spread Sensitivity",
                      "market_maker_spread", "half_spread_ticks", kHalfSpreads, sweep_csv,
                      [](SimulationConfig& cfg, double half_spread_val) {
                          cfg.market_maker_config.half_spread_ticks =
                              static_cast<martingale::Price>(half_spread_val);
                      });

        // Sweep 3: Momentum trader participation impact on volatility and flow
        constexpr std::array<double, 4> kMomentumCounts{0.0, 1.0, 3.0, 5.0};
        execute_sweep("Sweep 3: Momentum Trader Participation",
                      "momentum_traders", "num_momentum_traders", kMomentumCounts, sweep_csv,
                      [](SimulationConfig& cfg, double count_val) {
                          cfg.num_momentum_traders = static_cast<std::size_t>(count_val);
                      });

        if (!export_dir.empty()) {
            std::filesystem::create_directories(export_dir);

            // Baseline high-resolution simulation for time-series and L2 heatmap CSV exports
            const SimulationConfig baseline_cfg = make_baseline_config();
            const SimulationResult baseline_result =
                martingale::MarketSimulator(baseline_cfg).run();

            write_text_file(export_dir / "sim_steps.csv",
                            martingale::MicrostructureAnalyzer::steps_to_csv(baseline_result));
            write_text_file(
                export_dir / "sim_trades.csv",
                martingale::MicrostructureAnalyzer::trades_to_csv(baseline_result.trades));
            write_text_file(export_dir / "sim_l2_depth.csv",
                            martingale::MicrostructureAnalyzer::l2_snapshots_to_csv(
                                baseline_result.l2_snapshots));
            write_text_file(export_dir / "experiment_sweeps.csv", sweep_csv.str());

            std::cout << "\nExported CSV datasets to: " << export_dir.string() << '\n';
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
