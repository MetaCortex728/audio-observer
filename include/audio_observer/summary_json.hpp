#pragma once

/// @file
/// JSON reports of endpoints and session summaries, in the schema described
/// in docs/summary-schema.md. Reports hold scalars and short strings only.

#include <ostream>
#include <string>
#include <vector>

#include "audio_observer/backend.hpp"
#include "audio_observer/session.hpp"

namespace audio_observer {

/// Identifier written as "schema" in a session summary.
inline constexpr const char* kSummarySchema = "audio-observer.summary/3";
/// Identifier written as "schema" in an endpoint listing.
inline constexpr const char* kInspectSchema = "audio-observer.inspect/2";

/// Context of a meter run that the session summary itself does not hold.
struct SummaryContext {
    std::string backend;
    bool coexistence = false;
    /// Overall error (session error, or a check such as "Microphone returned
    /// no frames"). Written as null when empty.
    std::string error;
    /// Caveat about the result. Written as null when empty.
    std::string note;
};

/// Writes a session summary as one JSON object.
void write_summary_json(std::ostream& out, const SummaryContext& context,
                        const SessionSummary& summary);

/// One endpoint the user selected by name, and how selection went.
struct SelectionResult {
    std::string role;  ///< "mic" or "system".
    std::string name;
    StreamDirection direction = StreamDirection::capture;
    std::string error;  ///< Empty when exactly one endpoint matched.
};

/// Writes an endpoint listing and the selection results as one JSON object.
void write_inspect_json(std::ostream& out, const std::string& backend,
                        const std::vector<EndpointInfo>& endpoints,
                        const std::vector<SelectionResult>& selected);

}  // namespace audio_observer
