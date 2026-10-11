"""Public independent-oracle regressions; no game data or paid sessions."""
import copy
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
import wave

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('device_checks', ROOT / 'profiles/mhp3rd/tools/device_checks.py')
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


def png(width, height, rgb, mode=0, color=2):
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    channels = 3 if color == 2 else 4
    stride = width * channels
    previous, rows = bytes(stride), []
    for row in range(height):
        data = rgb[row * stride:(row + 1) * stride]
        encoded = []
        for i, value in enumerate(data):
            left, up, corner = (data[i - channels] if i >= channels else 0), previous[i], (previous[i - channels] if i >= channels else 0)
            estimate = left + up - corner
            paeth = min((left, up, corner), key=lambda x: abs(estimate - x))
            encoded.append((value - (0, left, up, (left + up) // 2, paeth)[mode]) & 255)
        rows.append(bytes([mode]) + bytes(encoded))
        previous = data
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, color, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')


class OracleTests(unittest.TestCase):
    def test_authored_colour_marker_rejects_wrong_channel_region_and_buffer(self):
        picture = (2, 2, bytes([240, 40, 40, 0, 255, 0, 240, 40, 40, 0, 0, 255]))
        self.assertEqual(checks.colour_count(picture, [0, 0, 2, 2], [240, 40, 40], 0), 2)
        self.assertEqual(checks.colour_count(picture, [1, 0, 1, 2], [240, 40, 40], 0), 0)
        self.assertEqual(checks.colour_count(picture, [0, 0, 2, 2], [40, 40, 240], 0), 0)
        for region in ([0, 0, 3, 2], [-1, 0, 1, 1], [0, 0, 0, 1], [True, 0, 1, 1]):
            with self.assertRaises(ValueError):
                checks.colour_count(picture, region, [240, 40, 40])
        for colour in ([300, 0, 0], [True, 0, 0], [0, 0]):
            with self.assertRaises(ValueError):
                checks.colour_count(picture, [0, 0, 1, 1], colour)
        with self.assertRaises(ValueError):
            checks.colour_count((2, 2, b''), [0, 0, 1, 1], [0, 0, 0])
        shaded = (2, 1, bytes([122, 4, 110, 0, 0, 0]))
        self.assertEqual(checks.colour_count(shaded, [0, 0, 2, 1], [244, 8, 220], 5, normalise=True), 1)
        self.assertEqual(checks.colour_count(shaded, [0, 0, 2, 1], [244, 8, 220], 5), 0)
        self.assertEqual(checks.colour_count(shaded, [0, 0, 2, 1], [8, 244, 220], 5, normalise=True), 0)
        with self.assertRaises(ValueError):
            checks.colour_count(shaded, [0, 0, 1, 1], [0, 0, 0], normalise=True)

    def test_comparisons_and_mutations(self):
        checks.compare(204, 'eq', 204)
        for wrong in (309, None, True, '204', 204.0):
            with self.assertRaises(checks.CheckFailed):
                checks.compare(wrong, 'eq', 204)
        for op in ('gt', 'ge', 'lt', 'le', 'ne'):
            checks.compare(2, op, 1 if op in ('gt', 'ge', 'ne') else 3)
            with self.assertRaises(checks.CheckFailed):
                checks.compare(2, op, 2 if op == 'ne' else 3 if op in ('gt', 'ge') else 1)
        with self.assertRaises(checks.CheckFailed):
            checks.compare(None, 'gt', 0)
        for value in (float('nan'), float('inf')):
            with self.assertRaises(checks.CheckFailed):
                checks.compare(value, 'eq', value)
        with self.assertRaises(checks.ValueError if hasattr(checks, 'ValueError') else ValueError):
            checks.compare(1, 'maybe', 1)
        self.assertEqual(checks.field({'settings': {'video.scale': '1'}}, 'settings.video.scale'), '1')
        self.assertEqual(checks.field({'a': [1, {'b': 7}]}, 'a.1.b'), 7)

    def test_files_preservation_and_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'data').mkdir()
            (root / 'data/save').write_bytes(b'public save fixture')
            before = checks.inventory(root / 'data')
            (root / 'data/save').write_bytes(b'wrong save')
            with self.assertRaises(checks.CheckFailed):
                checks.compare(checks.inventory(root / 'data'), 'eq', before)
            (root / 'settings.ini').write_text('# private settings\nvideo.scale=1\nvideo.scale=2\n')
            self.assertEqual(checks.settings(root / 'settings.ini'), {'video.scale': '2'})
            self.assertEqual(checks.confined(root, 'data/save'), (root / 'data/save').resolve())
            for name in ('../outside', str(root / 'data/save'), 'data\\save'):
                with self.assertRaises(ValueError):
                    checks.confined(root, name)
            (root / 'data/link').symlink_to(root / 'settings.ini')
            with self.assertRaises(checks.CheckFailed):
                checks.inventory(root / 'data')
            with self.assertRaises(ValueError):
                checks.confined(root, 'data/link')
            with self.assertRaises(checks.CheckFailed):
                checks.inventory(root / 'absent')
            (root / 'empty').mkdir()
            with self.assertRaises(checks.CheckFailed):
                checks.inventory(root / 'empty')
            (root / 'settings.ini').write_text('# no values\n')
            with self.assertRaises(checks.CheckFailed):
                checks.settings(root / 'settings.ini')

    def test_png_pixels_and_negative_outcomes(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'capture.png'
            rgb = bytes(range(192))
            for mode in range(5):
                path.write_bytes(png(8, 8, rgb, mode))
                self.assertEqual(checks.pixels(path), (8, 8, rgb))
            result = checks.image_content((8, 8, rgb))
            self.assertEqual(result['colors'], 64)
            with self.assertRaises(checks.CheckFailed):
                checks.image_content((8, 8, bytes(192)))
            self.assertEqual(checks.image_difference((8, 8, rgb), (8, 8, rgb)), 0)
            self.assertEqual(checks.image_difference((8, 8, rgb), (8, 8, bytes(192))), 1)
            with self.assertRaises(checks.CheckFailed):
                checks.image_difference((8, 8, rgb), (4, 16, rgb))
            rgba = bytes(value for i in range(64) for value in (*rgb[i * 3:i * 3 + 3], 255))
            path.write_bytes(png(8, 8, rgba, color=6))
            self.assertEqual(checks.pixels(path)[2], rgb)
            for bad in (b'garbage', png(8, 8, rgb)[:-1], png(8, 8, rgb)[:40],
                        png(8, 8, rgb)[:20] + b'wrong CRC', png(8, 8, rgb).replace(b'IHDR', b'BAD!')):
                path.write_bytes(bad)
                with self.assertRaises((checks.CheckFailed, struct.error)):
                    checks.pixels(path)
            for width, height in ((0, 1), (8193, 1), (8192, 8192)):
                with self.assertRaises(checks.CheckFailed):
                    checks.bounded_dimensions(width, height)

    def test_bmp_top_and_bottom_rows(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'capture.bmp'
            for bits in (24, 32):
                for height in (-2, 2):
                    raw = bytes([3, 2, 1, 0]) * 2 if bits == 32 else bytes([3, 2, 1, 0, 6, 5, 4, 0])
                    header = b'BM' + struct.pack('<IHHI', 54 + len(raw), 0, 0, 54) + struct.pack('<IiiHHIIIIII', 40, 1, height, 1, bits, 0, len(raw), 0, 0, 0, 0)
                    path.write_bytes(header + raw)
                    self.assertEqual(checks.pixels(path)[:2], (1, 2))
                    path.write_bytes(header + raw[:-1])
                    with self.assertRaises(checks.CheckFailed):
                        checks.pixels(path)

    def test_performance_identity_noise_speed_and_tail_mutations(self):
        identity = {key: 'same device/workload' for key in ('device', 'gpu', 'driver', 'resolution', 'execution_mode',
                    'route_sha256', 'fixture_sha256', 'settings_sha256', 'scenario_sha256')}
        base = {'identity': identity, 'artifact_sha256': 'a' * 64, 'warmup_frames': 60,
                'frame_ms': [33.3] * 200, 'speed_percent': [100] * 4, 'startup_ms': 4000, 'load_ms': [2000] * 3}
        old, new = [copy.deepcopy(base) for _ in range(3)], [copy.deepcopy(base) for _ in range(3)]
        budget = {'p95_ms': 1, 'p99_ms': 1, 'startup_ms': 100, 'load_ms': 100, 'speed_percent': 1}
        self.assertTrue(checks.performance_comparison(old, new, identity, budget)['threshold_enabled'])
        self.assertFalse(checks.performance_comparison(old, new, identity)['threshold_enabled'])
        for mutate in (lambda r: r['identity'].update(gpu='other GPU'), lambda r: r.update(warmup_frames=0),
                       lambda r: r.update(frame_ms=[33]), lambda r: r.update(startup_ms=0), lambda r: r.update(load_ms=[])):
            wrong = copy.deepcopy(new)
            mutate(wrong[0])
            with self.assertRaises(checks.CheckFailed):
                checks.performance_comparison(old, wrong, identity, budget)
        for metric, wrong in (('speed_percent', [80] * 4), ('frame_ms', [50] * 200), ('load_ms', [3000] * 3), ('startup_ms', 5000)):
            mutated = copy.deepcopy(new)
            for run in mutated:
                run[metric] = wrong
                run['presented_fps'] = 120
            with self.assertRaises(checks.CheckFailed):
                checks.performance_comparison(old, mutated, identity, budget)
        old[0]['startup_ms'] = 4300
        with self.assertRaises(checks.CheckFailed):
            checks.performance_comparison(old, new, identity, budget)
        with self.assertRaises(checks.CheckFailed):
            checks.performance_comparison(old[:2], new, identity)
        with self.assertRaises(checks.CheckFailed):
            checks.performance_comparison(old, new, {})
        for invalid in ([], [float('nan')], [-1], [True]):
            with self.assertRaises(checks.CheckFailed):
                checks.percentile(invalid, .95)


class ExtendedOracles(unittest.TestCase):
    def test_nested_boolean_and_nonfinite_mutations(self):
        checks.compare({'armor': [171, 165]}, 'eq', {'armor': [171, 165]})
        for actual in ({'armor': [171, True]}, {'armor': [171, float('nan')]}, {'armor': [171, float('inf')]}):
            with self.assertRaises(checks.CheckFailed):
                checks.compare(actual, 'eq', {'armor': [171, 165]})

    def test_pcm_accepts_signal_and_rejects_silence_clipping_and_wrong_format(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'audio.wav'
            def write(samples, channels=2, rate=44100):
                with wave.open(str(path), 'wb') as stream:
                    stream.setnchannels(channels)
                    stream.setsampwidth(2)
                    stream.setframerate(rate)
                    stream.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
            write([1000, -1000] * 44100)
            self.assertEqual(checks.audio_content(path)['nonzero_samples'], 88200)
            for samples, channels, rate in (([0] * 88200, 2, 44100), ([32767] * 88200, 2, 44100),
                                             ([1000] * 88200, 1, 44100), ([1000] * 88200, 2, 22050),
                                             ([1000] * 100, 2, 44100)):
                write(samples, channels, rate)
                with self.assertRaises(checks.CheckFailed):
                    checks.audio_content(path)


if __name__ == '__main__':
    unittest.main()
