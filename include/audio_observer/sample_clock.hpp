#pragma once

/// @file
/// Continuity tracking of one stream's sample positions.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "audio_observer/frame_block.hpp"

namespace audio_observer {

/// How a block's position compares with the position the clock expected.
enum class ClockContinuity : std::uint8_t {
    first,       ///< First block seen since construction or reset.
    contiguous,  ///< Block starts exactly where the previous one ended.
    gap,         ///< Block starts after the expected position (frames missing).
    overlap,     ///< Block starts before the expected position (frames repeated).
};

/// Result of observing one block.
struct ClockObservation {
    ClockContinuity continuity = ClockContinuity::first;
    std::int64_t expected_index = 0;  ///< Expected start (equals actual for @c first).
    std::int64_t actual_index = 0;    ///< Block's first sample index.

    /// actual - expected in frames: positive for a gap, negative for an overlap.
    std::int64_t offset_frames() const noexcept { return actual_index - expected_index; }
};

/// Tracks the expected next sample index of one stream and reports gaps and
/// overlaps when a block's position disagrees. After a discontinuity the clock
/// resynchronises to the block's own position.
class SampleClock {
public:
    /// Checks a block's position, updates the counters and advances the
    /// expected index to the end of the block.
    ClockObservation observe(const FrameBlock& block) noexcept {
        return observe(block.first_sample_index(), block.frame_count());
    }

    /// Same as observe(const FrameBlock&) for a position and frame count.
    ClockObservation observe(std::int64_t first_sample_index, std::size_t frame_count) noexcept;

    /// Expected start of the next block, or empty before the first block.
    std::optional<std::int64_t> expected_next() const noexcept { return expected_next_; }

    std::uint64_t block_count() const noexcept { return blocks_; }
    std::uint64_t frame_count() const noexcept { return frames_; }
    std::uint64_t gap_count() const noexcept { return gaps_; }
    std::uint64_t overlap_count() const noexcept { return overlaps_; }
    /// Total frames skipped over by all gaps.
    std::uint64_t gap_frames() const noexcept { return gap_frames_; }
    /// Total frames repeated by all overlaps.
    std::uint64_t overlap_frames() const noexcept { return overlap_frames_; }

    /// Forgets the expected position and clears all counters.
    void reset() noexcept { *this = SampleClock{}; }

private:
    std::optional<std::int64_t> expected_next_;
    std::uint64_t blocks_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t overlaps_ = 0;
    std::uint64_t gap_frames_ = 0;
    std::uint64_t overlap_frames_ = 0;
};

}  // namespace audio_observer
