#include "exchange.hpp"

bool is_valid_msg_type(MsgType type) {
    switch (type) {
        case MsgType::NEW:
        case MsgType::CANCEL:
        case MsgType::MODIFY:
            return true;
    }

    return false;
}

bool is_valid_side(Side side) {
    return side == Side::BUY || side == Side::SELL;
}

bool is_valid_order_request(const OrderRequest& request) {
    if (!is_valid_msg_type(request.type)) {
        return false;
    }

    if (!is_valid_side(request.side)) {
        return false;
    }

    if (request.price > MAX_ORDER_PRICE) {
        return false;
    }

    if (request.type == MsgType::NEW && request.quantity == 0) {
        return false;
    }

    return true;
}

ClientPool::ClientPool() {
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        free_slots[i] = MAX_CLIENTS - 1 - i;
    }
}

ClientState* ClientPool::acquire(int fd) {
    if (free_count == 0) {
        return nullptr;
    }

    size_t slot = free_slots[--free_count];
    ClientState& client = clients[slot];
    client.fd = fd;
    client.bytes_received = 0;
    return &client;
}

void ClientPool::release(ClientState* client) {
    if (client == nullptr || client->fd == -1) {
        return;
    }

    size_t slot = static_cast<size_t>(client - clients.data());
    client->fd = -1;
    client->bytes_received = 0;
    free_slots[free_count++] = slot;
}

size_t ClientPool::available() const {
    return free_count;
}
