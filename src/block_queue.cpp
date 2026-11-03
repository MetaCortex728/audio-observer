#include "audio_observer/block_queue.hpp"

#include <limits>
#include <stdexcept>

namespace audio_observer {

namespace {

std::size_t checked_samples(std::size_t channel_count,
                            std::size_t capacity_frames,
                            std::size_t capacity_blocks,
                            std::size_t max_block_frames) {
    if (channel_count == 0 || capacity_frames == 0 || capacity_blocks == 0 ||
        max_block_frames == 0 || max_block_frames > capacity_frames) {
        throw std::invalid_argument("BlockQueue needs non-zero sizes and blocks within capacity");
    }
    if (capacity_frames > std::numeric_limits<std::size_t>::max() / channel_count ||
        max_block_frames > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("BlockQueue capacity is too large");
    }
    return capacity_frames * channel_count;
}

}  // namespace

BlockQueue::BlockQueue(std::size_t channel_count,
                       std::size_t capacity_frames,
                       std::size_t capacity_blocks,
                       std::size_t max_block_frames)
    : channel_count_(channel_count),
      max_block_frames_(max_block_frames),
      samples_(checked_samples(channel_count, capacity_frames, capacity_blocks, max_block_frames)),
      headers_(capacity_blocks),
      scratch_(new float[max_block_frames * channel_count]) {}

bool BlockQueue::can_push(std::size_t frame_count) const noexcept {
    return frame_count <= max_block_frames_ && headers_.free_space() >= 1 &&
           samples_.free_space() >= frame_count * channel_count_;
}

void BlockQueue::count_drop(std::size_t frame_count) noexcept {
    ++pending_dropped_blocks_;
    pending_dropped_frames_ += frame_count;
    overruns_.fetch_add(1, std::memory_order_relaxed);
    overrun_frames_.fetch_add(frame_count, std::memory_order_relaxed);
}

void BlockQueue::drop(std::size_t frame_count) noexcept {
    count_drop(frame_count);
}

bool BlockQueue::push(const FrameBlock& block, std::uint32_t flags) noexcept {
    // Free space only grows behind the producer's back, so a successful
    // check here guarantees both pushes below succeed.
    if (block.channel_count() != channel_count_ || !can_push(block.frame_count())) {
        count_drop(block.frame_count());
        return false;
    }
    samples_.push(block.data(), block.sample_count());
    const Header header{
        block.first_sample_index(),
        static_cast<std::int64_t>(block.host_time().time_since_epoch().count()),
        pending_dropped_frames_,
        static_cast<std::uint32_t>(block.frame_count()),
        flags,
        pending_dropped_blocks_,
    };
    headers_.push(&header, 1);
    pending_dropped_blocks_ = 0;
    pending_dropped_frames_ = 0;
    return true;
}

bool BlockQueue::pop(DeliveredBlock& out) noexcept {
    Header header{};
    if (headers_.pop(&header, 1) == 0) {
        return false;
    }
    // The samples were pushed before the header, so they are all available.
    const std::size_t count = static_cast<std::size_t>(header.frame_count) * channel_count_;
    samples_.pop(scratch_.get(), count);
    out.block = FrameBlock(scratch_.get(), header.frame_count, channel_count_,
                           header.first_sample_index,
                           HostTime(std::chrono::nanoseconds(header.host_time_ns)));
    out.flags = header.flags;
    out.dropped_blocks_before = header.dropped_blocks_before;
    out.dropped_frames_before = header.dropped_frames_before;
    return true;
}

}  // namespace audio_observer
