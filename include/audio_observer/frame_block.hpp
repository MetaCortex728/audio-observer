#pragma once

/// @file
/// Non-owning view of a block of interleaved float samples.

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace audio_observer {

/// Host time point on the steady clock, with nanosecond resolution.
using HostTime = std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds>;

/// Returns the current host time.
inline HostTime host_now() noexcept {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now());
}

/// A view of @c frame_count frames of @c channel_count interleaved float
/// samples, stamped with the stream position of its first frame and the host
/// time at which it was received.
///
/// The view does not own or copy the samples; the caller keeps them alive for
/// as long as the view is used. Positions count frames (one sample per
/// channel) from the start of the stream.
class FrameBlock {
public:
    /// An empty block with one channel.
    FrameBlock() noexcept = default;

    /// Wraps @p samples, which must hold @p frame_count * @p channel_count
    /// floats (it may be null only when @p frame_count is zero).
    /// @p channel_count must be at least 1.
    FrameBlock(const float* samples,
               std::size_t frame_count,
               std::size_t channel_count,
               std::int64_t first_sample_index,
               HostTime host_time) noexcept
        : samples_(samples),
          frame_count_(frame_count),
          channel_count_(channel_count),
          first_sample_index_(first_sample_index),
          host_time_(host_time) {
        assert(channel_count >= 1);
        assert(samples != nullptr || frame_count == 0);
    }

    /// Interleaved samples: frame f, channel c is at index f * channel_count() + c.
    const float* data() const noexcept { return samples_; }
    std::size_t frame_count() const noexcept { return frame_count_; }
    std::size_t channel_count() const noexcept { return channel_count_; }
    /// Total samples across all channels.
    std::size_t sample_count() const noexcept { return frame_count_ * channel_count_; }
    bool empty() const noexcept { return frame_count_ == 0; }

    /// Stream position (frame index) of the first frame in the block.
    std::int64_t first_sample_index() const noexcept { return first_sample_index_; }
    /// Stream position one past the last frame in the block.
    std::int64_t end_sample_index() const noexcept {
        return first_sample_index_ + static_cast<std::int64_t>(frame_count_);
    }
    /// Host time at which the block was received.
    HostTime host_time() const noexcept { return host_time_; }

    /// Sample of @p channel in @p frame. Both must be in range.
    float at(std::size_t frame, std::size_t channel) const noexcept {
        assert(frame < frame_count_ && channel < channel_count_);
        return samples_[frame * channel_count_ + channel];
    }

private:
    const float* samples_ = nullptr;
    std::size_t frame_count_ = 0;
    std::size_t channel_count_ = 1;
    std::int64_t first_sample_index_ = 0;
    HostTime host_time_{};
};

}  // namespace audio_observer
