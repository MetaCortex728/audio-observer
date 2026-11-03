#include "audio_observer/wasapi_format.hpp"

#include <cstring>
#include <limits>

#include "audio_observer/block_queue.hpp"

namespace audio_observer::wasapi {

namespace {

bool fail(std::string* why, const char* reason) {
    if (why != nullptr) {
        *why = reason;
    }
    return false;
}

std::size_t container_bytes(Container container) noexcept {
    switch (container) {
        case Container::int16:
            return 2;
        case Container::int24:
            return 3;
        case Container::float32:
        case Container::int32:
            return 4;
    }
    return 0;
}

}  // namespace

std::optional<SampleLayout> describe_mix_format(const WaveFormatFields& f, std::string* why) {
    if (f.channels == 0 || f.samples_per_sec == 0) {
        fail(why, "mix format has no channels or no sample rate");
        return std::nullopt;
    }

    bool is_float = false;
    bool is_pcm = false;
    std::uint16_t valid_bits = f.bits_per_sample;
    if (f.format_tag == kFormatTagIeeeFloat) {
        is_float = true;
    } else if (f.format_tag == kFormatTagPcm) {
        is_pcm = true;
    } else if (f.format_tag == kFormatTagExtensible) {
        is_float = f.sub_format == SubFormat::ieee_float;
        is_pcm = f.sub_format == SubFormat::pcm;
        if (f.valid_bits_per_sample != 0) {
            valid_bits = f.valid_bits_per_sample;
        }
    }
    if (!is_float && !is_pcm) {
        fail(why, "mix format is neither PCM nor IEEE float");
        return std::nullopt;
    }
    if (valid_bits > f.bits_per_sample) {
        fail(why, "mix format has more valid bits than its container");
        return std::nullopt;
    }

    SampleLayout layout;
    layout.format.sample_rate_hz = f.samples_per_sec;
    layout.format.channel_count = f.channels;
    if (is_float) {
        if (f.bits_per_sample != 32 || valid_bits != 32) {
            fail(why, "only 32-bit float mix formats are supported");
            return std::nullopt;
        }
        layout.container = Container::float32;
        layout.format.sample_type = SampleType::float32;
    } else if (f.bits_per_sample == 16 && valid_bits == 16) {
        layout.container = Container::int16;
        layout.format.sample_type = SampleType::int16;
    } else if (f.bits_per_sample == 24 && valid_bits == 24) {
        layout.container = Container::int24;
        layout.format.sample_type = SampleType::int24;
    } else if (f.bits_per_sample == 32 && valid_bits >= 17) {
        // 24 (or 20) valid bits are left-justified in a 32-bit container, so
        // reading the whole container as int32 scales them correctly.
        layout.container = Container::int32;
        layout.format.sample_type = valid_bits == 32 ? SampleType::int32 : SampleType::int24;
    } else {
        fail(why, "only 16-, 24- and 32-bit PCM mix formats are supported");
        return std::nullopt;
    }
    layout.bytes_per_frame = container_bytes(layout.container) * f.channels;
    if (f.block_align != layout.bytes_per_frame) {
        fail(why, "mix format block alignment does not match its channels and sample size");
        return std::nullopt;
    }
    return layout;
}

void convert_to_float(const void* source, std::size_t frame_count, const SampleLayout& layout,
                      float* destination) noexcept {
    const auto* in = static_cast<const unsigned char*>(source);
    const std::size_t samples = frame_count * layout.format.channel_count;
    switch (layout.container) {
        case Container::float32:
            std::memcpy(destination, in, samples * sizeof(float));
            return;
        case Container::int16:
            for (std::size_t i = 0; i < samples; ++i) {
                std::int16_t v = 0;
                std::memcpy(&v, in + 2 * i, 2);
                destination[i] = static_cast<float>(v) * (1.0f / 32768.0f);
            }
            return;
        case Container::int24:
            for (std::size_t i = 0; i < samples; ++i) {
                const unsigned char* p = in + 3 * i;  // little-endian
                std::uint32_t u = static_cast<std::uint32_t>(p[0]) |
                                  (static_cast<std::uint32_t>(p[1]) << 8) |
                                  (static_cast<std::uint32_t>(p[2]) << 16);
                if ((u & 0x800000u) != 0) {
                    u |= 0xFF000000u;  // sign-extend
                }
                std::int32_t v = 0;
                std::memcpy(&v, &u, 4);
                destination[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
            }
            return;
        case Container::int32:
            for (std::size_t i = 0; i < samples; ++i) {
                std::int32_t v = 0;
                std::memcpy(&v, in + 4 * i, 4);
                destination[i] = static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
            }
            return;
    }
}

std::uint64_t qpc_ticks_to_hns(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    constexpr std::uint64_t kHnsPerSecond = 10'000'000;
    return (ticks / frequency) * kHnsPerSecond + (ticks % frequency) * kHnsPerSecond / frequency;
}

PacketMapping map_packet(std::uint64_t device_position, std::uint64_t qpc_position_hns,
                         std::uint32_t buffer_flags,
                         std::chrono::nanoseconds qpc_to_host_offset) noexcept {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    PacketMapping m;
    m.first_sample_index = static_cast<std::int64_t>(device_position > kMax ? kMax : device_position);
    const std::uint64_t hns = qpc_position_hns > kMax / 100 ? kMax / 100 : qpc_position_hns;
    m.host_time = HostTime(std::chrono::nanoseconds(static_cast<std::int64_t>(hns) * 100) +
                           qpc_to_host_offset);
    if ((buffer_flags & kBufferFlagDataDiscontinuity) != 0) {
        m.flags |= block_flags::discontinuity;
    }
    if ((buffer_flags & kBufferFlagTimestampError) != 0) {
        m.flags |= block_flags::timestamp_error;
    }
    m.silent = (buffer_flags & kBufferFlagSilent) != 0;
    return m;
}

}  // namespace audio_observer::wasapi
