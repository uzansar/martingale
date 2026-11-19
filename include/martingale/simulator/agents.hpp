#ifndef MARTINGALE_SIMULATOR_AGENTS_HPP
#define MARTINGALE_SIMULATOR_AGENTS_HPP

#include <martingale/core/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <vector>

namespace martingale {

/// Categories of synthetic market participants modeled in the exchange simulator.
enum class AgentType : std::uint8_t {
    MarketMaker = 0,
    RandomTrader = 1,
    MomentumTrader = 2,
    InstitutionalTrader = 3,
};

/// Read-only market state visible to an agent on each simulation step.
struct SimulationContext {
    std::size_t step_index{0};
    InstrumentId instrument_id{1};
    Timestamp current_time{0};
    Price reference_mid_price{0};
    std::optional<Price> best_bid{};
    Quantity best_bid_quantity{0};
    std::optional<Price> best_ask{};
    Quantity best_ask_quantity{0};
};

/// Command interface used by trading agents to submit or cancel orders.
class OrderSubmissionSink {
public:
    virtual ~OrderSubmissionSink() = default;

    virtual OrderId submit_limit(std::size_t agent_index, Side side, Price price,
                                 Quantity quantity) = 0;
    virtual OrderId submit_market(std::size_t agent_index, Side side, Quantity quantity) = 0;
    virtual void cancel(std::size_t agent_index, OrderId order_id) = 0;
};

/// Per-agent execution and inventory accounting tracked during simulation.
struct AgentMetrics {
    std::size_t agent_index{0};
    AgentType type{AgentType::RandomTrader};
    std::int64_t inventory{0};
    Quantity traded_volume{0};
    std::int64_t cash_flow_ticks{0};
    std::uint64_t orders_submitted{0};
    std::uint64_t cancels_submitted{0};
    std::uint64_t fills_count{0};

    [[nodiscard]] constexpr bool operator==(const AgentMetrics&) const noexcept = default;
};

/// Abstract base class for deterministic synthetic market participants.
class TradingAgent {
public:
    TradingAgent(std::size_t agent_index, AgentType type) noexcept
        : metrics_{agent_index, type} {}

    virtual ~TradingAgent() = default;

    TradingAgent(const TradingAgent&) = delete;
    TradingAgent& operator=(const TradingAgent&) = delete;

    virtual void on_step(const SimulationContext& ctx, OrderSubmissionSink& sink) = 0;

    virtual void on_fill(Side side, Price price, Quantity quantity) {
        const auto signed_qty = static_cast<std::int64_t>(quantity);
        const auto signed_notional = static_cast<std::int64_t>(price) * signed_qty;
        if (side == Side::Buy) {
            metrics_.inventory += signed_qty;
            metrics_.cash_flow_ticks -= signed_notional;
        } else {
            metrics_.inventory -= signed_qty;
            metrics_.cash_flow_ticks += signed_notional;
        }
        metrics_.traded_volume += quantity;
        ++metrics_.fills_count;
    }

    void record_order_submitted() noexcept { ++metrics_.orders_submitted; }
    void record_cancel_submitted() noexcept { ++metrics_.cancels_submitted; }

    [[nodiscard]] const AgentMetrics& metrics() const noexcept { return metrics_; }

protected:
    AgentMetrics metrics_;
};

/// Configuration parameters for MarketMakerAgent.
struct MarketMakerConfig {
    Price half_spread_ticks{2};
    std::size_t num_levels{3};
    Quantity base_quantity{50};
    Quantity level_quantity_step{25};
    std::int64_t inventory_skew_divisor{50};
    std::int64_t max_inventory{1000};
};

/// Two-sided liquidity provider quoting a multi-level Bid/Ask ladder skewed by inventory.
class MarketMakerAgent final : public TradingAgent {
public:
    MarketMakerAgent(std::size_t agent_index, MarketMakerConfig cfg = {})
        : TradingAgent(agent_index, AgentType::MarketMaker), cfg_(cfg) {
        active_order_ids_.reserve(cfg_.num_levels * 2U);
    }

    void on_step(const SimulationContext& ctx, OrderSubmissionSink& sink) override {
        for (const OrderId oid : active_order_ids_) {
            sink.cancel(metrics_.agent_index, oid);
        }
        active_order_ids_.clear();

        const Price skew =
            (cfg_.inventory_skew_divisor > 0) ? (metrics_.inventory / cfg_.inventory_skew_divisor)
                                              : 0;
        const Price fair_center = std::max<Price>(1, ctx.reference_mid_price - skew);
        const Price best_bid_price = fair_center - cfg_.half_spread_ticks;
        const Price best_ask_price = std::max<Price>(1, fair_center + cfg_.half_spread_ticks);

        for (std::size_t level = 0U; level < cfg_.num_levels; ++level) {
            const Quantity qty =
                cfg_.base_quantity + static_cast<Quantity>(level) * cfg_.level_quantity_step;
            const Price bid_price = best_bid_price - static_cast<Price>(level);
            if (bid_price > 0 && metrics_.inventory < cfg_.max_inventory) {
                active_order_ids_.push_back(
                    sink.submit_limit(metrics_.agent_index, Side::Buy, bid_price, qty));
            }
            const Price ask_price = best_ask_price + static_cast<Price>(level);
            if (metrics_.inventory > -cfg_.max_inventory) {
                active_order_ids_.push_back(
                    sink.submit_limit(metrics_.agent_index, Side::Sell, ask_price, qty));
            }
        }
    }

private:
    MarketMakerConfig cfg_;
    std::vector<OrderId> active_order_ids_{};
};

/// Configuration parameters for RandomTraderAgent.
struct RandomTraderConfig {
    std::uint64_t seed{1};
    double market_order_probability{0.25};
    Price max_price_offset_ticks{4};
    Quantity min_quantity{10};
    Quantity max_quantity{60};
    std::size_t max_resting_orders{12};
};

/// Noise trader submitting seeded Limit and Market orders around reference mid-price.
class RandomTraderAgent final : public TradingAgent {
public:
    RandomTraderAgent(std::size_t agent_index, RandomTraderConfig cfg = {})
        : TradingAgent(agent_index, AgentType::RandomTrader), cfg_(cfg), rng_(cfg.seed) {}

    void on_step(const SimulationContext& ctx, OrderSubmissionSink& sink) override {
        if (recent_resting_ids_.size() >= cfg_.max_resting_orders) {
            sink.cancel(metrics_.agent_index, recent_resting_ids_.front());
            recent_resting_ids_.pop_front();
        }

        std::uniform_real_distribution<double> unit_dist(0.0, 1.0);
        std::uniform_int_distribution<Quantity> qty_dist(cfg_.min_quantity, cfg_.max_quantity);
        std::uniform_int_distribution<Price> offset_dist(-cfg_.max_price_offset_ticks,
                                                         cfg_.max_price_offset_ticks);

        const Side side = (unit_dist(rng_) < 0.5) ? Side::Buy : Side::Sell;
        const Quantity qty = qty_dist(rng_);

        if (unit_dist(rng_) < cfg_.market_order_probability) {
            static_cast<void>(sink.submit_market(metrics_.agent_index, side, qty));
            return;
        }

        const Price center =
            (ctx.best_bid.has_value() && ctx.best_ask.has_value())
                ? (*ctx.best_bid + (*ctx.best_ask - *ctx.best_bid) / 2)
                : ctx.reference_mid_price;
        const Price limit_price = std::max<Price>(1, center + offset_dist(rng_));
        const OrderId oid = sink.submit_limit(metrics_.agent_index, side, limit_price, qty);
        recent_resting_ids_.push_back(oid);
    }

private:
    RandomTraderConfig cfg_;
    std::mt19937_64 rng_;
    std::deque<OrderId> recent_resting_ids_{};
};

/// Configuration parameters for MomentumTraderAgent.
struct MomentumTraderConfig {
    std::size_t lookback_window{5};
    Price breakout_threshold_ticks{3};
    Quantity order_quantity{40};
};

/// Trend-following trader submitting aggressive orders on short-horizon mid-price breakouts.
class MomentumTraderAgent final : public TradingAgent {
public:
    MomentumTraderAgent(std::size_t agent_index, MomentumTraderConfig cfg = {})
        : TradingAgent(agent_index, AgentType::MomentumTrader), cfg_(cfg) {}

    void on_step(const SimulationContext& ctx, OrderSubmissionSink& sink) override {
        const Price observed_mid =
            (ctx.best_bid.has_value() && ctx.best_ask.has_value())
                ? (*ctx.best_bid + (*ctx.best_ask - *ctx.best_bid) / 2)
                : ctx.reference_mid_price;
        price_history_.push_back(observed_mid);

        if (price_history_.size() <= cfg_.lookback_window) {
            return;
        }

        const Price past_mid =
            price_history_[price_history_.size() - 1U - cfg_.lookback_window];
        const Price delta = observed_mid - past_mid;

        if (delta >= cfg_.breakout_threshold_ticks) {
            static_cast<void>(
                sink.submit_market(metrics_.agent_index, Side::Buy, cfg_.order_quantity));
        } else if (delta <= -cfg_.breakout_threshold_ticks) {
            static_cast<void>(
                sink.submit_market(metrics_.agent_index, Side::Sell, cfg_.order_quantity));
        }
    }

private:
    MomentumTraderConfig cfg_;
    std::vector<Price> price_history_{};
};

/// Configuration parameters for InstitutionalTraderAgent (TWAP child-slice execution).
struct InstitutionalTraderConfig {
    Side side{Side::Buy};
    Quantity total_parent_quantity{1000};
    Quantity child_slice_quantity{50};
    std::size_t start_step{50};
    std::size_t execution_interval_steps{5};
};

/// Institutional liquidity taker slicing a large parent order across fixed intervals.
class InstitutionalTraderAgent final : public TradingAgent {
public:
    InstitutionalTraderAgent(std::size_t agent_index, InstitutionalTraderConfig cfg = {})
        : TradingAgent(agent_index, AgentType::InstitutionalTrader), cfg_(cfg) {}

    void on_step(const SimulationContext& ctx, OrderSubmissionSink& sink) override {
        if (ctx.step_index < cfg_.start_step ||
            submitted_quantity_ >= cfg_.total_parent_quantity) {
            return;
        }

        if (!arrival_mid_price_.has_value()) {
            const Price arrival_mid =
                (ctx.best_bid.has_value() && ctx.best_ask.has_value())
                    ? (*ctx.best_bid + (*ctx.best_ask - *ctx.best_bid) / 2)
                    : ctx.reference_mid_price;
            arrival_mid_price_ = arrival_mid;
        }

        if ((ctx.step_index - cfg_.start_step) % cfg_.execution_interval_steps != 0U) {
            return;
        }

        const Quantity remaining = cfg_.total_parent_quantity - submitted_quantity_;
        const Quantity slice_qty = std::min(cfg_.child_slice_quantity, remaining);
        static_cast<void>(sink.submit_market(metrics_.agent_index, cfg_.side, slice_qty));
        submitted_quantity_ += slice_qty;
    }

    void on_fill(Side side, Price price, Quantity quantity) override {
        TradingAgent::on_fill(side, price, quantity);
        executed_quantity_ += quantity;
        notional_ticks_ += static_cast<std::int64_t>(price) * static_cast<std::int64_t>(quantity);
    }

    [[nodiscard]] bool is_completed() const noexcept {
        return executed_quantity_ >= cfg_.total_parent_quantity;
    }
    [[nodiscard]] Quantity submitted_quantity() const noexcept { return submitted_quantity_; }
    [[nodiscard]] Quantity executed_quantity() const noexcept { return executed_quantity_; }
    [[nodiscard]] std::optional<Price> arrival_mid_price() const noexcept {
        return arrival_mid_price_;
    }
    [[nodiscard]] std::optional<double> average_execution_price_ticks() const noexcept {
        if (executed_quantity_ == 0U) {
            return std::nullopt;
        }
        return static_cast<double>(notional_ticks_) / static_cast<double>(executed_quantity_);
    }
    [[nodiscard]] Side parent_side() const noexcept { return cfg_.side; }

private:
    InstitutionalTraderConfig cfg_;
    Quantity submitted_quantity_{0};
    Quantity executed_quantity_{0};
    std::int64_t notional_ticks_{0};
    std::optional<Price> arrival_mid_price_{};
};

}  // namespace martingale

#endif  // MARTINGALE_SIMULATOR_AGENTS_HPP
