import unittest
import numpy as np
import pyaudiowpatch as pa

from direct_probe import Meter, select


class DirectProbeTests(unittest.TestCase):
    def test_select_exact_host_and_name(self):
        good = dict(hostApi=2, name='Headset', maxInputChannels=2, isLoopbackDevice=True)
        self.assertEqual(select([good], 2, 'Headset', 'maxInputChannels', True), good)
        for devices in ([], [good, good], [dict(good, hostApi=1)],
                        [dict(good, isLoopbackDevice=False)],
                        [dict(good, name='Headset other')]):
            with self.assertRaises(RuntimeError):
                select(devices, 2, 'Headset', 'maxInputChannels', True)

    def test_channels_remain_distinct(self):
        meter = Meter(2)
        samples = np.array([[0.5, 0], [-0.5, 0]], dtype=np.float32)
        self.assertEqual(meter.callback(samples.tobytes(), 2, {}, 0), (None, pa.paContinue))
        result = meter.snapshot()
        self.assertEqual(result['frames'], 2)
        self.assertEqual(result['channel_peak_dbfs'], [-6.0, -180.0])
        self.assertEqual(result['warnings'], 0)

    def test_bad_buffer_aborts(self):
        meter = Meter(2)
        self.assertEqual(meter.callback(b'x', 1, {}, 0), (None, pa.paAbort))
        self.assertIsNotNone(meter.snapshot()['error'])

    def test_silence_and_warning(self):
        meter = Meter(1)
        meter.callback(np.zeros(10, dtype=np.float32).tobytes(), 10, {}, 2)
        result = meter.snapshot()
        self.assertEqual(result['peak_dbfs'], -180.0)
        self.assertEqual(result['warnings'], 1)


if __name__ == '__main__':
    unittest.main()
