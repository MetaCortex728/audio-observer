cmake_minimum_required(VERSION 3.22)

project(audio-observer
    VERSION 0.1.0
    DESCRIPTION "Multi-stream audio health and timing diagnostics (never records audio)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AUDIO_OBSERVER_BUILD_TESTS "Build the audio-observer unit tests" ON)
option(AUDIO_OBSERVER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(AUDIO_OBSERVER_SANITIZE "none" CACHE STRING
    "Sanitizers to build with: none, address (ASan + UBSan) or thread (TSan)")
set_property(CACHE AUDIO_OBSERVER_SANITIZE PROPERTY STRINGS none address thread)

# Shared warning and sanitizer settings, applied per target.
add_library(audio_observer_options INTERFACE)
if(MSVC)
    target_compile_options(audio_observer_options INTERFACE
        /W4 /permissive- /utf-8 /EHsc)
    if(AUDIO_OBSERVER_WARNINGS_AS_ERRORS)
        target_compile_options(audio_observer_options INTERFACE /WX)
    endif()
else()
    target_compile_options(audio_observer_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(AUDIO_OBSERVER_WARNINGS_AS_ERRORS)
        target_compile_options(audio_observer_options INTERFACE -Werror)
    endif()
    if(AUDIO_OBSERVER_SANITIZE STREQUAL "address")
        target_compile_options(audio_observer_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(audio_observer_options INTERFACE -fsanitize=address,undefined)
    elseif(AUDIO_OBSERVER_SANITIZE STREQUAL "thread")
        target_compile_options(audio_observer_options INTERFACE
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(audio_observer_options INTERFACE -fsanitize=thread)
    elseif(NOT AUDIO_OBSERVER_SANITIZE STREQUAL "none")
        message(FATAL_ERROR "Unknown AUDIO_OBSERVER_SANITIZE value: ${AUDIO_OBSERVER_SANITIZE}")
    endif()
endif()

find_package(Threads REQUIRED)

# Core library: standard library only, no audio API.
add_library(audio_observer STATIC
    src/audio_format.cpp
    src/backend.cpp
    src/block_queue.cpp
    src/dropout_detector.cpp
    src/meters.cpp
    src/sample_clock.cpp
    src/session.cpp
    src/synthetic_backend.cpp
    src/version.cpp)
add_library(audio_observer::audio_observer ALIAS audio_observer)
target_include_directories(audio_observer PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(audio_observer PRIVATE
    AUDIO_OBSERVER_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(audio_observer
    PUBLIC Threads::Threads
    PRIVATE $<BUILD_INTERFACE:audio_observer_options>)

if(AUDIO_OBSERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
