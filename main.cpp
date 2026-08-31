#include "engine.hpp"
#include "network.hpp"
#include "order_book.hpp"
#include "ring_buffer.hpp"
#include "shutdown.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>

int main() {
    constexpr uint16_t PORT = 9000;
    constexpr uint16_t INSTRUMENT_ID = 0;

    if (!install_shutdown_handlers()) {
        std::cerr << "Failed to install shutdown handlers\n";
        return 1;
    }

    OrderRingBuffer order_queue;
    TradeRingBuffer trade_queue;
    OrderBook order_book(INSTRUMENT_ID, &trade_queue);
    std::atomic<bool> running{true};

    std::thread matching_thread(
        run_matching_engine,
        std::ref(order_queue),
        std::ref(order_book),
        std::cref(running)
    );

    std::thread trade_publisher_thread(
        run_trade_publisher,
        std::ref(trade_queue),
        std::cref(running)
    );

    try {
        listen_server(PORT, order_queue, running);
    } catch (const std::exception& error) {
        std::cerr << "Server failed: " << error.what() << "\n";
    }

    request_shutdown();
    running.store(false, std::memory_order_release);
    matching_thread.join();
    trade_publisher_thread.join();

    return 0;
}
