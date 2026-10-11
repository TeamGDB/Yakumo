#!/usr/bin/env python3
"""Run the ten automatic scenarios; missing outcomes remain NOT TESTED."""
import argparse
import base64
import copy
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile

import device_checks as checks
import device_testing as runner

DEFINITIONS = runner.ROOT / 'profiles/mhp3rd/tests/device-suite'
ACTIONS = {'fixture', 'input', 'debug', 'snapshot', 'assert', 'settings', 'tree',
           'capture', 'image', 'difference', 'restart', 'hold', 'log', 'parameter', 'reference', 'approach', 'attack',
           'installation', 'game_png'}


def definitions():
    values = [runner.read_json(path) for path in sorted(DEFINITIONS.glob('*.json'))]
    if len(values) != 10 or [value['id'] for value in values] != [f'{i:02}' for i in range(1, 11)]:
        raise ValueError('Suite must have exactly ten ordered standard scenario IDs')
    for value in values:
        if not value['checks'] or len({item['id'] for item in value['checks']}) != len(value['checks']):
            raise ValueError('Scenario needs unique required subchecks')
        for item in value['checks']:
            runner.safe_name(item['id'])
            validate_program(item['steps'])
            if not any(step['action'] in ('assert', 'image', 'difference') for step in item['steps']):
                raise ValueError('Every subcheck requires an independent outcome assertion')
    return values


def validate_program(steps, fixture=False):
    if not isinstance(steps, list) or not 0 < len(steps) <= 128:
        raise ValueError('Programs need 1..128 finite steps')
    for step in steps:
        if step.get('action') not in ACTIONS or fixture and step['action'] == 'fixture':
            raise ValueError('Unknown action or recursive fixture')
        if not 0 < step.get('timeout', 20) <= 120:
            raise ValueError('Step deadline must be 0..120 seconds')
        if step['action'] in ('input', 'debug'):
            runner.validate_step({step['action']: step['value']})
        if step['action'] in ('hold',) and not 0.1 <= step['seconds'] <= 10:
            raise ValueError('Stable-state hold must be 0.1..10 seconds')


def verdict(items):
    statuses = [item['verdict'] for item in items]
    return 'FAIL' if 'FAIL' in statuses else 'PASS' if statuses and all(x == 'PASS' for x in statuses) else 'NOT TESTED'


def condition(sample, expected):
    for key, value in expected.items():
        actual = sample.get(key)
        try:
            if isinstance(value, dict) and set(value) == {'op', 'value'}:
                checks.compare(actual, value['op'], value['value'])
            else:
                checks.compare(actual, 'eq', value)
        except checks.CheckFailed:
            return False
    return True


def exit_status(report):
    value = verdict(report['devices'])
    return 0 if value == 'PASS' else 1 if value == 'FAIL' else 2


def render(root, report):
    runner.write_json(root / 'report.json', report)
    lines = ['# Automatic pre-release suite', '', f'Status: **{verdict(report["devices"])}**', '',
             '| Device | Scenario | Subcheck | Verdict | Reason |', '| --- | --- | --- | --- | --- |']
    def clean(value):
        return str(value).replace('|', '\\|').replace('\n', ' ')
    for device in report['devices']:
        for scenario in device['scenarios']:
            for item in scenario['checks']:
                lines.append('| ' + ' | '.join(map(clean, (device['name'], scenario['id'], item['id'],
                                                          item['verdict'], item['reason']))) + ' |')
    lines.extend(['', 'Private evidence only. Developer readbacks do not certify exact release artifacts.',
                  'Physical controls, audible playback and long hunts require complementary human sign-off.'])
    (root / 'suite.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


class Session:
    """One prepared device; restarts reuse game inputs and reset only owned copies."""
    def __init__(self, device, root, deadline):
        self.device, self.root, self.deadline = copy.deepcopy(device), root, deadline
        self.adapter = runner.Local(self.device, root, deadline) if device['adapter'] == 'local' else runner.Adb(self.device, root, deadline)
        if device.get('streaming_cleanup_deadline'):
            self.adapter.cleanup_deadline = device['streaming_cleanup_deadline']
        self.identity = {}
        self.data_root = root / 'data'
        self.request, self.phase = 0, 0
        self.samples = []
        self.logs = []
        self.started = False
        self.observation_ready = False

    def prepare(self, launch=True):
        self.adapter.prepare(self.identity)
        runner.copy_fixture(self.root / 'data/ms0', self.root / 'initial-ms0')
        if (self.root / 'data/settings.ini').exists():
            runner.copy_regular_file(self.root / 'data/settings.ini', self.root / 'initial-settings.ini')
        if launch:
            self.start()

    def start(self):
        self.adapter.deadline = self.deadline
        self.adapter.owned = False
        self.adapter.launch()
        self.phase += 1
        self.started = True
        self.observation_ready = False

    def stop(self):
        try:
            if self.started:
                self.logs.append(self.adapter.text())
        finally:
            self.adapter.close()
            self.started = False

    def restart(self, reset=False):
        self.stop()
        if reset:
            target = self.data_root / 'ms0'
            if target.is_symlink():
                raise ValueError('Owned save tree became a link')
            shutil.rmtree(target)
            runner.copy_fixture(self.root / 'initial-ms0', target)
            settings = self.data_root / 'settings.ini'
            settings.unlink(missing_ok=True)
            if (self.root / 'initial-settings.ini').exists():
                runner.copy_regular_file(self.root / 'initial-settings.ini', settings)
            if isinstance(self.adapter, runner.Adb):
                self.adapter.deadline = self.deadline
                self.adapter.app('rm -rf files/device-testing/data/ms0 && rm -f files/device-testing/data/settings.ini')
                for path in sorted((self.root / 'data').rglob('*')):
                    if path.is_file() and path.name not in ('disc.iso', 'EBOOT.ELF'):
                        relative = path.relative_to(self.root / 'data').as_posix()
                        destination = 'files/device-testing/data/' + relative
                        self.adapter.app('mkdir -p ' + runner.shlex.quote(str(runner.PurePosixPath(destination).parent)))
                        self.adapter.upload(path, destination)
        for name in ('input-live.txt', 'debug.txt'):
            (self.root / name).write_text('')
        if isinstance(self.adapter, runner.Adb):
            self.adapter.deadline = self.deadline
            self.adapter.app('printf "" > files/device-testing/input-live.txt; printf "" > files/device-testing/debug.txt')
            for key in ('watchdog', 'watchdog_pid'):
                if hasattr(self.adapter, key):
                    delattr(self.adapter, key)
        self.start()

    def snapshot(self, timeout=20, until=None, probe=None, performance=False):
        if not self.device.get('observations'):
            raise runner.NotTested('Artifact lacks explicit developer structured-observation contract')
        deadline = min(self.deadline, time.monotonic() + timeout)
        if not self.observation_ready:
            runner.wait_condition(self.adapter, r'\[debug\] reading commands from ', timeout, deadline)
            self.observation_ready = True
        while True:
            self.request += 1
            request = self.request
            offset = len(self.adapter.text())
            suffix = ''
            if performance:
                suffix = ' perf'
            if probe:
                if len(probe) != 3 or any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in probe):
                    raise ValueError('Text probe needs numeric block/table/index')
                suffix = ' text ' + ' '.join(map(str, probe))
            self.adapter.send('debug', 'observe ' + str(request) + suffix)
            while True:
                complete = self.adapter.text()
                text = complete[offset:]
                if re.search(r'FATAL EXCEPTION|Fatal signal|VK_ERROR_DEVICE_LOST|\[script\] unknown |\[debug\] (?:not started|refused)', complete):
                    raise checks.CheckFailed('Game crash, device loss or rejected scripted input')
                for line in text.splitlines():
                    marker = '[debug] observation '
                    if marker in line:
                        sample = json.loads(line.split(marker, 1)[1])
                        if sample.get('schema') == 1 and sample.get('request') == request:
                            sample['_wall_seconds'] = time.monotonic()
                            self.samples.append({key: value for key, value in sample.items() if key != 'frame_ms'})
                            self.samples = self.samples[-128:]
                            if until is None or condition(sample, until):
                                return sample
                            break
                else:
                    if not self.adapter.alive():
                        raise checks.CheckFailed('Game exited before readback')
                    runner.remaining(deadline)
                    time.sleep(.1)
                    continue
                break
            runner.remaining(deadline)
            time.sleep(.1)

    def capture(self, name):
        self.capture_number = getattr(self, 'capture_number', 0) + 1
        return self.adapter.capture(f'{self.phase}-{self.capture_number}-{runner.safe_name(name)}')

    def game_png(self):
        if not isinstance(self.adapter, runner.Local):
            raise runner.NotTested('App-private PNG collection for ADB is not implemented yet')
        folder = self.data_root / 'screenshots'
        before = set(folder.glob('*.png'))
        offset = len(self.adapter.text())
        self.adapter.send('input', '0:scan F12 4')
        limit = min(self.deadline, time.monotonic() + 10)
        runner.wait_condition(self.adapter, r'\[screenshot\] .+ \([0-9]+x[0-9]+\)', 10, limit, offset)
        while True:
            created = set(folder.glob('*.png')) - before
            if created:
                if len(created) != 1:
                    raise checks.CheckFailed('One screenshot input produced multiple files')
                source = next(iter(created))
                checks.confined(self.root, source.relative_to(self.root).as_posix())
                if not source.is_file() or source.is_symlink():
                    raise ValueError('Screenshot is not an owned regular file')
                self.capture_number = getattr(self, 'capture_number', 0) + 1
                target = self.root / 'shots' / f'{self.phase}-{self.capture_number}-game-png.png'
                runner.copy_regular_file(source, target)
                return target.relative_to(self.root).as_posix()
            runner.remaining(limit)
            time.sleep(.1)

    def file(self, name):
        path = checks.confined(self.root, name)
        if isinstance(self.adapter, runner.Local) and name.startswith('data/'):
            path = checks.confined(self.root, (self.data_root / name[5:]).relative_to(self.root).as_posix())
        if isinstance(self.adapter, runner.Adb):
            if not name.startswith('data/'):
                raise runner.NotTested('ADB file oracle requires app-private data path')
            data = self.adapter.app('cat ' + runner.shlex.quote('files/device-testing/' + name))
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.is_symlink():
                raise ValueError('Pulled file destination became a link')
            path.write_bytes(data)
        return path

    def installation(self, unicode=False, update=False):
        if not isinstance(self.adapter, runner.Local):
            raise runner.NotTested('Desktop installer/portable variant requires a local host artifact')
        if update and not self.device.get('baseline_binary'):
            raise runner.NotTested('Update needs a distinct pinned baseline artifact')
        self.stop()
        directory = self.root / ('portable-ユニコード' if unicode else 'installation')
        directory.mkdir()
        data = directory / 'data'
        data.mkdir()
        runner.copy_fixture(self.root / 'initial-ms0', data / 'ms0')
        if (self.root / 'initial-settings.ini').exists():
            runner.copy_regular_file(self.root / 'initial-settings.ini', data / 'settings.ini')
        before = checks.inventory(data / 'ms0')
        settings_before = checks.settings(data / 'settings.ini') if (data / 'settings.ini').exists() else {}
        source = self.adapter.binary
        name = 'Yakumo.exe' if source.suffix.lower() == '.exe' else 'Yakumo'
        binary = directory / name
        baseline_hash = None
        if update:
            baseline = self.device['baseline_binary']
            baseline_hash = runner.fingerprint(baseline)
            if baseline_hash != self.device.get('baseline_sha256') or baseline_hash == self.adapter.artifact_hash:
                raise ValueError('Update baseline is unpinned or identical to the candidate')
            runner.copy_regular_file(baseline, binary)
            if runner.fingerprint(binary) != baseline_hash:
                raise ValueError('Installed baseline fingerprint differs')
            binary.unlink()
        runner.copy_regular_file(source, binary)
        binary.chmod(binary.stat().st_mode | 0o111)
        candidate_hash = runner.fingerprint(binary)
        if candidate_hash != self.adapter.artifact_hash:
            raise ValueError('Installed artifact differs from candidate')
        if unicode:
            (directory / 'portable.txt').write_text('')
        env = dict(self.adapter.env, MHP3RD_GAME_DIR=str(data), MHP3RD_DATA_DIR=str(data))
        image = str((Path(self.device['game_dir']) / 'disc.iso').resolve())
        output = runner.command([str(binary), '--install', image, '--in-place'], self.deadline,
                                maximum=120, env=env).decode(errors='replace')
        (directory / 'install.log').write_text(output)
        eboot_hash = runner.fingerprint(data / 'EBOOT.ELF')
        checks.compare(eboot_hash, 'eq', runner.fingerprint(Path(self.device['game_dir']) / 'EBOOT.ELF'))
        if not (data / 'disc.iso').exists():
            try:
                (data / 'disc.iso').symlink_to(image)
            except OSError:
                runner.os.link(image, data / 'disc.iso')
        after = checks.inventory(data / 'ms0')
        checks.compare(after, 'eq', before)
        persisted = checks.settings(data / 'settings.ini')
        for key, value in settings_before.items():
            if key != 'disc_image':
                checks.compare(persisted.get(key), 'eq', value)
        if unicode:
            env.pop('MHP3RD_DATA_DIR')
        self.adapter.binary, self.adapter.env = binary, env
        self.data_root = data
        self.start()
        return {'before': before, 'after': after, 'artifact_sha256': candidate_hash,
                'baseline_sha256': baseline_hash, 'eboot_sha256': eboot_hash,
                'settings_preserved': True, 'portable': unicode}

    def close(self):
        self.stop()
        (self.root / 'run.log').write_text('\n'.join(self.logs), encoding='utf-8')


def resolve(value, context, parameters):
    if isinstance(value, dict):
        if set(value) == {'from'}:
            return checks.field(context, value['from'])
        if set(value) == {'from', 'add'}:
            previous = checks.field(context, value['from'])
            if type(previous) not in (int, float) or type(value['add']) not in (int, float):
                raise ValueError('Observation offset requires numbers')
            return previous + value['add']
        if set(value) == {'parameter'}:
            name = value['parameter']
            if name not in parameters:
                raise runner.NotTested('Missing independent private expectation: ' + name)
            return parameters[name]
        if set(value) in ({'point', 'minimum'}, {'angle', 'minimum'}):
            return {key: resolve(item, context, parameters) for key, item in value.items()}
        raise ValueError('Invalid expectation reference')
    return value


def approach(session, deadline, kind=None, maximum_steps=24, stop_distance=180):
    sample = session.snapshot()
    if not sample.get('on_quest') or sample.get('free_camera') or sample.get('quest_cheats'):
        raise checks.CheckFailed('Combat approach requires ordinary quest control without cheats/free camera')
    point = sample.get('camera_target')
    if point is None:
        raise runner.NotTested('Current follow-camera coordinates unavailable')
    targets = [item for item in sample['monsters'] if item.get('same_area') and item['health'] > 0 and
               (kind is None or item['kind'] == kind)]
    if not targets:
        raise checks.CheckFailed('Expected live combat target is absent from this area')
    target = min(targets, key=lambda item: math.hypot(item['position'][0] - point[0], item['position'][2] - point[2]))
    address = target['address']
    try:
        for movement in range(maximum_steps):
            runner.remaining(deadline)
            sample = session.snapshot()
            point = sample.get('camera_target')
            current = next((item for item in sample['monsters'] if item['address'] == address), None)
            if point is None or current is None or not current.get('same_area') or current['health'] <= 0:
                raise checks.CheckFailed('Combat target or follow camera left the observed area')
            coordinates = point + current['position']
            if any(type(value) not in (int, float) or not math.isfinite(value) for value in coordinates):
                raise checks.CheckFailed('Nonfinite combat coordinates')
            dx, dz = current['position'][0] - point[0], current['position'][2] - point[2]
            distance = math.hypot(dx, dz)
            if distance < stop_distance:
                return {**current, 'distance': distance, 'moves': movement}
            yaw = math.radians(sample['camera_yaw'])
            x = (dx * math.cos(yaw) - dz * math.sin(yaw)) / distance
            y = (dx * math.sin(yaw) + dz * math.cos(yaw)) / distance
            session.adapter.send('input', f'0:axis leftx {x}')
            session.adapter.send('input', f'0:axis lefty {y}')
            # Each individual move releases itself even if the controller stops.
            session.adapter.send('input', '18:axis leftx 0')
            session.adapter.send('input', '18:axis lefty 0')
            time.sleep(min(.55, runner.remaining(deadline)))
        raise checks.CheckFailed('Bounded approach did not reach combat range')
    finally:
        session.adapter.send('input', '0:axis leftx 0')
        session.adapter.send('input', '0:axis lefty 0')


def attack(session, target, deadline, strikes=3):
    if not 1 <= strikes <= 3:
        raise ValueError('Combat uses at most three finite strikes')
    for strike in range(strikes):
        session.adapter.send('input', f'{strike * 90}:pad y 4')
    limit = min(deadline, time.monotonic() + 10)
    while True:
        sample = session.snapshot()
        if sample.get('quest_cheats') or sample.get('free_camera'):
            raise checks.CheckFailed('Combat verification cannot use quest cheats or free camera')
        current = next((item for item in sample['monsters'] if item['address'] == target['address']), None)
        if current is None or not current.get('same_area'):
            raise checks.CheckFailed('Target disappeared before an independently observed hit')
        if current['health'] < target['health']:
            return {'before_health': target['health'], 'after_health': current['health'], 'address': target['address']}
        runner.remaining(limit)
        time.sleep(.1)


def execute(session, steps, context, parameters, device, deadline):
    assertions = 0
    for step in steps:
        runner.remaining(deadline)
        action = step['action']
        name = step.get('as', '')
        if action == 'fixture':
            fixture = device.get('suite_fixtures', {}).get(step['name'])
            if not fixture:
                raise runner.NotTested('Missing finite fixture navigation: ' + step['name'])
            validate_program(fixture, fixture=True)
            assertions += execute(session, fixture, context, parameters, device, deadline)
        elif action in ('input', 'debug'):
            if action == 'debug':
                if device['build'] != 'dev' or step['value'].split()[0] not in ('quest', 'state', 'peek', 'table'):
                    raise runner.NotTested('Only declared developer navigation/read commands are supported')
                if step['value'].startswith('quest start') and parameters.get('multiplayer'):
                    raise ValueError('Quest shortcuts are forbidden in multiplayer')
            context['_log_offset'] = len(session.adapter.text())
            session.adapter.send(action, step['value'])
        elif action == 'snapshot':
            probe = resolve(step['text_probe'], context, parameters) if 'text_probe' in step else None
            until = copy.deepcopy(step.get('until'))
            if until:
                for key, rule in until.items():
                    if isinstance(rule, dict) and 'value' in rule:
                        value = rule['value']
                        if isinstance(value, dict) and set(value) in ({'point', 'minimum'}, {'angle', 'minimum'}):
                            rule['value'] = {k: resolve(v, context, parameters) for k, v in value.items()}
                        else:
                            rule['value'] = resolve(value, context, parameters)
            context[name] = session.snapshot(step.get('timeout', 20), until, probe, step.get('performance', False))
        elif action == 'assert':
            actual = checks.field(context, step['actual'])
            if 'subtract' in step:
                previous = checks.field(context, step['subtract'])
                if type(actual) not in (int, float) or type(previous) not in (int, float):
                    raise checks.CheckFailed('Delta requires two observed numbers')
                actual -= previous
            checks.compare(actual, step['op'], resolve(step['expected'], context, parameters))
            assertions += 1
        elif action == 'settings':
            limit = min(deadline, time.monotonic() + step.get('timeout', 20))
            while True:
                context[name] = checks.settings(session.file('data/settings.ini'))
                if not step.get('until') or condition(context[name], step['until']):
                    break
                runner.remaining(limit)
                time.sleep(.1)
        elif action == 'tree':
            if isinstance(session.adapter, runner.Adb):
                raise runner.NotTested('Remote tree inventory transport is not implemented')
            context[name] = checks.inventory(checks.confined(session.root, step['path']))
        elif action == 'capture':
            context[name] = session.capture(name)
        elif action == 'reference':
            reference = device.get('navigation_references', {}).get(step['name'])
            if not reference or not reference.get('reviewed') or reference.get('hardware') != device.get('hardware'):
                raise runner.NotTested('Missing reviewed navigation reference for this hardware: ' + step['name'])
            if runner.fingerprint(reference['path']) != reference['sha256']:
                raise ValueError('Navigation reference changed')
            expected = checks.pixels(reference['path'])
            limit = min(deadline, time.monotonic() + step.get('timeout', 20))
            first_capture, previous_capture = None, None
            while True:
                path = session.capture('navigation-' + step['name'])
                if previous_capture and previous_capture != first_capture:
                    checks.confined(session.root, previous_capture).unlink()
                first_capture = first_capture or path
                previous_capture = path
                try:
                    checks.reference_region(checks.pixels(checks.confined(session.root, path)), expected,
                                            reference['region'], reference.get('tolerance', 12), reference.get('maximum_bad_fraction', .02))
                    context['_navigation_' + step['name']] = {'evidence': path, 'reference_sha256': reference['sha256']}
                    break
                except checks.CheckFailed:
                    runner.remaining(limit)
                    time.sleep(.5)
        elif action == 'image':
            picture = checks.pixels(checks.confined(session.root, checks.field(context, step['image'])))
            context[name] = checks.image_content(picture)
            if 'dimensions' in step:
                checks.compare(list(picture[:2]), 'eq', resolve(step['dimensions'], context, parameters))
            assertions += 1
        elif action == 'difference':
            before, after = [checks.pixels(checks.confined(session.root, checks.field(context, step[key])))
                             for key in ('before', 'after')]
            difference = checks.image_difference(before, after)
            checks.compare(difference, step['op'], step['expected'])
            context[name] = difference
            assertions += 1
        elif action == 'restart':
            session.restart(step.get('reset', False))
        elif action == 'hold':
            previous = session.snapshot()
            until = time.monotonic() + step['seconds']
            while time.monotonic() < until:
                current = session.snapshot(timeout=min(20, runner.remaining(deadline)))
                for key in step['fields']:
                    checks.compare(checks.field(current, key), 'eq', checks.field(previous, key))
                time.sleep(.1)
            assertions += 1
        elif action == 'log':
            runner.wait_condition(session.adapter, step['pattern'], step.get('timeout', 20), deadline,
                                  0 if step.get('history') else context.get('_log_offset', len(session.adapter.text())))
        elif action == 'parameter':
            context[name] = resolve({'parameter': step['name']}, context, parameters)
        elif action == 'approach':
            context[name] = approach(session, deadline, step.get('kind'))
        elif action == 'attack':
            context[name] = attack(session, checks.field(context, step['target']), deadline)
        elif action == 'installation':
            context[name] = session.installation(step.get('unicode', False), step.get('update', False))
        elif action == 'game_png':
            context[name] = session.game_png()
    return assertions


def run_device(device, name, suite, root, seconds):
    rows = [{'id': value['id'], 'title': value['title'], 'scenario_sha256': runner.object_hash(value),
             'checks': [{'id': check['id'], 'verdict': 'NOT TESTED', 'reason': 'Not reached'} for check in value['checks']]}
            for value in suite]
    report = {'name': name, 'build': device['build'], 'commit': device['commit'], 'scenarios': rows,
              'definition_sha256': runner.object_hash(suite), 'configuration_sha256': runner.object_hash(device)}
    started = time.monotonic()
    setup = device.get('setup_seconds', 0)
    hard_deadline = started + seconds + setup
    deadline = started + setup if setup else hard_deadline
    session = None
    try:
        if device['adapter'] not in ('local', 'adb'):
            raise runner.NotTested('Suite session delegation for this adapter is not implemented')
        session = Session(device, root, deadline)
        session.prepare(launch=False)
        runner.remaining(deadline)
        deadline = min(hard_deadline, time.monotonic() + seconds)
        session.deadline = session.adapter.deadline = deadline
        session.start()
        report['launched'] = True
        report.update(session.identity)
        for definition, row in zip(suite, rows):
            if definition is not suite[0]:
                session.restart(reset=True)
            for check, result in zip(definition['checks'], row['checks']):
                context = {}
                check_started = time.monotonic()
                check_deadline = min(deadline, check_started + check.get('seconds', 90))
                session.deadline = session.adapter.deadline = check_deadline
                try:
                    parameters = {**device.get('expectations', {}), 'multiplayer': definition['id'] == '08'}
                    count = execute(session, check['steps'], context, parameters, device, check_deadline)
                    if not count:
                        raise checks.CheckFailed('No outcome assertion executed')
                    result.update(verdict='PASS', reason=f'{count} independent assertions passed')
                except runner.NotTested as error:
                    result.update(reason=str(error))
                except KeyError as error:
                    result.update(reason='Structured observation capability is unavailable: ' + str(error))
                except (Exception, KeyboardInterrupt) as error:
                    result.update(verdict='FAIL', reason=str(error) or type(error).__name__)
                    if isinstance(error, KeyboardInterrupt):
                        raise
                finally:
                    result.update(duration_seconds=round(time.monotonic() - check_started, 3), evidence=context)
                    session.deadline = session.adapter.deadline = deadline
                    runner.write_json(root / 'report.json', report)
                if time.monotonic() >= deadline:
                    raise runner.NotTested('Session cap reached; remaining checks not run')
                if not session.adapter.alive():
                    code = getattr(session.adapter.process, 'returncode', None) if isinstance(session.adapter, runner.Local) else None
                    raise checks.CheckFailed('Owned game exited before session completion; observed exit code ' + str(code))
    except runner.NotTested as error:
        report['session_reason'] = str(error)
    except (Exception, KeyboardInterrupt) as error:
        report['session_reason'] = str(error) or type(error).__name__
        report['session_failure'] = True
        report['interrupted'] = isinstance(error, KeyboardInterrupt)
    finally:
        if session:
            try:
                session.deadline = session.adapter.deadline = deadline
                session.close()
                report['cleanup'] = 'PASS'
            except Exception as error:
                report['cleanup'] = 'FAIL: ' + str(error)
                report['session_failure'] = True
            report['samples'] = session.samples
        if 'session_reason' in report:
            for row in rows:
                for result in row['checks']:
                    if result['reason'] == 'Not reached':
                        result['reason'] = report['session_reason']
        report['duration_seconds'] = round(time.monotonic() - started, 3)
        report['verdict'] = 'FAIL' if report.get('session_failure') else verdict(
            [check for row in rows for check in row['checks']])
        runner.write_json(root / 'report.json', report)
    return report


def ssh_device(device, name, definitions, root, seconds):
    remote = copy.deepcopy(device['remote'])
    request = uuid.uuid4().hex
    payload = {'device': remote, 'name': name, 'definitions': definitions, 'seconds': seconds, 'request': request}
    encoded = base64.b64encode(json.dumps(payload).encode()).decode()
    executable = device['checkout'].rstrip('/\\') + '/profiles/mhp3rd/tools/device_suite.py'
    args = device.get('python', ['python3']) + [executable, '_remote', encoded]
    if device.get('shell') == 'powershell':
        quote = lambda value: "'" + value.replace("'", "''") + "'"
        code = '& ' + ' '.join(quote(value) for value in args) + '; exit $LASTEXITCODE'
        invocation = 'powershell -NoProfile -EncodedCommand ' + base64.b64encode(code.encode('utf-16le')).decode()
    else:
        invocation = runner.shlex.join(args)
    transport_cap = seconds + remote.get('setup_seconds', 0) + 30
    result = runner.command(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', device['host'], invocation],
                            time.monotonic() + transport_cap, maximum=transport_cap, check=False,
                            return_status=True, output_limit=runner.EVIDENCE_LIMIT * 3 // 2)
    output = result.stdout.decode(errors='replace')
    (root / 'transport.log').write_text(output + result.stderr.decode(errors='replace'))
    matches = re.findall(r'^YAKUMO_SUITE:([A-Za-z0-9+/=]+)\r?$', output, re.M)
    if len(matches) != 1:
        raise ValueError('Expected exactly one remote suite evidence bundle')
    raw = base64.b64decode(matches[0], validate=True)
    if len(raw) > runner.EVIDENCE_LIMIT:
        raise ValueError('Oversized suite evidence')
    with zipfile.ZipFile(io.BytesIO(raw)) as bundle:
        members, names = runner.evidence_members(bundle)
        report = json.loads(bundle.read('report.json'))
        if report.get('request') != request or report.get('name') != name or report.get('commit') != remote['commit'] or \
                report.get('build') != remote['build'] or report.get('definition_sha256') != runner.object_hash(definitions):
            raise ValueError('Remote suite identity differs or is stale')
        if [(row['id'], [item['id'] for item in row['checks']]) for row in report['scenarios']] != \
                [(row['id'], [item['id'] for item in row['checks']]) for row in definitions]:
            raise ValueError('Remote required subchecks differ')
        runner.verify_artifact(remote, report.get('artifact_sha256'))
        runner.verify_artifact(device, report.get('artifact_sha256'))
        expected = 'FAIL' if report.get('session_failure') else verdict([item for row in report['scenarios'] for item in row['checks']])
        if report.get('verdict') != expected or result.returncode != (0 if expected == 'PASS' else 1 if expected == 'FAIL' else 2):
            raise ValueError('Remote aggregate/exit status differs')
        for row in report['scenarios']:
            for item in row['checks']:
                if item.get('verdict') not in runner.VERDICTS or not isinstance(item.get('reason'), str):
                    raise ValueError('Invalid remote check verdict')
                if item['verdict'] == 'PASS' and (not report.get('artifact_sha256') or report.get('cleanup') != 'PASS'):
                    raise ValueError('Remote PASS lacks artifact or cleanup evidence')
        for member in members:
            target = runner.owned_evidence_path(root, member.filename)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as stream:
                stream.write(bundle.read(member))
    return report


def streaming_device(device, name, definitions, root, seconds):
    if not 1 <= seconds <= 300 or device.get('setup_seconds'):
        raise ValueError('Streaming has one inclusive cap of at most 300 seconds; no setup extension')
    ledger_path = Path(device['reservation_ledger'])
    ledger = runner.read_json(ledger_path)
    if ledger.get('issue') != 310 or ledger.get('allowance') != 2 or ledger.get('maximum_seconds') != 300:
        raise ValueError('Streaming requires the task-specific private two-attempt ledger')
    if len(ledger['reservations']) >= 2 or any(row.get('state') != 'released' for row in ledger['reservations']):
        raise runner.NotTested('Cloud allowance exhausted or previous owned reservation is unconfirmed')
    lock = ledger_path.with_suffix('.lock')
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    started = time.monotonic()
    hard = started + seconds
    control = hard - min(30, seconds / 5)
    cli = device.get('android', ['android']) + ['--no-metrics', 'device', 'remote']
    reservation, watchdog, report = None, None, None
    row = None
    try:
        runner.require_idle_project(cli, device['project'], control)
        # Persist consumption before creation: ambiguous activation cannot be retried.
        ledger = runner.read_json(ledger_path)
        if len(ledger['reservations']) >= 2 or any(item.get('state') != 'released' for item in ledger['reservations']):
            raise runner.NotTested('Concurrent/resumed cloud allowance changed')
        row = {'attempt': uuid.uuid4().hex, 'model': device['model'], 'cap_seconds': seconds,
               'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'state': 'attempting',
               'creation_log': str(root / 'reservation-create.log')}
        ledger['reservations'].append(row)
        runner.write_json(ledger_path, ledger)
        creation_log = root / 'reservation-create.log'
        creation_log.touch()
        payload = base64.b64encode(json.dumps({'log': str(creation_log), 'deadline': control,
            'cli': cli, 'project': device['project']}).encode()).decode()
        watchdog = subprocess.Popen([sys.executable, str(Path(runner.__file__).resolve()), '_lease-watchdog', payload],
                                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with creation_log.open('wb') as output:
            creator = subprocess.Popen(cli + ['create', device['model'], '--project=' + device['project']],
                                       stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
            try:
                creator.wait(timeout=runner.remaining(control, 240))
            finally:
                runner.stop_process(creator)
                output.flush()
                reservation = runner.parse_reservation(creation_log.read_text(errors='replace'))
                row['reservation'] = reservation
                runner.write_json(ledger_path, ledger)
        if creator.returncode or not reservation:
            raise RuntimeError('Reservation creation failed/unrecognized; no automatic retry')
        remote = {**device, 'adapter': 'adb', 'serial': runner.parse_adb_serial(creation_log.read_text(errors='replace')),
                  'streaming_cleanup_deadline': hard}
        report = run_device(remote, name, definitions, root, runner.remaining(control, 300))
        if not report.get('launched') and time.monotonic() >= control:
            report.pop('session_failure', None)
            report['verdict'] = 'NOT TESTED'
            report['session_reason'] = 'Cloud preparation consumed the inclusive cap; gameplay incomplete'
        return report
    finally:
        try:
            if reservation:
                runner.command(cli + ['remove', reservation, '--project=' + device['project']], hard,
                               maximum=runner.remaining(hard))
                runner.require_idle_project(cli, device['project'], hard)
                row['state'] = 'released'
                if report is not None:
                    report['reservation_released'] = True
            elif row is not None:
                row['state'] = 'unconfirmed'
        finally:
            if row is not None:
                row['duration_seconds'] = round(time.monotonic() - started, 3)
                runner.write_json(ledger_path, ledger)
            if row is None or row.get('state') == 'released':
                runner.stop_process(watchdog)
            lock.unlink(missing_ok=True)
            if report is not None:
                report['reservation_total_cap_seconds'] = seconds
                report['reservation_duration_seconds'] = round(time.monotonic() - started, 3)
                runner.write_json(root / 'report.json', report)


def remote(payload):
    value = json.loads(base64.b64decode(payload, validate=True))
    device = runner.validate_device(value['device'])
    root = runner.output_directory(tempfile.gettempdir())
    report = run_device(device, runner.safe_name(value['name']), value['definitions'], root, value['seconds'])
    report['request'] = value['request']
    runner.write_json(root / 'report.json', report)
    print('YAKUMO_SUITE:' + base64.b64encode(runner.export_evidence(root)).decode(), flush=True)
    return 0 if report['verdict'] == 'PASS' else 1 if report['verdict'] == 'FAIL' else 2


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == '_remote':
        return remote(argv[1])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config')
    parser.add_argument('--devices', nargs='+')
    parser.add_argument('--scenarios', nargs='+', choices=[f'{i:02}' for i in range(1, 11)])
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--output-parent', default=tempfile.gettempdir())
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args(argv)
    suite = definitions()
    if args.list:
        for item in suite:
            print(item['id'] + ': ' + item['title'] + ' (' + str(len(item['checks'])) + ' automatic subchecks)')
        return 0
    if not args.config or not args.devices or not 1 <= args.seconds <= 1800:
        parser.error('Supply --config, --devices and a total per-device cap of 1..1800 seconds')
    if args.scenarios:
        suite = [value for value in suite if value['id'] in args.scenarios]
    root = runner.output_directory(args.output_parent)
    report = {'schema': 1, 'run_id': uuid.uuid4().hex,
              'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'devices': []}
    devices = [(name, runner.load_device(args.config, name)) for name in args.devices]
    for name, device in devices:
        report['devices'].append({'name': name, 'build': device['build'], 'commit': device['commit'],
            'verdict': 'NOT TESTED', 'scenarios': [{'id': value['id'], 'checks': [
                {'id': check['id'], 'verdict': 'NOT TESTED', 'reason': 'Device session not reached'}
                for check in value['checks']]} for value in suite]})
    render(root, report)
    print('Private suite evidence: ' + str(root), flush=True)
    for index, (name, device) in enumerate(devices):
        target = root / runner.safe_name(name)
        target.mkdir()
        function = ssh_device if device['adapter'] == 'ssh' else streaming_device if device['adapter'] == 'streaming' else run_device
        try:
            result = function(device, name, suite, target, min(args.seconds, device.get('seconds', args.seconds)))
        except (Exception, KeyboardInterrupt) as error:
            result = report['devices'][index]
            result['verdict'] = 'NOT TESTED' if isinstance(error, runner.NotTested) else 'FAIL'
            result['interrupted'] = isinstance(error, KeyboardInterrupt)
            result['session_reason'] = str(error) or type(error).__name__
            for value in result['scenarios']:
                for check in value['checks']:
                    check['reason'] = result['session_reason']
        report['devices'][index] = result
        render(root, report)
        print(name + ': ' + result['verdict'], flush=True)
        if result.get('interrupted'):
            break
    return exit_status(report)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    sys.exit(main())
