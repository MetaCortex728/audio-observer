# Design rules

The rules audio-observer follows when it captures audio. The README gives a one-line summary.

## Privacy rule

**No samples are written, exported or sent anywhere.** Audio is used in memory to compute numbers and then discarded. There are no audio files, uploads, transcription, speech models, background capture or autostart. The JSON output holds scalar figures and short strings only; the JSON writer has no way to write an array, and a test checks every command's output for this. The tool never changes default devices, routing, volumes or exclusive mode, and it only ever opens capture streams.

Note that a loopback stream carries everything played on that output, including notifications and voice calls. The figures it produces are levels and counts, but close sources you do not want included before a run.
