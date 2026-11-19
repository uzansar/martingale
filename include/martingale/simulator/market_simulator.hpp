#ifndef MARTINGALE_SIMULATOR_MARKET_SIMULATOR_HPP
#define MARTINGALE_SIMULATOR_MARKET_SIMULATOR_HPP

#include <martingale/core/order.hpp>
#include <martingale/core/trade.hpp>
#include <martingale/core/types.hpp>
#include <martingale/engine/matching_engine.hpp>
#include <martingale/market_data/market_data_publisher.hpp>
#include <martingale/simulator/agents.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace martingale {

/// Top-level configuration for a deterministic multi-agent market simulation run.
struct SimulationConfig {
    std::uint64_t seed{42};
    InstrumentId instrument_id{1};
    std::size_t total_steps{500};
    std::size_t snapshot_interval{10};
    std::size_t snapshot_depth{5};
    Price initial_mid_price{10000};
    Timestamp step_duration_ns{1'000'000};
    Price fundamental_step_sigma_ticks{1};
    MarketMakerConfig market_maker_config{};
    std::size_t num_random_traders{4};
    std::size_t num_momentum_traders{1};
    bool include_institutional_trader{true};
    InstitutionalTraderConfig institutional_config{};
};

/// Time-series record captured at the end of each simulation step.
struct SimulationStepRecord {
    std::size_t step_index{0};
    Timestamp timestamp{0};
    Price reference_mid_price{0};
    std::optional<Price> best_bid{};
    Quantity best_bid_quantity{0};
    std::optional<Price> best_ask{};
    Quantity best_ask_quantity{0};
    std::size_t trade_count{0};
    Quantity step_volume{0};

    [[nodiscard]] constexpr bool operator==(const SimulationStepRecord&) const noexcept = default;
};

/// Complete deterministic output dataset produced by MarketSimulator::run().
struct SimulationResult {
    std::vector<SimulationStepRecord> step_records{};
    std::vector<Trade> trades{};
    std::vector<OrderBookSnapshot> l2_snapshots{};
    std::vector<AgentMetrics> agent_metrics{};
    std::optional<Price> institutional_arrival_mid{};
    std::optional<double> institutional_avg_exec_price{};
    Quantity institutional_executed_qty{0};
    Side institutional_side{Side::Buy};
};

/// Seeded, event-driven simulation harness orchestrating synthetic agents over
/// FastMatchingEngine (FlatOrderBook + ObjectPool) and MarketDataPublisher.
class MarketSimulator {
public:
    explicit MarketSimulator(SimulationConfig config = {}) : config_(config) {}

    [[nodiscard]] SimulationResult run() const {
        FastMatchingEngine engine(config_.instrument_id);
        MarketDataPublisher publisher;
        std::vector<std::unique_ptr<TradingAgent>> agents;
        std::unordered_map<OrderId, std::size_t> order_owners;

        // 1. Register MarketMakerAgent at index 0
        agents.push_back(std::make_unique<MarketMakerAgent>(0U, config_.market_maker_config));

        // 2. Register seeded RandomTraderAgents
        std::mt19937_64 master_rng(config_.seed);
        for (std::size_t i = 0U; i < config_.num_random_traders; ++i) {
            RandomTraderConfig rand_cfg;
            rand_cfg.seed = master_rng();
            agents.push_back(std::make_unique<RandomTraderAgent>(agents.size(), rand_cfg));
        }

        // 3. Register MomentumTraderAgents
        for (std::size_t i = 0U; i < config_.num_momentum_traders; ++i) {
            agents.push_back(std::make_unique<MomentumTraderAgent>(agents.size()));
        }

        // 4. Register optional InstitutionalTraderAgent
        InstitutionalTraderAgent* inst_ptr = nullptr;
        if (config_.include_institutional_trader) {
            auto inst = std::make_unique<InstitutionalTraderAgent>(agents.size(),
                                                                   config_.institutional_config);
            inst_ptr = inst.get();
            agents.push_back(std::move(inst));
        }

        ExchangeSink sink(engine, publisher, agents, order_owners, config_.instrument_id);
        SimulationResult result;
        result.step_records.reserve(config_.total_steps);

        Price reference_mid = config_.initial_mid_price;
        std::uniform_int_distribution<Price> fundamental_step_dist(
            -config_.fundamental_step_sigma_ticks, config_.fundamental_step_sigma_ticks);

        for (std::size_t step = 0U; step < config_.total_steps; ++step) {
            if (step > 0U && config_.fundamental_step_sigma_ticks > 0) {
                reference_mid = std::max<Price>(10, reference_mid + fundamental_step_dist(master_rng));
            }

            const Timestamp step_time = static_cast<Timestamp>(step + 1U) * config_.step_duration_ns;
            sink.begin_step(step_time);

            for (auto& agent : agents) {
                const auto& book = engine.book(config_.instrument_id);
                const SimulationContext ctx{
                    .step_index = step,
                    .instrument_id = config_.instrument_id,
                    .current_time = step_time,
                    .reference_mid_price = reference_mid,
                    .best_bid = book.best_bid(),
                    .best_bid_quantity = book.best_bid_quantity(),
                    .best_ask = book.best_ask(),
                    .best_ask_quantity = book.best_ask_quantity(),
                };
                agent->on_step(ctx, sink);
            }

            const auto& book = engine.book(config_.instrument_id);
            result.step_records.push_back(SimulationStepRecord{
                .step_index = step,
                .timestamp = step_time,
                .reference_mid_price = reference_mid,
                .best_bid = book.best_bid(),
                .best_bid_quantity = book.best_bid_quantity(),
                .best_ask = book.best_ask(),
                .best_ask_quantity = book.best_ask_quantity(),
                .trade_count = sink.step_trade_count(),
                .step_volume = sink.step_volume(),
            });

            if (config_.snapshot_interval > 0U && ((step + 1U) % config_.snapshot_interval == 0U)) {
                result.l2_snapshots.push_back(
                    publisher.capture_snapshot(book, config_.snapshot_depth, step_time));
            }
        }

        result.trades = engine.trades();
        result.agent_metrics.reserve(agents.size());
        for (const auto& agent : agents) {
            result.agent_metrics.push_back(agent->metrics());
        }
        if (inst_ptr != nullptr) {
            result.institutional_arrival_mid = inst_ptr->arrival_mid_price();
            result.institutional_avg_exec_price = inst_ptr->average_execution_price_ticks();
            result.institutional_executed_qty = inst_ptr->executed_quantity();
            result.institutional_side = inst_ptr->parent_side();
        }
        return result;
    }

private:
    class ExchangeSink final : public OrderSubmissionSink {
    public:
        ExchangeSink(FastMatchingEngine& engine, MarketDataPublisher& publisher,
                     std::vector<std::unique_ptr<TradingAgent>>& agents,
                     std::unordered_map<OrderId, std::size_t>& order_owners,
                     InstrumentId instrument_id) noexcept
            : engine_(engine),
              publisher_(publisher),
              agents_(agents),
              order_owners_(order_owners),
              instrument_id_(instrument_id) {}

        void begin_step(Timestamp ts) noexcept {
            current_time_ = ts;
            step_trade_count_ = 0U;
            step_volume_ = 0U;
        }

        OrderId submit_limit(std::size_t agent_index, Side side, Price price,
                             Quantity quantity) override {
            return submit_impl(agent_index, OrderType::Limit, side, price, quantity);
        }

        OrderId submit_market(std::size_t agent_index, Side side, Quantity quantity) override {
            return submit_impl(agent_index, OrderType::Market, side, kMarketOrderPrice, quantity);
        }

        void cancel(std::size_t agent_index, OrderId order_id) override {
            const auto cancelled = engine_.cancel_order(instrument_id_, order_id);
            if (cancelled.has_value()) {
                agents_[agent_index]->record_cancel_submitted();
                publisher_.on_order_cancelled(*cancelled, engine_.book(instrument_id_),
                                              current_time_);
                order_owners_.erase(order_id);
            }
        }

        [[nodiscard]] std::size_t step_trade_count() const noexcept { return step_trade_count_; }
        [[nodiscard]] Quantity step_volume() const noexcept { return step_volume_; }

    private:
        OrderId submit_impl(std::size_t agent_index, OrderType type, Side side, Price price,
                            Quantity quantity) {
            const OrderId oid = next_order_id_++;
            order_owners_.emplace(oid, agent_index);
            agents_[agent_index]->record_order_submitted();

            const Order order(oid, instrument_id_, side, type, price, quantity, current_time_);
            const OrderResult res = engine_.submit_order(order);
            const auto& book = engine_.book(instrument_id_);
            publisher_.on_order_processed(res, book);

            for (const auto& trade : res.trades) {
                const auto buyer_it = order_owners_.find(trade.buyer_order_id);
                if (buyer_it != order_owners_.end()) {
                    agents_[buyer_it->second]->on_fill(Side::Buy, trade.price, trade.quantity);
                }
                const auto seller_it = order_owners_.find(trade.seller_order_id);
                if (seller_it != order_owners_.end()) {
                    agents_[seller_it->second]->on_fill(Side::Sell, trade.price, trade.quantity);
                }

                // Clean up resting counterparty lookup once completely filled
                const OrderId resting_id =
                    (trade.aggressor_side == Side::Buy) ? trade.seller_order_id
                                                        : trade.buyer_order_id;
                if (book.find_order(resting_id) == nullptr) {
                    order_owners_.erase(resting_id);
                }

                ++step_trade_count_;
                step_volume_ += trade.quantity;
            }

            if (!res.final_order.is_active()) {
                order_owners_.erase(oid);
            }
            return oid;
        }

        FastMatchingEngine& engine_;
        MarketDataPublisher& publisher_;
        std::vector<std::unique_ptr<TradingAgent>>& agents_;
        std::unordered_map<OrderId, std::size_t>& order_owners_;
        InstrumentId instrument_id_;
        OrderId next_order_id_{1};
        Timestamp current_time_{0};
        std::size_t step_trade_count_{0};
        Quantity step_volume_{0};
    };

    SimulationConfig config_;
};

}  // namespace martingale

#endif  // MARTINGALE_SIMULATOR_MARKET_SIMULATOR_HPP
