#include "audio_observer/dropout_detector.hpp"

#include <chrono>

namespace audio_observer {

const char* to_string(GlitchKind kind) noexcept {
    switch (kind) {
        case GlitchKind::gap:
            return "gap";
        case GlitchKind::overlap:
            return "overlap";
        case GlitchKind::discontinuity:
            return "discontinuity";
        case GlitchKind::overrun:
            return "overrun";
        case GlitchKind::timestamp_error:
            return "timestamp_error";
        case GlitchKind::time_regression:
            return "time_regression";
    }
    return "unknown";
}

DropoutDetector::DropoutDetector(std::uint32_t sample_rate_hz, std::size_t max_events)
    : sample_rate_hz_(static_cast<double>(sample_rate_hz)), max_events_(max_events) {
    report_.events.reserve(max_events_);
}

void DropoutDetector::record(GlitchKind kind, std::int64_t sample_index, std::uint64_t frames,
                             HostTime host_time) noexcept {
    if (report_.events.size() < max_events_) {
        report_.events.push_back(GlitchEvent{kind, sample_index, frames, host_time});
    } else {
        ++report_.events_omitted;
    }
}

void DropoutDetector::observe(const DeliveredBlock& delivered) noexcept {
    const FrameBlock& block = delivered.block;
    const HostTime host_time = block.host_time();

    if (delivered.dropped_blocks_before > 0) {
        report_.overruns += delivered.dropped_blocks_before;
        report_.overrun_frames += delivered.dropped_frames_before;
        const auto dropped = static_cast<std::int64_t>(delivered.dropped_frames_before);
        if (const auto expected = clock_.expected_next()) {
            // The dropped frames would have started where the clock expected
            // the next block; account for them so they are not seen as a gap.
            record(GlitchKind::overrun, *expected, delivered.dropped_frames_before, host_time);
            clock_.observe(*expected, static_cast<std::size_t>(delivered.dropped_frames_before));
        } else {
            record(GlitchKind::overrun, block.first_sample_index() - dropped,
                   delivered.dropped_frames_before, host_time);
        }
    }

    const ClockObservation observation = clock_.observe(block);
    if (observation.continuity == ClockContinuity::gap) {
        record(GlitchKind::gap, observation.expected_index,
               static_cast<std::uint64_t>(observation.offset_frames()), host_time);
    } else if (observation.continuity == ClockContinuity::overlap) {
        record(GlitchKind::overlap, observation.actual_index,
               static_cast<std::uint64_t>(-observation.offset_frames()), host_time);
    }
    report_.gaps = clock_.gap_count();
    report_.gap_frames = clock_.gap_frames();
    report_.overlaps = clock_.overlap_count();
    report_.overlap_frames = clock_.overlap_frames();

    if ((delivered.flags & block_flags::discontinuity) != 0) {
        ++report_.discontinuities;
        record(GlitchKind::discontinuity, block.first_sample_index(), 0, host_time);
    }
    if ((delivered.flags & block_flags::timestamp_error) != 0) {
        ++report_.timestamp_errors;
        record(GlitchKind::timestamp_error, block.first_sample_index(), 0, host_time);
    }
    if (previous_host_time_ && host_time < *previous_host_time_) {
        ++report_.time_regressions;
        record(GlitchKind::time_regression, block.first_sample_index(), 0, host_time);
    }
    previous_host_time_ = host_time;

    if (!report_.first_sample_index) {
        report_.first_sample_index = block.first_sample_index();
        report_.first_host_time = host_time;
    }
    last_sample_index_ = block.first_sample_index();
    last_host_time_ = host_time;
    const double seconds =
        std::chrono::duration<double>(last_host_time_ - report_.first_host_time).count();
    if (seconds > 0.0 && sample_rate_hz_ > 0.0) {
        const double rate =
            static_cast<double>(last_sample_index_ - *report_.first_sample_index) / seconds;
        report_.rate_offset_ppm = (rate / sample_rate_hz_ - 1.0) * 1e6;
    }
}

void DropoutDetector::finish(std::uint64_t queue_overruns,
                             std::uint64_t queue_overrun_frames) noexcept {
    if (queue_overruns > report_.overruns) {
        report_.unplaced_overruns = queue_overruns - report_.overruns;
        report_.overruns = queue_overruns;
    }
    if (queue_overrun_frames > report_.overrun_frames) {
        report_.overrun_frames = queue_overrun_frames;
    }
}

}  // namespace audio_observer
