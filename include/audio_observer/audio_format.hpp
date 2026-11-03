#pragma once

/// @file
/// Description of a stream's sample format.

#include <cstddef>
#include <cstdint>

namespace audio_observer {

/// Sample encoding delivered by a device. Analysis always works on float
/// samples; this records what the device produced before conversion.
enum class SampleType : std::uint8_t {
    float32,  ///< 32-bit IEEE float, nominal range [-1, 1].
    int16,    ///< 16-bit signed integer PCM.
    int24,    ///< 24-bit signed integer PCM (packed or in 32-bit containers).
    int32,    ///< 32-bit signed integer PCM.
};

/// Returns the size in bytes of one sample of @p type as delivered by a
/// device (3 for packed 24-bit).
std::size_t bytes_per_sample(SampleType type) noexcept;

/// Sample rate, channel count and sample type of a stream.
struct AudioFormat {
    std::uint32_t sample_rate_hz = 0;  ///< Frames per second, e.g. 48000.
    std::uint32_t channel_count = 0;   ///< Interleaved channels per frame (>= 1 when valid).
    SampleType sample_type = SampleType::float32;

    /// True when the sample rate and channel count are both non-zero.
    bool valid() const noexcept { return sample_rate_hz > 0 && channel_count > 0; }
};

bool operator==(const AudioFormat& a, const AudioFormat& b) noexcept;
bool operator!=(const AudioFormat& a, const AudioFormat& b) noexcept;

}  // namespace audio_observer
