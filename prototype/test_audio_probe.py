import copy
import unittest
import numpy as np
from audio_probe import levels, routing_errors, select_input


def good_state():
    inputs = []
    for device, routes in [('Microphone (PRO X 2 LIGHTSPEED)', (0, 1, 0)),
                           ('Loopback 3 + 4 (Focusrite USB Audio)', (1, 0, 1)),
                           ('', (0, 0, 0)), ('Virtual', (0, 0, 0)), ('Virtual', (0, 0, 0))]:
        inputs.append(dict(device=device, A1=routes[0], B1=routes[1], B2=routes[2],
                           A2=0, A3=0, Mute=0, Gain=0))
    return dict(type=2, inputs=inputs, hardware_output='Logi_hds (PRO X 2 LIGHTSPEED)',
                buses=[dict(index=i, Mute=0) for i in [0, 3, 4]])


class ProbeTests(unittest.TestCase):
    def test_requested_routing(self):
        self.assertEqual(routing_errors(good_state()), [])

    def test_standard_rejected(self):
        self.assertTrue(routing_errors({'type': 1}))

    def test_crossfeed_rejected(self):
        state = good_state()
        state['inputs'][0]['B2'] = 1
        self.assertTrue(routing_errors(state))

    def test_wrong_source_rejected(self):
        state = good_state()
        state['inputs'][1]['device'] = 'Analogue 1 + 2 (Focusrite USB Audio)'
        self.assertTrue(routing_errors(state))

    def test_unexpected_monitor_rejected(self):
        state = good_state()
        state['inputs'][3]['A2'] = 1
        self.assertTrue(routing_errors(state))

    def test_muted_bus_rejected(self):
        state = good_state()
        state['buses'][2]['Mute'] = 1
        self.assertTrue(routing_errors(state))

    def test_exact_endpoint_selection(self):
        apis = [{'name': 'Windows WASAPI'}, {'name': 'MME'}]
        device = dict(name='B1', max_input_channels=2, hostapi=0)
        self.assertEqual(select_input([device], apis, 'B1')[0], 0)
        for devices in [[], [device, copy.copy(device)], [dict(device, hostapi=1)],
                        [dict(device, name='B10')]]:
            with self.assertRaises(RuntimeError):
                select_input(devices, apis, 'B1')

    def test_levels(self):
        self.assertEqual(levels(np.zeros((48, 2)))['peak_dbfs'], -180.0)
        self.assertAlmostEqual(levels(np.full((48, 2), 0.5))['rms_dbfs'], -6.0)


if __name__ == '__main__':
    unittest.main()
