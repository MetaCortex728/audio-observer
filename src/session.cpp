#include "audio_observer/session.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

#include "audio_observer/block_queue.hpp"

namespace audio_observer {

const char* to_string(SessionState state) noexcept {
    switch (state) {
        case SessionState::completed:
            return "completed";
        case SessionState::stopped:
            return "stopped";
        case SessionState::failed:
            return "failed";
    }
    return "unknown";
}

Session::Session(Backend& backend, SessionConfig config)
    : backend_(backend), config_(std::move(config)) {
    if (config_.streams.empty()) {
        throw std::invalid_argument("A session needs at least one stream");
    }
    std::set<std::string> labels;
    for (const StreamRequest& request : config_.streams) {
        if (request.label.empty() || !labels.insert(request.label).second) {
            throw std::invalid_argument("Stream labels must be non-empty and unique");
        }
    }
    if (!(config_.min_seconds > 0.0) || !(config_.max_seconds >= config_.min_seconds) ||
        !std::isfinite(config_.max_seconds)) {
        throw std::invalid_argument("Session duration bounds are invalid");
    }
    if (!(config_.seconds >= config_.min_seconds && config_.seconds <= config_.max_seconds)) {
        throw std::invalid_argument("Duration must be " + std::to_string(config_.min_seconds) +
                                    "-" + std::to_string(config_.max_seconds) + " seconds");
    }
    if (!(config_.queue_seconds > 0.0)) {
        throw std::invalid_argument("Queue size must be positive");
    }
    if (!(config_.safety_margin_seconds >= 0.0) || !std::isfinite(config_.safety_margin_seconds)) {
        throw std::invalid_argument("Session safety margin must be finite and not negative");
    }
}

namespace {

// One opened stream. The stream shares ownership of the queue while its
// thread runs (see CaptureStream::start), so the order of these members does
// not matter for safety.
struct ActiveStream {
    StreamRequest request;
    std::shared_ptr<BlockQueue> queue;
    std::unique_ptr<CaptureStream> stream;
    std::optional<StreamMeter> meter;
    std::optional<DropoutDetector> detector;
    std::uint64_t target_frames = 0;  // seconds x sample rate
    std::uint64_t blocks = 0;
    std::uint64_t frames = 0;
    std::uint64_t flagged_blocks = 0;
    // Queue overrun totals when the target was reached; later drops are
    // beyond the requested duration.
    std::optional<std::uint64_t> overruns_at_target;
    std::uint64_t overrun_frames_at_target = 0;

    bool reached_target() const noexcept { return frames >= target_frames; }
};

// Moves waiting blocks of @p active into its meter until the stream has
// delivered its target frame count; the block that crosses the target is
// truncated there and anything after it is discarded. Returns whether
// anything was waiting.
bool drain(ActiveStream& active) {
    DeliveredBlock delivered;
    bool any = false;
    while (active.queue->pop(delivered)) {
        any = true;
        if (active.reached_target()) {
            continue;  // beyond the requested duration
        }
        const std::uint64_t remaining = active.target_frames - active.frames;
        if (delivered.block.frame_count() > remaining) {
            const FrameBlock b = delivered.block;
            delivered.block = FrameBlock(b.data(), static_cast<std::size_t>(remaining),
                                         b.channel_count(), b.first_sample_index(), b.host_time());
        }
        ++active.blocks;
        active.frames += delivered.block.frame_count();
        if (delivered.flags != 0) {
            ++active.flagged_blocks;
        }
        active.detector->observe(delivered);
        active.meter->process(delivered.block);
        if (active.reached_target()) {
            active.overruns_at_target = active.queue->overrun_count();
            active.overrun_frames_at_target = active.queue->overrun_frames();
        }
    }
    return any;
}

void stop_all(std::vector<ActiveStream>& streams) noexcept {
    for (ActiveStream& active : streams) {
        active.stream->stop();
    }
}

}  // namespace

SessionSummary Session::run(const std::atomic<bool>* stop_request) {
    SessionSummary summary;
    summary.seconds_requested = config_.seconds;
    std::vector<ActiveStream> streams;
    streams.reserve(config_.streams.size());
    const HostTime begin = host_now();

    try {
        for (const StreamRequest& request : config_.streams) {
            ActiveStream active;
            active.request = request;
            active.stream = backend_.open(request.endpoint_name, request.direction);
            const AudioFormat format = active.stream->info().format;
            const std::size_t max_block = active.stream->max_block_frames();
            const std::size_t capacity = std::max(
                static_cast<std::size_t>(std::llround(config_.queue_seconds *
                                                            static_cast<double>(format.sample_rate_hz))),
                4 * max_block);
            active.queue = std::make_shared<BlockQueue>(format.channel_count, capacity,
                                                        capacity / std::max<std::size_t>(max_block / 4, 1) + 4,
                                                        max_block);
            active.target_frames = static_cast<std::uint64_t>(
                std::llround(config_.seconds * static_cast<double>(format.sample_rate_hz)));
            active.meter.emplace(format.sample_rate_hz, format.channel_count, config_.meter);
            active.detector.emplace(format.sample_rate_hz, config_.max_events_per_stream);
            streams.push_back(std::move(active));
        }
        for (ActiveStream& active : streams) {
            active.stream->start(active.queue);
        }

        // Sessions end on frame counts, not on the wall clock, so coarse
        // scheduler granularity cannot let extra blocks through. The wall
        // clock only bounds a session whose streams deliver too slowly.
        const auto deadline = begin + std::chrono::nanoseconds(std::llround(
                                          (config_.seconds + config_.safety_margin_seconds) * 1e9));
        for (;;) {
            bool any = false;
            for (ActiveStream& active : streams) {
                any = drain(active) || any;
            }
            const auto failed = std::find_if(streams.begin(), streams.end(), [](const ActiveStream& a) {
                return a.stream->state() == StreamState::failed;
            });
            if (failed != streams.end()) {
                summary.state = SessionState::failed;
                summary.error = "Stream " + failed->request.label + " failed: " + failed->stream->error();
                break;
            }
            if (stop_request != nullptr && stop_request->load()) {
                summary.state = SessionState::stopped;
                break;
            }
            bool all_ended = true;
            for (ActiveStream& active : streams) {
                if (active.reached_target()) {
                    // Stop as soon as the requested frames are in, so the
                    // stream does not keep capturing for nothing.
                    active.stream->stop();
                } else if (active.stream->state() != StreamState::finished) {
                    all_ended = false;
                }
            }
            if (all_ended || host_now() >= deadline) {
                break;
            }
            if (!any) {
                std::this_thread::sleep_for(config_.poll_interval);
            }
        }
        stop_all(streams);
        for (ActiveStream& active : streams) {
            drain(active);
        }
    } catch (const std::exception& e) {
        stop_all(streams);
        summary.state = SessionState::failed;
        summary.error = e.what();
    }

    summary.elapsed_seconds = std::chrono::duration<double>(host_now() - begin).count();
    for (ActiveStream& active : streams) {
        StreamSummary s;
        s.label = active.request.label;
        s.info = active.stream->info();
        s.final_state = active.stream->state();
        s.error = active.stream->error();
        s.blocks = active.blocks;
        s.frames = active.frames;
        s.flagged_blocks = active.flagged_blocks;
        if (active.meter) {
            s.levels = active.meter->stream();
            for (std::size_t c = 0; c < active.meter->channel_count(); ++c) {
                s.channels.push_back(active.meter->channel(c));
            }
        }
        if (active.detector) {
            // The stream has stopped, so the queue's totals are final.
            if (active.overruns_at_target) {
                active.detector->finish(*active.overruns_at_target, active.overrun_frames_at_target);
            } else {
                active.detector->finish(active.queue->overrun_count(), active.queue->overrun_frames());
            }
            s.dropouts = active.detector->report();
        }
        summary.streams.push_back(std::move(s));
    }
    // Streams are released here, when `streams` goes out of scope.
    return summary;
}

}  // namespace audio_observer
