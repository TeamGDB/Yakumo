"""Public contracts for bounded device scenarios; no game files or cloud account."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('device_testing', ROOT / 'profiles/mhp3rd/tools/device_testing.py')
dt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dt)

FAKE_GAME = '''import os, pathlib, time
root = pathlib.Path(os.environ['MHP3RD_DATA_DIR']).parent
assert pathlib.Path(os.environ['MHP3RD_GAME_DIR']) == root / 'data'
print('Functions: 12345', flush=True)
print('[overlay] installed demo_task', flush=True)
offset = 0
while True:
    live = (root / 'input-live.txt').read_text()
    for line in live[offset:].splitlines():
        if 'shot ' in line:
            name = line.split('shot ')[1]
            (root / 'shots' / (name + '.bmp')).write_bytes(b'BM fixture')
        if 'quit' in line:
            raise SystemExit(0)
    offset = len(live)
    time.sleep(.02)
'''


class DeviceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='yakumo-device-contract-')
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        self.game.mkdir()
        for name in ('EBOOT.ELF', 'disc.iso'):
            (self.game / name).write_bytes(b'public synthetic bytes')
        self.saves = self.root / 'saves'
        self.saves.mkdir()
        (self.saves / 'save.dat').write_bytes(b'original fixture')
        self.fake = self.root / 'fake.py'
        self.fake.write_text(FAKE_GAME)
        self.device = {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40,
                       'binary': sys.executable, 'args': [str(self.fake)], 'seconds': 5,
                       'game_dir': str(self.game), 'saves': str(self.saves)}
        self.suite = dt.scenario('boot')
        self.suite['seconds'] = 5
        self.run_dir = self.root / 'evidence'
        self.run_dir.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def report(self):
        return dt.read_json(self.run_dir / 'report.json')

    def test_real_bounded_game_fixture_and_review(self):
        self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 2)
        report = self.report()
        self.assertEqual(report['automatic']['verdict'], 'PASS')
        self.assertEqual(dt.result_status(report), 'NOT TESTED')
        self.assertEqual(report['captures'], ['shots/opening.bmp'])
        self.assertEqual((self.saves / 'save.dat').read_bytes(), b'original fixture')
        self.assertNotEqual(self.run_dir / 'data/ms0', self.saves)
        dt.review(self.run_dir, 'opening', 'PASS', 'agent', 'Synthetic image inspected', ['shots/opening.bmp'])
        dt.render_report(self.run_dir)
        self.assertEqual(dt.result_status(self.report()), 'PASS')
        self.assertIn('PASS', (self.run_dir / 'review.md').read_text())

    def test_timeout_is_failure_and_process_stopped(self):
        self.suite['steps'] = [{'wait': 'condition-never-emitted', 'timeout': .25}]
        self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 1)
        self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')
        self.assertLess(self.report()['duration_seconds'], 10)

    def test_early_crash_and_debug_refusal(self):
        for content in ("raise SystemExit(3)", "print('[debug] not started, menu open', flush=True); import time; time.sleep(2)"):
            with self.subTest(content=content):
                self.fake.write_text(content)
                self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 1)
                self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')

    def test_missing_fixture_and_release_dev_guard(self):
        self.device['build'] = 'release'
        self.assertEqual(dt.run(dt.scenario('quest-village'), self.device, 'mac', self.run_dir), 2)
        self.assertEqual(self.report()['automatic']['verdict'], 'NOT TESTED')
        self.device['build'] = 'dev'
        self.suite['steps'] = [{'fixture': 'load-character'}]
        self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 2)
        self.assertIn('Missing fixture navigation', self.report()['automatic']['reason'])

    def test_manual_run_does_not_automate_or_infer_visual_pass(self):
        self.device['seconds'] = .3
        suite = dt.scenario('release-player')
        self.device['build'] = 'release'
        suite['steps'] = [{'input': '0:quit'}]
        self.assertEqual(dt.run(suite, self.device, 'mac', self.run_dir, manual=True), 2)
        report = self.report()
        self.assertEqual(report['automatic']['verdict'], 'PASS')
        self.assertEqual(report['captures'], [])
        self.assertEqual(dt.result_status(report), 'NOT TESTED')

    def test_manual_permission_and_missing_translation(self):
        self.device['build'] = 'release'
        self.assertEqual(dt.run(dt.scenario('release-player'), self.device, 'mac', self.run_dir), 2)
        self.assertIn('Use --manual', self.report()['automatic']['reason'])
        self.device['build'] = 'dev'
        self.assertEqual(dt.run(dt.scenario('quest-translation'), self.device, 'mac', self.run_dir, True), 2)
        self.assertIn('translation fixture', self.report()['automatic']['reason'])

    def test_fixture_symlinks_and_special_files_rejected(self):
        link = self.saves / 'alias'
        try:
            link.symlink_to(self.saves / 'save.dat')
        except OSError:
            self.skipTest('Symlink privilege unavailable')
        with self.assertRaises(ValueError):
            dt.copy_fixture(self.saves, self.root / 'copy')
        link.unlink()
        if hasattr(os, 'mkfifo'):
            os.mkfifo(link)
            with self.assertRaises(ValueError):
                dt.copy_fixture(self.saves, self.root / 'copy')

    def test_validation_and_config_controls(self):
        bad_steps = [{'capture': '../escape'}, {'input': '0:pad a;100:pad a'},
                     {'debug': 'state\nquest'}, {'wait': '['}, {'wait': 'ready', 'timeout': 0},
                     {'wait': 'ready', 'unknown': True}, {'wait': 'ready', 'capture': 'both'}]
        for item in bad_steps:
            with self.subTest(item=item), self.assertRaises((ValueError, TypeError)):
                dt.validate_step(item)
        for changed in ({'build': 'release', 'steps': [{'debug': 'quest start 204'}]},
                        {'execution': 'automatic', 'steps': []}, {'seconds': 1801},
                        {'review': [{'id': 'same', 'expect': 'x'}, {'id': 'same', 'expect': 'y'}]}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                dt.validate_scenario({**self.suite, **changed})
        config = self.root / 'config.json'
        for changed in ({'commit': 'unknown'}, {'env': {'MHP3RD_DATA_DIR': '/player'}},
                        {'adapter': 'unknown'}, {'seconds': 0},
                        {'fixtures': {'x': [{'fixture': 'x'}]}},
                        {'build': 'release', 'fixtures': {'x': [{'debug': 'quest start 204'}]}}):
            dt.write_json(config, {'devices': {'mac': {**self.device, **changed}}})
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                dt.load_device(config, 'mac')
        dt.write_json(config, {'devices': {'mac': self.device}})
        self.assertEqual(dt.load_device(config, 'mac')['commit'], 'a' * 40)

    def test_output_parent_and_review_evidence_escape(self):
        self.assertTrue(dt.output_directory(self.root).is_dir())
        with self.assertRaises(ValueError):
            dt.output_directory('/nonexistent-parent')
        dt.write_json(self.run_dir / 'report.json', dt.new_report(self.suite, self.device, 'mac'))
        for evidence in ('../fake.py', '/etc/passwd', 'missing.png'):
            with self.assertRaises(ValueError):
                dt.review(self.run_dir, 'opening', 'PASS', 'human', 'notes', [evidence])
        with self.assertRaises(ValueError):
            dt.review(self.run_dir, 'opening', 'PASS', 'human', ' ', [])
        with self.assertRaises(ValueError):
            dt.review(self.run_dir, 'opening', 'GREEN', 'human', 'notes', [])

    def test_final_report_preserves_review_written_during_session(self):
        class Adapter:
            owned = True
            def __init__(inner, device, root, deadline):
                inner.root = root
            def prepare(inner, report):
                report['artifact_sha256'] = 'b' * 64
            def launch(inner):
                dt.review(inner.root, 'opening', 'PASS', 'human', 'Observed image', [])
            def text(inner):
                return 'ready'
            def alive(inner):
                return True
            def close(inner):
                pass
        self.suite['steps'] = [{'wait': 'ready'}]
        with patch.object(dt, 'Local', Adapter):
            self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 0)
        self.assertEqual(self.report()['review']['opening']['reviewer'], 'human')

    def test_interrupt_and_failed_log_collection_still_clean_up(self):
        class Adapter:
            closed = False
            owned = True
            def __init__(inner, *args):
                pass
            def prepare(inner, report):
                pass
            def launch(inner):
                raise KeyboardInterrupt()
            def text(inner):
                raise OSError('log unavailable')
            def close(inner):
                Adapter.closed = True
        with patch.object(dt, 'Local', Adapter):
            self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 1)
        self.assertTrue(Adapter.closed)

    def test_fresh_log_observation_and_fixture_steps(self):
        class Adapter:
            content = 'old-marker'
            def text(inner):
                return inner.content
            def alive(inner):
                return False
            def send(inner, kind, value):
                inner.content += '\nnew-marker'
            def capture(inner, name):
                return 'shots/' + name + '.png'
        adapter = Adapter()
        report = {'observations': [], 'captures': []}
        dt.execute_steps(adapter, [{'input': '0:pad a'}, {'wait': 'new-marker'}, {'capture': 'one'}], self.device, report, time.monotonic() + 2)
        self.assertEqual(report['observations'], ['new-marker'])
        with self.assertRaises(RuntimeError):
            dt.wait_condition(adapter, 'old-marker', 1, time.monotonic() + 2, len('old-marker'))
        self.device['fixtures'] = {'load': [{'wait': 'new-marker'}]}
        dt.execute_steps(adapter, [{'fixture': 'load'}], self.device, report, time.monotonic() + 2)
        self.device['build'] = 'release'
        with self.assertRaises(dt.NotTested):
            dt.execute_steps(adapter, [{'debug': 'state'}], self.device, report, time.monotonic() + 2)

    def test_artifact_bound_matrix_and_regeneration(self):
        report = dt.new_report(dt.scenario('boot'), self.device, 'mac')
        report['artifact_sha256'] = 'b' * 64
        report['automatic'] = {'verdict': 'PASS'}
        report['review']['opening']['verdict'] = 'PASS'
        runs = self.root / 'runs'
        (runs / 'one').mkdir(parents=True)
        dt.write_json(runs / 'one/report.json', report)
        requirement = {key: report[key] for key in ('device', 'scenario', 'commit', 'artifact_sha256', 'build')}
        plan = self.root / 'plan.json'
        output = self.root / 'matrix.md'
        dt.write_json(plan, {'required': [requirement]})
        self.assertEqual(dt.matrix(plan, runs, output), 0)
        self.assertEqual(dt.matrix(plan, runs, output), 0)
        self.assertEqual(dt.read_json(runs / 'one/report.json')['review']['opening']['verdict'], 'PASS')
        requirement['artifact_sha256'] = 'c' * 64
        dt.write_json(plan, {'required': [requirement]})
        self.assertEqual(dt.matrix(plan, runs, output), 2)
        requirement['artifact_sha256'] = 'b' * 64
        report['review']['opening']['verdict'] = 'FAIL'
        dt.write_json(runs / 'one/report.json', report)
        dt.write_json(plan, {'required': [requirement]})
        self.assertEqual(dt.matrix(plan, runs, output), 1)
        for rows in ([], [requirement, requirement], [{**requirement, 'commit': 'unknown'}]):
            dt.write_json(plan, {'required': rows})
            with self.assertRaises(ValueError):
                dt.matrix(plan, runs, output)

    def test_human_and_agent_reviews_are_independent(self):
        report = dt.new_report(dt.scenario('release-player'), {**self.device, 'build': 'release'}, 'mac')
        report['artifact_sha256'] = 'a' * 64
        dt.write_json(self.run_dir / 'report.json', report)
        dt.review(self.run_dir, 'audio', 'PASS', 'agent', 'Log inspected, listening still needed', [])
        self.assertEqual(self.report()['review']['audio']['verdict'], 'NOT TESTED')
        dt.review(self.run_dir, 'audio', 'PASS', 'human', 'Listened to music and effects', [])
        check = self.report()['review']['audio']
        self.assertEqual(set(check['assessments']), {'agent', 'human'})
        self.assertEqual(check['verdict'], 'PASS')
        dt.review(self.run_dir, 'audio', 'FAIL', 'agent', 'A reproducible audio error found', [])
        self.assertEqual(self.report()['review']['audio']['verdict'], 'FAIL')
        self.assertIn('human: PASS', (self.run_dir / 'review.md').read_text())
        self.assertEqual(self.report()['review']['audio']['assessments']['human']['notes'], 'Listened to music and effects')

    def test_android_manifest_and_non_test_package_rejection(self):
        output = self.root / 'manifest.xml'
        contract = dt.android_manifest('org.example.yakumo.devicetest', output)
        self.assertIn('SDL_ENV.MHP3RD_DATA_DIR', output.read_text())
        self.assertIn('io.github.teamgdb.yakumo.YakumoActivity', output.read_text())
        self.assertEqual(contract['env']['MHP3RD_DEBUG_MENU'], '1')
        with self.assertRaises(ValueError):
            dt.android_contract('io.github.teamgdb.yakumo')
        with self.assertRaises(ValueError):
            dt.Adb({'package': 'io.github.teamgdb.yakumo', 'serial': 'usb'}, self.run_dir, 10)

    def test_subprocess_failure_timeout_and_metrics(self):
        with self.assertRaises(RuntimeError):
            dt.command([sys.executable, '-c', "import sys; print('failure'); sys.exit(3)"], time.monotonic() + 2)
        with self.assertRaises(TimeoutError):
            dt.remaining(time.monotonic() - 1)
        with self.assertRaises(subprocess.TimeoutExpired):
            dt.command([sys.executable, '-c', 'import time; time.sleep(2)'], time.monotonic() + .1)
        metrics = dt.summarize_log('[perf] fps 30.0 game 30.0 speed 100%\n[gpu] Vulkan driver example')
        self.assertEqual(metrics['performance_samples'][0]['game_fps'], 30)
        self.assertEqual(len(metrics['gpu_driver_lines']), 1)

    def test_cli_list_prepare_and_review(self):
        config = self.root / 'config.json'
        dt.write_json(config, {'devices': {'mac': self.device}})
        self.assertEqual(dt.main(['list']), 0)
        self.assertEqual(dt.main(['prepare', 'boot', '--config', str(config), '--device', 'mac', '--output-parent', str(self.root)]), 2)
        manifest = self.root / 'test.xml'
        self.assertEqual(dt.main(['android-manifest', '--package', 'org.example.yakumo.devicetest', '--output', str(manifest), '--contract', str(self.root / 'contract.json')]), 0)
    def make_adb(self):
        if os.name == 'nt':
            self.skipTest('Synthetic ADB transport uses POSIX shell; real Windows supervisor is tested separately')
        import zipfile
        self.adb_root = self.root / 'android-device'
        self.adb_root.mkdir()
        (self.adb_root / 'files').mkdir()
        package = 'org.example.yakumo.devicetest'
        apk = self.root / 'test.apk'
        with zipfile.ZipFile(apk, 'w') as archive:
            archive.writestr('assets/device-testing.json', json.dumps(dt.android_contract(package)))
        adb = self.root / 'fake-adb.py'
        adb.write_text('''import os, pathlib, shlex, subprocess, sys
root = pathlib.Path(os.environ['FAKE_ADB_ROOT'])
args = sys.argv[3:]
if args[0] == 'install':
    print('Success')
elif args[:3] == ['exec-out', 'screencap', '-p']:
    sys.stdout.buffer.write(b'\\x89PNG\\r\\n\\x1a\\npublic fixture')
elif args[0] == 'shell':
    words = shlex.split(' '.join(args[1:]))
    if words[0] == 'run-as':
        payload = words[4]
        result = subprocess.run(['sh', '-c', payload], cwd=root, input=sys.stdin.buffer.read(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    if words[0] == 'pidof':
        if (root / 'pid').exists(): print((root / 'pid').read_text())
    elif words[:2] == ['am', 'start']:
        (root / 'pid').write_text('12345')
        (root / 'files/yakumo.log').write_text('Functions: 12345\\n[overlay] installed demo_task\\n')
        print('Status: ok')
    elif words[:2] == ['am', 'force-stop']:
        (root / 'pid').unlink(missing_ok=True)
    elif words[0] == 'getprop':
        print('[ro.product.model]: [Synthetic Android]\\n[ro.product.cpu.abilist]: [arm64-v8a]')
    elif words[:3] == ['cmd', 'gpu', 'vkjson']:
        print('{"devices": [{"properties": {"apiVersion": 4198400}}]}')
    elif words[0] == 'nohup':
        print('12346')
    elif words[0] in ('cat', 'rm', 'kill'):
        sys.stdin.buffer.read()
elif args[0] == 'logcat':
    import time
    time.sleep(20)
''')
        device = {**self.device, 'adapter': 'adb', 'package': package, 'serial': 'synthetic-usb',
                  'apk': str(apk), 'adb': [sys.executable, str(adb)]}
        return device

    def test_adb_public_transport_stages_controls_and_cleans(self):
        device = self.make_adb()
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            self.assertEqual(dt.run(self.suite, device, 'phone', self.run_dir), 2)
        report = self.report()
        self.assertEqual(report['automatic']['verdict'], 'PASS')
        self.assertEqual(report['captures'], ['shots/opening.png'])
        self.assertFalse((self.adb_root / 'pid').exists())
        self.assertEqual((self.adb_root / 'files/device-testing/data/ms0/save.dat').read_bytes(), b'original fixture')
        self.assertEqual((self.adb_root / 'files/device-testing/data/disc.iso').read_bytes(), b'public synthetic bytes')

    def test_adb_running_package_is_not_disrupted(self):
        device = self.make_adb()
        (self.adb_root / 'pid').write_text('current-user-session')
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            self.assertEqual(dt.run(self.suite, device, 'phone', self.run_dir), 2)
        self.assertEqual((self.adb_root / 'pid').read_text(), 'current-user-session')
        self.assertIn('already running', self.report()['automatic']['reason'])

    def test_adb_replacement_pid_and_capture_failure(self):
        device = self.make_adb()
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            adapter = dt.Adb(device, self.run_dir, time.monotonic() + 5)
            adapter.owned = True
            adapter.pid = 'original-owned-pid'
            (self.adb_root / 'pid').write_text('other-pid')
            with self.assertRaises(RuntimeError):
                adapter.close()
            self.assertEqual((self.adb_root / 'pid').read_text(), 'other-pid')
            with patch.object(adapter, 'adb', return_value=b'not a screenshot'):
                with self.assertRaises(RuntimeError):
                    adapter.capture('screen')

    def test_cloud_cleanup_after_known_partial_creation_failure(self):
        device = {**self.device, 'adapter': 'streaming', 'seconds': 5, 'model': 'synthetic/33',
                  'project': 'public-fixture', 'android': [sys.executable, str(self.root / 'cloud.py')]}
        (self.root / 'cloud.py').write_text("print('Reservation ID: abcdefgh1234', flush=True); raise SystemExit(3)")
        calls = []
        def cleanup(args, deadline, **kwargs):
            calls.append(args)
            return b''
        with patch.object(dt, 'command', cleanup):
            self.assertEqual(dt.run(self.suite, device, 'cloud', self.run_dir), 1)
        self.assertTrue(any('remove' in item and 'abcdefgh1234' in item for item in calls))
        self.assertTrue(all('--no-metrics' in item for item in calls))

    def test_cloud_failure_without_id_never_deletes_unowned_devices(self):
        device = {**self.device, 'adapter': 'streaming', 'seconds': 5, 'model': 'synthetic/33',
                  'project': 'public-fixture', 'android': [sys.executable, str(self.root / 'cloud.py')]}
        (self.root / 'cloud.py').write_text("print('No available device'); raise SystemExit(3)")
        with patch.object(dt, 'command', return_value=b'No reservations found.') as calls:
            self.assertEqual(dt.run(self.suite, device, 'cloud', self.run_dir), 1)
        self.assertFalse(any('remove' in item.args[0] for item in calls.call_args_list))
        self.assertIsNone(dt.parse_reservation('unrelated output'))
        with self.assertRaises(RuntimeError):
            dt.parse_adb_serial('no endpoint')
        self.assertEqual(dt.parse_adb_serial('ADB localhost:1234'), 'localhost:1234')

    def test_cloud_success_still_releases_on_adapter_error(self):
        device = {**self.device, 'adapter': 'streaming', 'seconds': 5, 'model': 'synthetic/33',
                  'project': 'public-fixture', 'android': [sys.executable, str(self.root / 'cloud.py')]}
        (self.root / 'cloud.py').write_text("print('New reservation created with id abcdefgh1234. Device connected on port 1234', flush=True)")
        calls = []
        def command(args, deadline, **kwargs):
            calls.append(args)
            return b'No reservations found.' if 'list' in args else b''
        with patch.object(dt, 'command', command), patch.object(dt, 'Adb', side_effect=ValueError('bad contract')):
            self.assertEqual(dt.run(self.suite, device, 'cloud', self.run_dir), 1)
        self.assertEqual(calls[-1][-3:], ['remove', 'abcdefgh1234', '--project=public-fixture'])
        device['seconds'] = 301
        self.assertEqual(dt.run(self.suite, device, 'cloud', self.run_dir), 1)

    @unittest.skipIf(os.name == 'nt', 'Synthetic SSH executable uses POSIX shell')
    def test_ssh_executes_same_real_runner_and_imports_only_evidence(self):
        fake_bin = self.root / 'bin'
        fake_bin.mkdir()
        ssh = fake_bin / 'ssh'
        ssh.write_text('#!/bin/sh\nfor last; do :; done\nexec sh -c "$last"\n')
        ssh.chmod(0o755)
        device = {**self.device, 'adapter': 'ssh', 'host': 'synthetic-host',
                  'checkout': str(ROOT), 'python': [sys.executable], 'remote': self.device}
        with patch.dict(os.environ, {'PATH': str(fake_bin) + os.pathsep + os.environ.get('PATH', '')}):
            self.assertEqual(dt.ssh_run(self.suite, device, 'deck', self.run_dir, False), 2)
        self.assertEqual(self.report()['automatic']['verdict'], 'PASS')
        self.assertFalse((self.run_dir / 'data').exists())
        self.assertTrue((self.run_dir / 'shots/opening.bmp').is_file())

    def test_pathological_regex_and_ssh_timeout_are_bounded_failures(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            dt.bounded_match('(a+)+$', 'a' * 1000 + '!', time.monotonic() + .2)
        self.assertTrue(dt.bounded_match('ready', 'ready', time.monotonic() + 2))
        self.assertFalse(dt.bounded_match('ready', 'not yet', time.monotonic() + 2))
        device = {**self.device, 'adapter': 'ssh', 'host': 'synthetic-host',
                  'checkout': str(ROOT), 'remote': self.device}
        for error in (subprocess.TimeoutExpired('ssh', .1), KeyboardInterrupt()):
            with patch.object(dt, 'command', side_effect=error):
                self.assertEqual(dt.ssh_run(self.suite, device, 'deck', self.run_dir, False), 1)
            self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')
            self.assertIn('SSH transport failed', (self.run_dir / 'transport.log').read_text())
        for name in ('org.example.devicetest', 'a.b.devicetest'):
            self.assertTrue(dt.valid_package(name))
        for name in ('a.devicetest', 'a..devicetest', 'a.b-devicetest', 'a.' + 'a' * 200 + '.devicetest'):
            self.assertFalse(dt.valid_package(name))

    def test_ssh_failure_and_powershell_quoting(self):
        device = {**self.device, 'adapter': 'ssh', 'host': 'synthetic-host',
                  'checkout': 'C:/Example Space/Yakumo', 'python': ['python'],
                  'shell': 'powershell', 'remote': self.device}
        with patch.object(dt, 'command', return_value=b'connection failed') as calls:
            self.assertEqual(dt.ssh_run(self.suite, device, 'windows', self.run_dir, False), 1)
        self.assertIn('EncodedCommand', calls.call_args.args[0][-1])
        device['host'] = '-option'
        with self.assertRaises(ValueError):
            dt.ssh_run(self.suite, device, 'windows', self.run_dir, False)
    def test_prepared_manual_release_can_be_reviewed_without_fake_automation(self):
        suite = dt.scenario('release-player')
        self.device['build'] = 'release'
        report = dt.new_report(suite, self.device, 'phone')
        report['artifact_sha256'] = dt.fingerprint(self.fake)
        dt.write_json(self.run_dir / 'report.json', report)
        for key in report['review']:
            dt.review(self.run_dir, key, 'PASS', 'human', 'Observed expected behavior on exact synthetic artifact', [])
        self.assertEqual(dt.result_status(self.report()), 'PASS')
        self.assertEqual(self.report()['automatic']['verdict'], 'NOT TESTED')
        dt.review(self.run_dir, 'dlc', 'NOT TESTED', 'human', 'No DLC fixture available', [])
        self.assertEqual(dt.result_status(self.report()), 'NOT TESTED')

    def test_stale_scenario_cannot_certify_current_release_plan(self):
        report = dt.new_report(dt.scenario('boot'), self.device, 'mac')
        report['artifact_sha256'] = 'b' * 64
        report['scenario_sha256'] = 'outdated-scenario'
        report['automatic']['verdict'] = 'PASS'
        report['review']['opening']['verdict'] = 'PASS'
        runs = self.root / 'runs'
        (runs / 'one').mkdir(parents=True)
        dt.write_json(runs / 'one/report.json', report)
        required = {key: report[key] for key in ('device', 'scenario', 'commit', 'artifact_sha256', 'build')}
        dt.write_json(self.root / 'plan.json', {'required': [required]})
        self.assertEqual(dt.matrix(self.root / 'plan.json', runs, self.root / 'matrix.md'), 2)

    def test_cloud_deadline_watchdog_owns_only_its_creation_id(self):
        log = self.root / 'creation.log'
        log.write_text('New reservation created with id abcdefgh1234. Device connected on port 1234')
        payload = dt.base64.b64encode(json.dumps({'log': str(log), 'deadline': time.monotonic(),
                     'cli': ['android', '--no-metrics', 'device', 'remote'], 'project': 'public-fixture'}).encode()).decode()
        with patch.object(dt, 'command', return_value=b'') as calls:
            self.assertEqual(dt.lease_watchdog(payload), 0)
        self.assertEqual(calls.call_args.args[0][-3:], ['remove', 'abcdefgh1234', '--project=public-fixture'])
        self.assertEqual(dt.parse_reservation('\x1b[32mNew reservation created with id abcdefgh1234\x1b[0m'), 'abcdefgh1234')
        self.assertEqual(dt.parse_adb_serial('Device connected on port 1234'), 'localhost:1234')

    def test_manual_crash_cannot_become_a_human_session_pass(self):
        self.device['build'] = 'release'
        self.fake.write_text('raise SystemExit(3)')
        self.assertEqual(dt.run(dt.scenario('release-player'), self.device, 'mac', self.run_dir, True), 1)
        self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')

    def test_automatic_scenario_cannot_bypass_observations_with_manual_flag(self):
        self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir, True), 2)
        self.assertIn('must not be bypassed', self.report()['automatic']['reason'])

    def test_android_abi_vulkan_preconditions(self):
        hardware = {'ro.product.cpu.abilist': 'arm64-v8a,armeabi-v7a'}
        valid = b'{"devices": [{"properties": {"apiVersion": 4198400, "deviceName": "Synthetic GPU", "driverVersion": 1}}]}'
        dt.verify_android_capabilities(hardware, valid)
        self.assertEqual(hardware['gpus'][0]['deviceName'], 'Synthetic GPU')
        for device, vk in (({}, valid), (hardware, b'not JSON'),
                           (hardware, b'{"devices": []}'),
                           (hardware, b'{"devices": [{"properties": {"apiVersion": 4194304}}]}')):
            with self.assertRaises(dt.NotTested):
                dt.verify_android_capabilities(device, vk)


if __name__ == '__main__':
    unittest.main()
