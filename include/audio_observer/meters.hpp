#pragma once

/// @file
/// Level meters: RMS and peak in dBFS, peak hold, clipping counts and silence
/// detection, per channel and per stream.
///
/// Levels are relative to digital full scale: a sample of magnitude 1.0 is
/// 0 dBFS, so a full-scale sine has an RMS level of -3.01 dBFS. Meters keep
/// only running totals; samples are read once and never stored.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio_observer/frame_block.hpp"

namespace audio_observer {

/// Level reported for silence, matching the prototype (20 * log10(1e-9)).
inline constexpr double kDefaultFloorDbfs = -180.0;
/// Default magnitude at or beyond which a sample counts as clipped.
inline constexpr float kDefaultClipThreshold = 0.999f;

/// Converts a linear magnitude to dBFS, clamped to @p floor_dbfs. Zero,
/// negative and NaN inputs return the floor.
double linear_to_dbfs(double linear, double floor_dbfs = kDefaultFloorDbfs) noexcept;

/// Converts dBFS to a linear magnitude.
double dbfs_to_linear(double dbfs) noexcept;

/// Running level totals for one channel or for all channels of a stream.
struct LevelStats {
    std::uint64_t sample_count = 0;
    double sum_of_squares = 0.0;
    float peak = 0.0f;                  ///< Largest magnitude seen.
    std::uint64_t clipped_samples = 0;  ///< Samples with magnitude >= the clip threshold.

    /// Adds one sample.
    void add(float sample, float clip_threshold = kDefaultClipThreshold) noexcept;
    /// Combines totals from another span of samples.
    void merge(const LevelStats& other) noexcept;

    /// Linear RMS, or 0 when no samples were added.
    double rms() const noexcept;
    double rms_dbfs(double floor_dbfs = kDefaultFloorDbfs) const noexcept;
    double peak_dbfs(double floor_dbfs = kDefaultFloorDbfs) const noexcept;
};

/// Levels of one block, per channel and for the stream as a whole (all
/// samples of all channels).
struct BlockLevels {
    std::vector<LevelStats> channels;
    LevelStats stream;
};

/// Measures one block. Allocates the result; use StreamMeter where
/// allocation matters.
BlockLevels measure_block(const FrameBlock& block,
                          float clip_threshold = kDefaultClipThreshold);

/// Peak hold with linear decay in dB: the held level stays at the highest
/// recent peak for a hold time, then falls at a fixed rate until a new peak
/// reaches it. Time is counted in frames, so results are exact for a given
/// sample rate. Updated once per block.
class PeakHold {
public:
    PeakHold(std::uint32_t sample_rate_hz,
             double hold_seconds,
             double decay_db_per_second,
             double floor_dbfs = kDefaultFloorDbfs) noexcept;

    /// Advances by @p frames and then takes the block's peak into account.
    void update(double block_peak_dbfs, std::uint64_t frames) noexcept;

    /// Held level in dBFS, never below the floor.
    double value_dbfs() const noexcept { return held_dbfs_; }

    /// Drops the held level to the floor.
    void reset() noexcept;

private:
    double frames_per_second_;
    std::uint64_t hold_frames_;
    double decay_db_per_second_;
    double floor_dbfs_;
    double held_dbfs_;
    std::uint64_t frames_since_peak_ = 0;
};

/// Detects silence: a signal is silent once its magnitude has stayed below a
/// threshold for at least a minimum number of consecutive frames.
class SilenceDetector {
public:
    /// A minimum of zero frames is treated as one.
    explicit SilenceDetector(std::uint64_t min_frames) noexcept
        : min_frames_(min_frames == 0 ? 1 : min_frames) {}

    /// Feeds one frame, @p quiet when it is below the threshold.
    void observe(bool quiet) noexcept {
        if (!quiet) {
            quiet_run_ = 0;
            return;
        }
        ++quiet_run_;
        if (quiet_run_ == min_frames_) {
            ++silence_count_;
        }
    }

    /// True while the current quiet run has lasted at least the minimum.
    bool silent() const noexcept { return quiet_run_ >= min_frames_; }
    /// Length of the current run of quiet frames.
    std::uint64_t quiet_run_frames() const noexcept { return quiet_run_; }
    /// Number of times the signal has become silent.
    std::uint64_t silence_count() const noexcept { return silence_count_; }

    void reset() noexcept {
        quiet_run_ = 0;
        silence_count_ = 0;
    }

private:
    std::uint64_t min_frames_;
    std::uint64_t quiet_run_ = 0;
    std::uint64_t silence_count_ = 0;
};

/// Settings for StreamMeter.
struct MeterConfig {
    float clip_threshold = kDefaultClipThreshold;
    double floor_dbfs = kDefaultFloorDbfs;
    double peak_hold_seconds = 1.0;
    double peak_decay_db_per_second = 20.0;
    double silence_threshold_dbfs = -60.0;  ///< Samples below this magnitude are quiet.
    double silence_min_seconds = 0.5;       ///< Quiet time before a signal counts as silent.
};

/// Snapshot of one channel or of the whole stream.
struct MeterReading {
    double rms_dbfs = kDefaultFloorDbfs;
    double peak_dbfs = kDefaultFloorDbfs;
    double peak_hold_dbfs = kDefaultFloorDbfs;
    std::uint64_t clipped_samples = 0;
    std::uint64_t sample_count = 0;
    bool silent = false;
    std::uint64_t silence_count = 0;
};

/// Meters for every channel of one stream and for the stream as a whole.
/// Allocates only in the constructor. The stream counts as silent when all
/// channels are quiet for the minimum duration at the same time.
class StreamMeter {
public:
    /// Throws std::invalid_argument when @p sample_rate_hz or
    /// @p channel_count is zero.
    StreamMeter(std::uint32_t sample_rate_hz, std::size_t channel_count, MeterConfig config = {});

    /// Feeds one block. Throws std::invalid_argument when its channel count
    /// differs from the meter's.
    void process(const FrameBlock& block);

    std::size_t channel_count() const noexcept { return channels_.size(); }
    const MeterConfig& config() const noexcept { return config_; }

    /// Reading for channel @p index (must be in range).
    MeterReading channel(std::size_t index) const noexcept;
    /// Reading over all channels.
    MeterReading stream() const noexcept;

    /// Starts a new measurement interval: clears RMS, peak, clipping and
    /// sample counts, but keeps peak hold and silence state.
    void reset_levels() noexcept;
    /// Clears everything.
    void reset() noexcept;

private:
    struct Channel {
        LevelStats totals;
        LevelStats block;
        PeakHold hold;
        SilenceDetector silence;
    };

    MeterReading reading(const LevelStats& totals,
                         const PeakHold& hold,
                         const SilenceDetector& silence) const noexcept;

    MeterConfig config_;
    float quiet_below_;
    std::vector<Channel> channels_;
    LevelStats stream_totals_;
    PeakHold stream_hold_;
    SilenceDetector stream_silence_;
};

}  // namespace audio_observer
