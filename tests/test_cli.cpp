#include <doctest.h>

#include <atomic>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define AO_POPEN _popen
#define AO_PCLOSE _pclose
#else
#include <sys/wait.h>
#define AO_POPEN popen
#define AO_PCLOSE pclose
#endif

#include "audio_observer/json_writer.hpp"
#include "cli.hpp"
#include "json_check.hpp"

namespace cli = audio_observer::cli;

namespace {

struct Run {
    int code = -1;
    std::string out;
    std::string err;
    json_check::Result json;
};

// Runs the CLI in-process. Unless the arguments name a backend, the
// synthetic one is used, so no test opens a real device whatever the
// platform's default backend is.
Run run_cli(std::vector<std::string> args, const std::atomic<bool>* stop = nullptr) {
    bool has_backend = false;
    for (const std::string& arg : args) {
        has_backend = has_backend || arg == "--backend";
    }
    if (!has_backend && !args.empty()) {
        args.insert(args.end(), {"--backend", "synthetic"});
    }
    std::ostringstream out;
    std::ostringstream err;
    Run r;
    r.code = cli::run(args, out, err, stop);
    r.out = out.str();
    r.err = err.str();
    r.json = json_check::parse(r.out);
    return r;
}

// Only scalars and short strings: no arrays, and a bounded string length.
void check_scalars_only(const json_check::Result& json) {
    REQUIRE_MESSAGE(json.valid, json.problem);
    CHECK_FALSE(json.has_array);
    CHECK(json.longest_string <= audio_observer::JsonWriter::kMaxStringBytes);
}

const std::vector<std::string> kFaults = {
    "--fault", "mic:dropout@4800+960",   "--fault", "mic:discontinuity@9600",
    "--fault", "mic:overlap@14400+240",  "--fault", "mic:overrun@19200",
    "--fault", "output:drift=150",       "--fault", "output:discontinuity@0",
};

std::vector<std::string> with_faults(std::vector<std::string> args) {
    args.insert(args.end(), kFaults.begin(), kFaults.end());
    return args;
}

// Checks the figures that the injected kFaults must produce.
void check_fault_figures(const json_check::Result& json, const std::string& seconds) {
    CHECK(json.at("schema") == "audio-observer.summary/2");
    CHECK(json.at("backend") == "synthetic");
    CHECK(json.at("state") == "completed");
    CHECK(json.at("error") == "null");
    CHECK(json.at("seconds_requested") == seconds + ".000");

    const long frames = std::stol(seconds) * 48000;
    CHECK(json.at("streams.mic.endpoint") == "Synthetic Microphone");
    CHECK(json.at("streams.mic.direction") == "capture");
    CHECK(json.at("streams.mic.sample_rate_hz") == "48000");
    CHECK(json.at("streams.mic.channels") == "1");
    CHECK(json.at("streams.mic.frames") == std::to_string(frames - 480));
    CHECK(json.at("streams.mic.warnings") == "1");
    CHECK(json.at("streams.mic.peak_dbfs") == "-12.00");
    CHECK(json.at("streams.mic.per_channel.0.peak_dbfs") == "-12.00");
    CHECK(json.at("streams.mic.dropouts.total") == "4");
    CHECK(json.at("streams.mic.dropouts.gaps") == "1");
    CHECK(json.at("streams.mic.dropouts.gap_frames") == "960");
    CHECK(json.at("streams.mic.dropouts.overlaps") == "1");
    CHECK(json.at("streams.mic.dropouts.overlap_frames") == "240");
    CHECK(json.at("streams.mic.dropouts.discontinuities") == "1");
    CHECK(json.at("streams.mic.dropouts.overruns") == "1");
    CHECK(json.at("streams.mic.dropouts.overrun_frames") == "480");
    CHECK(json.at("streams.mic.dropouts.events.0.kind") == "gap");
    CHECK(json.at("streams.mic.dropouts.events.0.sample_index") == "4800");
    CHECK(json.at("streams.mic.dropouts.events.0.frames") == "960");
    CHECK(json.at("streams.mic.dropouts.events.0.stream_seconds") == "0.100000");
    CHECK(json.at("streams.mic.dropouts.events.1.kind") == "discontinuity");
    CHECK(json.at("streams.mic.dropouts.events.1.sample_index") == "9600");
    CHECK(json.at("streams.mic.dropouts.events.2.kind") == "overlap");
    CHECK(json.at("streams.mic.dropouts.events.2.sample_index") == "14160");
    CHECK(json.at("streams.mic.dropouts.events.3.kind") == "overrun");
    CHECK(json.at("streams.mic.dropouts.events.3.sample_index") == "19440");
    CHECK_FALSE(json.has("streams.mic.dropouts.events.4.kind"));

    CHECK(json.at("streams.system.endpoint") == "Synthetic Headset");
    CHECK(json.at("streams.system.direction") == "loopback");
    CHECK(json.at("streams.system.channels") == "2");
    CHECK(json.at("streams.system.frames") == std::to_string(frames));
    CHECK(json.at("streams.system.peak_dbfs") == "-6.00");
    CHECK(json.at("streams.system.dropouts.total") == "1");
    CHECK(json.at("streams.system.dropouts.discontinuities") == "1");
    CHECK(json.at("streams.system.dropouts.events.0.sample_index") == "0");
    const double ppm = std::stod(json.at("streams.system.dropouts.rate_offset_ppm"));
    CHECK(ppm == doctest::Approx(150.0).epsilon(1e-4));
}

}  // namespace

TEST_CASE("cli help and version") {
    Run r = run_cli({"--help"});
    CHECK(r.code == cli::kExitOk);
    CHECK(r.out.find("usage: audio-observer inspect") != std::string::npos);
    r = run_cli({"--version"});
    CHECK(r.code == cli::kExitOk);
    CHECK(r.out.find("audio-observer 0.") == 0);
}

TEST_CASE("cli rejects bad usage with exit code 2") {
    const std::vector<std::vector<std::string>> bad = {
        {},
        {"record"},
        {"meter", "extra"},
        {"meter", "--seconds", "0"},
        {"meter", "--seconds", "61"},
        {"meter", "--seconds", "1.5"},
        {"meter", "--seconds", "-3"},
        {"meter", "--seconds"},
        {"meter", "--verbose"},
        {"meter", "--backend", "alsa"},
        {"meter", "--fault", "mic:dropout@100"},
        {"meter", "--fault", "speaker:overrun@100"},
        {"meter", "--fault", "mic:overrun@100+5"},
        {"meter", "--fault", "mic:wobble=3"},
        {"meter", "--fault", "mic:jitter=abc"},
        {"meter", "--backend", "wasapi", "--fast"},
        {"inspect", "--coexistence"},
    };
    for (const auto& args : bad) {
        const Run r = run_cli(args);
        CAPTURE(r.err);
        CHECK(r.code == cli::kExitUsage);
        CHECK(r.out.empty());
        CHECK(r.err.find("audio-observer: error:") != std::string::npos);
    }
    CHECK(run_cli({"meter", "--seconds", "61"}).err.find("Duration must be 1-60 seconds") !=
          std::string::npos);
}

TEST_CASE("cli reports that wasapi is not available on this build") {
    for (const char* command : {"inspect", "meter"}) {
        const Run r = run_cli({command, "--backend", "wasapi"});
        CHECK(r.code == cli::kExitFailure);
        CHECK(r.out.empty());
        CHECK(r.err == "ERROR: The wasapi backend is not available on this build\n");
    }
}

TEST_CASE("cli inspect lists endpoints and checks the selection") {
    Run r = run_cli({"inspect"});
    CHECK(r.code == cli::kExitOk);
    check_scalars_only(r.json);
    CHECK(r.json.at("schema") == "audio-observer.inspect/1");
    CHECK(r.json.at("endpoints.0.name") == "Synthetic Microphone");
    CHECK(r.json.at("endpoints.1.name") == "Synthetic Headset");
    CHECK(r.json.at("endpoints.1.direction") == "output");
    CHECK(r.json.at("endpoints.1.loopback") == "true");
    CHECK(r.json.at("selected.mic.ok") == "true");
    CHECK(r.json.at("selected.system.ok") == "true");
    CHECK(r.err.empty());

    r = run_cli({"inspect", "--mic", "Synthetic Microphone (2)"});
    CHECK(r.code == cli::kExitFailure);
    check_scalars_only(r.json);
    CHECK(r.json.at("selected.mic.ok") == "false");
    CHECK(r.json.at("selected.mic.error") ==
          "Expected one exact capture endpoint for Synthetic Microphone (2); got 0");
    CHECK(r.json.at("selected.system.ok") == "true");
}

TEST_CASE("cli meter reports injected faults in scalar JSON") {
    const Run r = run_cli(with_faults({"meter", "--seconds", "2", "--fast", "--seed", "3"}));
    CHECK(r.code == cli::kExitOk);
    CHECK(r.err.find("ACTIVE: 2s local meters only") == 0);
    check_scalars_only(r.json);
    check_fault_figures(r.json, "2");
    CHECK(r.json.at("coexistence") == "false");
    CHECK_FALSE(r.json.has("streams.mic_second_client.frames"));
}

TEST_CASE("cli summary size does not grow with the duration") {
    const Run short_run = run_cli(with_faults({"meter", "--seconds", "1", "--fast"}));
    const Run long_run = run_cli(with_faults({"meter", "--seconds", "10", "--fast"}));
    check_scalars_only(short_run.json);
    check_scalars_only(long_run.json);
    CHECK(short_run.json.scalar_count == long_run.json.scalar_count);
    // A handful of figures per channel and per stream, nothing like a
    // sample count (10 s is 480000 frames per channel).
    CHECK(long_run.json.scalar_count < 300);
}

TEST_CASE("cli coexistence opens a second microphone client") {
    const Run r = run_cli({"meter", "--seconds", "1", "--fast", "--coexistence"});
    CHECK(r.code == cli::kExitOk);
    check_scalars_only(r.json);
    CHECK(r.json.at("coexistence") == "true");
    CHECK(r.json.at("streams.mic_second_client.endpoint") == "Synthetic Microphone");
    CHECK(r.json.at("streams.mic_second_client.frames") == "48000");
    CHECK(r.json.at("streams.mic.frames") == "48000");
}

TEST_CASE("cli meter fails on a device failure or a missing endpoint, still with a summary") {
    Run r = run_cli({"meter", "--seconds", "5", "--fast", "--fault", "mic:fail@24000"});
    CHECK(r.code == cli::kExitFailure);
    check_scalars_only(r.json);
    CHECK(r.json.at("state") == "failed");
    CHECK(r.json.at("error") == "Stream mic failed: synthetic device failure at frame 24000");
    CHECK(r.json.at("streams.mic.state") == "failed");
    CHECK(r.json.at("streams.mic.frames") == "24000");
    CHECK(r.err.find("ERROR: Stream mic failed") != std::string::npos);

    r = run_cli({"meter", "--seconds", "1", "--fast", "--output", "Speakers"});
    CHECK(r.code == cli::kExitFailure);
    check_scalars_only(r.json);
    CHECK(r.json.at("state") == "failed");
    CHECK(r.json.at("error") == "Expected one exact loopback endpoint for Speakers; got 0");
}

TEST_CASE("cli meter reports a capture cut short by the safety limit as timed out") {
    // Real time; the mic clock runs at a fifth of its rate, so one second of
    // frames would take five seconds, beyond the limit (seconds + 2 s).
    const Run r = run_cli({"meter", "--seconds", "1", "--fault", "mic:drift=-800000"});
    CHECK(r.code == cli::kExitFailure);
    check_scalars_only(r.json);
    CHECK(r.json.at("schema") == "audio-observer.summary/2");
    CHECK(r.json.at("state") == "timed_out");
    CHECK(r.json.at("error") ==
          "Wall-clock safety limit passed before every stream delivered the requested frames "
          "(short: mic)");
    CHECK(std::stol(r.json.at("streams.mic.frames")) < 48000);
    CHECK(r.json.at("streams.system.frames") == "48000");
    CHECK(r.err.find("ERROR: Wall-clock safety limit passed") != std::string::npos);
}

TEST_CASE("cli meter stops on request with exit code 130") {
    const std::atomic<bool> stop{true};
    const Run r = run_cli({"meter", "--seconds", "60"}, &stop);
    CHECK(r.code == cli::kExitStopped);
    check_scalars_only(r.json);
    CHECK(r.json.at("state") == "stopped");
    CHECK(r.err.find("STOPPED by user; capture streams closed.") != std::string::npos);
}

TEST_CASE("cli meter reports a silent microphone as silent") {
    Run r = run_cli({"meter", "--seconds", "1", "--fast", "--mic", "Synthetic Silence"});
    CHECK(r.code == cli::kExitOk);
    CHECK(r.json.at("streams.mic.peak_dbfs") == "-180.00");
    CHECK(r.json.at("streams.mic.silent") == "true");
    CHECK(r.json.at("note") == "null");
}

TEST_CASE("end to end: the audio-observer executable reports injected faults") {
    std::string command = std::string("\"") + AUDIO_OBSERVER_CLI_PATH +
                          "\" meter --backend synthetic --seconds 2 --fast";
    for (const std::string& arg : kFaults) {
        command += " " + arg;
    }
    command += " 2>&1";
    FILE* pipe = AO_POPEN(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string output;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
        output.append(buffer, n);
    }
    const int status = AO_PCLOSE(pipe);
#if defined(_WIN32)
    CHECK(status == 0);
#else
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
#endif
    // The ACTIVE notice comes first on stderr; the JSON follows on stdout.
    const std::size_t brace = output.find('{');
    REQUIRE(brace != std::string::npos);
    CHECK(output.substr(0, brace).find("ACTIVE: 2s local meters only") == 0);
    const json_check::Result json = json_check::parse(output.substr(brace));
    check_scalars_only(json);
    check_fault_figures(json, "2");
}
