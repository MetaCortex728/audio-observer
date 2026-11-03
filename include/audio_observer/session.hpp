#pragma once

/// @file
/// Bounded measurement session over one or more capture streams.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_observer/backend.hpp"
#include "audio_observer/dropout_detector.hpp"
#include "audio_observer/meters.hpp"
#include "audio_observer/stream_info.hpp"

namespace audio_observer {

/// One stream to open, under a label such as "mic" or "system".
struct StreamRequest {
    std::string label;
    std::string endpoint_name;
    StreamDirection direction = StreamDirection::capture;
};

struct SessionConfig {
    std::vector<StreamRequest> streams;
    /// Requested duration. Must lie within [min_seconds, max_seconds]. Each
    /// stream delivers exactly round(seconds x sample rate) frames unless it
    /// ends, fails or is stopped first.
    double seconds = 10.0;
    double min_seconds = 1.0;
    double max_seconds = 60.0;
    MeterConfig meter;
    /// Wall-clock safety limit: the session ends after seconds +
    /// safety_margin_seconds even if some stream has not delivered all of its
    /// frames (for example because its device clock runs slow or the device
    /// stalls). Must not be negative.
    double safety_margin_seconds = 2.0;
    /// Capacity of each stream's queue, in seconds of audio.
    double queue_seconds = 2.0;
    /// Glitch events kept per stream (all are counted).
    std::size_t max_events_per_stream = 16;
    /// How long the consumer sleeps when no stream has data.
    std::chrono::milliseconds poll_interval{2};
};

/// How a session ended.
enum class SessionState : std::uint8_t {
    /// Every stream delivered the requested frames or ended normally, or the
    /// wall-clock safety limit passed.
    completed,
    stopped,    ///< Stopped on request (Ctrl+C in the CLI).
    failed,     ///< A stream could not be opened or started, or failed while running.
};

/// Returns "completed", "stopped" or "failed".
const char* to_string(SessionState state) noexcept;

/// Scalar results for one stream. Holds no samples.
struct StreamSummary {
    std::string label;
    StreamInfo info;
    StreamState final_state = StreamState::opened;
    std::string error;
    std::uint64_t blocks = 0;
    std::uint64_t frames = 0;
    /// Blocks carrying any device flag (block_flags), like the prototype's
    /// callback status warnings.
    std::uint64_t flagged_blocks = 0;
    MeterReading levels;                 ///< Whole stream.
    std::vector<MeterReading> channels;  ///< One per channel.
    DropoutReport dropouts;              ///< Gaps, overlaps, flags and overruns.
};

struct SessionSummary {
    SessionState state = SessionState::completed;
    std::string error;  ///< Empty unless state is failed.
    double seconds_requested = 0.0;
    double elapsed_seconds = 0.0;
    std::vector<StreamSummary> streams;  ///< Streams that were opened, in request order.
};

/// Runs a bounded session: opens every requested stream, starts them, feeds
/// meters from the calling (consumer) thread and stops when every stream has
/// delivered seconds x sample rate frames (the last block is truncated at
/// that position) or ended, the wall-clock safety limit has passed, a stream fails or a stop is requested.
/// Every opened stream is stopped and released before run() returns, in all
/// of these cases and when an exception escapes.
class Session {
public:
    /// Throws std::invalid_argument for an empty stream list, empty or
    /// repeated labels, or a duration outside the configured bounds.
    Session(Backend& backend, SessionConfig config);

    const SessionConfig& config() const noexcept { return config_; }

    /// Runs the session. @p stop_request, when given, is polled by the
    /// consumer; setting it to true ends the session as stopped. Backend
    /// errors are reported in the summary rather than thrown.
    SessionSummary run(const std::atomic<bool>* stop_request = nullptr);

private:
    Backend& backend_;
    SessionConfig config_;
};

}  // namespace audio_observer
