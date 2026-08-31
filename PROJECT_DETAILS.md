# Low-Latency Market Exchange and Matching Engine

Last updated: 2026-08-31

Project folder:

```text
/Users/aryankumarsingh/Desktop/MatchEngine
```

This file records the full design, implementation details, benchmark results, and important concepts discussed while building the matching engine.

---

## 1. Project Summary

This project is a C++20 low-latency market exchange prototype.

The engine accepts binary TCP order messages, validates them, pushes them into a lock-free SPSC order queue, processes them through a single-threaded matching engine, stores resting orders in a custom limit order book, emits trade events into another SPSC queue, and finally consumes/prints the trade events.

High-level pipeline:

```text
TCP client
    -> non-blocking macOS kqueue network server
    -> validation
    -> OrderRingBuffer
    -> single-threaded matching engine
    -> OrderBook
    -> TradeRingBuffer
    -> trade publisher/output loop
```

The core design is intentionally exchange-style:

- one producer stage for network input
- one consumer/matching stage for order-book mutation
- one trade-output stage
- SPSC queues between stages
- no locks inside the order book
- preallocated pools for hot-path storage
- radix-bitmap price lookup for fast best-bid and best-ask search

---

## 2. Final Project Name Options

Simple project/repo name:

```text
MatchEngine
```

CV-style project title:

```text
Low-Latency Market Exchange and Matching Engine
```

Other possible titles:

- High-Performance Limit Order Book and Matching Engine
- C++20 Low-Latency Market Exchange Platform
- Lock-Free Market Exchange and Order Matching Engine
- Low-Latency Order Matching and Trade Execution Engine

Recommended CV title:

```text
Low-Latency Market Exchange and Matching Engine
```

---

## 3. Current Folder Structure

```text
MatchEngine/
├── include/
│   ├── exchange.hpp
│   ├── ring_buffer.hpp
│   ├── order_book.hpp
│   ├── network.hpp
│   ├── engine.hpp
│   └── shutdown.hpp
├── src/
│   ├── exchange.cpp
│   ├── ring_buffer.cpp
│   ├── order_book.cpp
│   ├── network.cpp
│   ├── engine.cpp
│   ├── shutdown.cpp
│   └── main.cpp
├── benchmark/
│   └── benchmark.cpp
├── Makefile
└── PROJECT_DETAILS.md
```

---

## 4. Build Commands

Build main engine:

```bash
make
```

Build benchmark binary:

```bash
make benchmark
```

Run benchmark with default operation count:

```bash
make run-benchmark
```

Run benchmark manually with 1,000,000 operations:

```bash
./match_engine_benchmark 1000000
```

Clean build artifacts:

```bash
make clean
```

Full rebuild:

```bash
make clean && make && make benchmark
```

Compiler settings:

```text
C++ standard: C++20
Main binary:  -O2
Benchmark:    -O3 -march=native -DNDEBUG
Compiler:     clang++
Include path: -Iinclude
```

---

## 5. Core Protocol Types

Defined in:

```text
include/exchange.hpp
src/exchange.cpp
```

### MsgType

```cpp
enum class MsgType : uint8_t {
    NEW = 1,
    CANCEL = 2,
    MODIFY = 3
};
```

Meaning:

- `NEW`: add a new order
- `CANCEL`: cancel an existing resting order
- `MODIFY`: modify an existing resting order

### Side

```cpp
enum class Side : uint8_t {
    BUY = 1,
    SELL = 2
};
```

Meaning:

- `BUY`: bid side
- `SELL`: ask side

### OrderRequest

```cpp
struct alignas(32) OrderRequest {
    MsgType  type;
    Side     side;
    uint16_t instrument_id;
    uint32_t quantity;
    uint64_t price;
    uint64_t order_id;
    uint64_t timestamp;
};
```

This is the binary order message received over TCP and pushed through the order queue.

Important fields:

- `type`: order action, such as `NEW`, `CANCEL`, or `MODIFY`
- `side`: buy or sell
- `instrument_id`: currently expected to be `0`
- `quantity`: order quantity
- `price`: integer price
- `order_id`: unique order identifier
- `timestamp`: timestamp supplied by sender/benchmark

Current max price:

```cpp
MAX_ORDER_PRICE = (1ULL << 30) - 1
```

This means prices are treated as 30-bit unsigned integer values.

### TradeEvent

```cpp
struct alignas(32) TradeEvent {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint64_t price;
    uint32_t quantity;
    uint16_t instrument_id;
};
```

This is produced by the matching engine when two orders execute.

Important fields:

- `buy_order_id`: order id of the buy order involved in trade
- `sell_order_id`: order id of the sell order involved in trade
- `price`: execution price
- `quantity`: executed quantity
- `instrument_id`: instrument id

---

## 6. Validation Rules

Implemented in:

```text
src/exchange.cpp
```

Validation currently checks:

- message type must be `NEW`, `CANCEL`, or `MODIFY`
- side must be `BUY` or `SELL`
- price must be less than or equal to `MAX_ORDER_PRICE`
- `NEW` order quantity must be greater than zero

Invalid network messages are rejected and the client connection is closed.

Order book also validates:

- valid message type
- valid side
- valid price
- non-zero quantity for `NEW`
- matching `instrument_id`

Current instrument behavior:

- only one order book is used
- main creates the order book with `INSTRUMENT_ID = 0`
- requests with different `instrument_id` return `INVALID_INSTRUMENT`

---

## 7. Networking Layer

Implemented in:

```text
include/network.hpp
src/network.cpp
```

Function:

```cpp
void listen_server(
    uint16_t port,
    OrderRingBuffer& order_queue,
    const std::atomic<bool>& running
);
```

The server runs on:

```text
port 9000
```

### Important Socket Concepts

Socket creation:

```cpp
socket(AF_INET, SOCK_STREAM, 0)
```

Meaning:

- `AF_INET`: IPv4
- `SOCK_STREAM`: TCP stream socket
- protocol `0`: let OS choose the default protocol for TCP

Server socket:

- one listening file descriptor
- accepts new client connections

Client socket:

- every accepted TCP connection gets its own client file descriptor
- each client fd is watched by `kqueue`

Binding:

```cpp
bind(server_fd, ...)
```

Meaning:

- attaches the server socket to an IP address and port
- for this project, it binds to `INADDR_ANY`, meaning all local interfaces
- port is converted using `htons(port)`

Listening:

```cpp
listen(server_fd, SOMAXCONN)
```

Meaning:

- tells OS this socket will accept incoming TCP connections
- OS maintains a pending connection queue

Accepting:

```cpp
accept(server_fd, ...)
```

Meaning:

- creates a new client fd for an accepted connection
- server fd remains the listening socket
- client fd is used for reading that client’s data

### macOS Event Loop

The project uses macOS `kqueue`.

Important calls:

```cpp
kqueue()
EV_SET(...)
kevent(...)
```

Meaning:

- `kqueue()` creates an OS event queue
- `EV_SET` prepares a kernel event registration
- `kevent` registers events or waits for ready events

Server fd is registered for read events:

```cpp
EVFILT_READ
EV_ADD | EV_ENABLE | EV_CLEAR
```

Client fds are also registered for read events after `accept`.

Current max events per `kevent` call:

```cpp
MAX_EVENTS = 64
```

This does not mean only 64 total clients can exist.

It means:

- each `kevent` call returns up to 64 ready events
- if more than 64 clients are ready, the remaining ready events are processed in later event-loop iterations

### Non-blocking Sockets

Sockets are made non-blocking using:

```cpp
fcntl(fd, F_SETFL, flags | O_NONBLOCK)
```

Why:

- a slow client should not block the whole server
- `recv` returns available bytes immediately
- when kernel buffer is empty, `recv` returns `EAGAIN` or `EWOULDBLOCK`

### Partial TCP Message Handling

TCP is a byte stream.

One `recv` is not guaranteed to return one full `OrderRequest`.

Problem solved:

- slow client may send only half an order
- server must remember partial bytes
- next read continues from previous offset

Each client has:

```cpp
std::array<char, sizeof(OrderRequest)> buffer;
size_t bytes_received;
```

Logic:

```text
read bytes into per-client buffer
increase bytes_received
if bytes_received == sizeof(OrderRequest):
    memcpy bytes into OrderRequest
    reset bytes_received to 0
    validate order
    push into OrderRingBuffer
```

### Socket Options

Used options:

```cpp
SO_REUSEADDR
TCP_NODELAY
```

Meaning:

- `SO_REUSEADDR`: makes restarting server on same port easier
- `TCP_NODELAY`: disables Nagle’s algorithm to reduce latency for small messages

---

## 8. Fixed Client Pool

Defined in:

```text
include/exchange.hpp
src/exchange.cpp
```

Current max clients:

```cpp
MAX_CLIENTS = 1024
```

Data structure:

```cpp
struct ClientPool {
    std::array<ClientState, MAX_CLIENTS> clients;
    std::array<size_t, MAX_CLIENTS> free_slots;
    size_t free_count;
};
```

Reason for fixed pool:

- avoid heap allocation per connection
- avoid `unordered_map` on the hot path
- avoid fd-indexed arrays because file descriptors are not guaranteed to be dense or serial

Each `ClientState` stores:

- client fd
- partial message buffer
- number of bytes received so far

Free-list initialization:

```cpp
for (size_t i = 0; i < MAX_CLIENTS; i++) {
    free_slots[i] = MAX_CLIENTS - 1 - i;
}
```

Because `acquire` pops from the end:

```cpp
size_t slot = free_slots[--free_count];
```

This makes first acquired slot equal to `0`.

`free_slots[i] = i` would also work, but first acquired slot would be `MAX_CLIENTS - 1`.

How a client is released:

```text
client fd is closed
client state is reset
slot index is pushed back into free_slots
free_count increases
```

How slot index is recovered:

```cpp
size_t slot = client - clients.data();
```

Meaning:

- `clients.data()` gives pointer to first array element
- pointer subtraction gives index of current client inside the array

---

## 9. SPSC Ring Buffers

Defined in:

```text
include/ring_buffer.hpp
src/ring_buffer.cpp
```

Two ring buffers exist:

```cpp
OrderRingBuffer
TradeRingBuffer
```

Purpose:

- `OrderRingBuffer`: network producer -> matching consumer
- `TradeRingBuffer`: matching producer -> trade publisher consumer

Current capacity:

```cpp
CAPACITY = 4096
```

Usable capacity:

```text
4095
```

Reason:

- one slot is intentionally left empty
- this makes full and empty detection simple

Full condition:

```cpp
((tail + 1) & MASK) == head
```

Empty condition:

```cpp
head == tail
```

Mask:

```cpp
MASK = CAPACITY - 1
```

Because capacity is a power of two, wraparound can use:

```cpp
(index + 1) & MASK
```

instead of:

```cpp
(index + 1) % CAPACITY
```

This is faster.

### Atomic Design

Each queue has:

```cpp
alignas(64) std::atomic<size_t> head_;
alignas(64) std::atomic<size_t> tail_;
```

Meaning:

- `head_`: consumer-owned read index
- `tail_`: producer-owned write index
- `alignas(64)`: tries to keep indices on separate cache lines to reduce false sharing

### Memory Ordering

Producer push:

```cpp
tail = tail_.load(std::memory_order_relaxed);
head = head_.load(std::memory_order_acquire);
buffer_[tail] = item;
tail_.store(next_tail, std::memory_order_release);
```

Consumer pop:

```cpp
head = head_.load(std::memory_order_relaxed);
tail = tail_.load(std::memory_order_acquire);
item = buffer_[head];
head_.store(next_head, std::memory_order_release);
```

Meaning:

- `relaxed`: safe for the index owned by the current thread
- `acquire`: used when reading index published by the other thread
- `release`: used when publishing a new index to the other thread

This is correct for SPSC because:

- only producer writes `tail_`
- only consumer writes `head_`
- item write happens before producer publishes `tail_`
- item read happens after consumer sees producer’s `tail_`

### Push/Pop Return Values

```cpp
bool push(...)
bool pop(...)
```

Meaning:

- `push` returns `false` if queue is full
- `pop` returns `false` if queue is empty

Current behavior in producer hot paths:

```cpp
while (!queue.push(item)) {}
```

This means:

- if queue is full, producer spins
- data is not dropped
- consumer runs on another thread and can free space

---

## 10. Threading Model

Defined in:

```text
src/main.cpp
src/engine.cpp
```

Main creates:

```cpp
OrderRingBuffer order_queue;
TradeRingBuffer trade_queue;
OrderBook order_book(INSTRUMENT_ID, &trade_queue);
std::atomic<bool> running{true};
```

Then starts:

```cpp
matching_thread
trade_publisher_thread
```

Main thread runs:

```cpp
listen_server(PORT, order_queue, running);
```

Architecture:

```text
main thread:
    network server

matching_thread:
    pop OrderRequest from OrderRingBuffer
    call order_book.process(order)

trade_publisher_thread:
    pop TradeEvent from TradeRingBuffer
    print trade event
```

Important:

- order book is single-threaded
- only matching thread mutates order book
- therefore order book internals do not need locks or atomics
- SPSC queues handle communication between threads

---

## 11. Shutdown Handling

Defined in:

```text
include/shutdown.hpp
src/shutdown.cpp
```

Signals handled:

```text
SIGINT
SIGTERM
```

This supports clean stopping using:

```text
Ctrl+C
```

Shutdown flag:

```cpp
volatile std::sig_atomic_t stop_requested
```

Network loop checks:

```cpp
running.load(std::memory_order_acquire) && !shutdown_requested()
```

`kevent` uses a timeout:

```text
100 ms
```

Reason:

- without timeout, server could block inside `kevent`
- with timeout, it periodically wakes up and sees shutdown request

After network exits:

```text
request_shutdown()
running = false
join matching thread
join trade publisher thread
close server fd
close kqueue fd
close all client fds
```

Matching loop drains remaining orders:

```cpp
while (running || !order_queue.empty())
```

Trade publisher drains remaining trades:

```cpp
while (running || !trade_queue.empty())
```

---

## 12. Order Book Overview

Defined in:

```text
include/order_book.hpp
src/order_book.cpp
```

Class:

```cpp
class OrderBook
```

Important constants:

```cpp
MAX_RESTING_ORDERS       = 1ULL << 20
MAX_ACTIVE_PRICE_LEVELS  = 1ULL << 16
ORDER_ID_INDEX_SIZE      = 1ULL << 21
MAX_PRICE                = (1ULL << 30) - 1
RADIX_BITS               = 6
RADIX_FANOUT             = 64
RADIX_DEPTH              = 5
```

Meaning:

- max resting orders: 1,048,576
- max active price levels: 65,536
- order id hash index slots: 2,097,152
- max price range: 30-bit integer price
- radix chunks: 6 bits per level
- radix tree depth: 5 levels because `5 * 6 = 30`

Current supported operations:

- add new order
- cancel existing order
- modify existing order quantity downward
- quantity zero modify becomes cancel
- price change modify is currently unsupported
- quantity increase modify is currently unsupported

---

## 13. Buy/Sell Matching Rules

Best buy price:

```text
highest bid
```

Best sell price:

```text
lowest ask
```

Buy order matching:

```text
while incoming buy quantity > 0:
    find lowest ask
    if lowest ask > buy limit price:
        stop
    match against FIFO sell orders at that ask price
```

Sell order matching:

```text
while incoming sell quantity > 0:
    find highest bid
    if highest bid < sell limit price:
        stop
    match against FIFO buy orders at that bid price
```

Execution price:

```text
resting order price
```

FIFO behavior:

- orders at same price are matched in insertion order
- oldest resting order at a price level executes first

---

## 14. Order Book Data Structures

The order book is not one single data structure. It is a group of connected data structures.

### PriceLevel

```cpp
struct PriceLevel {
    uint32_t head_order;
    uint32_t tail_order;
    uint64_t total_quantity;
    uint32_t order_count;
};
```

Purpose:

- stores FIFO list metadata for one active price level
- does not store the price directly
- price is known through the radix tree mapping

Fields:

- `head_order`: first/oldest order at this price
- `tail_order`: last/newest order at this price
- `total_quantity`: total remaining quantity at price level
- `order_count`: number of resting orders at price level

Why both head and tail:

- head is needed for matching oldest order first
- tail is needed for O(1) append of new resting orders

### OrderNode

```cpp
struct OrderNode {
    uint64_t order_id;
    uint64_t price;
    uint64_t timestamp;
    uint32_t quantity;
    uint32_t prev_order;
    uint32_t next_order;
    uint32_t price_level;
    uint16_t instrument_id;
    Side side;
};
```

Purpose:

- stores actual resting order data
- also stores intrusive linked-list pointers

Fields:

- `order_id`: unique id
- `price`: limit price
- `timestamp`: timestamp
- `quantity`: remaining quantity
- `prev_order`: previous order in same price level FIFO list
- `next_order`: next order in same price level FIFO list
- `price_level`: index of owning price level
- `instrument_id`: currently `0`
- `side`: buy or sell

Why intrusive list:

- no separate linked-list node allocation
- order itself contains next/previous indexes
- O(1) append
- O(1) unlink when cancelling known order

### Price Level FIFO List

For each price:

```text
PriceLevel.head_order -> oldest order
PriceLevel.tail_order -> newest order
```

Example:

```text
PriceLevel for price 100:

head_order
   |
   v
Order A <-> Order B <-> Order C
                         ^
                         |
                     tail_order
```

Matching consumes from the head:

```text
Order A first, then Order B, then Order C
```

New passive order is appended at the tail:

```text
Order A <-> Order B <-> Order C <-> New Order D
```

### OrderIndexEntry

```cpp
struct OrderIndexEntry {
    uint64_t order_id;
    uint32_t order_index;
    IndexState state;
};
```

Purpose:

- maps `order_id` to `orders_` array index
- required for O(1) average cancel/modify lookup

Index states:

```cpp
EMPTY
OCCUPIED
DELETED
```

Why `DELETED` exists:

- open-addressed hash table cannot simply mark removed slots as empty
- doing that could break probe chains
- tombstone preserves lookup correctness

Current concern:

- long-running cancel-heavy workloads can accumulate tombstones
- this can increase probing over time
- future improvement could add tombstone cleanup or rehashing strategy

### RadixNode

```cpp
struct RadixNode {
    uint64_t child_bitmap;
    std::array<uint32_t, 64> children;
    uint32_t price_level;
};
```

Purpose:

- node in radix bitmap tree
- stores active child branches using a bitmap
- maps active prices to price-level indexes

Fields:

- `child_bitmap`: which child slots exist
- `children[64]`: child node indexes
- `price_level`: valid only at leaf node

---

## 15. Storage / Memory Allocation

The order book uses `std::vector`, but vectors are allocated upfront in the constructor.

Important:

- vector storage is heap-backed
- hot path does not call `new` per order
- hot path uses preallocated pools and free lists

Major storage:

```cpp
std::vector<PriceLevel> price_levels_;
std::vector<uint32_t> free_price_levels_;
std::vector<OrderNode> orders_;
std::vector<uint32_t> free_orders_;
std::vector<OrderIndexEntry> order_index_;
RadixTree bid_prices_;
RadixTree ask_prices_;
```

Pool behavior:

- `orders_` stores all possible resting orders
- `free_orders_` stores available order indexes
- `price_levels_` stores all possible active price levels
- `free_price_levels_` stores available price-level indexes
- radix trees store active prices for bid and ask sides

When order is added:

```text
acquire free order index
find or create price level
append order to price-level FIFO list
insert order_id -> order_index into hash index
```

When order is canceled:

```text
find order index by order_id
remove from order-id index
unlink from price-level FIFO list
release order slot back to free_orders_
remove price level if it becomes empty
```

---

## 16. Radix Bitmap Tree

The order book uses separate radix bitmap trees:

```text
bid_prices_
ask_prices_
```

Purpose:

- find best bid quickly
- find best ask quickly
- support sparse price values without dense array over full price range

Why not dense price ladder:

- price range can be `0` to around `1e9`
- allocating one array slot per price would waste too much memory
- sparse prices require a sparse structure

Why radix bitmap:

- price is split into fixed-size bit chunks
- each node has a 64-bit bitmap for child existence
- best price can be found by bit operations instead of scanning many prices

Constants:

```cpp
RADIX_BITS = 6
RADIX_FANOUT = 64
RADIX_DEPTH = 5
```

Reason:

```text
5 levels * 6 bits per level = 30 bits
```

30 bits covers:

```text
0 to (2^30 - 1)
```

### Chunk Extraction

For a price, each level reads the next 6-bit chunk:

```cpp
(price >> shift) & 63
```

This extracts one chunk in one operation.

Example:

```text
price = 4003
30-bit representation is split as:

[bits 29..24] [bits 23..18] [bits 17..12] [bits 11..6] [bits 5..0]
   chunk 0      chunk 1      chunk 2      chunk 3     chunk 4
```

Each chunk value is between:

```text
0 and 63
```

### Bitmap Optimization

Each node has:

```cpp
uint64_t child_bitmap
```

If child index `i` exists:

```cpp
child_bitmap has bit i set
```

Set child bit:

```cpp
child_bitmap |= (1ULL << child_index)
```

Clear child bit:

```cpp
child_bitmap &= ~(1ULL << child_index)
```

Find lowest active child:

```cpp
std::countr_zero(bitmap)
```

Find highest active child:

```cpp
63 - std::countl_zero(bitmap)
```

This avoids checking all 64 child slots one by one.

### Best Ask

Best ask is lowest sell price.

Logic:

```text
start at root
at every level choose lowest set bit in bitmap
descend to that child
after 5 levels, reconstructed price is lowest active ask
```

### Best Bid

Best bid is highest buy price.

Logic:

```text
start at root
at every level choose highest set bit in bitmap
descend to that child
after 5 levels, reconstructed price is highest active bid
```

### Example Prices

Suppose active prices are:

```text
10, 300, 4000, 4001, 4002, 4003, 500000
```

The radix tree stores only paths that are actually needed.

It is like a trie over price bits, but each node has:

- bitmap to know which children exist
- child index array to move to existing children

For best ask:

```text
lowest active price = 10
```

The tree finds this by repeatedly selecting the lowest set child bit.

For best bid:

```text
highest active price = 500000
```

The tree finds this by repeatedly selecting the highest set child bit.

### Radix Complexity

Insert price:

```text
O(RADIX_DEPTH)
```

For current constants:

```text
O(5)
```

This can also be described as:

```text
O(log_64(price_range))
```

Because each level consumes 6 bits, not 1 bit.

Best bid / best ask:

```text
O(RADIX_DEPTH) = O(5)
```

Find exact price:

```text
O(RADIX_DEPTH) = O(5)
```

Erase price:

```text
O(RADIX_DEPTH) = O(5)
```

---

## 17. Order Book Operations

### Add New Order

Entry point:

```cpp
OrderBookStatus OrderBook::add_order(const OrderRequest& request)
```

Steps:

```text
validate side and quantity
validate price
check duplicate order id
copy request into incoming
if buy: match against asks
if sell: match against bids
if incoming quantity becomes zero: done
otherwise rest remaining quantity into book
```

### Rest Passive Order

Entry point:

```cpp
OrderBookStatus OrderBook::rest_order(const OrderRequest& request)
```

Steps:

```text
acquire order slot
find existing price level in radix tree
if no level exists:
    acquire price level
    insert price into radix tree
fill OrderNode fields
append order to FIFO list
insert order id into order_index_
increase active order count
```

### Cancel Order

Entry point:

```cpp
OrderBookStatus OrderBook::cancel_order(uint64_t order_id)
```

Steps:

```text
find order index by order_id
if not found: return NOT_FOUND
subtract quantity from price level
erase order id from hash index
unlink order from FIFO list
release order slot
decrease active order count
if price level is empty:
    remove price from radix tree
    release price level
```

### Modify Order

Entry point:

```cpp
OrderBookStatus OrderBook::modify_order(const OrderRequest& request)
```

Supported:

- reduce quantity in place
- quantity zero becomes cancel

Unsupported:

- price change
- side change
- quantity increase

Reason:

- increasing quantity or changing price usually changes priority
- implementing full modify requires more exchange-specific rules

### Match Buy

Entry point:

```cpp
void OrderBook::match_buy(OrderRequest& incoming)
```

Logic:

```text
while incoming quantity > 0:
    get lowest ask
    if lowest ask > incoming buy price:
        stop
    match against FIFO orders at that ask price
```

### Match Sell

Entry point:

```cpp
void OrderBook::match_sell(OrderRequest& incoming)
```

Logic:

```text
while incoming quantity > 0:
    get highest bid
    if highest bid < incoming sell price:
        stop
    match against FIFO orders at that bid price
```

### Emit Trade

Entry point:

```cpp
void OrderBook::emit_trade(...)
```

Creates:

```cpp
TradeEvent
```

Then pushes into:

```cpp
TradeRingBuffer
```

Current behavior:

```cpp
while (!trade_queue_->push(trade)) {}
```

Meaning:

- if trade queue is full, matching thread waits/spins
- trade is not dropped

---

## 18. Time Complexity Summary

Let:

```text
d = RADIX_DEPTH = 5
k = number of resting orders actually matched by an aggressive order
p = number of hash probes for order-id lookup
```

Best bid lookup:

```text
O(d) = O(5)
```

Best ask lookup:

```text
O(d) = O(5)
```

Add passive order at existing price:

```text
average O(p + d), practically near O(1)
```

Add passive order at new price:

```text
average O(p + d), practically near O(1)
```

Cancel order:

```text
average O(p + d), practically near O(1)
```

Modify order quantity down:

```text
average O(p), practically near O(1)
```

Aggressive matching:

```text
O(k * d)
```

If one incoming order matches many resting orders, `k` can be large.

This is unavoidable because every execution must actually consume one or more resting orders and emit trades.

Main potential problematic case:

```text
one aggressive order consumes many tiny resting orders
```

That case is O(number of orders matched).

---

## 19. Benchmarking

Benchmark file:

```text
benchmark/benchmark.cpp
```

Build:

```bash
make benchmark
```

Run:

```bash
./match_engine_benchmark 1000000
```

Benchmarks implemented:

- `spsc.order_ring_buffer`
- `spsc.trade_ring_buffer`
- `order_book.passive_insert`
- `order_book.aggressive_match`
- `order_book.cancel`
- `order_book.mixed`
- `pipeline.order_to_match_to_trade`

Benchmark output includes:

- operations
- accepted count
- total milliseconds
- ns/op
- ops/sec
- average latency
- p50 latency
- p95 latency
- p99 latency
- p99.9 latency
- max latency

Order-book benchmarks measure per-operation latency using `Clock::now()`.

Note:

- per-operation timing adds measurement overhead
- throughput numbers are still useful for comparison
- a future version can add warmup, CPU pinning, CSV output, and no-per-op-timestamp throughput mode

---

## 20. Benchmark Results

Run configuration:

```text
operations_per_benchmark = 1000000
max_resting_orders       = 1048576
max_active_price_levels  = 65536
```

Results:

```text
spsc.order_ring_buffer
ops=1000000 accepted=1000000 total_ms=71.579 ns/op=71.6 ops/sec=13970643

spsc.trade_ring_buffer
ops=1000000 accepted=1000000 total_ms=51.864 ns/op=51.9 ops/sec=19281135

order_book.passive_insert
ops=1000000 accepted=1000000 total_ms=83.682 ns/op=83.7 ops/sec=11950013
avg=66 p50=42 p95=125 p99=209 p99.9=1500 max=28667

order_book.aggressive_match
ops=1000000 accepted=1000000 total_ms=116.780 ns/op=116.8 ops/sec=8563125
avg=99 p50=84 p95=208 p99=250 p99.9=375 max=34208

order_book.cancel
ops=1000000 accepted=1000000 total_ms=72.138 ns/op=72.1 ops/sec=13862239
avg=55 p50=42 p95=125 p99=167 p99.9=250 max=8208

order_book.mixed
ops=1000000 accepted=1000000 total_ms=83.042 ns/op=83.0 ops/sec=12042142
avg=66 p50=42 p95=166 p99=208 p99.9=292 max=25833

pipeline.order_to_match_to_trade
ops=1000000 accepted=1000000 total_ms=85.210 ns/op=85.2 ops/sec=11735718
```

Summary:

- order ring buffer: about 13.97M ops/sec
- trade ring buffer: about 19.28M ops/sec
- passive inserts: about 11.95M inserts/sec
- aggressive matches: about 8.56M matches/sec
- cancels: about 13.86M cancels/sec
- mixed workload: about 12.04M ops/sec
- internal order-to-trade pipeline: about 11.73M ops/sec

Strong latency points:

- passive insert p50: 42 ns
- passive insert p99: 209 ns
- aggressive match p50: 84 ns
- aggressive match p99: 250 ns
- cancel p50: 42 ns
- cancel p99: 167 ns
- mixed workload p50: 42 ns
- mixed workload p99: 208 ns

---

## 21. Round-Trip Latency Clarification

True exchange round-trip latency usually means:

```text
client sends order
-> server receives over socket
-> server parses and validates
-> order enters queue
-> matching engine processes
-> trade/ack is produced
-> server sends response over socket
-> client receives response
```

The current benchmark:

```text
pipeline.order_to_match_to_trade
```

measures:

```text
OrderRequest created in process
-> pushed into OrderRingBuffer
-> consumed by matching thread
-> processed by OrderBook
-> TradeEvent emitted into TradeRingBuffer
-> consumed by trade-output thread
```

So it is better described as:

```text
internal order-to-trade latency
```

or:

```text
core matching pipeline latency
```

It excludes:

- socket receive latency
- network stack latency
- client-server round trip
- response send latency

CV phrasing should say:

```text
internal order-to-trade pipeline
```

not full network round trip.

---

## 22. CV Points

Balanced CV bullets:

- Built C++20 low-latency exchange core with kqueue TCP ingress, lock-free SPSC queues and FIFO price-time matching
- Achieved 11.7M ops/sec in order-to-trade pipeline benchmarks, averaging 85.2 ns/op core processing latency
- Optimized order book to 11.9M inserts/sec, 8.6M matches/sec, 13.8M cancels/sec, with p99 under 250ns
- Used radix-bitmap best-price lookup, intrusive FIFO lists and fixed memory pools to avoid hot-path allocations

Longer version:

- Engineered a C++20 low-latency exchange core with kqueue-based TCP ingress, lock-free SPSC queues, and FIFO price-time matching.
- Achieved 11.7M ops/sec in internal order-to-trade pipeline benchmarks, with average core processing latency of 85.2 ns/op.
- Optimized order book hot paths with custom data structures, reaching 11.9M inserts/sec, 8.6M matches/sec, and 13.8M cancels/sec.
- Used radix-bitmap best-price lookup, intrusive FIFO order lists, fixed-size memory pools, and preallocation to avoid hot-path heap allocation.

Recommended CV project title:

```text
Low-Latency Market Exchange and Matching Engine
```

---

## 23. Important Design Decisions

### macOS Instead of Linux

Original Linux design idea involved `epoll`.

Final project targets macOS, so networking uses:

```text
kqueue
```

### Fixed Client Pool Instead of unordered_map

Rejected approach:

```text
unordered_map<fd, client_state>
```

Reason:

- heap allocation
- hashing overhead
- less predictable latency

Also rejected:

```text
array indexed directly by fd
```

Reason:

- file descriptors are not guaranteed to be serial/dense
- could waste lots of memory

Chosen:

```text
fixed ClientPool + kqueue udata pointer
```

### Single Matching Thread

Order book uses one matching thread.

Reason:

- avoids locks inside order book
- keeps deterministic order processing
- matches exchange-style partitioning
- SPSC queue name implies single producer and single consumer

### SPSC Ring Buffer

Chosen because:

- one producer writes orders
- one consumer matches orders
- simpler and faster than MPSC/MPMC
- acquire/release memory ordering is enough

### Radix Bitmap Instead of Dense Price Array

Rejected:

```text
dense array for every possible price
```

Reason:

- price range can be very large
- if prices range from 0 to 1e9, dense ladder wastes memory

Chosen:

```text
radix bitmap tree
```

Reason:

- supports sparse prices
- fast best price lookup
- bounded depth
- bit operations avoid child scanning

### Preallocated Pools

Chosen because:

- no per-order heap allocation in hot path
- stable memory layout
- predictable latency
- cache-friendly compared with pointer-heavy heap lists

---

## 24. Current Limitations

The current project is fast and functional, but still a prototype.

Known limitations:

- trade publisher prints using `std::cout`, which is not suitable for a real low-latency output path
- no actual binary response/ack is sent back to client yet
- no formal unit test suite yet
- no CSV benchmark output yet
- no client load generator yet
- no CPU pinning or real-time scheduling in benchmark
- no warmup phase in benchmark
- benchmark latency uses `Clock::now()` per operation, which adds overhead
- order-id hash table tombstones can accumulate under long cancel-heavy workloads
- `MODIFY` only supports quantity reduction or cancel-by-zero
- network benchmark is not yet measuring true client/server round-trip latency
- protocol still contains `instrument_id`, although current design uses one instrument/order book

---

## 25. Potential Future Improvements

Useful next improvements if the project continues:

- add binary ACK/fill responses back to clients
- add a benchmark mode that saves CSV files
- add warmup runs before measured benchmark
- add CPU affinity/pinning for benchmark threads
- add synthetic TCP client load generator
- add unit tests for order book matching behavior
- add tests for partial TCP message handling
- add tombstone cleanup strategy for order-id index
- add proper logging abstraction instead of direct `std::cout`
- add latency histogram for internal order-to-trade events
- add true socket-level round-trip benchmark
- add graceful backpressure metrics for full queues
- add optional pause instruction in spin loops

---

## 26. Code File Responsibilities

### include/exchange.hpp

Contains:

- protocol enums
- `OrderRequest`
- `TradeEvent`
- validation declarations
- client state
- fixed client pool declarations

### src/exchange.cpp

Contains:

- message validation functions
- client pool constructor
- client acquire/release logic

### include/ring_buffer.hpp

Contains:

- `OrderRingBuffer` declaration
- `TradeRingBuffer` declaration
- capacity constants
- deleted copy constructor and copy assignment

Deleted copying matters because:

- atomic members should not be copied
- queue state cannot be safely duplicated
- each queue represents one live producer/consumer channel

### src/ring_buffer.cpp

Contains:

- SPSC push/pop logic
- empty/full/size/capacity logic
- acquire/release memory ordering

### include/order_book.hpp

Contains:

- `OrderBook` declaration
- status enum
- price level structure
- order node structure
- order-id hash index
- radix node/tree declaration
- constants for max orders, active price levels, radix depth, and index size

### src/order_book.cpp

Contains:

- matching logic
- order insert/cancel/modify logic
- FIFO list append/unlink logic
- radix tree insert/find/erase/best-price logic
- trade event emission
- pool acquire/release logic
- order-id hash table logic

### include/network.hpp

Contains:

- `listen_server` declaration

### src/network.cpp

Contains:

- socket creation
- bind/listen/accept
- non-blocking socket setup
- `kqueue` event loop
- partial TCP message buffering
- client pool usage
- order validation before queue push
- client close and cleanup

### include/engine.hpp

Contains:

- matching loop declaration
- trade publisher loop declaration

### src/engine.cpp

Contains:

- matching thread loop
- trade publisher loop

### include/shutdown.hpp

Contains:

- shutdown function declarations

### src/shutdown.cpp

Contains:

- signal handler setup
- shutdown request flag

### src/main.cpp

Contains:

- program startup
- queue construction
- order book construction
- thread startup
- network server startup
- shutdown coordination

### benchmark/benchmark.cpp

Contains:

- SPSC queue benchmarks
- order book benchmarks
- mixed workload benchmark
- internal order-to-trade pipeline benchmark

### Makefile

Contains:

- main build target
- benchmark build target
- run benchmark target
- clean target

---

## 27. Key Numbers To Remember

```text
Language:                  C++20
OS event system:            macOS kqueue
TCP mode:                   non-blocking
Server port:                9000
Max clients:                1024
Order queue capacity:       4096 slots, 4095 usable
Trade queue capacity:       4096 slots, 4095 usable
Max resting orders:         1,048,576
Max active price levels:    65,536
Order-id index slots:       2,097,152
Price range:                0 to 2^30 - 1
Radix bits per level:       6
Radix fanout:               64
Radix depth:                5
Benchmark pipeline speed:   11.7M ops/sec
Pipeline average:           85.2 ns/op
Passive insert speed:       11.9M inserts/sec
Aggressive match speed:     8.6M matches/sec
Cancel speed:               13.8M cancels/sec
Order-book p99 target seen: under 250 ns in local benchmark
```

---

## 28. Simple Explanation Of Full System

The server listens on port `9000`.

When a client connects, the server accepts it and stores the client in a fixed-size client pool.

Each client may send an `OrderRequest`. Because TCP is a stream, the server may receive the request in pieces. The server stores partial bytes in the client’s buffer until a full order message is available.

After a full order is received, the server validates it. If it is valid, it pushes it into the `OrderRingBuffer`. If the queue is full, it waits until the matching thread consumes some orders.

The matching thread pops orders from the order queue and calls `OrderBook::process`.

The order book matches aggressive orders against the opposite side:

- buy orders match lowest asks
- sell orders match highest bids

If an incoming order is not fully matched, the remaining quantity becomes a resting order.

Resting orders are stored in preallocated order nodes. Each active price level has a FIFO linked list. The oldest order at a price level is matched first.

Active prices are stored in radix bitmap trees. This allows fast best-bid and best-ask lookup without allocating a huge dense price array.

Whenever a match happens, the order book creates a `TradeEvent` and pushes it into the `TradeRingBuffer`.

The trade publisher thread pops trade events and currently prints them.

When Ctrl+C is pressed, the server exits, open clients are closed, queues are drained, and worker threads join cleanly.

---

## 29. One-Line Architecture

```text
Non-blocking kqueue TCP ingress -> SPSC order queue -> single-threaded radix-backed FIFO order book -> SPSC trade queue -> trade publisher
```

