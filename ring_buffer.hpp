#pragma once

#include "exchange.hpp"

#include <array>
#include <atomic>
#include <cstddef>

class OrderRingBuffer {
public:
    static constexpr size_t CAPACITY = 4096;

    OrderRingBuffer();

    OrderRingBuffer(const OrderRingBuffer&) = delete;
    OrderRingBuffer& operator=(const OrderRingBuffer&) = delete;

    bool push(const OrderRequest& order);
    bool pop(OrderRequest& order);

    bool empty() const;
    bool full() const;
    size_t size() const;
    size_t capacity() const;

private:
    static constexpr size_t MASK = CAPACITY - 1;

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::array<OrderRequest, CAPACITY> buffer_;
};

class TradeRingBuffer {
public:
    static constexpr size_t CAPACITY = 4096;

    TradeRingBuffer();

    TradeRingBuffer(const TradeRingBuffer&) = delete;
    TradeRingBuffer& operator=(const TradeRingBuffer&) = delete;

    bool push(const TradeEvent& trade);
    bool pop(TradeEvent& trade);

    bool empty() const;
    bool full() const;
    size_t size() const;
    size_t capacity() const;

private:
    static constexpr size_t MASK = CAPACITY - 1;

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::array<TradeEvent, CAPACITY> buffer_;
};
