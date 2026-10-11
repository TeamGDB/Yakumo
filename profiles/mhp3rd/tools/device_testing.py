#!/usr/bin/env python3
"""Run bounded device scenarios and keep automatic evidence separate from review."""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
SCENARIOS = ROOT / 'profiles/mhp3rd/tests/scenarios'
VERDICTS = ('PASS', 'FAIL', 'NOT TESTED')
NAME = re.compile(r'[a-z0-9][a-z0-9_-]{0,63}\Z')
ENV_KEYS = {'MHP3RD_LANGUAGE', 'MHP3RD_TRACE_TEXT', 'MHP3RD_NO_RENDER',
            'MHP3RD_NO_AUDIO', 'MHP3RD_FONT', 'MHP3RD_GPU_COMPAT',
            'MHP3RD_TEXT_NO_QUEST_DIRECT', 'MHP3RD_TEXT_SEARCH_UNLIMITED'}


class NotTested(ValueError):
    """An unmet prerequisite is a visible coverage gap, not a pass."""


def read_json(path):
    """Read a file deliberately selected by the local tester, not a remote request."""
    return json.loads(Path(path).read_text(encoding='utf-8'))


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def fingerprint(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def object_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def safe_name(value):
    if not isinstance(value, str) or not NAME.fullmatch(value):
        raise ValueError('Names must contain only lower-case letters, numbers, _ or -')
    return value


def validate_step(step):
    if not isinstance(step, dict) or len(set(step) & {'wait', 'input', 'debug', 'capture', 'fixture'}) != 1:
        raise ValueError('Each step needs one wait/input/debug/capture/fixture action')
    if set(step) - {'wait', 'input', 'debug', 'capture', 'fixture', 'timeout', 'fresh'}:
        raise ValueError('Unknown step field')
    if not 0 < step.get('timeout', 20) <= 1800:
        raise ValueError('Step timeout must be positive and at most 1800 seconds')
    if 'wait' in step:
        try:
            re.compile(step['wait'])
        except re.error as error:
            raise ValueError('Invalid wait expression') from error
    for kind in ('input', 'debug'):
        if kind in step and (not isinstance(step[kind], str) or '\n' in step[kind] or not step[kind]):
            raise ValueError('Commands must be a single nonempty line')
    if 'input' in step:
        frame, separator, action = step['input'].partition(':')
        if not separator or not frame.isascii() or not frame.isdecimal() or not action or ';' in action or len(step['input']) > 512:
            raise ValueError('Input must be one finite frame:action command')
    for kind in ('capture', 'fixture'):
        if kind in step:
            safe_name(step[kind])


def validate_scenario(value):
    safe_name(value['id'])
    if value.get('build') not in ('dev', 'release', 'either'):
        raise ValueError('Scenario build must be dev, release or either')
    if value.get('execution') not in ('automatic', 'manual'):
        raise ValueError('Scenario execution must be automatic or manual')
    if not 1 <= value['seconds'] <= 1800:
        raise ValueError('Scenario deadline must be 1..1800 seconds')
    if not value.get('prerequisites') or not value.get('description'):
        raise ValueError('Describe prerequisites and expected behavior')
    for step in value.get('steps', []):
        validate_step(step)
        if 'debug' in step and value['build'] != 'dev':
            raise ValueError('Debug commands are restricted to dev scenarios')
    checks = value.get('review', [])
    keys = [safe_name(check['id']) for check in checks]
    if len(keys) != len(set(keys)) or not checks or any(not item.get('expect') or item.get('requires', 'either') not in ('human', 'agent', 'either') for item in checks):
        raise ValueError('Unique review checks with explicit expectations are required')
    if value['execution'] == 'automatic' and not any('wait' in item for item in value.get('steps', [])):
        raise ValueError('An automatic scenario must observe expected behavior')
    return value


def scenario(name):
    return validate_scenario(read_json(SCENARIOS / (safe_name(name) + '.json')))


def load_device(config, name):
    value = copy.deepcopy(read_json(config)['devices'][safe_name(name)])
    if value.get('adapter') not in ('local', 'ssh', 'adb', 'streaming'):
        raise ValueError('Unknown device adapter')
    if value.get('build') not in ('dev', 'release'):
        raise ValueError('Declare the actual artifact build type')
    if not re.fullmatch(r'[0-9a-f]{40}', value.get('commit', '')):
        raise ValueError('Declare the exact 40-character source commit')
    if not 1 <= value.get('seconds', 60) <= 1800:
        raise ValueError('Device cap must be 1..1800 seconds')
    if set(value.get('env', {})) - ENV_KEYS:
        raise ValueError('Unsupported environment override; isolation controls cannot be replaced')
    for steps in value.get('fixtures', {}).values():
        for step in steps:
            validate_step(step)
            if 'fixture' in step:
                raise ValueError('Fixture navigation cannot recursively invoke fixtures')
            if 'debug' in step and value['build'] != 'dev':
                raise ValueError('Release fixture cannot use debug commands')
    if value['adapter'] == 'ssh':
        remote = value['remote']
        if remote.get('build') != value['build'] or remote.get('commit') != value['commit']:
            raise ValueError('SSH wrapper and remote artifact identity must match')
    return value


def output_directory(parent):
    parent = Path(parent).resolve()
    allowed = (Path(tempfile.gettempdir()).resolve(), (ROOT / 'out').resolve())
    if not parent.is_dir() or not any(parent == item or item in parent.parents for item in allowed):
        raise ValueError('Output parent must exist under repository out/ or the OS temporary directory')
    return Path(tempfile.mkdtemp(prefix='yakumo-device-', dir=parent))


def copy_fixture(source, destination):
    source = Path(source).resolve()
    if not source.is_dir():
        raise NotTested('Missing copied-save fixture')
    # Reject all links, including links into the source: no alias to player files.
    for item in source.rglob('*'):
        if item.is_symlink() or not (item.is_dir() or item.is_file()):
            raise ValueError('Fixture must not contain symlinks or special files')
    shutil.copytree(source, destination)
    return object_hash([(item.relative_to(source).as_posix(), fingerprint(item))
                        for item in sorted(source.rglob('*')) if item.is_file()])


def remaining(deadline, maximum=20):
    seconds = min(maximum, deadline - time.monotonic())
    if seconds <= 0:
        raise TimeoutError('Overall session deadline exceeded')
    return seconds


def command(argv, deadline, *, data=None, check=True, maximum=20):
    """Execute trusted configured tools with argv boundaries and a time limit."""
    result = subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=remaining(deadline, maximum), check=False)
    if check and result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {(result.stderr or result.stdout).decode(errors="replace")[-1500:]}')
    return result.stdout


def stop_process(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=8)


class Local:
    def __init__(self, device, root, deadline):
        self.device, self.root, self.deadline = device, root, deadline
        self.process = None
        self.log = None
        self.owned = False

    def prepare(self, report):
        binary = Path(self.device['binary']).resolve()
        if not binary.is_file():
            raise NotTested('Executable is unavailable')
        report['artifact_sha256'] = fingerprint(binary)
        report['hardware'] = {'os': platform.platform(), **self.device.get('hardware', {})}
        data = self.root / 'data'
        data.mkdir()
        report['fixture_sha256'] = copy_fixture(self.device['saves'], data / 'ms0')
        settings = self.device.get('settings')
        if settings:
            if Path(settings).is_symlink():
                raise ValueError('Settings fixture must not be a symlink')
            shutil.copyfile(settings, data / 'settings.ini')
            report['settings_sha256'] = fingerprint(data / 'settings.ini')
        # Both legacy and current save roots point into the disposable directory.
        for name in ('EBOOT.ELF', 'disc.iso'):
            source = Path(self.device['game_dir']) / name
            if not source.is_file():
                raise NotTested('Prepared game input is unavailable')
            try:
                (data / name).symlink_to(source.resolve())
            except OSError:
                # Windows without link privileges can make a read-only hard link.
                os.link(source, data / name)
        report['game_sha256'] = fingerprint(data / 'disc.iso')
        (self.root / 'shots').mkdir()
        for name in ('input-live.txt', 'debug.txt'):
            (self.root / name).touch()
        if self.device.get('translation'):
            (self.root / 'translations').mkdir()
            destination = self.root / 'translations/selected.lang'
            shutil.copyfile(self.device['translation'], destination)
            report['translation_sha256'] = fingerprint(destination)
        env = {key: value for key, value in os.environ.items() if not key.startswith('MHP3RD_')}
        env.update(self.device.get('env', {}))
        env.update(MHP3RD_GAME_DIR=str(data), MHP3RD_DATA_DIR=str(data),
                   MHP3RD_INPUT_LIVE=str(self.root / 'input-live.txt'),
                   MHP3RD_DEBUG_COMMANDS=str(self.root / 'debug.txt'),
                   MHP3RD_SCREENSHOT_DIR=str(self.root / 'shots'), MHP3RD_SCREENSHOT_EVERY='0',
                   MHP3RD_PERF='log', MHP3RD_WINDOW_TITLE='Yakumo device scenario')
        if self.device.get('overlays'):
            env['MHP3RD_OVERLAY_DIR'] = str(Path(self.device['overlays']).resolve())
        if self.device['build'] == 'dev':
            env['MHP3RD_DEBUG_MENU'] = '1'
        if self.device.get('translation'):
            env['MHP3RD_TRANSLATIONS_DIR'] = str(self.root / 'translations')
        self.env, self.binary = env, binary

    def launch(self):
        self.log = (self.root / 'run.log').open('wb')
        # A separate bounded supervisor survives a dropped SSH/control connection.
        self.process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_supervise',
                                         str(remaining(self.deadline, 1800)), str(self.binary), *self.device.get('args', [])],
                                        cwd=self.binary.parent, env=self.env,
                                        stdout=self.log, stderr=subprocess.STDOUT)
        self.owned = True

    def text(self):
        # The app's own log flushes where redirected stdout may be block buffered.
        return (self.root / 'run.log').read_text(errors='replace')

    def alive(self):
        return self.process is not None and self.process.poll() is None

    def send(self, kind, value):
        with (self.root / ('debug.txt' if kind == 'debug' else 'input-live.txt')).open('a') as output:
            output.write(value + '\n')

    def capture(self, name):
        self.send('input', '0:shot ' + safe_name(name))
        path = self.root / 'shots' / (name + '.bmp')
        limit = time.monotonic() + remaining(self.deadline, 10)
        while not path.is_file():
            if not self.alive():
                raise RuntimeError('Game exited before capture')
            remaining(min(limit, self.deadline))
            time.sleep(0.1)
        return path.relative_to(self.root).as_posix()

    def close(self):
        stop_process(self.process)
        if self.log:
            self.log.close()


class Adb:
    def __init__(self, device, root, deadline):
        self.device, self.root, self.deadline = device, root, deadline
        self.prefix = device.get('adb', ['adb']) + ['-s', device['serial']]
        self.package = device['package']
        if not valid_package(self.package):
            raise ValueError('ADB automation requires a dedicated .devicetest package')
        self.remote = '/data/data/' + self.package + '/files/device-testing'
        self.owned = False
        self.pid = None
        self.log_process = None
        self.log_file = None

    def adb(self, *args, data=None, check=True):
        return command(self.prefix + list(args), self.deadline, data=data, check=check)

    def shell(self, text, **kwargs):
        return self.adb('shell', text, **kwargs)

    def app(self, text, **kwargs):
        return self.shell('run-as ' + self.package + ' sh -c ' + shlex.quote(text), **kwargs)

    def prepare(self, report):
        apk = Path(self.device['apk'])
        with zipfile.ZipFile(apk) as archive:
            contract = json.loads(archive.read('assets/device-testing.json'))
        if contract != android_contract(self.package):
            raise ValueError('APK lacks the generated device-testing contract')
        report['artifact_sha256'] = fingerprint(apk)
        if self.shell('pidof ' + self.package, check=False).strip():
            raise NotTested('Dedicated test package is already running; stop it manually first')
        props = self.shell('getprop').decode(errors='replace')
        report['hardware'] = {key: next((line for line in props.splitlines() if key in line), 'unknown')
                              for key in ('ro.product.model', 'ro.build.version.release',
                                          'ro.product.cpu.abilist', 'ro.hardware')}
        vulkan = self.shell('cmd gpu vkjson', check=False)
        (self.root / 'vulkan.json').write_bytes(vulkan)
        report['hardware']['vulkan_evidence'] = 'vulkan.json'
        verify_android_capabilities(report['hardware'], vulkan)
        command(self.prefix + ['install', '-r', str(apk)], self.deadline, maximum=180)
        # Remove only this dedicated test package's fixed harness directory.
        self.app('rm -rf files/device-testing && mkdir -p files/device-testing/data files/device-testing/shots')
        staging = self.root / 'data'
        staging.mkdir()
        report['fixture_sha256'] = copy_fixture(self.device['saves'], staging / 'ms0')
        if self.device.get('settings'):
            if Path(self.device['settings']).is_symlink():
                raise ValueError('Settings fixture must not be a symlink')
            shutil.copyfile(self.device['settings'], staging / 'settings.ini')
            report['settings_sha256'] = fingerprint(staging / 'settings.ini')
        for name in ('EBOOT.ELF', 'disc.iso'):
            source = Path(self.device['game_dir']) / name
            if not source.is_file():
                raise NotTested('Prepared game input is unavailable')
            # Stream into app-private storage; no shared temporary copy of game data.
            with source.open('rb') as stream:
                result = subprocess.run(self.prefix + ['shell', 'run-as', self.package, 'sh', '-c',
                                                       shlex.quote('cat > files/device-testing/data/' + name)],
                                        stdin=stream, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        timeout=remaining(self.deadline, 240), check=False)
            if result.returncode:
                raise RuntimeError('Game upload failed')
        report['game_sha256'] = fingerprint(Path(self.device['game_dir']) / 'disc.iso')
        for item in sorted(staging.rglob('*')):
            relative = item.relative_to(staging).as_posix()
            if item.is_file():
                destination = 'files/device-testing/data/' + relative
                self.app('mkdir -p ' + shlex.quote(str(Path(destination).parent)) +
                         ' && cat > ' + shlex.quote(destination), data=item.read_bytes())
        self.app('touch files/device-testing/input-live.txt files/device-testing/debug.txt')
        if self.device.get('env') or self.device.get('translation'):
            raise NotTested('ADB APK contract has fixed environment; language/mod variant needs its own packaged contract')

    def launch(self):
        self.log_file = (self.root / 'logcat.log').open('wb')
        self.log_process = subprocess.Popen(self.prefix + ['logcat', '-T', '1'],
                                            stdout=self.log_file, stderr=subprocess.STDOUT)
        # Mark ownership before am start: partial activation failure must still stop it.
        self.owned = True
        self.shell('am start -W -n ' + self.package + '/' + self.device.get('activity', 'io.github.teamgdb.yakumo.YakumoActivity'))
        self.pid = self.shell('pidof ' + self.package).decode().strip()
        if not self.pid:
            raise RuntimeError('Android app did not start')
        # Detached, PID-guarded watchdog survives a lost host/SSH connection.
        seconds = max(1, int(remaining(self.deadline, 1800)))
        script = (f'#!/system/bin/sh\nsleep {seconds} &\nsleeper=$!\n'
                  "trap 'kill $sleeper 2>/dev/null; exit' TERM INT HUP\n"
                  'wait $sleeper\n'
                  f'[ "$(pidof {self.package})" = "{self.pid}" ] && am force-stop {self.package}\n')
        remote = '/data/local/tmp/yakumo-test-' + uuid.uuid4().hex + '.sh'
        self.shell('cat > ' + remote, data=script.encode())
        self.watchdog = remote
        watchdog_pid = self.shell('nohup sh ' + remote + ' >/dev/null 2>&1 </dev/null & echo $!').decode().strip()
        if not watchdog_pid.isdigit():
            raise RuntimeError('Android watchdog did not return its PID')
        self.watchdog_pid = watchdog_pid

    def text(self):
        text = self.app('cat files/yakumo.log', check=False).decode(errors='replace')
        (self.root / 'run.log').write_text(text)
        return text

    def alive(self):
        return bool(self.shell('pidof ' + self.package, check=False).strip())

    def send(self, kind, value):
        path = 'files/device-testing/' + ('debug.txt' if kind == 'debug' else 'input-live.txt')
        self.app('cat >> ' + path, data=(value + '\n').encode())

    def capture(self, name):
        path = self.root / 'shots' / (safe_name(name) + '.png')
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(self.adb('exec-out', 'screencap', '-p'))
        if not path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'):
            raise RuntimeError('Device capture is not a PNG')
        return path.relative_to(self.root).as_posix()

    def close(self):
        # Cleanup gets its own short deadline even after the gameplay/upload cap.
        self.deadline = getattr(self, 'cleanup_deadline', None) or time.monotonic() + 15
        try:
            if self.owned:
                current = self.shell('pidof ' + self.package, check=False).decode().strip()
                if self.pid and current and current != self.pid:
                    raise RuntimeError('Android PID changed; refusing to stop a replacement process')
                self.shell('am force-stop ' + self.package)
                if self.alive():
                    raise RuntimeError('Owned Android process did not stop')
            if hasattr(self, 'watchdog_pid'):
                self.shell('grep -F ' + shlex.quote(self.watchdog) + ' /proc/' + self.watchdog_pid +
                           '/cmdline >/dev/null 2>&1 && kill ' + self.watchdog_pid, check=False)
            if hasattr(self, 'watchdog'):
                self.shell('rm -f ' + self.watchdog)
        finally:
            stop_process(self.log_process)
            if self.log_file:
                self.log_file.close()


def verify_android_capabilities(hardware, vulkan):
    if 'arm64-v8a' not in hardware.get('ro.product.cpu.abilist', ''):
        raise NotTested('Device does not advertise the required arm64-v8a ABI')
    try:
        devices = json.loads(vulkan)['devices']
        supported = [item['properties'] for item in devices
                     if item.get('properties', {}).get('apiVersion', 0) >= 0x401000]
    except (ValueError, KeyError, TypeError):
        raise NotTested('Vulkan capabilities could not be verified; inspect vulkan.json')
    if not supported:
        raise NotTested('Device does not advertise Vulkan 1.1')
    hardware['gpus'] = [{key: item.get(key, 'unknown') for key in ('deviceName', 'driverVersion', 'apiVersion')}
                        for item in supported]


def valid_package(package):
    if not isinstance(package, str) or len(package) > 160:
        return False
    parts = package.split('.')
    return len(parts) >= 3 and parts[-1] == 'devicetest' and all(
        part and 'a' <= part[0] <= 'z' and all(char in 'abcdefghijklmnopqrstuvwxyz0123456789_' for char in part)
        for part in parts)


def android_contract(package):
    if not valid_package(package):
        raise ValueError('Test package must end in .devicetest')
    root = '/data/data/' + package + '/files/device-testing'
    return {'package': package, 'env': {'MHP3RD_GAME_DIR': root + '/data',
            'MHP3RD_DATA_DIR': root + '/data', 'MHP3RD_INPUT_LIVE': root + '/input-live.txt',
            'MHP3RD_DEBUG_COMMANDS': root + '/debug.txt', 'MHP3RD_DEBUG_MENU': '1',
            'MHP3RD_SCREENSHOT_DIR': root + '/shots', 'MHP3RD_PERF': 'log'}}


def android_manifest(package, output):
    contract = android_contract(package)
    ns = '{http://schemas.android.com/apk/res/android}'
    ET.register_namespace('android', ns[1:-1])
    tree = ET.parse(ROOT / 'profiles/mhp3rd/packaging/android/AndroidManifest.xml')
    tree.getroot().set('package', package)
    app = tree.getroot().find('application')
    app.set(ns + 'debuggable', 'true')
    # Keep the Java activity fully qualified after changing the package identity.
    app.find('activity').set(ns + 'name', 'io.github.teamgdb.yakumo.YakumoActivity')
    app.find('activity').set(ns + 'label', 'Yakumo device tests')
    app.set(ns + 'label', 'Yakumo device tests')
    for key, value in contract['env'].items():
        ET.SubElement(app, 'meta-data', {ns + 'name': 'SDL_ENV.' + key, ns + 'value': value})
    tree.write(output, encoding='utf-8', xml_declaration=True)
    return contract


def bounded_match(pattern, text, deadline):
    # Run tester-authored regexes in an owned process: backtracking cannot defeat a wait deadline.
    result = command([sys.executable, str(Path(__file__).resolve()), '_match'], deadline,
                     data=json.dumps([pattern, text]).encode(), maximum=1800)
    return result == b'1'


def regex_worker():
    pattern, text = json.load(sys.stdin)
    # Intentional tester-authored expression; parent enforces a subprocess deadline.
    print('1' if re.search(pattern, text) else '0', end='')
    return 0


def wait_condition(adapter, pattern, timeout, deadline, offset=0):
    limit = min(deadline, time.monotonic() + timeout)
    while True:
        text = adapter.text()[offset:]
        if re.search(r'\[debug\] (?:not started|refused)|FATAL EXCEPTION|Fatal signal|VK_ERROR_DEVICE_LOST', text):
            raise RuntimeError('Game reported a refusal, crash or device loss')
        if bounded_match(pattern, text, limit):
            return
        if not adapter.alive():
            raise RuntimeError('Game exited before expected condition: ' + pattern)
        remaining(limit)
        time.sleep(0.1)


def execute_steps(adapter, steps, device, report, deadline):
    offset = 0
    for step in steps:
        remaining(deadline)
        if 'fixture' in step:
            key = step['fixture']
            if key not in device.get('fixtures', {}):
                raise NotTested('Missing fixture navigation: ' + key)
            execute_steps(adapter, device['fixtures'][key], device, report, deadline)
        elif 'wait' in step:
            wait_condition(adapter, step['wait'], step.get('timeout', 20), deadline,
                           offset if step.get('fresh', True) else 0)
            report['observations'].append(step['wait'])
        elif 'capture' in step:
            report['captures'].append(adapter.capture(step['capture']))
        else:
            offset = len(adapter.text())
            kind = 'debug' if 'debug' in step else 'input'
            if kind == 'debug' and device['build'] != 'dev':
                raise NotTested('Debug commands require a developer artifact')
            adapter.send(kind, step[kind])


def summarize_log(text):
    samples = [dict(zip(('fps', 'game_fps', 'speed_percent'), map(float, item)))
               for item in re.findall(r'\[perf\] fps ([\d.]+) game ([\d.]+) speed ([\d.]+)%', text)]
    gpu = [line for line in text.splitlines() if '[gpu]' in line and
           any(word in line.lower() for word in ('device:', 'driver', 'vulkan'))]
    return {'performance_samples': samples, 'gpu_driver_lines': gpu[:20],
            'note': 'Samples include loading; they do not certify a hunt or visual correctness.'}


def result_status(report):
    checks = [item['verdict'] for item in report['review'].values()]
    automatic = report['automatic']['verdict']
    if automatic == 'FAIL' or 'FAIL' in checks:
        return 'FAIL'
    manual_complete = report.get('execution') == 'manual' and report.get('artifact_sha256') and all(value == 'PASS' for value in checks)
    if (automatic != 'PASS' and not manual_complete) or any(value != 'PASS' for value in checks):
        return 'NOT TESTED'
    return 'PASS'


def render_report(root):
    report = read_json(Path(root) / 'report.json')
    text = f'# {report["scenario"]} on {report["device"]}\n\n'
    text += f'Status: **{result_status(report)}**; automatic: {report["automatic"]["verdict"]}\n\n'
    text += f'Commit: `{report["commit"]}`; artifact SHA-256: `{report.get("artifact_sha256", "unknown")}`\n\n'
    text += '| Review check | Verdict | Expected | Reviewer / notes |\n| --- | --- | --- | --- |\n'
    for key, item in report['review'].items():
        escape = lambda value: str(value).replace('|', '\\|').replace('\n', ' ')
        assessments = item.get('assessments', {})
        notes = '; '.join(actor + ': ' + value['verdict'] + ' — ' + value['notes']
                          for actor, value in assessments.items()) or item.get('notes', '')
        text += f'| {key} | {item["verdict"]} | {escape(item["expect"])} | {escape(notes)} |\n'
        for actor, value in assessments.items():
            for evidence in value.get('evidence', []):
                text += f'| | | | {actor}: [{escape(evidence)}]({evidence}) |\n'
    text += '\nEvidence: [run.log](run.log)\n\n'
    for capture in report.get('captures', []):
        text += f'- [{capture}]({capture})\n'
    (Path(root) / 'review.md').write_text(text, encoding='utf-8')


def new_report(suite, device, name):
    return {'schema': 1, 'run_id': uuid.uuid4().hex, 'scenario': suite['id'],
            'scenario_sha256': object_hash(suite), 'execution': suite['execution'], 'device': name, 'build': device['build'],
            'commit': device['commit'], 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            'deadline_seconds': min(suite['seconds'], device.get('seconds', suite['seconds'])),
            'settings': device.get('env', {}), 'hardware': device.get('hardware', {'os': 'NOT TESTED', 'gpu': 'NOT TESTED', 'driver': 'NOT TESTED'}), 'automatic': {'verdict': 'NOT TESTED', 'reason': 'Not run'},
            'observations': [], 'captures': [],
            'review': {item['id']: {**item, 'verdict': 'NOT TESTED'} for item in suite['review']}}


def run(suite, device, name, root, manual=False):
    report = new_report(suite, device, name)
    cap = min(suite['seconds'], device.get('seconds', suite['seconds']))
    hard_deadline = time.monotonic() + cap
    deadline = hard_deadline - min(20, cap / 5) if device['adapter'] == 'streaming' else hard_deadline
    adapter = None
    reservation = None
    lease_watchdog = None
    reservation_released = False
    started = time.monotonic()
    try:
        if manual and suite['execution'] != 'manual':
            raise NotTested('Use an explicit manual scenario; automatic observations must not be bypassed')
        if suite['build'] != 'either' and suite['build'] != device['build']:
            raise NotTested('Scenario needs a ' + suite['build'] + ' artifact')
        if suite.get('translation') and not device.get('translation'):
            raise NotTested('A translation fixture is required')
        if device['adapter'] == 'streaming':
            if not 1 <= device.get('seconds', 0) <= 300:
                raise ValueError('Streaming requires an explicit overall cap of at most 300 seconds')
            cli = device.get('android', ['android']) + ['--no-metrics', 'device', 'remote']
            # Parse the CLI's own creation output. Never delete a different user's reservation.
            active = command(cli + ['list', '--short', '--project=' + device['project']], deadline, check=False)
            if active.strip() and b'No reservations found' not in active:
                raise NotTested('Project has existing reservations; use an idle dedicated test project')
            creation_log = root / 'reservation-create.log'
            creation_log.touch()
            watchdog_payload = base64.b64encode(json.dumps({'log': str(creation_log), 'deadline': deadline,
                'cli': cli, 'project': device['project']}).encode()).decode()
            lease_watchdog = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                              '_lease-watchdog', watchdog_payload],
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with creation_log.open('wb') as output:
                creator = subprocess.Popen(cli + ['create', device['model'], '--project=' + device['project']],
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    creator.wait(timeout=remaining(deadline, 240))
                finally:
                    stop_process(creator)
                    output.flush()
                    reservation = parse_reservation(creation_log.read_text(errors='replace'))
            if creator.returncode or not reservation:
                raise RuntimeError('Reservation creation failed or output was unrecognized; inspect reservation-create.log')
            device = {**device, 'serial': parse_adb_serial(creation_log.read_text(errors='replace'))}
        adapter = Local(device, root, deadline) if device['adapter'] == 'local' else Adb(device, root, deadline)
        if device['adapter'] == 'streaming':
            adapter.cleanup_deadline = hard_deadline
        adapter.prepare(report)
        write_json(root / 'report.json', report)
        render_report(root)
        if suite['execution'] == 'manual' and not manual:
            raise NotTested('Use --manual to run this human checklist')
        adapter.launch()
        if manual:
            while adapter.alive():
                remaining(deadline)
                time.sleep(0.2)
            if isinstance(adapter, Local) and adapter.process.returncode not in (0, 124):
                raise RuntimeError('Game exited abnormally during manual session')
            # The harness completed; the checklist still requires explicit review.
        else:
            execute_steps(adapter, suite['steps'], device, report, deadline)
        report['automatic'] = {'verdict': 'PASS', 'reason': 'Harness completed; see observations and separate review'}
    except NotTested as error:
        report['automatic'] = {'verdict': 'NOT TESTED', 'reason': str(error)}
    except TimeoutError as error:
        report['automatic'] = {'verdict': 'PASS' if manual and adapter and adapter.owned else 'FAIL',
                               'reason': 'Manual session deadline reached' if manual else str(error)}
    except (KeyboardInterrupt, Exception) as error:
        report['automatic'] = {'verdict': 'FAIL', 'reason': str(error) or type(error).__name__}
    finally:
        try:
            if adapter:
                try:
                    if adapter.owned:
                        final_log = adapter.text()
                        report['metrics'] = summarize_log(final_log)
                finally:
                    adapter.close()
        except Exception as error:
            report['automatic'] = {'verdict': 'FAIL', 'reason': 'Cleanup failed: ' + str(error)}
        if reservation:
            try:
                command(cli + ['remove', reservation, '--project=' + device['project']], max(hard_deadline, time.monotonic() + 2))
                reservation_released = True
            except Exception as error:
                report['automatic'] = {'verdict': 'FAIL', 'reason': 'Reservation cleanup failed: ' + str(error)}
        if not reservation or reservation_released:
            stop_process(lease_watchdog)
        report['duration_seconds'] = round(time.monotonic() - started, 3)
        # Review may have been recorded while a human was playing; preserve it.
        if (root / 'report.json').exists():
            current = read_json(root / 'report.json')
            if current.get('run_id') == report['run_id']:
                report['review'] = current['review']
        write_json(root / 'report.json', report)
        render_report(root)
    return 1 if report['automatic']['verdict'] == 'FAIL' else 2 if result_status(report) != 'PASS' else 0


def plain_output(text):
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', text)


def parse_reservation(output):
    match = re.search(r'(?:new reservation created with id\s+|reservation(?:\s+id)?\s*[:=]\s*|reservations/)([a-z0-9]{8,})', plain_output(output), re.I)
    return match.group(1) if match else None


def parse_adb_serial(output):
    text = plain_output(output)
    match = re.search(r'(?:localhost|127\.0\.0\.1):\d+', text)
    if match:
        return match.group(0)
    match = re.search(r'(?:device |already )?connected on port\s+(\d+)', text, re.I)
    if not match:
        raise RuntimeError('No connected ADB endpoint in reservation output')
    return 'localhost:' + match.group(1)


def lease_watchdog(payload):
    value = json.loads(base64.b64decode(payload))
    time.sleep(max(0, value['deadline'] - time.monotonic()))
    reservation = parse_reservation(Path(value['log']).read_text(errors='replace'))
    if reservation:
        command(value['cli'] + ['remove', reservation, '--project=' + value['project']], time.monotonic() + 20)
    return 0


def windows_job(process):
    """Kill the owned game if Windows forcibly terminates its supervisor."""
    import ctypes
    from ctypes import wintypes
    class Basic(ctypes.Structure):
        _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                    ('flags', wintypes.DWORD), ('minimum', ctypes.c_size_t),
                    ('maximum', ctypes.c_size_t), ('processes', wintypes.DWORD),
                    ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD),
                    ('scheduling', wintypes.DWORD)]
    class Limits(ctypes.Structure):
        _fields_ = [('basic', Basic), ('io', ctypes.c_uint64 * 6),
                    ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                    ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    api.CreateJobObjectW.restype = wintypes.HANDLE
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    job = api.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = Limits()
    limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    handle = api.OpenProcess(0x0101, False, process.pid)  # SET_QUOTA | TERMINATE
    try:
        if not handle or not api.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or not api.AssignProcessToJobObject(job, handle):
            raise ctypes.WinError(ctypes.get_last_error())
    except BaseException:
        api.CloseHandle(job)
        raise
    finally:
        if handle:
            api.CloseHandle(handle)
    return lambda: api.CloseHandle(job)


def supervise(seconds, binary, args=()):
    """Own the game even when the controlling SSH process disappears."""
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    if hasattr(signal, 'SIGHUP'):
        signal.signal(signal.SIGHUP, interrupted)
    process = subprocess.Popen([binary, *args])
    close_job = None
    try:
        if os.name == 'nt':
            close_job = windows_job(process)
        return process.wait(timeout=float(seconds))
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        return 124
    finally:
        stop_process(process)
        if close_job:
            close_job()


def ssh_run(suite, device, name, root, manual):
    remote = device['remote']
    if remote.get('adapter') not in ('local', 'adb'):
        raise ValueError('SSH delegates only local or ADB execution')
    host = device['host']
    if not isinstance(host, str) or host.startswith('-') or any(ch.isspace() for ch in host):
        raise ValueError('Use one SSH alias or user@host')
    windows = device.get('shell') == 'powershell'
    remote_root = device['checkout'].rstrip('/\\')
    runner = remote_root + '/profiles/mhp3rd/tools/device_testing.py'
    payload = base64.b64encode(json.dumps({'scenario': suite, 'device': remote, 'name': name,
                                          'manual': manual}).encode()).decode()
    args = device.get('python', ['python3']) + [runner, '_remote', payload]
    if windows:
        quote = lambda text: "'" + text.replace("'", "''") + "'"
        script = '& ' + ' '.join(quote(item) for item in args)
        invocation = 'powershell -NoProfile -EncodedCommand ' + base64.b64encode(script.encode('utf-16le')).decode()
    else:
        invocation = shlex.join(args)
    prefix = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host]
    deadline = time.monotonic() + suite['seconds'] + 30
    try:
        output = command(prefix + [invocation], deadline, check=False, maximum=suite['seconds'] + 30).decode(errors='replace')
    except (Exception, KeyboardInterrupt) as error:
        output = 'SSH transport failed: ' + (str(error) or type(error).__name__)
    (root / 'transport.log').write_text(output)
    match = re.search(r'YAKUMO_EVIDENCE:([A-Za-z0-9+/=]+)', output)
    if not match:
        report = new_report(suite, device, name)
        report['automatic'] = {'verdict': 'FAIL', 'reason': 'No remote evidence bundle; see transport.log'}
        write_json(root / 'report.json', report)
        render_report(root)
        return 1
    # The bundle contains evidence only, not game data or copied saves.
    import io
    with zipfile.ZipFile(io.BytesIO(base64.b64decode(match.group(1)))) as bundle:
        for item in bundle.infolist():
            target = (root / item.filename).resolve()
            if root.resolve() not in target.parents or target.suffix not in ('.json', '.md', '.log', '.bmp', '.png'):
                raise ValueError('Unsafe remote evidence path')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(bundle.read(item))
    status = result_status(read_json(root / 'report.json'))
    return 1 if status == 'FAIL' else 2 if status != 'PASS' else 0


def remote_run(payload):
    import io
    value = json.loads(base64.b64decode(payload))
    suite = validate_scenario(value['scenario'])
    device = value['device']
    root = output_directory(tempfile.gettempdir())
    code = run(suite, device, safe_name(value['name']), root, value['manual'])
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as bundle:
        paths = [root / 'report.json', root / 'review.md', root / 'run.log', root / 'vulkan.json',
                 *list((root / 'shots').glob('*'))]
        for path in paths:
            if path.is_file():
                bundle.write(path, path.relative_to(root).as_posix())
    print('YAKUMO_EVIDENCE:' + base64.b64encode(output.getvalue()).decode(), flush=True)
    return code


def review(root, check, verdict, reviewer, notes, evidence):
    path = Path(root) / 'report.json'
    report = read_json(path)
    if not notes.strip():
        raise ValueError('Record what was observed, not only a verdict')
    for item in evidence:
        resolved = (Path(root) / item).resolve()
        if Path(root).resolve() not in resolved.parents or not resolved.is_file():
            raise ValueError('Evidence must be an existing file within this run')
    if verdict not in VERDICTS or reviewer not in ('human', 'agent'):
        raise ValueError('Invalid review verdict/reviewer')
    item = report['review'][check]
    assessments = item.setdefault('assessments', {})
    assessments[reviewer] = {'verdict': verdict, 'notes': notes, 'evidence': evidence}
    required = item.get('requires', 'either')
    values = [value['verdict'] for value in assessments.values()]
    effective = ('FAIL' if 'FAIL' in values else
                 assessments.get(required, {}).get('verdict', 'NOT TESTED') if required != 'either' else
                 'PASS' if 'PASS' in values else 'NOT TESTED')
    item.update(verdict=effective, reviewer=reviewer, notes=notes, evidence=evidence)
    write_json(path, report)
    render_report(root)


def matrix(plan, runs, output):
    plan = read_json(plan)
    reports = [{**read_json(path), '_evidence': path.parent.name + '/review.md'}
               for path in Path(runs).glob('*/report.json')]
    if len({(item['device'], item['scenario']) for item in plan['required']}) != len(plan['required']):
        raise ValueError('Release plan has duplicate scenario/device rows')
    rows = []
    for required in plan['required']:
        safe_name(required['device'])
        safe_name(required['scenario'])
        if not re.fullmatch(r'[0-9a-f]{40}', required['commit']) or not re.fullmatch(r'[0-9a-f]{64}', required['artifact_sha256']):
            raise ValueError('Release plan requires exact commit and artifact SHA-256')
        if required['build'] not in ('dev', 'release'):
            raise ValueError('Release plan requires actual build type')
        expected_scenario = required.get('scenario_sha256', object_hash(scenario(required['scenario'])))
        matches = [item for item in reports if item.get('scenario_sha256') == expected_scenario and all(item.get(key) == required[key]
                   for key in ('device', 'scenario', 'commit', 'artifact_sha256', 'build'))]
        current = max(matches, key=lambda item: item['started_utc'], default=None)
        status = result_status(current) if current else 'NOT TESTED'
        rows.append({**required, 'verdict': status, 'run_id': current['run_id'] if current else None,
                     'evidence': current['_evidence'] if current else None})
    if not rows:
        raise ValueError('A release plan must contain required scenario/device rows')
    write_json(Path(output).with_suffix('.json'), {'required': rows})
    lines = ['# Release device matrix', '', '| Scenario | Device | Build | Verdict | Evidence |', '| --- | --- | --- | --- | --- |']
    for row in rows:
        evidence = '[review](' + row['evidence'] + ')' if row['evidence'] else 'Missing'
        lines.append(f'| {row["scenario"]} | {row["device"]} | {row["build"]} | {row["verdict"]} | {evidence} |')
    Path(output).write_text('\n'.join(lines) + '\n')
    return 1 if any(row['verdict'] == 'FAIL' for row in rows) else 2 if any(row['verdict'] != 'PASS' for row in rows) else 0


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] == '_match':
        return regex_worker()
    if argv and argv[0] == '_supervise':
        return supervise(argv[1], argv[2], argv[3:])
    if argv and argv[0] == '_lease-watchdog':
        return lease_watchdog(argv[1])
    if argv and argv[0] == '_remote':
        return remote_run(argv[1])
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list', help='List public scenarios')
    for kind in ('prepare', 'run'):
        part = sub.add_parser(kind, help='Prepare checklist or execute a bounded session')
        part.add_argument('scenario')
        part.add_argument('--config', required=True)
        part.add_argument('--device', required=True)
        part.add_argument('--output-parent', default=tempfile.gettempdir())
        if kind == 'run':
            part.add_argument('--manual', action='store_true', help='Bounded human play, no scripted actions')
    part = sub.add_parser('review', help='Record human/agent evidence without changing automatic results')
    part.add_argument('--run', required=True)
    part.add_argument('--check', required=True)
    part.add_argument('--verdict', choices=VERDICTS, required=True)
    part.add_argument('--reviewer', choices=('human', 'agent'), required=True)
    part.add_argument('--notes', required=True)
    part.add_argument('--evidence', nargs='*', default=[])
    part = sub.add_parser('matrix', help='Require exact artifacts and show missing/failed release checks')
    part.add_argument('--plan', required=True)
    part.add_argument('--runs', required=True)
    part.add_argument('--output', required=True)
    part = sub.add_parser('android-manifest', help='Generate a dedicated dev-only APK test manifest')
    part.add_argument('--package', required=True)
    part.add_argument('--output', required=True)
    part.add_argument('--contract', required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'list':
            for path in sorted(SCENARIOS.glob('*.json')):
                value = validate_scenario(read_json(path))
                print(f'{value["id"]}: {value["build"]}/{value["execution"]} - {value["description"]}')
        elif args.command == 'android-manifest':
            write_json(args.contract, android_manifest(args.package, args.output))
        elif args.command == 'review':
            review(args.run, args.check, args.verdict, args.reviewer, args.notes, args.evidence)
        elif args.command == 'matrix':
            return matrix(args.plan, args.runs, args.output)
        else:
            suite = scenario(args.scenario)
            device = load_device(args.config, args.device)
            root = output_directory(args.output_parent)
            print('Local evidence directory: ' + str(root), flush=True)
            if args.command == 'prepare':
                report = new_report(suite, device, args.device)
                artifact = device.get('apk', device.get('binary'))
                if artifact and Path(artifact).is_file():
                    report['artifact_sha256'] = fingerprint(artifact)
                write_json(root / 'report.json', report)
                render_report(root)
                return 2
            if device['adapter'] == 'ssh':
                return ssh_run(suite, device, args.device, root, args.manual)
            return run(suite, device, args.device, root, args.manual)
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    return 0


if __name__ == '__main__':
    sys.exit(main())
