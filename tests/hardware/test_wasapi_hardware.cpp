// Opt-in smoke test of the WASAPI backend on real audio hardware.
//
// It opens the microphone, so it never runs unless asked: CTest label
// "hardware" (excluded by the test presets), and the executable itself skips
// (exit code 77) unless AUDIO_OBSERVER_HARDWARE=1. Name the endpoints with
// AUDIO_OBSERVER_TEST_MIC and, optionally, AUDIO_OBSERVER_TEST_OUTPUT (exact
// names from `audio-observer inspect`). Only scalar figures are printed.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "audio_observer/session.hpp"
#include "audio_observer/summary_json.hpp"
#include "audio_observer/wasapi_backend.hpp"

using namespace audio_observer;

namespace {

constexpr int kSkip = 77;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

}  // namespace

TEST_CASE("wasapi lists endpoints without opening streams") {
    const std::unique_ptr<Backend> backend = make_wasapi_backend();
    CHECK(backend->name() == "wasapi");
    const std::vector<EndpointInfo> endpoints = backend->endpoints();
    for (const EndpointInfo& e : endpoints) {
        CAPTURE(e.name);
        CHECK_FALSE(e.name.empty());
        CHECK(e.loopback == (e.direction == EndpointDirection::output));
    }
    MESSAGE("active endpoints: " << endpoints.size());
}

TEST_CASE("wasapi endpoint matching fails closed") {
    const std::unique_ptr<Backend> backend = make_wasapi_backend();
    CHECK_THROWS_AS(backend->open("audio-observer: no endpoint has this name",
                                  StreamDirection::capture),
                    BackendError);
    const std::string mic = env("AUDIO_OBSERVER_TEST_MIC");
    if (!mic.empty()) {
        CHECK_THROWS_AS(backend->open(mic + " ", StreamDirection::capture), BackendError);
        // A microphone is not a loopback source.
        CHECK_THROWS_AS(backend->open(mic, StreamDirection::loopback), BackendError);
    }
}

TEST_CASE("wasapi meters the named endpoints for two seconds and releases them") {
    const std::string mic = env("AUDIO_OBSERVER_TEST_MIC");
    const std::string output = env("AUDIO_OBSERVER_TEST_OUTPUT");
    if (mic.empty()) {
        MESSAGE("AUDIO_OBSERVER_TEST_MIC is not set; no stream opened");
        return;
    }
    const std::unique_ptr<Backend> backend = make_wasapi_backend();
    SessionConfig config;
    config.seconds = 2.0;
    config.streams = {{"mic", mic, StreamDirection::capture}};
    if (!output.empty()) {
        config.streams.push_back({"system", output, StreamDirection::loopback});
    }
    const SessionSummary summary = Session(*backend, config).run();
    SummaryContext context;
    context.backend = backend->name();
    write_summary_json(std::cout, context, summary);
    std::cout << '\n';

    CHECK(summary.error.empty());
    REQUIRE(!summary.streams.empty());
    const StreamSummary& m = summary.streams[0];
    CHECK(m.error.empty());
    CHECK(m.final_state == StreamState::stopped);
    CHECK(m.frames == static_cast<std::uint64_t>(2 * m.info.format.sample_rate_hz));
    // A loopback stream may legitimately deliver nothing while nothing plays,
    // which ends the session as timed_out; anything else must complete.
    if (summary.state == SessionState::timed_out) {
        REQUIRE(summary.streams.size() == 2);
        MESSAGE("playback delivered " << summary.streams[1].frames
                                      << " frames; play something to test loopback fully");
    } else {
        CHECK(summary.state == SessionState::completed);
    }
}

int main(int argc, char** argv) {
    if (env("AUDIO_OBSERVER_HARDWARE") != "1") {
        std::cout << "Skipped: set AUDIO_OBSERVER_HARDWARE=1 to run the hardware smoke test "
                     "(it opens the microphone).\n";
        return kSkip;
    }
    doctest::Context context(argc, argv);
    return context.run();
}
