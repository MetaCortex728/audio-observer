#include <doctest.h>

#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "audio_observer/block_queue.hpp"

using audio_observer::BlockQueue;
using audio_observer::DeliveredBlock;
using audio_observer::FrameBlock;
using audio_observer::HostTime;
namespace block_flags = audio_observer::block_flags;

namespace {

FrameBlock block_of(const std::vector<float>& samples, std::size_t channels, std::int64_t first,
                    std::int64_t host_ns = 0) {
    return FrameBlock(samples.data(), samples.size() / channels, channels, first,
                      HostTime(std::chrono::nanoseconds(host_ns)));
}

}  // namespace

TEST_CASE("block queue rejects bad sizes") {
    CHECK_THROWS_AS(BlockQueue(0, 10, 2, 5), std::invalid_argument);
    CHECK_THROWS_AS(BlockQueue(1, 0, 2, 1), std::invalid_argument);
    CHECK_THROWS_AS(BlockQueue(1, 10, 0, 5), std::invalid_argument);
    CHECK_THROWS_AS(BlockQueue(1, 10, 2, 0), std::invalid_argument);
    CHECK_THROWS_AS(BlockQueue(1, 10, 2, 11), std::invalid_argument);
}

TEST_CASE("block queue delivers blocks with position, time and flags") {
    BlockQueue queue(2, 16, 4, 4);
    const std::vector<float> a{1, 2, 3, 4, 5, 6};
    const std::vector<float> b{7, 8};
    CHECK(queue.push(block_of(a, 2, 100, 5000), block_flags::discontinuity));
    CHECK(queue.push(block_of(b, 2, 103, 6000)));

    DeliveredBlock out;
    REQUIRE(queue.pop(out));
    CHECK(out.block.frame_count() == 3);
    CHECK(out.block.channel_count() == 2);
    CHECK(out.block.first_sample_index() == 100);
    CHECK(out.block.host_time().time_since_epoch().count() == 5000);
    CHECK(out.block.at(2, 1) == 6.0f);
    CHECK(out.flags == block_flags::discontinuity);
    CHECK(out.dropped_blocks_before == 0);

    REQUIRE(queue.pop(out));
    CHECK(out.block.first_sample_index() == 103);
    CHECK(out.block.at(0, 0) == 7.0f);
    CHECK(out.flags == 0);
    CHECK_FALSE(queue.pop(out));
}

TEST_CASE("block queue drops whole blocks and reports them on the next delivery") {
    BlockQueue queue(1, 8, 2, 4);
    const std::vector<float> four{1, 2, 3, 4};
    CHECK(queue.push(block_of(four, 1, 0)));
    CHECK(queue.push(block_of(four, 1, 4)));
    // Full: both the next blocks are dropped.
    CHECK_FALSE(queue.push(block_of(four, 1, 8)));
    CHECK_FALSE(queue.push(block_of(four, 1, 12)));
    // Wrong channel count and oversize blocks are dropped too.
    CHECK_FALSE(queue.push(FrameBlock(four.data(), 2, 2, 16, HostTime{})));
    CHECK(queue.overrun_count() == 3);
    CHECK(queue.overrun_frames() == 10);

    DeliveredBlock out;
    REQUIRE(queue.pop(out));
    REQUIRE(queue.pop(out));
    CHECK(queue.push(block_of(four, 1, 20)));
    queue.drop(4);
    CHECK(queue.push(block_of(four, 1, 28)));
    REQUIRE(queue.pop(out));
    CHECK(out.block.first_sample_index() == 20);
    CHECK(out.dropped_blocks_before == 3);
    CHECK(out.dropped_frames_before == 10);
    REQUIRE(queue.pop(out));
    CHECK(out.dropped_blocks_before == 1);
    CHECK(out.dropped_frames_before == 4);
    CHECK(queue.overrun_count() == 4);
}

TEST_CASE("block queue is limited by its header count") {
    BlockQueue queue(1, 100, 2, 10);
    const std::vector<float> one{0.5f};
    CHECK(queue.push(block_of(one, 1, 0)));
    CHECK(queue.push(block_of(one, 1, 1)));
    CHECK_FALSE(queue.can_push(1));
    CHECK_FALSE(queue.push(block_of(one, 1, 2)));
    CHECK(queue.overrun_count() == 1);
}

TEST_CASE("block queue hands over every block intact between threads") {
    constexpr std::size_t channels = 3;
    constexpr std::size_t blocks = 20000;
    BlockQueue queue(channels, 64, 8, 16);
    bool pushed_all = true;
    std::thread producer([&] {
        std::vector<float> samples;
        std::int64_t position = 0;
        for (std::size_t i = 0; i < blocks; ++i) {
            const std::size_t frames = 1 + i % 16;
            samples.assign(frames * channels, static_cast<float>(i % 1000));
            const FrameBlock block(samples.data(), frames, channels, position, HostTime{});
            while (!queue.can_push(frames)) {
                std::this_thread::yield();
            }
            pushed_all = queue.push(block) && pushed_all;
            position += static_cast<std::int64_t>(frames);
        }
    });
    std::int64_t expected = 0;
    std::size_t received = 0;
    bool intact = true;
    DeliveredBlock out;
    while (received < blocks) {
        if (!queue.pop(out)) {
            std::this_thread::yield();
            continue;
        }
        const std::size_t frames = 1 + received % 16;
        intact = intact && out.block.frame_count() == frames &&
                 out.block.first_sample_index() == expected;
        for (std::size_t s = 0; s < out.block.sample_count(); ++s) {
            intact = intact && out.block.data()[s] == static_cast<float>(received % 1000);
        }
        expected += static_cast<std::int64_t>(frames);
        ++received;
    }
    producer.join();
    CHECK(intact);
    CHECK(pushed_all);
    CHECK(queue.overrun_count() == 0);
}
