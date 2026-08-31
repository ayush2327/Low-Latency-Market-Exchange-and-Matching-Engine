#include "ring_buffer.hpp"

OrderRingBuffer::OrderRingBuffer() {
    head_.store(0);
    tail_.store(0);
}

bool OrderRingBuffer::empty() const {
    size_t head = head_.load();
    size_t tail = tail_.load();
    return head == tail;
}

bool OrderRingBuffer::full() const {
    size_t head = head_.load();
    size_t tail = tail_.load();
    return ((tail + 1) & MASK) == head;
}

size_t OrderRingBuffer::size() const {
    size_t head = head_.load();
    size_t tail = tail_.load();
    size_t ans = 0;
    if (head <= tail) ans = tail - head;
    else ans = tail - head + CAPACITY;
    return ans;
}

size_t OrderRingBuffer::capacity() const {
    return CAPACITY - 1;
}

bool OrderRingBuffer::push(const OrderRequest& order) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    if (((tail + 1) & MASK) == head_.load(std::memory_order_acquire)) return false;
    buffer_[tail] = order;
    tail_.store((tail + 1) & MASK, std::memory_order_release);
    return true;
}

bool OrderRingBuffer::pop(OrderRequest& order) {
    size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    order = buffer_[head];
    head_.store((head + 1) & MASK, std::memory_order_release);
    return true;
}

TradeRingBuffer::TradeRingBuffer() {
    head_.store(0);
    tail_.store(0);
}

bool TradeRingBuffer::empty() const {
    size_t head = head_.load();
    size_t tail = tail_.load();
    return head == tail;
}

bool TradeRingBuffer::full() const {
    size_t head = head_.load();
    size_t tail = tail_.load();
    return ((tail + 1) & MASK) == head;
}

size_t TradeRingBuffer::size() const {
    size_t head = head_.load();
    size_t tail = tail_.load();
    size_t ans = 0;
    if (head <= tail) ans = tail - head;
    else ans = tail - head + CAPACITY;
    return ans;
}

size_t TradeRingBuffer::capacity() const {
    return CAPACITY - 1;
}

bool TradeRingBuffer::push(const TradeEvent& trade) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    if (((tail + 1) & MASK) == head_.load(std::memory_order_acquire)) return false;
    buffer_[tail] = trade;
    tail_.store((tail + 1) & MASK, std::memory_order_release);
    return true;
}

bool TradeRingBuffer::pop(TradeEvent& trade) {
    size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    trade = buffer_[head];
    head_.store((head + 1) & MASK, std::memory_order_release);
    return true;
}
