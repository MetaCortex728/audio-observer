// The WASAPI backend's format conversion and packet mapping, tested with fake
// inputs on every platform; no device is involved.

#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "audio_observer/block_queue.hpp"
#include "audio_observer/wasapi_format.hpp"

using namespace audio_observer;
using namespace audio_observer::wasapi;

namespace {

WaveFormatFields plain(std::uint16_t tag, std::uint16_t channels, std::uint16_t bits) {
    WaveFormatFields f;
    f.format_tag = tag;
    f.channels = channels;
    f.samples_per_sec = 48000;
    f.bits_per_sample = bits;
    f.block_align = static_cast<std::uint16_t>(channels * bits / 8);
    return f;
}

WaveFormatFields extensible(SubFormat sub, std::uint16_t channels, std::uint16_t container_bits,
                            std::uint16_t valid_bits) {
    WaveFormatFields f = plain(kFormatTagExtensible, channels, container_bits);
    f.sub_format = sub;
    f.valid_bits_per_sample = valid_bits;
    return f;
}

template <typename T>
std::vector<unsigned char> bytes_of(const std::vector<T>& values) {
    std::vector<unsigned char> out(values.size() * sizeof(T));
    std::memcpy(out.data(), values.data(), out.size());
    return out;
}

std::vector<float> convert(const std::vector<unsigned char>& bytes, const SampleLayout& layout,
                           std::size_t frames, std::size_t offset = 0) {
    // Copy to an odd offset so the conversion cannot rely on alignment.
    std::vector<unsigned char> shifted(offset + bytes.size());
    std::memcpy(shifted.data() + offset, bytes.data(), bytes.size());
    std::vector<float> out(frames * layout.format.channel_count, 99.0f);
    convert_to_float(shifted.data() + offset, frames, layout, out.data());
    return out;
}

}  // namespace

TEST_CASE("the usual WASAPI mix format, extensible 32-bit float, is read as float32") {
    std::string why;
    const auto layout = describe_mix_format(extensible(SubFormat::ieee_float, 8, 32, 32), &why);
    REQUIRE(layout);
    CHECK(why.empty());
    CHECK(layout->format.sample_rate_hz == 48000);
    CHECK(layout->format.channel_count == 8);
    CHECK(layout->format.sample_type == SampleType::float32);
    CHECK(layout->container == Container::float32);
    CHECK(layout->bytes_per_frame == 32);
}

TEST_CASE("PCM mix formats map to their sample types and containers") {
    struct Case {
        WaveFormatFields fields;
        SampleType type;
        Container container;
        std::size_t frame_bytes;
    };
    const Case cases[] = {
        {plain(kFormatTagIeeeFloat, 2, 32), SampleType::float32, Container::float32, 8},
        {plain(kFormatTagPcm, 2, 16), SampleType::int16, Container::int16, 4},
        {plain(kFormatTagPcm, 1, 24), SampleType::int24, Container::int24, 3},
        {plain(kFormatTagPcm, 2, 32), SampleType::int32, Container::int32, 8},
        {extensible(SubFormat::pcm, 2, 16, 16), SampleType::int16, Container::int16, 4},
        {extensible(SubFormat::pcm, 2, 24, 24), SampleType::int24, Container::int24, 6},
        {extensible(SubFormat::pcm, 2, 32, 24), SampleType::int24, Container::int32, 8},
        {extensible(SubFormat::pcm, 2, 32, 32), SampleType::int32, Container::int32, 8},
        // An extensible format without a valid-bit count uses the container size.
        {extensible(SubFormat::pcm, 1, 16, 0), SampleType::int16, Container::int16, 2},
    };
    for (const Case& c : cases) {
        CAPTURE(c.fields.format_tag);
        CAPTURE(c.fields.bits_per_sample);
        CAPTURE(c.fields.valid_bits_per_sample);
        const auto layout = describe_mix_format(c.fields);
        REQUIRE(layout);
        CHECK(layout->format.sample_type == c.type);
        CHECK(layout->container == c.container);
        CHECK(layout->bytes_per_frame == c.frame_bytes);
    }
}

TEST_CASE("unsupported or inconsistent mix formats are refused with a reason") {
    std::vector<WaveFormatFields> bad = {
        plain(kFormatTagPcm, 2, 8),                        // 8-bit
        plain(kFormatTagIeeeFloat, 2, 64),                 // 64-bit float
        plain(0x0055, 2, 16),                              // MP3 tag
        extensible(SubFormat::other, 2, 32, 32),           // unknown sub-format
        extensible(SubFormat::none, 2, 32, 32),            // truncated extensible
        extensible(SubFormat::pcm, 2, 16, 24),             // more valid bits than container
        extensible(SubFormat::ieee_float, 2, 32, 24),      // float with padding
        plain(kFormatTagPcm, 0, 16),                       // no channels
    };
    WaveFormatFields misaligned = plain(kFormatTagPcm, 2, 16);
    misaligned.block_align = 6;
    bad.push_back(misaligned);
    WaveFormatFields no_rate = plain(kFormatTagIeeeFloat, 2, 32);
    no_rate.samples_per_sec = 0;
    bad.push_back(no_rate);
    for (const WaveFormatFields& f : bad) {
        CAPTURE(f.format_tag);
        CAPTURE(f.bits_per_sample);
        std::string why;
        CHECK_FALSE(describe_mix_format(f, &why));
        CHECK_FALSE(why.empty());
    }
}

TEST_CASE("float32 samples are copied unchanged") {
    const auto layout = describe_mix_format(plain(kFormatTagIeeeFloat, 2, 32));
    REQUIRE(layout);
    const std::vector<float> in = {0.0f, -1.0f, 0.5f, 1.0f, -0.25f, 1.5f};
    CHECK(convert(bytes_of(in), *layout, 3, 1) == in);
}

TEST_CASE("int16 samples are scaled by 2^15") {
    const auto layout = describe_mix_format(plain(kFormatTagPcm, 2, 16));
    REQUIRE(layout);
    const std::vector<std::int16_t> in = {0, -32768, 16384, 32767, -1, 1};
    const std::vector<float> out = convert(bytes_of(in), *layout, 3, 1);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == -1.0f);
    CHECK(out[2] == 0.5f);
    CHECK(out[3] == doctest::Approx(32767.0 / 32768.0));
    CHECK(out[4] == doctest::Approx(-1.0 / 32768.0));
    CHECK(out[5] == doctest::Approx(1.0 / 32768.0));
}

TEST_CASE("packed int24 samples are sign-extended and scaled by 2^23") {
    const auto layout = describe_mix_format(plain(kFormatTagPcm, 1, 24));
    REQUIRE(layout);
    // Little-endian 3-byte samples.
    const std::vector<unsigned char> in = {
        0x00, 0x00, 0x00,  // 0
        0x00, 0x00, 0x80,  // -8388608
        0x00, 0x00, 0x40,  // 4194304
        0xFF, 0xFF, 0x7F,  // 8388607
        0xFF, 0xFF, 0xFF,  // -1
        0x00, 0x00, 0xC0,  // -4194304
    };
    const std::vector<float> out = convert(in, *layout, 6, 3);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == -1.0f);
    CHECK(out[2] == 0.5f);
    CHECK(out[3] == doctest::Approx(8388607.0 / 8388608.0));
    CHECK(out[4] == doctest::Approx(-1.0 / 8388608.0));
    CHECK(out[5] == -0.5f);
}

TEST_CASE("int32 samples, including 24 valid bits in 32, are scaled by 2^31") {
    for (const std::uint16_t valid : {std::uint16_t{32}, std::uint16_t{24}}) {
        CAPTURE(valid);
        const auto layout = describe_mix_format(extensible(SubFormat::pcm, 2, 32, valid));
        REQUIRE(layout);
        // 24-in-32 samples are left-justified: the low byte is padding.
        const std::vector<std::int32_t> in = {0, std::numeric_limits<std::int32_t>::min(),
                                              0x40000000, 0x7FFFFF00, -256, 0x20000000};
        const std::vector<float> out = convert(bytes_of(in), *layout, 3, 2);
        CHECK(out[0] == 0.0f);
        CHECK(out[1] == -1.0f);
        CHECK(out[2] == 0.5f);
        CHECK(out[3] == doctest::Approx(1.0).epsilon(1e-6));
        CHECK(out[4] == doctest::Approx(-1.0 / 8388608.0));
        CHECK(out[5] == 0.25f);
    }
}

TEST_CASE("conversion writes exactly frames x channels samples") {
    const auto layout = describe_mix_format(plain(kFormatTagPcm, 2, 16));
    REQUIRE(layout);
    const std::vector<std::int16_t> in = {100, 200, 300, 400};
    std::vector<float> out(6, 99.0f);
    convert_to_float(in.data(), 2, *layout, out.data());
    CHECK(out[3] == doctest::Approx(400.0 / 32768.0));
    CHECK(out[4] == 99.0f);
    CHECK(out[5] == 99.0f);
    convert_to_float(in.data(), 0, *layout, out.data());  // nothing to do
}

TEST_CASE("packet mapping uses the device position as the sample index") {
    const PacketMapping m = map_packet(123456, 0, 0, std::chrono::nanoseconds(0));
    CHECK(m.first_sample_index == 123456);
    CHECK(m.flags == 0);
    CHECK_FALSE(m.silent);
    // Beyond the int64 range it is clamped, never negative.
    CHECK(map_packet(std::numeric_limits<std::uint64_t>::max(), 0, 0, {}).first_sample_index ==
          std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("packet mapping moves the QPC time onto the host clock") {
    // 2.5 s on the QPC clock, in 100 ns units, with the host clock 7 s ahead.
    const PacketMapping m = map_packet(0, 25'000'000, 0, std::chrono::seconds(7));
    CHECK(m.host_time.time_since_epoch() == std::chrono::milliseconds(9500));
    const PacketMapping behind = map_packet(0, 25'000'000, 0, std::chrono::seconds(-1));
    CHECK(behind.host_time.time_since_epoch() == std::chrono::milliseconds(1500));
}

TEST_CASE("buffer flags map to block flags; silence is not a fault") {
    const auto flags_of = [](std::uint32_t buffer_flags) {
        return map_packet(0, 0, buffer_flags, {});
    };
    CHECK(flags_of(kBufferFlagDataDiscontinuity).flags == block_flags::discontinuity);
    CHECK(flags_of(kBufferFlagTimestampError).flags == block_flags::timestamp_error);
    CHECK(flags_of(kBufferFlagSilent).flags == 0);
    CHECK(flags_of(kBufferFlagSilent).silent);
    const PacketMapping all =
        flags_of(kBufferFlagDataDiscontinuity | kBufferFlagSilent | kBufferFlagTimestampError);
    CHECK(all.flags == (block_flags::discontinuity | block_flags::timestamp_error));
    CHECK(all.silent);
    CHECK(flags_of(0x80u).flags == 0);  // unknown bits are ignored
    CHECK_FALSE(flags_of(0x80u).silent);
}

TEST_CASE("QPC ticks convert to 100 ns units without overflow") {
    CHECK(qpc_ticks_to_hns(10'000'000, 10'000'000) == 10'000'000);  // 1 s at 10 MHz
    CHECK(qpc_ticks_to_hns(3'000'000, 3'000'000) == 10'000'000);    // 1 s at 3 MHz
    CHECK(qpc_ticks_to_hns(1'500'000, 3'000'000) == 5'000'000);
    CHECK(qpc_ticks_to_hns(1, 3'000'000) == 3);  // rounds down
    // A year of uptime at 3 GHz would overflow ticks x 10^7.
    const std::uint64_t year_ticks = 3'000'000'000ull * 31'536'000ull;
    CHECK(qpc_ticks_to_hns(year_ticks, 3'000'000'000ull) == 31'536'000ull * 10'000'000ull);
}
