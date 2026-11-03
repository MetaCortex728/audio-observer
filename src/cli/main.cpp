#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must come first.
#include <shellapi.h>
#endif

#include "cli.hpp"

namespace {

// Lock-free, so it may be set from a signal handler.
std::atomic<bool> g_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "stop flag must be lock-free");

extern "C" void request_stop(int /*signal*/) {
    g_stop_requested.store(true);
}

#if defined(_WIN32)
// Endpoint names may hold any character, and argv uses the ANSI code page,
// so read the UTF-16 command line and convert it to UTF-8 (the encoding of
// endpoint names in the library).
bool utf8_arguments(std::vector<std::string>& out) {
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return false;
    }
    bool ok = true;
    for (int i = 1; i < count && ok; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            ok = false;
            break;
        }
        std::string arg(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
        arg.resize(static_cast<std::size_t>(size - 1));
        out.push_back(std::move(arg));
    }
    LocalFree(wide);
    return ok;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
#if defined(SIGBREAK)
    std::signal(SIGBREAK, request_stop);  // Ctrl+Break on Windows
#endif
    std::vector<std::string> args;
#if defined(_WIN32)
    if (!utf8_arguments(args)) {
        args.assign(argv + 1, argv + argc);
    }
#else
    args.assign(argv + 1, argv + argc);
#endif
    return audio_observer::cli::run(args, std::cout, std::cerr, &g_stop_requested);
}
