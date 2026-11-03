"""Local-only, bounded B1/B2 meter. No recording, network or routing writes."""

import argparse
from contextlib import ExitStack
import ctypes
import json
import math
import os
from pathlib import Path
import queue
import time


def select_input(devices, apis, name):
    matches = [
        (i, d) for i, d in enumerate(devices)
        if d['name'] == name and d['max_input_channels'] >= 2
        and apis[d['hostapi']]['name'] == 'Windows WASAPI'
    ]
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one stereo WASAPI input: {name}; found {len(matches)}')
    return matches[0]


def routing_errors(state):
    errors = []
    if state['type'] != 2:
        errors.append('This probe requires Voicemeeter Banana; switch from Standard first.')
        return errors
    if 'PRO X 2' not in state['inputs'][0]['device'].upper():
        errors.append('Hardware Input 1 is not the PRO X 2 microphone.')
    second = state['inputs'][1]['device'].lower()
    if 'focusrite' not in second or 'loopback' not in second:
        errors.append('Hardware Input 2 is not a named Focusrite Loopback input.')
    if 'PRO X 2' not in state['hardware_output'].upper():
        errors.append('Hardware Out A1 is not the PRO X 2 headset.')
    expected = [(0, 1, 0), (1, 0, 1), (0, 0, 0), (0, 0, 0), (0, 0, 0)]
    for index, (strip, routes) in enumerate(zip(state['inputs'], expected)):
        actual = tuple(int(strip[key]) for key in ('A1', 'B1', 'B2'))
        if actual != routes:
            errors.append(f'Strip {index + 1}: expected A1/B1/B2={routes}, got {actual}.')
        if strip['A2'] or strip['A3']:
            errors.append(f'Strip {index + 1}: unexpected A2/A3 route is enabled.')
        if index < 2 and strip['Mute']:
            errors.append(f'Strip {index + 1} is muted.')
    for bus in state['buses']:
        if bus['Mute']:
            errors.append(f"Bus {bus['index']} is muted.")
    return errors


def read_voicemeeter():
    if os.name != 'nt':
        raise RuntimeError('Voicemeeter inspection requires Windows.')
    filename = 'VoicemeeterRemote64.dll' if ctypes.sizeof(ctypes.c_void_p) == 8 else 'VoicemeeterRemote.dll'
    dll = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'VB' / 'Voicemeeter' / filename
    api = ctypes.WinDLL(str(dll))
    signatures = {
        'Login': [], 'Logout': [], 'IsParametersDirty': [],
        'GetVoicemeeterType': [ctypes.POINTER(ctypes.c_int32)],
        'GetVoicemeeterVersion': [ctypes.POINTER(ctypes.c_int32)],
        'GetParameterFloat': [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)],
        'GetParameterStringA': [ctypes.c_char_p, ctypes.c_void_p],
    }
    for name, arguments in signatures.items():
        function = getattr(api, 'VBVMR_' + name)
        function.argtypes, function.restype = arguments, ctypes.c_int32

    def checked(code, operation):
        if code < 0:
            raise RuntimeError(f'{operation}: Voicemeeter API error {code}')

    def number(name):
        value = ctypes.c_float()
        checked(api.VBVMR_GetParameterFloat(name.encode('ascii'), ctypes.byref(value)), name)
        return value.value

    def string(name):
        value = ctypes.create_string_buffer(512)
        checked(api.VBVMR_GetParameterStringA(name.encode('ascii'), value), name)
        return value.value.decode('mbcs', errors='replace')

    checked(api.VBVMR_Login(), 'login')
    try:
        time.sleep(0.2)
        checked(api.VBVMR_IsParametersDirty(), 'read parameters')
        kind, version = ctypes.c_int32(), ctypes.c_int32()
        checked(api.VBVMR_GetVoicemeeterType(ctypes.byref(kind)), 'type')
        checked(api.VBVMR_GetVoicemeeterVersion(ctypes.byref(version)), 'version')
        count = {1: 3, 2: 5, 3: 8}.get(kind.value)
        if count is None:
            raise RuntimeError('No supported Voicemeeter instance detected.')
        physical = {1: 2, 2: 3, 3: 5}[kind.value]
        inputs = []
        fields = ['A1', 'B1', 'Mute', 'Gain']
        if kind.value >= 2:
            fields += ['A2', 'A3', 'B2']
        for i in range(count):
            strip = {key: number(f'Strip[{i}].{key}') for key in fields}
            strip['device'] = string(f'Strip[{i}].device.name') if i < physical else 'Virtual input'
            inputs.append(strip)
        bus_indices = [0, 3, 4] if kind.value == 2 else [0]
        return {
            'type': kind.value,
            'version': '.'.join(str((version.value >> shift) & 255) for shift in (24, 16, 8, 0)),
            'inputs': inputs,
            'hardware_output': string('Bus[0].device.name'),
            'buses': [{'index': i, 'Mute': number(f'Bus[{i}].Mute')} for i in bus_indices],
        }
    finally:
        api.VBVMR_Logout()


def levels(samples):
    import numpy as np
    peak = float(np.max(np.abs(samples))) if samples.size else 0.0
    rms = float(np.sqrt(np.mean(np.square(samples, dtype=np.float64)))) if samples.size else 0.0
    return {'peak_dbfs': round(20 * math.log10(max(peak, 1e-9)), 1),
            'rms_dbfs': round(20 * math.log10(max(rms, 1e-9)), 1)}


def inspect(sd):
    devices, apis = list(sd.query_devices()), list(sd.query_hostapis())
    state = read_voicemeeter()
    errors = routing_errors(state)
    selected = {}
    for label, name in [('mic', 'Voicemeeter Out B1 (VB-Audio Voicemeeter VAIO)'),
                        ('system', 'Voicemeeter Out B2 (VB-Audio Voicemeeter VAIO)')]:
        try:
            selected[label] = select_input(devices, apis, name)
        except RuntimeError as error:
            errors.append(str(error))
    endpoints = [{'index': i, 'name': d['name'], 'inputs': d['max_input_channels'],
                  'outputs': d['max_output_channels'], 'sample_rate': d['default_samplerate']}
                 for i, d in enumerate(devices) if apis[d['hostapi']]['name'] == 'Windows WASAPI']
    return {'voicemeeter': state, 'wasapi_endpoints': endpoints,
            'routing_ready': not errors, 'blockers': errors}, selected


def meter(sd, selected, seconds):
    events = queue.Queue(maxsize=64)
    drops = {'mic': 0, 'system': 0}

    def callback_for(label):
        def callback(samples, frames, timing, status):
            # Only scalar levels leave the callback; no audio is retained.
            event = {'source': label, 'received_monotonic': time.monotonic(),
                     'status': str(status), **levels(samples)}
            try:
                events.put_nowait(event)
            except queue.Full:
                drops[label] += 1
        return callback

    summary = {label: {'blocks': 0, 'peak_dbfs': -180.0, 'stream_warnings': 0}
               for label in selected}
    with ExitStack() as stack:
        streams = []
        for label, (index, device) in selected.items():
            streams.append(stack.enter_context(sd.InputStream(
                device=index, channels=2, samplerate=device['default_samplerate'],
                dtype='float32', blocksize=0, latency='high',
                callback=callback_for(label))))
        print(f'ACTIVE: local level meter for {seconds}s. No files, uploads or transcription. Ctrl+C stops.', flush=True)
        end, next_display = time.monotonic() + seconds, 0.0
        while time.monotonic() < end:
            if not all(stream.active for stream in streams):
                raise RuntimeError('A capture stream stopped unexpectedly.')
            try:
                event = events.get(timeout=0.2)
            except queue.Empty:
                continue
            row = summary[event['source']]
            row['blocks'] += 1
            row['peak_dbfs'] = max(row['peak_dbfs'], event['peak_dbfs'])
            row['stream_warnings'] += bool(event['status'])
            row['latest_rms_dbfs'] = event['rms_dbfs']
            if time.monotonic() >= next_display:
                print(json.dumps(summary), flush=True)
                next_display = time.monotonic() + 1.0
    print(json.dumps({'state': 'stopped', 'summary': summary, 'dropped_meter_events': drops}))
    if any(not row['blocks'] for row in summary.values()):
        raise RuntimeError('No samples received from at least one source.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['inspect', 'meter'])
    parser.add_argument('--seconds', type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error('--seconds must be between 1 and 60')
    import sounddevice as sd
    report, selected = inspect(sd)
    print(json.dumps(report, indent=2))
    if args.mode == 'inspect':
        return 0
    if not report['routing_ready']:
        raise RuntimeError('Routing is not ready. No audio capture started.')
    meter(sd, selected, args.seconds)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print('STOPPED by user; audio streams closed.')
        raise SystemExit(130)
    except Exception as error:
        print(f'ERROR: {error}')
        raise SystemExit(1)
