#include <doctest.h>

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#include "audio_observer/json_writer.hpp"
#include "json_check.hpp"

using audio_observer::JsonWriter;

TEST_CASE("json writer produces valid nested objects") {
    std::ostringstream out;
    JsonWriter json(out);
    json.begin_object();
    json.key("name").string("a \"quoted\" \\ name\n\x01");
    json.key("count").unsigned_integer(18446744073709551615ull);
    json.key("offset").integer(-42);
    json.key("level").number(-12.345, 2);
    json.key("zero").number(-0.0001, 2);
    json.key("huge").number(1e300, 2);
    json.key("missing").number(std::numeric_limits<double>::quiet_NaN(), 2);
    json.key("flag").boolean(true);
    json.key("nothing").null();
    json.key("empty").begin_object().end_object();
    json.key("inner").begin_object();
    json.key("x").number(1.5, 1);
    json.end_object();
    json.end_object();

    const json_check::Result r = json_check::parse(out.str());
    REQUIRE_MESSAGE(r.valid, r.problem << "\n" << out.str());
    CHECK_FALSE(r.has_array);
    CHECK(r.at("name") == "a \"quoted\" \\ name\n?");
    CHECK(r.at("count") == "18446744073709551615");
    CHECK(r.at("offset") == "-42");
    CHECK(r.at("level") == "-12.35");
    CHECK(r.at("zero") == "0.00");
    CHECK(r.at("huge") == "1.000000e+300");
    CHECK(r.at("missing") == "null");
    CHECK(r.at("flag") == "true");
    CHECK(r.at("nothing") == "null");
    CHECK(r.at("inner.x") == "1.5");
}

TEST_CASE("json writer cuts long strings at a character boundary") {
    std::string long_text(JsonWriter::kMaxStringBytes - 4, 'a');
    long_text += "\xC3\xA9\xC3\xA9\xC3\xA9";  // "ééé" straddles the limit
    const std::string quoted = JsonWriter::quote(long_text);
    CHECK(quoted.size() <= JsonWriter::kMaxStringBytes + 2);
    CHECK(quoted.substr(quoted.size() - 4) == "...\"");
    // The kept part ends on whole characters.
    CHECK(quoted.substr(quoted.size() - 6, 2) == "aa");
    CHECK(JsonWriter::quote(std::string(500, 'x')).size() == JsonWriter::kMaxStringBytes + 2);
}

TEST_CASE("json writer refuses malformed structure") {
    std::ostringstream out;
    JsonWriter json(out);
    json.begin_object();
    CHECK_THROWS_AS(json.string("no key"), std::logic_error);
    json.key("k");
    CHECK_THROWS_AS(json.key("again"), std::logic_error);
    CHECK_THROWS_AS(json.end_object(), std::logic_error);
}

TEST_CASE("json checker sees arrays and invalid documents") {
    CHECK(json_check::parse("{\"a\": [1, 2]}").has_array);
    CHECK_FALSE(json_check::parse("{\"a\": 1,}").valid);
    CHECK_FALSE(json_check::parse("{\"a\": 01}").valid);
    CHECK_FALSE(json_check::parse("{\"a\": 1} x").valid);
}
