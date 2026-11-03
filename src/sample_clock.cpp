#include "audio_observer/sample_clock.hpp"

namespace audio_observer {

ClockObservation SampleClock::observe(std::int64_t first_sample_index,
                                      std::size_t frame_count) noexcept {
    ClockObservation result;
    result.actual_index = first_sample_index;
    result.expected_index = expected_next_.value_or(first_sample_index);

    if (!expected_next_) {
        result.continuity = ClockContinuity::first;
    } else if (first_sample_index == *expected_next_) {
        result.continuity = ClockContinuity::contiguous;
    } else if (first_sample_index > *expected_next_) {
        result.continuity = ClockContinuity::gap;
        ++gaps_;
        gap_frames_ += static_cast<std::uint64_t>(first_sample_index - *expected_next_);
    } else {
        result.continuity = ClockContinuity::overlap;
        ++overlaps_;
        overlap_frames_ += static_cast<std::uint64_t>(*expected_next_ - first_sample_index);
    }

    ++blocks_;
    frames_ += frame_count;
    expected_next_ = first_sample_index + static_cast<std::int64_t>(frame_count);
    return result;
}

}  // namespace audio_observer
