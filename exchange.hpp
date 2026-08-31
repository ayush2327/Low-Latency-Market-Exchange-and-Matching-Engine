#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

enum class MsgType : uint8_t { NEW = 1, CANCEL = 2, MODIFY = 3 };
enum class Side    : uint8_t { BUY = 1, SELL = 2 };

static constexpr uint64_t MAX_ORDER_PRICE = (1ULL << 30) - 1;

struct alignas(32) OrderRequest {
    MsgType  type;
    Side     side;
    uint16_t instrument_id;
    uint32_t quantity;
    uint64_t price;
    uint64_t order_id;
    uint64_t timestamp;
};

struct alignas(32) TradeEvent {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint64_t price;
    uint32_t quantity;
    uint16_t instrument_id;
};

bool is_valid_msg_type(MsgType type);
bool is_valid_side(Side side);
bool is_valid_order_request(const OrderRequest& request);

static constexpr size_t MAX_CLIENTS = 1024;

struct ClientState {
    int fd = -1;
    std::array<char, sizeof(OrderRequest)> buffer{};
    size_t bytes_received = 0;
};

struct ClientPool {
    std::array<ClientState, MAX_CLIENTS> clients{};
    std::array<size_t, MAX_CLIENTS> free_slots{};
    size_t free_count = MAX_CLIENTS;

    ClientPool();

    ClientState* acquire(int fd);
    void release(ClientState* client);
    size_t available() const;
};
