#include "order_book.hpp"
#include "ring_buffer.hpp"

#include <algorithm>
#include <bit>

void OrderBook::PriceLevel::reset() {
    head_order = INVALID_INDEX;
    tail_order = INVALID_INDEX;
    total_quantity = 0;
    order_count = 0;
}

void OrderBook::OrderNode::reset() {
    order_id = 0;
    price = 0;
    timestamp = 0;
    quantity = 0;
    prev_order = INVALID_INDEX;
    next_order = INVALID_INDEX;
    price_level = INVALID_INDEX;
    instrument_id = 0;
    side = Side::BUY;
}

void OrderBook::OrderIndexEntry::reset() {
    order_id = 0;
    order_index = INVALID_INDEX;
    state = IndexState::EMPTY;
}

OrderBook::RadixNode::RadixNode() {
    reset();
}

void OrderBook::RadixNode::reset() {
    child_bitmap = 0;
    children.fill(INVALID_INDEX);
    price_level = INVALID_INDEX;
}

OrderBook::RadixTree::RadixTree()
    : nodes(MAX_RADIX_NODES),
      free_nodes(MAX_RADIX_NODES - 1),
      free_count(MAX_RADIX_NODES - 1) {
    for (size_t i = 0; i < MAX_RADIX_NODES - 1; i++) {
        free_nodes[i] = static_cast<uint32_t>(MAX_RADIX_NODES - 1 - i);
    }
}

bool OrderBook::RadixTree::insert(uint64_t price, uint32_t price_level) {
    if (price > MAX_PRICE) {
        return false;
    }

    uint32_t node_index = 0;
    size_t missing_nodes = 0;

    for (size_t depth = 0; depth < RADIX_DEPTH; depth++) {
        uint32_t child_index = chunk(price, depth);
        uint32_t next_node = nodes[node_index].children[child_index];

        if (next_node == INVALID_INDEX) {
            missing_nodes = RADIX_DEPTH - depth;
            break;
        }

        node_index = next_node;
    }

    if (free_count < missing_nodes) {
        return false;
    }

    node_index = 0;

    for (size_t depth = 0; depth < RADIX_DEPTH; depth++) {
        uint32_t child_index = chunk(price, depth);
        uint32_t& next_node = nodes[node_index].children[child_index];

        if (next_node == INVALID_INDEX) {
            uint32_t new_node = acquire_node();
            if (new_node == INVALID_INDEX) {
                return false;
            }

            next_node = new_node;
            nodes[node_index].child_bitmap |= (1ULL << child_index);
        }

        node_index = next_node;
    }

    if (nodes[node_index].price_level != INVALID_INDEX) {
        return false;
    }

    nodes[node_index].price_level = price_level;
    return true;
}

bool OrderBook::RadixTree::erase(uint64_t price) {
    if (price > MAX_PRICE) {
        return false;
    }

    std::array<uint32_t, RADIX_DEPTH + 1> path{};
    std::array<uint32_t, RADIX_DEPTH> chunks{};
    uint32_t node_index = 0;
    path[0] = node_index;

    for (size_t depth = 0; depth < RADIX_DEPTH; depth++) {
        uint32_t child_index = chunk(price, depth);
        uint32_t next_node = nodes[node_index].children[child_index];

        if (next_node == INVALID_INDEX) {
            return false;
        }

        chunks[depth] = child_index;
        node_index = next_node;
        path[depth + 1] = node_index;
    }

    if (nodes[node_index].price_level == INVALID_INDEX) {
        return false;
    }

    nodes[node_index].price_level = INVALID_INDEX;

    for (size_t depth = RADIX_DEPTH; depth > 0; depth--) {
        uint32_t current_node = path[depth];

        if (nodes[current_node].child_bitmap != 0 ||
            nodes[current_node].price_level != INVALID_INDEX) {
            break;
        }

        uint32_t parent_node = path[depth - 1];
        uint32_t child_index = chunks[depth - 1];

        nodes[parent_node].children[child_index] = INVALID_INDEX;
        nodes[parent_node].child_bitmap &= ~(1ULL << child_index);
        release_node(current_node);
    }

    return true;
}

uint32_t OrderBook::RadixTree::find(uint64_t price) const {
    if (price > MAX_PRICE) {
        return INVALID_INDEX;
    }

    uint32_t node_index = 0;

    for (size_t depth = 0; depth < RADIX_DEPTH; depth++) {
        uint32_t child_index = chunk(price, depth);
        uint32_t next_node = nodes[node_index].children[child_index];

        if (next_node == INVALID_INDEX) {
            return INVALID_INDEX;
        }

        node_index = next_node;
    }

    return nodes[node_index].price_level;
}

bool OrderBook::RadixTree::best_lowest(uint64_t& price) const {
    uint32_t node_index = 0;
    uint64_t result = 0;

    for (size_t depth = 0; depth < RADIX_DEPTH; depth++) {
        uint64_t bitmap = nodes[node_index].child_bitmap;
        if (bitmap == 0) {
            return false;
        }

        uint32_t child_index = static_cast<uint32_t>(std::countr_zero(bitmap));
        uint32_t next_node = nodes[node_index].children[child_index];
        if (next_node == INVALID_INDEX) {
            return false;
        }

        result = (result << RADIX_BITS) | child_index;
        node_index = next_node;
    }

    if (nodes[node_index].price_level == INVALID_INDEX) {
        return false;
    }

    price = result;
    return true;
}

bool OrderBook::RadixTree::best_highest(uint64_t& price) const {
    uint32_t node_index = 0;
    uint64_t result = 0;

    for (size_t depth = 0; depth < RADIX_DEPTH; depth++) {
        uint64_t bitmap = nodes[node_index].child_bitmap;
        if (bitmap == 0) {
            return false;
        }

        uint32_t child_index = 63U - static_cast<uint32_t>(std::countl_zero(bitmap));
        uint32_t next_node = nodes[node_index].children[child_index];
        if (next_node == INVALID_INDEX) {
            return false;
        }

        result = (result << RADIX_BITS) | child_index;
        node_index = next_node;
    }

    if (nodes[node_index].price_level == INVALID_INDEX) {
        return false;
    }

    price = result;
    return true;
}

uint32_t OrderBook::RadixTree::acquire_node() {
    if (free_count == 0) {
        return INVALID_INDEX;
    }

    uint32_t node_index = free_nodes[--free_count];
    nodes[node_index].reset();
    return node_index;
}

void OrderBook::RadixTree::release_node(uint32_t node_index) {
    if (node_index == 0 || node_index == INVALID_INDEX) {
        return;
    }

    nodes[node_index].reset();
    free_nodes[free_count++] = node_index;
}

uint32_t OrderBook::RadixTree::chunk(uint64_t price, size_t depth) {
    size_t shift = (RADIX_DEPTH - 1 - depth) * RADIX_BITS;
    return static_cast<uint32_t>((price >> shift) & (RADIX_FANOUT - 1));
}

OrderBook::OrderBook(uint16_t instrument_id, TradeRingBuffer* trade_queue)
    : instrument_id_(instrument_id),
      trade_queue_(trade_queue),
      price_levels_(MAX_ACTIVE_PRICE_LEVELS),
      free_price_levels_(MAX_ACTIVE_PRICE_LEVELS),
      free_price_level_count_(MAX_ACTIVE_PRICE_LEVELS),
      orders_(MAX_RESTING_ORDERS),
      free_orders_(MAX_RESTING_ORDERS),
      free_order_count_(MAX_RESTING_ORDERS),
      order_index_(ORDER_ID_INDEX_SIZE),
      active_orders_(0),
      active_price_levels_(0) {
    for (size_t i = 0; i < MAX_ACTIVE_PRICE_LEVELS; i++) {
        free_price_levels_[i] = static_cast<uint32_t>(MAX_ACTIVE_PRICE_LEVELS - 1 - i);
    }

    for (size_t i = 0; i < MAX_RESTING_ORDERS; i++) {
        free_orders_[i] = static_cast<uint32_t>(MAX_RESTING_ORDERS - 1 - i);
    }
}

OrderBookStatus OrderBook::process(const OrderRequest& request) {
    if (!is_valid_msg_type(request.type) || !is_valid_side(request.side)) {
        return OrderBookStatus::REJECTED;
    }

    if (request.price > MAX_PRICE) {
        return OrderBookStatus::INVALID_PRICE;
    }

    if (request.type == MsgType::NEW && request.quantity == 0) {
        return OrderBookStatus::REJECTED;
    }

    if (request.instrument_id != instrument_id_) {
        return OrderBookStatus::INVALID_INSTRUMENT;
    }

    switch (request.type) {
        case MsgType::NEW:
            return add_order(request);
        case MsgType::CANCEL:
            return cancel_order(request.order_id);
        case MsgType::MODIFY:
            return modify_order(request);
    }

    return OrderBookStatus::REJECTED;
}

bool OrderBook::best_bid(uint64_t& price) const {
    return bid_prices_.best_highest(price);
}

bool OrderBook::best_ask(uint64_t& price) const {
    return ask_prices_.best_lowest(price);
}

size_t OrderBook::active_order_count() const {
    return active_orders_;
}

size_t OrderBook::active_price_level_count() const {
    return active_price_levels_;
}

uint16_t OrderBook::instrument_id() const {
    return instrument_id_;
}

void OrderBook::set_trade_queue(TradeRingBuffer* trade_queue) {
    trade_queue_ = trade_queue;
}

OrderBookStatus OrderBook::add_order(const OrderRequest& request) {
    if (!is_valid_side(request.side) || request.quantity == 0) {
        return OrderBookStatus::REJECTED;
    }

    if (request.price > MAX_PRICE) {
        return OrderBookStatus::INVALID_PRICE;
    }

    if (find_order(request.order_id) != INVALID_INDEX) {
        return OrderBookStatus::DUPLICATE_ORDER_ID;
    }

    OrderRequest incoming = request;

    if (incoming.side == Side::BUY) {
        match_buy(incoming);
    } else {
        match_sell(incoming);
    }

    if (incoming.quantity == 0) {
        return OrderBookStatus::ACCEPTED;
    }

    return rest_order(incoming);
}

OrderBookStatus OrderBook::cancel_order(uint64_t order_id) {
    uint32_t order_index = find_order(order_id);
    if (order_index == INVALID_INDEX) {
        return OrderBookStatus::NOT_FOUND;
    }

    OrderNode& order = orders_[order_index];
    uint32_t level_index = order.price_level;
    uint64_t price = order.price;
    Side side = order.side;
    PriceLevel& level = price_levels_[level_index];

    level.total_quantity -= order.quantity;
    erase_order_index(order.order_id);
    unlink_order(order_index);
    release_order(order_index);
    active_orders_--;

    if (level.order_count == 0) {
        remove_empty_level(side, price, level_index);
    }

    return OrderBookStatus::ACCEPTED;
}

OrderBookStatus OrderBook::modify_order(const OrderRequest& request) {
    if (!is_valid_side(request.side)) {
        return OrderBookStatus::REJECTED;
    }

    if (request.price > MAX_PRICE) {
        return OrderBookStatus::INVALID_PRICE;
    }

    if (request.quantity == 0) {
        return cancel_order(request.order_id);
    }

    uint32_t order_index = find_order(request.order_id);
    if (order_index == INVALID_INDEX) {
        return OrderBookStatus::NOT_FOUND;
    }

    OrderNode& order = orders_[order_index];
    if (request.side != order.side || request.price != order.price ||
        request.quantity > order.quantity) {
        return OrderBookStatus::UNSUPPORTED_MODIFY;
    }

    PriceLevel& level = price_levels_[order.price_level];
    uint32_t quantity_reduction = order.quantity - request.quantity;
    order.quantity = request.quantity;
    level.total_quantity -= quantity_reduction;
    return OrderBookStatus::ACCEPTED;
}

void OrderBook::match_buy(OrderRequest& incoming) {
    uint64_t best_price = 0;

    while (incoming.quantity > 0 && ask_prices_.best_lowest(best_price)) {
        if (best_price > incoming.price) {
            break;
        }

        uint32_t level_index = ask_prices_.find(best_price);
        if (level_index == INVALID_INDEX) {
            break;
        }

        match_level(incoming, level_index, best_price, Side::SELL);
    }
}

void OrderBook::match_sell(OrderRequest& incoming) {
    uint64_t best_price = 0;

    while (incoming.quantity > 0 && bid_prices_.best_highest(best_price)) {
        if (best_price < incoming.price) {
            break;
        }

        uint32_t level_index = bid_prices_.find(best_price);
        if (level_index == INVALID_INDEX) {
            break;
        }

        match_level(incoming, level_index, best_price, Side::BUY);
    }
}

void OrderBook::match_level(
    OrderRequest& incoming,
    uint32_t level_index,
    uint64_t price,
    Side resting_side
) {
    PriceLevel& level = price_levels_[level_index];

    while (incoming.quantity > 0 && level.head_order != INVALID_INDEX) {
        uint32_t resting_index = level.head_order;
        OrderNode& resting = orders_[resting_index];
        uint32_t executed_quantity = std::min(incoming.quantity, resting.quantity);

        incoming.quantity -= executed_quantity;
        resting.quantity -= executed_quantity;
        level.total_quantity -= executed_quantity;
        emit_trade(incoming, resting, executed_quantity);

        if (resting.quantity == 0) {
            erase_order_index(resting.order_id);
            unlink_order(resting_index);
            release_order(resting_index);
            active_orders_--;
        }
    }

    if (level.order_count == 0) {
        remove_empty_level(resting_side, price, level_index);
    }
}

void OrderBook::emit_trade(
    const OrderRequest& incoming,
    const OrderNode& resting,
    uint32_t quantity
) {
    if (trade_queue_ == nullptr) {
        return;
    }

    TradeEvent trade{};
    trade.buy_order_id = incoming.side == Side::BUY ? incoming.order_id : resting.order_id;
    trade.sell_order_id = incoming.side == Side::SELL ? incoming.order_id : resting.order_id;
    trade.price = resting.price;
    trade.quantity = quantity;
    trade.instrument_id = incoming.instrument_id;

    while (!trade_queue_->push(trade)) {}
}

OrderBookStatus OrderBook::rest_order(const OrderRequest& request) {
    uint32_t order_index = acquire_order();
    if (order_index == INVALID_INDEX) {
        return OrderBookStatus::BOOK_FULL;
    }

    RadixTree& price_tree = request.side == Side::BUY ? bid_prices_ : ask_prices_;
    uint32_t level_index = price_tree.find(request.price);
    bool created_level = false;

    if (level_index == INVALID_INDEX) {
        level_index = acquire_price_level();
        if (level_index == INVALID_INDEX) {
            release_order(order_index);
            return OrderBookStatus::BOOK_FULL;
        }

        if (!price_tree.insert(request.price, level_index)) {
            release_price_level(level_index);
            release_order(order_index);
            return OrderBookStatus::BOOK_FULL;
        }

        created_level = true;
        active_price_levels_++;
    }

    OrderNode& order = orders_[order_index];
    order.order_id = request.order_id;
    order.price = request.price;
    order.timestamp = request.timestamp;
    order.quantity = request.quantity;
    order.instrument_id = request.instrument_id;
    order.side = request.side;
    order.price_level = level_index;

    append_order(order_index, level_index);

    if (!insert_order_index(order.order_id, order_index)) {
        PriceLevel& level = price_levels_[level_index];
        level.total_quantity -= order.quantity;
        unlink_order(order_index);
        release_order(order_index);

        if (created_level && level.order_count == 0) {
            remove_empty_level(request.side, request.price, level_index);
        }

        return OrderBookStatus::BOOK_FULL;
    }

    active_orders_++;
    return OrderBookStatus::ACCEPTED;
}

void OrderBook::append_order(uint32_t order_index, uint32_t level_index) {
    PriceLevel& level = price_levels_[level_index];
    OrderNode& order = orders_[order_index];

    order.prev_order = level.tail_order;
    order.next_order = INVALID_INDEX;
    order.price_level = level_index;

    if (level.tail_order != INVALID_INDEX) {
        orders_[level.tail_order].next_order = order_index;
    } else {
        level.head_order = order_index;
    }

    level.tail_order = order_index;
    level.total_quantity += order.quantity;
    level.order_count++;
}

void OrderBook::unlink_order(uint32_t order_index) {
    OrderNode& order = orders_[order_index];
    PriceLevel& level = price_levels_[order.price_level];

    if (order.prev_order != INVALID_INDEX) {
        orders_[order.prev_order].next_order = order.next_order;
    } else {
        level.head_order = order.next_order;
    }

    if (order.next_order != INVALID_INDEX) {
        orders_[order.next_order].prev_order = order.prev_order;
    } else {
        level.tail_order = order.prev_order;
    }

    order.prev_order = INVALID_INDEX;
    order.next_order = INVALID_INDEX;
    order.price_level = INVALID_INDEX;

    if (level.order_count > 0) {
        level.order_count--;
    }
}

void OrderBook::remove_empty_level(Side side, uint64_t price, uint32_t level_index) {
    if (side == Side::BUY) {
        bid_prices_.erase(price);
    } else {
        ask_prices_.erase(price);
    }

    release_price_level(level_index);
    active_price_levels_--;
}

uint32_t OrderBook::acquire_order() {
    if (free_order_count_ == 0) {
        return INVALID_INDEX;
    }

    uint32_t order_index = free_orders_[--free_order_count_];
    orders_[order_index].reset();
    return order_index;
}

void OrderBook::release_order(uint32_t order_index) {
    if (order_index == INVALID_INDEX) {
        return;
    }

    orders_[order_index].reset();
    free_orders_[free_order_count_++] = order_index;
}

uint32_t OrderBook::acquire_price_level() {
    if (free_price_level_count_ == 0) {
        return INVALID_INDEX;
    }

    uint32_t level_index = free_price_levels_[--free_price_level_count_];
    price_levels_[level_index].reset();
    return level_index;
}

void OrderBook::release_price_level(uint32_t level_index) {
    if (level_index == INVALID_INDEX) {
        return;
    }

    price_levels_[level_index].reset();
    free_price_levels_[free_price_level_count_++] = level_index;
}

uint32_t OrderBook::find_order(uint64_t order_id) const {
    size_t start = hash_order_id(order_id) & ORDER_ID_INDEX_MASK;

    for (size_t probe = 0; probe < ORDER_ID_INDEX_SIZE; probe++) {
        const OrderIndexEntry& entry = order_index_[(start + probe) & ORDER_ID_INDEX_MASK];

        if (entry.state == IndexState::EMPTY) {
            return INVALID_INDEX;
        }

        if (entry.state == IndexState::OCCUPIED && entry.order_id == order_id) {
            return entry.order_index;
        }
    }

    return INVALID_INDEX;
}

bool OrderBook::insert_order_index(uint64_t order_id, uint32_t order_index) {
    size_t start = hash_order_id(order_id) & ORDER_ID_INDEX_MASK;
    size_t first_deleted = ORDER_ID_INDEX_SIZE;

    for (size_t probe = 0; probe < ORDER_ID_INDEX_SIZE; probe++) {
        size_t entry_index = (start + probe) & ORDER_ID_INDEX_MASK;
        OrderIndexEntry& entry = order_index_[entry_index];

        if (entry.state == IndexState::OCCUPIED && entry.order_id == order_id) {
            return false;
        }

        if (entry.state == IndexState::DELETED && first_deleted == ORDER_ID_INDEX_SIZE) {
            first_deleted = entry_index;
            continue;
        }

        if (entry.state == IndexState::EMPTY) {
            OrderIndexEntry& target =
                order_index_[first_deleted == ORDER_ID_INDEX_SIZE ? entry_index : first_deleted];
            target.order_id = order_id;
            target.order_index = order_index;
            target.state = IndexState::OCCUPIED;
            return true;
        }
    }

    if (first_deleted != ORDER_ID_INDEX_SIZE) {
        OrderIndexEntry& target = order_index_[first_deleted];
        target.order_id = order_id;
        target.order_index = order_index;
        target.state = IndexState::OCCUPIED;
        return true;
    }

    return false;
}

bool OrderBook::erase_order_index(uint64_t order_id) {
    size_t start = hash_order_id(order_id) & ORDER_ID_INDEX_MASK;

    for (size_t probe = 0; probe < ORDER_ID_INDEX_SIZE; probe++) {
        OrderIndexEntry& entry = order_index_[(start + probe) & ORDER_ID_INDEX_MASK];

        if (entry.state == IndexState::EMPTY) {
            return false;
        }

        if (entry.state == IndexState::OCCUPIED && entry.order_id == order_id) {
            entry.order_id = 0;
            entry.order_index = INVALID_INDEX;
            entry.state = IndexState::DELETED;
            return true;
        }
    }

    return false;
}

uint64_t OrderBook::hash_order_id(uint64_t order_id) {
    order_id += 0x9e3779b97f4a7c15ULL;
    order_id = (order_id ^ (order_id >> 30)) * 0xbf58476d1ce4e5b9ULL;
    order_id = (order_id ^ (order_id >> 27)) * 0x94d049bb133111ebULL;
    return order_id ^ (order_id >> 31);
}
