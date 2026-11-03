#include "cli.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>

#include "audio_observer/session.hpp"
#include "audio_observer/summary_json.hpp"
#include "audio_observer/synthetic_backend.hpp"
#include "audio_observer/version.hpp"

namespace audio_observer::cli {

namespace {

constexpr const char* kUsage = R"(usage: audio-observer inspect [options]
       audio-observer meter [--seconds N] [--coexistence] [options]

Captures audio streams and reports scalar health figures as JSON. Samples are
used in memory and discarded: nothing is recorded, exported or sent.

commands:
  inspect              list endpoints and check the selected names; opens no streams
  meter                meter the microphone and the output loopback for N seconds

options:
  --backend NAME       synthetic or wasapi (default: synthetic)
  --mic NAME           exact microphone endpoint name
  --output NAME        exact playback endpoint name, captured as a loopback
  --seconds N          meter duration, whole seconds from 1 to 60 (default: 10)
  --coexistence        also open a second client on the microphone
  --help               show this help
  --version            show the version

synthetic backend only:
  --seed N             seed for generated signals and jitter (default: 1)
  --fast               generate as fast as possible instead of in real time
  --fault ROLE:SPEC    inject a fault; ROLE is mic or output and SPEC one of
                       dropout@FRAME+N, overlap@FRAME+N, discontinuity@FRAME,
                       overrun@FRAME, fail@FRAME, drift=PPM, jitter=SECONDS
)";

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FaultSpec {
    std::string role;  // "mic" or "output"
    std::optional<Fault> fault;
    std::optional<double> drift_ppm;
    std::optional<double> jitter_seconds;
};

struct Options {
    std::string command;
    std::string backend = "synthetic";
    std::optional<std::string> mic;
    std::optional<std::string> output;
    int seconds = 10;
    bool coexistence = false;
    std::optional<std::uint64_t> seed;
    bool fast = false;
    std::vector<FaultSpec> faults;
};

std::int64_t parse_integer(const std::string& text, const std::string& what) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        throw UsageError(what + " must be a whole number: " + text);
    }
    errno = 0;
    const long long value = std::strtoll(text.c_str(), nullptr, 10);
    if (errno == ERANGE) {
        throw UsageError(what + " is too large: " + text);
    }
    return value;
}

double parse_double(const std::string& text, const std::string& what) {
    std::size_t used = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used == 0 || used != text.size() || !std::isfinite(value)) {
        throw UsageError(what + " must be a number: " + text);
    }
    return value;
}

FaultSpec parse_fault(const std::string& text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) {
        throw UsageError("--fault needs ROLE:SPEC: " + text);
    }
    FaultSpec spec;
    spec.role = text.substr(0, colon);
    if (spec.role != "mic" && spec.role != "output") {
        throw UsageError("--fault role must be mic or output: " + text);
    }
    const std::string body = text.substr(colon + 1);
    const std::size_t equals = body.find('=');
    if (equals != std::string::npos) {
        const std::string name = body.substr(0, equals);
        const double value = parse_double(body.substr(equals + 1), "--fault " + name);
        if (name == "drift") {
            if (value <= -1e6) {
                throw UsageError("--fault drift must be above -1000000 ppm");
            }
            spec.drift_ppm = value;
        } else if (name == "jitter") {
            if (value < 0.0 || value > 1.0) {
                throw UsageError("--fault jitter must be 0-1 seconds");
            }
            spec.jitter_seconds = value;
        } else {
            throw UsageError("unknown --fault setting: " + text);
        }
        return spec;
    }
    const std::size_t at = body.find('@');
    if (at == std::string::npos) {
        throw UsageError("--fault needs KIND@FRAME: " + text);
    }
    const std::string kind = body.substr(0, at);
    std::string position = body.substr(at + 1);
    std::int64_t frames = 0;
    const std::size_t plus = position.find('+');
    if (plus != std::string::npos) {
        frames = parse_integer(position.substr(plus + 1), "--fault size");
        position = position.substr(0, plus);
    }
    Fault fault;
    fault.at_frame = parse_integer(position, "--fault position");
    fault.frames = frames;
    const bool sized = kind == "dropout" || kind == "overlap";
    if (kind == "dropout") {
        fault.kind = FaultKind::dropout;
    } else if (kind == "overlap") {
        fault.kind = FaultKind::overlap;
    } else if (kind == "discontinuity") {
        fault.kind = FaultKind::discontinuity;
    } else if (kind == "overrun") {
        fault.kind = FaultKind::overrun;
    } else if (kind == "fail") {
        fault.kind = FaultKind::failure;
    } else {
        throw UsageError("unknown --fault kind: " + text);
    }
    if (sized != (plus != std::string::npos) || (sized && frames <= 0)) {
        throw UsageError(sized ? "--fault " + kind + " needs a size: " + text
                               : "--fault " + kind + " takes no size: " + text);
    }
    spec.fault = fault;
    return spec;
}

Options parse(const std::vector<std::string>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw UsageError(arg + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--backend") {
            options.backend = value();
        } else if (arg == "--mic") {
            options.mic = value();
        } else if (arg == "--output") {
            options.output = value();
        } else if (arg == "--seconds") {
            const std::string& text = value();
            const std::int64_t seconds = parse_integer(text, "--seconds");
            if (seconds < 1 || seconds > 60) {
                throw UsageError("Duration must be 1-60 seconds");
            }
            options.seconds = static_cast<int>(seconds);
        } else if (arg == "--coexistence") {
            options.coexistence = true;
        } else if (arg == "--seed") {
            options.seed = static_cast<std::uint64_t>(parse_integer(value(), "--seed"));
        } else if (arg == "--fast") {
            options.fast = true;
        } else if (arg == "--fault") {
            options.faults.push_back(parse_fault(value()));
        } else if (!arg.empty() && arg[0] == '-') {
            throw UsageError("unknown option: " + arg);
        } else if (options.command.empty()) {
            options.command = arg;
        } else {
            throw UsageError("unexpected argument: " + arg);
        }
    }
    if (options.command != "inspect" && options.command != "meter") {
        throw UsageError(options.command.empty() ? "a command is required"
                                                 : "unknown command: " + options.command);
    }
    if (options.command == "inspect" && (options.coexistence || options.fast ||
                                         !options.faults.empty())) {
        throw UsageError("inspect opens no streams; --coexistence, --fast and --fault apply to meter");
    }
    if (options.backend != "synthetic" && options.backend != "wasapi") {
        throw UsageError("unknown backend: " + options.backend + " (expected synthetic or wasapi)");
    }
    if (options.backend != "synthetic" && (options.seed || options.fast || !options.faults.empty())) {
        throw UsageError("--seed, --fast and --fault need --backend synthetic");
    }
    return options;
}

// Default endpoint names per backend.
std::string default_mic(const Options& options) {
    return options.mic.value_or(kSyntheticMicName);
}

std::string default_output(const Options& options) {
    return options.output.value_or(kSyntheticOutputName);
}

std::unique_ptr<Backend> make_backend(const Options& options) {
    if (options.backend == "wasapi") {
        throw BackendError("The wasapi backend is not available on this build");
    }
    SyntheticConfig config;
    if (options.seed) {
        config.seed = *options.seed;
    }
    if (options.fast) {
        config.pacing = Pacing::fast;
        config.stop_after_seconds = static_cast<double>(options.seconds);
    }
    const std::string mic = default_mic(options);
    const std::string output = default_output(options);
    for (const FaultSpec& spec : options.faults) {
        const std::string& target = spec.role == "mic" ? mic : output;
        for (SyntheticEndpoint& endpoint : config.endpoints) {
            if (endpoint.info.name != target) {
                continue;
            }
            if (spec.fault) {
                endpoint.faults.push_back(*spec.fault);
            }
            if (spec.drift_ppm) {
                endpoint.drift_ppm = *spec.drift_ppm;
            }
            if (spec.jitter_seconds) {
                endpoint.jitter_seconds = *spec.jitter_seconds;
            }
        }
    }
    return std::make_unique<SyntheticBackend>(std::move(config));
}

int inspect(const Options& options, Backend& backend, std::ostream& out, std::ostream& err) {
    const std::vector<EndpointInfo> endpoints = backend.endpoints();
    std::vector<SelectionResult> selected{
        {"mic", default_mic(options), StreamDirection::capture, {}},
        {"system", default_output(options), StreamDirection::loopback, {}},
    };
    bool ok = true;
    for (SelectionResult& s : selected) {
        try {
            select_endpoint(endpoints, s.name, s.direction);
        } catch (const BackendError& e) {
            s.error = e.what();
            ok = false;
            err << "ERROR: " << e.what() << '\n';
        }
    }
    write_inspect_json(out, backend.name(), endpoints, selected);
    return ok ? kExitOk : kExitFailure;
}

int meter(const Options& options, Backend& backend, std::ostream& out, std::ostream& err,
          const std::atomic<bool>* stop_request) {
    SessionConfig config;
    config.seconds = static_cast<double>(options.seconds);
    config.streams = {{"mic", default_mic(options), StreamDirection::capture},
                      {"system", default_output(options), StreamDirection::loopback}};
    if (options.coexistence) {
        config.streams.push_back({"mic_second_client", default_mic(options), StreamDirection::capture});
    }
    Session session(backend, config);
    err << "ACTIVE: " << options.seconds
        << "s local meters only; no recording/upload/transcription." << std::endl;
    const SessionSummary summary = session.run(stop_request);

    SummaryContext context;
    context.backend = backend.name();
    context.coexistence = options.coexistence;
    int code = kExitOk;
    if (summary.state == SessionState::failed || summary.state == SessionState::timed_out) {
        // A short capture is a failure of the measurement, not a result.
        code = kExitFailure;
    } else if (summary.state == SessionState::stopped) {
        code = kExitStopped;
    } else {
        // The prototype's checks: the microphone must deliver; a silent
        // playback device may legitimately deliver nothing.
        for (const StreamSummary& s : summary.streams) {
            if (s.label == "mic" && s.frames == 0) {
                context.error = "Microphone returned no frames";
                code = kExitFailure;
            }
            if (s.label == "system" && s.frames == 0) {
                context.note = "Inconclusive: no playback frames; silence may produce no loopback callbacks";
            }
        }
    }
    write_summary_json(out, context, summary);
    out.flush();
    if (code == kExitStopped) {
        err << "STOPPED by user; capture streams closed." << std::endl;
    } else if (code == kExitFailure) {
        err << "ERROR: " << (context.error.empty() ? summary.error : context.error) << std::endl;
    }
    return code;
}

}  // namespace

int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
        const std::atomic<bool>* stop_request) {
    for (const std::string& arg : args) {
        if (arg == "--help" || arg == "-h") {
            out << kUsage;
            return kExitOk;
        }
        if (arg == "--version") {
            out << "audio-observer " << version() << '\n';
            return kExitOk;
        }
    }
    Options options;
    try {
        options = parse(args);
    } catch (const UsageError& e) {
        err << kUsage << "\naudio-observer: error: " << e.what() << '\n';
        return kExitUsage;
    }
    try {
        const std::unique_ptr<Backend> backend = make_backend(options);
        if (options.command == "inspect") {
            return inspect(options, *backend, out, err);
        }
        return meter(options, *backend, out, err, stop_request);
    } catch (const std::exception& e) {
        err << "ERROR: " << e.what() << '\n';
        return kExitFailure;
    }
}

}  // namespace audio_observer::cli
