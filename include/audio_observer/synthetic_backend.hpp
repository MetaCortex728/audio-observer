#pragma once

/// @file
/// A backend that generates signals and faults deterministically from a
/// seed, so analyses can be tested against known answers without hardware.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio_observer/backend.hpp"

namespace audio_observer {

/// Signal generated on every channel of a synthetic endpoint.
enum class SignalKind : std::uint8_t {
    sine,          ///< Sine at frequency_hz and level_dbfs (peak level).
    white_noise,   ///< Uniform white noise with peak magnitude level_dbfs.
    silence,       ///< All zeros.
    clipped_tone,  ///< Sine at level_dbfs (above 0 dBFS) hard-clipped to [-1, 1].
};

/// Returns "sine", "white_noise", "silence" or "clipped_tone".
const char* to_string(SignalKind kind) noexcept;

struct SignalConfig {
    SignalKind kind = SignalKind::sine;
    double frequency_hz = 440.0;
    double level_dbfs = -12.0;
};

/// A fault applied once, at the first block whose start position (before
/// the fault) is at or after @c at_frame.
enum class FaultKind : std::uint8_t {
    dropout,        ///< Skip @c frames positions: the block starts @c frames later.
    overlap,        ///< Step back @c frames positions: the block repeats positions.
    discontinuity,  ///< Flag the block with block_flags::discontinuity.
    overrun,        ///< Drop the block as if the queue were full.
    failure,        ///< The device fails: no more blocks, state becomes failed.
};

/// Returns the lower-case name of @p kind.
const char* to_string(FaultKind kind) noexcept;

struct Fault {
    FaultKind kind = FaultKind::dropout;
    std::int64_t at_frame = 0;
    std::int64_t frames = 0;  ///< Size of a dropout or overlap.
};

/// One synthetic endpoint and what its streams produce.
struct SyntheticEndpoint {
    EndpointInfo info;
    SignalConfig signal;
    std::vector<Fault> faults;
    /// Device clock rate relative to the host clock, in parts per million:
    /// +100 means the device delivers 48004.8 frames per host second at 48 kHz.
    double drift_ppm = 0.0;
    /// Host timestamps are offset by a uniform random amount in
    /// [-jitter_seconds, +jitter_seconds].
    double jitter_seconds = 0.0;
};

/// How fast synthetic streams deliver.
enum class Pacing : std::uint8_t {
    /// Blocks arrive when their last frame would have been captured on the
    /// host clock. A full queue causes overruns, as with a real device.
    real_time,
    /// Blocks arrive as fast as the consumer takes them: the stream waits
    /// for queue space instead of overrunning. For tests.
    fast,
};

/// Default names, matching the roles of the prototype's endpoints.
inline constexpr const char* kSyntheticMicName = "Synthetic Microphone";
inline constexpr const char* kSyntheticOutputName = "Synthetic Headset";

struct SyntheticConfig {
    std::uint64_t seed = 1;
    Pacing pacing = Pacing::real_time;
    std::size_t block_frames = 480;
    /// Streams finish after delivering (or dropping) this many seconds of
    /// frames at the nominal rate. Unlimited when empty.
    std::optional<double> stop_after_seconds;
    std::vector<SyntheticEndpoint> endpoints = default_endpoints();

    /// A mono 48 kHz microphone (sine), a stereo 48 kHz headset output with
    /// loopback (sine), and one input each for white noise, silence and a
    /// clipped tone.
    static std::vector<SyntheticEndpoint> default_endpoints();
};

/// Generates the signal of one endpoint, deterministic from a seed. Exposed
/// for tests; streams use it internally.
class SignalGenerator {
public:
    SignalGenerator(SignalConfig signal, std::uint32_t sample_rate_hz,
                    std::size_t channel_count, std::uint64_t seed) noexcept;
    /// Writes @p frame_count interleaved frames to @p out.
    void generate(float* out, std::size_t frame_count) noexcept;

private:
    SignalConfig signal_;
    double amplitude_;
    double phase_ = 0.0;
    double phase_step_;
    std::size_t channel_count_;
    std::uint64_t rng_;
};

/// The synthetic backend. Every stream gets its own seed derived from the
/// configured seed, the endpoint name and the order of open() calls, so a
/// session is reproducible. Several streams may share an endpoint, like
/// shared-mode clients.
class SyntheticBackend final : public Backend {
public:
    explicit SyntheticBackend(SyntheticConfig config = {});
    ~SyntheticBackend() override;

    std::string name() const override { return "synthetic"; }
    std::vector<EndpointInfo> endpoints() override;
    std::unique_ptr<CaptureStream> open(const std::string& endpoint_name,
                                        StreamDirection direction) override;

    const SyntheticConfig& config() const noexcept { return config_; }

    /// Streams opened and not yet destroyed (released).
    int open_stream_count() const noexcept;
    /// Streams whose delivery thread is running.
    int running_stream_count() const noexcept;
    /// Total successful open() calls.
    int total_opened() const noexcept;

    struct Counters {
        std::atomic<int> open{0};
        std::atomic<int> running{0};
        std::atomic<int> total{0};
    };

private:
    SyntheticConfig config_;
    std::shared_ptr<Counters> counters_;
};

}  // namespace audio_observer
