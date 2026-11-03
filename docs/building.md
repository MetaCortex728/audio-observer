# Building and testing

Presets build into `build/<preset>` with warnings treated as errors:

```sh
cmake --preset default && cmake --build --preset default && ctest --preset default   # GCC
cmake --preset clang   && cmake --build --preset clang   && ctest --preset clang     # Clang
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan      # GCC, ASan + UBSan
cmake --preset tsan    && cmake --build --preset tsan    && ctest --preset tsan      # Clang, TSan
cmake --preset mingw   && cmake --build --preset mingw                               # Windows cross-build, tests not run
```

On Windows, use the `msvc` preset for all three steps. CI builds and tests the four Linux presets, cross-builds `mingw`, and builds and tests `msvc` on Windows.

`ctest` runs the unit tests, which need no audio hardware: the ring buffer and block queue (including under concurrent load), meters, the sample clock and dropout detection, the synthetic backend, bounded sessions (completion, timeouts, failures, stop requests, and that every stream is released), the command line and its JSON output, and the WASAPI backend's format conversion (float32, int16, int24 and int32 PCM) and its mapping of device positions, performance-counter times and buffer flags, using fake inputs.

## Opt-in hardware test

A smoke test of the WASAPI backend on real devices is built on Windows but never runs by default: it carries the CTest label `hardware`, which the test presets exclude, and it skips itself unless `AUDIO_OBSERVER_HARDWARE=1`. It lists endpoints, checks that matching fails closed and, if you name a microphone, meters it (and optionally a playback loopback) for two seconds. It opens the microphone, so run it only on your own machine and on purpose:

```powershell
$env:AUDIO_OBSERVER_TEST_MIC = "Microphone (USB Headset)"      # exact names from inspect
$env:AUDIO_OBSERVER_TEST_OUTPUT = "Headphones (USB Headset)"   # optional
ctest --preset msvc-hardware
```
