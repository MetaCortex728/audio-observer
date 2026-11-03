#include "audio_observer/json_writer.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace audio_observer {

JsonWriter::JsonWriter(std::ostream& out, int indent) : out_(out), indent_(indent < 0 ? 0 : indent) {}

std::string JsonWriter::quote(std::string_view text) {
    if (text.size() > kMaxStringBytes) {
        std::size_t cut = kMaxStringBytes - 3;
        // Step back over UTF-8 continuation bytes so a character is not split.
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        return quote(std::string(text.substr(0, cut)) + "...");
    }
    std::string result;
    result.reserve(text.size() + 2);
    result += '"';
    for (const char c : text) {
        switch (c) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
                    result += escaped;
                } else {
                    result += c;
                }
        }
    }
    result += '"';
    return result;
}

void JsonWriter::newline() {
    if (indent_ > 0) {
        out_ << '\n' << std::string(has_members_.size() * static_cast<std::size_t>(indent_), ' ');
    }
}

void JsonWriter::before_value() {
    if (has_members_.empty()) {
        return;  // top-level value
    }
    if (!expecting_value_) {
        throw std::logic_error("JsonWriter: a value inside an object needs a key");
    }
    expecting_value_ = false;
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    out_ << '{';
    has_members_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    if (has_members_.empty() || expecting_value_) {
        throw std::logic_error("JsonWriter: unbalanced end_object");
    }
    const bool had_members = has_members_.back();
    has_members_.pop_back();
    if (had_members) {
        newline();
    }
    out_ << '}';
    if (has_members_.empty()) {
        out_ << '\n';
    }
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    if (has_members_.empty() || expecting_value_) {
        throw std::logic_error("JsonWriter: key outside an object");
    }
    if (has_members_.back()) {
        out_ << ',';
    }
    has_members_.back() = true;
    newline();
    out_ << quote(name) << (indent_ > 0 ? ": " : ":");
    expecting_value_ = true;
    return *this;
}

JsonWriter& JsonWriter::string(std::string_view text) {
    before_value();
    out_ << quote(text);
    return *this;
}

JsonWriter& JsonWriter::boolean(bool flag) {
    before_value();
    out_ << (flag ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::integer(std::int64_t number) {
    before_value();
    out_ << number;
    return *this;
}

JsonWriter& JsonWriter::unsigned_integer(std::uint64_t number) {
    before_value();
    out_ << number;
    return *this;
}

JsonWriter& JsonWriter::number(double number, int decimals) {
    before_value();
    if (!std::isfinite(number)) {
        out_ << "null";
        return *this;
    }
    if (decimals < 0) {
        decimals = 0;
    } else if (decimals > 9) {
        decimals = 9;
    }
    char text[64];
    if (std::fabs(number) >= 1e15) {
        std::snprintf(text, sizeof text, "%.6e", number);
    } else {
        std::snprintf(text, sizeof text, "%.*f", decimals, number);
    }
    // Avoid "-0.00".
    std::string formatted(text);
    if (formatted.find_first_not_of("-0.") == std::string::npos && formatted[0] == '-') {
        formatted.erase(0, 1);
    }
    out_ << formatted;
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_ << "null";
    return *this;
}

}  // namespace audio_observer
