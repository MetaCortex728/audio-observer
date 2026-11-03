# Hardware run

On 3 November 2026 the MSVC Release build ran on the owner's Windows PC with a USB wireless gaming headset, with music playing on the headset:

```
audio-observer meter --seconds 10 --mic "Microphone (PRO X 2 LIGHTSPEED)" --output "Logi_hds (PRO X 2 LIGHTSPEED)"
```

The session was `completed` in 10.058 s with exit status 0, and the Windows default devices were unchanged afterwards.

| Stream | Frames | Blocks | Channels | Peak | RMS | Gaps, overruns | Rate offset |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Microphone | 480000 | 1000 | 1 | −22.21 dBFS | −47.87 dBFS | none | +5.6 ppm |
| Loopback | 480000 | 1000 | 8 | −2.01 dBFS | −24.25 dBFS | none | −0.6 ppm |

- The rate offsets are rough: a two-point estimate from the first and last blocks against the host clock, sensitive to timestamp jitter. Proper drift estimation is Stage 2 work, so read these only as "no large offset".
- In all four runs, with and without playback, each stream's first packet carried WASAPI's discontinuity flag at sample index 0. That flag has nothing before it to be discontinuous with, so it is now recorded as `start_discontinuity` rather than counted as a fault (summary schema version 3).
- With nothing playing, the loopback stream delivered no packets at all, and the session ended `timed_out` with the note that the playback stream delivered no frames. This is by design, and the Python prototype behaves the same: play something on the output while measuring it.

The output fields are described in [`summary-schema.md`](summary-schema.md).
