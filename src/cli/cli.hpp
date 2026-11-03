#pragma once

/// @file
/// The audio-observer command line, callable in-process for tests.

#include <atomic>
#include <ostream>
#include <string>
#include <vector>

namespace audio_observer::cli {

/// Exit codes.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitStopped = 130;  ///< Stopped by the user, as in the prototype.

/// Runs the command line @p args (without the program name). JSON goes to
/// @p out, messages to @p err. A meter session stops early when
/// @p stop_request becomes true.
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
        const std::atomic<bool>* stop_request = nullptr);

}  // namespace audio_observer::cli
