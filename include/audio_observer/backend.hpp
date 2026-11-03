#pragma once

/// @file
/// Interface to an audio API: endpoint enumeration and capture streams.
///
/// A backend only ever opens capture streams (a microphone, or the loopback
/// of a playback endpoint). It never changes routing or default devices, and
/// it hands samples to a BlockQueue for in-memory analysis only.

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio_observer/audio_format.hpp"
#include "audio_observer/block_queue.hpp"
#include "audio_observer/stream_info.hpp"

namespace audio_observer {

/// Failure reported by a backend, for example an endpoint that cannot be
/// matched exactly or a device that stops working.
class BackendError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Whether an endpoint records (a microphone) or plays (a headset output).
enum class EndpointDirection : std::uint8_t {
    input,
    output,
};

/// Returns "input" or "output".
const char* to_string(EndpointDirection direction) noexcept;
/// Returns "capture" or "loopback".
const char* to_string(StreamDirection direction) noexcept;

/// One endpoint as enumerated by a backend.
struct EndpointInfo {
    std::string name;  ///< Exact name used to select the endpoint.
    EndpointDirection direction = EndpointDirection::input;
    /// For output endpoints: what it plays can be captured as a loopback stream.
    bool loopback = false;
    AudioFormat format;  ///< Shared-mode mix format.
};

/// Returns the single endpoint that a stream of @p direction may open under
/// @p name: an input endpoint for capture, or a loopback-capable output
/// endpoint for loopback. Fails closed: throws BackendError naming the match
/// count when there are zero or several matches, and never substitutes a
/// similar endpoint.
const EndpointInfo& select_endpoint(const std::vector<EndpointInfo>& endpoints,
                                    const std::string& name,
                                    StreamDirection direction);

/// Lifecycle of a capture stream.
enum class StreamState : std::uint8_t {
    opened,    ///< Opened, not started.
    running,   ///< Delivering blocks.
    finished,  ///< The source ended normally (finite sources only).
    failed,    ///< The device failed; see CaptureStream::error().
    stopped,   ///< Stopped on request.
};

/// Returns the lower-case name of @p state.
const char* to_string(StreamState state) noexcept;

/// An open capture stream. Blocks are delivered from the stream's own thread
/// into the queue given to start(). Destroying the stream stops it and
/// releases the device.
class CaptureStream {
public:
    virtual ~CaptureStream() = default;
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;

    /// Endpoint name, direction and format of the stream.
    virtual const StreamInfo& info() const noexcept = 0;
    /// Largest block the stream delivers, in frames.
    virtual std::size_t max_block_frames() const noexcept = 0;

    /// Starts delivery into @p queue, which must outlive the stream (or the
    /// next stop()) and have info().format.channel_count channels.
    /// Throws BackendError when the stream cannot start.
    virtual void start(BlockQueue& queue) = 0;
    /// Stops delivery and waits for the stream's thread. Idempotent.
    virtual void stop() noexcept = 0;

    /// Current state. Safe from any thread.
    virtual StreamState state() const noexcept = 0;
    /// Failure description once state() is StreamState::failed, else empty.
    virtual std::string error() const = 0;

protected:
    CaptureStream() = default;
};

/// An audio API.
class Backend {
public:
    virtual ~Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    /// Short identifier, e.g. "synthetic".
    virtual std::string name() const = 0;
    /// Lists endpoints without opening any stream.
    virtual std::vector<EndpointInfo> endpoints() = 0;
    /// Opens (but does not start) a stream on the endpoint chosen by
    /// select_endpoint(). Throws BackendError on failure.
    virtual std::unique_ptr<CaptureStream> open(const std::string& endpoint_name,
                                                StreamDirection direction) = 0;

protected:
    Backend() = default;
};

}  // namespace audio_observer
