#pragma once

/// @file
/// Static description of one audio stream.

#include <cstdint>
#include <string>
#include <vector>

#include "audio_observer/audio_format.hpp"

namespace audio_observer {

/// Where a stream's samples come from.
enum class StreamDirection : std::uint8_t {
    capture,   ///< An input device such as a microphone.
    loopback,  ///< The signal sent to a playback device, captured as it is rendered.
};

/// Position of one channel's transducer in metres, in a device-defined frame.
struct ChannelPosition {
    double x_m = 0.0;
    double y_m = 0.0;
    double z_m = 0.0;
};

/// Identity, direction and format of a stream, plus optional device geometry.
struct StreamInfo {
    std::string id;    ///< Stable identifier, e.g. a backend endpoint ID.
    std::string name;  ///< Human-readable name.
    StreamDirection direction = StreamDirection::capture;
    AudioFormat format;

    /// Per-channel positions in metres. Empty when the geometry is unknown,
    /// which is the default; otherwise one entry per channel.
    std::vector<ChannelPosition> channel_positions;

    /// True when a geometry is present.
    bool has_geometry() const noexcept { return !channel_positions.empty(); }

    /// True when the format is valid and the geometry is either absent or has
    /// exactly one position per channel.
    bool valid() const noexcept {
        return format.valid() &&
               (channel_positions.empty() || channel_positions.size() == format.channel_count);
    }
};

}  // namespace audio_observer
