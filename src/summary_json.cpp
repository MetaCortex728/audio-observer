#include "audio_observer/summary_json.hpp"

#include <chrono>
#include <string>

#include "audio_observer/json_writer.hpp"
#include "audio_observer/version.hpp"

namespace audio_observer {

namespace {

constexpr int kDbDecimals = 2;
constexpr int kSecondsDecimals = 6;

const char* to_string(SampleType type) noexcept {
    switch (type) {
        case SampleType::float32:
            return "float32";
        case SampleType::int16:
            return "int16";
        case SampleType::int24:
            return "int24";
        case SampleType::int32:
            return "int32";
    }
    return "unknown";
}

JsonWriter& optional_string(JsonWriter& json, const std::string& text) {
    return text.empty() ? json.null() : json.string(text);
}

void write_levels(JsonWriter& json, const MeterReading& reading) {
    json.key("peak_dbfs").number(reading.peak_dbfs, kDbDecimals);
    json.key("rms_dbfs").number(reading.rms_dbfs, kDbDecimals);
    json.key("peak_hold_dbfs").number(reading.peak_hold_dbfs, kDbDecimals);
    json.key("clipped_samples").unsigned_integer(reading.clipped_samples);
    json.key("silent").boolean(reading.silent);
    json.key("silence_count").unsigned_integer(reading.silence_count);
}

void write_dropouts(JsonWriter& json, const DropoutReport& r, std::uint32_t sample_rate_hz) {
    json.key("dropouts").begin_object();
    json.key("total").unsigned_integer(r.total());
    json.key("gaps").unsigned_integer(r.gaps);
    json.key("gap_frames").unsigned_integer(r.gap_frames);
    json.key("overlaps").unsigned_integer(r.overlaps);
    json.key("overlap_frames").unsigned_integer(r.overlap_frames);
    json.key("discontinuities").unsigned_integer(r.discontinuities);
    json.key("overruns").unsigned_integer(r.overruns);
    json.key("overrun_frames").unsigned_integer(r.overrun_frames);
    json.key("unplaced_overruns").unsigned_integer(r.unplaced_overruns);
    json.key("timestamp_errors").unsigned_integer(r.timestamp_errors);
    json.key("time_regressions").unsigned_integer(r.time_regressions);
    json.key("rate_offset_ppm");
    if (r.rate_offset_ppm) {
        json.number(*r.rate_offset_ppm, 3);
    } else {
        json.null();
    }
    json.key("events_omitted").unsigned_integer(r.events_omitted);

    // Events are an object keyed "0", "1", ... (the schema has no arrays).
    json.key("events").begin_object();
    const std::int64_t origin = r.first_sample_index.value_or(0);
    for (std::size_t i = 0; i < r.events.size(); ++i) {
        const GlitchEvent& event = r.events[i];
        json.key(std::to_string(i)).begin_object();
        json.key("kind").string(to_string(event.kind));
        json.key("sample_index").integer(event.sample_index);
        json.key("frames").unsigned_integer(event.frames);
        json.key("stream_seconds")
            .number(sample_rate_hz > 0 ? static_cast<double>(event.sample_index - origin) /
                                             static_cast<double>(sample_rate_hz)
                                       : 0.0,
                    kSecondsDecimals);
        json.key("host_seconds")
            .number(std::chrono::duration<double>(event.host_time - r.first_host_time).count(),
                    kSecondsDecimals);
        json.end_object();
    }
    json.end_object();
    json.end_object();
}

void write_stream(JsonWriter& json, const StreamSummary& s) {
    json.key(s.label).begin_object();
    json.key("endpoint").string(s.info.name);
    json.key("direction").string(to_string(s.info.direction));
    json.key("sample_rate_hz").unsigned_integer(s.info.format.sample_rate_hz);
    json.key("channels").unsigned_integer(s.info.format.channel_count);
    json.key("sample_type").string(to_string(s.info.format.sample_type));
    json.key("state").string(to_string(s.final_state));
    json.key("error");
    optional_string(json, s.error);
    json.key("blocks").unsigned_integer(s.blocks);
    json.key("frames").unsigned_integer(s.frames);
    json.key("warnings").unsigned_integer(s.flagged_blocks);
    write_levels(json, s.levels);
    json.key("per_channel").begin_object();
    for (std::size_t c = 0; c < s.channels.size(); ++c) {
        json.key(std::to_string(c)).begin_object();
        write_levels(json, s.channels[c]);
        json.end_object();
    }
    json.end_object();
    write_dropouts(json, s.dropouts, s.info.format.sample_rate_hz);
    json.end_object();
}

}  // namespace

void write_summary_json(std::ostream& out, const SummaryContext& context,
                        const SessionSummary& summary) {
    JsonWriter json(out);
    json.begin_object();
    json.key("schema").string(kSummarySchema);
    json.key("tool_version").string(version());
    json.key("backend").string(context.backend);
    json.key("retention").string("scalar figures only; no samples were stored or sent");
    json.key("state").string(to_string(summary.state));
    json.key("error");
    optional_string(json, context.error.empty() ? summary.error : context.error);
    json.key("note");
    optional_string(json, context.note);
    json.key("coexistence").boolean(context.coexistence);
    json.key("seconds_requested").number(summary.seconds_requested, 3);
    json.key("elapsed_seconds").number(summary.elapsed_seconds, 3);
    json.key("streams").begin_object();
    for (const StreamSummary& s : summary.streams) {
        write_stream(json, s);
    }
    json.end_object();
    json.end_object();
}

void write_inspect_json(std::ostream& out, const std::string& backend,
                        const std::vector<EndpointInfo>& endpoints,
                        const std::vector<SelectionResult>& selected) {
    JsonWriter json(out);
    json.begin_object();
    json.key("schema").string(kInspectSchema);
    json.key("tool_version").string(version());
    json.key("backend").string(backend);
    json.key("endpoints").begin_object();
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        const EndpointInfo& e = endpoints[i];
        json.key(std::to_string(i)).begin_object();
        json.key("name").string(e.name);
        json.key("direction").string(to_string(e.direction));
        json.key("loopback").boolean(e.loopback);
        json.key("sample_rate_hz").unsigned_integer(e.format.sample_rate_hz);
        json.key("channels").unsigned_integer(e.format.channel_count);
        json.key("sample_type").string(to_string(e.format.sample_type));
        json.end_object();
    }
    json.end_object();
    json.key("selected").begin_object();
    for (const SelectionResult& s : selected) {
        json.key(s.role).begin_object();
        json.key("name").string(s.name);
        json.key("direction").string(to_string(s.direction));
        json.key("ok").boolean(s.error.empty());
        json.key("error");
        optional_string(json, s.error);
        json.end_object();
    }
    json.end_object();
    json.end_object();
}

}  // namespace audio_observer
