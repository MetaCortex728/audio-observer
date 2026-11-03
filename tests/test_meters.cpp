#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "audio_observer/frame_block.hpp"
#include "audio_observer/meters.hpp"

using namespace audio_observer;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Interleaved buffer of `frames` frames for `channels` channels, each filled
// by gen(channel, frame).
template <typename Gen>
std::vector<float> generate(std::size_t frames, std::size_t channels, Gen gen) {
    std::vector<float> samples(frames * channels);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            samples[f * channels + c] = gen(c, f);
        }
    }
    return samples;
}

float sine(double amplitude, double hz, double rate, std::size_t frame) {
    return static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(frame) / rate));
}

float square(float amplitude, std::size_t period, std::size_t frame) {
    return (frame % period) < period / 2 ? amplitude : -amplitude;
}

FrameBlock view(const std::vector<float>& samples, std::size_t channels, std::int64_t start = 0) {
    return FrameBlock(samples.data(), samples.size() / channels, channels, start, HostTime{});
}

}  // namespace

TEST_CASE("dBFS conversion and floor") {
    CHECK(linear_to_dbfs(1.0) == doctest::Approx(0.0));
    CHECK(linear_to_dbfs(0.5) == doctest::Approx(-6.0206).epsilon(1e-5));
    CHECK(linear_to_dbfs(0.0) == kDefaultFloorDbfs);
    CHECK(linear_to_dbfs(-1.0) == kDefaultFloorDbfs);
    CHECK(linear_to_dbfs(std::numeric_limits<double>::quiet_NaN()) == kDefaultFloorDbfs);
    CHECK(linear_to_dbfs(1e-12) == kDefaultFloorDbfs);
    CHECK(linear_to_dbfs(1e-4, -60.0) == -60.0);
    CHECK(dbfs_to_linear(-20.0) == doctest::Approx(0.1));
}

TEST_CASE("full-scale sine: RMS -3.01 dBFS, peak 0 dBFS, only the crests clip") {
    // 1 kHz at 48 kHz: exactly 48 samples per period, crests land on samples.
    const auto samples = generate(48000, 1, [](std::size_t, std::size_t f) {
        return sine(1.0, 1000.0, 48000.0, f);
    });
    const BlockLevels levels = measure_block(view(samples, 1));
    REQUIRE(levels.channels.size() == 1);
    const LevelStats& ch = levels.channels[0];
    CHECK(ch.sample_count == 48000);
    CHECK(ch.rms_dbfs() == doctest::Approx(-3.0103).epsilon(1e-4));
    CHECK(std::fabs(ch.rms_dbfs() - (-3.01)) < 0.01);
    CHECK(ch.peak_dbfs() == doctest::Approx(0.0));
    // |sin| >= 0.999 only at the +1 and -1 crests: two samples per period.
    CHECK(ch.clipped_samples == 2000);
}

TEST_CASE("DC levels") {
    const auto positive = generate(1000, 1, [](std::size_t, std::size_t) { return 0.5f; });
    const LevelStats p = measure_block(view(positive, 1)).stream;
    CHECK(p.rms_dbfs() == doctest::Approx(-6.0206).epsilon(1e-5));
    CHECK(p.peak_dbfs() == doctest::Approx(-6.0206).epsilon(1e-5));
    CHECK(p.clipped_samples == 0);

    const auto negative = generate(1000, 1, [](std::size_t, std::size_t) { return -0.25f; });
    const LevelStats n = measure_block(view(negative, 1)).stream;
    CHECK(n.rms_dbfs() == doctest::Approx(-12.0412).epsilon(1e-5));
    CHECK(n.peak_dbfs() == doctest::Approx(-12.0412).epsilon(1e-5));

    const auto full = generate(10, 1, [](std::size_t, std::size_t) { return -1.0f; });
    CHECK(measure_block(view(full, 1)).stream.clipped_samples == 10);
}

TEST_CASE("silence reads as the floor") {
    const std::vector<float> zeros(4800, 0.0f);
    const LevelStats s = measure_block(view(zeros, 2)).stream;
    CHECK(s.sample_count == 4800);
    CHECK(s.rms_dbfs() == kDefaultFloorDbfs);
    CHECK(s.peak_dbfs() == kDefaultFloorDbfs);
    CHECK(s.rms_dbfs(-120.0) == -120.0);
    CHECK(s.clipped_samples == 0);

    // An empty block has no samples and reads as the floor.
    const LevelStats e = measure_block(FrameBlock{}).stream;
    CHECK(e.sample_count == 0);
    CHECK(e.rms_dbfs() == kDefaultFloorDbfs);
}

TEST_CASE("clipped square wave: every sample at full scale counts") {
    const auto clipped = generate(4800, 1, [](std::size_t, std::size_t f) {
        return square(1.0f, 48, f);
    });
    const LevelStats c = measure_block(view(clipped, 1)).stream;
    CHECK(c.rms_dbfs() == doctest::Approx(0.0));
    CHECK(c.peak_dbfs() == doctest::Approx(0.0));
    CHECK(c.clipped_samples == 4800);

    // The threshold is inclusive.
    const auto at_threshold = generate(100, 1, [](std::size_t, std::size_t f) {
        return square(0.999f, 10, f);
    });
    CHECK(measure_block(view(at_threshold, 1)).stream.clipped_samples == 100);

    const auto below = generate(100, 1, [](std::size_t, std::size_t f) {
        return square(0.998f, 10, f);
    });
    CHECK(measure_block(view(below, 1)).stream.clipped_samples == 0);

    // A custom threshold.
    const auto half = generate(100, 1, [](std::size_t, std::size_t f) {
        return square(0.5f, 10, f);
    });
    CHECK(measure_block(view(half, 1), 0.5f).stream.clipped_samples == 100);
    CHECK(measure_block(view(half, 1)).stream.rms_dbfs() == doctest::Approx(-6.0206).epsilon(1e-5));
}

TEST_CASE("per-channel and per-stream levels of a mixed multichannel block") {
    // Channel 0: full-scale sine, 1: silence, 2: DC 0.25, 3: clipped square.
    const auto samples = generate(4800, 4, [](std::size_t c, std::size_t f) {
        switch (c) {
            case 0:
                return sine(1.0, 1000.0, 48000.0, f);
            case 1:
                return 0.0f;
            case 2:
                return 0.25f;
            default:
                return square(1.0f, 48, f);
        }
    });
    const BlockLevels levels = measure_block(view(samples, 4));
    REQUIRE(levels.channels.size() == 4);
    CHECK(levels.channels[0].rms_dbfs() == doctest::Approx(-3.0103).epsilon(1e-4));
    CHECK(levels.channels[1].rms_dbfs() == kDefaultFloorDbfs);
    CHECK(levels.channels[2].rms_dbfs() == doctest::Approx(-12.0412).epsilon(1e-5));
    CHECK(levels.channels[3].clipped_samples == 4800);

    // Stream RMS is over all samples of all channels: (0.5 + 0 + 0.0625 + 1) / 4.
    const double expected_rms = std::sqrt((0.5 + 0.0 + 0.0625 + 1.0) / 4.0);
    CHECK(levels.stream.sample_count == 4 * 4800);
    CHECK(levels.stream.rms() == doctest::Approx(expected_rms).epsilon(1e-6));
    CHECK(levels.stream.peak_dbfs() == doctest::Approx(0.0));
    CHECK(levels.stream.clipped_samples == 200 + 4800);
}

TEST_CASE("peak hold holds, then decays linearly in dB") {
    // 1000 Hz, hold 1 s, decay 20 dB/s.
    PeakHold hold(1000, 1.0, 20.0);
    CHECK(hold.value_dbfs() == kDefaultFloorDbfs);

    hold.update(0.0, 100);
    CHECK(hold.value_dbfs() == 0.0);

    for (int i = 0; i < 10; ++i) {
        hold.update(kDefaultFloorDbfs, 100);  // 1 s of silence
    }
    CHECK(hold.value_dbfs() == 0.0);

    hold.update(kDefaultFloorDbfs, 250);  // 0.25 s past the hold
    CHECK(hold.value_dbfs() == doctest::Approx(-5.0));
    hold.update(-20.0, 250);  // quieter peak does not catch the held level
    CHECK(hold.value_dbfs() == doctest::Approx(-10.0));
    hold.update(-6.0, 100);  // louder than the decayed level: new peak
    CHECK(hold.value_dbfs() == -6.0);

    // Decay never goes below the floor.
    hold.update(kDefaultFloorDbfs, 1'000'000);
    CHECK(hold.value_dbfs() == kDefaultFloorDbfs);

    hold.update(-3.0, 1);
    hold.reset();
    CHECK(hold.value_dbfs() == kDefaultFloorDbfs);
}

TEST_CASE("peak hold with no hold time decays from the next block") {
    PeakHold hold(48000, 0.0, 48.0, -100.0);
    hold.update(-10.0, 480);
    CHECK(hold.value_dbfs() == -10.0);
    hold.update(-100.0, 4800);  // 0.1 s at 48 dB/s
    CHECK(hold.value_dbfs() == doctest::Approx(-14.8));
}

TEST_CASE("silence detector needs the minimum quiet duration") {
    SilenceDetector detector(500);
    for (int i = 0; i < 499; ++i) {
        detector.observe(true);
    }
    CHECK_FALSE(detector.silent());
    detector.observe(true);
    CHECK(detector.silent());
    CHECK(detector.silence_count() == 1);
    detector.observe(true);
    CHECK(detector.silence_count() == 1);  // one event per quiet run

    detector.observe(false);
    CHECK_FALSE(detector.silent());
    CHECK(detector.quiet_run_frames() == 0);
    for (int i = 0; i < 500; ++i) {
        detector.observe(true);
    }
    CHECK(detector.silent());
    CHECK(detector.silence_count() == 2);

    SilenceDetector immediate(0);
    immediate.observe(true);
    CHECK(immediate.silent());
    CHECK(immediate.silence_count() == 1);
}

TEST_CASE("StreamMeter: channel and stream readings across blocks") {
    MeterConfig config;
    config.peak_hold_seconds = 1.0;
    config.peak_decay_db_per_second = 20.0;
    config.silence_threshold_dbfs = -60.0;
    config.silence_min_seconds = 0.5;
    StreamMeter meter(1000, 2, config);
    CHECK(meter.channel_count() == 2);

    // 0.1 s: channel 0 full-scale square, channel 1 quiet noise floor at -66 dBFS.
    const auto loud = generate(100, 2, [](std::size_t c, std::size_t f) {
        return c == 0 ? square(1.0f, 10, f) : 0.0005f;
    });
    meter.process(view(loud, 2, 0));

    MeterReading ch0 = meter.channel(0);
    MeterReading ch1 = meter.channel(1);
    CHECK(ch0.rms_dbfs == doctest::Approx(0.0));
    CHECK(ch0.clipped_samples == 100);
    CHECK(ch0.peak_hold_dbfs == doctest::Approx(0.0));
    CHECK_FALSE(ch0.silent);
    CHECK(ch1.peak_dbfs == doctest::Approx(-66.0206).epsilon(1e-5));
    CHECK_FALSE(ch1.silent);  // quiet, but only for 0.1 s

    // 1.5 s of silence on both channels, in blocks of 100 frames.
    const std::vector<float> zeros(200, 0.0f);
    for (int i = 0; i < 15; ++i) {
        meter.process(view(zeros, 2, 100 + 100 * i));
    }
    ch0 = meter.channel(0);
    ch1 = meter.channel(1);
    CHECK(ch0.silent);
    CHECK(ch1.silent);
    CHECK(ch0.silence_count == 1);
    CHECK(ch1.silence_count == 1);
    CHECK(meter.stream().silent);  // channel 1 was quiet throughout, channel 0 for 1.5 s
    CHECK(meter.stream().silence_count == 1);
    // Held 1 s after the peak, then 0.5 s of decay at 20 dB/s.
    CHECK(ch0.peak_hold_dbfs == doctest::Approx(-10.0));
    CHECK(meter.stream().peak_hold_dbfs == doctest::Approx(-10.0));

    // Totals cover all 1.6 s: channel 0 is full scale for 0.1 s of it.
    CHECK(ch0.sample_count == 1600);
    CHECK(ch0.rms_dbfs == doctest::Approx(linear_to_dbfs(std::sqrt(100.0 / 1600.0))));
    CHECK(ch0.peak_dbfs == doctest::Approx(0.0));
    const MeterReading stream = meter.stream();
    CHECK(stream.sample_count == 3200);
    CHECK(stream.clipped_samples == 100);

    // A new interval clears levels but keeps hold and silence state.
    meter.reset_levels();
    ch0 = meter.channel(0);
    CHECK(ch0.sample_count == 0);
    CHECK(ch0.rms_dbfs == kDefaultFloorDbfs);
    CHECK(ch0.clipped_samples == 0);
    CHECK(ch0.peak_hold_dbfs == doctest::Approx(-10.0));
    CHECK(ch0.silent);

    meter.reset();
    CHECK(meter.channel(0).peak_hold_dbfs == kDefaultFloorDbfs);
    CHECK_FALSE(meter.channel(0).silent);
    CHECK(meter.stream().silence_count == 0);
}

TEST_CASE("StreamMeter: one loud channel keeps the stream from being silent") {
    MeterConfig config;
    config.silence_min_seconds = 0.01;
    StreamMeter meter(48000, 3, config);
    const auto samples = generate(4800, 3, [](std::size_t c, std::size_t f) {
        return c == 2 ? sine(0.5, 1000.0, 48000.0, f) : 0.0f;
    });
    meter.process(view(samples, 3));
    CHECK(meter.channel(0).silent);
    CHECK(meter.channel(1).silent);
    CHECK_FALSE(meter.channel(2).silent);
    CHECK_FALSE(meter.stream().silent);
    CHECK(meter.channel(2).rms_dbfs == doctest::Approx(-9.0309).epsilon(1e-4));
}

TEST_CASE("StreamMeter rejects bad set-up and mismatched blocks") {
    CHECK_THROWS_AS(StreamMeter(0, 2), std::invalid_argument);
    CHECK_THROWS_AS(StreamMeter(48000, 0), std::invalid_argument);

    StreamMeter meter(48000, 2);
    const std::vector<float> mono(10, 0.0f);
    CHECK_THROWS_AS(meter.process(view(mono, 1)), std::invalid_argument);
}
