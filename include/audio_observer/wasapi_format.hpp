#pragma once

/// @file
/// The platform-independent parts of the WASAPI backend: reading a mix
/// format, converting device samples to float, and turning a packet's device
/// position, buffer flags and performance-counter time into block fields.
///
/// Nothing here includes Windows headers, so it is compiled and unit-tested
/// on every platform with fake inputs. The constants below mirror documented
/// Windows SDK values; the WASAPI source checks them against the SDK with
/// static_assert.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "audio_observer/audio_format.hpp"
#include "audio_observer/frame_block.hpp"

namespace audio_observer::wasapi {

/// WAVEFORMATEX::wFormatTag values that a shared-mode mix format can use.
inline constexpr std::uint16_t kFormatTagPcm = 0x0001;         // WAVE_FORMAT_PCM
inline constexpr std::uint16_t kFormatTagIeeeFloat = 0x0003;   // WAVE_FORMAT_IEEE_FLOAT
inline constexpr std::uint16_t kFormatTagExtensible = 0xFFFE;  // WAVE_FORMAT_EXTENSIBLE

/// AUDCLNT_BUFFERFLAGS_* bits returned by IAudioCaptureClient::GetBuffer.
inline constexpr std::uint32_t kBufferFlagDataDiscontinuity = 0x1;
inline constexpr std::uint32_t kBufferFlagSilent = 0x2;
inline constexpr std::uint32_t kBufferFlagTimestampError = 0x4;

/// WAVEFORMATEXTENSIBLE::SubFormat, reduced to what matters here.
enum class SubFormat : std::uint8_t {
    none,        ///< Not an extensible format.
    pcm,         ///< KSDATAFORMAT_SUBTYPE_PCM
    ieee_float,  ///< KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    other,       ///< Anything else (unsupported).
};

/// The fields of a WAVEFORMATEX or WAVEFORMATEXTENSIBLE that decide how to
/// read its samples.
struct WaveFormatFields {
    std::uint16_t format_tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t samples_per_sec = 0;
    std::uint16_t block_align = 0;      ///< Bytes per frame.
    std::uint16_t bits_per_sample = 0;  ///< Container size in bits.
    /// WAVEFORMATEXTENSIBLE::Samples.wValidBitsPerSample; 0 when not extensible.
    std::uint16_t valid_bits_per_sample = 0;
    SubFormat sub_format = SubFormat::none;
};

/// How to read one sample.
enum class Container : std::uint8_t {
    float32,  ///< 4-byte IEEE float.
    int16,    ///< 2-byte signed integer.
    int24,    ///< 3-byte packed signed integer.
    int32,    ///< 4-byte signed integer; also 24 (or 20) valid bits left-justified.
};

/// A supported mix format: what the analysis sees, and how to read it.
struct SampleLayout {
    AudioFormat format;  ///< sample_type reports the valid bits (int24 for 24-in-32).
    Container container = Container::float32;
    std::size_t bytes_per_frame = 0;
};

/// Interprets a mix format. Returns empty for formats this backend does not
/// convert (for example 8-bit or 64-bit float samples, a zero channel count
/// or a block alignment that does not match the channels and container), and
/// then sets @p why when it is not null.
std::optional<SampleLayout> describe_mix_format(const WaveFormatFields& fields,
                                                std::string* why = nullptr);

/// Converts @p frame_count interleaved frames from @p source, laid out as
/// @p layout says, to float in [-1, 1] at @p destination (frame_count x
/// channel_count floats). @p source need not be aligned. Integer samples are
/// scaled by 2^(bits - 1), so the most negative value maps to exactly -1.
/// Does not allocate.
void convert_to_float(const void* source, std::size_t frame_count, const SampleLayout& layout,
                      float* destination) noexcept;

/// Converts a performance-counter reading to 100-nanosecond units without
/// overflowing for any realistic uptime. @p frequency must be non-zero.
std::uint64_t qpc_ticks_to_hns(std::uint64_t ticks, std::uint64_t frequency) noexcept;

/// Block fields derived from one GetBuffer() call.
struct PacketMapping {
    std::int64_t first_sample_index = 0;  ///< From u64DevicePosition (frames).
    HostTime host_time{};                 ///< From u64QPCPosition (100 ns units).
    std::uint32_t flags = 0;              ///< block_flags bits.
    /// The packet should be treated as silence (AUDCLNT_BUFFERFLAGS_SILENT):
    /// its data must not be read and zeros are delivered instead. Silence is
    /// a normal condition, not a fault, so it sets no block flag; the meters
    /// see it as silence.
    bool silent = false;
};

/// Maps a packet's device position, QPC position and buffer flags.
///
/// - The device position counts frames from the start of the stream and
///   becomes the block's sample index (clamped to the int64 range).
/// - The QPC position, in 100 ns units, plus @p qpc_to_host_offset (host
///   clock minus QPC clock, measured when the stream starts) becomes the
///   host time at which the device captured the first frame.
/// - AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY becomes
///   block_flags::discontinuity, AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR becomes
///   block_flags::timestamp_error, and AUDCLNT_BUFFERFLAGS_SILENT sets
///   PacketMapping::silent. Unknown bits are ignored.
PacketMapping map_packet(std::uint64_t device_position, std::uint64_t qpc_position_hns,
                         std::uint32_t buffer_flags,
                         std::chrono::nanoseconds qpc_to_host_offset) noexcept;

}  // namespace audio_observer::wasapi
