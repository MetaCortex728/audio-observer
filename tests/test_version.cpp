#include <doctest.h>

#include <string>

#include "audio_observer/version.hpp"

TEST_CASE("version matches the project version") {
    CHECK(std::string(audio_observer::version()) == AUDIO_OBSERVER_EXPECTED_VERSION);
}
