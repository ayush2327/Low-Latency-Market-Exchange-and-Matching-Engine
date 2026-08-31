#include "engine.hpp"

#include <iostream>

void run_matching_engine(
    OrderRingBuffer& order_queue,
    OrderBook& order_book,
    const std::atomic<bool>& running
) {
    OrderRequest order{};

    while (running.load(std::memory_order_acquire) || !order_queue.empty()) {
        if (order_queue.pop(order)) {
            (void)order_book.process(order);
        }
    }
}

void run_trade_publisher(
    TradeRingBuffer& trade_queue,
    const std::atomic<bool>& running
) {
    TradeEvent trade{};

    while (running.load(std::memory_order_acquire) || !trade_queue.empty()) {
        if (trade_queue.pop(trade)) {
            std::cout << "TRADE buy_order_id=" << trade.buy_order_id
                      << " sell_order_id=" << trade.sell_order_id
                      << " instrument_id=" << trade.instrument_id
                      << " price=" << trade.price
                      << " quantity=" << trade.quantity << "\n";
        }
    }
}
