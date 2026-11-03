#pragma once

// A strict little JSON parser for tests. It validates a document, notes
// whether it holds any array, measures its strings and flattens every scalar
// to a dotted path, e.g. "streams.mic.frames" -> "96000".

#include <cctype>
#include <cstddef>
#include <map>
#include <string>

namespace json_check {

struct Result {
    bool valid = false;
    std::string problem;
    bool has_array = false;
    std::size_t longest_string = 0;
    std::size_t scalar_count = 0;
    std::map<std::string, std::string> values;  // strings unquoted, others verbatim

    bool has(const std::string& path) const { return values.count(path) != 0; }
    std::string at(const std::string& path) const {
        const auto it = values.find(path);
        return it == values.end() ? "<missing>" : it->second;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Result parse() {
        try {
            skip_space();
            value("");
            skip_space();
            if (pos_ != text_.size()) {
                fail("trailing characters");
            }
            result_.valid = true;
        } catch (const std::string& problem) {
            result_.valid = false;
            result_.problem = problem + " at offset " + std::to_string(pos_);
        }
        return result_;
    }

private:
    [[noreturn]] void fail(const std::string& problem) { throw problem; }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    void expect(char c) {
        if (peek() != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++pos_;
    }

    void literal(const char* word) {
        for (const char* p = word; *p != '\0'; ++p) {
            expect(*p);
        }
    }

    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
            }
            if (c == '\\') {
                const char e = peek();
                ++pos_;
                if (e == 'u') {
                    for (int i = 0; i < 4; ++i) {
                        if (!std::isxdigit(static_cast<unsigned char>(peek()))) {
                            fail("bad \\u escape");
                        }
                        ++pos_;
                    }
                    out += '?';
                } else if (e == 'n') {
                    out += '\n';
                } else if (e == 't') {
                    out += '\t';
                } else if (e == 'r') {
                    out += '\r';
                } else if (e == 'b') {
                    out += '\b';
                } else if (e == 'f') {
                    out += '\f';
                } else if (e == '"' || e == '\\' || e == '/') {
                    out += e;
                } else {
                    fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        if (out.size() > result_.longest_string) {
            result_.longest_string = out.size();
        }
        return out;
    }

    std::string number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            fail("bad number");
        }
        if (peek() == '0') {
            ++pos_;
        } else {
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("bad fraction");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("bad exponent");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        return text_.substr(start, pos_ - start);
    }

    void scalar(const std::string& path, const std::string& value) {
        ++result_.scalar_count;
        result_.values[path] = value;
    }

    void value(const std::string& path) {
        skip_space();
        const char c = peek();
        if (c == '{') {
            ++pos_;
            skip_space();
            if (peek() == '}') {
                ++pos_;
                return;
            }
            for (;;) {
                skip_space();
                const std::string key = string();
                skip_space();
                expect(':');
                value(path.empty() ? key : path + "." + key);
                skip_space();
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                expect('}');
                return;
            }
        }
        if (c == '[') {
            result_.has_array = true;
            ++pos_;
            skip_space();
            if (peek() == ']') {
                ++pos_;
                return;
            }
            for (std::size_t i = 0;; ++i) {
                value(path + "." + std::to_string(i));
                skip_space();
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                expect(']');
                return;
            }
        }
        if (c == '"') {
            scalar(path, string());
        } else if (c == 't') {
            literal("true");
            scalar(path, "true");
        } else if (c == 'f') {
            literal("false");
            scalar(path, "false");
        } else if (c == 'n') {
            literal("null");
            scalar(path, "null");
        } else {
            scalar(path, number());
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    Result result_;
};

inline Result parse(const std::string& text) {
    return Parser(text).parse();
}

}  // namespace json_check
