#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "audio_observer/meters.hpp"
#include "audio_observer/synthetic_backend.hpp"

using namespace audio_observer;

namespace {

struct Received {
    std::int64_t first_index;
    std::size_t frames;
    std::int64_t host_ns;
    std::uint32_t flags;
    std::uint64_t dropped_frames_before;
};

struct Capture {
    std::vector<Received> blocks;
    std::vector<float> first_samples;  // first sample of every block
    LevelStats levels;
    StreamState final_state = StreamState::opened;
    std::string error;
};

// Runs a fast-paced stream to its end and records what arrived.
Capture capture_all(CaptureStream& stream) {
    auto queue_owner = std::make_shared<BlockQueue>(stream.info().format.channel_count, 4096, 64,
                                                    stream.max_block_frames());
    BlockQueue& queue = *queue_owner;
    Capture result;
    stream.start(queue_owner);
    DeliveredBlock out;
    for (;;) {
        const bool done = stream.state() != StreamState::running;
        bool any = false;
        while (queue.pop(out)) {
            any = true;
            result.blocks.push_back({out.block.first_sample_index(), out.block.frame_count(),
                                     out.block.host_time().time_since_epoch().count(), out.flags,
                                     out.dropped_frames_before});
            result.first_samples.push_back(out.block.data()[0]);
            for (std::size_t i = 0; i < out.block.sample_count(); ++i) {
                result.levels.add(out.block.data()[i]);
            }
        }
        if (done && !any) {
            break;
        }
        std::this_thread::yield();
    }
    stream.stop();
    result.final_state = stream.state();
    result.error = stream.error();
    return result;
}

SyntheticConfig fast_config(double seconds) {
    SyntheticConfig config;
    config.pacing = Pacing::fast;
    config.stop_after_seconds = seconds;
    return config;
}

SyntheticEndpoint& endpoint_named(SyntheticConfig& config, const std::string& name) {
    for (SyntheticEndpoint& e : config.endpoints) {
        if (e.info.name == name) {
            return e;
        }
    }
    FAIL("no endpoint " << name);
    return config.endpoints.front();
}

}  // namespace

TEST_CASE("endpoint selection matches exactly and fails closed") {
    std::vector<EndpointInfo> list(4);
    list[0] = {"Mic", EndpointDirection::input, false, {48000, 1}};
    list[1] = {"Headset", EndpointDirection::output, true, {48000, 2}};
    list[2] = {"Twin", EndpointDirection::input, false, {48000, 1}};
    list[3] = {"Twin", EndpointDirection::input, false, {44100, 1}};

    CHECK(&select_endpoint(list, "Mic", StreamDirection::capture) == &list[0]);
    CHECK(&select_endpoint(list, "Headset", StreamDirection::loopback) == &list[1]);

    // Direction matters: a microphone has no loopback, an output is not an input.
    CHECK_THROWS_WITH_AS(select_endpoint(list, "Mic", StreamDirection::loopback),
                         "Expected one exact loopback endpoint for Mic; got 0", BackendError);
    CHECK_THROWS_AS(select_endpoint(list, "Headset", StreamDirection::capture), BackendError);
    // No substring or case-insensitive matches.
    CHECK_THROWS_AS(select_endpoint(list, "mic", StreamDirection::capture), BackendError);
    CHECK_THROWS_AS(select_endpoint(list, "Mi", StreamDirection::capture), BackendError);
    // Ambiguity is an error naming the count.
    CHECK_THROWS_WITH_AS(select_endpoint(list, "Twin", StreamDirection::capture),
                         "Expected one exact capture endpoint for Twin; got 2", BackendError);
}

TEST_CASE("synthetic backend lists its default endpoints without opening streams") {
    SyntheticBackend backend;
    const std::vector<EndpointInfo> list = backend.endpoints();
    REQUIRE(list.size() == 5);
    CHECK(list[0].name == kSyntheticMicName);
    CHECK(list[0].direction == EndpointDirection::input);
    CHECK(list[1].name == kSyntheticOutputName);
    CHECK(list[1].direction == EndpointDirection::output);
    CHECK(list[1].loopback);
    CHECK(list[1].format.channel_count == 2);
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.total_opened() == 0);

    CHECK_THROWS_AS(backend.open("Nothing", StreamDirection::capture), BackendError);
    CHECK(backend.open_stream_count() == 0);
}

TEST_CASE("synthetic streams are counted until released") {
    SyntheticBackend backend(fast_config(0.1));
    {
        auto a = backend.open(kSyntheticMicName, StreamDirection::capture);
        auto b = backend.open(kSyntheticMicName, StreamDirection::capture);  // shared mode
        auto c = backend.open(kSyntheticOutputName, StreamDirection::loopback);
        CHECK(backend.open_stream_count() == 3);
        CHECK(a->info().direction == StreamDirection::capture);
        CHECK(c->info().direction == StreamDirection::loopback);
        auto queue = std::make_shared<BlockQueue>(1, 100000, 1000, a->max_block_frames());
        a->start(queue);
        CHECK_THROWS_AS(a->start(queue), BackendError);
        CHECK_THROWS_AS(b->start(nullptr), BackendError);
        // `queue` goes out of scope before `a`; the running stream keeps the
        // queue alive until destroying it has stopped and joined its thread.
    }
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.running_stream_count() == 0);
    CHECK(backend.total_opened() == 3);
}

TEST_CASE("synthetic stream delivers contiguous blocks and finishes") {
    SyntheticBackend backend(fast_config(1.0));
    auto stream = backend.open(kSyntheticMicName, StreamDirection::capture);
    const Capture capture = capture_all(*stream);
    CHECK(capture.final_state == StreamState::finished);
    REQUIRE(capture.blocks.size() == 100);
    std::int64_t expected = 0;
    for (const Received& r : capture.blocks) {
        CHECK(r.first_index == expected);
        CHECK(r.flags == 0);
        expected += static_cast<std::int64_t>(r.frames);
    }
    CHECK(expected == 48000);
    // -12 dBFS sine: peak -12, RMS about -15.01.
    CHECK(capture.levels.peak_dbfs() == doctest::Approx(-12.0).epsilon(0.001));
    CHECK(capture.levels.rms_dbfs() == doctest::Approx(-15.01).epsilon(0.001));
}

TEST_CASE("synthetic signals have the configured character") {
    SyntheticBackend backend(fast_config(0.5));
    SUBCASE("silence") {
        auto stream = backend.open("Synthetic Silence", StreamDirection::capture);
        const Capture capture = capture_all(*stream);
        CHECK(capture.levels.peak == 0.0f);
    }
    SUBCASE("noise") {
        auto stream = backend.open("Synthetic Noise", StreamDirection::capture);
        const Capture capture = capture_all(*stream);
        // Uniform noise of peak -20 dBFS has RMS -20 - 4.77 dBFS.
        CHECK(capture.levels.peak_dbfs() <= -20.0);
        CHECK(capture.levels.peak_dbfs() > -20.1);
        CHECK(capture.levels.rms_dbfs() == doctest::Approx(-24.77).epsilon(0.002));
    }
    SUBCASE("clipped tone") {
        auto stream = backend.open("Synthetic Clipped Tone", StreamDirection::capture);
        const Capture capture = capture_all(*stream);
        CHECK(capture.levels.peak == 1.0f);
        // A +6 dBFS sine (amplitude 2) is at or beyond full scale wherever
        // |sin| >= 0.5, which is two thirds of the time.
        const double fraction = static_cast<double>(capture.levels.clipped_samples) /
                                static_cast<double>(capture.levels.sample_count);
        CHECK(fraction == doctest::Approx(2.0 / 3.0).epsilon(0.02));
    }
}

TEST_CASE("synthetic streams are deterministic from the seed") {
    auto noise_of = [](std::uint64_t seed) {
        SyntheticConfig config = fast_config(0.2);
        config.seed = seed;
        endpoint_named(config, "Synthetic Noise").jitter_seconds = 0.001;
        SyntheticBackend backend(config);
        auto stream = backend.open("Synthetic Noise", StreamDirection::capture);
        const Capture capture = capture_all(*stream);
        std::vector<double> fingerprint(capture.first_samples.begin(), capture.first_samples.end());
        // Timestamps relative to the first block, which removes the start time.
        for (const Received& r : capture.blocks) {
            fingerprint.push_back(static_cast<double>(r.host_ns - capture.blocks[0].host_ns));
        }
        return fingerprint;
    };
    CHECK(noise_of(7) == noise_of(7));
    CHECK(noise_of(7) != noise_of(8));
}

TEST_CASE("synthetic faults land at the configured positions") {
    SyntheticConfig config = fast_config(0.5);  // 50 blocks of 480 frames
    endpoint_named(config, kSyntheticMicName).faults = {
        {FaultKind::dropout, 4800, 960},       // block 10 starts at 5760
        {FaultKind::discontinuity, 9600, 0},   // first block at or after 9600
        {FaultKind::overlap, 14400, 240},      // block starts 240 early
        {FaultKind::overrun, 19200, 0},        // block dropped
    };
    SyntheticBackend backend(config);
    auto stream = backend.open(kSyntheticMicName, StreamDirection::capture);
    const Capture capture = capture_all(*stream);
    CHECK(capture.final_state == StreamState::finished);
    // One block was dropped as an overrun.
    REQUIRE(capture.blocks.size() == 49);

    std::vector<std::int64_t> starts;
    std::vector<std::size_t> flagged;
    for (std::size_t i = 0; i < capture.blocks.size(); ++i) {
        starts.push_back(capture.blocks[i].first_index);
        if (capture.blocks[i].flags & block_flags::discontinuity) {
            flagged.push_back(i);
        }
    }
    CHECK(starts[9] == 4320);
    CHECK(starts[10] == 5760);  // gap of 960 frames
    // After the dropout, blocks start at 5760 + 480k; 9600 is passed at 9600.
    CHECK(starts[18] == 9600);
    REQUIRE(flagged.size() == 1);
    CHECK(flagged[0] == 18);
    CHECK(starts[28] == 14400 - 240);  // overlap
    // The overrun hit the first block planned at or after 19200, which was
    // planned at 19440; the next delivered block reports it.
    CHECK(starts[38] == 18960);
    CHECK(starts[39] == 19920);
    CHECK(capture.blocks[38].dropped_frames_before == 0);
    CHECK(capture.blocks[39].dropped_frames_before == 480);
}

TEST_CASE("synthetic device failure stops delivery with an error") {
    SyntheticConfig config = fast_config(1.0);
    endpoint_named(config, kSyntheticMicName).faults = {{FaultKind::failure, 9600, 0}};
    SyntheticBackend backend(config);
    auto stream = backend.open(kSyntheticMicName, StreamDirection::capture);
    const Capture capture = capture_all(*stream);
    CHECK(capture.final_state == StreamState::failed);
    CHECK(capture.error == "synthetic device failure at frame 9600");
    CHECK(capture.blocks.size() == 20);
}

TEST_CASE("synthetic clock drift changes the rate against the host clock") {
    SyntheticConfig config = fast_config(10.0);
    endpoint_named(config, kSyntheticMicName).drift_ppm = 250.0;
    SyntheticBackend backend(config);
    auto stream = backend.open(kSyntheticMicName, StreamDirection::capture);
    const Capture capture = capture_all(*stream);
    const Received& first = capture.blocks.front();
    const Received& last = capture.blocks.back();
    const double seconds = static_cast<double>(last.host_ns - first.host_ns) * 1e-9;
    const double rate = static_cast<double>(last.first_index - first.first_index) / seconds;
    CHECK(rate == doctest::Approx(48000.0 * (1.0 + 250e-6)).epsilon(1e-7));
}

TEST_CASE("synthetic jitter stays within its bound") {
    SyntheticConfig config = fast_config(1.0);
    endpoint_named(config, kSyntheticMicName).jitter_seconds = 0.002;
    SyntheticBackend backend(config);
    auto stream = backend.open(kSyntheticMicName, StreamDirection::capture);
    const Capture capture = capture_all(*stream);
    // Ideal spacing is 10 ms; jitter of at most 2 ms on each end.
    double largest = 0.0;
    for (std::size_t i = 1; i < capture.blocks.size(); ++i) {
        const double step =
            static_cast<double>(capture.blocks[i].host_ns - capture.blocks[i - 1].host_ns) * 1e-9;
        CHECK(step >= 0.006 - 1e-9);
        CHECK(step <= 0.014 + 1e-9);
        largest = std::max(largest, std::fabs(step - 0.01));
    }
    CHECK(largest > 0.001);  // jitter is actually applied
}

TEST_CASE("real-time synthetic streams follow the host clock and stop on request") {
    SyntheticBackend backend;  // real-time, unlimited
    auto stream = backend.open(kSyntheticOutputName, StreamDirection::loopback);
    auto queue_owner = std::make_shared<BlockQueue>(2, 48000, 200, stream->max_block_frames());
    BlockQueue& queue = *queue_owner;
    const HostTime begin = host_now();
    stream->start(queue_owner);
    CHECK(backend.running_stream_count() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stream->stop();
    const double elapsed = std::chrono::duration<double>(host_now() - begin).count();
    CHECK(stream->state() == StreamState::stopped);
    CHECK(backend.running_stream_count() == 0);

    std::size_t frames = 0;
    DeliveredBlock out;
    while (queue.pop(out)) {
        frames += out.block.frame_count();
    }
    // No more frames than the elapsed time allows, and not far fewer.
    CHECK(static_cast<double>(frames) <= elapsed * 48000.0 + 480.0);
    CHECK(frames >= 4800);
}

TEST_CASE("a running stream keeps its queue alive when the caller releases it first") {
    SUBCASE("real-time stream, caller's queue reference dropped while running") {
        SyntheticBackend backend;  // real-time, unlimited
        auto stream = backend.open(kSyntheticOutputName, StreamDirection::loopback);
        {
            auto queue = std::make_shared<BlockQueue>(2, 4800, 20, stream->max_block_frames());
            stream->start(queue);
        }  // the only caller reference is gone; the stream keeps writing
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(stream->state() == StreamState::running);
        stream.reset();  // stops, joins, then releases the last reference
        CHECK(backend.running_stream_count() == 0);
        CHECK(backend.open_stream_count() == 0);
    }
    SUBCASE("fast stream filling its queue, destroyed with the queue released first") {
        SyntheticBackend backend(fast_config(60.0));
        std::unique_ptr<CaptureStream> stream =
            backend.open(kSyntheticMicName, StreamDirection::capture);
        auto queue = std::make_shared<BlockQueue>(1, 4096, 64, stream->max_block_frames());
        stream->start(queue);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        queue.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stream.reset();
        CHECK(backend.running_stream_count() == 0);
    }
    SUBCASE("backend destroyed before a running stream") {
        auto backend = std::make_unique<SyntheticBackend>();
        auto stream = backend->open(kSyntheticMicName, StreamDirection::capture);
        stream->start(std::make_shared<BlockQueue>(1, 4800, 20, stream->max_block_frames()));
        backend.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stream.reset();
    }
}
