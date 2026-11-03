# audio-observer

audio-observer captures several audio streams at once, for example a headset's microphone and what the same headset is playing, and reports how healthy they are and how their timing behaves: levels, clipping, silence, dropouts, discontinuities, buffer overruns and clock rate. It is a C++17 library and a command-line tool, and it runs for a bounded time (1 to 60 seconds) and then stops.

## Privacy rule

**No samples are written, exported or sent anywhere.** Audio is used in memory to compute numbers and then discarded. There are no audio files, uploads, transcription, speech models, background capture or autostart. The JSON output holds scalar figures and short strings only; the JSON writer has no way to write an array, and a test checks every command's output for this. The tool never changes default devices, routing, volumes or exclusive mode, and it only ever opens capture streams.

Note that a loopback stream carries everything played on that output, including notifications and voice calls. The figures it produces are levels and counts, but close sources you do not want included before a run.

## What it measures

Per stream, over the whole run and per channel:

- **Levels:** peak and RMS in dBFS, a peak-hold meter, clipped sample counts, and silence detection.
- **Glitches:** gaps and overlaps in the device's sample positions, discontinuities and timestamp errors reported by the device, and blocks lost to buffer overruns, each counted and the first few timestamped.
- **Clock:** a rough device clock rate against the host clock, in parts per million.
- **Session state:** whether every stream delivered exactly the requested number of frames (`completed`), or the run was cut short (`timed_out`, `stopped`, `failed`). A short capture is never reported as complete.

The output format is documented in [`docs/summary-schema.md`](docs/summary-schema.md).

## Platforms

| Platform | Backend | Status |
| --- | --- | --- |
| Windows | `wasapi` (default): shared-mode microphone capture and playback loopback | Cross-compiled with MinGW-w64 and built with MSVC in CI. **Not yet run against real audio hardware.** |
| Windows, Linux | `synthetic`: deterministic generated signals with injectable faults | Used by all automated tests |
| Linux | ALSA | Planned (see the roadmap) |

## Quick start on Windows

You need Visual Studio 2022 with the C++ workload (it includes CMake and the Windows SDK). From a Developer PowerShell in the repository:

```powershell
cmake --preset msvc
cmake --build --preset msvc
ctest --preset msvc
```

List the audio endpoints. This opens no streams; it prints each endpoint's exact name, direction and shared-mode format:

```powershell
.\build\msvc\Release\audio-observer.exe inspect
```

Then meter a microphone and the loopback of a playback endpoint for 10 seconds, using names exactly as `inspect` printed them (these two are examples):

```powershell
.\build\msvc\Release\audio-observer.exe meter --seconds 10 `
    --mic "Microphone (USB Headset)" --output "Headphones (USB Headset)"
```

Names must match exactly one endpoint. A misspelt or ambiguous name fails with an error instead of falling back to another device. Add `--coexistence` to open a second client on the microphone, which shows that capture does not get in the way of other applications using it. Ctrl+C stops the run and closes every stream (exit status 130). A loopback stream receives nothing while nothing is playing on that output, so play something during the run or expect a `timed_out` result with a note.

## Quick start on Linux

Only the synthetic backend is available on Linux for now. You need CMake 3.22+, Ninja and GCC or Clang; the `Dockerfile` provides the same toolchain (Ubuntu 22.04 with GCC, Clang and MinGW-w64).

```sh
cmake --preset default && cmake --build --preset default && ctest --preset default
./build/default/audio-observer inspect
./build/default/audio-observer meter --seconds 10
./build/default/audio-observer meter --seconds 2 --fast --fault mic:dropout@4800+960
```

`--fault` injects dropouts, overlaps, discontinuities, overruns, device failures, clock drift or timestamp jitter into a synthetic stream; see `--help`.

## Building and testing

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

### Opt-in hardware test

A smoke test of the WASAPI backend on real devices is built on Windows but never runs by default: it carries the CTest label `hardware`, which the test presets exclude, and it skips itself unless `AUDIO_OBSERVER_HARDWARE=1`. It lists endpoints, checks that matching fails closed and, if you name a microphone, meters it (and optionally a playback loopback) for two seconds. It opens the microphone, so run it only on your own machine and on purpose:

```powershell
$env:AUDIO_OBSERVER_TEST_MIC = "Microphone (USB Headset)"      # exact names from inspect
$env:AUDIO_OBSERVER_TEST_OUTPUT = "Headphones (USB Headset)"   # optional
ctest --preset msvc-hardware
```

## Design history

**Voicemeeter and Focusrite routing (early October 2026).** The project began as a Windows Python prototype for observing a Logitech PRO X 2 headset's microphone and the system's playback side by side, with levels only. The first design routed audio physically: system playback went to a Focusrite interface, its Loopback channels fed a Voicemeeter Banana input sent to bus B2, and the headset microphone fed another Banana input sent to bus B1, so that the two sources stayed separate. The tool read device names and Voicemeeter route settings and opened two WASAPI input streams for a bounded level test. It depended on Banana running, specific Focusrite settings, the Windows default output staying on the Focusrite, and manual isolation tests, and it could not check some of those settings itself.

**Direct WASAPI capture (10 October 2026).** The requirement changed: normal use of the USB headset, both listening and using its microphone in other applications, had to stay independent of the observer. The routing design was dropped, and an unused routing writer was removed without ever being run. The new prototype, `prototype/direct_probe.py`, captures the headset microphone with a shared-mode WASAPI stream and the headset's playback with WASAPI loopback: two capture streams and no new virtual devices, with no routing or default-device writes and no Focusrite or Voicemeeter dependency. Endpoints are matched by exact name and fail closed. A verified ten-second run captured a mono microphone and an 8-channel loopback at 48 kHz, plus a second microphone client, with zero warnings, and the Windows defaults were the same before and after. That run showed concurrent capture, not sample synchronization, perfect isolation or long-run stability; confirmation that listening and other applications' microphone use were unaffected was still pending.

**This C++ library (November 2026).** The prototype's level events carried local receipt times, not sample-accurate positions, and it counted callback warnings without telling dropouts, discontinuities and overruns apart. Measuring timing between streams (callback jitter, clock drift between devices, latency) needs sample-accurate clocks per stream, a capture path that does not allocate or lock, and tests with known answers, which hardware cannot provide. The prototype is therefore continued as a C++17 library with a lock-free ring buffer, per-stream sample clocks, dropout detection, a deterministic synthetic backend with fault injection for tests, and a WASAPI backend that keeps the prototype's safety rules: bounded runs, exact endpoint matching that fails closed, no routing or default-device changes, and only scalar results. The prototype is kept unchanged in [`prototype/`](prototype/), with its own README.

The Python prototype was developed locally in October 2026 by Han, with OpenAI Codex as a coding assistant and without version control. The first commit of this repository imports it unchanged except for two README sentences that referred to people and tools outside this project.

## Roadmap

See [`ROADMAP.md`](ROADMAP.md): timing between streams (callback jitter, clock drift, latency with a test tone, device changes), a Linux ALSA backend, a live terminal view, and longer-term directions such as microphone arrays.

## Licence

audio-observer is licensed under the Apache License 2.0; see [`LICENSE`](LICENSE). [`NOTICE`](NOTICE) lists third-party software: doctest (MIT), used only by the unit tests.

Release binaries are not published yet. If Windows binaries built with MinGW-w64 are published later, they link the GCC runtime statically, so the GCC runtime's licence notice (GPL with the GCC Runtime Library Exception) and the winpthreads licence notice must ship with them.
