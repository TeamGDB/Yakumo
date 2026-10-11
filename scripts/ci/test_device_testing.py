"""Public contracts for bounded device scenarios; no game files or cloud account."""
import importlib.util
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import signal
import stat
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('device_testing', ROOT / 'profiles/mhp3rd/tools/device_testing.py')
dt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dt)

FAKE_GAME = '''import os, pathlib, time
root = pathlib.Path(os.environ['MHP3RD_DATA_DIR']).parent
assert pathlib.Path(os.environ['MHP3RD_GAME_DIR']) == root / 'data'
print('Functions:  12345', flush=True)
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
                       'binary': sys.executable, 'args': [str(self.fake)], 'seconds': 30 if os.name == 'nt' else 5,
                       'game_dir': str(self.game), 'saves': str(self.saves)}
        self.suite = dt.scenario('boot')
        self.suite['seconds'] = 30 if os.name == 'nt' else 5
        self.run_dir = self.root / 'evidence'
        self.run_dir.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def report(self):
        return dt.read_json(self.run_dir / 'report.json')

    def test_real_bounded_game_fixture_and_review(self):
        self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 2, self.report())
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

    def test_boot_counter_accepts_native_banner_spacing_and_requires_nonzero_count(self):
        pattern = dt.scenario('boot')['steps'][0]['wait']
        for text in ('Functions:  362482', 'Functions: 12345', 'Functions:\t12345'):
            self.assertIsNotNone(dt.re.search(pattern, text))
        for text in ('Functions:  0', 'Functions: missing', 'Functions:\n362482'):
            self.assertIsNone(dt.re.search(pattern, text))

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
        self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 2, self.report())
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

    def test_settings_scenarios_keep_visual_and_physical_checks_unverified(self):
        for name in ('settings-menu', 'performance-overlay'):
            with self.subTest(scenario=name):
                suite = dt.scenario(name)
                self.assertEqual(suite['build'], 'either')
                self.assertEqual(suite['execution'], 'manual')
                self.assertEqual(suite['steps'], [])
                self.assertLessEqual(suite['seconds'], 180)
                evidence = self.run_dir / name
                evidence.mkdir()
                self.assertEqual(dt.run(suite, self.device, 'fixture', evidence), 2)
                report = dt.read_json(evidence / 'report.json')
                self.assertIn('Use --manual', report['automatic']['reason'])
                self.assertEqual(dt.result_status(report), 'NOT TESTED')
        menu = dt.scenario('settings-menu')
        physical = next(check for check in menu['review'] if check['id'] == '07-physical-input')
        self.assertEqual(physical['requires'], 'human')

    def test_performance_setting_is_editable_without_inherited_override(self):
        for mode in ('settings', 'log'):
            with self.subTest(mode=mode):
                device = dict(self.device, performance=mode)
                expected = None if mode == 'settings' else 'log'
                self.fake.write_text(f'import os; assert os.environ.get("MHP3RD_PERF") == {expected!r}\n' + FAKE_GAME)
                evidence = self.run_dir / mode
                evidence.mkdir()
                with patch.dict(os.environ, {'MHP3RD_PERF': 'compact'}):
                    self.assertEqual(dt.run(self.suite, device, 'fixture', evidence), 2)
                self.assertEqual(dt.read_json(evidence / 'report.json')['automatic']['verdict'], 'PASS')
        for invalid in ('compact', '', True):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                dt.validate_device(dict(self.device, performance=invalid))

    def test_android_settings_performance_contract_keeps_menu_unlocked(self):
        package = 'org.example.yakumo.devicetest'
        output = self.root / 'settings-manifest.xml'
        contract = dt.android_manifest(package, output, 'settings')
        self.assertNotIn('MHP3RD_PERF', contract['env'])
        self.assertNotIn('SDL_ENV.MHP3RD_PERF', output.read_text())
        self.assertEqual(contract, dt.android_contract(package, 'settings'))
        self.assertEqual(dt.android_contract(package)['env']['MHP3RD_PERF'], 'log')
        for invalid in ('compact', None):
            with self.assertRaises(ValueError):
                dt.android_contract(package, invalid)
        apk = self.root / 'forced-log.apk'
        with zipfile.ZipFile(apk, 'w') as archive:
            archive.writestr('assets/device-testing.json', json.dumps(dt.android_contract(package)))
        device = {'package': package, 'serial': 'fixture', 'apk': str(apk), 'performance': 'settings'}
        adapter = dt.Adb(device, self.run_dir, time.monotonic() + 10)
        with patch.object(dt, 'command', side_effect=AssertionError('Must reject before contacting ADB')):
            with self.assertRaisesRegex(ValueError, 'contract'):
                adapter.prepare({})

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
        dt.execute_steps(adapter, [{'input': '0:pad a'}, {'wait': 'new-marker'}, {'capture': 'one'}], self.device, report, time.monotonic() + (5 if os.name == 'nt' else 2))
        self.assertEqual(report['observations'], ['new-marker'])
        with self.assertRaises(RuntimeError):
            dt.wait_condition(adapter, 'old-marker', 5 if os.name == 'nt' else 1,
                              time.monotonic() + (6 if os.name == 'nt' else 2), len('old-marker'))
        self.device['fixtures'] = {'load': [{'wait': 'new-marker'}]}
        dt.execute_steps(adapter, [{'fixture': 'load'}], self.device, report, time.monotonic() + (5 if os.name == 'nt' else 2))
        self.device['build'] = 'release'
        with self.assertRaises(dt.NotTested):
            dt.execute_steps(adapter, [{'debug': 'state'}], self.device, report, time.monotonic() + (5 if os.name == 'nt' else 2))

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
            dt.command([sys.executable, '-c', "import sys; print('failure'); sys.exit(3)"], time.monotonic() + (10 if os.name == 'nt' else 2))
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
    def test_tool_without_input_does_not_wait_for_controller_stdin(self):
        program = ('import importlib.util, pathlib, sys, time; '
                   f'spec=importlib.util.spec_from_file_location("dt", {str(ROOT / "profiles/mhp3rd/tools/device_testing.py")!r}); '
                   'dt=importlib.util.module_from_spec(spec); spec.loader.exec_module(dt); '
                   'print(dt.command([sys.executable,"-c","import sys; print(len(sys.stdin.buffer.read()))"], '
                   'time.monotonic()+3).decode().strip())')
        process = subprocess.Popen([sys.executable, '-c', program], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            process.wait(timeout=12)
            self.assertEqual(process.returncode, 0, process.stderr.read().decode(errors='replace'))
            self.assertEqual(process.stdout.read().strip(), b'0')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            process.stdout.close()
            process.stderr.close()

    @unittest.skipIf(os.name == 'nt', 'POSIX process group cleanup')
    def test_finished_group_permission_race_preserves_output_limit_failure(self):
        real_popen = subprocess.Popen
        children = []
        def launch(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            children.append(child)
            return child
        def empty_group(pid, sig):
            self.assertEqual(pid, children[0].pid)
            children[0].wait(timeout=5)
            raise PermissionError('Public empty-group race')
        with patch.object(dt.subprocess, 'Popen', side_effect=launch), patch.object(dt.os, 'killpg', side_effect=empty_group):
            with self.assertRaisesRegex(ValueError, 'output exceeds'):
                dt.command([sys.executable, '-c', 'import sys; sys.stderr.write("x"*100000)'],
                           time.monotonic()+10, output_limit=10)
        self.assertIsNotNone(children[0].returncode)

    @unittest.skipIf(os.name == 'nt', 'POSIX process group cleanup')
    def test_live_group_permission_failure_is_not_hidden(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(10)'], start_new_session=True)
        try:
            with patch.object(dt.os, 'killpg', side_effect=PermissionError('Public live-group refusal')):
                with self.assertRaises(PermissionError):
                    dt.kill_command(child, force_group=True)
            self.assertIsNone(child.poll())
        finally:
            child.kill()
            child.wait(timeout=5)

    def test_adb_setup_budget_does_not_extend_gameplay_or_streaming(self):
        device = dict(self.device, adapter='adb', setup_seconds=1, seconds=1)
        self.assertEqual(dt.validate_device(device)['setup_seconds'], 1)
        for change in ({'adapter': 'streaming'}, {'adapter': 'local'}, {'setup_seconds': True},
                       {'setup_seconds': -1}, {'setup_seconds': 901}, {'seconds': 1800}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                dt.validate_device({**device, **change})
        class SlowPrepare(dt.Local):
            def prepare(adapter, report):
                adapter.total_deadline = adapter.deadline + 1
                time.sleep(.6)
                super().prepare(report)
                adapter.prepared_at = time.monotonic()
            def launch(adapter):
                # Report writes and process scheduling consume the budget too.
                # Test the deadline assignment, not a minimum time left at launch.
                self.assertAlmostEqual(adapter.deadline, min(adapter.total_deadline, adapter.prepared_at + 1), delta=.05)
                self.assertLessEqual(adapter.deadline, adapter.total_deadline)
                super().launch()
        with patch.object(dt, 'Adb', SlowPrepare):
            self.assertEqual(dt.run(dt.scenario('performance-overlay'), device, 'fixture', self.run_dir, True),
                             2, self.report())
        self.assertEqual(self.report()['deadline_seconds'], 2)
        self.assertEqual(self.report()['automatic']['verdict'], 'PASS', self.report())
        blocked = self.run_dir / 'streaming-blocked'
        blocked.mkdir()
        with patch.object(dt, 'command') as tools:
            self.assertEqual(dt.run(self.suite, {**device, 'adapter': 'streaming'}, 'fixture', blocked), 1)
            tools.assert_not_called()
        self.assertEqual(dt.read_json(blocked / 'report.json')['automatic']['verdict'], 'FAIL')

    def test_manual_probe_timeout_is_normal_only_at_overall_deadline(self):
        class TimedProbe(dt.Local):
            def alive(adapter):
                time.sleep(.2)
                raise subprocess.TimeoutExpired(['public-probe'], .1)
        for cap, verdict in ((.1, 'PASS'), (5, 'FAIL')):
            with self.subTest(cap=cap):
                evidence = self.run_dir / str(cap)
                evidence.mkdir()
                with patch.object(dt, 'Local', TimedProbe):
                    code = dt.run(dt.scenario('performance-overlay'), dict(self.device, seconds=cap),
                                  'fixture', evidence, True)
                self.assertEqual(code, 2 if verdict == 'PASS' else 1)
                self.assertEqual(dt.read_json(evidence / 'report.json')['automatic']['verdict'], verdict)

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
        adb.write_text('''import os, pathlib, shlex, subprocess, sys, hashlib
root = pathlib.Path(os.environ['FAKE_ADB_ROOT'])
args = sys.argv[3:]
if args[0] == 'push':
    import shutil,time
    target=root / 'tmp' / pathlib.Path(args[2]).parent.name / pathlib.Path(args[2]).name
    target.parent.mkdir(parents=True,exist_ok=True)
    print('transfer started',flush=True)
    time.sleep(float(os.environ.get('FAKE_PUSH_DELAY','0')))
    shutil.copyfile(args[1],target)
    if os.environ.get('FAKE_PUSH_CORRUPT'): target.write_bytes(b'wrong APK')
    print(str(target.stat().st_size)+' bytes transferred',flush=True)
elif args[0] == 'install':
    print('Success')
elif args[:3] == ['exec-out', 'screencap', '-p']:
    sys.stdout.buffer.write(b'\\x89PNG\\r\\n\\x1a\\npublic fixture')
elif args[0] == 'shell':
    words = shlex.split(' '.join(args[1:]))
    if words[:2] == ['mkdir','-m']:
        (root / 'tmp' / pathlib.Path(words[-1]).name).mkdir(parents=True)
        raise SystemExit(0)
    if words[:2] == ['rm','-rf'] and words[-1].startswith('/data/local/tmp/yakumo-apk-'):
        import shutil
        shutil.rmtree(root / 'tmp' / pathlib.Path(words[-1]).name)
        raise SystemExit(0)
    if words[0] == 'sha256sum':
        target=root / 'tmp' / pathlib.Path(words[1]).parent.name / pathlib.Path(words[1]).name
        print(hashlib.sha256(target.read_bytes()).hexdigest()+'  '+words[1])
        raise SystemExit(0)
    if words[:2] == ['pm','install']:
        import time
        print('package installation started',flush=True)
        time.sleep(float(os.environ.get('FAKE_INSTALL_DELAY','0')))
        print('Success')
        raise SystemExit(0)
    if words[0] == 'run-as':
        payload = words[4]
        data = sys.stdin.buffer.read()
        if os.environ.get('TRUNCATE_BINARY_STDIN'):
            data = data.split(b'\\x1a', 1)[0]
        if payload.startswith('sha256sum '):
            target = root / shlex.split(payload)[1]
            print(hashlib.sha256(target.read_bytes()).hexdigest() + '  ' + str(target))
            raise SystemExit(0)
        result = subprocess.run(['sh', '-c', payload], cwd=root, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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

    def test_adb_binary_upload_and_controls_survive_windows_eof_bytes(self):
        device = self.make_adb()
        payload = bytes(range(256)) * 1700
        source = self.root / 'binary-fixture.bin'
        source.write_bytes(payload)
        adapter = dt.Adb(device, self.run_dir, time.monotonic() + 20)
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root), 'TRUNCATE_BINARY_STDIN': '1'}):
            adapter.upload(source, 'files/binary-fixture.bin')
            self.assertEqual((self.adb_root / 'files/binary-fixture.bin').read_bytes(), payload)
            adapter.app('cat > files/piped.bin', data=payload)
            self.assertEqual((self.adb_root / 'files/piped.bin').read_bytes(), payload)
            with patch.object(adapter, 'app', return_value=b'0' * 64 + b'  files/binary-fixture.bin'):
                with self.assertRaisesRegex(ValueError, 'fingerprint mismatch'):
                    adapter.upload(source, 'files/binary-fixture.bin')
        self.assertEqual(list(self.run_dir.iterdir()), [])

    def test_adb_public_transport_stages_controls_and_cleans(self):
        # First preparation starts many instrumented tool processes; keep it
        # separate from the unchanged five-second gameplay bound.
        device = {**self.make_adb(), 'setup_seconds': 20}
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            self.assertEqual(dt.run(self.suite, device, 'phone', self.run_dir), 2, self.report())
        report = self.report()
        self.assertEqual(report['automatic']['verdict'], 'PASS')
        self.assertEqual(report['deadline_seconds'], 25)
        self.assertEqual(report['captures'], ['shots/opening.png'])
        self.assertFalse((self.adb_root / 'pid').exists())
        self.assertEqual((self.adb_root / 'files/device-testing/data/ms0/save.dat').read_bytes(), b'original fixture')
        self.assertEqual((self.adb_root / 'files/device-testing/data/disc.iso').read_bytes(), b'public synthetic bytes')

    @unittest.skipIf(os.name == 'nt', 'Raw upload is explicitly unavailable on Windows')
    def test_opt_in_raw_upload_checks_binary_integrity_and_rejects_windows(self):
        device = {**self.make_adb(), 'raw_adb_upload': True}
        source = self.run_dir / 'binary-fixture.bin'; payload = bytes(range(256)) * 1700
        source.write_bytes(payload)
        adapter = dt.Adb(device, self.run_dir, time.monotonic() + 20)
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            adapter.upload(source, 'files/raw.bin')
            self.assertEqual((self.adb_root / 'files/raw.bin').read_bytes(), payload)
            with patch.dict(os.environ, {'TRUNCATE_BINARY_STDIN': '1'}):
                with self.assertRaisesRegex(ValueError, 'fingerprint mismatch'):
                    adapter.upload(source, 'files/raw.bin')
            outside = self.root / 'outside.bin'; outside.write_bytes(b'unrelated private file')
            with self.assertRaises(ValueError):
                adapter.upload(outside, 'files/raw.bin')
            link = self.run_dir / 'escaped.bin'; link.symlink_to(outside)
            with self.assertRaises(ValueError):
                adapter.upload(link, 'files/raw.bin')
        with patch.object(dt.os, 'name', 'nt'):
            with self.assertRaises(ValueError):
                adapter.upload(source, 'files/raw.bin')
            with self.assertRaises(ValueError):
                dt.validate_device(device)

    @unittest.skipIf(os.name == 'nt', 'Raw upload is explicitly unavailable on Windows')
    def test_raw_prepare_stages_only_declared_inputs_and_preserves_originals(self):
        device = {**self.make_adb(), 'raw_adb_upload': True}
        originals = {name: dt.fingerprint(Path(device['game_dir']) / name) for name in ('EBOOT.ELF', 'disc.iso')}
        adapter = dt.Adb(device, self.run_dir, time.monotonic() + 20)
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            adapter.prepare({})
        for name, digest in originals.items():
            self.assertEqual(dt.fingerprint(Path(device['game_dir']) / name), digest)
            self.assertEqual(dt.fingerprint(self.run_dir / 'upload-inputs' / name), digest)
        outside = self.root / 'unrelated.iso'; outside.write_bytes(b'unrelated private input')
        disc = Path(device['game_dir']) / 'disc.iso'; disc.unlink(); disc.symlink_to(outside)
        another = self.root / 'escaped-game-run'; another.mkdir()
        rejected = dt.Adb(device, another, time.monotonic() + 20)
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}), self.assertRaisesRegex(ValueError, 'escapes'):
            rejected.prepare({})
        self.assertFalse((another / 'upload-inputs/disc.iso').exists())

    def test_adb_running_package_is_not_disrupted(self):
        device = self.make_adb()
        (self.adb_root / 'pid').write_text('12344')
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            self.assertEqual(dt.run(self.suite, device, 'phone', self.run_dir), 2)
        self.assertEqual((self.adb_root / 'pid').read_text(), '12344')
        self.assertIn('already running', self.report()['automatic']['reason'])

    def test_deployment_split_budget_and_timeout_keep_output_cleanup_and_originals(self):
        device = self.make_adb()
        adapter = dt.Adb({**device, 'deployment_share_percent': 70, 'gameplay_reserve_seconds': 5},
                         self.run_dir, time.monotonic() + 20)
        overall = adapter.deadline
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            report = {}; adapter.prepare(report)
        phases = report['deployment']['phases']
        self.assertEqual([p['name'] for p in phases], ['apk-transfer','package-install','game-eboot','game-iso'])
        self.assertTrue(all(p['status']=='PASS' for p in phases))
        self.assertIn('bytes transferred', phases[0]['stdout'])
        self.assertEqual(report['deployment']['device_apk_sha256'], report['artifact_sha256'])
        self.assertLessEqual(report['deployment']['budget_seconds'], 14)
        self.assertEqual(adapter.deadline, overall)
        self.assertEqual(report['deployment']['apk_staging_cleanup'], 'PASS')
        self.assertEqual(list((self.adb_root/'tmp').iterdir()), [])
        for flag, phase in (('FAKE_PUSH_DELAY','apk-transfer'),('FAKE_INSTALL_DELAY','package-install')):
            root=self.root/flag; root.mkdir()
            # Leave room for interpreter/coverage startup before the delayed phase.
            # The deployment deadline must still stop it and retain its output.
            timed=dt.Adb({**device,'deployment_share_percent':50},root,time.monotonic()+10)
            deadline=timed.deadline
            evidence={}
            with patch.dict(os.environ, {'FAKE_ADB_ROOT':str(self.adb_root),flag:'10'}):
                with self.assertRaises(subprocess.TimeoutExpired): timed.prepare(evidence)
            last=evidence['deployment']['phases'][-1]
            self.assertEqual(last['name'],phase)
            self.assertEqual(last['status'],'TIMEOUT')
            self.assertIn('started',last['stdout'])
            self.assertLessEqual(evidence['deployment']['budget_seconds'],5)
            self.assertEqual(timed.deadline,deadline)
            self.assertEqual(evidence['deployment']['apk_staging_cleanup'],'PASS')
            self.assertEqual(list((self.adb_root/'tmp').iterdir()), [])
        rejected=dt.Adb({**device,'gameplay_reserve_seconds':30},self.run_dir,time.monotonic()+10)
        with self.assertRaises(dt.NotTested): rejected.prepare({})

    def test_transfer_hash_mismatch_prevents_package_install(self):
        device=self.make_adb(); adapter=dt.Adb(device,self.run_dir,time.monotonic()+20); report={}
        with patch.dict(os.environ,{'FAKE_ADB_ROOT':str(self.adb_root),'FAKE_PUSH_CORRUPT':'1'}):
            with self.assertRaisesRegex(ValueError,'fingerprint mismatch'): adapter.prepare(report)
        self.assertEqual([p['name']for p in report['deployment']['phases']],['apk-transfer'])
        self.assertEqual(report['deployment']['apk_staging_cleanup'],'PASS')
        self.assertEqual(list((self.adb_root/'tmp').iterdir()),[])

    def test_official_installer_uses_selected_device_no_metrics_and_verified_copy(self):
        device = self.make_adb()
        installer = self.root / 'installer.py'
        marker = self.root / 'install-call.json'
        installer.write_text("""import json,sys,zipfile
from pathlib import Path
assert '--no-metrics' in sys.argv and 'install' in sys.argv
assert '--device=synthetic-usb' in sys.argv and '--install-options=-r' in sys.argv
apk=next(x.split('=',1)[1] for x in sys.argv if x.startswith('--apks='))
with zipfile.ZipFile(apk) as z: assert 'assets/device-testing.json' in z.namelist()
Path(""" + repr(str(marker)) + ").write_text(json.dumps(sys.argv))\n")
        adapter = dt.Adb({**device, 'android_cli_install': True, 'android': [sys.executable, str(installer)]},
                         self.run_dir, time.monotonic() + 20)
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            report = {}
            adapter.prepare(report)
        self.assertTrue(marker.exists())
        self.assertEqual(report['artifact_sha256'], dt.fingerprint(self.run_dir / 'artifact.apk'))
        self.assertFalse((self.adb_root / 'pid').exists())

    def test_adb_replacement_pid_and_capture_failure(self):
        device = self.make_adb()
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}):
            adapter = dt.Adb(device, self.run_dir, time.monotonic() + 5)
            adapter.owned = True
            adapter.pid = '12345'
            (self.adb_root / 'pid').write_text('12347')
            with self.assertRaises(RuntimeError):
                adapter.close()
            self.assertEqual((self.adb_root / 'pid').read_text(), '12347')
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
            return subprocess.CompletedProcess(args, 1, b'No reservations found.\n', b'') if kwargs.get('return_status') else b''
        with patch.object(dt, 'command', cleanup):
            self.assertEqual(dt.run(self.suite, device, 'cloud', self.run_dir), 1)
        self.assertTrue(any('remove' in item and 'abcdefgh1234' in item for item in calls))
        self.assertTrue(all('--no-metrics' in item for item in calls))

    def test_cloud_failure_without_id_never_deletes_unowned_devices(self):
        device = {**self.device, 'adapter': 'streaming', 'seconds': 5, 'model': 'synthetic/33',
                  'project': 'public-fixture', 'android': [sys.executable, str(self.root / 'cloud.py')]}
        (self.root / 'cloud.py').write_text("print('No available device'); raise SystemExit(3)")
        with patch.object(dt, 'command', return_value=subprocess.CompletedProcess([], 1, b'No reservations found.\n', b'')) as calls:
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
            return subprocess.CompletedProcess(args, 1, b'No reservations found.\n', b'') if kwargs.get('return_status') else b''
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
        self.assertTrue(dt.bounded_match('ready', 'ready', time.monotonic() + (5 if os.name == 'nt' else 2)))
        self.assertFalse(dt.bounded_match('ready', 'not yet', time.monotonic() + (5 if os.name == 'nt' else 2)))
        device = {**self.device, 'adapter': 'ssh', 'host': 'synthetic-host',
                  'checkout': str(ROOT), 'remote': self.device}
        for error in (subprocess.TimeoutExpired('ssh', .1), KeyboardInterrupt()):
            with patch.object(dt, 'command', side_effect=error):
                self.assertEqual(dt.ssh_run(self.suite, device, 'deck', self.run_dir, False), 1)
            self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')
            self.assertIn('SSH transport/evidence failed', (self.run_dir / 'transport.log').read_text())
        for name in ('org.example.devicetest', 'a.b.devicetest'):
            self.assertTrue(dt.valid_package(name))
        for name in ('a.devicetest', 'a..devicetest', 'a.b-devicetest', 'a.' + 'a' * 200 + '.devicetest'):
            self.assertFalse(dt.valid_package(name))

    def test_ssh_failure_and_powershell_quoting(self):
        device = {**self.device, 'adapter': 'ssh', 'host': 'synthetic-host',
                  'checkout': 'C:/Example Space/Yakumo', 'python': ['python'],
                  'shell': 'powershell', 'remote': self.device}
        with patch.object(dt, 'command', return_value=subprocess.CompletedProcess([], 255, b'', b'connection failed')) as calls:
            self.assertEqual(dt.ssh_run(self.suite, device, 'windows', self.run_dir, False), 1)
        self.assertIn('EncodedCommand', calls.call_args.args[0][-1])
        script = dt.base64.b64decode(calls.call_args.args[0][-1].split()[-1]).decode('utf-16le')
        self.assertIn("'C:/Example Space/Yakumo/profiles/mhp3rd/tools/device_testing.py'", script)
        self.assertTrue(script.endswith('; exit $LASTEXITCODE'))
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

    def test_config_is_snapshot_and_remote_validation_matches_local_rules(self):
        config = self.root / 'devices.json'
        dt.write_json(config, {'devices': {'mac': self.device}})
        loaded = dt.load_device(config, 'mac')
        dt.write_json(config, {'devices': {'mac': {**self.device, 'env': {'MHP3RD_DATA_DIR': '/other'}}}})
        self.assertEqual(loaded, self.device)
        remote = {**self.device, 'adapter': 'ssh', 'remote': copy.deepcopy(self.device)}
        self.assertEqual(dt.validate_device(remote), remote)
        for change in ({'env': {'MHP3RD_DATA_DIR': '/other'}}, {'python': 'shell words'},
                       {'args': ['nul\0']}, {'artifact_sha256': 'bad'},
                       {'fixtures': {'one': [{'fixture': 'two'}]}}, {'build': 'release'}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                dt.validate_device({**remote, 'remote': {**self.device, **change}})

    def test_atomic_json_does_not_follow_planted_temporary_link(self):
        victim = self.root / 'victim'
        victim.write_text('keep')
        planted = self.run_dir / 'report.tmp'
        try:
            planted.symlink_to(victim)
        except OSError:
            self.skipTest('Symlink privilege unavailable')
        dt.write_json(self.run_dir / 'report.json', {'safe': True})
        self.assertEqual(victim.read_text(), 'keep')
        self.assertEqual(list(self.run_dir.glob('.report.json-*')), [])
        with patch.object(dt.Path, 'replace', side_effect=OSError('synthetic failure')), self.assertRaises(OSError):
            dt.write_json(self.run_dir / 'report.json', {})
        self.assertEqual(list(self.run_dir.glob('.report.json-*')), [])

    def test_copied_fixture_hashes_snapshot_and_rejects_late_links(self):
        original = dt.shutil.copytree
        def changed_source(source, destination, **kwargs):
            result = original(source, destination, **kwargs)
            (self.saves / 'save.dat').write_bytes(b'changed original')
            return result
        with patch.object(dt.shutil, 'copytree', changed_source):
            digest = dt.copy_fixture(self.saves, self.root / 'snapshot')
        self.assertEqual(digest, dt.object_hash([('save.dat', dt.fingerprint(self.root / 'snapshot/save.dat'))]))
        self.assertEqual((self.root / 'snapshot/save.dat').read_bytes(), b'original fixture')
        def late_link(source, destination, **kwargs):
            result = original(source, destination, **kwargs)
            try:
                (Path(destination) / 'late').symlink_to(self.saves / 'save.dat')
            except OSError:
                self.skipTest('Symlink privilege unavailable')
            return result
        with patch.object(dt.shutil, 'copytree', late_link), self.assertRaises(ValueError):
            dt.copy_fixture(self.saves, self.root / 'late-copy')
        with self.assertRaises(dt.NotTested):
            dt.copy_fixture(self.root / 'missing', self.root / 'missing-copy')

    def test_regular_copy_rejects_identity_race_and_mutation(self):
        source = self.saves / 'save.dat'
        original = dt.os.open
        def swap(path, flags):
            source.rename(source.with_suffix('.old'))
            source.write_bytes(b'replacement')
            return original(path, flags)
        with patch.object(dt.os, 'open', swap), self.assertRaises(ValueError):
            dt.copy_regular_file(source, self.root / 'copy')
        original_copy = dt.shutil.copyfileobj
        def mutate(input_file, output):
            original_copy(input_file, output)
            source.write_bytes(b'changed while copying')
        with patch.object(dt.shutil, 'copyfileobj', mutate), self.assertRaises(ValueError):
            dt.copy_regular_file(source, self.root / 'mutated-copy')

    def test_artifact_identity_is_checked_before_and_after_play(self):
        # Use the fake script as executable only for preparation, not launch.
        device = {**self.device, 'binary': str(self.fake)}
        adapter = dt.Local(device, self.run_dir, time.monotonic() + 2)
        adapter.prepare(dt.new_report(self.suite, device, 'mac'))
        self.fake.write_text('changed')
        with self.assertRaises(ValueError):
            adapter.launch()
        adapter.owned = True
        with self.assertRaises(ValueError):
            adapter.close()
        with self.assertRaises(ValueError):
            dt.verify_artifact({'artifact_sha256': '0' * 64}, '1' * 64)

    def test_command_bounds_outputs_stdin_exit_status_and_deadline(self):
        for stream in ('stdout', 'stderr'):
            with self.subTest(stream=stream), self.assertRaises(ValueError):
                dt.command([sys.executable, '-c', f'import sys; sys.{stream}.write("x"*100000)'],
                           time.monotonic() + 5, output_limit=10)
        result = dt.command([sys.executable, '-c', 'import sys; print(sys.stdin.read()); sys.exit(3)'],
                            time.monotonic() + 5, data=b'input', check=False, return_status=True)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(result.stdout.strip(), b'input')
        with self.assertRaises(RuntimeError):
            dt.command([sys.executable, '-c', 'raise SystemExit(4)'], time.monotonic() + 5)
        with self.assertRaises(TimeoutError):
            dt.command([sys.executable], time.monotonic() - 1)

    def test_command_kills_descendant_holding_pipe_after_parent_exit(self):
        marker = self.root / 'descendant-survived'
        child = 'import time,pathlib; time.sleep(1.5); pathlib.Path(' + repr(str(marker)) + ').touch()'
        parent = 'import subprocess,sys; subprocess.Popen([sys.executable,"-c",' + repr(child) + '])'
        started = time.monotonic()
        if os.name == 'nt':
            # The job supervisor can clean descendants as soon as the leader exits.
            try:
                result = dt.command([sys.executable, '-c', parent], time.monotonic() + .7, return_status=True)
                self.assertEqual(result.returncode, 0)
            except subprocess.TimeoutExpired:
                pass  # Slow interpreter startup may instead reach the same cap.
        else:
            with self.assertRaises(subprocess.TimeoutExpired):
                dt.command([sys.executable, '-c', parent], time.monotonic() + .7)
        self.assertLess(time.monotonic() - started, 4)
        time.sleep(1)
        self.assertFalse(marker.exists())

    def test_command_partial_thread_setup_and_interrupt_collect_owned_process(self):
        original = dt.subprocess.Popen
        children = []
        def spawn(*args, **kwargs):
            child = original(*args, **kwargs)
            children.append(child)
            return child
        for error in (RuntimeError('cannot start reader'), KeyboardInterrupt()):
            with patch.object(dt.subprocess, 'Popen', spawn), patch.object(dt.threading.Thread, 'start', side_effect=error):
                with self.assertRaises(type(error)):
                    dt.command([sys.executable, '-c', 'import time; time.sleep(10)'], time.monotonic() + 5, data=b'input')
            self.assertIsNotNone(children[-1].poll())
            self.assertTrue(children[-1].stdout.closed and children[-1].stderr.closed)
            self.assertTrue(children[-1].stdin.closed)

    @unittest.skipIf(os.name == 'nt', 'POSIX controller signal regression')
    def test_regex_worker_has_own_lifetime_after_controller_killed(self):
        pidfile = self.root / 'regex-supervisor-pid'
        controller_script = f'''
import importlib.util,pathlib,time
spec = importlib.util.spec_from_file_location('runner', {dt.__file__!r})
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
original = runner.subprocess.Popen
def spawn(*args, **kwargs):
    child = original(*args, **kwargs)
    pathlib.Path({str(pidfile)!r}).write_text(str(child.pid))
    return child
runner.subprocess.Popen = spawn
runner.bounded_match('(a+)+$', 'a'*1000+'!', time.monotonic()+2)
'''
        controller = subprocess.Popen([sys.executable, '-c', controller_script], start_new_session=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 4
            while not pidfile.exists():
                dt.remaining(deadline)
                time.sleep(.02)
            supervisor_pid = int(pidfile.read_text())
            controller.kill()
            controller.communicate(timeout=4)
            # The orphaned supervisor kills the regex by its own cap.
            while True:
                state = subprocess.run(['ps', '-o', 'stat=', '-p', str(supervisor_pid)], capture_output=True).stdout.strip()
                if not state or state.startswith(b'Z'):
                    break
                dt.remaining(deadline)
                time.sleep(.05)
            self.assertTrue(not state or state.startswith(b'Z'))
        finally:
            try:
                os.killpg(controller.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            controller.communicate(timeout=4)

    def test_activity_and_pid_results_cannot_be_shell_fragments(self):
        device = self.make_adb()
        for activity in ('OtherActivity', dt.ACTIVITY + ';echo bad'):
            with self.assertRaises(ValueError):
                dt.Adb({**device, 'activity': activity}, self.run_dir, time.monotonic() + 5)
        self.assertEqual(dt.parse_pids(b'234 123\n'), '123 234')
        self.assertEqual(dt.parse_pids(b''), '')
        for value in (b'1;touch bad', b'"123"', b'0', b'-1', b'2147483648', b'1 1', b'01 1', b'\xff'):
            with self.subTest(value=value), self.assertRaises((ValueError, UnicodeError)):
                dt.parse_pids(value)
        for value in ('Device connected on port 0', 'localhost:65536', 'localhost:123456'):
            with self.assertRaises(ValueError):
                dt.parse_adb_serial(value)

    def test_adb_partial_activation_uses_cleanup_budget_and_manual_is_failure(self):
        device = {**self.make_adb(), 'seconds': .15}
        package = device['package']
        class Partial(dt.Adb):
            def prepare(self, report):
                report['artifact_sha256'] = 'a' * 64
            def shell(inner, text, **kwargs):
                dt.remaining(inner.deadline)
                if text.startswith('am start'):
                    (self.adb_root / 'pid').write_text('12345')
                    time.sleep(.18)
                    raise subprocess.TimeoutExpired('am start', .15)
                if text.startswith('pidof'):
                    return (self.adb_root / 'pid').read_bytes() if (self.adb_root / 'pid').exists() else b''
                if text.startswith('am force-stop ' + package):
                    (self.adb_root / 'pid').unlink()
                return b''
            def text(inner):
                dt.remaining(inner.deadline)
                return 'partial'
        with patch.object(dt, 'Adb', Partial):
            self.assertEqual(dt.run(dt.scenario('quest-translation'), {**device, 'translation': 'fixture'},
                                    'phone', self.run_dir, True), 1)
        self.assertFalse((self.adb_root / 'pid').exists())
        self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')

    def test_adb_manual_deadline_collects_logs_with_cleanup_budget(self):
        device = {**self.device, 'adapter': 'adb', 'seconds': .15}
        class Manual(dt.Adb):
            def __init__(inner, device, root, deadline):
                inner.deadline, inner.owned = deadline, False
            def prepare(inner, report):
                report['artifact_sha256'] = 'a' * 64
            def launch(inner):
                inner.owned = True
            def alive(inner):
                return True
            def text(inner):
                dt.remaining(inner.deadline)
                return 'completed session'
            def close(inner):
                dt.remaining(inner.deadline)
                inner.owned = False
        with patch.object(dt, 'Adb', Manual):
            self.assertEqual(dt.run(dt.scenario('quest-translation'), {**device, 'translation': 'fixture'},
                                    'phone', self.run_dir, True), 2)
        self.assertEqual(self.report()['automatic']['verdict'], 'PASS')

    def test_cloud_preflight_fails_closed_except_exact_idle_response(self):
        device = {'adapter': 'streaming', 'build': 'dev', 'commit': 'a' * 40,
                  'seconds': 500, 'streaming_cap_seconds': 500}
        self.assertEqual(dt.validate_device(device), device)
        for change in ({'seconds': 501}, {'streaming_cap_seconds': 801}, {'streaming_cap_seconds': True},
                       {'streaming_cap_seconds': 0}, {'streaming_cap_seconds': 300}):
            with self.assertRaises(ValueError):
                dt.validate_device({**device, **change})
        self.assertEqual(dt.streaming_limit({}), 300)
        for result in (subprocess.CompletedProcess([], 1, b'No reservations found.\n', b''),
                       subprocess.CompletedProcess([], 0, b'No reservations found.\n', b'')):
            with patch.object(dt, 'command', return_value=result):
                dt.require_idle_project(['android'], 'public-fixture', time.monotonic() + 2)
        for code, output, error in ((1, b'', b''), (1, b'auth failed', b''),
                                   (0, b'', b''), (0, b'unknown', b''),
                                   (1, b'No reservations found.', b'network error')):
            with patch.object(dt, 'command', return_value=subprocess.CompletedProcess([], code, output, error)) as calls:
                with self.assertRaises((RuntimeError, dt.NotTested)):
                    dt.require_idle_project(['android'], 'public-fixture', time.monotonic() + 2)
                self.assertEqual(calls.call_count, 1)

    def remote_bundle(self, change=None, extra=()):
        device = {**self.device, 'adapter': 'ssh', 'host': 'synthetic', 'checkout': str(ROOT), 'remote': self.device}
        report = dt.new_report(self.suite, self.device, 'deck')
        report.update(request_id='nonce', artifact_sha256='a' * 64,
                      observations=dt.expected_actions(self.suite, self.device, 'wait'), captures=['shots/opening.bmp'])
        report['automatic'] = {'verdict': 'PASS', 'reason': 'synthetic run'}
        if change:
            change(report)
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as archive:
            archive.writestr('report.json', json.dumps(report))
            archive.writestr('shots/opening.bmp', b'BM synthetic')
            for name, contents in extra:
                archive.writestr(name, contents)
        return device, data.getvalue()

    def test_remote_report_identity_verdict_and_artifact_are_verified(self):
        device, data = self.remote_bundle()
        report = dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        self.assertEqual(report['automatic']['verdict'], 'PASS')
        self.assertTrue((self.run_dir / 'shots/opening.bmp').is_file())
        changes = [lambda r: r.update(commit='b' * 40), lambda r: r.update(device='other'),
                   lambda r: r.update(request_id='old'), lambda r: r.update(scenario_sha256='bad'),
                   lambda r: r.update(artifact_sha256='bad'), lambda r: r.update(observations=[]),
                   lambda r: r.update(captures=[]), lambda r: r.update(review={}),
                   lambda r: r['automatic'].update(verdict='MAYBE'),
                   lambda r: r['review']['opening'].update(verdict='PASS'),
                   lambda r: r['review']['opening'].update(requires='human')]
        for change in changes:
            device, data = self.remote_bundle(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        device, data = self.remote_bundle()
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 0)
        device['artifact_sha256'] = 'b' * 64
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)

    def test_remote_archive_rejects_traversal_duplicates_types_and_limits(self):
        for entry in ('../escape', 'data/save.dat', 'review.md', 'shots/../escape.png', 'shots/a\\b.png'):
            device, data = self.remote_bundle(extra=[(entry, b'bad')])
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        bad = zipfile.ZipInfo('shots/link.png')
        bad.create_system, bad.external_attr = 3, (stat.S_IFLNK | 0o777) << 16
        device, data = self.remote_bundle(extra=[(bad, b'outside')])
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        device, data = self.remote_bundle(extra=[('shots/opening.bmp', b'duplicate')])
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        device, data = self.remote_bundle(extra=[('shots/' + str(i) + '.png', b'x') for i in range(128)])
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        with patch.object(dt, 'EVIDENCE_LIMIT', 10), self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)

    def test_export_and_import_reject_capture_symlinks(self):
        device, data = self.remote_bundle()
        shots = self.run_dir / 'shots'
        shots.mkdir()
        try:
            (shots / 'opening.bmp').symlink_to(self.saves / 'save.dat')
        except OSError:
            self.skipTest('Symlink privilege unavailable')
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 2)
        dt.write_json(self.run_dir / 'report.json', dt.new_report(self.suite, self.device, 'deck'))
        with self.assertRaises(ValueError):
            dt.export_evidence(self.run_dir)
        self.assertEqual((self.saves / 'save.dat').read_bytes(), b'original fixture')

    def test_malformed_ssh_transport_always_leaves_a_fail_report(self):
        device, data = self.remote_bundle()
        outputs = [b'no marker', b'YAKUMO_EVIDENCE:AAAA\n',
                   b'YAKUMO_EVIDENCE:' + dt.base64.b64encode(data) + b'\n']
        for output in outputs:
            result = subprocess.CompletedProcess([], 2, output, b'')
            with patch.object(dt, 'command', return_value=result):
                self.assertEqual(dt.ssh_run(self.suite, device, 'deck', self.run_dir, False), 1)
            self.assertEqual(self.report()['automatic']['verdict'], 'FAIL')
            self.assertTrue((self.run_dir / 'review.md').exists())

    def test_empty_review_cannot_make_unreviewed_scenario_green(self):
        report = dt.new_report(self.suite, self.device, 'mac')
        report['automatic']['verdict'] = 'PASS'
        report['review'] = {}
        self.assertEqual(dt.result_status(report), 'NOT TESTED')

    def test_remote_review_needs_actor_verdict_notes_and_owned_evidence(self):
        def reviewed(report):
            report['review']['opening'].update(verdict='PASS', assessments={
                'agent': {'verdict': 'PASS', 'notes': 'Synthetic image inspected', 'evidence': ['shots/opening.bmp']}})
        device, data = self.remote_bundle(reviewed)
        report = dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 0)
        self.assertEqual(dt.result_status(report), 'PASS')
        for field, value in (('verdict', 'UNKNOWN'), ('notes', ''), ('evidence', ['run.log']),
                             ('evidence', ['../private']), ('evidence', 'shots/opening.bmp')):
            def invalid(report):
                reviewed(report)
                report['review']['opening']['assessments']['agent'][field] = value
            device, data = self.remote_bundle(invalid)
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 0)
        def invalid_actor(report):
            reviewed(report)
            report['review']['opening']['assessments']['unknown'] = report['review']['opening']['assessments'].pop('agent')
        device, data = self.remote_bundle(invalid_actor)
        with self.assertRaises(ValueError):
            dt.import_evidence(data, self.suite, device, 'deck', self.run_dir, 'nonce', 0)

    def test_archive_metadata_limits_and_export_races_fail_closed(self):
        info = zipfile.ZipInfo('report.json')
        archive = unittest.mock.Mock()
        archive.infolist.return_value = [info]
        info.file_size = 1024 * 1024 + 1
        with self.assertRaises(ValueError):
            dt.evidence_members(archive)
        info.file_size, info.flag_bits = 1, 1
        with self.assertRaises(ValueError):
            dt.evidence_members(archive)
        archive.infolist.return_value = []
        with self.assertRaises(ValueError):
            dt.evidence_members(archive)
        report = self.run_dir / 'report.json'
        dt.write_json(report, dt.new_report(self.suite, self.device, 'deck'))
        original_open = dt.os.open
        def swap(path, flags):
            Path(path).rename(Path(path).with_suffix('.old'))
            Path(path).write_text('replacement')
            return original_open(path, flags)
        with patch.object(dt.os, 'open', swap), self.assertRaises(ValueError):
            dt.export_evidence(self.run_dir)
        with patch.object(dt, 'EVIDENCE_LIMIT', 1), self.assertRaises(ValueError):
            dt.export_evidence(self.run_dir)

    def test_remote_pass_cannot_drop_human_requirement(self):
        suite = dt.scenario('release-player')
        device = {**self.device, 'build': 'release'}
        ssh = {**device, 'adapter': 'ssh', 'remote': device}
        report = dt.new_report(suite, device, 'deck')
        report.update(request_id='nonce', artifact_sha256='a' * 64)
        report['automatic'] = {'verdict': 'PASS', 'reason': 'manual bounded session'}
        dt.validate_remote_report(report, suite, ssh, 'deck', 'nonce', {'report.json'})
        first = next(iter(report['review'].values()))
        first.update(verdict='PASS', assessments={'agent': {'verdict': 'PASS', 'notes': 'Only agent', 'evidence': []}})
        with self.assertRaises(ValueError):
            dt.validate_remote_report(report, suite, ssh, 'deck', 'nonce', {'report.json'})

    def test_run_uses_config_snapshot_even_when_original_changes(self):
        original = dt.Local.prepare
        def prepare(adapter, report):
            self.device['env'] = {'MHP3RD_DATA_DIR': 'changed'}
            self.suite['steps'] = []
            return original(adapter, report)
        with patch.object(dt.Local, 'prepare', prepare):
            self.assertEqual(dt.run(self.suite, self.device, 'mac', self.run_dir), 2, self.report())
        self.assertEqual(self.report()['captures'], ['shots/opening.bmp'])
        self.assertEqual(self.report()['settings'], {})

    def test_watchdog_setup_failure_stops_only_owned_android_activation(self):
        device = self.make_adb()
        class BrokenWatchdog(dt.Adb):
            def shell(inner, text, **kwargs):
                if text.startswith('nohup'):
                    raise RuntimeError('synthetic watchdog setup failed')
                return super().shell(text, **kwargs)
        with patch.dict(os.environ, {'FAKE_ADB_ROOT': str(self.adb_root)}), patch.object(dt, 'Adb', BrokenWatchdog):
            self.assertEqual(dt.run(self.suite, device, 'phone', self.run_dir), 1)
        self.assertFalse((self.adb_root / 'pid').exists())
        self.assertIn('watchdog setup failed', self.report()['automatic']['reason'])


if __name__ == '__main__':
    unittest.main()
