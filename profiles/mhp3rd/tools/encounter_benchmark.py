#!/usr/bin/env python3
"""Bounded private Barioth comparisons using the shared copied-fixture adapters.

Public CI tests the collector and guards with synthetic observations. This CLI
never runs game-data benchmarks in ordinary CI and never reserves cloud devices.
"""
import argparse
import json
import math
from pathlib import Path
import re
import time

import device_checks as checks
import device_suite as suite
import device_testing as runner


class Frames:
    """Consume only new presents, including full ring wraps; reject lost data."""
    def __init__(self, sample):
        self.sequence = self.number(sample)
        self.values = []

    @staticmethod
    def number(sample):
        value = sample.get('frame_sequence')
        if type(value) is not int or value < 0:
            raise checks.CheckFailed('Artifact lacks a valid present sequence')
        return value

    def add(self, sample):
        sequence = self.number(sample)
        count = sequence - self.sequence
        history = sample.get('frame_ms', [])
        if not isinstance(history, list) or count < 0 or count > len(history):
            raise checks.CheckFailed('Frame history reset or observation cadence lost presents')
        new = history[-count:] if count else []
        if any(type(x) not in (int, float) or not math.isfinite(x) or x <= 0 for x in new):
            raise checks.CheckFailed('Unknown/nonpositive frame duration')
        self.values.extend(new)
        self.sequence = sequence


def timing_kind(device):
    alternating = device.get('performance', 'log').startswith('alternate-') or device.get('env', {}).get('MHP3RD_PERF_ALTERNATE')
    return 'alternating-diagnostic' if alternating else 'fixed'


def encounter_records(text):
    records = []
    for line in text.splitlines(keepends=True):
        # File readers can race a write. An incomplete line is not a response.
        if not line.endswith('\n') or '[debug] encounter {' not in line:
            continue
        record = json.loads(line.split('[debug] encounter ', 1)[1])
        if type(record.get('schema')) is not int or record['schema'] != 1 or record.get('preset') != 'barioth-tundra':
            raise checks.CheckFailed('Unknown encounter observation schema/preset')
        records.append(record)
    return records


def encounter(session, timeout=5):
    offset = len(session.adapter.text())
    session.adapter.send('debug', 'encounter status')
    deadline = min(session.deadline, time.monotonic() + timeout)
    while True:
        records = encounter_records(session.adapter.text()[offset:])
        if records:
            return records[-1]
        if not session.adapter.alive():
            raise checks.CheckFailed('Owned game exited before encounter readback')
        runner.remaining(deadline)
        time.sleep(.05)


def live(sample, ready, expected=None):
    required = {'on_quest': True, 'task': 'game_task.ovl', 'map': 'P_m04a07d.ovl',
                'accepted_id': 507, 'active_id': 507, 'online': False, 'quest_cheats': False,
                'photo': False, 'fast_forward': False, 'fast_loading': False}
    if not suite.condition(sample, required):
        raise checks.CheckFailed('Timing requires the original active offline quest, without pause/cheats/acceleration')
    if ready.get('phase') != 'ready' or type(ready.get('actor')) is not int or not ready['actor'] or \
            type(ready.get('model')) is not int or not ready['model']:
        raise checks.CheckFailed('Target actor/model lifetime is not ready')
    target = next((m for m in sample.get('monsters', []) if m.get('address') == ready['actor']), None)
    if not target or target.get('kind') != 5 or target.get('same_area') is not True or target.get('health', 0) <= 0:
        raise checks.CheckFailed('The live Barioth left the encounter')
    if sample.get('lock_on') is not True or sample.get('lock_target') != ready['actor']:
        raise checks.CheckFailed('Follow camera is not tracking the actual Barioth')
    if any(type(sample.get(key)) is not int or sample[key] <= 0 for key in ('target_width', 'target_height', 'host_pid')):
        raise checks.CheckFailed('Actual rendering resolution/process identity is unknown')
    identity = (ready['actor'], ready['model'], sample['target_width'], sample['target_height'], sample['host_pid'])
    if expected is not None and identity != expected:
        raise checks.CheckFailed('Actor/model lifetime or rendering resolution changed during timing')
    return identity


def collect(session, frames, start_tick, deadline):
    ready = encounter(session)
    if ready.get('in_view') is not True:
        raise checks.CheckFailed('Barioth is outside the camera frustum before warmup')
    sample = session.snapshot(performance=True)
    identity = live(sample, ready)
    if sample['quest_ticks'] <= start_tick:
        raise checks.CheckFailed('Setup missed the declared measurement start tick')
    # Warmup ends at a fixed original quest tick, not a different AI age on
    # each device/artifact. The external session cap still bounds slow games.
    while sample['quest_ticks'] > start_tick:
        runner.remaining(deadline)
        time.sleep(.1)
        sample = session.snapshot(performance=True)
        live(sample, encounter(session), identity)
    first = sample
    history = Frames(first)
    seconds, seen = [], set()
    while sample['quest_ticks'] > start_tick - frames:
        runner.remaining(deadline)
        time.sleep(.1)
        sample = session.snapshot(performance=True)
        state = encounter(session)
        live(sample, state, identity)
        history.add(sample)
        if sample.get('perf_valid') and sample['perf_second'] not in seen:
            seen.add(sample['perf_second'])
            seconds.append({key: value for key, value in sample.items() if key in {
                'perf_second', 'speed_percent', 'game_fps', 'presented_fps', 'guest_ms', 'render_ms',
                'wait_ms', 'pacing_ms', 'gpu_valid', 'gpu_ms', 'ge_draws', 'recorded_draws',
                'render_passes', 'target_copies', 'requested_rate', 'running_rate', 'vertex_mib', 'index_mib'}})
    if len(seconds) < 2 or not history.values:
        raise checks.CheckFailed('Insufficient independent telemetry windows or frame durations')
    return {'start_tick': first['quest_ticks'], 'end_tick': sample['quest_ticks'],
            'start_vblanks': first['vblanks'], 'end_vblanks': sample['vblanks'],
            'target_size': list(identity[2:4]), 'present_frames': len(history.values),
            'frame_ms': history.values, 'median_ms': checks.percentile(history.values, .5),
            'p95_ms': checks.percentile(history.values, .95), 'p99_ms': checks.percentile(history.values, .99),
            'seconds': seconds}


def run(device, root, seconds, frames, start_tick):
    if device['adapter'] not in ('local', 'adb'):
        raise runner.NotTested('Use a local/ADB device; invoke this command on the SSH host for remote native testing')
    if device['build'] != 'dev' or not device.get('observations') or 'rtc_epoch_us' not in device:
        raise ValueError('Encounter benchmarks require a developer artifact, observations and a pinned RTC')
    program = device.get('suite_fixtures', {}).get('load-character')
    if not program:
        raise runner.NotTested('Missing finite load-character navigation fixture')
    suite.validate_program(program, fixture=True)
    report = {'schema': 1, 'source_commit': device['commit'], 'status': 'FAIL',
              'timing_kind': timing_kind(device),
              'configuration_sha256': runner.object_hash(device), 'rtc_epoch_us': device['rtc_epoch_us'],
              'preset': 'barioth-tundra', 'capture_review': 'PENDING', 'error': ''}
    session = suite.Session(device, root, time.monotonic() + seconds)
    try:
        session.prepare()
        report.update(session.identity)
        session.snapshot(timeout=30, until={'task': 'demo_task.ovl'})
        suite.execute(session, program, {}, {}, device, session.deadline)
        session.snapshot(timeout=60, until={'village_ready': True, 'online': False})
        text = session.adapter.text()
        if f'[device-test] RTC epoch {device["rtc_epoch_us"]} us' not in text:
            raise checks.CheckFailed('Native boot did not confirm the declared pinned RTC epoch')
        report['rtc_verified'] = True
        if not re.search(r'Overlay corpora: 355\b', text) or not re.search(r'Functions:\s+[1-9][0-9]+', text):
            raise checks.CheckFailed('Full native AOT and all 355 overlay corpora are required')
        session.adapter.send('debug', 'encounter start barioth')
        setup_deadline = min(session.deadline, time.monotonic() + 65)
        while True:
            state = encounter(session)
            if state['phase'] == 'ready':
                break
            if state['phase'] in ('failed', 'finished') or 'encounter refused:' in session.adapter.text():
                raise checks.CheckFailed('Encounter setup failed: ' + state.get('reason', 'refused'))
            runner.remaining(setup_deadline)
            time.sleep(.1)
        report['setup'] = state
        report['captures'] = [session.capture('encounter-ready')]
        report['measurement'] = collect(session, frames, start_tick, session.deadline)
        report['captures'].append(session.capture('encounter-measured'))
        text = session.adapter.text()
        report['gpu'] = re.findall(r'^\[gpu\] (.+driver .+)$', text, re.M)
        report['gpu_compatibility'] = re.findall(r'^\[gpu-compat\] (.+)$', text, re.M)
        if len(report['gpu']) != 1 or not report['gpu_compatibility']:
            raise runner.NotTested('Actual GPU/driver or resolved GPU compatibility mode is unavailable')
        report['language'] = device.get('env', {}).get('MHP3RD_LANGUAGE', '')
        report['performance_environment'] = {key: value for key, value in device.get('env', {}).items()
                                             if key not in ('MHP3RD_NO_SPARSE_VERTEX_DECODE',
                                                            'MHP3RD_NO_PLAIN_ALPHA_VARIANTS')}
        if device.get('performance', 'log').startswith('alternate-'):
            report['performance_environment']['MHP3RD_PERF_ALTERNATE'] = device['performance'].removeprefix('alternate-')
        report['settings_sha256'] = runner.fingerprint(session.file('data/settings.ini'))
        report['status'] = 'COLLECTED'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            session.close()
        except Exception as error:
            report['status'], report['error'] = 'FAIL', 'Cleanup/integrity: ' + str(error)
            raise
        finally:
            runner.write_json(root / 'encounter-report.json', report)
    return report


def comparison_identity(report):
    return {key: report[key] for key in ('fixture_sha256', 'game_sha256', 'settings_sha256',
                                        'rtc_epoch_us', 'gpu', 'gpu_compatibility')} | {
        'target_size': report['measurement']['target_size'], 'hardware': report['hardware'],
        'language': report['language'], 'translation_sha256': report.get('translation_sha256'),
        'performance_environment': report.get('performance_environment', {})}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--device', required=True)
    parser.add_argument('--baseline', help='Optional baseline device entry; settings/fixtures must match')
    parser.add_argument('--repeat', type=int, choices=range(1, 5), default=3)
    parser.add_argument('--seconds', type=int, default=180, help='Hard lifetime cap per owned session, including setup')
    parser.add_argument('--frames', type=int, default=300, help='Original 30 Hz quest ticks to measure')
    parser.add_argument('--start-tick', type=int, default=89550, help='Original quest tick at which measurement starts')
    parser.add_argument('--output-parent', type=Path, default=runner.ROOT / 'out')
    args = parser.parse_args(argv)
    if not 1 <= args.seconds <= 900 or not 30 <= args.frames <= 1800 or not args.frames < args.start_tick < 89900:
        parser.error('Invalid bounded session, measurement interval or warmup tick')
    devices = [args.baseline, args.device] if args.baseline else [args.device]
    kind = timing_kind(runner.load_device(args.config, args.device))
    if args.baseline and any(timing_kind(runner.load_device(args.config, name)) != 'fixed' for name in devices):
        raise ValueError('Alternating diagnostics must be analyzed separately, without a baseline entry')
    results = []
    ensemble = runner.output_directory(args.output_parent)
    print(ensemble, flush=True)
    identity = None
    summary = {'status': 'FAIL', 'runs': results, 'capture_review': 'PENDING', 'timing_kind': kind}
    try:
        for repetition in range(args.repeat):
            for name in devices if repetition % 2 == 0 else reversed(devices):
                device = runner.load_device(args.config, name)
                root = ensemble / f'{repetition}-{runner.safe_name(name)}'
                root.mkdir()
                print(root, flush=True)
                report = run(device, root, args.seconds, args.frames, args.start_tick)
                current = comparison_identity(report)
                if identity is not None and current != identity:
                    raise checks.CheckFailed('Hardware, GPU mode, game/save/settings, RTC or resolution differs between runs')
                identity = current
                results.append({'device': name, 'repetition': repetition, 'report': str(root / 'encounter-report.json'),
                                'source_commit': report['source_commit'], 'measurement': report['measurement']})
                print(json.dumps({key: report['measurement'][key] for key in ('median_ms', 'p95_ms', 'p99_ms')}), flush=True)
        summary['status'] = 'COLLECTED'
    finally:
        runner.write_json(ensemble / 'benchmark-report.json', summary)
    print('Collected private measurements; inspect the declared captures before making correctness/performance claims.')
    return results


if __name__ == '__main__':
    main()
