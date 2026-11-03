#include "audio_observer/backend.hpp"

namespace audio_observer {

const char* to_string(EndpointDirection direction) noexcept {
    return direction == EndpointDirection::input ? "input" : "output";
}

const char* to_string(StreamDirection direction) noexcept {
    return direction == StreamDirection::capture ? "capture" : "loopback";
}

const char* to_string(StreamState state) noexcept {
    switch (state) {
        case StreamState::opened:
            return "opened";
        case StreamState::running:
            return "running";
        case StreamState::finished:
            return "finished";
        case StreamState::failed:
            return "failed";
        case StreamState::stopped:
            return "stopped";
    }
    return "unknown";
}

const EndpointInfo& select_endpoint(const std::vector<EndpointInfo>& endpoints,
                                    const std::string& name,
                                    StreamDirection direction) {
    const EndpointInfo* match = nullptr;
    std::size_t count = 0;
    for (const EndpointInfo& endpoint : endpoints) {
        const bool usable = direction == StreamDirection::capture
                                ? endpoint.direction == EndpointDirection::input
                                : endpoint.direction == EndpointDirection::output && endpoint.loopback;
        if (usable && endpoint.name == name) {
            match = &endpoint;
            ++count;
        }
    }
    if (count != 1) {
        throw BackendError("Expected one exact " + std::string(to_string(direction)) +
                           " endpoint for " + name + "; got " + std::to_string(count));
    }
    return *match;
}

}  // namespace audio_observer
