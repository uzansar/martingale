#include <benchmark/benchmark.h>
#include <martingale/book/flat_order_book.hpp>
#include <martingale/book/order_book.hpp>
#include <martingale/core/order.hpp>
#include <martingale/core/types.hpp>
#include <martingale/engine/matching_engine.hpp>
#include <martingale/performance/latency_recorder.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace martingale::benchmarks {
namespace {

struct SimCommand {
    bool is_cancel{false};
    Order order{};
    OrderId cancel_id{kInvalidOrderId};
};

[[nodiscard]] std::vector<SimCommand> generate_mixed_workload(std::size_t count,
                                                              std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<Price> price_dist(9990, 10010);
    std::uniform_int_distribution<Quantity> qty_dist(1U, 100U);

    std::vector<SimCommand> commands;
    commands.reserve(count);
    std::vector<OrderId> submitted_ids;
    submitted_ids.reserve(count);

    OrderId next_id = 1U;
    for (std::size_t i = 0U; i < count; ++i) {
        const int action = action_dist(rng);
        if (action < 10 && !submitted_ids.empty()) {
            std::uniform_int_distribution<std::size_t> id_dist(0U, submitted_ids.size() - 1U);
            commands.push_back(SimCommand{true, Order{}, submitted_ids[id_dist(rng)]});
            continue;
        }
        const Side side = (action_dist(rng) < 50) ? Side::Buy : Side::Sell;
        const OrderType type = (action < 25) ? OrderType::Market : OrderType::Limit;
        const Price price = (type == OrderType::Market) ? kMarketOrderPrice : price_dist(rng);
        const Quantity qty = qty_dist(rng);
        const Timestamp ts = static_cast<Timestamp>(i + 1U) * 1000U;

        const OrderId oid = next_id++;
        submitted_ids.push_back(oid);
        commands.push_back(
            SimCommand{false, Order(oid, 1U, side, type, price, qty, ts), kInvalidOrderId});
    }
    return commands;
}

void publish_latency_counters(benchmark::State& state, const LatencyStats& stats) {
    state.counters["Mean_ns"] = benchmark::Counter(stats.mean_ns);
    state.counters["P50_ns"] = benchmark::Counter(static_cast<double>(stats.p50_ns));
    state.counters["P90_ns"] = benchmark::Counter(static_cast<double>(stats.p90_ns));
    state.counters["P95_ns"] = benchmark::Counter(static_cast<double>(stats.p95_ns));
    state.counters["P99_ns"] = benchmark::Counter(static_cast<double>(stats.p99_ns));
    state.counters["P99_9_ns"] = benchmark::Counter(static_cast<double>(stats.p99_9_ns));
    state.counters["Max_ns"] = benchmark::Counter(static_cast<double>(stats.max_ns));
}

template <typename BookType>
void run_add_and_cancel_benchmark(benchmark::State& state) {
    constexpr std::size_t kBatchSize = 1000U;
    BookType book(1U);
    OrderId next_id = 1U;

    for (auto _ : state) {
        for (std::size_t i = 0U; i < kBatchSize; ++i) {
            const Price price = 10000 + static_cast<Price>(i % 20U);
            const OrderId oid = 1U + (next_id % 10000U);
            const Order order(oid, 1U, Side::Buy, OrderType::Limit, price, 100U, next_id);
            bool added = book.add_order(order);
            benchmark::DoNotOptimize(added);
            auto cancelled = book.cancel_order(oid);
            benchmark::DoNotOptimize(cancelled);
            ++next_id;
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kBatchSize));

    LatencyRecorder recorder;
    recorder.reserve(kBatchSize);
    for (std::size_t i = 0U; i < kBatchSize; ++i) {
        const Price price = 10000 + static_cast<Price>(i % 20U);
        const OrderId oid = 1U + (next_id % 10000U);
        const Order order(oid, 1U, Side::Buy, OrderType::Limit, price, 100U, next_id);
        recorder.measure([&]() {
            bool added = book.add_order(order);
            benchmark::DoNotOptimize(added);
            auto cancelled = book.cancel_order(oid);
            benchmark::DoNotOptimize(cancelled);
        });
        ++next_id;
    }
    publish_latency_counters(state, recorder.compute_stats());
}

template <typename EngineType>
void run_resting_limit_benchmark(benchmark::State& state) {
    constexpr std::size_t kOrderCount = 1000U;

    for (auto _ : state) {
        EngineType engine(1U);
        for (std::size_t i = 0U; i < kOrderCount; ++i) {
            const Side side = (i % 2U == 0U) ? Side::Buy : Side::Sell;
            const Price price = (side == Side::Buy) ? (9990 - static_cast<Price>(i % 50U))
                                                    : (10010 + static_cast<Price>(i % 50U));
            const OrderId oid = static_cast<OrderId>(i + 1U);
            auto res = engine.submit_order(Order(oid, 1U, side, OrderType::Limit, price, 100U, oid));
            benchmark::DoNotOptimize(res);
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kOrderCount));

    LatencyRecorder recorder;
    recorder.reserve(kOrderCount);
    EngineType engine(1U);
    for (std::size_t i = 0U; i < kOrderCount; ++i) {
        const Side side = (i % 2U == 0U) ? Side::Buy : Side::Sell;
        const Price price = (side == Side::Buy) ? (9990 - static_cast<Price>(i % 50U))
                                                : (10010 + static_cast<Price>(i % 50U));
        const OrderId oid = static_cast<OrderId>(i + 1U);
        recorder.measure([&]() {
            auto res = engine.submit_order(Order(oid, 1U, side, OrderType::Limit, price, 100U, oid));
            benchmark::DoNotOptimize(res);
        });
    }
    publish_latency_counters(state, recorder.compute_stats());
}

template <typename EngineType>
void run_crossing_limit_benchmark(benchmark::State& state) {
    constexpr std::size_t kPairCount = 1000U;

    for (auto _ : state) {
        state.PauseTiming();
        EngineType engine(1U);
        for (std::size_t i = 0U; i < kPairCount; ++i) {
            const Price price = 10000 + static_cast<Price>(i % 25U);
            const OrderId oid = static_cast<OrderId>(i + 1U);
            static_cast<void>(
                engine.submit_order(Order(oid, 1U, Side::Sell, OrderType::Limit, price, 50U, oid)));
        }
        state.ResumeTiming();

        for (std::size_t i = 0U; i < kPairCount; ++i) {
            const Price price = 10000 + static_cast<Price>(i % 25U);
            const OrderId oid = static_cast<OrderId>(kPairCount + i + 1U);
            auto res =
                engine.submit_order(Order(oid, 1U, Side::Buy, OrderType::Limit, price, 50U, oid));
            benchmark::DoNotOptimize(res);
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kPairCount));

    LatencyRecorder recorder;
    recorder.reserve(kPairCount);
    EngineType engine(1U);
    for (std::size_t i = 0U; i < kPairCount; ++i) {
        const Price price = 10000 + static_cast<Price>(i % 25U);
        const OrderId oid = static_cast<OrderId>(i + 1U);
        static_cast<void>(
            engine.submit_order(Order(oid, 1U, Side::Sell, OrderType::Limit, price, 50U, oid)));
    }
    for (std::size_t i = 0U; i < kPairCount; ++i) {
        const Price price = 10000 + static_cast<Price>(i % 25U);
        const OrderId oid = static_cast<OrderId>(kPairCount + i + 1U);
        recorder.measure([&]() {
            auto res =
                engine.submit_order(Order(oid, 1U, Side::Buy, OrderType::Limit, price, 50U, oid));
            benchmark::DoNotOptimize(res);
        });
    }
    publish_latency_counters(state, recorder.compute_stats());
}

template <typename EngineType>
void run_mixed_workload_benchmark(benchmark::State& state) {
    constexpr std::size_t kCommandCount = 10000U;
    const std::vector<SimCommand> commands = generate_mixed_workload(kCommandCount, 42ULL);

    for (auto _ : state) {
        EngineType engine(1U);
        for (const auto& cmd : commands) {
            if (cmd.is_cancel) {
                auto cancelled = engine.cancel_order(cmd.cancel_id);
                benchmark::DoNotOptimize(cancelled);
            } else {
                auto res = engine.submit_order(cmd.order);
                benchmark::DoNotOptimize(res);
            }
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kCommandCount));

    LatencyRecorder recorder;
    recorder.reserve(kCommandCount);
    EngineType engine(1U);
    for (const auto& cmd : commands) {
        recorder.measure([&]() {
            if (cmd.is_cancel) {
                auto cancelled = engine.cancel_order(cmd.cancel_id);
                benchmark::DoNotOptimize(cancelled);
            } else {
                auto res = engine.submit_order(cmd.order);
                benchmark::DoNotOptimize(res);
            }
        });
    }
    publish_latency_counters(state, recorder.compute_stats());
}

void BM_OrderBook_AddAndCancel(benchmark::State& state) {
    run_add_and_cancel_benchmark<OrderBook>(state);
}
void BM_FlatOrderBook_AddAndCancel(benchmark::State& state) {
    run_add_and_cancel_benchmark<FlatOrderBook>(state);
}

void BM_MatchingEngine_RestingLimitOrders(benchmark::State& state) {
    run_resting_limit_benchmark<MatchingEngine>(state);
}
void BM_FastMatchingEngine_RestingLimitOrders(benchmark::State& state) {
    run_resting_limit_benchmark<FastMatchingEngine>(state);
}

void BM_MatchingEngine_CrossingLimitOrders(benchmark::State& state) {
    run_crossing_limit_benchmark<MatchingEngine>(state);
}
void BM_FastMatchingEngine_CrossingLimitOrders(benchmark::State& state) {
    run_crossing_limit_benchmark<FastMatchingEngine>(state);
}

void BM_MatchingEngine_MixedWorkload_10k(benchmark::State& state) {
    run_mixed_workload_benchmark<MatchingEngine>(state);
}
void BM_FastMatchingEngine_MixedWorkload_10k(benchmark::State& state) {
    run_mixed_workload_benchmark<FastMatchingEngine>(state);
}

BENCHMARK(BM_OrderBook_AddAndCancel);
BENCHMARK(BM_FlatOrderBook_AddAndCancel);
BENCHMARK(BM_MatchingEngine_RestingLimitOrders);
BENCHMARK(BM_FastMatchingEngine_RestingLimitOrders);
BENCHMARK(BM_MatchingEngine_CrossingLimitOrders);
BENCHMARK(BM_FastMatchingEngine_CrossingLimitOrders);
BENCHMARK(BM_MatchingEngine_MixedWorkload_10k);
BENCHMARK(BM_FastMatchingEngine_MixedWorkload_10k);

}  // namespace
}  // namespace martingale::benchmarks
