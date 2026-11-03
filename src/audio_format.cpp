#include "audio_observer/audio_format.hpp"

namespace audio_observer {

std::size_t bytes_per_sample(SampleType type) noexcept {
    switch (type) {
        case SampleType::int16:
            return 2;
        case SampleType::int24:
            return 3;
        case SampleType::float32:
        case SampleType::int32:
            return 4;
    }
    return 0;
}

bool operator==(const AudioFormat& a, const AudioFormat& b) noexcept {
    return a.sample_rate_hz == b.sample_rate_hz && a.channel_count == b.channel_count &&
           a.sample_type == b.sample_type;
}

bool operator!=(const AudioFormat& a, const AudioFormat& b) noexcept {
    return !(a == b);
}

}  // namespace audio_observer
