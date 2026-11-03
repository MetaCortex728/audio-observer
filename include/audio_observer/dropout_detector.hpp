#pragma once

/// @file
/// Dropout and glitch detection for one stream: gaps and overlaps in the
/// sample clock, discontinuities and timestamp errors reported by the device,
/// and blocks lost to queue overruns, each counted and timestamped.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "audio_observer/block_queue.hpp"
#include "audio_observer/frame_block.hpp"
#include "audio_observer/sample_clock.hpp"

namespace audio_observer {

enum class GlitchKind : std::uint8_t {
    gap,              ///< Frames missing in the device's sample positions.
    overlap,          ///< Frames repeated in the device's sample positions.
    discontinuity,    ///< The device flagged a data discontinuity.
    overrun,          ///< Blocks dropped because the queue was full.
    timestamp_error,  ///< The device flagged an unreliable timestamp.
    time_regression,  ///< A block's host time is earlier than its predecessor's.
};

/// Returns the lower-case name of @p kind, e.g. "gap".
const char* to_string(GlitchKind kind) noexcept;

/// One detected glitch.
struct GlitchEvent {
    GlitchKind kind = GlitchKind::gap;
    /// Stream position where it happened: the first missing, repeated or
    /// dropped frame, or the first frame of the flagged block.
    std::int64_t sample_index = 0;
    /// Frames missing, repeated or dropped (0 for flags and time regressions).
    std::uint64_t frames = 0;
    /// Host time of the block that revealed the glitch.
    HostTime host_time{};
};

/// Totals for one stream, plus the first few events.
struct DropoutReport {
    std::uint64_t gaps = 0;
    std::uint64_t gap_frames = 0;
    std::uint64_t overlaps = 0;
    std::uint64_t overlap_frames = 0;
    std::uint64_t discontinuities = 0;
    /// Blocks lost to queue overruns, and their frames.
    std::uint64_t overruns = 0;
    std::uint64_t overrun_frames = 0;
    /// Overruns after the last delivered block, which have no position.
    std::uint64_t unplaced_overruns = 0;
    std::uint64_t timestamp_errors = 0;
    std::uint64_t time_regressions = 0;

    /// Position and host time of the first delivered block, the origin for
    /// event times. Empty until a block arrives.
    std::optional<std::int64_t> first_sample_index;
    HostTime first_host_time{};

    /// Rough device clock rate against the host clock, in ppm, from the
    /// first and last blocks' positions and host times. Empty with fewer
    /// than two blocks or no elapsed host time.
    std::optional<double> rate_offset_ppm;

    /// The first events in arrival order, at most the configured limit.
    std::vector<GlitchEvent> events;
    /// Events beyond the limit, counted but not kept.
    std::uint64_t events_omitted = 0;

    /// Sum of all glitch counts.
    std::uint64_t total() const noexcept {
        return gaps + overlaps + discontinuities + overruns + timestamp_errors + time_regressions;
    }
};

/// Watches the blocks of one stream as the consumer receives them. Uses a
/// SampleClock for positions: frames that the queue dropped are first
/// accounted for at the expected position, so the clock reports only gaps
/// and overlaps that come from the device. Does not allocate after
/// construction.
class DropoutDetector {
public:
    explicit DropoutDetector(std::uint32_t sample_rate_hz, std::size_t max_events = 16);

    /// Examines one delivered block.
    void observe(const DeliveredBlock& delivered) noexcept;

    /// Reconciles with the queue's own totals once the stream has stopped,
    /// counting overruns that no delivered block reported.
    void finish(std::uint64_t queue_overruns, std::uint64_t queue_overrun_frames) noexcept;

    const DropoutReport& report() const noexcept { return report_; }
    const SampleClock& clock() const noexcept { return clock_; }

private:
    void record(GlitchKind kind, std::int64_t sample_index, std::uint64_t frames,
                HostTime host_time) noexcept;

    double sample_rate_hz_;
    std::size_t max_events_;
    SampleClock clock_;
    DropoutReport report_;
    std::optional<HostTime> previous_host_time_;
    std::int64_t last_sample_index_ = 0;
    HostTime last_host_time_{};
};

}  // namespace audio_observer
