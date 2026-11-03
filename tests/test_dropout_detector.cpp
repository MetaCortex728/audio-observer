#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "audio_observer/dropout_detector.hpp"
#include "audio_observer/session.hpp"
#include "audio_observer/synthetic_backend.hpp"

using namespace audio_observer;

namespace {

const std::vector<float> kSamples(1024, 0.0f);

DeliveredBlock delivered(std::int64_t first, std::size_t frames, std::int64_t host_ms,
                         std::uint32_t flags = 0, std::uint32_t dropped_blocks = 0,
                         std::uint64_t dropped_frames = 0) {
    DeliveredBlock d;
    d.block = FrameBlock(kSamples.data(), frames, 1, first,
                         HostTime(std::chrono::milliseconds(host_ms)));
    d.flags = flags;
    d.dropped_blocks_before = dropped_blocks;
    d.dropped_frames_before = dropped_frames;
    return d;
}

void check_event(const GlitchEvent& event, GlitchKind kind, std::int64_t index,
                 std::uint64_t frames, std::int64_t host_ms) {
    CHECK(event.kind == kind);
    CHECK(event.sample_index == index);
    CHECK(event.frames == frames);
    CHECK(event.host_time == HostTime(std::chrono::milliseconds(host_ms)));
}

}  // namespace

TEST_CASE("contiguous blocks produce no glitches") {
    DropoutDetector detector(1000);
    for (int i = 0; i < 10; ++i) {
        detector.observe(delivered(i * 100, 100, 100 + i * 100));
    }
    const DropoutReport& r = detector.report();
    CHECK(r.total() == 0);
    CHECK(r.events.empty());
    CHECK(*r.first_sample_index == 0);
    REQUIRE(r.rate_offset_ppm);
    CHECK(*r.rate_offset_ppm == doctest::Approx(0.0));
}

TEST_CASE("gaps, overlaps and device flags are counted and placed") {
    DropoutDetector detector(1000);
    detector.observe(delivered(0, 100, 100));
    detector.observe(delivered(150, 100, 250));  // gap of 50 at 100
    detector.observe(delivered(230, 100, 330));  // overlap of 20 at 230
    detector.observe(delivered(330, 100, 430, block_flags::discontinuity));
    detector.observe(delivered(430, 100, 420, block_flags::timestamp_error));  // time goes back

    const DropoutReport& r = detector.report();
    CHECK(r.gaps == 1);
    CHECK(r.gap_frames == 50);
    CHECK(r.overlaps == 1);
    CHECK(r.overlap_frames == 20);
    CHECK(r.discontinuities == 1);
    CHECK(r.timestamp_errors == 1);
    CHECK(r.time_regressions == 1);
    CHECK(r.overruns == 0);
    REQUIRE(r.events.size() == 5);
    check_event(r.events[0], GlitchKind::gap, 100, 50, 250);
    check_event(r.events[1], GlitchKind::overlap, 230, 20, 330);
    check_event(r.events[2], GlitchKind::discontinuity, 330, 0, 430);
    check_event(r.events[3], GlitchKind::timestamp_error, 430, 0, 420);
    check_event(r.events[4], GlitchKind::time_regression, 430, 0, 420);
}

TEST_CASE("overruns are told apart from device gaps") {
    DropoutDetector detector(1000);
    detector.observe(delivered(0, 100, 100));
    // Two blocks (200 frames) were dropped by the queue, no device gap.
    detector.observe(delivered(300, 100, 400, 0, 2, 200));
    // One block dropped, and the device also skipped 30 frames.
    detector.observe(delivered(530, 100, 630, 0, 1, 100));
    detector.finish(3, 300);

    const DropoutReport& r = detector.report();
    CHECK(r.overruns == 3);
    CHECK(r.overrun_frames == 300);
    CHECK(r.unplaced_overruns == 0);
    CHECK(r.gaps == 1);
    CHECK(r.gap_frames == 30);
    REQUIRE(r.events.size() == 3);
    check_event(r.events[0], GlitchKind::overrun, 100, 200, 400);
    check_event(r.events[1], GlitchKind::overrun, 400, 100, 630);
    check_event(r.events[2], GlitchKind::gap, 500, 30, 630);
    // Delivered frames only; the clock's own count includes the dropped ones.
    CHECK(detector.clock().gap_count() == 1);
}

TEST_CASE("overruns with no later block are counted but not placed") {
    DropoutDetector detector(1000);
    detector.observe(delivered(0, 100, 100));
    detector.finish(4, 400);
    const DropoutReport& r = detector.report();
    CHECK(r.overruns == 4);
    CHECK(r.overrun_frames == 400);
    CHECK(r.unplaced_overruns == 4);
    CHECK(r.events.empty());
}

TEST_CASE("events beyond the limit are counted, not kept") {
    DropoutDetector detector(1000, 2);
    detector.observe(delivered(0, 10, 1));
    for (int i = 1; i <= 5; ++i) {
        detector.observe(delivered(i * 20, 10, 1 + i, block_flags::discontinuity));
    }
    const DropoutReport& r = detector.report();
    CHECK(r.gaps == 5);
    CHECK(r.discontinuities == 5);
    CHECK(r.events.size() == 2);
    CHECK(r.events_omitted == 8);
}

TEST_CASE("injected synthetic faults are found with exact counts and positions") {
    SyntheticConfig synthetic;
    synthetic.pacing = Pacing::fast;
    synthetic.stop_after_seconds = 2.0;
    synthetic.endpoints[0].faults = {
        {FaultKind::dropout, 4800, 960},
        {FaultKind::discontinuity, 9600, 0},
        {FaultKind::overlap, 14400, 240},
        {FaultKind::overrun, 19200, 0},
        {FaultKind::dropout, 48000, 48},
        {FaultKind::discontinuity, 48000, 0},
    };
    synthetic.endpoints[1].drift_ppm = 150.0;
    SyntheticBackend backend(synthetic);

    SessionConfig config;
    config.seconds = 2.0;
    config.streams = {{"mic", kSyntheticMicName, StreamDirection::capture},
                      {"system", kSyntheticOutputName, StreamDirection::loopback}};
    const SessionSummary summary = Session(backend, config).run();
    REQUIRE(summary.state == SessionState::completed);
    REQUIRE(summary.streams.size() == 2);

    const StreamSummary& mic = summary.streams[0];
    const DropoutReport& r = mic.dropouts;
    CHECK(r.gaps == 2);
    CHECK(r.gap_frames == 960 + 48);
    CHECK(r.overlaps == 1);
    CHECK(r.overlap_frames == 240);
    CHECK(r.discontinuities == 2);
    CHECK(r.overruns == 1);
    CHECK(r.overrun_frames == 480);
    CHECK(r.time_regressions == 0);
    CHECK(mic.flagged_blocks == 2);
    CHECK(mic.frames == 96000 - 480);

    REQUIRE(r.events.size() == 6);
    CHECK(r.events[0].kind == GlitchKind::gap);
    CHECK(r.events[0].sample_index == 4800);
    CHECK(r.events[0].frames == 960);
    CHECK(r.events[1].kind == GlitchKind::discontinuity);
    CHECK(r.events[1].sample_index == 9600);
    CHECK(r.events[2].kind == GlitchKind::overlap);
    CHECK(r.events[2].sample_index == 14160);
    CHECK(r.events[2].frames == 240);
    CHECK(r.events[3].kind == GlitchKind::overrun);
    CHECK(r.events[3].sample_index == 19440);
    CHECK(r.events[3].frames == 480);
    // Positions continue from 14160 + 480k; 48000 is first passed at 48240.
    CHECK(r.events[4].kind == GlitchKind::gap);
    CHECK(r.events[4].sample_index == 48240);
    CHECK(r.events[4].frames == 48);
    CHECK(r.events[5].kind == GlitchKind::discontinuity);
    CHECK(r.events[5].sample_index == 48288);
    // Event host times follow the synthetic device clock: the first gap is
    // revealed by the block ending 5760 + 480 device frames in.
    const double gap_seconds =
        std::chrono::duration<double>(r.events[0].host_time - r.first_host_time).count();
    CHECK(gap_seconds == doctest::Approx((6240.0 - 480.0) / 48000.0).epsilon(1e-6));

    const DropoutReport& system = summary.streams[1].dropouts;
    CHECK(system.total() == 0);
    REQUIRE(system.rate_offset_ppm);
    CHECK(*system.rate_offset_ppm == doctest::Approx(150.0).epsilon(1e-4));
    // The mic's estimate is biased by its 240-frame overlap (about -2500
    // ppm over 2 s); the estimate is rough by design.
    REQUIRE(r.rate_offset_ppm);
    CHECK(*r.rate_offset_ppm == doctest::Approx(-240.0 / 96000.0 * 1e6).epsilon(0.01));
}
