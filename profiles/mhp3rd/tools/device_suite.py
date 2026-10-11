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
import socket
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
ACTIONS.add('measure')
ACTIONS.update(('audio', 'text_fields'))
ACTIONS.add('variant')
ACTIONS.update(('peer_start', 'peer_fixture', 'peer_snapshot', 'peer_close'))
ACTIONS.add('focus_cycle')
ACTIONS.add('menu_advance')
ACTIONS.update(('native_contract', 'telemetry'))
CONTRACTS = {
    'installation': ['mhp3rd_data_directory_tests', 'mhp3rd_settings_tests'],
    'saves': ['mhp3rd_savedata_tests'],
    'translations': ['mhp3rd_text_runtime_tests'],
    'mods': ['mhp3rd_mods_tests', 'mhp3rd_texture_pack_tests', 'mhp3rd_equipment_models_tests'],
    'multiplayer': ['mhp3rd_adhoc_tests', 'mhp3rd_kernel_hle_tests'],
    'lifecycle': ['mhp3rd_android_jni_tests', 'mhp3rd_android_display_success', 'mhp3rd_guest_pcm_tests'],
}


def definitions(profile='extended'):
    if profile not in ('quick', 'extended'):
        raise ValueError('Choose the QUICK stage or the deferred extended matrix')
    directory = DEFINITIONS if profile == 'extended' else DEFINITIONS.parent / 'device-quick'
    values = [runner.read_json(path) for path in sorted(directory.glob('*.json'))]
    if len(values) != 10 or [value['id'] for value in values] != [f'{i:02}' for i in range(1, 11)]:
        raise ValueError('Suite must have exactly ten ordered standard scenario IDs')
    for value in values:
        if not value['checks'] or len({item['id'] for item in value['checks']}) != len(value['checks']):
            raise ValueError('Scenario needs unique required subchecks')
        for item in value['checks']:
            runner.safe_name(item['id'])
            validate_program(item['steps'])
            if not any(step['action'] in ('assert', 'image', 'difference', 'native_contract') for step in item['steps']):
                raise ValueError('Every subcheck requires an independent outcome assertion')
    return values


def validate_program(steps, fixture=False):
    if not isinstance(steps, list) or not 0 < len(steps) <= 128:
        raise ValueError('Programs need 1..128 finite steps')
    for step in steps:
        offline = step.get('action') == 'fixture' and re.fullmatch(r'(?:ensure-)?offline-quest-[0-9]{1,5}', step.get('name', ''))
        if step.get('action') not in ACTIONS or fixture and step['action'] == 'fixture' and not offline:
            raise ValueError('Unknown action or recursive fixture')
        if not 0 < step.get('timeout', 20) <= 120:
            raise ValueError('Step deadline must be 0..120 seconds')
        if step['action'] in ('input', 'debug'):
            runner.validate_step({step['action']: step['value']})
        if step['action'] == 'menu_advance':
            runner.validate_step({'input': step['input']})
            if type(step['maximum_presses']) is not int or not 1 <= step['maximum_presses'] <= 4:
                raise ValueError('Menu navigation permits at most four finite presses')
            ready = step.get('ready_colour')
            if ready and (type(ready.get('minimum_pixels')) is not int or not 1 <= ready['minimum_pixels'] <= 4096):
                raise ValueError('Menu readiness needs a bounded positive colour marker count')
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
    lines = ['# Automatic pre-release suite: ' + report.get('profile', 'extended').upper(), '',
             f'Status: **{verdict(report["devices"])}**', '',
             '| Device | Scenario | Subcheck | Evidence kind | Verdict | Reason |', '| --- | --- | --- | --- | --- | --- |']
    def clean(value):
        return str(value).replace('|', '\\|').replace('\n', ' ')
    for device in report['devices']:
        for scenario in device['scenarios']:
            for item in scenario['checks']:
                lines.append('| ' + ' | '.join(map(clean, (device['name'], scenario['id'], item['id'], item.get('evidence_kind', 'Not reached'),
                                                          item['verdict'], item['reason']))) + ' |')
    lines.extend(['', 'Private evidence only. Developer readbacks do not certify exact release artifacts.',
                  'Physical controls, audible playback and long hunts require complementary human sign-off.'])
    checks.confined(root, 'suite.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


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
        self.peer = None
        self.server = None
        self.server_log = None
        self.cap_deadline = deadline

    def prepare(self, launch=True):
        self.adapter.prepare(self.identity)
        if isinstance(self.adapter, runner.Local):
            self.language_env = {key: self.adapter.env.get(key) for key in
                                 ('MHP3RD_LANGUAGE', 'MHP3RD_TRANSLATIONS_DIR', 'MHP3RD_TRACE_TEXT')}
        if self.device.get('audio_capture') and isinstance(self.adapter, runner.Local):
            self.adapter.env.pop('MHP3RD_NO_AUDIO', None)
            self.adapter.env['MHP3RD_AUDIO_DUMP'] = str(self.root / 'audio.wav')
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

    def restart(self, reset=False, retain_variant=False):
        if reset and (getattr(self, 'peer', None) or getattr(self, 'server', None)):
            self.close()
        self.stop()
        if reset:
            if getattr(self, 'variant_active', False) and not retain_variant:
                self.stage_game(Path(self.device['game_dir']))
                for key, value in self.language_env.items():
                    if value is None:
                        self.adapter.env.pop(key, None)
                    else:
                        self.adapter.env[key] = value
                if self.device.get('translation'):
                    target = checks.confined(self.root, 'translations/selected.lang')
                    target.unlink()
                    runner.copy_regular_file(self.device['translation'], target)
                self.variant_active = False
                self.variant_name = None
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
                kind = 'field' if isinstance(probe, dict) else 'text'
                values = [probe['base'], probe['ref']] if kind == 'field' else probe
                if len(values) != (2 if kind == 'field' else 3) or any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in values):
                    raise ValueError('Text probe needs numeric block/table/index')
                suffix = ' ' + kind + ' ' + ' '.join(map(str, values))
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
                        if type(sample.get('schema')) is int and sample['schema'] == 1 and \
                                type(sample.get('request')) is int and sample['request'] == request:
                            sample['_wall_seconds'] = time.monotonic()
                            self.samples.append({key: value for key, value in sample.items() if key != 'frame_ms'})
                            self.samples = self.samples[-128:]
                            if until and set(until) - set(sample):
                                raise runner.NotTested('Required observation fields are unavailable: ' + ', '.join(sorted(set(until) - set(sample))))
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

    def tree(self, name):
        if name != 'data' and not name.startswith('data/'):
            raise ValueError('File inventories require an owned app data path')
        if not isinstance(self.adapter, runner.Adb):
            target = checks.confined(self.root, (self.data_root / name[5:]).relative_to(self.root).as_posix())
            return checks.inventory(target)
        remote = 'files/device-testing/' + name
        listed = self.adapter.app('find ' + runner.shlex.quote(remote) + ' -type f -print').decode('utf-8').splitlines()
        if not 1 <= len(listed) <= 4096:
            raise checks.CheckFailed('Missing or oversized owned app file tree')
        result = {}
        for entry in listed:
            if not entry.startswith(remote + '/'):
                raise ValueError('App file inventory escaped its root')
            relative = entry[len(remote) + 1:]
            checks.confined(self.root, name + '/' + relative)
            if relative in result:
                raise ValueError('Duplicate inventory file')
            output = self.adapter.app('sha256sum ' + runner.shlex.quote(entry)).decode('utf-8')
            digest = output.split()[0]
            if not re.fullmatch('[a-f0-9]{64}', digest):
                raise ValueError('Invalid remote file fingerprint')
            result[relative] = digest
        return result

    def installation(self, unicode=False, update=False):
        if not isinstance(self.adapter, runner.Local):
            raise runner.NotTested('Desktop installer/portable variant requires a local host artifact')
        if update and not self.device.get('baseline_binary'):
            raise runner.NotTested('Update needs a distinct pinned baseline artifact')
        self.stop()
        directory = self.root / ('portable-unicode-\u03a9' if unicode else 'installation')
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
        image = str(checks.confined(self.device['game_dir'], 'disc.iso'))
        output = runner.command([str(binary), '--install', image, '--in-place'], self.deadline,
                                maximum=120, env=env).decode(errors='replace')
        (directory / 'install.log').write_text(output)
        eboot_hash = runner.fingerprint(data / 'EBOOT.ELF')
        checks.compare(eboot_hash, 'eq', runner.fingerprint(Path(self.device['game_dir']) / 'EBOOT.ELF'))
        if not (data / 'disc.iso').exists():
            try:
                (data / 'disc.iso').symlink_to(image)
            except OSError:
                runner.os.link(image, checks.confined(self.root, (data / 'disc.iso').relative_to(self.root).as_posix()))
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

    def variant(self, name):
        variant = self.device.get('game_variants', {}).get(name)
        if not variant:
            raise runner.NotTested('Verified private image/language variant is unavailable: ' + name)
        if not isinstance(self.adapter, runner.Local):
            raise runner.NotTested('Image/language variant staging requires a native local/SSH session')
        directory = Path(variant['game_dir'])
        if runner.fingerprint(directory / 'disc.iso') != variant['game_sha256']:
            raise ValueError('Variant game image differs from its verified fingerprint')
        self.stop()
        self.stage_game(directory)
        language_hash = None
        if variant.get('translation'):
            source = variant['translation']
            language_hash = runner.fingerprint(source)
            if language_hash != variant['translation_sha256']:
                raise ValueError('Variant language fixture changed')
            translations = self.root / 'translations'
            translations.mkdir(exist_ok=True)
            target = translations / 'selected.lang'
            target.unlink(missing_ok=True)
            runner.copy_regular_file(source, target)
            self.adapter.env['MHP3RD_TRANSLATIONS_DIR'] = str(translations)
        self.adapter.env['MHP3RD_LANGUAGE'] = variant['language']
        self.adapter.env['MHP3RD_TRACE_TEXT'] = '1'
        self.variant_active = True
        self.variant_name = name
        self.restart(reset=True, retain_variant=True)
        return {'variant': name, 'game_sha256': variant['game_sha256'], 'language': variant['language'],
                'translation_sha256': language_hash, 'eboot_sha256': runner.fingerprint(self.data_root / 'EBOOT.ELF')}

    def stage_game(self, directory):
        checks.confined(self.root, self.data_root.relative_to(self.root).as_posix())
        for filename in ('EBOOT.ELF', 'disc.iso'):
            target = self.data_root / filename
            target.unlink()
            source = checks.confined(directory, filename)
            if filename == 'EBOOT.ELF':
                runner.copy_regular_file(source, target)
            else:
                try:
                    target.symlink_to(source)
                except OSError:
                    os.link(source, checks.confined(self.root, target.relative_to(self.root).as_posix()))

    def close(self):
        try:
            if getattr(self, 'peer', None):
                self.peer.close()
                self.logs.extend(self.peer.logs)
                self.peer = None
        finally:
            try:
                runner.stop_process(getattr(self, 'server', None))
                self.server = None
                if getattr(self, 'server_log', None):
                    self.server_log.close()
            finally:
                self.stop()
                (self.root / 'run.log').write_text('\n'.join(self.logs), encoding='utf-8')

    def open_peer(self):
        if not isinstance(self.adapter, runner.Local):
            raise runner.NotTested('Two-peer coordinator currently requires a native local/SSH host')
        if self.peer is not None or self.server is not None:
            raise ValueError('Owned peer/server already exists')
        self.stop()
        # Claim two adjacent loopback ports without taking over an existing server.
        with socket.socket() as match, socket.socket() as relay:
            match.bind(('127.0.0.1', 0))
            port = match.getsockname()[1]
            if port >= 65535:
                raise runner.NotTested('No usable bounded loopback port pair')
            relay.bind(('127.0.0.1', port + 1))
        root = self.root / 'peer'
        root.mkdir()
        config = copy.deepcopy(self.device)
        config.pop('audio_capture', None)
        self.peer = Session(config, root, self.deadline)
        self.peer.prepare(launch=False)
        for index, participant in enumerate((self, self.peer)):
            path = participant.file('data/settings.ini')
            with path.open('a', encoding='utf-8') as output:
                output.write(f'\nnetwork.adhoc=1\nnetwork.server=127.0.0.1:{port}\n'
                             f'network.mac=02:31:00:00:00:{index + 1:02x}\nnetwork.nickname=suite-peer-{index}\n')
        self.server_log = (self.root / 'server.log').open('wb')
        self.server = subprocess.Popen([sys.executable, str(Path(runner.__file__).resolve()), '_supervise',
            str(runner.remaining(self.cap_deadline, 1800)), str(self.adapter.binary), '--adhoc-server', str(port)],
            stdin=subprocess.DEVNULL, stdout=self.server_log, stderr=subprocess.STDOUT)
        self.start()
        self.peer.start()
        return {'peer_artifact_sha256': self.peer.identity['artifact_sha256'],
                'peer_fixture_sha256': self.peer.identity['fixture_sha256'], 'port': port}

    def activate(self):
        if not isinstance(self.adapter, runner.Local):
            raise runner.NotTested('Native desktop activation is unavailable on this adapter')
        pid = self.snapshot()['host_pid']
        if type(pid) is not int or pid <= 1 or not self.adapter.alive():
            raise ValueError('No live owned game PID')
        if os.name != 'nt':
            parent = runner.command(['ps', '-o', 'ppid=', '-p', str(pid)], self.deadline).decode().strip()
            if parent != str(self.adapter.process.pid):
                raise ValueError('Activation PID is not the supervised game child')
        if sys.platform == 'darwin':
            code = "ObjC.import('AppKit'); var app=$.NSRunningApplication.runningApplicationWithProcessIdentifier(" + str(pid) + "); app.activateWithOptions(3);"
            runner.command(['osascript', '-l', 'JavaScript', '-e', code], self.deadline)
        elif os.name == 'nt':
            code = "$p=Get-CimInstance Win32_Process -Filter 'ProcessId=" + str(pid) + "'; if ($p.ParentProcessId -ne " + str(self.adapter.process.pid) + ") { throw 'Not the supervised child' }; (New-Object -ComObject WScript.Shell).AppActivate(" + str(pid) + ")"
            runner.command(['powershell', '-NoProfile', '-Command', code], self.deadline)
        elif shutil.which('xdotool'):
            windows = runner.command(['xdotool', 'search', '--pid', str(pid)], self.deadline).decode().splitlines()
            if len(windows) != 1 or not windows[0].isdecimal():
                raise runner.NotTested('Desktop cannot identify exactly one owned game window')
            runner.command(['xdotool', 'windowactivate', '--sync', windows[0]], self.deadline)
        else:
            raise runner.NotTested('Desktop activation needs xdotool on this host')
        return self.snapshot(until={'focus': True})


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
    limit = min(deadline, time.monotonic() + 10)
    sent, next_strike = 0, 0
    while True:
        sample = session.snapshot()
        if not sample.get('on_quest') or sample.get('quest_cheats') or sample.get('free_camera'):
            raise checks.CheckFailed('Combat verification cannot use quest cheats or free camera')
        current = next((item for item in sample['monsters'] if item['address'] == target['address']), None)
        if current is None or not current.get('same_area'):
            raise checks.CheckFailed('Target disappeared before an independently observed hit')
        if current['health'] < target['health']:
            if sent == 0:
                raise checks.CheckFailed('Target health changed before the attack input')
            return {'before_health': target['health'], 'after_health': current['health'], 'address': target['address']}
        runner.remaining(limit)
        # Send the next finite strike only after the previous one had time to land.
        # An early successful hit must not leave future attacks queued for the next check.
        if sent < strikes and sample['vblanks'] >= next_strike:
            session.adapter.send('input', '0:pad y 4')
            next_strike = sample['vblanks'] + 180
            sent += 1
        time.sleep(.1)


def measure(session, profile_name, device, deadline, scenario_hash):
    """Collect six real runs before deriving a noise-aware non-regression budget."""
    profile = device.get('performance_profiles', {}).get(profile_name)
    if not profile or not device.get('baseline_commit') or not device.get('baseline_sha256'):
        raise runner.NotTested('Performance needs a pinned observable baseline and finite workload profile')
    if device['adapter'] != 'local':
        raise runner.NotTested('Repeated performance collection currently requires a native local/SSH session')
    for key in ('prepare', 'route'):
        validate_program(profile[key], fixture=True)
    if profile.get('execution_mode') not in ('aot', 'interpreter'):
        raise ValueError('Declare one execution mode per comparison')
    session.stop()
    results, identity = {'baseline': [], 'candidate': []}, None
    try:
        for artifact in results:
            for repetition in range(3):
                runner.remaining(deadline)
                config = copy.deepcopy(device)
                config.pop('audio_capture', None)
                if artifact == 'baseline':
                    config.update(binary=device['baseline_binary'], commit=device['baseline_commit'],
                                  artifact_sha256=device['baseline_sha256'])
                root = session.root / f'perf-{runner.safe_name(profile_name)}-{artifact}-{repetition}'
                root.mkdir()
                child = Session(config, root, deadline)
                started = time.monotonic()
                try:
                    child.prepare()
                    child.snapshot(until={'task': 'demo_task.ovl'})
                    startup_ms = (time.monotonic() - started) * 1000
                    loading = time.monotonic()
                    execute(child, profile['prepare'], {}, {}, config, deadline)
                    loaded = child.snapshot(until=profile['state'])
                    load_ms = [(time.monotonic() - loading) * 1000]
                    settings_hash = runner.fingerprint(child.file('data/settings.ini'))
                    execute(child, profile['route'], {}, {}, config, deadline)
                    child.snapshot(timeout=10, until={'vblanks': {'op': 'ge', 'value': loaded['vblanks'] + 120}})
                    speeds, seconds_seen = [], set()
                    steady = time.monotonic() + 7
                    while time.monotonic() < steady or len(speeds) < 3:
                        sample = child.snapshot(performance=True)
                        if sample['fast_forward'] or sample['fast_loading'] or sample['quest_cheats'] or sample['photo']:
                            raise checks.CheckFailed('Timing route is accelerated, paused or uses quest cheats')
                        perf_lines = re.findall(r'\[perf\] fps [^\n]+', child.adapter.text())
                        # A new logged second is independent of observation polling frequency.
                        if len(perf_lines) not in seconds_seen and sample.get('perf_valid'):
                            seconds_seen.add(len(perf_lines))
                            speeds.append(sample['speed_percent'])
                        time.sleep(min(.2, runner.remaining(deadline)))
                    path = child.capture('timing-resolution')
                    width, height, _ = checks.pixels(checks.confined(root, path))
                    gpu = re.findall(r'^\[gpu\] (.+driver .+)$', child.adapter.text(), re.M)
                    if len(gpu) != 1:
                        raise runner.NotTested('Actual GPU/driver identity is unavailable or changed')
                    current = {'device': device.get('hardware', {}).get('device', device.get('name', 'local')),
                               'gpu': gpu[0], 'driver': gpu[0], 'resolution': [width, height],
                               'execution_mode': profile['execution_mode'],
                               'route_sha256': runner.object_hash(profile),
                               'fixture_sha256': child.identity['fixture_sha256'],
                               'settings_sha256': settings_hash, 'scenario_sha256': scenario_hash}
                    identity = identity or current
                    checks.compare(current, 'eq', identity)
                    results[artifact].append({'identity': current, 'artifact_sha256': child.identity['artifact_sha256'],
                        'source_commit': config['commit'], 'game_sha256': child.identity['game_sha256'],
                        'warmup_frames': (sample['vblanks'] - loaded['vblanks']) // 2,
                        'frame_ms': sample['frame_ms'], 'speed_percent': speeds,
                        'startup_ms': startup_ms, 'load_ms': load_ms, 'presented_fps': sample['presented_fps']})
                finally:
                    child.close()
                    session.logs.extend(child.logs)
        calibration = checks.performance_comparison(results['baseline'], results['candidate'], identity)
        budget = {}
        for metric, value in calibration['metrics'].items():
            if value['baseline_spread'] > max(1, value['baseline'] * .2):
                raise runner.NotTested('Baseline noise is too large for a regression decision: ' + metric)
            budget[metric] = max(value['baseline_spread'] * 2,
                                 1 if metric == 'speed_percent' else value['baseline'] * .05)
        comparison = checks.performance_comparison(results['baseline'], results['candidate'], identity, budget)
        checks.compare(comparison['metrics']['speed_percent']['candidate'], 'ge', 99)
        return {'runs': results, 'calibration': calibration, 'budget': budget, 'comparison': comparison}
    finally:
        session.deadline = session.adapter.deadline = deadline
        session.start()


def offline_quest(session, quest_id, context, parameters, device, deadline, reuse=False):
    """Reusable offline setup, with actual state assertions after the existing shortcut."""
    if parameters.get('multiplayer'):
        raise ValueError('Offline quest setup is forbidden in multiplayer')
    if device['build'] != 'dev':
        raise runner.NotTested('Offline quest setup needs developer tools; release-player UI is separate')
    if not 2 <= quest_id <= 65535:
        raise ValueError('Offline setup needs a valid nonempty quest ID')
    if reuse:
        current = session.snapshot()
        if condition(current, {'task': 'game_task.ovl', 'on_quest': True, 'active_id': quest_id,
                               'quest_cheats': False, 'online': False, 'fast_loading': False}):
            later = session.snapshot(until={'quest_ticks': {'op': 'lt', 'value': current['quest_ticks']}})
            context['_offline_setup'] = {'method': 'reused verified offline quest', 'quest_id': quest_id,
                                        'loaded': current, 'advancing': later}
            return 2
    session.restart(reset=True, retain_variant=True)
    variant = device.get('game_variants', {}).get(getattr(session, 'variant_name', None), {})
    load = variant.get('suite_fixtures', {}).get('load-character') or device.get('suite_fixtures', {}).get('load-character')
    if not load:
        raise runner.NotTested('Missing finite copied-character navigation for offline setup')
    validate_program(load, fixture=True)
    count = execute(session, load, context, parameters, device, deadline)
    ready = session.snapshot(until={'loaded': True, 'task': 'lobby_task.ovl', 'village_ready': True,
                                   'fast_loading': False, 'on_quest': False})
    checks.compare(ready['quest_cheats'], 'eq', False)
    checks.compare(ready['online'], 'eq', False)
    session.adapter.send('debug', f'quest start {quest_id}')
    quest = session.snapshot(timeout=min(40, runner.remaining(deadline)),
        until={'task': 'game_task.ovl', 'on_quest': True, 'active_id': quest_id, 'fast_loading': False,
               'quest_ticks': {'op': 'lt', 'value': 89970}})
    checks.compare(quest['accepted_id'], 'eq', quest_id)
    checks.compare(quest['quest_cheats'], 'eq', False)
    checks.compare(quest['quest_ticks'], 'gt', 0)
    checks.compare(quest['map'].startswith('P_m'), 'eq', True)
    later = session.snapshot(until={'quest_ticks': {'op': 'lt', 'value': quest['quest_ticks']}})
    checks.compare(later['active_id'], 'eq', quest_id)
    context['_offline_setup'] = {'method': 'developer quest start', 'quest_id': quest_id,
                                  'village': ready, 'loaded': quest, 'advancing': later}
    return count + 7


def native_contract(session, group, device, deadline):
    if group not in CONTRACTS:
        raise ValueError('Unknown public native contract group')
    if not isinstance(session.adapter, runner.Local) or not device.get('native_test_dir'):
        raise runner.NotTested('This adapter/configuration has no executable public native contract build')
    build = checks.confined(runner.ROOT, device['native_test_dir'])
    tests = json.loads(runner.command(['ctest', '--test-dir', str(build), '--show-only=json-v1'], deadline).decode())['tests']
    selected = [item for item in tests if item['name'] in CONTRACTS[group]]
    if {item['name'] for item in selected} != set(CONTRACTS[group]):
        raise runner.NotTested('Required public native contracts are absent from the registered build')
    artifacts = {}
    for test in selected:
        path = checks.confined(build, Path(test['command'][0]).relative_to(build).as_posix())
        artifacts[test['name']] = runner.fingerprint(path)
    output = runner.command(['ctest', '--test-dir', str(build), '-R', '^(' + '|'.join(CONTRACTS[group]) + ')$',
                             '--output-on-failure', '--no-tests=error', '--timeout', '60'], deadline,
                            maximum=runner.remaining(deadline, 120)).decode(errors='replace')
    if '100% tests passed' not in output:
        raise checks.CheckFailed('Native contract runner did not report all registered checks passed')
    path = session.root / ('contract-' + runner.safe_name(group) + '.log')
    path.write_text(output, encoding='utf-8')
    return {'evidence_kind': 'synthetic/native contract', 'tests': CONTRACTS[group], 'artifact_sha256': artifacts,
            'log': path.name, 'all_passed': True, 'scope': 'Public synthetic inputs; no real-game release sign-off'}


def quick_telemetry(session, deadline):
    first = session.snapshot(performance=True)
    resolution = [first.get('target_width'), first.get('target_height')]
    if any(type(value) is not int or value <= 0 for value in resolution):
        raise runner.NotTested('Actual render resolution is unavailable')
    session.snapshot(until={'vblanks': {'op': 'ge', 'value': first['vblanks'] + 60}})
    values, seen = [], set()
    until = min(deadline, time.monotonic() + 8)
    while len(values) < 3:
        sample = session.snapshot(performance=True)
        if sample['fast_loading'] or sample['fast_forward'] or sample['quest_cheats'] or sample['photo']:
            raise checks.CheckFailed('Quick telemetry requires an unpaused, ordinary fixed scene')
        checks.compare(sample['map'], 'eq', first['map'])
        checks.compare([sample.get('target_width'), sample.get('target_height')], 'eq', resolution)
        if sample.get('perf_valid') and sample['perf_second'] not in seen:
            checks.compare(sample['speed_percent'], 'gt', 0)
            frames = [value for value in sample['frame_ms'][-30:] if value > 0]
            if len(frames) < 30 or any(not math.isfinite(value) for value in frames):
                raise checks.CheckFailed('Missing or invalid short frame-time sample')
            values.append({'speed_percent': sample['speed_percent'], 'game_fps': sample['game_fps'],
                           'presented_fps': sample['presented_fps'], 'frame_ms': frames,
                           'p95_ms': checks.percentile(frames, .95), 'p99_ms': checks.percentile(frames, .99)})
            seen.add(sample['perf_second'])
        runner.remaining(until)
        time.sleep(.1)
    gpu = re.findall(r'^\[gpu\] (.+driver .+)$', session.adapter.text(), re.M)
    if len(gpu) != 1:
        raise runner.NotTested('Actual GPU/driver identity is unavailable or ambiguous')
    return {'collected': True, 'evidence_kind': 'real-game smoke', 'scene': first['map'], 'samples': values,
            'gpu_driver': gpu[0], 'resolution': resolution,
            'comparison': 'Deferred: no calibrated regression budget or improvement claim'}


def execute(session, steps, context, parameters, device, deadline):
    assertions = 0
    for step in steps:
        runner.remaining(deadline)
        action = step['action']
        name = step.get('as', '')
        variant = device.get('game_variants', {}).get(getattr(session, 'variant_name', None), {})
        if action == 'fixture':
            offline = re.fullmatch(r'(ensure-)?offline-quest-([0-9]{1,5})', step['name'])
            if offline:
                assertions += offline_quest(session, int(offline[2]), context, parameters, device, deadline, bool(offline[1]))
                continue
            fixture = variant.get('suite_fixtures', {}).get(step['name']) or device.get('suite_fixtures', {}).get(step['name'])
            if not fixture:
                raise runner.NotTested('Missing finite fixture navigation: ' + step['name'])
            validate_program(fixture, fixture=True)
            assertions += execute(session, fixture, context, parameters, device, deadline)
        elif action in ('input', 'debug'):
            if action == 'debug':
                command = step['value'].split()[0]
                if device['build'] != 'dev' or command not in ('quest', 'state', 'peek', 'table'):
                    raise runner.NotTested('Only declared developer navigation/read commands are supported')
                if command == 'quest' and parameters.get('multiplayer'):
                    raise ValueError('Quest mutations and shortcuts are forbidden in multiplayer')
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
            context[name] = session.tree(step['path'])
        elif action == 'capture':
            context[name] = session.capture(name)
        elif action in ('reference', 'menu_advance'):
            reference = variant.get('navigation_references', {}).get(step['name']) or device.get('navigation_references', {}).get(step['name'])
            if not reference or not reference.get('reviewed') or reference.get('hardware') != device.get('hardware'):
                raise runner.NotTested('Missing reviewed navigation reference for this hardware: ' + step['name'])
            if runner.fingerprint(reference['path']) != reference['sha256']:
                raise ValueError('Navigation reference changed')
            reference_path = Path(reference['path'])
            expected = checks.pixels(checks.confined(reference_path.parent, reference_path.name))
            limit = min(deadline, time.monotonic() + step.get('timeout', 20))
            first_capture, previous_capture = None, None
            presses = 0
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
                    if action == 'menu_advance':
                        ready = step.get('ready_colour')
                        if ready and checks.colour_count(checks.pixels(checks.confined(session.root, path)),
                                ready['region'], ready['rgb'], ready.get('tolerance', 20)) < ready['minimum_pixels']:
                            time.sleep(.5)
                            continue
                        if presses >= step['maximum_presses']:
                            raise checks.CheckFailed('Finite menu navigation exhausted without its observed target')
                        before = session.snapshot()['vblanks']
                        session.adapter.send('input', step['input'])
                        presses += 1
                        session.snapshot(timeout=min(10, runner.remaining(limit)),
                                         until={'vblanks': {'op': 'ge', 'value': before + 360}})
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
        elif action == 'measure':
            context[name] = measure(session, step['profile'], device, deadline, runner.object_hash(steps))
        elif action == 'native_contract':
            context[name] = native_contract(session, step['group'], device, deadline)
            assertions += len(context[name]['tests'])
        elif action == 'telemetry':
            context[name] = quick_telemetry(session, deadline)
        elif action == 'audio':
            if not device.get('audio_capture'):
                raise runner.NotTested('Guest PCM capture is unavailable in this artifact/session contract')
            limit = min(deadline, time.monotonic() + step.get('timeout', 20))
            while True:
                try:
                    context[name] = checks.audio_content(checks.confined(session.root, 'audio.wav'))
                    assertions += 1
                    break
                except (OSError, checks.CheckFailed):
                    runner.remaining(limit)
                    time.sleep(.2)
        elif action == 'text_fields':
            rows = parameters.get(step['name'])
            if not rows:
                raise runner.NotTested('Independent source/language field expectations are missing: ' + step['name'])
            if not isinstance(rows, list) or not 1 <= len(rows) <= 4096:
                raise ValueError('Text checks need 1..4096 bounded field expectations')
            context[name] = []
            for row in rows:
                if row.get('coverage') not in ('covered', 'original-fallback') or not isinstance(row['expected_hex'], str):
                    raise ValueError('Distinguish missing language keys from runtime failures')
                probe = row['probe']
                if isinstance(probe, dict) and 'active_ref_delta' in probe:
                    active = session.snapshot(until={'on_quest': True})
                    delta = probe['active_ref_delta']
                    if type(delta) is not int or not 0 <= delta < 65536 or delta % 4:
                        raise ValueError('Active field references must come from the validated source catalog')
                    probe = {'base': 0x08A3A630, 'ref': active['active_record_offset'] + delta}
                elif isinstance(probe, dict) and 'entry' in probe:
                    limit = min(deadline, time.monotonic() + step.get('timeout', 20))
                    while True:
                        loaded = session.snapshot()
                        block = next((item for item in loaded.get('text_blocks', []) if item['entry'] == probe['entry'] and item['address']), None)
                        if block:
                            base_offset = probe.get('base_offset', 0)
                            if type(base_offset) is not int or not 0 <= base_offset < 1024 * 1024:
                                raise ValueError('Text block base offset must come from a bounded source layout')
                            probe = {'base': block['address'] + base_offset, 'ref': probe['ref']}
                            break
                        runner.remaining(limit)
                        time.sleep(.1)
                sample = session.snapshot(timeout=step.get('timeout', 20), probe=probe,
                                          until={'text_hex': row['expected_hex']})
                checks.compare(sample['text_hex'], 'eq', row['expected_hex'])
                context[name].append({'coverage': row['coverage'], 'probe': probe,
                                      'expected_hex': row['expected_hex'], 'observed_hex': sample['text_hex'],
                                      'request': sample['request']})
                assertions += 1
        elif action == 'variant':
            context[name] = session.variant(step['name'])
        elif action == 'peer_start':
            context[name] = session.open_peer()
        elif action in ('peer_fixture', 'peer_snapshot', 'peer_close'):
            if not session.peer:
                raise runner.NotTested('An independently owned second game peer is required')
            session.peer.deadline = session.peer.adapter.deadline = deadline
            if action == 'peer_fixture':
                fixture = session.peer.device.get('suite_fixtures', {}).get(step['name'])
                if not fixture:
                    raise runner.NotTested('Missing finite peer navigation: ' + step['name'])
                validate_program(fixture, fixture=True)
                context[name] = {}
                assertions += execute(session.peer, fixture, context[name], {**parameters, 'multiplayer': True},
                                      session.peer.device, deadline)
            elif action == 'peer_snapshot':
                context[name] = session.peer.snapshot(step.get('timeout', 20), step.get('until'))
                checks.compare(context[name]['quest_cheats'], 'eq', False)
            else:
                session.peer.close()
                session.logs.extend(session.peer.logs)
                session.peer = None
        elif action == 'focus_cycle':
            before = session.activate()
            session.adapter.send('input', '0:window minimize')
            background = session.snapshot(until={'focus': False})
            session.adapter.send('input', '0:window restore')
            returned = session.activate()
            checks.compare(returned['clock_us'], 'gt', before['clock_us'])
            checks.compare(returned['pad_buttons'], 'eq', 0)
            context[name] = {'before': before, 'background': background, 'returned': returned}
            assertions += 4
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
        report['setup_duration_seconds'] = round(time.monotonic() - started, 3)
        report['setup_cap_seconds'] = setup
        runner.remaining(deadline)
        deadline = min(hard_deadline, time.monotonic() + seconds)
        session.deadline = session.adapter.deadline = deadline
        session.cap_deadline = deadline
        session.start()
        report['launched'] = True
        report.update(session.identity)
        quick = bool(suite and suite[0].get('profile') == 'quick')
        report['profile'] = 'quick' if quick else 'extended'
        for definition, row in zip(suite, rows):
            if definition is not suite[0] and not quick:
                session.restart(reset=True)
            for check, result in zip(definition['checks'], row['checks']):
                context = {}
                result['evidence_kind'] = check.get('evidence_kind', 'real-game/device scenario')
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
                    result.update(verdict='FAIL', reason='Owned game exited during subcheck; observed exit code ' + str(code))
                if result['verdict'] != 'PASS' and check is not definition['checks'][-1]:
                    # Preserve the original failure, then reset only owned copies.
                    # A failed UI route must not become setup for the next core check.
                    session.deadline = session.adapter.deadline = deadline
                    session.restart(reset=True, retain_variant=True)
                    report.setdefault('recovery', []).append({'scenario': definition['id'], 'after': check['id'],
                                                              'method': 'owned isolated restart'})
    except runner.NotTested as error:
        report['session_reason'] = str(error)
    except (Exception, KeyboardInterrupt) as error:
        report['session_reason'] = str(error) or type(error).__name__
        report['session_failure'] = True
        report['interrupted'] = isinstance(error, KeyboardInterrupt)
    finally:
        if session:
            # Preserve verified artifact/capability evidence even if setup failed.
            report.update(session.identity)
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
    unavailable = re.search(r'(?:(?:Connection|Operation) timed out|Connection refused|No route to host|Could not resolve hostname)',
                            result.stderr.decode(errors='replace'), re.I)
    if result.returncode == 255 and not output and unavailable:
        raise runner.NotTested('SSH device is unavailable: ' + unavailable[0])
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


def cloud_authorization(ledger, device, seconds):
    if ledger.get('issue') != 310 or ledger.get('allowance') != 2:
        raise ValueError('Streaming requires the task-specific private authorization ledger')
    phase = device.get('authorization_phase')
    if phase:
        runner.safe_name(phase)
        matches = [value for value in ledger.get('authorization_phases', []) if value.get('id') == phase]
        if len(matches) != 1:
            raise ValueError('Select one explicitly authorized phase; history cannot be reset')
        authorization = matches[0]
        prior = [row for row in ledger['reservations'] if not row.get('authorization_phase')]
        if authorization.get('previous_total_attempts') != 2 or len(prior) != 2 or ledger.get('maximum_seconds') != 500:
            raise ValueError('The rerun phase must retain both exhausted historical attempts and their authorization')
        if authorization.get('allowance') != 2 or authorization.get('maximum_seconds') != 800 or \
                authorization.get('rate_usd_per_minute') != .15 or authorization.get('maximum_cost_usd') != 2:
            raise ValueError('Rerun phase requires two attempts, 800 seconds and the verified per-device cost limit')
        models = authorization.get('models', [])
        if len(models) != 2 or len(set(models)) != 2 or device['model'] not in models:
            raise ValueError('Device model is outside the finite authorized pair')
        rows = [row for row in ledger['reservations'] if row.get('authorization_phase') == phase]
        if any(row['model'] == device['model'] for row in rows):
            raise runner.NotTested('The authorized model attempt has already been consumed; no retry')
    else:
        authorization = ledger
        if type(authorization.get('maximum_seconds')) is not int or not 1 <= authorization['maximum_seconds'] <= 500:
            raise ValueError('Legacy authorization remains capped at 500 seconds')
        rows = [row for row in ledger['reservations'] if not row.get('authorization_phase')]
    if runner.streaming_limit(device) > authorization['maximum_seconds'] or seconds > authorization['maximum_seconds']:
        raise ValueError('Configured cap exceeds the selected authorization')
    if len(rows) >= authorization['allowance'] or any(row.get('state') != 'released' for row in ledger['reservations']):
        raise runner.NotTested('Allowance exhausted or an earlier owned reservation is unconfirmed')
    estimate = max(60, seconds) * authorization.get('rate_usd_per_minute', .15) / 60
    if phase and estimate > authorization['maximum_cost_usd']:
        raise ValueError('Selected duration/rate exceeds the authorized per-device cost')
    return authorization


def streaming_device(device, name, definitions, root, seconds):
    maximum = runner.streaming_limit(device)
    if not 1 <= seconds <= maximum or device.get('setup_seconds'):
        raise ValueError('Streaming exceeds its explicit inclusive cap; no setup extension')
    ledger_name = os.path.normcase(os.path.realpath(device['reservation_ledger']))
    private_root = os.path.normcase(os.path.realpath(runner.ROOT / 'out'))
    temporary_root = os.path.normcase(os.path.realpath(tempfile.gettempdir()))
    if not (ledger_name.startswith(private_root.rstrip(os.sep) + os.sep) or
            ledger_name.startswith(temporary_root.rstrip(os.sep) + os.sep)):
        raise ValueError('Reservation ledger must remain in private out/ or temporary storage')
    ledger_path = Path(ledger_name)
    ledger = runner.read_json(ledger_path)
    cloud_authorization(ledger, device, seconds)
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
        runner.require_idle_project(cli, device['project'], control, root / 'reservation-preflight.json')
        # Persist consumption before creation: ambiguous activation cannot be retried.
        ledger = runner.read_json(ledger_path)
        authorization = cloud_authorization(ledger, device, seconds)
        row = {'attempt': uuid.uuid4().hex, 'model': device['model'], 'cap_seconds': seconds,
               'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'state': 'attempting',
               'creation_log': str(root / 'reservation-create.log')}
        if device.get('authorization_phase'):
            row.update(authorization_phase=device['authorization_phase'], rate_usd_per_minute=.15,
                       target_seconds=authorization.get('target_seconds', seconds),
                       maximum_estimated_cost_usd=round(max(60, seconds) * .15 / 60, 4))
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
                  'streaming_cleanup_deadline': hard,
                  'gameplay_reserve_seconds': device.get('gameplay_reserve_seconds', min(120, int(seconds / 4)))}
        report = run_device(remote, name, definitions, root, runner.remaining(control, seconds))
        if not report.get('launched') and time.monotonic() >= control:
            report.pop('session_failure', None)
            report['verdict'] = 'NOT TESTED'
            report['session_reason'] = 'Cloud preparation consumed the inclusive cap; gameplay incomplete'
        return report
    finally:
        try:
            if reservation:
                removal = runner.command(cli + ['remove', reservation, '--project=' + device['project']], hard,
                                         maximum=runner.remaining(hard), check=False, return_status=True)
                runner.write_json(root / 'reservation-remove.json', {'returncode': removal.returncode,
                    'stdout': removal.stdout.decode(errors='replace'), 'stderr': removal.stderr.decode(errors='replace')})
                if removal.returncode:
                    raise RuntimeError('Owned reservation removal failed; inspect private cleanup evidence')
                runner.require_idle_project(cli, device['project'], hard, root / 'reservation-final-list.json')
                row['state'] = 'released'
                if report is not None:
                    report['reservation_released'] = True
            elif row is not None:
                row['state'] = 'unconfirmed'
        finally:
            if row is not None:
                row['duration_seconds'] = round(time.monotonic() - started, 3)
                if row.get('authorization_phase'):
                    row['estimated_cost_usd'] = round(max(60, row['duration_seconds']) * row['rate_usd_per_minute'] / 60, 4)
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
    parser.add_argument('--profile', choices=('quick', 'extended'), default='quick')
    parser.add_argument('--seconds', type=int, help='Per-device cap (QUICK: 360; extended: 1800)')
    parser.add_argument('--output-parent', default=tempfile.gettempdir())
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args(argv)
    if args.seconds is None:
        args.seconds = 360 if args.profile == 'quick' else 1800
    suite = definitions(args.profile)
    if args.list:
        for item in suite:
            print(item['id'] + ': ' + item['title'] + ' (' + str(len(item['checks'])) + ' automatic subchecks)')
        return 0
    if not args.config or not args.devices or not 1 <= args.seconds <= 1800:
        parser.error('Supply --config, --devices and a total per-device cap of 1..1800 seconds')
    if args.scenarios:
        suite = [value for value in suite if value['id'] in args.scenarios]
    root = runner.output_directory(args.output_parent)
    report = {'schema': 1, 'run_id': uuid.uuid4().hex, 'profile': args.profile,
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
        target = checks.confined(root, runner.safe_name(name))
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
