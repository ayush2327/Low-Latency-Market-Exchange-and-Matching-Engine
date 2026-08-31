#pragma once

#include "ring_buffer.hpp"

#include <atomic>
#include <cstdint>

void listen_server(
    uint16_t port,
    OrderRingBuffer& order_queue,
    const std::atomic<bool>& running
);
