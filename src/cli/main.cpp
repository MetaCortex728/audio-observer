#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"

namespace {

// Lock-free, so it may be set from a signal handler.
std::atomic<bool> g_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "stop flag must be lock-free");

extern "C" void request_stop(int /*signal*/) {
    g_stop_requested.store(true);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    const std::vector<std::string> args(argv + 1, argv + argc);
    return audio_observer::cli::run(args, std::cout, std::cerr, &g_stop_requested);
}
