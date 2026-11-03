#include <doctest.h>

#include <array>
#include <chrono>
#include <vector>

#include "audio_observer/audio_format.hpp"
#include "audio_observer/frame_block.hpp"
#include "audio_observer/sample_clock.hpp"
#include "audio_observer/stream_info.hpp"

using namespace audio_observer;

TEST_CASE("AudioFormat validity, equality and sample sizes") {
    AudioFormat format{48000, 2, SampleType::float32};
    CHECK(format.valid());
    CHECK_FALSE(AudioFormat{}.valid());
    CHECK_FALSE((AudioFormat{48000, 0, SampleType::int16}.valid()));
    CHECK_FALSE((AudioFormat{0, 1, SampleType::int16}.valid()));

    CHECK(format == AudioFormat{48000, 2, SampleType::float32});
    CHECK(format != AudioFormat{44100, 2, SampleType::float32});
    CHECK(format != AudioFormat{48000, 2, SampleType::int16});

    CHECK(bytes_per_sample(SampleType::int16) == 2);
    CHECK(bytes_per_sample(SampleType::int24) == 3);
    CHECK(bytes_per_sample(SampleType::int32) == 4);
    CHECK(bytes_per_sample(SampleType::float32) == 4);
}

TEST_CASE("StreamInfo geometry is empty by default and checked against channels") {
    StreamInfo info;
    info.id = "endpoint-1";
    info.name = "Headset microphone";
    info.direction = StreamDirection::capture;
    info.format = {48000, 2, SampleType::float32};
    CHECK_FALSE(info.has_geometry());
    CHECK(info.valid());

    info.channel_positions = {{0.0, 0.0, 0.0}, {0.05, 0.0, 0.0}};
    CHECK(info.has_geometry());
    CHECK(info.valid());
    CHECK(info.channel_positions[1].x_m == doctest::Approx(0.05));

    info.channel_positions.pop_back();
    CHECK_FALSE(info.valid());
}

TEST_CASE("FrameBlock indexes interleaved samples for any channel count") {
    // Three frames of five channels: value = frame * 10 + channel.
    constexpr std::size_t frames = 3;
    constexpr std::size_t channels = 5;
    std::array<float, frames * channels> samples{};
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            samples[f * channels + c] = static_cast<float>(f * 10 + c);
        }
    }
    const HostTime stamp{std::chrono::nanoseconds{123'456'789}};
    const std::int64_t start = 5'000'000'000LL;  // beyond 32 bits
    FrameBlock block(samples.data(), frames, channels, start, stamp);

    CHECK(block.frame_count() == frames);
    CHECK(block.channel_count() == channels);
    CHECK(block.sample_count() == frames * channels);
    CHECK_FALSE(block.empty());
    CHECK(block.first_sample_index() == start);
    CHECK(block.end_sample_index() == start + 3);
    CHECK(block.host_time().time_since_epoch().count() == 123'456'789);
    CHECK(block.at(0, 0) == 0.0f);
    CHECK(block.at(1, 4) == 14.0f);
    CHECK(block.at(2, 3) == 23.0f);
    CHECK(block.data() == samples.data());

    FrameBlock empty;
    CHECK(empty.empty());
    CHECK(empty.channel_count() == 1);
    CHECK(empty.sample_count() == 0);
}

TEST_CASE("host_now is monotonic") {
    const HostTime a = host_now();
    const HostTime b = host_now();
    CHECK(b >= a);
}

TEST_CASE("SampleClock reports contiguous blocks, gaps and overlaps") {
    SampleClock clock;
    CHECK_FALSE(clock.expected_next().has_value());

    auto first = clock.observe(1000, 256);
    CHECK(first.continuity == ClockContinuity::first);
    CHECK(first.offset_frames() == 0);
    CHECK(clock.expected_next() == 1256);

    auto next = clock.observe(1256, 256);
    CHECK(next.continuity == ClockContinuity::contiguous);
    CHECK(clock.expected_next() == 1512);

    // 100 frames missing.
    auto gap = clock.observe(1612, 256);
    CHECK(gap.continuity == ClockContinuity::gap);
    CHECK(gap.expected_index == 1512);
    CHECK(gap.actual_index == 1612);
    CHECK(gap.offset_frames() == 100);
    CHECK(clock.expected_next() == 1868);  // resynchronised to the block

    // 68 frames repeated.
    auto overlap = clock.observe(1800, 128);
    CHECK(overlap.continuity == ClockContinuity::overlap);
    CHECK(overlap.offset_frames() == -68);
    CHECK(clock.expected_next() == 1928);

    CHECK(clock.observe(1928, 0).continuity == ClockContinuity::contiguous);

    CHECK(clock.block_count() == 5);
    CHECK(clock.frame_count() == 256 * 3 + 128);
    CHECK(clock.gap_count() == 1);
    CHECK(clock.gap_frames() == 100);
    CHECK(clock.overlap_count() == 1);
    CHECK(clock.overlap_frames() == 68);

    clock.reset();
    CHECK_FALSE(clock.expected_next().has_value());
    CHECK(clock.gap_count() == 0);
    CHECK(clock.observe(0, 10).continuity == ClockContinuity::first);
}

TEST_CASE("SampleClock observes FrameBlock positions") {
    std::vector<float> samples(2 * 64, 0.0f);
    SampleClock clock;
    const std::int64_t positions[] = {0, 64, 128, 256};
    ClockContinuity seen[4]{};
    for (int i = 0; i < 4; ++i) {
        FrameBlock block(samples.data(), 64, 2, positions[i], host_now());
        seen[i] = clock.observe(block).continuity;
    }
    CHECK(seen[0] == ClockContinuity::first);
    CHECK(seen[1] == ClockContinuity::contiguous);
    CHECK(seen[2] == ClockContinuity::contiguous);
    CHECK(seen[3] == ClockContinuity::gap);
    CHECK(clock.gap_frames() == 64);
}
