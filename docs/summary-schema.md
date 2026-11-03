# JSON output schema

`audio-observer` writes one JSON object to standard output per command. Messages (the `ACTIVE:` notice, errors, the stop notice) go to standard error, so standard output is always a single JSON document.

The output holds **scalars and short strings only**. The JSON writer (`include/audio_observer/json_writer.hpp`) has no way to write an array, and it cuts strings at 240 bytes. Lists, such as channels or glitch events, are objects keyed `"0"`, `"1"`, and so on, and their length depends on the device (channel count) or is capped (events), never on how long the session ran. A test checks this on every CLI output. Samples are used in memory to compute these figures and then discarded.

Decibel figures are dBFS with two decimals: a sample of magnitude 1.0 is 0 dBFS, and silence is reported as `-180.00`, as in the prototype. Times are seconds. Frame counts and sample positions count frames (one sample per channel) from the start of the stream.

## Schema versions

- `audio-observer.summary/3`: adds `start_discontinuity` to `dropouts`. A discontinuity flagged on a stream's first delivered block is recorded there instead of being counted in `discontinuities`, `total` and `warnings`. In version 2 it was counted, so every WASAPI stream reported one discontinuity at sample index 0.
- `audio-observer.summary/2`: adds the session state `"timed_out"`. In version 1 a session ended by the wall-clock safety limit was reported as `"completed"` even though some stream had delivered fewer frames than requested.
- `audio-observer.summary/1`: first version.

## `meter`: session summary (`audio-observer.summary/3`)

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | string | `"audio-observer.summary/3"` |
| `tool_version` | string | Version of `audio-observer` |
| `backend` | string | `"wasapi"` or `"synthetic"` |
| `retention` | string | Reminder that only scalar figures were kept |
| `state` | string | `"completed"` (every stream delivered its frames or ended normally), `"timed_out"` (the wall-clock safety limit passed first, so the capture is short), `"stopped"` (Ctrl+C) or `"failed"` |
| `error` | string or null | Why the session failed or timed out, e.g. which streams were short when the safety limit passed, an endpoint that did not match exactly once, a stream that failed, or `"Microphone returned no frames"` |
| `note` | string or null | Caveat, e.g. that a playback stream delivered no frames, which is inconclusive because silence may produce no loopback callbacks |
| `coexistence` | bool | Whether a second microphone client was opened |
| `seconds_requested` | number | Requested duration, 1–60 |
| `elapsed_seconds` | number | Wall-clock time from opening the first stream to stopping |
| `streams` | object | One member per opened stream, keyed by label (below) |

Stream labels follow the prototype: `mic` (capture of `--mic`), `system` (loopback of `--output`) and, with `--coexistence`, `mic_second_client` (a second client on the same microphone, which shows that capture coexists with other applications). Streams that could not be opened are absent; `error` says why.

### Per stream

| Field | Type | Meaning |
| --- | --- | --- |
| `endpoint` | string | Exact endpoint name |
| `direction` | string | `"capture"` or `"loopback"` |
| `sample_rate_hz` | integer | Nominal sample rate |
| `channels` | integer | Channel count |
| `sample_type` | string | Device sample type: `"float32"`, `"int16"`, `"int24"` or `"int32"` |
| `state` | string | Final stream state: `"stopped"`, `"finished"` (a finite source ended), `"failed"` |
| `error` | string or null | Stream failure |
| `blocks` | integer | Blocks metered, including a truncated last block |
| `frames` | integer | Frames metered. A session that completes normally meters exactly `seconds_requested` × `sample_rate_hz` frames (rounded) per stream: the block that crosses that position is truncated and later blocks are discarded, so the count does not depend on scheduler granularity. It is lower when the stream ended, failed or was stopped first, or when the wall-clock safety limit (`seconds_requested` + 2 s) passed before the stream delivered enough frames; the session `state` is then `"timed_out"` |
| `warnings` | integer | Blocks the device flagged (discontinuity or timestamp error), like the prototype's callback status count. A discontinuity on the stream's first block is not counted (see `start_discontinuity`) |
| `peak_dbfs` | number | Highest sample magnitude over all channels |
| `rms_dbfs` | number | RMS over all channels and the whole session |
| `peak_hold_dbfs` | number | Peak-hold meter at the end (1 s hold, then 20 dB/s decay) |
| `clipped_samples` | integer | Samples with magnitude ≥ 0.999 |
| `silent` | bool | All channels below −60 dBFS for at least 0.5 s at the end |
| `silence_count` | integer | Times the stream became silent |
| `per_channel` | object | Keyed `"0"`, `"1"`, …; each holds the six level fields above for one channel |
| `dropouts` | object | Dropout and glitch figures (below) |

### `dropouts`

| Field | Type | Meaning |
| --- | --- | --- |
| `total` | integer | Sum of the counts below (gaps, overlaps, discontinuities, overruns, timestamp errors, time regressions) |
| `gaps`, `gap_frames` | integer | Jumps forward in the device's sample positions, and the frames missing |
| `overlaps`, `overlap_frames` | integer | Jumps backward, and the frames repeated |
| `discontinuities` | integer | Blocks after the stream's first that the device flagged as discontinuous |
| `start_discontinuity` | bool | Whether the stream's first delivered block was flagged as discontinuous. There is no earlier block for it to be discontinuous with, so it is not a glitch: it is kept here, not counted in `discontinuities`, `total` or `warnings`, and has no event. If buffer overruns dropped blocks before the first delivered block, that block's flag counts as a normal discontinuity |
| `overruns`, `overrun_frames` | integer | Blocks dropped because the consumer fell behind, and their frames. These are not counted as gaps. |
| `unplaced_overruns` | integer | Overruns after the last received block, which have no position |
| `timestamp_errors` | integer | Blocks the device flagged as having an unreliable timestamp |
| `time_regressions` | integer | Blocks whose host time is earlier than the previous block's |
| `rate_offset_ppm` | number or null | Rough device clock rate against the host clock, from the first and last blocks; overlaps and timestamp jitter bias it. A proper drift estimate is Stage 2 work. |
| `events_omitted` | integer | Events beyond the first 16, counted but not listed |
| `events` | object | The first 16 events, keyed `"0"`, `"1"`, … in arrival order |

Each event has:

| Field | Type | Meaning |
| --- | --- | --- |
| `kind` | string | `"gap"`, `"overlap"`, `"discontinuity"`, `"overrun"`, `"timestamp_error"` or `"time_regression"` |
| `sample_index` | integer | First missing, repeated or dropped frame, or the first frame of the flagged block |
| `frames` | integer | Frames missing, repeated or dropped (0 for flags) |
| `stream_seconds` | number | `sample_index` relative to the first received frame, at the nominal rate |
| `host_seconds` | number | Host time of the block that revealed the event, relative to the first block's host time |

### With the `wasapi` backend

- Sample positions are the device position that WASAPI reports for each packet (`u64DevicePosition`), so gaps and overlaps are what the device's own clock says.
- Host times are the performance-counter time at which the device captured a packet's first frame (`u64QPCPosition`), moved onto the host steady clock.
- `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` counts as a discontinuity and `AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR` as a timestamp error; both count as `warnings`. WASAPI commonly sets `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` on the first packet after a stream starts; on real hardware it did so on every capture and loopback stream, so expect `start_discontinuity` to be `true`. Packets flagged `AUDCLNT_BUFFERFLAGS_SILENT` are metered as zeros and are not warnings, since silence is normal.
- A loopback stream receives nothing while its playback endpoint plays nothing; on real hardware, a session with nothing playing ended `"timed_out"` with the note that the playback stream delivered no frames. If playback stops part way through, the stream may not reach its frame count before the safety limit (`"timed_out"`), and whether the pause also shows as a gap depends on how the device position advances meanwhile. Playback stopping part way through has not yet been checked on real hardware.

## `inspect`: endpoint listing (`audio-observer.inspect/2`)

`inspect` opens no streams. It lists the backend's endpoints and checks that `--mic` and `--output` each match exactly one endpoint; it exits with status 1 if either does not. With the `wasapi` backend, only the names given are checked, so `inspect` with no names simply lists the endpoints; listing reads each endpoint's shared-mode mix format but initializes no stream. With the `synthetic` backend, missing names default to the synthetic endpoints.

Version 2 (`audio-observer.inspect/2`) may omit `mic` or `system` from `selected` when no name was given; in version 1 both were always present.

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | string | `"audio-observer.inspect/2"` |
| `tool_version`, `backend` | string | As above |
| `endpoints` | object | Keyed `"0"`, `"1"`, …; each has `name`, `direction` (`"input"` or `"output"`), `loopback` (bool: an output whose sound can be captured), `sample_rate_hz`, `channels` and `sample_type` |
| `selected` | object | `mic` and `system` (each only when checked, see above), each with `name`, `direction`, `ok` (bool) and `error` (string or null, naming the match count) |

## Exit status

| Status | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Failure: endpoint not matched exactly, stream failure, session timed out (short capture), no microphone frames, backend not available on this platform |
| 2 | Usage error, including a duration outside 1–60 seconds |
| 130 | Stopped by Ctrl+C; every stream was closed |
