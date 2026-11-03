#pragma once

/// @file
/// Hand-off of whole frame blocks from a stream's thread to the consumer.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "audio_observer/frame_block.hpp"
#include "audio_observer/spsc_ring_buffer.hpp"

namespace audio_observer {

/// Per-block status bits reported by a device.
namespace block_flags {
/// The device reports that this block does not follow on from the previous
/// one (WASAPI: AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY).
inline constexpr std::uint32_t discontinuity = 1u << 0;
/// The device could not provide a reliable timestamp for this block.
inline constexpr std::uint32_t timestamp_error = 1u << 1;
}  // namespace block_flags

/// A block as received by the consumer. The sample view stays valid until
/// the next call to BlockQueue::pop().
struct DeliveredBlock {
    FrameBlock block;
    std::uint32_t flags = 0;  ///< block_flags bits.
    /// Blocks and frames the queue dropped (overruns) between the previous
    /// delivered block and this one.
    std::uint32_t dropped_blocks_before = 0;
    std::uint64_t dropped_frames_before = 0;
};

/// Single-producer, single-consumer queue of frame blocks for one stream.
///
/// Samples and block headers travel through two SpscRingBuffer instances.
/// The producer (a stream's thread) never allocates, locks or blocks: a block
/// that does not fit is dropped whole and counted as an overrun, and the next
/// delivered block says how much was dropped before it, so the consumer can
/// tell overruns apart from gaps reported by the device.
class BlockQueue {
public:
    /// Holds up to @p capacity_frames frames and @p capacity_blocks blocks of
    /// @p channel_count channels; no block may exceed @p max_block_frames.
    /// Throws std::invalid_argument when any argument is zero or
    /// @p max_block_frames exceeds @p capacity_frames.
    BlockQueue(std::size_t channel_count,
               std::size_t capacity_frames,
               std::size_t capacity_blocks,
               std::size_t max_block_frames);

    BlockQueue(const BlockQueue&) = delete;
    BlockQueue& operator=(const BlockQueue&) = delete;

    std::size_t channel_count() const noexcept { return channel_count_; }
    std::size_t max_block_frames() const noexcept { return max_block_frames_; }

    /// Producer: true when a block of @p frame_count frames would fit now.
    bool can_push(std::size_t frame_count) const noexcept;

    /// Producer: queues a copy of @p block with @p flags. Returns false and
    /// counts an overrun when it does not fit, when its channel count is
    /// wrong or when it is larger than max_block_frames().
    bool push(const FrameBlock& block, std::uint32_t flags = 0) noexcept;

    /// Producer: discards a block of @p frame_count frames as if the queue
    /// had been full. Used for fault injection.
    void drop(std::size_t frame_count) noexcept;

    /// Consumer: takes the oldest block. Returns false when none is waiting.
    bool pop(DeliveredBlock& out) noexcept;

    /// Blocks dropped so far, and their total frames. Safe from any thread.
    std::uint64_t overrun_count() const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }
    std::uint64_t overrun_frames() const noexcept {
        return overrun_frames_.load(std::memory_order_relaxed);
    }

private:
    struct Header {
        std::int64_t first_sample_index;
        std::int64_t host_time_ns;
        std::uint64_t dropped_frames_before;
        std::uint32_t frame_count;
        std::uint32_t flags;
        std::uint32_t dropped_blocks_before;
    };

    void count_drop(std::size_t frame_count) noexcept;

    std::size_t channel_count_;
    std::size_t max_block_frames_;
    SpscRingBuffer<float> samples_;
    SpscRingBuffer<Header> headers_;
    std::unique_ptr<float[]> scratch_;  // consumer-owned

    // Producer-owned: drops not yet reported to the consumer.
    std::uint32_t pending_dropped_blocks_ = 0;
    std::uint64_t pending_dropped_frames_ = 0;

    std::atomic<std::uint64_t> overruns_{0};
    std::atomic<std::uint64_t> overrun_frames_{0};
};

}  // namespace audio_observer
