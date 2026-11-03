#pragma once

/// @file
/// Windows Audio Session API (WASAPI) backend. Built on Windows only.
///
/// - Endpoints come from IMMDeviceEnumerator: active capture endpoints, and
///   active render endpoints, which are offered as loopback sources. Names
///   are the endpoints' friendly names. Listing activates an IAudioClient on
///   each endpoint to read its shared-mode mix format, but initializes and
///   starts no stream.
/// - Streams are shared-mode and event-driven (IAudioClient /
///   IAudioCaptureClient); a loopback stream captures what a render endpoint
///   plays, with AUDCLNT_STREAMFLAGS_LOOPBACK. Samples arrive in the mix
///   format and are converted to float (see wasapi_format.hpp).
/// - Each block's sample index is the device position and its host time is
///   the performance-counter time at which the device captured the first
///   frame, moved onto the host steady clock.
/// - Endpoint names are matched exactly with select_endpoint(), which fails
///   closed. The backend never changes default devices, routing, volumes or
///   the share mode, never opens a render stream and never uses exclusive
///   mode.
/// - Every COM object is held by an RAII wrapper and every stream is stopped
///   and released on completion, error and stop requests (Ctrl+C).
///
/// A loopback stream delivers nothing while its render endpoint plays
/// nothing: Windows does not run the endpoint, so no packets arrive.

#if defined(_WIN32)

#include <memory>

#include "audio_observer/backend.hpp"

namespace audio_observer {

/// Creates the WASAPI backend. Initializes COM on the calling thread in the
/// multithreaded apartment for the backend's lifetime; throws BackendError
/// when that or creating the device enumerator fails.
std::unique_ptr<Backend> make_wasapi_backend();

}  // namespace audio_observer

#endif  // _WIN32
