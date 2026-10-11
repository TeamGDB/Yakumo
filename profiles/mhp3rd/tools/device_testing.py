#!/usr/bin/env python3
"""Run bounded device scenarios and keep automatic evidence separate from review."""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import queue
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import threading
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
EVIDENCE_LIMIT = 64 * 1024 * 1024
EVIDENCE_FILES = {'report.json', 'run.log', 'vulkan.json', 'logcat.log'}
ACTIVITY = 'io.github.teamgdb.yakumo.YakumoActivity'


class NotTested(ValueError):
    """An unmet prerequisite is a visible coverage gap, not a pass."""


def read_json(path):
    """Read a file deliberately selected by the local tester, not a remote request."""
    return json.loads(Path(path).read_text(encoding='utf-8'))


def write_json(path, value):
    path = Path(path)
    # An exclusive random sibling avoids following a planted report.tmp symlink.
    fd, name = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as output:
            json.dump(value, output, indent=2)
            output.write('\n')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


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
    return validate_device(copy.deepcopy(read_json(config)['devices'][safe_name(name)]))


def validate_device(value):
    if 'rtc_epoch_us' in value and (type(value['rtc_epoch_us']) is not int or
            not 946684800000000 <= value['rtc_epoch_us'] <= 4102444800000000 or not value.get('observations')):
        raise ValueError('A pinned RTC epoch requires explicit developer observations and a year from 2000 to 2100')
    if type(value.get('observations', False)) is not bool or value.get('observations') and value.get('build') != 'dev':
        raise ValueError('Structured observations require an explicit developer artifact')
    if value.get('adapter') not in ('local', 'ssh', 'adb', 'streaming'):
        raise ValueError('Unknown device adapter')
    if value.get('build') not in ('dev', 'release'):
        raise ValueError('Declare the actual artifact build type')
    if not re.fullmatch(r'[0-9a-f]{40}', value.get('commit', '')):
        raise ValueError('Declare the exact 40-character source commit')
    if not 1 <= value.get('seconds', 60) <= 1800:
        raise ValueError('Device cap must be 1..1800 seconds')
    setup = value.get('setup_seconds', 0)
    if type(setup) is not int or not 0 <= setup <= 900 or (setup and value['adapter'] != 'adb'):
        raise ValueError('Setup cap must be 0..900 seconds on physical/emulated ADB devices only')
    if setup + value.get('seconds', 60) > 1800:
        raise ValueError('Setup and gameplay must fit the 1800-second total cap')
    if value['adapter'] == 'streaming' and (value.get('seconds', 60) > streaming_limit(value) or setup):
        raise ValueError('Streaming exceeds its explicit inclusive cap or requests a setup extension')
    if type(value.get('raw_adb_upload', False)) is not bool or value.get('raw_adb_upload') and os.name == 'nt':
        raise ValueError('Raw ADB upload is an explicit POSIX-only option; Windows requires Base64')
    if type(value.get('android_cli_install', False)) is not bool:
        raise ValueError('Android CLI installation must be an explicit boolean option')
    if type(value.get('deployment_share_percent', 70)) is not int or not 1 <= value.get('deployment_share_percent', 70) <= 90:
        raise ValueError('Deployment share must be 1..90 percent of remaining time')
    if type(value.get('gameplay_reserve_seconds', 0)) is not int or not 0 <= value.get('gameplay_reserve_seconds', 0) <= 1800:
        raise ValueError('Gameplay reserve must be 0..1800 seconds within the session')
    if set(value.get('env', {})) - ENV_KEYS:
        raise ValueError('Unsupported environment override; isolation controls cannot be replaced')
    if value.get('performance', 'log') not in ('log', 'settings'):
        raise ValueError('Performance must be log or settings')
    for key in ('adb', 'android', 'python', 'args'):
        if key in value and (not isinstance(value[key], list) or
                (key != 'args' and not value[key]) or any(not isinstance(item, str) or '\0' in item for item in value[key])):
            raise ValueError('Tool prefixes and arguments must be explicit string arrays')
    if 'artifact_sha256' in value and not re.fullmatch(r'[0-9a-f]{64}', value['artifact_sha256']):
        raise ValueError('Expected artifact fingerprint must be SHA-256')
    for steps in value.get('fixtures', {}).values():
        for step in steps:
            validate_step(step)
            if 'fixture' in step:
                raise ValueError('Fixture navigation cannot recursively invoke fixtures')
            if 'debug' in step and value['build'] != 'dev':
                raise ValueError('Release fixture cannot use debug commands')
    if value['adapter'] == 'ssh':
        remote = validate_device(value['remote'])
        if remote['adapter'] not in ('local', 'adb') or value.get('shell', 'posix') not in ('posix', 'powershell'):
            raise ValueError('SSH delegates local/ADB execution through POSIX or PowerShell')
        if remote.get('build') != value['build'] or remote.get('commit') != value['commit']:
            raise ValueError('SSH wrapper and remote artifact identity must match')
    return value


def output_directory(parent):
    parent = os.path.normcase(os.path.realpath(parent))
    temporary = os.path.normcase(os.path.realpath(tempfile.gettempdir()))
    output = os.path.normcase(os.path.realpath(ROOT / 'out'))
    if parent == temporary:
        parent = temporary
    elif parent == output:
        parent = output
    elif parent.startswith(temporary.rstrip(os.sep) + os.sep):
        parent = output_descendant(temporary, os.path.relpath(parent, temporary))
    elif parent.startswith(output.rstrip(os.sep) + os.sep):
        parent = output_descendant(output, os.path.relpath(parent, output))
    else:
        raise ValueError('Output parent must exist under repository out/ or the OS temporary directory')
    if not os.path.isdir(parent):
        raise ValueError('Output parent must be an existing directory')
    return Path(tempfile.mkdtemp(prefix='yakumo-device-', dir=parent))


def output_descendant(root, relative):
    """Resolve an explicitly allowed output subtree before filesystem access."""
    root = os.path.normcase(os.path.realpath(root))
    destination = os.path.normcase(os.path.realpath(os.path.join(root, relative)))
    if not destination.startswith(root.rstrip(os.sep) + os.sep):
        raise ValueError('Output subtree escapes the allowed root')
    return destination


def copy_fixture(source, destination):
    requested = Path(source).absolute()
    if not requested.exists() and not requested.is_symlink():
        raise NotTested('Missing copied-save fixture')
    original = requested.lstat()
    if stat.S_ISLNK(original.st_mode):
        raise ValueError('Fixture root must not be a symlink')
    source = requested.resolve()
    if not source.is_dir():
        raise NotTested('Missing copied-save fixture')
    # Reject all links, including links into the source: no alias to player files.
    for item in source.rglob('*'):
        if item.is_symlink() or not (item.is_dir() or item.is_file()):
            raise ValueError('Fixture must not contain symlinks or special files')
    before = source.stat()
    if (original.st_dev, original.st_ino) != (before.st_dev, before.st_ino):
        raise ValueError('Fixture root changed before copying')
    # Preserve late-added links instead of dereferencing them, then reject them.
    shutil.copytree(source, destination, symlinks=True, copy_function=copy_regular_file)
    after = source.lstat()
    requested_after = requested.lstat()
    if not stat.S_ISDIR(after.st_mode) or (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino) or \
            (original.st_dev, original.st_ino) != (requested_after.st_dev, requested_after.st_ino):
        raise ValueError('Fixture root changed during copying')
    destination = Path(destination)
    for item in destination.rglob('*'):
        if item.is_symlink() or not (item.is_dir() or item.is_file()):
            raise ValueError('Fixture changed to contain links or special files')
    return object_hash([(item.relative_to(destination).as_posix(), fingerprint(item))
                        for item in sorted(destination.rglob('*')) if item.is_file()])


def copy_regular_file(source, destination):
    """Copy a regular-file snapshot without following a late source symlink."""
    before = Path(source).lstat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError('Fixture must be a regular file')
    flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_BINARY', 0)
    fd = os.open(source, flags)
    with os.fdopen(fd, 'rb') as input_file:
        opened = os.fstat(input_file.fileno())
        if not stat.S_ISREG(opened.st_mode) or (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise ValueError('Fixture changed before opening')
        with Path(destination).open('xb') as output:
            shutil.copyfileobj(input_file, output)
        after = os.fstat(input_file.fileno())
        if (opened.st_size, opened.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError('Fixture changed while copying')
    return str(destination)


def remaining(deadline, maximum=20):
    seconds = min(maximum, deadline - time.monotonic())
    if seconds <= 0:
        raise TimeoutError('Overall session deadline exceeded')
    return seconds


def kill_command(process, force_group=False):
    # On failure the leader has not been reaped: its PGID cannot be reused.
    if os.name != 'nt' and force_group:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except PermissionError:
            # Darwin can reject signaling an empty group after its leader exits.
            # A live leader still indicates a real cleanup permission failure.
            if process.poll() is None:
                raise
    elif process.poll() is None:
        if os.name == 'nt':
            process.kill()
        else:
            os.killpg(process.pid, signal.SIGKILL)
    process.wait(timeout=8)


def command(argv, deadline, *, data=None, check=True, maximum=20, return_status=False,
            input_file=None, output_limit=EVIDENCE_LIMIT, env=None):
    """Execute trusted configured tools with argv boundaries and a time limit."""
    seconds = remaining(deadline, maximum)
    launch = argv
    if os.name == 'nt':
        # The supervisor joins its own job BEFORE spawning the requested tool.
        launch = [sys.executable, str(Path(__file__).resolve()), '_supervise', str(seconds), *argv]
    stopped = threading.Event()
    threads = []
    completed = False
    process = subprocess.Popen(launch, stdin=subprocess.PIPE if data is not None else
                               input_file if input_file is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=env,
                               start_new_session=os.name != 'nt')
    try:
        chunks = queue.Queue(maxsize=4)
        def enqueue(item):
            while not stopped.is_set():
                try:
                    chunks.put(item, timeout=.1)
                    return
                except queue.Full:
                    pass
        def reader(pipe, key):
            try:
                for block in iter(lambda: pipe.read1(65536), b''):
                    enqueue((key, block))
            finally:
                enqueue((key, None))
        def writer():
            try:
                process.stdin.write(data)
            except (BrokenPipeError, OSError):
                pass
            finally:
                process.stdin.close()
        threads = [threading.Thread(target=reader, args=(pipe, key), daemon=True)
                   for key, pipe in (('stdout', process.stdout), ('stderr', process.stderr))]
        if data is not None:
            threads.append(threading.Thread(target=writer, daemon=True))
        for thread in threads:
            thread.start()
        output = {'stdout': bytearray(), 'stderr': bytearray()}
        eof = 0
        limit = min(deadline, time.monotonic() + seconds)
        while eof != 2:
            if time.monotonic() >= limit:
                raise subprocess.TimeoutExpired(argv, seconds)
            try:
                key, block = chunks.get(timeout=max(.001, min(.1, limit - time.monotonic())))
            except queue.Empty:
                continue
            if block is None:
                eof += 1
            else:
                output[key].extend(block)
                if sum(map(len, output.values())) > output_limit:
                    raise ValueError('Command output exceeds the evidence limit')
        process.wait(timeout=remaining(limit))
        completed = True
    except subprocess.TimeoutExpired as error:
        error.output = bytes(output['stdout'])
        error.stderr = bytes(output['stderr'])
        raise
    finally:
        stopped.set()
        kill_command(process, force_group=not completed)
        for thread in threads:
            if thread.ident is not None:
                thread.join(timeout=2)
        if process.stdin:
            process.stdin.close()
        process.stdout.close()
        process.stderr.close()
    result = subprocess.CompletedProcess(argv, process.returncode, bytes(output['stdout']), bytes(output['stderr']))
    if check and result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {(result.stderr or result.stdout).decode(errors="replace")[-1500:]}')
    return result if return_status else result.stdout


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
        verify_artifact(self.device, report['artifact_sha256'])
        self.artifact_hash = report['artifact_sha256']
        report['hardware'] = {'os': platform.platform(), **self.device.get('hardware', {})}
        data = self.root / 'data'
        data.mkdir()
        report['fixture_sha256'] = copy_fixture(self.device['saves'], data / 'ms0')
        settings = self.device.get('settings')
        if settings:
            if Path(settings).is_symlink():
                raise ValueError('Settings fixture must not be a symlink')
            copy_regular_file(settings, data / 'settings.ini')
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
            copy_regular_file(self.device['translation'], destination)
            report['translation_sha256'] = fingerprint(destination)
        env = {key: value for key, value in os.environ.items() if not key.startswith('MHP3RD_')}
        env.update(self.device.get('env', {}))
        env.update(MHP3RD_GAME_DIR=str(data), MHP3RD_DATA_DIR=str(data),
                   MHP3RD_INPUT_LIVE=str(self.root / 'input-live.txt'),
                   MHP3RD_DEBUG_COMMANDS=str(self.root / 'debug.txt'),
                   MHP3RD_SCREENSHOT_DIR=str(self.root / 'shots'), MHP3RD_SCREENSHOT_EVERY='0',
                   MHP3RD_PERF='log', MHP3RD_WINDOW_TITLE='Yakumo device scenario')
        if self.device.get('performance') == 'settings':
            # Menu regressions need the copied setting to remain editable.
            env.pop('MHP3RD_PERF')
        if self.device.get('overlays'):
            env['MHP3RD_OVERLAY_DIR'] = str(Path(self.device['overlays']).resolve())
        if self.device['build'] == 'dev':
            env['MHP3RD_DEBUG_MENU'] = '1'
        if self.device.get('observations'):
            env['MHP3RD_DEVICE_TEST'] = '1'
            env['MHP3RD_TRACE_CAMERA'] = '1'
            if 'rtc_epoch_us' in self.device:
                env['MHP3RD_DEVICE_TEST_RTC_US'] = str(self.device['rtc_epoch_us'])
        if self.device.get('translation'):
            env['MHP3RD_TRANSLATIONS_DIR'] = str(self.root / 'translations')
        self.env, self.binary = env, binary

    def launch(self):
        if fingerprint(self.binary) != self.artifact_hash:
            raise ValueError('Executable changed after preparation')
        self.log = (self.root / 'run.log').open('wb')
        # A separate bounded supervisor survives a dropped SSH/control connection.
        self.process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_supervise',
                                         str(remaining(self.deadline, 1800)), str(self.binary), *self.device.get('args', [])],
                                        cwd=self.binary.parent, env=self.env,
                                        stdin=subprocess.DEVNULL,
                                        stdout=self.log, stderr=subprocess.STDOUT)
        self.owned = True

    def text(self):
        # PERF=log flushes redirected stdout at every frame-statistics interval.
        return (self.root / 'run.log').read_text(errors='replace')

    def alive(self):
        return self.process is not None and self.process.poll() is None

    def send(self, kind, value):
        if kind not in ('debug', 'input'):
            raise ValueError('Unsupported command channel')
        destination = owned_evidence_path(self.root, 'debug.txt' if kind == 'debug' else 'input-live.txt', control=True)
        with destination.open('a', encoding='utf-8') as output:
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
        if self.owned and fingerprint(self.binary) != self.artifact_hash:
            raise ValueError('Executable changed during the session')


class Adb:
    def __init__(self, device, root, deadline):
        self.device, self.root, self.deadline = device, root, deadline
        self.prefix = device.get('adb', ['adb']) + ['-s', device['serial']]
        self.package = device['package']
        if not valid_package(self.package):
            raise ValueError('ADB automation requires a dedicated .devicetest package')
        if device.get('activity', ACTIVITY) != ACTIVITY:
            raise ValueError('Dedicated test APK must use the generated Yakumo activity')
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
        if kwargs.get('data') is not None:
            # ADB's Windows stdin treats Ctrl-Z as EOF; send binary payloads as ASCII.
            kwargs['data'] = base64.b64encode(kwargs['data'])
            text = 'base64 -d | ( ' + text + ' )'
        return self.shell('run-as ' + self.package + ' sh -c ' + shlex.quote(text), **kwargs)

    def upload(self, source, destination):
        raw = self.device.get('raw_adb_upload')
        if raw and os.name == 'nt':
            raise ValueError('Windows binary stdin requires the verified Base64 upload path')
        if raw:
            owned = os.path.normcase(os.path.realpath(self.root))
            source = os.path.normcase(os.path.realpath(source))
            if not source.startswith(owned.rstrip(os.sep) + os.sep):
                raise ValueError('Raw upload source must remain within owned staging')
        expected = fingerprint(source)
        if raw:
            with Path(source).open('rb') as stream:
                size = os.fstat(stream.fileno()).st_size
                command(self.prefix + ['shell', 'run-as', self.package, 'sh', '-c',
                                       shlex.quote('cat > ' + shlex.quote(destination))],
                        self.deadline, input_file=stream, maximum=remaining(self.deadline, 1800))
            self.verify_upload(source, destination, expected)
            return {'bytes': size, 'sha256': expected}
        # Keep encoding bounded and private, without a shared device-side game copy.
        with tempfile.TemporaryFile(mode='w+b', dir=self.root) as encoded:
            size = 0
            with Path(source).open('rb') as stream:
                for block in iter(lambda: stream.read(3 * 65536), b''):
                    remaining(self.deadline)
                    size += len(block)
                    encoded.write(base64.b64encode(block))
            encoded.seek(0)
            command(self.prefix + ['shell', 'run-as', self.package, 'sh', '-c',
                                   shlex.quote('base64 -d > ' + shlex.quote(destination))],
                    self.deadline, input_file=encoded, maximum=remaining(self.deadline, 1800))
        self.verify_upload(source, destination, expected)
        return {'bytes': size, 'sha256': expected}

    def verify_upload(self, source, destination, expected):
        actual = self.app('sha256sum ' + shlex.quote(destination)).decode(errors='replace').split()
        if not actual or actual[0] != expected or fingerprint(source) != expected:
            raise ValueError('Android upload fingerprint mismatch')

    def prepare(self, report):
        started = time.monotonic()
        overall = self.deadline
        available = remaining(overall, 1800)
        reserve = self.device.get('gameplay_reserve_seconds', 0)
        budget = min(available * self.device.get('deployment_share_percent', 70) / 100, available - reserve)
        if budget <= 0:
            raise NotTested('No deployment time remains after reserving gameplay and cleanup')
        report['deployment'] = {'available_seconds': available, 'budget_seconds': budget,
                                'gameplay_reserve_seconds': reserve, 'phases': []}
        self.session_deadline = overall
        self.deadline = min(overall, time.monotonic() + budget)
        try:
            self.prepare_with_budget(report)
        finally:
            report['deployment']['duration_seconds'] = round(time.monotonic() - started, 3)
            self.deadline = overall

    def deployment_command(self, name, argv, report):
        started = time.monotonic()
        record = {'name': safe_name(name), 'status': 'FAIL'}
        try:
            result = command(argv, self.deadline, maximum=remaining(self.deadline, 1800),
                             check=False, return_status=True)
            record.update(returncode=result.returncode, stdout=result.stdout.decode(errors='replace'),
                          stderr=result.stderr.decode(errors='replace'))
            if result.returncode:
                raise RuntimeError('Deployment phase failed: ' + name)
            record['status'] = 'PASS'
            return result.stdout
        except subprocess.TimeoutExpired as error:
            record.update(status='TIMEOUT', stdout=(error.output or b'').decode(errors='replace'),
                          stderr=(error.stderr or b'').decode(errors='replace'))
            raise
        finally:
            record['duration_seconds'] = round(time.monotonic() - started, 3)
            report['deployment']['phases'].append(record)
            write_json(self.root / ('deployment-' + safe_name(name) + '.json'), record)

    def install_staged_apk(self, apk, report):
        remote = '/data/local/tmp/yakumo-apk-' + uuid.uuid4().hex
        created = False
        try:
            self.shell('mkdir -m 700 ' + remote)
            created = True
            target = remote + '/artifact.apk'
            self.deployment_command('apk-transfer', self.prefix + ['push', str(apk), target], report)
            actual = self.shell('sha256sum ' + target).decode(errors='replace').split()
            if not actual or actual[0] != fingerprint(apk):
                raise ValueError('Transferred APK fingerprint mismatch')
            report['deployment']['apk_bytes'] = apk.stat().st_size
            report['deployment']['device_apk_sha256'] = actual[0]
            self.deployment_command('package-install', self.prefix + ['shell', 'pm install -r ' + target], report)
        finally:
            if created:
                original = self.deadline
                self.deadline = getattr(self, 'cleanup_deadline', None) or self.session_deadline
                try:
                    self.shell('rm -rf ' + remote)
                    report['deployment']['apk_staging_cleanup'] = 'PASS'
                finally:
                    self.deadline = original

    def deployment_upload(self, name, source, destination, report):
        started = time.monotonic()
        record = {'name': safe_name(name), 'status': 'FAIL'}
        try:
            record.update(self.upload(source, destination), status='PASS')
        except subprocess.TimeoutExpired as error:
            record.update(status='TIMEOUT', stdout=(error.output or b'').decode(errors='replace'),
                          stderr=(error.stderr or b'').decode(errors='replace'))
            raise
        finally:
            record['duration_seconds'] = round(time.monotonic() - started, 3)
            report['deployment']['phases'].append(record)
            write_json(self.root / ('deployment-' + safe_name(name) + '.json'), record)

    def prepare_with_budget(self, report):
        apk = self.root / 'artifact.apk'
        copy_regular_file(self.device['apk'], apk)
        with zipfile.ZipFile(apk) as archive:
            if archive.getinfo('assets/device-testing.json').file_size > 1024 * 1024:
                raise ValueError('APK test contract is oversized')
            contract = json.loads(archive.read('assets/device-testing.json'))
        if contract != android_contract(self.package, self.device.get('performance', 'log'), self.device.get('observations', False)):
            raise ValueError('APK lacks the generated device-testing contract')
        report['artifact_sha256'] = fingerprint(apk)
        verify_artifact(self.device, report['artifact_sha256'])
        if self.pids():
            raise NotTested('Dedicated test package is already running; stop it manually first')
        props = self.shell('getprop').decode(errors='replace')
        report['hardware'] = {key: next((line for line in props.splitlines() if key in line), 'unknown')
                              for key in ('ro.product.model', 'ro.build.version.release',
                                          'ro.product.cpu.abilist', 'ro.hardware')}
        vulkan = self.shell('cmd gpu vkjson', check=False)
        (self.root / 'vulkan.json').write_bytes(vulkan)
        report['hardware']['vulkan_evidence'] = 'vulkan.json'
        verify_android_capabilities(report['hardware'], vulkan)
        if self.device.get('android_cli_install'):
            self.deployment_command('combined-cli-install', self.device.get('android', ['android']) + ['--no-metrics', 'install',
                    '--device=' + self.device['serial'], '--apks=' + str(apk), '--install-options=-r'],
                    report)
        else:
            self.install_staged_apk(apk, report)
        # Remove only this dedicated test package's fixed harness directory.
        self.app('rm -rf files/device-testing && mkdir -p files/device-testing/data files/device-testing/shots')
        staging = self.root / 'data'
        staging.mkdir()
        report['fixture_sha256'] = copy_fixture(self.device['saves'], staging / 'ms0')
        if self.device.get('settings'):
            if Path(self.device['settings']).is_symlink():
                raise ValueError('Settings fixture must not be a symlink')
            copy_regular_file(self.device['settings'], staging / 'settings.ini')
            report['settings_sha256'] = fingerprint(staging / 'settings.ini')
        if self.device.get('raw_adb_upload'):
            (self.root / 'upload-inputs').mkdir()
        for name in ('EBOOT.ELF', 'disc.iso'):
            source = Path(self.device['game_dir']) / name
            if not source.is_file():
                raise NotTested('Prepared game input is unavailable')
            # Stream into app-private storage; no shared temporary copy of game data.
            if self.device.get('raw_adb_upload'):
                # Read-only upload can reuse an owned hard link without copying the ISO.
                game_root = os.path.normcase(os.path.realpath(self.device['game_dir']))
                declared = os.path.normcase(os.path.realpath(source))
                if not declared.startswith(game_root.rstrip(os.sep) + os.sep):
                    raise ValueError('Raw game input escapes its declared directory')
                staged = self.root / 'upload-inputs' / name
                try:
                    os.link(declared, staged)
                except OSError:
                    copy_regular_file(declared, staged)
                source = staged
            self.deployment_upload('game-eboot' if name == 'EBOOT.ELF' else 'game-iso', source,
                                   'files/device-testing/data/' + name, report)
        report['game_sha256'] = fingerprint(Path(self.device['game_dir']) / 'disc.iso')
        for item in sorted(staging.rglob('*')):
            relative = item.relative_to(staging).as_posix()
            if item.is_file():
                destination = 'files/device-testing/data/' + relative
                self.app('mkdir -p ' + shlex.quote(str(PurePosixPath(destination).parent)))
                self.upload(item, destination)
        self.app('touch files/device-testing/input-live.txt files/device-testing/debug.txt')
        if self.device.get('env') or self.device.get('translation'):
            raise NotTested('ADB APK contract has fixed environment; language/mod variant needs its own packaged contract')

    def launch(self):
        if self.pids():
            raise NotTested('Dedicated package started after preparation; refusing to interfere')
        self.log_file = (self.root / 'logcat.log').open('wb')
        self.log_process = subprocess.Popen(self.prefix + ['logcat', '-T', '1'],
                                            stdout=self.log_file, stderr=subprocess.STDOUT)
        try:
            self.shell('am start -W -n ' + self.package + '/' + ACTIVITY)
        finally:
            # A partial activation is owned only after observing safe numeric PIDs.
            original_deadline = self.deadline
            self.deadline = getattr(self, 'cleanup_deadline', None) or time.monotonic() + 15
            try:
                self.pid = self.pids()
                self.owned = bool(self.pid)
            finally:
                self.deadline = original_deadline
        if not self.pid:
            raise RuntimeError('Android app did not start')
        # Detached, PID-guarded watchdog survives a lost host/SSH connection.
        seconds = max(1, int(remaining(self.deadline, 1800)))
        script = (f'#!/system/bin/sh\nsleep {seconds} &\nsleeper=$!\n'
                  "trap 'kill $sleeper 2>/dev/null; exit' TERM INT HUP\n"
                  'wait $sleeper\n'
                  f'[ "$(pidof {self.package} | tr \' \' \'\\n\' | sort -n | xargs)" = "{self.pid}" ] && am force-stop {self.package}\n')
        remote = '/data/local/tmp/yakumo-test-' + uuid.uuid4().hex + '.sh'
        self.shell('cat > ' + remote, data=script.encode())
        self.watchdog = remote
        watchdog_pid = parse_pids(self.shell('nohup sh ' + remote + ' >/dev/null 2>&1 </dev/null & echo $!'))
        if not watchdog_pid or ' ' in watchdog_pid:
            raise RuntimeError('Android watchdog did not return its PID')
        self.watchdog_pid = watchdog_pid

    def text(self):
        text = self.app('cat files/yakumo.log', check=False).decode(errors='replace')
        (self.root / 'run.log').write_text(text)
        return text

    def alive(self):
        return bool(self.pids())

    def pids(self):
        return parse_pids(self.shell('pidof ' + self.package, check=False))

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
                current = self.pids()
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


def parse_pids(value):
    parts = value.decode('ascii').split()
    if any(not part.isdecimal() or len(part) > 10 or not 0 < int(part) <= 2147483647 for part in parts) or len(parts) != len(set(parts)):
        raise ValueError('Device returned an invalid PID list')
    numeric = sorted(map(int, parts))
    if len(numeric) != len(set(numeric)):
        raise ValueError('Device returned duplicate PIDs')
    return ' '.join(map(str, numeric))


def verify_artifact(device, actual):
    if device.get('artifact_sha256', actual) != actual:
        raise ValueError('Artifact differs from the configured fingerprint')


def android_contract(package, performance='log', observations=False):
    if not valid_package(package):
        raise ValueError('Test package must end in .devicetest')
    if performance not in ('log', 'settings'):
        raise ValueError('Performance must be log or settings')
    root = '/data/data/' + package + '/files/device-testing'
    contract = {'package': package, 'env': {'MHP3RD_GAME_DIR': root + '/data',
            'MHP3RD_DATA_DIR': root + '/data', 'MHP3RD_INPUT_LIVE': root + '/input-live.txt',
            'MHP3RD_DEBUG_COMMANDS': root + '/debug.txt', 'MHP3RD_DEBUG_MENU': '1',
            'MHP3RD_SCREENSHOT_DIR': root + '/shots', 'MHP3RD_PERF': 'log'}}
    if performance == 'settings':
        contract['env'].pop('MHP3RD_PERF')
    if observations:
        contract['env'].update(MHP3RD_DEVICE_TEST='1', MHP3RD_TRACE_CAMERA='1')
    return contract


def android_manifest(package, output, performance='log', observations=False):
    contract = android_contract(package, performance, observations)
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
    runner = str(Path(__file__).resolve())
    argv = [sys.executable, runner, '_match']
    if os.name != 'nt':
        # Windows command() already adds the independent job-owning supervisor.
        argv = [sys.executable, runner, '_supervise', str(remaining(deadline, 1800)), *argv]
    result = command(argv, deadline,
                     data=json.dumps([pattern, text]).encode(), maximum=1800)
    return result == b'1'


def regex_worker():
    pattern, text = json.load(sys.stdin)
    # Intentional tester-authored expression; an independent supervisor bounds it.
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
    if not checks:
        return 'NOT TESTED'
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
            'deadline_seconds': min(suite['seconds'], device.get('seconds', suite['seconds'])) + device.get('setup_seconds', 0),
            'settings': device.get('env', {}), 'hardware': device.get('hardware', {'os': 'NOT TESTED', 'gpu': 'NOT TESTED', 'driver': 'NOT TESTED'}), 'automatic': {'verdict': 'NOT TESTED', 'reason': 'Not run'},
            'observations': [], 'captures': [],
            'review': {item['id']: {**item, 'verdict': 'NOT TESTED'} for item in suite['review']}}


def run(suite, device, name, root, manual=False):
    suite, device = copy.deepcopy(suite), copy.deepcopy(device)
    report = new_report(suite, device, name)
    cap = min(suite['seconds'], device.get('seconds', suite['seconds']))
    setup = device.get('setup_seconds', 0)
    started_clock = time.monotonic()
    hard_deadline = started_clock + cap + setup
    deadline = (started_clock + setup if setup else
                hard_deadline - min(20, cap / 5) if device['adapter'] == 'streaming' else hard_deadline)
    adapter = None
    reservation = None
    lease_watchdog = None
    reservation_released = False
    launched = False
    started = time.monotonic()
    try:
        if setup and device['adapter'] != 'adb':
            raise ValueError('Setup extension is not permitted for this adapter')
        if manual and suite['execution'] != 'manual':
            raise NotTested('Use an explicit manual scenario; automatic observations must not be bypassed')
        if suite['build'] != 'either' and suite['build'] != device['build']:
            raise NotTested('Scenario needs a ' + suite['build'] + ' artifact')
        if suite.get('translation') and not device.get('translation'):
            raise NotTested('A translation fixture is required')
        if device['adapter'] == 'streaming':
            if not 1 <= device.get('seconds', 0) <= streaming_limit(device):
                raise ValueError('Streaming requires an explicit bounded overall cap')
            cli = device.get('android', ['android']) + ['--no-metrics', 'device', 'remote']
            # Parse the CLI's own creation output. Never delete a different user's reservation.
            require_idle_project(cli, device['project'], deadline)
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
        if setup:
            remaining(deadline)
            deadline = min(hard_deadline, time.monotonic() + cap)
            adapter.deadline = deadline
        write_json(root / 'report.json', report)
        render_report(root)
        if suite['execution'] == 'manual' and not manual:
            raise NotTested('Use --manual to run this human checklist')
        adapter.launch()
        launched = True
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
    except (TimeoutError, subprocess.TimeoutExpired) as error:
        ready = manual and launched and time.monotonic() >= deadline
        report['automatic'] = {'verdict': 'PASS' if ready else 'FAIL',
                               'reason': 'Manual session deadline reached' if ready else str(error)}
    except (KeyboardInterrupt, Exception) as error:
        report['automatic'] = {'verdict': 'FAIL', 'reason': str(error) or type(error).__name__}
    finally:
        try:
            if adapter:
                try:
                    if isinstance(adapter, Adb):
                        adapter.deadline = getattr(adapter, 'cleanup_deadline', None) or time.monotonic() + 15
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
    return report_exit(report)


def plain_output(text):
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', text)


def streaming_limit(device):
    maximum = device.get('streaming_cap_seconds', 300)
    if type(maximum) is not int or not 1 <= maximum <= 800:
        raise ValueError('Streaming authorization must be 1..800 seconds; default is 300')
    return maximum


def require_idle_project(cli, project, deadline, evidence=None):
    result = command(cli + ['list', '--short', '--project=' + project], deadline,
                     check=False, return_status=True)
    text = plain_output(result.stdout.decode(errors='replace')).strip()
    if evidence is not None:
        write_json(evidence, {'returncode': result.returncode, 'stdout': result.stdout.decode(errors='replace'),
                              'stderr': result.stderr.decode(errors='replace')})
    if result.returncode in (0, 1) and not result.stderr.strip() and text == 'No reservations found.':
        return
    if result.returncode or result.stderr.strip() or not text:
        raise RuntimeError('Idle project preflight failed; no reservation was created')
    raise NotTested('Idle project could not be verified; no reservation was created')


def parse_reservation(output):
    match = re.search(r'(?:new reservation created with id\s+|reservation(?:\s+id)?\s*[:=]\s*|reservations/)([a-z0-9]{8,})', plain_output(output), re.I)
    return match.group(1) if match else None


def parse_adb_serial(output):
    text = plain_output(output)
    match = re.search(r'(?:localhost|127\.0\.0\.1):\d+', text)
    if match:
        endpoint = match.group(0)
        port = endpoint.rsplit(':', 1)[1]
    else:
        match = re.search(r'(?:device |already )?connected on port\s+(\d+)', text, re.I)
        if not match:
            raise RuntimeError('No connected ADB endpoint in reservation output')
        port = match.group(1)
        endpoint = 'localhost:' + port
    if len(port) > 5 or not 0 < int(port) <= 65535:
        raise ValueError('Invalid ADB endpoint port')
    return endpoint


def lease_watchdog(payload):
    value = json.loads(base64.b64decode(payload))
    time.sleep(max(0, value['deadline'] - time.monotonic()))
    reservation = parse_reservation(Path(value['log']).read_text(errors='replace'))
    if reservation:
        command(value['cli'] + ['remove', reservation, '--project=' + value['project']], time.monotonic() + 20)
    return 0


def windows_job(pid):
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
    handle = api.OpenProcess(0x0101, False, pid)  # SET_QUOTA | TERMINATE
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
    if os.name == 'nt':
        # The sole handle stays open until interpreter exit. Closing it here
        # would kill this supervisor too; OS exit closes it and all descendants.
        windows_job(os.getpid())
    process = subprocess.Popen([binary, *args])
    try:
        return process.wait(timeout=float(seconds))
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        return 124
    finally:
        stop_process(process)


def ssh_run(suite, device, name, root, manual):
    suite, device = copy.deepcopy(suite), copy.deepcopy(device)
    remote = device['remote']
    if remote.get('adapter') not in ('local', 'adb'):
        raise ValueError('SSH delegates only local or ADB execution')
    host = device['host']
    if not isinstance(host, str) or host.startswith('-') or any(ch.isspace() for ch in host):
        raise ValueError('Use one SSH alias or user@host')
    windows = device.get('shell') == 'powershell'
    remote_root = device['checkout'].rstrip('/\\')
    runner = remote_root + '/profiles/mhp3rd/tools/device_testing.py'
    request_id = uuid.uuid4().hex
    payload = base64.b64encode(json.dumps({'scenario': suite, 'device': remote, 'name': name,
                                          'manual': manual, 'request_id': request_id}).encode()).decode()
    args = device.get('python', ['python3']) + [runner, '_remote', payload]
    if windows:
        quote = lambda text: "'" + text.replace("'", "''") + "'"
        script = '& ' + ' '.join(quote(item) for item in args) + '; exit $LASTEXITCODE'
        invocation = 'powershell -NoProfile -EncodedCommand ' + base64.b64encode(script.encode('utf-16le')).decode()
    else:
        invocation = shlex.join(args)
    prefix = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host]
    transport_cap = suite['seconds'] + remote.get('setup_seconds', 0) + 30
    deadline = time.monotonic() + transport_cap
    try:
        transport = command(prefix + [invocation], deadline, check=False, return_status=True,
                            maximum=transport_cap, output_limit=EVIDENCE_LIMIT * 3 // 2)
        output = transport.stdout.decode(errors='replace')
        (root / 'transport.log').write_text(output + transport.stderr.decode(errors='replace'))
        matches = re.findall(r'^YAKUMO_EVIDENCE:([A-Za-z0-9+/=]+)\r?$', output, re.M)
        if len(matches) != 1:
            raise ValueError('Expected exactly one remote evidence bundle')
        raw = base64.b64decode(matches[0], validate=True)
        report = import_evidence(raw, suite, device, name, root, request_id, transport.returncode)
        write_json(root / 'report.json', report)
        render_report(root)
        return report_exit(report)
    except (Exception, KeyboardInterrupt) as error:
        message = 'SSH transport/evidence failed: ' + (str(error) or type(error).__name__)
        with (root / 'transport.log').open('a') as output:
            output.write('\n' + message + '\n')
        report = new_report(suite, device, name)
        report['automatic'] = {'verdict': 'FAIL', 'reason': message}
        write_json(root / 'report.json', report)
        render_report(root)
        return 1


def report_exit(report):
    status = result_status(report)
    return 1 if status == 'FAIL' else 2 if status != 'PASS' else 0


def evidence_name(name):
    if not isinstance(name, str):
        raise ValueError('Invalid evidence filename')
    if name in EVIDENCE_FILES:
        return name
    path = PurePosixPath(name)
    if len(path.parts) == 2 and path.parts[0] == 'shots' and path.suffix in ('.bmp', '.png') and name == path.as_posix():
        safe_name(path.stem)
        return name
    raise ValueError('Unexpected evidence file: ' + name)


def owned_evidence_path(root, name, control=False):
    if control and name not in ('debug.txt', 'input-live.txt'):
        raise ValueError('Unsupported control file')
    root = os.path.normcase(os.path.realpath(root))
    lexical = os.path.normcase(os.path.abspath(os.path.join(root, name if control else evidence_name(name))))
    path = os.path.normcase(os.path.realpath(lexical))
    if path != lexical or not path.startswith(root.rstrip(os.sep) + os.sep):
        raise ValueError('Evidence path contains a symlink or escapes its session')
    return Path(path)


def evidence_members(bundle):
    names = set()
    total = 0
    members = bundle.infolist()
    if len(members) > 128:
        raise ValueError('Too many evidence files')
    for item in members:
        name = evidence_name(item.filename)
        mode = stat.S_IFMT(item.external_attr >> 16)
        if name in names or item.is_dir() or mode not in (0, stat.S_IFREG) or item.flag_bits & 1:
            raise ValueError('Duplicate, nonregular or encrypted evidence entry')
        if item.file_size > EVIDENCE_LIMIT or (name == 'report.json' and item.file_size > 1024 * 1024):
            raise ValueError('Oversized evidence file')
        names.add(name)
        total += item.file_size
    if total > EVIDENCE_LIMIT * 4 or 'report.json' not in names:
        raise ValueError('Missing report or oversized evidence archive')
    return members, names


def review_verdict(item):
    assessments = item.get('assessments', {})
    values = [value['verdict'] for value in assessments.values()]
    required = item.get('requires', 'either')
    return ('FAIL' if 'FAIL' in values else
            assessments.get(required, {}).get('verdict', 'NOT TESTED') if required != 'either' else
            'PASS' if 'PASS' in values else 'NOT TESTED')


def expected_actions(suite, device, kind):
    def expand(steps):
        for step in steps:
            if 'fixture' in step:
                yield from expand(device.get('fixtures', {}).get(step['fixture'], []))
            elif kind in step:
                yield step[kind]
    return list(expand(suite['steps']))


def validate_remote_report(report, suite, device, name, request_id, names):
    remote = device['remote']
    expected = new_report(suite, remote, name)
    for key in ('schema', 'scenario', 'scenario_sha256', 'execution', 'device', 'build', 'commit', 'settings', 'deadline_seconds'):
        if report.get(key) != expected[key]:
            raise ValueError('Remote report identity differs: ' + key)
    if report.get('request_id') != request_id or not re.fullmatch(r'[0-9a-f]{32}', report.get('run_id', '')):
        raise ValueError('Remote report is stale or lacks the request identity')
    automatic = report['automatic']
    if automatic.get('verdict') not in VERDICTS or not isinstance(automatic.get('reason'), str):
        raise ValueError('Invalid remote automatic verdict')
    artifact = report.get('artifact_sha256')
    if (artifact is not None or automatic['verdict'] == 'PASS') and not re.fullmatch(r'[0-9a-f]{64}', artifact or ''):
        raise ValueError('Remote report lacks a valid artifact fingerprint')
    verify_artifact(device, artifact)
    verify_artifact(remote, artifact)
    captures = report['captures']
    if not isinstance(captures, list) or len(captures) != len(set(captures)):
        raise ValueError('Invalid remote captures')
    for path in captures:
        if evidence_name(path) not in names or not path.startswith('shots/'):
            raise ValueError('Remote capture is missing or invalid')
    if automatic['verdict'] == 'PASS' and suite['execution'] == 'automatic':
        if report['observations'] != expected_actions(suite, remote, 'wait') or captures != [
                'shots/' + key + ('.png' if remote['adapter'] == 'adb' else '.bmp')
                for key in expected_actions(suite, remote, 'capture')]:
            raise ValueError('Remote PASS lacks the required observations/captures')
    if set(report['review']) != set(expected['review']):
        raise ValueError('Remote review checklist differs')
    for key, check in expected['review'].items():
        item = report['review'][key]
        if item.get('expect') != check['expect'] or item.get('requires', 'either') != check.get('requires', 'either') or item.get('id') != key:
            raise ValueError('Remote review requirements differ')
        assessments = item.get('assessments', {})
        if set(assessments) - {'human', 'agent'}:
            raise ValueError('Unknown remote reviewer')
        for value in assessments.values():
            if value.get('verdict') not in VERDICTS or not isinstance(value.get('notes'), str) or not value['notes'].strip():
                raise ValueError('Invalid remote review verdict')
            if not isinstance(value.get('evidence'), list) or any(evidence_name(path) not in names for path in value['evidence']):
                raise ValueError('Remote assessment evidence is missing or invalid')
        if item.get('verdict') != review_verdict(item):
            raise ValueError('Remote review verdict does not follow its requirements')
    return report


def import_evidence(raw, suite, device, name, root, request_id, returncode):
    import io
    if len(raw) > EVIDENCE_LIMIT:
        raise ValueError('Oversized encoded evidence bundle')
    with zipfile.ZipFile(io.BytesIO(raw)) as bundle:
        members, names = evidence_members(bundle)
        report = validate_remote_report(json.loads(bundle.read('report.json')), suite, device, name, request_id, names)
        if returncode != report_exit(report):
            raise ValueError('SSH exit status differs from remote report')
        # Validate every destination before writing; regenerate Markdown locally.
        targets = {item.filename: owned_evidence_path(root, item.filename) for item in members}
        for item in members:
            if item.filename == 'report.json':
                continue
            target = targets[item.filename]
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as output:
                output.write(bundle.read(item))
    return report


def export_evidence(root):
    import io
    output = io.BytesIO()
    paths = [root / name for name in EVIDENCE_FILES] + list((root / 'shots').glob('*'))
    paths = [path for path in paths if path.exists() or path.is_symlink()]
    if len(paths) > 128:
        raise ValueError('Too many exported evidence files')
    total = 0
    for path in paths:
        owned_evidence_path(root, path.relative_to(root).as_posix())
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('Exported evidence must be a regular owned file')
        total += before.st_size
        if before.st_size > EVIDENCE_LIMIT or total > EVIDENCE_LIMIT * 4:
            raise ValueError('Exported evidence is oversized')
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for path in paths:
            # Hold a checked descriptor, never reopen a capture by its name.
            before = path.lstat()
            flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_BINARY', 0)
            with os.fdopen(os.open(path, flags), 'rb') as source:
                opened = os.fstat(source.fileno())
                if not stat.S_ISREG(opened.st_mode) or (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
                    raise ValueError('Evidence changed before export')
                with bundle.open(path.relative_to(root).as_posix(), 'w') as destination:
                    copied = 0
                    while chunk := source.read(65536):
                        copied += len(chunk)
                        if copied > opened.st_size:
                            raise ValueError('Evidence grew during export')
                        destination.write(chunk)
                        if output.tell() > EVIDENCE_LIMIT:
                            raise ValueError('Exported evidence archive is oversized')
                after = os.fstat(source.fileno())
                if (opened.st_size, opened.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise ValueError('Evidence changed during export')
        evidence_members(bundle)
    if output.tell() > EVIDENCE_LIMIT:
        raise ValueError('Exported evidence archive is oversized')
    return output.getvalue()


def remote_run(payload):
    value = json.loads(base64.b64decode(payload))
    suite = validate_scenario(value['scenario'])
    device = validate_device(value['device'])
    root = output_directory(tempfile.gettempdir())
    code = run(suite, device, safe_name(value['name']), root, value['manual'])
    report = read_json(root / 'report.json')
    report['request_id'] = value['request_id']
    write_json(root / 'report.json', report)
    print('YAKUMO_EVIDENCE:' + base64.b64encode(export_evidence(root)).decode(), flush=True)
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
    item.update(verdict=review_verdict(item), reviewer=reviewer, notes=notes, evidence=evidence)
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
    part.add_argument('--performance', choices=('log', 'settings'), default='log')
    part.add_argument('--observations', action='store_true', help='Enable developer-only structured readbacks')
    args = parser.parse_args(argv)
    try:
        if args.command == 'list':
            for path in sorted(SCENARIOS.glob('*.json')):
                value = validate_scenario(read_json(path))
                print(f'{value["id"]}: {value["build"]}/{value["execution"]} - {value["description"]}')
        elif args.command == 'android-manifest':
            write_json(args.contract, android_manifest(args.package, args.output, args.performance, args.observations))
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
