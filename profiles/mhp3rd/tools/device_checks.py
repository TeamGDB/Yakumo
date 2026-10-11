"""Independent, bounded oracles for private device evidence (no game fixtures)."""
import hashlib
import math
import os
from pathlib import Path
import statistics
import struct
import wave
import zlib


class CheckFailed(ValueError):
    pass


def audio_content(path, minimum_seconds=.5):
    """Check decoded guest PCM; acoustic quality remains a human sign-off."""
    if Path(path).stat().st_size > 64 * 1024 * 1024:
        raise CheckFailed('Oversized audio evidence')
    with wave.open(str(path), 'rb') as stream:
        if stream.getnchannels() != 2 or stream.getsampwidth() != 2 or stream.getframerate() != 44100:
            raise CheckFailed('Expected 44100 Hz stereo signed 16-bit PCM')
        count = stream.getnframes()
        if count < minimum_seconds * 44100:
            raise CheckFailed('Insufficient audio samples')
        data = stream.readframes(count)
    if len(data) != count * 4:
        raise CheckFailed('Truncated PCM evidence')
    samples = [item[0] for item in struct.iter_unpack('<h', data)]
    nonzero = sum(value != 0 for value in samples)
    clipped = sum(abs(value) >= 32767 for value in samples) / len(samples)
    rms = math.sqrt(sum(value * value for value in samples) / len(samples))
    if not nonzero or rms < 1 or clipped >= .01:
        raise CheckFailed('Silent or excessively clipped guest PCM')
    return {'frames': count, 'seconds': count / 44100, 'nonzero_samples': nonzero,
            'clipped_fraction': clipped, 'rms': rms, 'sha256': hashlib.sha256(data).hexdigest()}


def field(value, path):
    if isinstance(value, dict) and path in value:
        return value[path]
    part, separator, rest = path.partition('.')
    value = value[int(part)] if isinstance(value, list) else value[part]
    return field(value, rest) if separator else value


def strict_equal(actual, expected):
    if type(actual) is not type(expected):
        return False
    if isinstance(actual, dict):
        return actual.keys() == expected.keys() and all(strict_equal(actual[key], expected[key]) for key in actual)
    if isinstance(actual, list):
        return len(actual) == len(expected) and all(strict_equal(a, b) for a, b in zip(actual, expected))
    return actual == expected


def finite_evidence(value):
    if isinstance(value, float) and not math.isfinite(value):
        raise CheckFailed('Nonfinite observation or expectation')
    if isinstance(value, (dict, list)):
        for item in value.values() if isinstance(value, dict) else value:
            finite_evidence(item)


def compare(actual, operator, expected):
    finite_evidence(actual)
    finite_evidence(expected)
    if operator == 'eq':
        ok = strict_equal(actual, expected)
    elif operator == 'ne':
        ok = type(actual) is type(expected) and not strict_equal(actual, expected)
    elif operator in ('gt', 'ge', 'lt', 'le'):
        if type(actual) not in (int, float) or type(expected) not in (int, float):
            raise CheckFailed('Numeric comparison requires numbers, not unknown/bool values')
        ok = {'gt': actual > expected, 'ge': actual >= expected,
              'lt': actual < expected, 'le': actual <= expected}[operator]
    elif operator == 'distance_gt':
        point, minimum = expected['point'], expected['minimum']
        if not isinstance(actual, list) or not isinstance(point, list) or len(actual) != 3 or len(point) != 3 or \
                any(type(v) not in (float, int) or not math.isfinite(v) for v in actual + point):
            raise CheckFailed('Distance requires two finite observed positions')
        ok = math.sqrt(sum((a - b) ** 2 for a, b in zip(actual, point))) > minimum
    elif operator == 'angle_delta_gt':
        if type(actual) not in (float, int) or type(expected['angle']) not in (float, int):
            raise CheckFailed('Angle delta requires observed numbers')
        ok = abs((actual - expected['angle'] + 180) % 360 - 180) > expected['minimum']
    else:
        raise ValueError('Unknown comparison operator')
    if not ok:
        raise CheckFailed(f'Expected {operator} {expected!r}; observed {actual!r}')


def confined(root, name):
    if not isinstance(name, str) or '\\' in name or os.path.isabs(name) or '..' in Path(name).parts:
        raise ValueError('Evidence names must be relative')
    root = os.path.normcase(os.path.realpath(root))
    lexical = os.path.normcase(os.path.abspath(os.path.join(root, name)))
    path = os.path.normcase(os.path.realpath(lexical))
    if path != lexical or not path.startswith(root.rstrip(os.sep) + os.sep):
        raise ValueError('Evidence path escapes or contains a link')
    return Path(path)


def inventory(root):
    root = Path(root)
    if not root.is_dir() or root.is_symlink():
        raise CheckFailed('Missing or linked file tree')
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise CheckFailed('File tree contains a link or special file')
        if path.is_file():
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(65536), b''):
                    digest.update(block)
            result[path.relative_to(root).as_posix()] = digest.hexdigest()
    if not result:
        raise CheckFailed('Empty file tree')
    return result


def settings(path):
    result = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line and not line.startswith(('#', ';')) and '=' in line:
            key, value = line.split('=', 1)
            result[key.strip()] = value.strip()
    if not result:
        raise CheckFailed('Missing settings values')
    return result


def pixels(path):
    """Decode bounded uncompressed RGB BMP or noninterlaced 8-bit RGB(A) PNG."""
    if Path(path).stat().st_size > 64 * 1024 * 1024:
        raise CheckFailed('Oversized capture')
    data = Path(path).read_bytes()
    if len(data) > 64 * 1024 * 1024:
        raise CheckFailed('Oversized capture')
    if data.startswith(b'BM'):
        offset = struct.unpack_from('<I', data, 10)[0]
        width, height, planes, bits, compression = struct.unpack_from('<iiHHI', data, 18)
        if planes != 1 or bits not in (24, 32) or compression != 0:
            raise CheckFailed('Unsupported BMP encoding')
        channels = bits // 8
        stride = (width * channels + 3) & ~3
        bounded_dimensions(width, abs(height))
        if offset < 54 or offset + stride * abs(height) > len(data):
            raise CheckFailed('Truncated BMP')
        rows = [data[offset + i * stride:offset + i * stride + width * channels] for i in range(abs(height))]
        if height > 0:
            rows.reverse()
        rgb = bytes(component for row in rows for i in range(0, len(row), channels)
                    for component in (row[i + 2], row[i + 1], row[i]))
        return width, abs(height), rgb
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        raise CheckFailed('Capture is not PNG/BMP')
    at, header, compressed, ended = 8, None, bytearray(), False
    while at + 12 <= len(data):
        size = struct.unpack_from('>I', data, at)[0]
        kind = data[at + 4:at + 8]
        chunk = data[at + 8:at + 8 + size]
        if at + size + 12 > len(data) or zlib.crc32(kind + chunk) != struct.unpack_from('>I', data, at + size + 8)[0]:
            raise CheckFailed('Truncated PNG or invalid CRC')
        at += size + 12
        if kind == b'IHDR':
            if header or size != 13:
                raise CheckFailed('Invalid PNG header')
            header = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'IDAT':
            compressed.extend(chunk)
        elif kind == b'IEND':
            ended = True
            break
    if not header or not ended:
        raise CheckFailed('Incomplete PNG')
    width, height, depth, color, compression, filtering, interlace = header
    bounded_dimensions(width, height)
    if depth != 8 or color not in (2, 6) or compression or filtering or interlace:
        raise CheckFailed('Unsupported PNG encoding')
    channels = 3 if color == 2 else 4
    stride = width * channels
    expected = (stride + 1) * height
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected + 1)
    if len(raw) != expected or not decoder.eof or decoder.unused_data:
        raise CheckFailed('Invalid or oversized PNG pixels')
    previous, rgb = bytearray(stride), bytearray()
    for row in range(height):
        start = row * (stride + 1)
        mode, current = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        if mode > 4:
            raise CheckFailed('Invalid PNG filter')
        for i in range(stride):
            left = current[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            estimate = left + up - corner
            paeth = min((left, up, corner), key=lambda v: abs(estimate - v))
            current[i] = (current[i] + (0, left, up, (left + up) // 2, paeth)[mode]) & 255
        rgb.extend(component for i in range(0, stride, channels) for component in current[i:i + 3])
        previous = current
    return width, height, bytes(rgb)


def bounded_dimensions(width, height):
    if not 0 < width <= 8192 or not 0 < height <= 8192 or width * height > 16 * 1024 * 1024:
        raise CheckFailed('Invalid or oversized capture dimensions')


def image_content(image, minimum_colors=32, minimum_nonblack=0.1):
    width, height, rgb = image
    count = width * height
    colored = sum(max(rgb[i:i + 3]) > 12 for i in range(0, len(rgb), 3))
    colors = len(set(rgb[i:i + 3] for i in range(0, len(rgb), 3)))
    if colors < minimum_colors or colored / count < minimum_nonblack:
        raise CheckFailed('Blank/flat capture')
    return {'width': width, 'height': height, 'colors': colors, 'nonblack_fraction': colored / count}


def colour_count(picture, region, colour, tolerance=20, normalise=False):
    """Count an authored RGB marker in a bounded region, without a reference frame."""
    width, height, rgb = picture
    if len(rgb) != width * height * 3:
        raise ValueError('Colour check needs a complete decoded RGB image')
    if len(region) != 4 or any(type(value) is not int for value in region):
        raise ValueError('Colour regions need four integer bounds')
    x, y, w, h = region
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > width or y + h > height:
        raise ValueError('Colour region leaves the decoded image')
    if len(colour) != 3 or any(type(value) is not int or not 0 <= value <= 255 for value in colour) or type(tolerance) is not int or not 0 <= tolerance <= 255:
        raise ValueError('Expected RGB colour/tolerance is invalid')
    expected_max = max(colour)
    if normalise and not expected_max:
        raise ValueError('A chromatic marker cannot be black')
    expected = [value * 255 / expected_max for value in colour] if normalise else colour
    count = 0
    for row in range(y, y + h):
        for column in range(x, x + w):
            pixel = rgb[(row * width + column) * 3:(row * width + column) * 3 + 3]
            maximum = max(pixel)
            if normalise:
                if maximum < 24:
                    continue
                pixel = [value * 255 / maximum for value in pixel]
            count += all(abs(pixel[channel] - expected[channel]) <= tolerance for channel in range(3))
    return count


def image_difference(before, after):
    if before[:2] != after[:2]:
        raise CheckFailed('Compared capture dimensions differ')
    count = before[0] * before[1]
    changed = sum(before[2][i:i + 3] != after[2][i:i + 3] for i in range(0, len(before[2]), 3))
    return changed / count


def reference_region(actual, reference, region, tolerance=12, maximum_bad_fraction=.02):
    """A navigation oracle; independent state checks must still prove outcomes."""
    if actual[:2] != reference[:2] or len(region) != 4 or not 0 <= tolerance <= 255 or not 0 <= maximum_bad_fraction < 1:
        raise CheckFailed('Invalid reference dimensions or tolerance')
    x, y, width, height = region
    if any(type(v) is not int for v in region) or x < 0 or y < 0 or width <= 0 or height <= 0 or \
            x + width > actual[0] or y + height > actual[1]:
        raise CheckFailed('Reference region escapes capture')
    bad = 0
    for row in range(y, y + height):
        for column in range(x, x + width):
            at = (row * actual[0] + column) * 3
            bad += max(abs(actual[2][at + c] - reference[2][at + c]) for c in range(3)) > tolerance
    fraction = bad / (width * height)
    if fraction > maximum_bad_fraction:
        raise CheckFailed(f'Reference region mismatch: {fraction:.4f}')
    return fraction


def percentile(samples, fraction):
    if not samples or any(type(x) not in (int, float) or not math.isfinite(x) or x < 0 for x in samples):
        raise CheckFailed('Missing/invalid timing samples')
    ordered = sorted(samples)
    return ordered[min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)]


def performance_comparison(baseline, candidate, identity, budget=None):
    """Thresholds are allowed only after same-device repeated baseline calibration."""
    required = ('device', 'gpu', 'driver', 'resolution', 'execution_mode', 'route_sha256',
                'fixture_sha256', 'settings_sha256', 'scenario_sha256')
    if any(key not in identity for key in required):
        raise CheckFailed('Incomplete performance comparison identity')
    if len(baseline) < 3 or len(candidate) < 3:
        raise CheckFailed('At least three warmed repetitions required per artifact')
    for run in baseline + candidate:
        if run.get('identity') != identity or not run.get('artifact_sha256') or run.get('warmup_frames', 0) < 60:
            raise CheckFailed('Workload/device/mode differs or warm-up is missing')
        if len(run.get('frame_ms', [])) < 120 or len(run.get('speed_percent', [])) < 3:
            raise CheckFailed('Insufficient steady-state samples')
        percentile(run['frame_ms'], .99)
        percentile(run['speed_percent'], .5)
        if type(run.get('startup_ms')) not in (int, float) or run['startup_ms'] <= 0:
            raise CheckFailed('Missing startup measurement')
        if not run.get('load_ms'):
            raise CheckFailed('Missing load measurements')
        percentile(run['load_ms'], .5)
    summary = {}
    for metric, extract in (
            ('p95_ms', lambda run: percentile(run['frame_ms'], .95)),
            ('p99_ms', lambda run: percentile(run['frame_ms'], .99)),
            ('load_ms', lambda run: statistics.median(run['load_ms'])),
            ('startup_ms', lambda run: run['startup_ms']),
            ('speed_percent', lambda run: statistics.median(run['speed_percent']))):
        old, new = [extract(run) for run in baseline], [extract(run) for run in candidate]
        center, changed = statistics.median(old), statistics.median(new)
        noise = max(old) - min(old)
        summary[metric] = {'baseline': center, 'candidate': changed, 'baseline_spread': noise}
        if budget is not None:
            if metric not in budget or budget[metric] < noise:
                raise CheckFailed('Regression budget is missing or narrower than measured baseline noise: ' + metric)
            delta = center - changed if metric == 'speed_percent' else changed - center
            if delta > budget[metric]:
                raise CheckFailed('Performance regression: ' + metric)
    return {'metrics': summary, 'threshold_enabled': budget is not None,
            'note': 'Presented FPS alone is not simulation or stutter evidence.'}
