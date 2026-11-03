#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include "audio_observer/session.hpp"
#include "audio_observer/synthetic_backend.hpp"

using namespace audio_observer;

namespace {

SyntheticConfig fast_config(double seconds) {
    SyntheticConfig config;
    config.pacing = Pacing::fast;
    config.stop_after_seconds = seconds;
    return config;
}

SessionConfig two_streams(double seconds) {
    SessionConfig config;
    config.seconds = seconds;
    config.streams = {{"mic", kSyntheticMicName, StreamDirection::capture},
                      {"system", kSyntheticOutputName, StreamDirection::loopback}};
    return config;
}

}  // namespace

TEST_CASE("session enforces its duration bounds and stream list") {
    SyntheticBackend backend;
    for (const double bad : {0.0, 0.99, 60.01, 3600.0, -1.0, std::nan("")}) {
        CHECK_THROWS_AS(Session(backend, two_streams(bad)), std::invalid_argument);
    }
    CHECK_NOTHROW(Session(backend, two_streams(1.0)));
    CHECK_NOTHROW(Session(backend, two_streams(60.0)));

    SessionConfig empty = two_streams(1.0);
    empty.streams.clear();
    CHECK_THROWS_AS(Session(backend, empty), std::invalid_argument);

    SessionConfig repeated = two_streams(1.0);
    repeated.streams[1].label = "mic";
    CHECK_THROWS_AS(Session(backend, repeated), std::invalid_argument);

    SessionConfig unbounded = two_streams(1.0);
    unbounded.max_seconds = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(Session(backend, unbounded), std::invalid_argument);
    CHECK(backend.total_opened() == 0);
}

TEST_CASE("session meters every stream and releases them on completion") {
    SyntheticBackend backend(fast_config(2.0));
    SessionConfig config = two_streams(2.0);
    config.streams.push_back({"mic_second_client", kSyntheticMicName, StreamDirection::capture});
    const SessionSummary summary = Session(backend, config).run();

    CHECK(summary.state == SessionState::completed);
    CHECK(summary.error.empty());
    REQUIRE(summary.streams.size() == 3);
    for (const StreamSummary& s : summary.streams) {
        CHECK(s.frames == 96000);
        CHECK(s.blocks == 200);
        CHECK(s.final_state == StreamState::finished);  // the synthetic source ended
        CHECK(s.error.empty());
        CHECK(s.flagged_blocks == 0);
    }
    const StreamSummary& mic = summary.streams[0];
    CHECK(mic.label == "mic");
    CHECK(mic.info.name == kSyntheticMicName);
    CHECK(mic.channels.size() == 1);
    CHECK(mic.levels.peak_dbfs == doctest::Approx(-12.0).epsilon(0.001));
    const StreamSummary& system = summary.streams[1];
    CHECK(system.info.direction == StreamDirection::loopback);
    CHECK(system.channels.size() == 2);
    CHECK(system.channels[1].peak_dbfs == doctest::Approx(-6.0).epsilon(0.001));

    CHECK(backend.total_opened() == 3);
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.running_stream_count() == 0);
}

TEST_CASE("a stream failing mid-session stops the session and releases every stream") {
    SyntheticConfig synthetic = fast_config(30.0);
    synthetic.endpoints[0].faults = {{FaultKind::failure, 24000, 0}};  // mic fails at 0.5 s
    SyntheticBackend backend(synthetic);
    const SessionSummary summary = Session(backend, two_streams(30.0)).run();

    CHECK(summary.state == SessionState::failed);
    CHECK(summary.error == "Stream mic failed: synthetic device failure at frame 24000");
    REQUIRE(summary.streams.size() == 2);
    CHECK(summary.streams[0].final_state == StreamState::failed);
    CHECK(summary.streams[0].frames == 24000);
    CHECK(summary.streams[1].error.empty());
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.running_stream_count() == 0);
}

TEST_CASE("a stream that cannot be opened fails the session and releases the others") {
    SyntheticBackend backend(fast_config(1.0));
    SessionConfig config = two_streams(1.0);
    config.streams[1].endpoint_name = "Synthetic Headset (2)";
    const SessionSummary summary = Session(backend, config).run();

    CHECK(summary.state == SessionState::failed);
    CHECK(summary.error ==
          "Expected one exact loopback endpoint for Synthetic Headset (2); got 0");
    CHECK(summary.streams.size() == 1);
    CHECK(backend.total_opened() == 1);
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.running_stream_count() == 0);
}

TEST_CASE("a stop request ends a real-time session early and releases every stream") {
    SyntheticBackend backend;  // real-time pacing, unlimited streams
    std::atomic<bool> stop{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop.store(true);
    });
    const SessionSummary summary = Session(backend, two_streams(60.0)).run(&stop);
    stopper.join();

    CHECK(summary.state == SessionState::stopped);
    CHECK(summary.elapsed_seconds < 10.0);
    REQUIRE(summary.streams.size() == 2);
    CHECK(summary.streams[0].frames > 0);
    CHECK(summary.streams[0].final_state == StreamState::stopped);
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.running_stream_count() == 0);
}

TEST_CASE("a real-time session runs for the requested time") {
    SyntheticBackend backend;
    const SessionSummary summary = Session(backend, two_streams(1.0)).run();
    CHECK(summary.state == SessionState::completed);
    CHECK(summary.elapsed_seconds >= 1.0 - 0.011);  // the last block is due at 1 s
    CHECK(summary.elapsed_seconds < 3.0);
    for (const StreamSummary& s : summary.streams) {
        // Exactly one second of frames, however coarse the scheduler is.
        CHECK(s.frames == 48000);
        CHECK(s.blocks == 100);
        CHECK(s.final_state == StreamState::stopped);
    }
    CHECK(backend.open_stream_count() == 0);
}

TEST_CASE("a session truncates the block that crosses its frame count") {
    SyntheticConfig synthetic = fast_config(10.0);
    synthetic.block_frames = 700;  // 48000 is not a multiple of 700
    SyntheticBackend backend(synthetic);
    const SessionSummary summary = Session(backend, two_streams(1.5)).run();
    CHECK(summary.state == SessionState::completed);
    REQUIRE(summary.streams.size() == 2);
    for (const StreamSummary& s : summary.streams) {
        CHECK(s.frames == 72000);
        CHECK(s.blocks == 103);  // 102 whole blocks and 600 frames of the next
        CHECK(s.dropouts.gaps == 0);
        CHECK(s.dropouts.overruns == 0);
    }
    CHECK(backend.open_stream_count() == 0);
    CHECK(backend.running_stream_count() == 0);
}

TEST_CASE("the wall-clock safety limit ends a session whose stream delivers too slowly") {
    SyntheticConfig synthetic;  // real time
    synthetic.endpoints[0].drift_ppm = -500000.0;  // mic delivers at half speed
    SyntheticBackend backend(synthetic);
    SessionConfig config = two_streams(1.0);
    config.safety_margin_seconds = 0.2;
    const SessionSummary summary = Session(backend, config).run();
    CHECK(summary.state == SessionState::completed);
    CHECK(summary.elapsed_seconds >= 1.2);
    CHECK(summary.elapsed_seconds < 3.0);
    REQUIRE(summary.streams.size() == 2);
    CHECK(summary.streams[0].frames < 48000);
    CHECK(summary.streams[1].frames == 48000);
    CHECK(backend.running_stream_count() == 0);

    SessionConfig bad = two_streams(1.0);
    bad.safety_margin_seconds = -1.0;
    CHECK_THROWS_AS(Session(backend, bad), std::invalid_argument);
}
