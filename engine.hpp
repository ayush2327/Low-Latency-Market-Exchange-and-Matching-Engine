#pragma once

#include "order_book.hpp"
#include "ring_buffer.hpp"

#include <atomic>

void run_matching_engine(
    OrderRingBuffer& order_queue,
    OrderBook& order_book,
    const std::atomic<bool>& running
);

void run_trade_publisher(
    TradeRingBuffer& trade_queue,
    const std::atomic<bool>& running
);
