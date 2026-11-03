"""Bounded PRO X 2 mic + playback capture; scalar levels only, no routing writes."""

import argparse
from contextlib import ExitStack
import json
import threading
import time

import numpy as np
import pyaudiowpatch as pa

from audio_probe import levels

MIC = 'Microphone (PRO X 2 LIGHTSPEED)'
OUTPUT = 'Logi_hds (PRO X 2 LIGHTSPEED)'


def select(devices, host_index, name, direction, loopback=False):
    matches = [d for d in devices if d['hostApi'] == host_index
               and d['name'] == name and d[direction] > 0
               and bool(d['isLoopbackDevice']) == loopback]
    if len(matches) != 1:
        raise RuntimeError(f'Expected one exact endpoint for {name}; got {len(matches)}')
    return matches[0]


def inspect(manager):
    host = manager.get_host_api_info_by_type(pa.paWASAPI)
    devices = list(manager.get_device_info_generator())
    mic = select(devices, host['index'], MIC, 'maxInputChannels')
    output = select(devices, host['index'], OUTPUT, 'maxOutputChannels')
    loopback = select(devices, host['index'], OUTPUT + ' [Loopback]',
                      'maxInputChannels', True)
    defaults = {
        'input': manager.get_device_info_by_index(host['defaultInputDevice'])['name'],
        'output': manager.get_device_info_by_index(host['defaultOutputDevice'])['name'],
    }
    return {'defaults': defaults, 'selected': {'mic': mic, 'system': loopback},
            'playback': output}


class Meter:
    def __init__(self, channels):
        self.channels = channels
        self.lock = threading.Lock()
        self.blocks = self.frames = self.warnings = 0
        self.peak = -180.0
        self.latest_rms = -180.0
        self.channel_peaks = np.zeros(channels)
        self.error = None

    def callback(self, data, frames, timing, status):
        try:
            samples = np.frombuffer(data, dtype=np.float32).reshape(-1, self.channels)
            measured = levels(samples)
            peaks = np.max(np.abs(samples), axis=0) if samples.size else np.zeros(self.channels)
            # Retain only scalar meters, never audio or speech content.
            with self.lock:
                self.blocks += 1
                self.frames += frames
                self.warnings += bool(status)
                self.peak = max(self.peak, measured['peak_dbfs'])
                self.latest_rms = measured['rms_dbfs']
                self.channel_peaks = np.maximum(self.channel_peaks, peaks)
            return None, pa.paContinue
        except Exception as error:
            with self.lock:
                self.error = str(error)
            return None, pa.paAbort

    def snapshot(self):
        with self.lock:
            return {'blocks': self.blocks, 'frames': self.frames,
                    'peak_dbfs': self.peak, 'latest_rms_dbfs': self.latest_rms,
                    'channel_peak_dbfs': np.round(20 * np.log10(
                        np.maximum(self.channel_peaks, 1e-9)), 1).tolist(),
                    'warnings': self.warnings, 'error': self.error}


def meter(manager, report, seconds, coexistence=False):
    endpoints = dict(report['selected'])
    if coexistence:
        endpoints['mic_second_client'] = report['selected']['mic']
    meters = {label: Meter(d['maxInputChannels']) for label, d in endpoints.items()}
    with ExitStack() as stack:
        streams = []
        for label, d in endpoints.items():
            stream = manager.open(format=pa.paFloat32, input=True,
                                  input_device_index=d['index'],
                                  channels=d['maxInputChannels'],
                                  rate=int(d['defaultSampleRate']),
                                  frames_per_buffer=1024, start=False,
                                  stream_callback=meters[label].callback)
            stack.callback(stream.close)
            streams.append(stream)
        print(f'ACTIVE: {seconds}s local meters only; no recording/upload/transcription.', flush=True)
        for stream in streams:
            stream.start_stream()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not all(s.is_active() for s in streams):
                raise RuntimeError('Capture stopped unexpectedly')
            print(json.dumps({k: v.snapshot() for k, v in meters.items()}), flush=True)
            time.sleep(min(1, max(0, deadline - time.monotonic())))
        for stream in streams:
            stream.stop_stream()
    summary = {k: v.snapshot() for k, v in meters.items()}
    print(json.dumps({'state': 'stopped', 'summary': summary}), flush=True)
    if any(row['error'] for row in summary.values()):
        raise RuntimeError('A callback failed')
    if not summary['mic']['frames']:
        raise RuntimeError('Microphone returned no frames')
    if not summary['system']['frames']:
        print('INCONCLUSIVE: no playback frames; silence may produce no loopback callbacks.')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['inspect', 'meter'])
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--coexistence', action='store_true',
                        help='Also open a second shared-mode mic client')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error('Duration must be 1-60 seconds')
    with pa.PyAudio() as manager:
        report = inspect(manager)
        print(json.dumps(report, indent=2), flush=True)
        if args.mode == 'meter':
            meter(manager, report, args.seconds, args.coexistence)
    if args.mode == 'meter':
        # Re-enumerate with a new PortAudio lifetime; defaults are never written.
        with pa.PyAudio() as manager:
            after = inspect(manager)['defaults']
        print(json.dumps({'defaults_after': after,
                          'defaults_unchanged': after == report['defaults']}))


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('STOPPED by user; capture streams closed.')
        raise SystemExit(130)
    except Exception as error:
        print(f'ERROR: {error}', flush=True)
        raise SystemExit(1)
