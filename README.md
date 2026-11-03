# audio-observer

audio-observer captures several audio streams at once, for example a headset's microphone and what the same headset is playing, and reports how healthy they are and how their timing behaves: levels, clipping, silence, dropouts, discontinuities, buffer overruns and clock rate. It is a C++17 library and a command-line tool, and it runs for a bounded time (1 to 60 seconds) and then stops.

## Intended use

- **Checking a capture setup before it matters:** before recording, streaming or a call, confirm that a microphone and a playback loopback deliver every sample, without gaps, clipping or silent channels.
- **Diagnosing intermittent audio problems:** crackles and dropouts become counted, timestamped events instead of impressions.
- **Testing capture code:** the synthetic backend reproduces dropouts, overlaps, overruns, clock drift and jitter deterministically, so capture and analysis code can be tested without hardware.

## Privacy

**No samples are written, exported or sent anywhere.** Audio is used in memory to compute numbers and then discarded, and the tool never changes default devices, routing, volumes or exclusive mode. The full privacy rule, including a note on loopback streams, is in [`docs/design.md`](docs/design.md).

## Quick start on Windows

You need Visual Studio 2022 or 2026 with the C++ workload, which includes CMake, Ninja and the Windows SDK. Run these commands from a Developer PowerShell (or an "x64 Native Tools" command prompt) for Visual Studio, in the repository:

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

Names must match exactly one endpoint. A loopback stream receives nothing while nothing is playing on that output, so play something during the run. More options are in [`docs/usage.md`](docs/usage.md).

## Quick start on Linux

Only the synthetic backend is available on Linux for now. You need CMake 3.22+, Ninja and GCC or Clang; the `Dockerfile` provides the same toolchain (Ubuntu 22.04 with GCC, Clang and MinGW-w64).

```sh
cmake --preset default && cmake --build --preset default && ctest --preset default
./build/default/audio-observer inspect
./build/default/audio-observer meter --seconds 10
./build/default/audio-observer meter --seconds 2 --fast --fault mic:dropout@4800+960
```

`--fault` injects faults into a synthetic stream; see [`docs/usage.md`](docs/usage.md) and `--help`.

## Validation

| Item | Result | Details |
| --- | --- | --- |
| Unit tests | Need no audio hardware; built and run in CI with GCC, Clang, ASan + UBSan, TSan and MSVC; `mingw` cross-built | [Building and testing](docs/building.md) |
| WASAPI backend (Windows) | Run once on a USB wireless headset: `completed` in 10.058 s, no gaps or overruns | [Hardware run](docs/hardware-run.md) |
| Synthetic backend (Windows, Linux) | Used by all automated tests | [Usage](docs/usage.md#platforms) |
| ALSA backend (Linux) | Planned | [Roadmap](ROADMAP.md) |

## Documentation

- [`docs/usage.md`](docs/usage.md): what it measures, platforms, endpoint names and fault injection.
- [`docs/summary-schema.md`](docs/summary-schema.md): the JSON output format.
- [`docs/building.md`](docs/building.md): presets, what the tests cover, and the opt-in hardware test.
- [`docs/design.md`](docs/design.md): the privacy rule.
- [`docs/hardware-run.md`](docs/hardware-run.md): the first run on real hardware.
- [`docs/design-history.md`](docs/design-history.md): how the design got here.

## Status

Stage 1 of the roadmap is complete: the C++ library, the command-line tool, the synthetic and WASAPI backends, and dropout detection, tested in CI on Linux and Windows and run once on real hardware. Timing between streams (Stage 2) is next.

## Ownership and contributors

This is an independent personal project by Han ([MetaCortex728](https://github.com/MetaCortex728)). It is not owned by, or developed for, any employer, client, university or other organization, and no one else currently works on the repository.

## Project history

The project began in October 2026 as a Windows Python prototype, first routing audio through Voicemeeter and a Focusrite interface, then capturing a headset directly with WASAPI. In November 2026 it was continued as this C++17 library. The prototype is kept unchanged in [`prototype/`](prototype/). The full history, and who built each part with which tools, is in [`docs/design-history.md`](docs/design-history.md).

## Roadmap

See [`ROADMAP.md`](ROADMAP.md): timing between streams (callback jitter, clock drift, latency with a test tone, device changes), a Linux ALSA backend, a live terminal view, and longer-term directions such as microphone arrays.

## Licence

audio-observer is licensed under the Apache License 2.0; see [`LICENSE`](LICENSE). [`NOTICE`](NOTICE) lists third-party software: doctest (MIT), used only by the unit tests.

Release binaries are not published yet. If Windows binaries built with MinGW-w64 are published later, they link the GCC runtime statically, so the GCC runtime's licence notice (GPL with the GCC Runtime Library Exception) and the winpthreads licence notice must ship with them.
