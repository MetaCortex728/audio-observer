# Roadmap

audio-observer captures several audio streams at once and diagnoses their health and timing. It never records, transcribes or uploads audio: samples are used in memory to compute numbers and then discarded. Every item below keeps that rule, and each one ships with tests that run without audio hardware (through the synthetic backend) unless it says otherwise.

## Done so far

- **Python prototype (October 2026, `prototype/`):** WASAPI capture of a headset microphone and its playback loopback side by side, scalar levels only, bounded to 1–60 seconds, exact endpoint matching that fails closed, no routing or default-device changes.

## Stage 1: C++ foundation (done, November 2026)

All eight items are implemented and tested with the synthetic backend on Linux (GCC, Clang, AddressSanitizer, ThreadSanitizer) and Windows (MSVC). The WASAPI backend has also run on real hardware (3 November 2026, with the owner's go-ahead): endpoint listing, and a 10-second microphone and loopback capture on a USB headset that completed with no gaps or overruns. That run showed WASAPI flagging each stream's first packet as discontinuous; this is now recorded separately (`start_discontinuity`) instead of being counted as a fault.

1. **Build and project setup:** CMake presets, C++17, doctest, warnings as errors, a Docker development image, CI on Linux and Windows, Apache-2.0 licence.
2. **Core types:** a multi-channel audio frame with any number of channels, a per-stream sample clock with sample-accurate positions and host timestamps, and stream descriptions that can carry device geometry later.
3. **Lock-free ring buffer:** single-producer, single-consumer, fixed capacity, no allocation or locks in the audio callback, with overrun counting. Tested under concurrent load.
4. **Meters:** RMS and peak level in dBFS, peak hold, clipping counts and silence detection per channel and per stream.
5. **Synthetic backend:** generates tones, noise, silence, clipping, dropouts and clock drift deterministically from a seed, so every analysis can be tested against known answers.
6. **Command-line tool:** `inspect` and `meter --seconds N [--coexistence]` with the prototype's behaviour and safety rules, plus JSON output of the scalar results.
7. **WASAPI backend:** shared-mode microphone capture and playback loopback on Windows, exact endpoint matching that fails closed, clean release on completion, error or Ctrl+C. Verified on real hardware only with the owner's go-ahead, since it opens the microphone.
8. **Dropout and glitch detection:** discontinuities reported by the device, buffer overruns and gaps in the sample clock, counted and timestamped.

## Stage 2: timing between streams

9. **Callback timing:** jitter and late callbacks per stream, with histograms (about 3 hours).
10. **Clock drift between devices:** estimate the rate difference between two streams' sample clocks over a session, tested with synthetic drift of known size (about 5 hours).
11. **Latency with a test tone:** the user starts a short tone on the playback device and the tool measures when it arrives on the microphone, by cross-correlation in memory (about 4 hours).
12. **Device changes:** unplugging, replugging and default-device switches handled without crashing; the session reports what happened and resumes or stops cleanly (about 3 hours).
13. **Session summary:** scalar statistics over time (levels, clipping, dropouts, jitter, drift) exported to JSON and CSV with a documented schema; a test checks that no sample data can be exported (about 2 hours).

## Stage 3: beyond one machine

14. **Linux ALSA backend** with the same interface and tests (about 5 hours). `libasound` is LGPL-2.1, so it is linked dynamically and not bundled.
15. **Live terminal view:** per-stream meters and timing figures that update while the session runs, with explicit start, pause and stop (about 3 hours).

## Later directions (not scheduled)

- **Multi-channel microphone arrays:** synchronized arrays with known geometry, as a basis for estimating where a sound comes from.
- **Connection to a sensor fusion pipeline:** directions of arrival placed in camera and LiDAR coordinates, using shared calibration and time association.
- **Acoustic sensing of space:** using sound to support spatial understanding when cameras, LiDAR or thermal imaging are degraded (smoke, darkness, glass).

These directions are why the core keeps arbitrary channel counts, sample-accurate clocks, an explicit clock-drift model and room for device geometry from the start.
