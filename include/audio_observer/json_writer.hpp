#pragma once

/// @file
/// Minimal JSON writer for scalar reports.
///
/// The writer deliberately has no array support and caps string length, so
/// a report written with it can only hold scalars, short strings and nested
/// objects: there is no way to emit a run of samples.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace audio_observer {

class JsonWriter {
public:
    /// Longest string written, in bytes; longer strings are cut at a UTF-8
    /// character boundary and end in "...".
    static constexpr std::size_t kMaxStringBytes = 240;

    /// Writes to @p out, indenting nested objects by @p indent spaces
    /// (0 writes everything on one line).
    explicit JsonWriter(std::ostream& out, int indent = 2);

    /// Opens an object: the top-level value, or the value of the last key().
    JsonWriter& begin_object();
    /// Closes the innermost object. After the top-level object, ends the
    /// line.
    JsonWriter& end_object();

    /// Starts a member of the current object.
    JsonWriter& key(std::string_view name);

    JsonWriter& string(std::string_view text);
    JsonWriter& boolean(bool flag);
    JsonWriter& integer(std::int64_t number);
    JsonWriter& unsigned_integer(std::uint64_t number);
    /// Writes @p number with @p decimals digits after the point, or null
    /// when it is not finite.
    JsonWriter& number(double number, int decimals);
    JsonWriter& null();

    /// Returns @p text escaped and quoted as a JSON string, cut to
    /// kMaxStringBytes.
    static std::string quote(std::string_view text);

private:
    void before_value();
    void newline();

    std::ostream& out_;
    int indent_;
    std::vector<bool> has_members_;  // one entry per open object
    bool expecting_value_ = false;   // after key()
};

}  // namespace audio_observer
