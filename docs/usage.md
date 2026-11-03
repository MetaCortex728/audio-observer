# Using the library and tool

What audio-observer measures and how to run it. The [README](../README.md) has the quick start.

## What it measures

Per stream, over the whole run and per channel:

- **Levels:** peak and RMS in dBFS, a peak-hold meter, clipped sample counts, and silence detection.
- **Glitches:** gaps and overlaps in the device's sample positions, discontinuities and timestamp errors reported by the device, and blocks lost to buffer overruns, each counted and the first few timestamped.
- **Clock:** a rough device clock rate against the host clock, in parts per million.
- **Session state:** whether every stream delivered exactly the requested number of frames (`completed`), or the run was cut short (`timed_out`, `stopped`, `failed`). A short capture is never reported as complete.

The output format is documented in [`summary-schema.md`](summary-schema.md).

## Platforms

| Platform | Backend | Status |
| --- | --- | --- |
| Windows | `wasapi` (default): shared-mode microphone capture and playback loopback | Cross-compiled with MinGW-w64 and built with MSVC in CI. Run once on a USB wireless headset (see [the hardware run](hardware-run.md)). |
| Windows, Linux | `synthetic`: deterministic generated signals with injectable faults | Used by all automated tests |
| Linux | ALSA | Planned (see the roadmap) |

## Endpoint names and running a meter on Windows

Names must match exactly one endpoint. A misspelt or ambiguous name fails with an error instead of falling back to another device. Add `--coexistence` to open a second client on the microphone, which shows that capture does not get in the way of other applications using it. Ctrl+C stops the run and closes every stream (exit status 130). A loopback stream receives nothing while nothing is playing on that output, so play something during the run or expect a `timed_out` result with a note.

## Fault injection on Linux

`--fault` injects dropouts, overlaps, discontinuities, overruns, device failures, clock drift or timestamp jitter into a synthetic stream; see `--help`.
