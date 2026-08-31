#include "shutdown.hpp"

#include <csignal>

namespace {
volatile std::sig_atomic_t stop_requested = 0;

void handle_shutdown_signal(int) {
    stop_requested = 1;
}
}

bool install_shutdown_handlers() {
    struct sigaction action {};
    action.sa_handler = handle_shutdown_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    return sigaction(SIGINT, &action, nullptr) == 0 &&
           sigaction(SIGTERM, &action, nullptr) == 0;
}

bool shutdown_requested() {
    return stop_requested != 0;
}

void request_shutdown() {
    stop_requested = 1;
}
