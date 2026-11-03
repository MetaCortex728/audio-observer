# audio-observer

A tool that captures several audio streams at once and diagnoses their health and timing. It never records, transcribes or uploads audio: samples are used in memory to compute numbers and then discarded.

## Origin

Han developed the first version locally in October 2026 with OpenAI Codex as a coding assistant, without version control. It is a Windows Python prototype that reads a USB headset's microphone and its playback loopback side by side through WASAPI and reports levels for a bounded time. This repository starts from that prototype, kept as it was in `prototype/`, and continues it as a C++17 library and command-line tool.

The first commit imports the prototype unchanged except for two README sentences that referred to people and tools outside this project.
