"""Suite aggregation, bounded state waits and negative outcome tests."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'profiles/mhp3rd/tools'))
import device_suite as suite
import device_testing as runner
import device_checks as checks


class Adapter:
    def __init__(self, sample=None):
        self.sample = sample or {'schema': 1, 'active_id': 204, 'loaded': True}
        self.output = '[debug] reading commands from fixture\n'
        self.deadline = time.monotonic() + 5
        self.owned = True
        self.running = True
        self.sent = []
        self.closed = False

    def text(self):
        return self.output

    def alive(self):
        return self.running

    def send(self, kind, value):
        self.sent.append((kind, value))
        if value.startswith('observe '):
            self.output += '[debug] observation ' + json.dumps({**self.sample, 'request': int(value.split()[1])}) + '\n'

    def launch(self):
        self.running = True

    def close(self):
        self.running = False
        self.closed = True


class SuiteTests(unittest.TestCase):
    def test_ten_definitions_are_asserting_and_gaps_visible(self):
        definitions = suite.definitions()
        self.assertEqual([x['id'] for x in definitions], [f'{n:02}' for n in range(1, 11)])
        with mock.patch.object(suite.runner, 'read_json', side_effect=lambda path: {'id': 'wrong'}):
            with self.assertRaises(ValueError):
                suite.definitions()
        invalid = copy.deepcopy(definitions)
        invalid[0]['checks'][0]['steps'] = [{'action': 'input', 'value': '0:key W'}]
        with mock.patch.object(suite.runner, 'read_json', side_effect=invalid):
            with self.assertRaises(ValueError):
                suite.definitions()
        for steps in ([], [{'action': 'no-op'}], [{'action': 'hold', 'seconds': 11}], [{'action': 'snapshot', 'timeout': 0}]):
            with self.assertRaises(ValueError):
                suite.validate_program(steps)
        with self.assertRaises(ValueError):
            suite.validate_program([{'action': 'fixture', 'name': 'recursive'}], fixture=True)

    def test_aggregate_refuses_missing_or_failed_checks(self):
        self.assertEqual(suite.verdict([]), 'NOT TESTED')
        self.assertEqual(suite.verdict([{'verdict': 'PASS'}, {'verdict': 'NOT TESTED'}]), 'NOT TESTED')
        self.assertEqual(suite.verdict([{'verdict': 'PASS'}, {'verdict': 'FAIL'}]), 'FAIL')
        for verdict, code in (('PASS', 0), ('FAIL', 1), ('NOT TESTED', 2)):
            report = {'devices': [{'verdict': verdict}]}
            self.assertEqual(suite.exit_status(report), code)
        with tempfile.TemporaryDirectory() as temp:
            report = {'devices': [{'name': 'mac', 'verdict': 'NOT TESTED', 'scenarios': [{'id': '03', 'checks': [
                {'id': 'combat', 'verdict': 'NOT TESTED', 'reason': 'Missing fixture | never passed\nnext'}]}]}]}
            suite.render(Path(temp), report)
            self.assertIn('NOT TESTED', (Path(temp) / 'suite.md').read_text())
            self.assertEqual(json.loads((Path(temp) / 'report.json').read_text()), report)

    def session(self, root, adapter):
        session = object.__new__(suite.Session)
        session.device, session.root, session.deadline = {'observations': True, 'build': 'dev'}, root, time.monotonic() + 3
        session.adapter, session.request, session.phase = adapter, 0, 1
        session.data_root = root / 'data'
        session.samples, session.logs, session.started, session.observation_ready = [], [], True, False
        return session

    def test_state_readbacks_request_identity_and_timeouts(self):
        with tempfile.TemporaryDirectory() as temp:
            adapter = Adapter()
            session = self.session(Path(temp), adapter)
            self.assertEqual(session.snapshot(until={'active_id': 204})['active_id'], 204)
            self.assertEqual(session.snapshot()['request'], 2)
            self.assertEqual(len(session.samples), 2)
            adapter.sample['active_id'] = 309
            with self.assertRaises(TimeoutError):
                session.snapshot(timeout=.02, until={'active_id': 204})
            session.device['observations'] = False
            with self.assertRaises(runner.NotTested):
                session.snapshot()
            session.close()
            self.assertTrue(adapter.closed)
            self.assertIn('observation', (Path(temp) / 'run.log').read_text())
            session.device['observations'] = True
            adapter.running = False
            adapter.send = lambda *args: None
            with self.assertRaises(checks.CheckFailed):
                session.snapshot()
            adapter.running = True
            adapter.send = lambda *args: setattr(adapter, 'output', adapter.output + 'Fatal signal\n')
            with self.assertRaises(checks.CheckFailed):
                session.snapshot()

    def test_program_actions_and_mutations(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / 'data').mkdir()
            (root / 'data/settings.ini').write_text('video.scale=1\n')
            (root / 'data/save').write_text('independent fixture')
            session = self.session(root, Adapter())
            context = {}
            program = [{'action': 'snapshot', 'as': 'quest'},
                       {'action': 'assert', 'actual': 'quest.active_id', 'op': 'eq', 'expected': 204},
                       {'action': 'input', 'value': '0:axis leftx 0'},
                       {'action': 'settings', 'as': 'settings'},
                       {'action': 'assert', 'actual': 'settings.video.scale', 'op': 'eq', 'expected': '1'},
                       {'action': 'tree', 'path': 'data', 'as': 'before'},
                       {'action': 'tree', 'path': 'data', 'as': 'after'},
                       {'action': 'assert', 'actual': 'after', 'op': 'eq', 'expected': {'from': 'before'}},
                       {'action': 'parameter', 'name': 'expected-id', 'as': 'expected'},
                       {'action': 'assert', 'actual': 'expected', 'op': 'eq', 'expected': {'parameter': 'expected-id'}},
                       {'action': 'hold', 'fields': ['active_id'], 'seconds': .1}]
            self.assertEqual(suite.execute(session, program, context, {'expected-id': 204}, {}, session.deadline), 5)
            session.adapter.sample['active_id'] = 309
            with self.assertRaises(checks.CheckFailed):
                suite.execute(session, program, {}, {'expected-id': 204}, {}, session.deadline)
            with self.assertRaises(runner.NotTested):
                suite.execute(session, [{'action': 'fixture', 'name': 'missing'}], {}, {}, {}, session.deadline)
            fixture = [{'action': 'snapshot', 'as': 'q'}, {'action': 'assert', 'actual': 'q.active_id', 'op': 'eq', 'expected': 309}]
            self.assertEqual(suite.execute(session, [{'action': 'fixture', 'name': 'route'}], {}, {}, {'suite_fixtures': {'route': fixture}}, session.deadline), 1)
            for expected in ({'parameter': 'missing'}, {'invalid': 1}):
                with self.assertRaises((runner.NotTested, ValueError)):
                    suite.resolve(expected, {}, {})
            with self.assertRaises(checks.CheckFailed):
                suite.execute(session, [{'action': 'assert', 'actual': 'a', 'subtract': 'b', 'op': 'eq', 'expected': 1}], {'a': None, 'b': 1}, {}, {}, session.deadline)
            session.device['build'] = 'release'
            with self.assertRaises(runner.NotTested):
                suite.execute(session, [{'action': 'debug', 'value': 'quest start 204'}], {}, {}, {'build': 'release'}, session.deadline)
            with self.assertRaises(ValueError):
                suite.execute(session, [{'action': 'debug', 'value': 'quest start 204'}], {}, {'multiplayer': True}, {'build': 'dev'}, session.deadline)

    def test_report_incomplete_failure_and_cleanup(self):
        definition = [{'id': '03', 'title': 'Quest', 'checks': [{'id': 'identity', 'steps': [
            {'action': 'snapshot', 'as': 'q'}, {'action': 'assert', 'actual': 'q.active_id', 'op': 'eq', 'expected': 204}]}]}]
        device = {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40}
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            session = self.session(root, Adapter())
            session.identity = {'artifact_sha256': 'b' * 64}
            session.prepare = lambda launch=True: None
            with mock.patch.object(suite, 'Session', return_value=session):
                report = suite.run_device(device, 'mac', definition, root, 2)
            self.assertEqual(report['verdict'], 'PASS')
            self.assertEqual(report['cleanup'], 'PASS')
            session = self.session(root, Adapter({'schema': 1, 'active_id': 309}))
            session.identity = {}
            session.prepare = lambda launch=True: None
            with mock.patch.object(suite, 'Session', return_value=session):
                report = suite.run_device(device, 'mac', definition, root, 2)
            self.assertEqual(report['verdict'], 'FAIL')
            missing = copy.deepcopy(definition)
            missing[0]['checks'][0]['steps'].insert(0, {'action': 'fixture', 'name': 'missing'})
            session = self.session(root, Adapter())
            session.identity = {}
            session.prepare = lambda launch=True: None
            with mock.patch.object(suite, 'Session', return_value=session):
                report = suite.run_device(device, 'mac', missing, root, 2)
            self.assertEqual(report['verdict'], 'NOT TESTED')
            report = suite.run_device({**device, 'adapter': 'ssh'}, 'deck', definition, root, 2)
            self.assertEqual(report['verdict'], 'NOT TESTED')
            with mock.patch.object(suite, 'Session', side_effect=RuntimeError('setup failed')):
                report = suite.run_device(device, 'mac', definition, root, 2)
            self.assertEqual(report['verdict'], 'FAIL')

class RealSessionTests(unittest.TestCase):
    def test_real_local_copy_restart_files_and_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            source, root = base / 'fixture', base / 'session'
            source.mkdir(); root.mkdir()
            (source / 'ms0').mkdir()
            (source / 'ms0/save').write_bytes(b'public copied-save fixture')
            (source / 'EBOOT.ELF').write_bytes(b'public synthetic executable input')
            (source / 'disc.iso').write_bytes(b'public synthetic disc input')
            (source / 'settings.ini').write_text('video.scale=1\n')
            game = base / 'fake_game.py'
            game.write_text('''import json,os,time
from pathlib import Path
path=Path(os.environ['MHP3RD_DEBUG_COMMANDS'])
print('[debug] reading commands from fixture',flush=True)
offset=0
end=time.monotonic()+8
while time.monotonic()<end:
 lines=path.read_text().splitlines()
 for line in lines[offset:]:
  if line.startswith('observe '):
   request=int(line.split()[1])
   print('[debug] observation '+json.dumps({'schema':1,'request':request,'active_id':204,'loaded':True}),flush=True)
 offset=len(lines)
 time.sleep(.01)
''')
            device = {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40, 'observations': True,
                      'binary': sys.executable, 'args': [str(game)], 'saves': str(source / 'ms0'),
                      'settings': str(source / 'settings.ini'), 'game_dir': str(source)}
            session = suite.Session(device, root, time.monotonic() + 8)
            try:
                session.prepare()
                self.assertEqual(session.snapshot()['active_id'], 204)
                self.assertEqual(checks.settings(session.file('data/settings.ini'))['video.scale'], '1')
                (root / 'data/ms0/save').write_bytes(b'changed owned save')
                (root / 'data/settings.ini').write_text('video.scale=2\n')
                session.restart(reset=True)
                self.assertEqual((root / 'data/ms0/save').read_bytes(), b'public copied-save fixture')
                self.assertEqual(session.snapshot()['request'], 2)
                session.restart(reset=False)
                self.assertEqual(session.snapshot()['request'], 3)
            finally:
                session.close()
            self.assertFalse(session.adapter.alive())
            self.assertEqual((source / 'ms0/save').read_bytes(), b'public copied-save fixture')
            self.assertGreaterEqual((root / 'run.log').read_text().count('reading commands'), 3)

    def test_main_list_and_selected_aggregate(self):
        self.assertEqual(suite.main(['--list']), 0)
        with self.assertRaises(SystemExit):
            suite.main([])
        with tempfile.TemporaryDirectory() as temp:
            device = {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40}
            config = Path(temp) / 'config.json'
            config.write_text(json.dumps({'devices': {'mac': device}}))
            with mock.patch.object(suite, 'run_device', return_value={'name': 'mac', 'verdict': 'NOT TESTED', 'scenarios': []}) as run:
                self.assertEqual(suite.main(['--config', str(config), '--devices', 'mac', '--scenarios', '03', '--seconds', '2']), 2)
                self.assertEqual(run.call_args.args[2][0]['id'], '03')

class SshSuiteTests(unittest.TestCase):
    def test_remote_bundle_roundtrip_and_rejections(self):
        import base64
        import io
        import subprocess
        import zipfile
        definitions = [{'id': '03', 'title': 'Quest', 'checks': [{'id': 'identity', 'steps': [
            {'action': 'snapshot', 'as': 'q'}, {'action': 'assert', 'actual': 'q.active_id', 'op': 'eq', 'expected': 204}]}]}]
        device = {'adapter': 'ssh', 'build': 'dev', 'commit': 'a' * 40, 'host': 'fixture', 'checkout': '/fixture',
                  'remote': {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40}, 'shell': 'posix'}
        def transport(args, *unused, **kwargs):
            if 'EncodedCommand' in args[-1]:
                script = base64.b64decode(args[-1].split()[-1]).decode('utf-16le')
                payload = json.loads(base64.b64decode(script.split("'")[-2]))
            else:
                payload = json.loads(base64.b64decode(args[-1].split()[-1]))
            report = {'name': 'mac', 'commit': 'a' * 40, 'build': 'dev', 'request': payload['request'],
                      'definition_sha256': runner.object_hash(definitions), 'cleanup': 'PASS', 'artifact_sha256': 'b' * 64,
                      'verdict': 'PASS', 'scenarios': [{'id': '03', 'checks': [{'id': 'identity', 'verdict': 'PASS', 'reason': 'actual assertions'}]}]}
            if mutation:
                mutation(report)
            data = io.BytesIO()
            with zipfile.ZipFile(data, 'w') as bundle:
                bundle.writestr('report.json', json.dumps(report))
                bundle.writestr('run.log', 'public actual observation fixture')
                if evil:
                    bundle.writestr('../outside', 'escape')
            encoded = base64.b64encode(data.getvalue())
            return subprocess.CompletedProcess(args, exit_code, stdout=b'YAKUMO_SUITE:' + encoded + b'\n', stderr=b'')
        mutation, evil, exit_code = None, False, 0
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            for shell in ('posix', 'powershell'):
                target = base / shell
                target.mkdir()
                with mock.patch.object(runner, 'command', side_effect=transport):
                    report = suite.ssh_device({**device, 'shell': shell}, 'mac', definitions, target, 2)
                self.assertEqual(report['verdict'], 'PASS')
                self.assertTrue((target / 'run.log').is_file())
            for n, change in enumerate((lambda r: r.update(request='stale'), lambda r: r.update(commit='c' * 40),
                    lambda r: r.update(verdict='NOT TESTED'), lambda r: r['scenarios'][0]['checks'][0].update(id='missing'),
                    lambda r: r['scenarios'][0]['checks'][0].update(verdict='MAYBE'), lambda r: r.update(cleanup='FAIL'),
                    lambda r: r.update(artifact_sha256=None))):
                mutation = change
                target = base / str(n)
                target.mkdir()
                with mock.patch.object(runner, 'command', side_effect=transport):
                    with self.assertRaises(ValueError):
                        suite.ssh_device(device, 'mac', definitions, target, 2)
                self.assertFalse((target / 'run.log').exists())
            mutation, evil = None, True
            target = base / 'escape'; target.mkdir()
            with mock.patch.object(runner, 'command', side_effect=transport):
                with self.assertRaises(ValueError):
                    suite.ssh_device(device, 'mac', definitions, target, 2)
            evil, exit_code = False, 1
            target = base / 'exit'; target.mkdir()
            with mock.patch.object(runner, 'command', side_effect=transport):
                with self.assertRaises(ValueError):
                    suite.ssh_device(device, 'mac', definitions, target, 2)
            target = base / 'missing'; target.mkdir()
            with mock.patch.object(runner, 'command', return_value=subprocess.CompletedProcess([], 0, b'no bundle', b'')):
                with self.assertRaises(ValueError):
                    suite.ssh_device(device, 'mac', definitions, target, 2)

    def test_remote_export_uses_owned_evidence_only(self):
        import base64
        import contextlib
        import io
        request = {'device': {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40},
                   'name': 'mac', 'definitions': [], 'seconds': 2, 'request': 'private request'}
        report = {'verdict': 'NOT TESTED'}
        output = io.StringIO()
        with mock.patch.object(suite, 'run_device', return_value=report), contextlib.redirect_stdout(output):
            self.assertEqual(suite.main(['_remote', base64.b64encode(json.dumps(request).encode()).decode()]), 2)
        self.assertTrue(output.getvalue().startswith('YAKUMO_SUITE:'))


class CombatOracleTests(unittest.TestCase):
    def sample(self, point=(1000, 0, 0), health=45, same_area=True, cheats=False):
        return {'schema': 1, 'on_quest': True, 'free_camera': False, 'quest_cheats': cheats,
                'camera_target': list(point), 'camera_yaw': 0,
                'monsters': [{'address': 7, 'kind': 50, 'health': health, 'maximum': 45,
                              'same_area': same_area, 'position': [0, 0, 0]}]}

    def test_bounded_approach_and_actual_health_change(self):
        actor = mock.Mock()
        actor.adapter = Adapter()
        actor.snapshot.side_effect = [self.sample(), self.sample(), self.sample((100, 0, 0))]
        with mock.patch.object(suite.time, 'sleep'):
            target = suite.approach(actor, time.monotonic() + 5, kind=50)
        self.assertEqual(target['distance'], 100)
        self.assertIn(('input', '18:axis leftx 0'), actor.adapter.sent)
        self.assertEqual(actor.adapter.sent[-2:], [('input', '0:axis leftx 0'), ('input', '0:axis lefty 0')])
        actor.snapshot.side_effect = [self.sample(health=45), self.sample(health=30)]
        with mock.patch.object(suite.time, 'sleep'):
            hit = suite.attack(actor, target, time.monotonic() + 5)
        self.assertEqual(hit['before_health'], 45)
        self.assertEqual(hit['after_health'], 30)

    def test_mutations_reject_no_hit_other_area_cheats_and_nonconvergence(self):
        actor = mock.Mock()
        actor.adapter = Adapter()
        for sample in (self.sample(same_area=False), self.sample(cheats=True)):
            actor.snapshot.side_effect = None
            actor.snapshot.return_value = sample
            with self.assertRaises(checks.CheckFailed):
                suite.approach(actor, time.monotonic() + 5)
        actor.snapshot.return_value = self.sample()
        with mock.patch.object(suite.time, 'sleep'):
            with self.assertRaises(checks.CheckFailed):
                suite.approach(actor, time.monotonic() + 5, maximum_steps=2)
        target = self.sample()['monsters'][0]
        with mock.patch.object(suite.time, 'sleep'), mock.patch.object(runner, 'remaining', side_effect=[1, TimeoutError('no hit')]):
            with self.assertRaises(TimeoutError):
                suite.attack(actor, target, time.monotonic() + 5)
        actor.snapshot.return_value = self.sample(same_area=False)
        with self.assertRaises(checks.CheckFailed):
            suite.attack(actor, target, time.monotonic() + 5)
        actor.snapshot.return_value = self.sample(cheats=True)
        with self.assertRaises(checks.CheckFailed):
            suite.attack(actor, target, time.monotonic() + 5)
        with self.assertRaises(ValueError):
            suite.attack(actor, target, time.monotonic() + 5, strikes=4)


class CloudAllowanceTests(unittest.TestCase):
    def test_two_sequential_consumed_attempts_and_owned_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            ledger = base / 'ledger.json'
            ledger.write_text(json.dumps({'issue': 310, 'allowance': 2, 'maximum_seconds': 300, 'reservations': []}))
            fake = base / 'android.py'
            fake.write_text("""import sys
if 'create' in sys.argv:
 print('new reservation created with id owned123456; connected on port 5555')
elif 'list' in sys.argv:print('No reservations found.')
""")
            device = {'adapter': 'streaming', 'android': [sys.executable, str(fake)], 'project': 'private-fixture',
                      'model': 'fixture/33', 'reservation_ledger': str(ledger)}
            for n in range(2):
                target = base / str(n); target.mkdir()
                result = {'verdict': 'NOT TESTED', 'launched': True}
                with mock.patch.object(suite, 'run_device', return_value=result) as run:
                    report = suite.streaming_device(device, 'cloud', [], target, 5)
                self.assertTrue(report['reservation_released'])
                self.assertEqual(run.call_args.args[0]['serial'], 'localhost:5555')
                rows = json.loads(ledger.read_text())['reservations']
                self.assertEqual(len(rows), n + 1)
                self.assertTrue(all(row['state'] == 'released' for row in rows))
                self.assertFalse(ledger.with_suffix('.lock').exists())
            with self.assertRaises(runner.NotTested):
                suite.streaming_device(device, 'cloud', [], base, 5)
            with self.assertRaises(ValueError):
                suite.streaming_device(device, 'cloud', [], base, 301)
            with self.assertRaises(ValueError):
                suite.streaming_device({**device, 'setup_seconds': 1}, 'cloud', [], base, 5)
            rows[0]['state'] = 'unconfirmed'
            ledger.write_text(json.dumps({'issue': 310, 'allowance': 2, 'maximum_seconds': 300, 'reservations': rows[:1]}))
            with self.assertRaises(runner.NotTested):
                suite.streaming_device(device, 'cloud', [], base, 5)

    def test_no_creation_before_recognized_idle_and_failure_never_retries(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve(); ledger = base / 'ledger.json'
            ledger.write_text(json.dumps({'issue': 310, 'allowance': 2, 'maximum_seconds': 300, 'reservations': []}))
            device = {'adapter': 'streaming', 'project': 'private-fixture', 'model': 'fixture/33',
                      'reservation_ledger': str(ledger)}
            with mock.patch.object(runner, 'require_idle_project', side_effect=runner.NotTested('occupied')):
                with self.assertRaises(runner.NotTested):
                    suite.streaming_device(device, 'cloud', [], base, 5)
            self.assertEqual(json.loads(ledger.read_text())['reservations'], [])
            fake = base / 'fail.py'
            fake.write_text("""import sys
print('No reservations found.' if 'list' in sys.argv else 'activation failed')
sys.exit(0 if 'list' in sys.argv else 1)
""")
            target = base / 'failure';target.mkdir()
            with self.assertRaises(RuntimeError):
                suite.streaming_device({**device, 'android': [sys.executable, str(fake)]}, 'cloud', [], target, 5)
            rows = json.loads(ledger.read_text())['reservations']
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['state'], 'unconfirmed')


class ExtendedSuiteTests(unittest.TestCase):
    session = SuiteTests.session
    def test_text_readbacks_cover_translation_and_original_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            adapter = Adapter({'schema': 1, 'text_hex': '4f7265', 'on_quest': True, 'active_record_offset': 4,
                               'text_blocks': [{'entry': 4060, 'address': 0x08A40640}]})
            session = self.session(Path(temp), adapter)
            rows = [{'coverage': 'covered', 'probe': [0x08A40640, 3, 3], 'expected_hex': '4f7265'},
                    {'coverage': 'original-fallback', 'probe': {'active_ref_delta': 200}, 'expected_hex': '4f7265'},
                    {'coverage': 'covered', 'probe': {'entry': 4060, 'ref': 44}, 'expected_hex': '4f7265'}]
            program = [{'action': 'text_fields', 'name': 'fields', 'as': 'text', 'timeout': 1}]
            context = {}
            self.assertEqual(suite.execute(session, program, context, {'fields': rows}, {}, session.deadline), 3)
            self.assertEqual(context['text'][1]['probe']['ref'], 204)
            program[0]['timeout'] = .02
            adapter.sample['text_hex'] = '77726f6e67'
            with self.assertRaises(TimeoutError):
                suite.execute(session, program, {}, {'fields': rows}, {}, session.deadline)
            adapter.sample.pop('text_hex')
            with self.assertRaises(runner.NotTested):
                session.snapshot(until={'text_hex': '4f7265'})
            with self.assertRaises(runner.NotTested):
                suite.execute(session, program, {}, {}, {}, session.deadline)
            rows[0]['coverage'] = 'unknown'
            with self.assertRaises(ValueError):
                suite.execute(session, program, {}, {'fields': rows}, {}, session.deadline)

    def test_tree_relocation_and_adb_fingerprint_mutation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            session = self.session(root, Adapter())
            session.data_root = root / 'installed/data'
            (session.data_root / 'ms0').mkdir(parents=True)
            (session.data_root / 'ms0/save').write_text('copied save')
            self.assertEqual(session.tree('data/ms0'), checks.inventory(session.data_root / 'ms0'))
            adapter = object.__new__(runner.Adb)
            adapter.app = lambda value: (b'files/device-testing/data/ms0/save\n' if value.startswith('find') else b'a' * 64 + b'  save\n')
            session.adapter = adapter
            self.assertEqual(session.tree('data/ms0'), {'save': 'a' * 64})
            adapter.app = lambda value: b'../outside\n'
            with self.assertRaises(ValueError):
                session.tree('data/ms0')
            for name in ('../outside', 'shots'):
                with self.assertRaises(ValueError):
                    session.tree(name)

    def test_developer_epoch_and_multiplayer_mutation_guards(self):
        device = {'adapter': 'local', 'build': 'dev', 'observations': True, 'commit': 'a' * 40}
        runner.validate_device({**device, 'rtc_epoch_us': 1609459200000000})
        for epoch in (True, 0, None, 4102444800000001):
            with self.assertRaises(ValueError):
                runner.validate_device({**device, 'rtc_epoch_us': epoch})
        with tempfile.TemporaryDirectory() as temp:
            session = self.session(Path(temp), Adapter())
            for command in ('quest start 204', 'quest one_hit on', 'quest health on'):
                with self.assertRaises(ValueError):
                    suite.execute(session, [{'action': 'debug', 'value': command}], {}, {'multiplayer': True}, device, session.deadline)
            with self.assertRaises(runner.NotTested):
                suite.execute(session, [{'action': 'audio', 'as': 'audio'}], {}, {}, device, session.deadline)

    def test_repeated_measurement_calibrates_and_rejects_acceleration(self):
        from test_device_checks import png
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            parent = mock.Mock(root=root, logs=[])
            profile = {'prepare': [{'action': 'snapshot', 'as': 'loaded'}],
                       'route': [{'action': 'input', 'value': '0:axis leftx 0'}],
                       'state': {'loaded': True}, 'execution_mode': 'aot'}
            config = {'adapter': 'local', 'commit': 'c' * 40, 'baseline_commit': 'b' * 40,
                      'baseline_binary': 'baseline', 'baseline_sha256': 'b' * 64,
                      'performance_profiles': {'route': profile}}
            created = []
            accelerated = False
            class Child:
                def __init__(self, device, directory, deadline):
                    self.root = directory
                    self.logs = ['retained actual timing stream']
                    self.adapter = Adapter()
                    self.adapter.text = self.text
                    self.identity = {'artifact_sha256': device.get('artifact_sha256', 'c' * 64),
                                     'fixture_sha256': 'f' * 64, 'game_sha256': 'g' * 64}
                    self.counter = 0
                    self.closed = False
                    created.append(self)
                def prepare(self):
                    (self.root / 'settings.ini').write_text('video.scale=1\n')
                def snapshot(self, *args, **kwargs):
                    self.counter += 1
                    return {'loaded': True, 'vblanks': self.counter * 120, 'fast_forward': accelerated,
                            'fast_loading': False, 'quest_cheats': False, 'photo': False, 'perf_valid': True,
                            'speed_percent': 100, 'presented_fps': 120, 'frame_ms': [33.3] * 192}
                def text(self):
                    return '[gpu] synthetic GPU, driver test\n' + '[perf] fps 120 game 30 speed 100%\n' * self.counter
                def file(self, name):
                    return self.root / 'settings.ini'
                def capture(self, name):
                    (self.root / 'capture.png').write_bytes(png(8, 8, bytes(range(192))))
                    return 'capture.png'
                def close(self):
                    self.closed = True
            clock = [0.0]
            def tick():
                clock[0] += .25
                return clock[0]
            with mock.patch.object(suite, 'Session', Child), mock.patch.object(suite.time, 'monotonic', tick), \
                    mock.patch.object(suite.time, 'sleep'):
                result = suite.measure(parent, 'route', config, 1000, 's' * 64)
            self.assertTrue(result['comparison']['threshold_enabled'])
            self.assertEqual([len(result['runs'][x]) for x in ('baseline', 'candidate')], [3, 3])
            self.assertEqual(result['comparison']['metrics']['speed_percent']['candidate'], 100)
            self.assertTrue(all(child.closed for child in created))
            self.assertEqual(len(parent.logs), 6)
            for path in root.glob('perf-*'):
                import shutil
                shutil.rmtree(path)
            accelerated = True
            with mock.patch.object(suite, 'Session', Child), mock.patch.object(suite.time, 'monotonic', tick), \
                    mock.patch.object(suite.time, 'sleep'):
                with self.assertRaises(checks.CheckFailed):
                    suite.measure(parent, 'route', config, 1000, 's' * 64)
            self.assertTrue(created[-1].closed)
            self.assertEqual(parent.start.call_count, 2)
            with self.assertRaises(runner.NotTested):
                suite.measure(parent, 'missing', config, 1000, 's' * 64)


if __name__ == '__main__':
    unittest.main()
