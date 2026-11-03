#include "audio_observer/meters.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace audio_observer {

double linear_to_dbfs(double linear, double floor_dbfs) noexcept {
    if (!(linear > 0.0)) {
        return floor_dbfs;
    }
    return std::max(20.0 * std::log10(linear), floor_dbfs);
}

double dbfs_to_linear(double dbfs) noexcept {
    return std::pow(10.0, dbfs / 20.0);
}

void LevelStats::add(float sample, float clip_threshold) noexcept {
    const float magnitude = std::fabs(sample);
    ++sample_count;
    sum_of_squares += static_cast<double>(sample) * static_cast<double>(sample);
    if (magnitude > peak) {
        peak = magnitude;
    }
    if (magnitude >= clip_threshold) {
        ++clipped_samples;
    }
}

void LevelStats::merge(const LevelStats& other) noexcept {
    sample_count += other.sample_count;
    sum_of_squares += other.sum_of_squares;
    peak = std::max(peak, other.peak);
    clipped_samples += other.clipped_samples;
}

double LevelStats::rms() const noexcept {
    if (sample_count == 0) {
        return 0.0;
    }
    return std::sqrt(sum_of_squares / static_cast<double>(sample_count));
}

double LevelStats::rms_dbfs(double floor_dbfs) const noexcept {
    return linear_to_dbfs(rms(), floor_dbfs);
}

double LevelStats::peak_dbfs(double floor_dbfs) const noexcept {
    return linear_to_dbfs(static_cast<double>(peak), floor_dbfs);
}

BlockLevels measure_block(const FrameBlock& block, float clip_threshold) {
    BlockLevels levels;
    levels.channels.resize(block.channel_count());
    const float* samples = block.data();
    const std::size_t channels = block.channel_count();
    for (std::size_t f = 0; f < block.frame_count(); ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            levels.channels[c].add(samples[f * channels + c], clip_threshold);
        }
    }
    for (const LevelStats& channel : levels.channels) {
        levels.stream.merge(channel);
    }
    return levels;
}

PeakHold::PeakHold(std::uint32_t sample_rate_hz,
                   double hold_seconds,
                   double decay_db_per_second,
                   double floor_dbfs) noexcept
    : frames_per_second_(static_cast<double>(sample_rate_hz)),
      hold_frames_(static_cast<std::uint64_t>(
          std::llround(std::max(hold_seconds, 0.0) * static_cast<double>(sample_rate_hz)))),
      decay_db_per_second_(std::max(decay_db_per_second, 0.0)),
      floor_dbfs_(floor_dbfs),
      held_dbfs_(floor_dbfs) {}

void PeakHold::update(double block_peak_dbfs, std::uint64_t frames) noexcept {
    const std::uint64_t before = frames_since_peak_;
    frames_since_peak_ += frames;
    if (frames_since_peak_ > hold_frames_ && frames_per_second_ > 0.0) {
        const std::uint64_t decaying_from = std::max(before, hold_frames_);
        const auto decay_frames = static_cast<double>(frames_since_peak_ - decaying_from);
        held_dbfs_ -= decay_db_per_second_ * decay_frames / frames_per_second_;
        held_dbfs_ = std::max(held_dbfs_, floor_dbfs_);
    }
    if (block_peak_dbfs >= held_dbfs_) {
        held_dbfs_ = std::max(block_peak_dbfs, floor_dbfs_);
        frames_since_peak_ = 0;
    }
}

void PeakHold::reset() noexcept {
    held_dbfs_ = floor_dbfs_;
    frames_since_peak_ = 0;
}

namespace {

std::uint64_t seconds_to_frames(double seconds, std::uint32_t sample_rate_hz) {
    return static_cast<std::uint64_t>(
        std::llround(std::max(seconds, 0.0) * static_cast<double>(sample_rate_hz)));
}

std::uint32_t checked_rate(std::uint32_t sample_rate_hz, std::size_t channel_count) {
    if (sample_rate_hz == 0 || channel_count == 0) {
        throw std::invalid_argument("StreamMeter needs a sample rate and at least one channel");
    }
    return sample_rate_hz;
}

}  // namespace

StreamMeter::StreamMeter(std::uint32_t sample_rate_hz,
                         std::size_t channel_count,
                         MeterConfig config)
    : config_(config),
      quiet_below_(static_cast<float>(dbfs_to_linear(config.silence_threshold_dbfs))),
      stream_hold_(checked_rate(sample_rate_hz, channel_count),
                   config.peak_hold_seconds,
                   config.peak_decay_db_per_second,
                   config.floor_dbfs),
      stream_silence_(seconds_to_frames(config.silence_min_seconds, sample_rate_hz)) {
    const PeakHold hold(sample_rate_hz, config.peak_hold_seconds,
                        config.peak_decay_db_per_second, config.floor_dbfs);
    const SilenceDetector silence(seconds_to_frames(config.silence_min_seconds, sample_rate_hz));
    channels_.assign(channel_count, Channel{LevelStats{}, LevelStats{}, hold, silence});
}

void StreamMeter::process(const FrameBlock& block) {
    const std::size_t channels = channels_.size();
    if (block.channel_count() != channels) {
        throw std::invalid_argument("FrameBlock channel count does not match the meter");
    }
    for (Channel& channel : channels_) {
        channel.block = LevelStats{};
    }

    const float* samples = block.data();
    for (std::size_t f = 0; f < block.frame_count(); ++f) {
        bool frame_quiet = true;
        for (std::size_t c = 0; c < channels; ++c) {
            const float sample = samples[f * channels + c];
            Channel& channel = channels_[c];
            channel.block.add(sample, config_.clip_threshold);
            const bool quiet = std::fabs(sample) < quiet_below_;
            channel.silence.observe(quiet);
            frame_quiet = frame_quiet && quiet;
        }
        stream_silence_.observe(frame_quiet);
    }

    LevelStats stream_block;
    for (Channel& channel : channels_) {
        channel.totals.merge(channel.block);
        channel.hold.update(channel.block.peak_dbfs(config_.floor_dbfs), block.frame_count());
        stream_block.merge(channel.block);
    }
    stream_totals_.merge(stream_block);
    stream_hold_.update(stream_block.peak_dbfs(config_.floor_dbfs), block.frame_count());
}

MeterReading StreamMeter::reading(const LevelStats& totals,
                                  const PeakHold& hold,
                                  const SilenceDetector& silence) const noexcept {
    MeterReading r;
    r.rms_dbfs = totals.rms_dbfs(config_.floor_dbfs);
    r.peak_dbfs = totals.peak_dbfs(config_.floor_dbfs);
    r.peak_hold_dbfs = hold.value_dbfs();
    r.clipped_samples = totals.clipped_samples;
    r.sample_count = totals.sample_count;
    r.silent = silence.silent();
    r.silence_count = silence.silence_count();
    return r;
}

MeterReading StreamMeter::channel(std::size_t index) const noexcept {
    const Channel& c = channels_[index];
    return reading(c.totals, c.hold, c.silence);
}

MeterReading StreamMeter::stream() const noexcept {
    return reading(stream_totals_, stream_hold_, stream_silence_);
}

void StreamMeter::reset_levels() noexcept {
    for (Channel& channel : channels_) {
        channel.totals = LevelStats{};
    }
    stream_totals_ = LevelStats{};
}

void StreamMeter::reset() noexcept {
    reset_levels();
    for (Channel& channel : channels_) {
        channel.hold.reset();
        channel.silence.reset();
    }
    stream_hold_.reset();
    stream_silence_.reset();
}

}  // namespace audio_observer
