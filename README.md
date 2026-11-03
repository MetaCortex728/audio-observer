# audio-observer

A tool that captures several audio streams at once and diagnoses their health and timing. It never records, transcribes or uploads audio: samples are used in memory to compute numbers and then discarded.

## Origin

Han developed the first version locally in October 2026 with OpenAI Codex as a coding assistant, without version control. It is a Windows Python prototype that reads a USB headset's microphone and its playback loopback side by side through WASAPI and reports levels for a bounded time. This repository starts from that prototype, kept as it was in `prototype/`, and continues it as a C++17 library and command-line tool.

The first commit imports the prototype unchanged except for two README sentences that referred to people and tools outside this project.

## Build and test

The C++ core needs CMake 3.22+, Ninja and a C++17 compiler; doctest is vendored under `third_party/`. The `Dockerfile` provides the same toolchain (Ubuntu 22.04 with GCC, Clang and MinGW-w64). Presets build into `build/<preset>` with warnings treated as errors:

```sh
cmake --preset default && cmake --build --preset default && ctest --preset default   # GCC
cmake --preset clang   && cmake --build --preset clang   && ctest --preset clang     # Clang
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan      # GCC, ASan + UBSan
cmake --preset tsan    && cmake --build --preset tsan    && ctest --preset tsan      # Clang, TSan
cmake --preset mingw   && cmake --build --preset mingw                               # Windows cross-build, tests not run
```

On Windows with Visual Studio 2022, use the `msvc` preset for all three steps. The tests use generated signals only and need no audio hardware.
