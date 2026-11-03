#include "audio_observer/synthetic_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <thread>

#include "audio_observer/meters.hpp"

namespace audio_observer {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// SplitMix64: small, fast and identical on every platform, unlike the
// standard library's distributions.
std::uint64_t next_random(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Uniform in [-1, 1).
double next_symmetric(std::uint64_t& state) noexcept {
    const double unit = static_cast<double>(next_random(state) >> 11) * 0x1.0p-53;
    return 2.0 * unit - 1.0;
}

std::uint64_t hash_name(const std::string& name) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a
    for (const char c : name) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

EndpointInfo endpoint(const char* name, EndpointDirection direction, bool loopback,
                      std::uint32_t channels) {
    EndpointInfo info;
    info.name = name;
    info.direction = direction;
    info.loopback = loopback;
    info.format = AudioFormat{48000, channels, SampleType::float32};
    return info;
}

class SyntheticStream final : public CaptureStream {
public:
    SyntheticStream(const SyntheticEndpoint& endpoint,
                    StreamDirection direction,
                    const SyntheticConfig& config,
                    std::uint64_t seed,
                    std::shared_ptr<SyntheticBackend::Counters> counters)
        : endpoint_(endpoint),
          pacing_(config.pacing),
          block_frames_(config.block_frames),
          frame_limit_(std::numeric_limits<std::uint64_t>::max()),
          seed_(seed),
          counters_(std::move(counters)) {
        info_.id = "synthetic:" + endpoint.info.name;
        info_.name = endpoint.info.name;
        info_.direction = direction;
        info_.format = endpoint.info.format;
        if (config.stop_after_seconds) {
            frame_limit_ = static_cast<std::uint64_t>(std::llround(
                std::max(*config.stop_after_seconds, 0.0) *
                static_cast<double>(endpoint.info.format.sample_rate_hz)));
        }
        counters_->open.fetch_add(1);
        counters_->total.fetch_add(1);
    }

    ~SyntheticStream() override {
        stop();
        counters_->open.fetch_sub(1);
    }

    const StreamInfo& info() const noexcept override { return info_; }
    std::size_t max_block_frames() const noexcept override { return block_frames_; }

    void start(std::shared_ptr<BlockQueue> queue) override {
        if (!queue) {
            throw BackendError("Stream " + info_.name + " needs a queue");
        }
        if (state_.load() != StreamState::opened) {
            throw BackendError("Stream " + info_.name + " was already started");
        }
        if (queue->channel_count() != info_.format.channel_count ||
            queue->max_block_frames() < block_frames_) {
            throw BackendError("Queue does not match stream " + info_.name);
        }
        state_.store(StreamState::running);
        counters_->running.fetch_add(1);
        try {
            // The thread holds its own reference: the queue lives at least
            // as long as the thread, however the caller orders destruction.
            thread_ = std::thread([this, queue = std::move(queue)] { run(*queue); });
        } catch (const std::exception& e) {
            counters_->running.fetch_sub(1);
            fail(std::string("cannot start thread: ") + e.what());
            throw BackendError("Cannot start stream " + info_.name);
        }
    }

    void stop() noexcept override {
        stop_requested_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        StreamState expected = StreamState::running;
        state_.compare_exchange_strong(expected, StreamState::stopped);
        expected = StreamState::opened;
        state_.compare_exchange_strong(expected, StreamState::stopped);
    }

    StreamState state() const noexcept override { return state_.load(std::memory_order_acquire); }

    std::string error() const override {
        return state() == StreamState::failed ? error_ : std::string();
    }

private:
    void fail(std::string message) {
        error_ = std::move(message);
        state_.store(StreamState::failed, std::memory_order_release);
    }

    // Waits until @p deadline unless a stop is requested first.
    bool wait_until(HostTime deadline) const {
        constexpr auto slice = std::chrono::milliseconds(20);
        while (!stop_requested_.load()) {
            const HostTime now = host_now();
            if (now >= deadline) {
                return true;
            }
            std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(deadline - now, slice));
        }
        return false;
    }

    void run(BlockQueue& queue) noexcept {
        try {
            deliver(queue);
        } catch (const std::exception& e) {
            fail(std::string("synthetic stream error: ") + e.what());
        }
        counters_->running.fetch_sub(1);
    }

    void deliver(BlockQueue& queue) {
        const AudioFormat format = info_.format;
        const std::size_t channels = format.channel_count;
        const double device_rate =
            static_cast<double>(format.sample_rate_hz) * (1.0 + endpoint_.drift_ppm * 1e-6);
        SignalGenerator generator(endpoint_.signal, format.sample_rate_hz, channels, seed_);
        std::uint64_t jitter_rng = seed_ ^ 0x5DEECE66Dull;
        std::vector<float> samples(block_frames_ * channels);
        std::vector<bool> fired(endpoint_.faults.size(), false);

        const HostTime start = host_now();
        std::int64_t position = 0;        // device sample position
        std::uint64_t elapsed_frames = 0;  // device time, including dropped-out frames
        std::uint64_t produced = 0;

        while (!stop_requested_.load()) {
            if (produced >= frame_limit_) {
                state_.store(StreamState::finished, std::memory_order_release);
                return;
            }
            const std::size_t frames = static_cast<std::size_t>(
                std::min<std::uint64_t>(block_frames_, frame_limit_ - produced));

            std::uint32_t flags = 0;
            bool drop = false;
            const std::int64_t planned = position;
            for (std::size_t i = 0; i < endpoint_.faults.size(); ++i) {
                const Fault& fault = endpoint_.faults[i];
                if (fired[i] || planned < fault.at_frame) {
                    continue;
                }
                fired[i] = true;
                switch (fault.kind) {
                    case FaultKind::dropout:
                        position += fault.frames;
                        elapsed_frames += static_cast<std::uint64_t>(std::max<std::int64_t>(fault.frames, 0));
                        break;
                    case FaultKind::overlap:
                        position -= fault.frames;
                        break;
                    case FaultKind::discontinuity:
                        flags |= block_flags::discontinuity;
                        break;
                    case FaultKind::overrun:
                        drop = true;
                        break;
                    case FaultKind::failure:
                        fail("synthetic device failure at frame " + std::to_string(planned));
                        return;
                }
            }

            generator.generate(samples.data(), frames);
            elapsed_frames += frames;
            const auto offset = std::chrono::nanoseconds(static_cast<std::int64_t>(
                std::llround(static_cast<double>(elapsed_frames) * 1e9 / device_rate)));
            const auto jitter = std::chrono::nanoseconds(static_cast<std::int64_t>(
                std::llround(next_symmetric(jitter_rng) * endpoint_.jitter_seconds * 1e9)));

            if (pacing_ == Pacing::real_time) {
                if (!wait_until(start + offset)) {
                    break;
                }
            } else {
                while (!drop && !queue.can_push(frames)) {
                    if (stop_requested_.load()) {
                        break;
                    }
                    std::this_thread::yield();
                }
                if (stop_requested_.load()) {
                    break;
                }
            }

            if (drop) {
                queue.drop(frames);
            } else {
                queue.push(FrameBlock(samples.data(), frames, channels, position,
                                      start + offset + jitter),
                           flags);
            }
            position += static_cast<std::int64_t>(frames);
            produced += frames;
        }
    }

    SyntheticEndpoint endpoint_;
    StreamInfo info_;
    Pacing pacing_;
    std::size_t block_frames_;
    std::uint64_t frame_limit_;
    std::uint64_t seed_;
    std::shared_ptr<SyntheticBackend::Counters> counters_;

    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<StreamState> state_{StreamState::opened};
    std::string error_;  // written before state_ becomes failed
};

}  // namespace

const char* to_string(SignalKind kind) noexcept {
    switch (kind) {
        case SignalKind::sine:
            return "sine";
        case SignalKind::white_noise:
            return "white_noise";
        case SignalKind::silence:
            return "silence";
        case SignalKind::clipped_tone:
            return "clipped_tone";
    }
    return "unknown";
}

const char* to_string(FaultKind kind) noexcept {
    switch (kind) {
        case FaultKind::dropout:
            return "dropout";
        case FaultKind::overlap:
            return "overlap";
        case FaultKind::discontinuity:
            return "discontinuity";
        case FaultKind::overrun:
            return "overrun";
        case FaultKind::failure:
            return "failure";
    }
    return "unknown";
}

std::vector<SyntheticEndpoint> SyntheticConfig::default_endpoints() {
    std::vector<SyntheticEndpoint> endpoints(5);
    endpoints[0].info = endpoint(kSyntheticMicName, EndpointDirection::input, false, 1);
    endpoints[0].signal = {SignalKind::sine, 440.0, -12.0};
    endpoints[1].info = endpoint(kSyntheticOutputName, EndpointDirection::output, true, 2);
    endpoints[1].signal = {SignalKind::sine, 1000.0, -6.0};
    endpoints[2].info = endpoint("Synthetic Noise", EndpointDirection::input, false, 1);
    endpoints[2].signal = {SignalKind::white_noise, 0.0, -20.0};
    endpoints[3].info = endpoint("Synthetic Silence", EndpointDirection::input, false, 2);
    endpoints[3].signal = {SignalKind::silence, 0.0, 0.0};
    endpoints[4].info = endpoint("Synthetic Clipped Tone", EndpointDirection::input, false, 1);
    endpoints[4].signal = {SignalKind::clipped_tone, 440.0, 6.0};
    return endpoints;
}

SignalGenerator::SignalGenerator(SignalConfig signal, std::uint32_t sample_rate_hz,
                                 std::size_t channel_count, std::uint64_t seed) noexcept
    : signal_(signal),
      amplitude_(signal.kind == SignalKind::silence ? 0.0 : dbfs_to_linear(signal.level_dbfs)),
      phase_step_(sample_rate_hz > 0 ? signal.frequency_hz / static_cast<double>(sample_rate_hz) : 0.0),
      channel_count_(channel_count),
      rng_(seed) {}

void SignalGenerator::generate(float* out, std::size_t frame_count) noexcept {
    for (std::size_t f = 0; f < frame_count; ++f) {
        double tone = 0.0;
        if (signal_.kind == SignalKind::sine || signal_.kind == SignalKind::clipped_tone) {
            tone = amplitude_ * std::sin(kTwoPi * phase_);
            phase_ += phase_step_;
            phase_ -= std::floor(phase_);
            if (signal_.kind == SignalKind::clipped_tone) {
                tone = std::clamp(tone, -1.0, 1.0);
            }
        }
        for (std::size_t c = 0; c < channel_count_; ++c) {
            double value = tone;
            if (signal_.kind == SignalKind::white_noise) {
                value = amplitude_ * next_symmetric(rng_);
            }
            out[f * channel_count_ + c] = static_cast<float>(value);
        }
    }
}

SyntheticBackend::SyntheticBackend(SyntheticConfig config)
    : config_(std::move(config)), counters_(std::make_shared<Counters>()) {
    if (config_.block_frames == 0) {
        throw std::invalid_argument("SyntheticBackend block size must be at least one frame");
    }
    for (const SyntheticEndpoint& e : config_.endpoints) {
        if (!e.info.format.valid()) {
            throw std::invalid_argument("Synthetic endpoint " + e.info.name + " has no valid format");
        }
    }
}

SyntheticBackend::~SyntheticBackend() = default;

std::vector<EndpointInfo> SyntheticBackend::endpoints() {
    std::vector<EndpointInfo> list;
    list.reserve(config_.endpoints.size());
    for (const SyntheticEndpoint& e : config_.endpoints) {
        list.push_back(e.info);
    }
    return list;
}

std::unique_ptr<CaptureStream> SyntheticBackend::open(const std::string& endpoint_name,
                                                      StreamDirection direction) {
    const std::vector<EndpointInfo> list = endpoints();
    const EndpointInfo& chosen = select_endpoint(list, endpoint_name, direction);
    const auto index = static_cast<std::size_t>(&chosen - list.data());
    constexpr std::uint64_t golden = 0x9E3779B97F4A7C15ull;
    const auto order = static_cast<std::uint64_t>(counters_->total.load());
    std::uint64_t seed = config_.seed ^ hash_name(endpoint_name) ^ (order * golden);
    seed = next_random(seed);
    return std::make_unique<SyntheticStream>(config_.endpoints[index], direction, config_, seed,
                                             counters_);
}

int SyntheticBackend::open_stream_count() const noexcept {
    return counters_->open.load();
}

int SyntheticBackend::running_stream_count() const noexcept {
    return counters_->running.load();
}

int SyntheticBackend::total_opened() const noexcept {
    return counters_->total.load();
}

}  // namespace audio_observer
