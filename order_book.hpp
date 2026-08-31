#pragma once

#include "exchange.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class TradeRingBuffer;

enum class OrderBookStatus : uint8_t {
    ACCEPTED,
    REJECTED,
    NOT_FOUND,
    DUPLICATE_ORDER_ID,
    BOOK_FULL,
    INVALID_INSTRUMENT,
    INVALID_PRICE,
    UNSUPPORTED_MODIFY
};

class OrderBook {
public:
    static constexpr uint64_t MAX_PRICE = MAX_ORDER_PRICE;
    static constexpr size_t MAX_RESTING_ORDERS = 1ULL << 20;
    static constexpr size_t MAX_ACTIVE_PRICE_LEVELS = 1ULL << 16;

    explicit OrderBook(uint16_t instrument_id = 0, TradeRingBuffer* trade_queue = nullptr);

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    OrderBookStatus process(const OrderRequest& request);

    bool best_bid(uint64_t& price) const;
    bool best_ask(uint64_t& price) const;

    size_t active_order_count() const;
    size_t active_price_level_count() const;
    uint16_t instrument_id() const;
    void set_trade_queue(TradeRingBuffer* trade_queue);

private:
    static constexpr uint32_t INVALID_INDEX = UINT32_MAX;
    static constexpr size_t RADIX_BITS = 6;
    static constexpr size_t RADIX_FANOUT = 1ULL << RADIX_BITS;
    static constexpr size_t RADIX_DEPTH = 5;
    static constexpr size_t MAX_RADIX_NODES = 1 + MAX_ACTIVE_PRICE_LEVELS * RADIX_DEPTH;
    static constexpr size_t ORDER_ID_INDEX_SIZE = 1ULL << 21;
    static constexpr size_t ORDER_ID_INDEX_MASK = ORDER_ID_INDEX_SIZE - 1;

    struct PriceLevel {
        uint32_t head_order = INVALID_INDEX;
        uint32_t tail_order = INVALID_INDEX;
        uint64_t total_quantity = 0;
        uint32_t order_count = 0;

        void reset();
    };

    struct OrderNode {
        uint64_t order_id = 0;
        uint64_t price = 0;
        uint64_t timestamp = 0;
        uint32_t quantity = 0;
        uint32_t prev_order = INVALID_INDEX;
        uint32_t next_order = INVALID_INDEX;
        uint32_t price_level = INVALID_INDEX;
        uint16_t instrument_id = 0;
        Side side = Side::BUY;

        void reset();
    };

    enum class IndexState : uint8_t {
        EMPTY,
        OCCUPIED,
        DELETED
    };

    struct OrderIndexEntry {
        uint64_t order_id = 0;
        uint32_t order_index = INVALID_INDEX;
        IndexState state = IndexState::EMPTY;

        void reset();
    };

    struct RadixNode {
        uint64_t child_bitmap = 0;
        std::array<uint32_t, RADIX_FANOUT> children{};
        uint32_t price_level = INVALID_INDEX;

        RadixNode();
        void reset();
    };

    struct RadixTree {
        std::vector<RadixNode> nodes;
        std::vector<uint32_t> free_nodes;
        size_t free_count = 0;

        RadixTree();

        bool insert(uint64_t price, uint32_t price_level);
        bool erase(uint64_t price);
        uint32_t find(uint64_t price) const;
        bool best_lowest(uint64_t& price) const;
        bool best_highest(uint64_t& price) const;

    private:
        uint32_t acquire_node();
        void release_node(uint32_t node_index);
        static uint32_t chunk(uint64_t price, size_t depth);
    };

    uint16_t instrument_id_;
    TradeRingBuffer* trade_queue_;

    std::vector<PriceLevel> price_levels_;
    std::vector<uint32_t> free_price_levels_;
    size_t free_price_level_count_;

    std::vector<OrderNode> orders_;
    std::vector<uint32_t> free_orders_;
    size_t free_order_count_;

    std::vector<OrderIndexEntry> order_index_;

    RadixTree bid_prices_;
    RadixTree ask_prices_;

    size_t active_orders_;
    size_t active_price_levels_;

    OrderBookStatus add_order(const OrderRequest& request);
    OrderBookStatus cancel_order(uint64_t order_id);
    OrderBookStatus modify_order(const OrderRequest& request);

    void match_buy(OrderRequest& incoming);
    void match_sell(OrderRequest& incoming);
    void match_level(OrderRequest& incoming, uint32_t level_index, uint64_t price, Side resting_side);
    void emit_trade(const OrderRequest& incoming, const OrderNode& resting, uint32_t quantity);

    OrderBookStatus rest_order(const OrderRequest& request);
    void append_order(uint32_t order_index, uint32_t level_index);
    void unlink_order(uint32_t order_index);
    void remove_empty_level(Side side, uint64_t price, uint32_t level_index);

    uint32_t acquire_order();
    void release_order(uint32_t order_index);
    uint32_t acquire_price_level();
    void release_price_level(uint32_t level_index);

    uint32_t find_order(uint64_t order_id) const;
    bool insert_order_index(uint64_t order_id, uint32_t order_index);
    bool erase_order_index(uint64_t order_id);

    static uint64_t hash_order_id(uint64_t order_id);
};
