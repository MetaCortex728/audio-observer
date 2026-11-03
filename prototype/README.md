# Audio observer: direct PRO X 2 capture

## Current design and verification (2026-10-10)

The Focusrite/Banana architecture below is superseded. The requirement changed: normal
USB-headset listening and microphone use must remain independent of the observer.
Use `direct_probe.py`, not the legacy Voicemeeter probe, for current testing.
The unused routing writer was removed without being executed.

```text
Apps -> PRO X 2 USB playback -> headset
                     +-> WASAPI loopback -> system stream
PRO X 2 microphone -> existing apps
                     +-> shared-mode capture -> mic stream
```

No routing or Windows-default writes. No Focusrite or Voicemeeter dependency.
These are two capture streams, not newly published Windows virtual devices.

```powershell
.\.venv\Scripts\python.exe direct_probe.py inspect
.\.venv\Scripts\python.exe direct_probe.py meter --seconds 10 --coexistence
.\.venv\Scripts\python.exe -m unittest -v
```

`inspect` only enumerates endpoints. `meter` runs 1-60 seconds, retains only scalar
levels, and closes on completion or Ctrl+C. `--coexistence` opens a second mic
client. No audio files, uploads, transcription, model calls, autostart, service,
or chat delivery. This standalone script has no voice-session dependency, but a
dedicated voice-off comparison has not yet been run.

Verified ten-second run: mono mic and 8-channel headset loopback at 48 kHz;
480256 mic frames, 479232 system frames and 479232 second-mic-client frames.
Peaks were -9.8 dBFS (mic) and -9.3 dBFS (system); playback activity appeared in
the first two channels. All streams reported zero warnings and closed normally.
WASAPI defaults were PRO X 2 input/output both before and after. Twelve unit
tests passed (four direct-capture tests, eight legacy tests).

This verifies concurrent signal capture, not recognition of music/speech content,
perfect isolation, sample synchronization, or long-run stability. User confirmation
of unaffected listening and existing-app mic use remains pending, as do deliberate
mic-only/playback-only checks. Loopback includes all sound rendered to the headset,
including notifications and voice playback. Close unwanted sources before a test.
Exact endpoint matching fails closed; it does not substitute another device.

Next: verify normal audio behavior and off-voice use, then choose transcription
location, retention, and explicit start/pause/stop controls. No paid API or speech
model has been configured.

References: https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
and https://github.com/s0d3s/PyAudioWPatch

## Historical proposal below (not current setup instructions)

Target: Focusrite system playback -> Loopback -> Banana Input 2 -> B2;
PRO X 2 microphone -> Banana Input 1 -> B1. The sources stay separate.
Windows default input/output are not modified by this tool.

## Current scope

- `inspect` reads device names and Voicemeeter route settings, without opening audio streams.
- `meter --seconds 10` opens two WASAPI input streams for a bounded level test.
- No raw audio files, network upload, transcription, model calls, chat delivery,
  autostart, hidden background capture, or routing writes are implemented.
- Ctrl+C stops the meter and closes both streams. Maximum duration: 60 seconds.
- Level events have local receipt timestamps, not sample-accurate synchronization
  across the two independent devices.
- The meter does not prove source isolation automatically. Test mic-only and
  playback-only separately and watch for activity on the wrong bus.
- The route check cannot verify Focusrite's Direct Monitor preference or
  per-application playback overrides. Inspect those separately.

## Setup

Use Python 3.12+ with the dependencies in `requirements.txt` in a local `.venv`.
Run from this directory:

```powershell
.\.venv\Scripts\python.exe audio_probe.py inspect
.\.venv\Scripts\python.exe -m unittest -v
.\.venv\Scripts\python.exe audio_probe.py meter --seconds 10
```

The tool requires Banana and fails closed for other editions. Installed B2
endpoints alone do not prove that Banana is running. Select the actual headset
output (currently named Logi_hds), not an assumed Speakers label.

Focusrite Notifier -> Expose/Hide Windows Channels must expose Loopback 3-4.
Focusrite Control 2 -> Preferences -> Send Direct Monitor Mix to Loopback stays OFF.
Keep the Windows default output on Focusrite and default input on PRO X 2.

| Banana strip | A1 | A2/A3 | B1 | B2 |
| --- | --- | --- | --- | --- |
| Input 1: PRO X 2 mic | Off | Off | On | Off |
| Input 2: Focusrite Loopback | On | Off | Off | On |
| Input 3, VAIO, AUX | Off | Off | Off | Off |

A1 is the PRO X 2 headset. No output is routed back to Focusrite playback.
All apps playing to Focusrite, including notifications, enter the system stream.
Before a test, close private audio sources you do not want included.

## Next stage

After physical routing passes the two isolation tests, choose local transcription
or explicitly authorized cloud transcription and define retention. Do not treat
this meter as continuous listening or a transcription service.

## References

- https://support.focusrite.com/hc/en-gb/articles/360010711620-Expose-Hide-Windows-Channels-for-non-ASIO-Windows-apps
- https://support.focusrite.com/hc/en-gb/articles/13229216604562-How-to-use-Loopback-in-Focusrite-Control-2
- https://vb-audio.com/Voicemeeter/banana.htm
- https://download.vb-audio.com/Download_CABLE/VoicemeeterRemoteAPI.pdf
- https://python-sounddevice.readthedocs.io/en/latest/api/streams.html
