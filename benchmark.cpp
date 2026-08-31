#include "order_book.hpp"
#include "ring_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint16_t INSTRUMENT_ID = 0;
constexpr uint64_t BASE_BID_PRICE = 100000;
constexpr uint64_t BASE_ASK_PRICE = 101000;
constexpr size_t PRICE_BUCKETS = 4096;

struct BenchmarkResult {
    std::string name;
    size_t operations = 0;
    size_t accepted = 0;
    double total_ms = 0.0;
    double ns_per_op = 0.0;
    double ops_per_second = 0.0;
    uint64_t avg_ns = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
    bool has_latency = false;
};

uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
    );
}

uint64_t percentile(const std::vector<uint64_t>& sorted_samples, double percentile_value) {
    if (sorted_samples.empty()) {
        return 0;
    }

    size_t index = static_cast<size_t>(
        (percentile_value / 100.0) * static_cast<double>(sorted_samples.size() - 1)
    );
    return sorted_samples[index];
}

OrderRequest make_new_order(
    uint64_t order_id,
    Side side,
    uint64_t price,
    uint32_t quantity
) {
    return OrderRequest{
        MsgType::NEW,
        side,
        INSTRUMENT_ID,
        quantity,
        price,
        order_id,
        order_id
    };
}

OrderRequest make_cancel_order(uint64_t order_id, Side side = Side::BUY) {
    return OrderRequest{
        MsgType::CANCEL,
        side,
        INSTRUMENT_ID,
        0,
        0,
        order_id,
        order_id
    };
}

BenchmarkResult build_throughput_result(
    const std::string& name,
    size_t operations,
    Clock::time_point start,
    Clock::time_point end
) {
    uint64_t total_ns = elapsed_ns(start, end);
    double ns_per_op = operations == 0 ? 0.0 : static_cast<double>(total_ns) / operations;

    BenchmarkResult result;
    result.name = name;
    result.operations = operations;
    result.accepted = operations;
    result.total_ms = static_cast<double>(total_ns) / 1'000'000.0;
    result.ns_per_op = ns_per_op;
    result.ops_per_second = ns_per_op == 0.0 ? 0.0 : 1'000'000'000.0 / ns_per_op;
    return result;
}

BenchmarkResult benchmark_order_book_requests(
    const std::string& name,
    OrderBook& order_book,
    const std::vector<OrderRequest>& requests
) {
    std::vector<uint64_t> latencies;
    latencies.resize(requests.size());

    size_t accepted = 0;
    auto total_start = Clock::now();

    for (size_t i = 0; i < requests.size(); i++) {
        auto start = Clock::now();
        OrderBookStatus status = order_book.process(requests[i]);
        auto end = Clock::now();

        latencies[i] = elapsed_ns(start, end);
        accepted += status == OrderBookStatus::ACCEPTED ? 1 : 0;
    }

    auto total_end = Clock::now();
    std::sort(latencies.begin(), latencies.end());

    uint64_t total_ns = elapsed_ns(total_start, total_end);
    uint64_t sum_ns = std::accumulate(latencies.begin(), latencies.end(), uint64_t{0});
    double ns_per_op = requests.empty() ? 0.0 : static_cast<double>(total_ns) / requests.size();

    BenchmarkResult result;
    result.name = name;
    result.operations = requests.size();
    result.accepted = accepted;
    result.total_ms = static_cast<double>(total_ns) / 1'000'000.0;
    result.ns_per_op = ns_per_op;
    result.ops_per_second = ns_per_op == 0.0 ? 0.0 : 1'000'000'000.0 / ns_per_op;
    result.avg_ns = requests.empty() ? 0 : sum_ns / requests.size();
    result.p50_ns = percentile(latencies, 50.0);
    result.p95_ns = percentile(latencies, 95.0);
    result.p99_ns = percentile(latencies, 99.0);
    result.p999_ns = percentile(latencies, 99.9);
    result.max_ns = latencies.empty() ? 0 : latencies.back();
    result.has_latency = true;
    return result;
}

std::vector<OrderRequest> make_passive_insert_workload(size_t operations) {
    std::vector<OrderRequest> requests;
    requests.reserve(operations);

    for (size_t i = 0; i < operations; i++) {
        uint64_t price = BASE_BID_PRICE + (i & (PRICE_BUCKETS - 1));
        requests.push_back(make_new_order(i + 1, Side::BUY, price, 100));
    }

    return requests;
}

BenchmarkResult benchmark_passive_inserts(size_t operations) {
    OrderBook order_book(INSTRUMENT_ID);
    std::vector<OrderRequest> requests = make_passive_insert_workload(operations);
    return benchmark_order_book_requests("order_book.passive_insert", order_book, requests);
}

BenchmarkResult benchmark_aggressive_matches(size_t operations) {
    OrderBook order_book(INSTRUMENT_ID);

    for (size_t i = 0; i < operations; i++) {
        order_book.process(make_new_order(i + 1, Side::SELL, BASE_ASK_PRICE, 1));
    }

    std::vector<OrderRequest> requests;
    requests.reserve(operations);

    for (size_t i = 0; i < operations; i++) {
        requests.push_back(
            make_new_order(operations + i + 1, Side::BUY, BASE_ASK_PRICE, 1)
        );
    }

    return benchmark_order_book_requests("order_book.aggressive_match", order_book, requests);
}

BenchmarkResult benchmark_cancels(size_t operations) {
    OrderBook order_book(INSTRUMENT_ID);

    for (size_t i = 0; i < operations; i++) {
        uint64_t price = BASE_BID_PRICE + (i & (PRICE_BUCKETS - 1));
        order_book.process(make_new_order(i + 1, Side::BUY, price, 100));
    }

    std::vector<OrderRequest> requests;
    requests.reserve(operations);

    for (size_t i = 0; i < operations; i++) {
        requests.push_back(make_cancel_order(i + 1));
    }

    return benchmark_order_book_requests("order_book.cancel", order_book, requests);
}

BenchmarkResult benchmark_mixed_workload(size_t operations) {
    OrderBook order_book(INSTRUMENT_ID);
    size_t preload_cancel_orders = operations / 4 + 2;
    size_t preload_ask_orders = operations / 4 + 2;

    for (size_t i = 0; i < preload_cancel_orders; i++) {
        order_book.process(make_new_order(10'000'000 + i, Side::BUY, BASE_BID_PRICE - 1000, 1));
    }

    for (size_t i = 0; i < preload_ask_orders; i++) {
        order_book.process(make_new_order(20'000'000 + i, Side::SELL, BASE_ASK_PRICE, 1));
    }

    std::vector<OrderRequest> requests;
    requests.reserve(operations);

    size_t cancel_index = 0;
    size_t aggressive_index = 0;
    uint64_t next_order_id = 100'000'000;

    for (size_t i = 0; i < operations; i++) {
        switch (i & 3U) {
            case 0:
                requests.push_back(
                    make_new_order(
                        next_order_id++,
                        Side::BUY,
                        BASE_BID_PRICE - (i & (PRICE_BUCKETS - 1)),
                        10
                    )
                );
                break;
            case 1:
                requests.push_back(make_cancel_order(10'000'000 + cancel_index++));
                break;
            case 2:
                requests.push_back(
                    make_new_order(
                        next_order_id++,
                        Side::BUY,
                        BASE_ASK_PRICE,
                        1
                    )
                );
                aggressive_index++;
                break;
            default:
                requests.push_back(
                    make_new_order(
                        next_order_id++,
                        Side::SELL,
                        BASE_ASK_PRICE + 1000 + (i & (PRICE_BUCKETS - 1)),
                        10
                    )
                );
                break;
        }
    }

    (void)aggressive_index;
    return benchmark_order_book_requests("order_book.mixed", order_book, requests);
}

BenchmarkResult benchmark_order_ring_buffer(size_t operations) {
    OrderRingBuffer queue;
    std::vector<OrderRequest> requests = make_passive_insert_workload(operations);
    std::atomic<bool> start{false};
    std::atomic<size_t> ready{0};
    std::atomic<uint64_t> checksum{0};

    std::thread producer([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        for (const OrderRequest& request : requests) {
            while (!queue.push(request)) {}
        }
    });

    std::thread consumer([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        OrderRequest request{};
        uint64_t local_checksum = 0;

        for (size_t consumed = 0; consumed < operations;) {
            if (queue.pop(request)) {
                local_checksum += request.order_id;
                consumed++;
            }
        }

        checksum.store(local_checksum, std::memory_order_release);
    });

    while (ready.load(std::memory_order_acquire) != 2) {}
    auto start_time = Clock::now();
    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();
    auto end_time = Clock::now();

    if (checksum.load(std::memory_order_acquire) == 0 && operations > 0) {
        std::cerr << "Unexpected zero checksum\n";
    }

    return build_throughput_result(
        "spsc.order_ring_buffer",
        operations,
        start_time,
        end_time
    );
}

BenchmarkResult benchmark_trade_ring_buffer(size_t operations) {
    TradeRingBuffer queue;
    std::vector<TradeEvent> trades;
    trades.reserve(operations);

    for (size_t i = 0; i < operations; i++) {
        trades.push_back(TradeEvent{
            i + 1,
            operations + i + 1,
            BASE_ASK_PRICE,
            1,
            INSTRUMENT_ID
        });
    }

    std::atomic<bool> start{false};
    std::atomic<size_t> ready{0};
    std::atomic<uint64_t> checksum{0};

    std::thread producer([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        for (const TradeEvent& trade : trades) {
            while (!queue.push(trade)) {}
        }
    });

    std::thread consumer([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        TradeEvent trade{};
        uint64_t local_checksum = 0;

        for (size_t consumed = 0; consumed < operations;) {
            if (queue.pop(trade)) {
                local_checksum += trade.buy_order_id;
                consumed++;
            }
        }

        checksum.store(local_checksum, std::memory_order_release);
    });

    while (ready.load(std::memory_order_acquire) != 2) {}
    auto start_time = Clock::now();
    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();
    auto end_time = Clock::now();

    if (checksum.load(std::memory_order_acquire) == 0 && operations > 0) {
        std::cerr << "Unexpected zero checksum\n";
    }

    return build_throughput_result(
        "spsc.trade_ring_buffer",
        operations,
        start_time,
        end_time
    );
}

BenchmarkResult benchmark_full_pipeline(size_t operations) {
    OrderRingBuffer order_queue;
    TradeRingBuffer trade_queue;
    OrderBook order_book(INSTRUMENT_ID, &trade_queue);
    std::vector<OrderRequest> requests;
    requests.reserve(operations);

    for (size_t i = 0; i < operations / 2; i++) {
        requests.push_back(make_new_order(i + 1, Side::SELL, BASE_ASK_PRICE, 1));
    }

    for (size_t i = operations / 2; i < operations; i++) {
        requests.push_back(make_new_order(i + 1, Side::BUY, BASE_ASK_PRICE, 1));
    }

    std::atomic<bool> start{false};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> matcher_done{false};
    std::atomic<size_t> ready{0};
    std::atomic<uint64_t> checksum{0};

    std::thread producer([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        for (const OrderRequest& request : requests) {
            while (!order_queue.push(request)) {}
        }

        producer_done.store(true, std::memory_order_release);
    });

    std::thread matcher([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        OrderRequest request{};
        while (!producer_done.load(std::memory_order_acquire) || !order_queue.empty()) {
            if (order_queue.pop(request)) {
                (void)order_book.process(request);
            }
        }

        matcher_done.store(true, std::memory_order_release);
    });

    std::thread trade_consumer([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}

        TradeEvent trade{};
        uint64_t local_checksum = 0;

        while (!matcher_done.load(std::memory_order_acquire) || !trade_queue.empty()) {
            if (trade_queue.pop(trade)) {
                local_checksum += trade.buy_order_id;
            }
        }

        checksum.store(local_checksum, std::memory_order_release);
    });

    while (ready.load(std::memory_order_acquire) != 3) {}
    auto start_time = Clock::now();
    start.store(true, std::memory_order_release);

    producer.join();
    matcher.join();
    trade_consumer.join();
    auto end_time = Clock::now();

    if (checksum.load(std::memory_order_acquire) == 0 && operations > 1) {
        std::cerr << "Unexpected zero checksum\n";
    }

    return build_throughput_result(
        "pipeline.order_to_match_to_trade",
        operations,
        start_time,
        end_time
    );
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::left << std::setw(36) << result.name
              << " ops=" << std::right << std::setw(10) << result.operations
              << " accepted=" << std::setw(10) << result.accepted
              << " total_ms=" << std::setw(10) << std::fixed << std::setprecision(3)
              << result.total_ms
              << " ns/op=" << std::setw(10) << std::setprecision(1)
              << result.ns_per_op
              << " ops/sec=" << std::setw(12) << std::setprecision(0)
              << result.ops_per_second;

    if (result.has_latency) {
        std::cout << " avg=" << result.avg_ns
                  << " p50=" << result.p50_ns
                  << " p95=" << result.p95_ns
                  << " p99=" << result.p99_ns
                  << " p99.9=" << result.p999_ns
                  << " max=" << result.max_ns;
    }

    std::cout << "\n";
}
}

int main(int argc, char** argv) {
    size_t operations = 100000;

    if (argc > 1) {
        operations = static_cast<size_t>(std::stoull(argv[1]));
    }

    std::cout << "operations_per_benchmark=" << operations << "\n";
    std::cout << "max_resting_orders=" << OrderBook::MAX_RESTING_ORDERS << "\n";
    std::cout << "max_active_price_levels=" << OrderBook::MAX_ACTIVE_PRICE_LEVELS << "\n\n";

    print_result(benchmark_order_ring_buffer(operations));
    print_result(benchmark_trade_ring_buffer(operations));
    print_result(benchmark_passive_inserts(operations));
    print_result(benchmark_aggressive_matches(operations));
    print_result(benchmark_cancels(operations));
    print_result(benchmark_mixed_workload(operations));
    print_result(benchmark_full_pipeline(operations));

    return 0;
}
