#include "exchange.hpp"
#include "network.hpp"
#include "ring_buffer.hpp"
#include "shutdown.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>


// Helper to make sockets non-blocking
bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

void listen_server(
    uint16_t port,
    OrderRingBuffer& order_queue,
    const std::atomic<bool>& running
) {
    // 1. Create the TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Failed to create socket\n";
        return;
    }

    // 2. Apply low-latency socket options
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // 3. Bind to the port
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
        std::cerr << "Failed to bind to port " << port << "\n";
        close(server_fd);
        return;
    }

    // 4. Listen and set to non-blocking
    if (listen(server_fd, SOMAXCONN) == -1) {
        std::cerr << "Failed to listen\n";
        close(server_fd);
        return;
    }
    set_nonblocking(server_fd);

    // 5. Initialize kqueue
    int kqueue_fd = kqueue();
    if (kqueue_fd == -1) {
        std::cerr << "Failed to create kqueue\n";
        close(server_fd);
        return;
    }

    struct kevent event{};
    EV_SET(&event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, nullptr);
    if (kevent(kqueue_fd, &event, 1, nullptr, 0, nullptr) == -1) {
        std::cerr << "Failed to add server_fd to kqueue\n";
        close(server_fd);
        close(kqueue_fd);
        return;
    }

    std::cout << "Server listening on port " << port << "...\n";

    // 6. The Event Loop
    const int MAX_EVENTS = 64;
    struct kevent events[MAX_EVENTS];
    ClientPool client_pool;

    auto close_client = [&client_pool](ClientState* client) {
        if (client == nullptr || client->fd == -1) {
            return;
        }

        int client_fd = client->fd;
        client_pool.release(client);
        close(client_fd);
    };

    while (running.load(std::memory_order_acquire) && !shutdown_requested()) {
        timespec timeout {};
        timeout.tv_sec = 0;
        timeout.tv_nsec = 100000000;

        int num_events = kevent(kqueue_fd, nullptr, 0, events, MAX_EVENTS, &timeout);
        if (num_events == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "kqueue wait failed\n";
            break;
        }

        for (int i = 0; i < num_events; i++) {
            int event_fd = static_cast<int>(events[i].ident);

            if (event_fd == server_fd) {
                // Handle incoming connections until EAGAIN
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                    
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // All pending connections handled
                        }
                        std::cerr << "Accept error\n";
                        break;
                    }

                    // Setup new client
                    set_nonblocking(client_fd);
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                    ClientState* client = client_pool.acquire(client_fd);
                    if (client == nullptr) {
                        std::cerr << "Client pool full, rejecting FD " << client_fd << "\n";
                        close(client_fd);
                        continue;
                    }

                    struct kevent client_event{};
                    EV_SET(&client_event, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, client);
                    if (kevent(kqueue_fd, &client_event, 1, nullptr, 0, nullptr) == -1) {
                        std::cerr << "Failed to add client_fd to kqueue: FD " << client_fd << "\n";
                        close_client(client);
                        continue;
                    }
                    
                    std::cout << "New client connected: FD " << client_fd
                              << ", slots left: " << client_pool.available() << "\n";
                }
            } else {
                // Handle incoming data on a client socket
                ClientState* client = static_cast<ClientState*>(events[i].udata);
                if (client == nullptr || client->fd != event_fd) {
                    continue;
                }

                int client_fd = client->fd;

                if (events[i].flags & EV_EOF) {
                    std::cout << "Client disconnected: FD " << client_fd << "\n";
                    close_client(client);
                    continue;
                }
                
                // Read until the kernel buffer is empty (EAGAIN)
                while (true) {
                    size_t remaining = sizeof(OrderRequest) - client->bytes_received;
                    ssize_t bytes_read = recv(
                        client_fd,
                        client->buffer.data() + client->bytes_received,
                        remaining,
                        0
                    );
                    
                    if (bytes_read == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // Kernel buffer drained, go back to kevent
                        }
                        std::cerr << "Read error or client disconnected abruptly: FD " << client_fd << "\n";
                        close_client(client);
                        break;
                    } else if (bytes_read == 0) {
                        std::cout << "Client disconnected gracefully: FD " << client_fd << "\n";
                        close_client(client);
                        break;
                    }

                    client->bytes_received += static_cast<size_t>(bytes_read);

                    if (client->bytes_received == sizeof(OrderRequest)) {
                        OrderRequest order{};
                        std::memcpy(&order, client->buffer.data(), sizeof(order));
                        client->bytes_received = 0;

                        if (!is_valid_order_request(order)) {
                            std::cerr << "Invalid order request, disconnecting FD "
                                      << client_fd << "\n";
                            close_client(client);
                            break;
                        }

                        while (!order_queue.push(order)) {}
                    }
                }
            }
        }
    }

    for (ClientState& client : client_pool.clients) {
        close_client(&client);
    }

    close(server_fd);
    close(kqueue_fd);
}
