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

    def test_readback_rejects_stale_and_boolean_request_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            for schema, request in ((True, 1), (1, True), (1, 0)):
                adapter = Adapter()
                session = self.session(Path(temp), adapter)
                session.observation_ready = True
                adapter.send = lambda *args: setattr(adapter, 'output', adapter.output + '[debug] observation ' +
                    json.dumps({'schema': schema, 'request': request, 'loaded': True}) + '\n')
                with self.assertRaises(TimeoutError):
                    session.snapshot(timeout=.02)
                self.assertEqual(session.samples, [])

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
    def fixture(self, base):
        source, root = base / 'fixture', base / 'session'
        source.mkdir(); root.mkdir()
        (source / 'ms0').mkdir()
        (source / 'ms0/save').write_bytes(b'public copied-save fixture')
        (source / 'EBOOT.ELF').write_bytes(b'public synthetic executable input')
        (source / 'disc.iso').write_bytes(b'public synthetic disc input')
        (source / 'settings.ini').write_text('video.scale=1\n')
        game = base / 'game.py'
        game.write_text('''import json,os,time
from pathlib import Path
path=Path(os.environ['MHP3RD_DEBUG_COMMANDS'])
print('[debug] reading commands from fixture',flush=True)
offset=0
end=time.monotonic()+12
while time.monotonic()<end:
 lines=path.read_text().splitlines()
 for line in lines[offset:]:
  if line.startswith('observe '):
   print('[debug] observation '+json.dumps({'schema':1,'request':int(line.split()[1]),'quest_cheats':False}),flush=True)
 offset=len(lines)
 time.sleep(.01)
''')
        device = {'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40, 'observations': True,
                  'binary': sys.executable, 'args': [str(game)], 'saves': str(source / 'ms0'),
                  'settings': str(source / 'settings.ini'), 'game_dir': str(source)}
        return source, root, device

    def test_verified_language_variant_restores_original_inputs_and_owned_save(self):
        with tempfile.TemporaryDirectory() as temp:
            source, root, device = self.fixture(Path(temp).resolve())
            alternate = root.parent / 'alternate'; alternate.mkdir()
            (alternate / 'EBOOT.ELF').write_bytes(b'independent variant executable')
            (alternate / 'disc.iso').write_bytes(b'independent variant disc')
            language = root.parent / 'selected.lang'; language.write_text('1:2 = public translation\n')
            device['translation'] = str(language)
            device['env'] = {'MHP3RD_LANGUAGE': 'original'}
            variant = {'game_dir': str(alternate), 'game_sha256': runner.fingerprint(alternate / 'disc.iso'),
                       'language': 'fixture', 'translation': str(language), 'translation_sha256': runner.fingerprint(language)}
            device['game_variants'] = {'alternate': variant}
            session = suite.Session(device, root, time.monotonic() + 12)
            try:
                session.prepare()
                original = session.tree('data/ms0')
                result = session.variant('alternate')
                self.assertEqual(result['eboot_sha256'], runner.fingerprint(alternate / 'EBOOT.ELF'))
                self.assertEqual((root / 'data/disc.iso').read_bytes(), b'independent variant disc')
                with self.assertRaises(ValueError):
                    session.file('data/disc.iso')
                self.assertEqual(session.adapter.env['MHP3RD_LANGUAGE'], 'fixture')
                (root / 'data/ms0/save').write_bytes(b'changed owned save')
                session.restart(reset=True)
                self.assertEqual(session.tree('data/ms0'), original)
                self.assertEqual((root / 'data/disc.iso').read_bytes(), b'public synthetic disc input')
                self.assertEqual(session.adapter.env['MHP3RD_LANGUAGE'], 'original')
                self.assertNotIn('MHP3RD_TRACE_TEXT', session.adapter.env)
                self.assertEqual((root / 'translations/selected.lang').read_bytes(), language.read_bytes())
                self.assertFalse(session.variant_active)
                self.assertIsNone(session.variant_name)
                with self.assertRaises(runner.NotTested):
                    session.variant('missing')
                (alternate / 'disc.iso').write_bytes(b'wrong image')
                with self.assertRaises(ValueError):
                    session.variant('alternate')
                self.assertTrue(session.adapter.alive())
                (alternate / 'disc.iso').write_bytes(b'independent variant disc')
                language.write_text('wrong translation')
                with self.assertRaises(ValueError):
                    session.variant('alternate')
            finally:
                session.close()
            self.assertEqual((source / 'ms0/save').read_bytes(), b'public copied-save fixture')

    def test_two_owned_peers_have_distinct_settings_and_cleanup_after_check_failure(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temp:
            source, root, device = self.fixture(Path(temp).resolve())
            session = suite.Session(device, root, time.monotonic() + 12)
            real_popen = subprocess.Popen
            launched = []
            def spawn(args, **kwargs):
                if '--adhoc-server' in args:
                    self.assertEqual(args[0], sys.executable)
                    self.assertGreater(float(args[3]), 0)
                    self.assertLessEqual(float(args[3]), 12)
                    self.assertIs(kwargs['stdin'], subprocess.DEVNULL)
                    # A real bounded process substitutes only the unavailable native server.
                    args = [sys.executable, '-c', 'import time; time.sleep(12)']
                process = real_popen(args, **kwargs)
                launched.append(process)
                return process
            try:
                session.prepare()
                with mock.patch.object(suite.subprocess, 'Popen', side_effect=spawn):
                    identity = session.open_peer()
                self.assertEqual(identity['peer_artifact_sha256'], runner.fingerprint(sys.executable))
                self.assertEqual(identity['peer_fixture_sha256'], session.identity['fixture_sha256'])
                primary = checks.settings(session.file('data/settings.ini'))
                secondary = checks.settings(session.peer.file('data/settings.ini'))
                self.assertEqual(primary['network.server'], secondary['network.server'])
                self.assertNotEqual(primary['network.mac'], secondary['network.mac'])
                self.assertEqual(session.peer.snapshot()['quest_cheats'], False)
                program = [{'action': 'snapshot', 'as': 'peer-state'},
                           {'action': 'assert', 'actual': 'peer-state.quest_cheats', 'op': 'eq', 'expected': False}]
                session.peer.device['suite_fixtures'] = {'ordinary': program,
                    'cheat': [{'action': 'debug', 'value': 'quest start 204'}]}
                context = {}
                self.assertEqual(suite.execute(session, [{'action': 'peer_fixture', 'name': 'ordinary', 'as': 'peer'}],
                    context, {}, device, session.deadline), 1)
                self.assertFalse(context['peer']['peer-state']['quest_cheats'])
                with self.assertRaises(ValueError):
                    suite.execute(session, [{'action': 'peer_fixture', 'name': 'cheat'}], {}, {}, device, session.deadline)
                with self.assertRaises(ValueError):
                    session.open_peer()
                with self.assertRaises(runner.NotTested):
                    suite.execute(session, [{'action': 'peer_fixture', 'name': 'missing'}], {}, {}, device, session.deadline)
                session.restart(reset=True)
                self.assertIsNone(session.peer)
                self.assertIsNone(session.server)
                self.assertNotIn('network.mac', checks.settings(session.file('data/settings.ini')))
            finally:
                session.close()
            self.assertTrue(all(process.poll() is not None for process in launched))
            self.assertEqual((source / 'settings.ini').read_text(), 'video.scale=1\n')

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
                for profile, cap in (('quick', 360), ('extended', 1800)):
                    self.assertEqual(suite.main(['--config', str(config), '--devices', 'mac',
                                                '--profile', profile, '--scenarios', '03']), 2)
                    self.assertEqual(run.call_args.args[-1], cap)

class SshSuiteTests(unittest.TestCase):
    def test_unavailable_ssh_is_incomplete_but_unknown_transport_failures_are_not_hidden(self):
        import subprocess
        device = {'remote': {'build': 'dev', 'commit': 'a' * 40}, 'checkout': '/fixture', 'host': 'fixture'}
        with tempfile.TemporaryDirectory() as temp:
            for reason in ('Connection timed out', 'Operation timed out', 'Connection refused', 'No route to host',
                           'Could not resolve hostname fixture'):
                result = subprocess.CompletedProcess([], 255, b'', ('ssh: ' + reason).encode())
                with mock.patch.object(runner, 'command', return_value=result), self.assertRaises(runner.NotTested):
                    suite.ssh_device(device, 'deck', [], Path(temp), 2)
            for output, reason in ((b'game already started', b'Connection timed out'),
                                   (b'', b'Host key verification failed'), (b'', b'remote process crashed')):
                result = subprocess.CompletedProcess([], 255, output, reason)
                with mock.patch.object(runner, 'command', return_value=result), self.assertRaises(ValueError):
                    suite.ssh_device(device, 'deck', [], Path(temp), 2)

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
        return {'schema': 1, 'on_quest': True, 'free_camera': False, 'quest_cheats': cheats, 'vblanks': 2,
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
        attacks = [value for kind, value in actor.adapter.sent if 'pad y' in value]
        self.assertEqual(attacks, ['0:pad y 4'])

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
        actor.snapshot.return_value = self.sample(health=30)
        with self.assertRaises(checks.CheckFailed):
            suite.attack(actor, target, time.monotonic() + 5)
        with self.assertRaises(ValueError):
            suite.attack(actor, target, time.monotonic() + 5, strikes=4)


class CloudAllowanceTests(unittest.TestCase):
    def test_rerun_authorization_preserves_two_old_rows_one_per_model_and_cost_cap(self):
        with tempfile.TemporaryDirectory() as temp:
            base=Path(temp).resolve(); ledger=base/'ledger.json'
            old=[{'model':'old-a/30','state':'released','cap_seconds':500,'duration_seconds':244},
                 {'model':'old-b/31','state':'released','cap_seconds':500,'duration_seconds':234}]
            value={'issue':310,'allowance':2,'maximum_seconds':500,'reservations':copy.deepcopy(old)}
            legacy={'streaming_cap_seconds':500,'model':'old-a/30'}
            with self.assertRaises(runner.NotTested): suite.cloud_authorization(value,legacy,500)
            phase={'id':'quick-rerun','allowance':2,'maximum_seconds':800,'target_seconds':600,
                   'rate_usd_per_minute':.15,'maximum_cost_usd':2,'previous_total_attempts':2,
                   'models':['fixture-a/30','fixture-b/31']}
            value['authorization_phases']=[phase];ledger.write_text(json.dumps(value))
            fake=base/'android.py';fake.write_text("""import sys
if 'create' in sys.argv: print('new reservation created with id owned123456; connected on port 5555')
elif 'list' in sys.argv: print('No reservations found.')
""")
            device={'adapter':'streaming','android':[sys.executable,str(fake)],'project':'private-fixture',
                    'model':'fixture-a/30','reservation_ledger':str(ledger),'streaming_cap_seconds':800,
                    'authorization_phase':'quick-rerun'}
            for model in phase['models']:
                selected={**device,'model':model}
                self.assertEqual(suite.cloud_authorization(value,selected,800),phase)
            for change in ({'model':'unlisted/30'},{'authorization_phase':'missing'},{'streaming_cap_seconds':801}):
                with self.assertRaises(ValueError): suite.cloud_authorization(value,{**device,**change},800)
            bad=copy.deepcopy(value);bad['authorization_phases'][0]['rate_usd_per_minute']=.20
            with self.assertRaises(ValueError):suite.cloud_authorization(bad,device,800)
            with self.assertRaises(ValueError):suite.cloud_authorization(value,device,801)
            for retained in ([],old[:1],old+old):
                bad=copy.deepcopy(value);bad['reservations']=retained
                with self.assertRaisesRegex(ValueError,'historical attempts'):
                    suite.cloud_authorization(bad,device,800)
            for count in (None,0,1,3):
                bad=copy.deepcopy(value);bad['authorization_phases'][0]['previous_total_attempts']=count
                with self.assertRaisesRegex(ValueError,'historical attempts'):
                    suite.cloud_authorization(bad,device,800)
            bad=copy.deepcopy(value);bad['maximum_seconds']=800
            with self.assertRaisesRegex(ValueError,'historical attempts'):
                suite.cloud_authorization(bad,device,800)
            for index,model in enumerate(phase['models']):
                root=base/str(index);root.mkdir()
                with mock.patch.object(suite,'run_device',return_value={'verdict':'NOT TESTED','launched':True}):
                    suite.streaming_device({**device,'model':model},model.replace('/','-'),[],root,800)
                saved=json.loads(ledger.read_text())
                self.assertEqual(saved['reservations'][:2],old)
                row=saved['reservations'][-1]
                self.assertEqual(row['maximum_estimated_cost_usd'],2)
                self.assertLessEqual(row['estimated_cost_usd'],2)
                self.assertEqual(row['state'],'released')
                with self.assertRaises(runner.NotTested):suite.cloud_authorization(saved,{**device,'model':model},800)
            self.assertEqual(len(saved['reservations']),4)
            with self.assertRaises(runner.NotTested):suite.cloud_authorization(saved,legacy,500)

    def test_explicit_500_budget_preserves_migrated_history_and_cleanup_deadline(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve(); ledger = base / 'ledger.json'
            history = {'attempt': 'earlier', 'state': 'released', 'cap_seconds': 300, 'reservation': 'older123456'}
            value = {'issue': 310, 'allowance': 2, 'maximum_seconds': 300, 'reservations': [history]}
            ledger.write_text(json.dumps(value))
            # An explicit authorization migration changes the bound, never consumption.
            migrated = json.loads(ledger.read_text()); migrated['maximum_seconds'] = 500
            migrated['authorization_history'] = [{'previous': 300, 'maximum': 500}]
            ledger.write_text(json.dumps(migrated))
            fake = base / 'android.py'
            fake.write_text("""import sys
if 'create' in sys.argv: print('new reservation created with id owned123456; connected on port 5555')
elif 'list' in sys.argv: print('No reservations found.')
""")
            device = {'adapter': 'streaming', 'android': [sys.executable, str(fake)], 'project': 'private-fixture',
                      'model': 'fixture/33', 'reservation_ledger': str(ledger), 'streaming_cap_seconds': 500}
            target = base / 'pilot'; target.mkdir()
            started = time.monotonic()
            with mock.patch.object(suite, 'run_device', return_value={'verdict': 'NOT TESTED', 'launched': True}) as run:
                report = suite.streaming_device(device, 'cloud', [], target, 500)
            self.assertGreater(run.call_args.args[-1], 450)
            self.assertLessEqual(run.call_args.args[-1], 470)
            self.assertGreaterEqual(run.call_args.args[0]['streaming_cleanup_deadline'], started + 500)
            self.assertLessEqual(run.call_args.args[0]['streaming_cleanup_deadline'], time.monotonic() + 500)
            self.assertEqual(report['reservation_total_cap_seconds'], 500)
            self.assertTrue(report['reservation_released'])
            saved = json.loads(ledger.read_text())
            self.assertEqual(saved['reservations'][0], history)
            self.assertEqual(len(saved['reservations']), 2)
            self.assertEqual(saved['authorization_history'], migrated['authorization_history'])
            for name in ('reservation-preflight.json', 'reservation-remove.json', 'reservation-final-list.json'):
                self.assertEqual(json.loads((target / name).read_text())['returncode'], 0)
            with self.assertRaises(ValueError):
                suite.streaming_device(device, 'cloud', [], target, 501)
            with self.assertRaises(ValueError):
                suite.streaming_device({k: v for k, v in device.items() if k != 'streaming_cap_seconds'},
                                       'cloud', [], target, 500)
            with self.assertRaises(runner.NotTested):
                suite.streaming_device(device, 'cloud', [], target, 500)

    def test_setup_failure_consumes_attempt_but_releases_owned_reservation(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve(); ledger = base / 'ledger.json'
            ledger.write_text(json.dumps({'issue': 310, 'allowance': 2, 'maximum_seconds': 500, 'reservations': []}))
            fake = base / 'android.py'
            fake.write_text("""import sys
if 'create' in sys.argv: print('new reservation created with id owned123456; connected on port 5555')
elif 'list' in sys.argv: print('No reservations found.')
""")
            device = {'adapter': 'streaming', 'android': [sys.executable, str(fake)], 'project': 'private-fixture',
                      'model': 'fixture/33', 'reservation_ledger': str(ledger), 'streaming_cap_seconds': 500}
            with mock.patch.object(suite, 'run_device', side_effect=TimeoutError('bounded upload expired')) as run:
                with self.assertRaises(TimeoutError):
                    suite.streaming_device(device, 'cloud', [], base, 500)
            self.assertEqual(run.call_count, 1)
            rows = json.loads(ledger.read_text())['reservations']
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['state'], 'released')
            self.assertFalse(ledger.with_suffix('.lock').exists())

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

    def test_quick_profile_has_ten_limited_categories_and_no_deep_prerequisites(self):
        quick = suite.definitions('quick')
        self.assertEqual(len(quick), 10)
        self.assertTrue(all(value['profile'] == 'quick' for value in quick))
        self.assertTrue(all(check['evidence_kind'] in ('real-game smoke', 'synthetic/native contract')
                            for value in quick for check in value['checks']))
        text = json.dumps(quick)
        for forbidden in ('normal-counter-gate', 'abandon-quest', 'reach-live-large', '"measure"', 'peer_fixture'):
            self.assertNotIn(forbidden, text)
        with self.assertRaises(ValueError):
            suite.definitions('pretend-full')

    @unittest.skipIf(sys.platform == 'win32', 'Public CTest transport oracle uses the POSIX C++ compiler')
    def test_native_contract_executes_registered_oracle_and_rejects_mutated_file(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve(); build = base / 'out/native'; build.mkdir(parents=True)
            oracle = build / 'oracle'; source = build / 'oracle.cpp'; data = build / 'fixture.bin'
            data.write_bytes(b'public independently expected save payload')
            source.write_text('#include <fstream>\n#include <string>\nint main(int n,char **v){if(n!=2)return 2;std::ifstream f(v[1]);std::string s;std::getline(f,s);return s=="public independently expected save payload"?0:1;}\n')
            subprocess.run(['c++', str(source), '-o', str(oracle)], check=True, timeout=30)
            # An actual CTest run exercises process, output and artifact recording.
            (build / 'CTestTestfile.cmake').write_text('add_test(mhp3rd_savedata_tests "' + str(oracle).replace('\\', '/') +
                '" "' + str(data).replace('\\', '/') + '")\n')
            session = self.session(base, object.__new__(runner.Local))
            device = {'native_test_dir': 'out/native'}
            with mock.patch.object(runner, 'ROOT', base):
                # Test executables are fingerprinted within the owned build directory.
                result = suite.native_contract(session, 'saves', device, time.monotonic() + 15)
                self.assertTrue(result['all_passed'])
                self.assertEqual(result['evidence_kind'], 'synthetic/native contract')
                self.assertEqual(result['artifact_sha256']['mhp3rd_savedata_tests'], runner.fingerprint(oracle))
                data.write_bytes(b'corrupted payload')
                with self.assertRaises(RuntimeError):
                    suite.native_contract(session, 'saves', device, time.monotonic() + 15)
                with self.assertRaises(runner.NotTested):
                    suite.native_contract(session, 'mods', device, time.monotonic() + 15)
            with self.assertRaises(ValueError):
                suite.native_contract(session, 'invented', device, session.deadline)
            with self.assertRaises(runner.NotTested):
                suite.native_contract(session, 'saves', {}, session.deadline)

    def test_short_telemetry_preserves_slow_speed_and_tails_without_claiming_comparison(self):
        with tempfile.TemporaryDirectory() as temp:
            session = self.session(Path(temp), Adapter())
            scene = {'map': 'P_m12a00d.ovl', 'vblanks': 100, 'fast_loading': False, 'fast_forward': False,
                     'quest_cheats': False, 'photo': False, 'perf_valid': True, 'speed_percent': 83,
                     'game_fps': 25, 'presented_fps': 90, 'frame_ms': [500] * 75 + [33.3] * 25 + [90] * 5,
                     'target_width': 480, 'target_height': 272}
            samples = [scene, scene] + [{**scene, 'perf_second': n} for n in range(3)]
            session.adapter.output += '[gpu] public fixture GPU, driver public fixture version\n'
            session.snapshot = mock.Mock(side_effect=samples)
            with mock.patch.object(suite.time, 'sleep'):
                value = suite.quick_telemetry(session, session.deadline)
            self.assertEqual(value['samples'][0]['speed_percent'], 83)
            self.assertEqual(value['samples'][0]['p99_ms'], 90)
            self.assertNotIn(500, value['samples'][0]['frame_ms'])
            self.assertIn('Deferred', value['comparison'])
            self.assertEqual(value['resolution'], [480, 272])
            self.assertTrue(session.snapshot.call_args_list[0].kwargs['performance'])
            session.adapter.output = ''
            session.snapshot.side_effect = samples
            with mock.patch.object(suite.time, 'sleep'), self.assertRaises(runner.NotTested):
                suite.quick_telemetry(session, session.deadline)
            session.snapshot.side_effect = [{**scene, 'target_width': None}]
            with self.assertRaises(runner.NotTested):
                suite.quick_telemetry(session, session.deadline)
            session.snapshot.side_effect = [scene, scene, {**scene, 'target_width': 481}]
            with self.assertRaises(checks.CheckFailed):
                suite.quick_telemetry(session, session.deadline)
            session.snapshot.side_effect = [scene, scene, {**scene, 'fast_forward': True}]
            with self.assertRaises(checks.CheckFailed):
                suite.quick_telemetry(session, session.deadline)
            session.snapshot.side_effect = [scene, scene, {**scene, 'perf_second': 1, 'frame_ms': [0] * 192}]
            with self.assertRaises(checks.CheckFailed):
                suite.quick_telemetry(session, session.deadline)

    def test_builtin_offline_setup_proves_actual_state_and_refuses_online_or_release(self):
        with tempfile.TemporaryDirectory() as temp:
            session = self.session(Path(temp), Adapter())
            session.restart = mock.Mock()
            village = {'loaded': True, 'task': 'lobby_task.ovl', 'village_ready': True, 'fast_loading': False,
                       'on_quest': False, 'quest_cheats': False, 'online': False}
            quest = {**village, 'task': 'game_task.ovl', 'on_quest': True, 'map': 'P_m12a00d.ovl',
                     'accepted_id': 204, 'active_id': 204, 'quest_ticks': 89960}
            device = {'build': 'dev', 'suite_fixtures': {'load-character': [{'action': 'input', 'value': '0:pad start 4'}]}}
            program = [{'action': 'fixture', 'name': 'offline-quest-204'}]
            suite.validate_program(program, fixture=True)
            context = {}
            session.snapshot = mock.Mock(side_effect=[village, quest, {**quest, 'quest_ticks': 89959}])
            self.assertEqual(suite.execute(session, program, context, {}, device, session.deadline), 7)
            self.assertEqual(context['_offline_setup']['method'], 'developer quest start')
            session.restart.assert_called_once_with(reset=True, retain_variant=True)
            self.assertIn(('debug', 'quest start 204'), session.adapter.sent)
            self.assertEqual(session.snapshot.call_args.kwargs['until'], {'quest_ticks': {'op': 'lt', 'value': 89960}})
            for mutation in ({**quest, 'quest_cheats': True}, {**quest, 'accepted_id': 309}, {**quest, 'map': 'P_v00a00d.ovl'},
                             {**quest, 'quest_ticks': 0}):
                session.snapshot.side_effect = [village, mutation]
                with self.assertRaises(checks.CheckFailed):
                    suite.execute(session, program, {}, {}, device, session.deadline)
            session.snapshot.side_effect = [{**village, 'online': True}]
            with self.assertRaises(checks.CheckFailed):
                suite.execute(session, program, {}, {}, device, session.deadline)
            for parameters, build in (({'multiplayer': True}, 'dev'), ({}, 'release')):
                with self.assertRaises((ValueError, runner.NotTested)):
                    suite.execute(session, program, {}, parameters, {**device, 'build': build}, session.deadline)
            with self.assertRaises(runner.NotTested):
                suite.execute(session, program, {}, {}, {'build': 'dev'}, session.deadline)
            with self.assertRaises(ValueError):
                suite.execute(session, [{'action': 'fixture', 'name': 'offline-quest-1'}], {}, {}, device, session.deadline)

    def test_failed_subcheck_recovers_without_erasing_failure_or_skipping_next(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            session = self.session(root, Adapter())
            session.identity = {}; session.prepare = lambda launch=True: None
            session.restart = mock.Mock()
            bad = [{'action': 'snapshot', 'as': 'q'}, {'action': 'assert', 'actual': 'q.active_id', 'op': 'eq', 'expected': 309}]
            good = [{**bad[0]}, {**bad[1], 'expected': 204}]
            definition = [{'id': '03', 'title': 'Core', 'checks': [{'id': 'failed-route', 'steps': bad},
                                                                {'id': 'independent-core', 'steps': good}]}]
            with mock.patch.object(suite, 'Session', return_value=session):
                report = suite.run_device({'adapter': 'local', 'build': 'dev', 'commit': 'a' * 40}, 'mac', definition, root, 3)
            checks_rows = report['scenarios'][0]['checks']
            self.assertEqual([row['verdict'] for row in checks_rows], ['FAIL', 'PASS'])
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertEqual(report['cleanup'], 'PASS')
            self.assertEqual(report['recovery'][0]['after'], 'failed-route')
            session.restart.assert_called_once_with(reset=True, retain_variant=True)

    def test_photo_pixels_screenshot_file_and_mutated_frozen_frame(self):
        from test_device_checks import png
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / 'shots').mkdir()
            (root / 'data/screenshots').mkdir(parents=True)
            visible = png(8, 8, bytes(range(192)))
            hidden = png(8, 8, bytes(reversed(range(192))))
            (root / 'shots/visible.png').write_bytes(visible)
            (root / 'shots/hidden.png').write_bytes(hidden)
            adapter = object.__new__(runner.Local)
            adapter.output = ''
            adapter.text = lambda: adapter.output
            def shutter(kind, value):
                self.assertEqual((kind, value), ('input', '0:scan F12 4'))
                (root / 'data/screenshots/new.png').write_bytes(hidden)
                adapter.output += '[screenshot] new.png (8x8)\n'
            adapter.send = shutter
            session = self.session(root, adapter)
            program = [{'action': 'game_png', 'as': 'png'},
                       {'action': 'image', 'image': 'png', 'dimensions': [8, 8], 'as': 'content'},
                       {'action': 'difference', 'before': 'visible', 'after': 'png', 'op': 'gt', 'expected': .1}]
            context = {'visible': 'shots/visible.png'}
            self.assertEqual(suite.execute(session, program, context, {}, {}, session.deadline), 2)
            self.assertEqual((root / context['png']).read_bytes(), hidden)
            (root / context['png']).write_bytes(visible)
            with self.assertRaises(checks.CheckFailed):
                suite.execute(session, [program[-1]], context, {}, {}, session.deadline)
            adapter = Adapter({'schema': 1, 'clock_us': 100, 'vblanks': 200})
            session = self.session(root, adapter)
            context = {}
            program = [{'action': 'hold', 'fields': ['clock_us', 'vblanks'], 'seconds': .1}]
            self.assertEqual(suite.execute(session, program, context, {}, {}, session.deadline), 1)
            original_send = adapter.send
            def advancing(kind, value):
                adapter.sample['vblanks'] += 1
                original_send(kind, value)
            adapter.send = advancing
            with self.assertRaises(checks.CheckFailed):
                suite.execute(session, program, context, {}, {}, session.deadline)

    def test_navigation_reference_is_hardware_pinned_and_cannot_replace_assertions(self):
        from test_device_checks import png
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            reference = root / 'expected.png'
            reference.write_bytes(png(8, 8, bytes(range(192))))
            (root / 'captured.png').write_bytes(reference.read_bytes())
            session = self.session(root, Adapter())
            session.capture = lambda name: 'captured.png'
            device = {'hardware': {'device': 'public synthetic hardware'}, 'navigation_references': {
                'menu': {'reviewed': True, 'hardware': {'device': 'public synthetic hardware'},
                         'path': str(reference), 'sha256': runner.fingerprint(reference), 'region': [0, 0, 8, 8]}}}
            context = {}
            program = [{'action': 'reference', 'name': 'menu'}]
            self.assertEqual(suite.execute(session, program, context, {}, device, session.deadline), 0)
            self.assertIn('_navigation_menu', context)
            device['hardware'] = {'device': 'different GPU'}
            with self.assertRaises(runner.NotTested):
                suite.execute(session, program, {}, {}, device, session.deadline)
            device['hardware'] = {'device': 'public synthetic hardware'}
            reference.write_bytes(b'changed reference')
            with self.assertRaises(ValueError):
                suite.execute(session, program, {}, {}, device, session.deadline)

    def test_variant_navigation_uses_its_own_route_and_returns_to_default(self):
        with tempfile.TemporaryDirectory() as temp:
            session = self.session(Path(temp), Adapter({'schema': 1, 'active_id': 309}))
            default = [{'action': 'snapshot', 'as': 'q'}, {'action': 'assert', 'actual': 'q.active_id', 'op': 'eq', 'expected': 204}]
            patched = [{'action': 'snapshot', 'as': 'q'}, {'action': 'assert', 'actual': 'q.active_id', 'op': 'eq', 'expected': 309}]
            device = {'suite_fixtures': {'load': default}, 'game_variants': {'patched': {'suite_fixtures': {'load': patched}}}}
            session.variant_name = 'patched'
            self.assertEqual(suite.execute(session, [{'action': 'fixture', 'name': 'load'}], {}, {}, device, session.deadline), 1)
            session.variant_name = None
            with self.assertRaises(checks.CheckFailed):
                suite.execute(session, [{'action': 'fixture', 'name': 'load'}], {}, {}, device, session.deadline)

    def test_finite_menu_advancing_stops_on_target_and_rejects_nonconvergence(self):
        from test_device_checks import png
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / 'expected.png').write_bytes(png(8, 8, bytes(range(192))))
            (root / 'wrong.png').write_bytes(png(8, 8, bytes(reversed(range(192)))))
            session = self.session(root, Adapter())
            session.capture = mock.Mock(side_effect=['wrong.png', 'expected.png'])
            session.snapshot = mock.Mock(return_value={'vblanks': 100})
            device = {'hardware': {'gpu': 'public synthetic GPU'}, 'navigation_references': {'stars': {
                'reviewed': True, 'hardware': {'gpu': 'public synthetic GPU'}, 'path': str(root / 'expected.png'),
                'sha256': runner.fingerprint(root / 'expected.png'), 'region': [0, 0, 8, 8]}}}
            program = [{'action': 'menu_advance', 'name': 'stars', 'input': '0:pad b 4', 'maximum_presses': 2}]
            suite.validate_program(program)
            with mock.patch.object(suite.time, 'sleep'):
                self.assertEqual(suite.execute(session, program, {}, {}, device, session.deadline), 0)
            self.assertEqual(session.adapter.sent, [('input', '0:pad b 4')])
            self.assertEqual(session.snapshot.call_args.kwargs['until'], {'vblanks': {'op': 'ge', 'value': 460}})
            session.adapter.sent.clear()
            session.capture = lambda name: 'wrong.png'
            with mock.patch.object(suite.time, 'sleep'), self.assertRaises(checks.CheckFailed):
                suite.execute(session, program, {}, {}, device, session.deadline)
            self.assertEqual(len(session.adapter.sent), 2)
            (root / 'ready.png').write_bytes(png(8, 8, bytes([240, 40, 40]) * 64))
            session.adapter.sent.clear()
            session.capture = mock.Mock(side_effect=['wrong.png', 'ready.png', 'expected.png'])
            ready_program = [{**program[0], 'ready_colour': {'region': [0, 0, 8, 8], 'rgb': [240, 40, 40],
                                                          'tolerance': 0, 'minimum_pixels': 16}}]
            with mock.patch.object(suite.time, 'sleep'):
                self.assertEqual(suite.execute(session, ready_program, {}, {}, device, session.deadline), 0)
            self.assertEqual(session.adapter.sent, [('input', '0:pad b 4')])
            for minimum in (True, 0, 4097):
                with self.assertRaises(ValueError):
                    suite.validate_program([{**ready_program[0], 'ready_colour': {'minimum_pixels': minimum}}])
            for value in (True, 0, 5):
                with self.assertRaises(ValueError):
                    suite.validate_program([{**program[0], 'maximum_presses': value}])

    def test_lifecycle_requires_actual_focus_clock_and_neutral_release(self):
        with tempfile.TemporaryDirectory() as temp:
            session = self.session(Path(temp), Adapter())
            before = {'focus': True, 'clock_us': 100, 'pad_buttons': 0}
            returned = {'focus': True, 'clock_us': 200, 'pad_buttons': 0}
            session.activate = mock.Mock(side_effect=[before, returned])
            session.snapshot = mock.Mock(return_value={'focus': False, 'clock_us': 110})
            context = {}
            program = [{'action': 'focus_cycle', 'as': 'cycle'}]
            self.assertEqual(suite.execute(session, program, context, {}, {}, session.deadline), 4)
            self.assertEqual(session.snapshot.call_args.kwargs['until'], {'focus': False})
            self.assertEqual(session.adapter.sent, [('input', '0:window minimize'), ('input', '0:window restore')])
            for mutation in ({**returned, 'clock_us': 100}, {**returned, 'pad_buttons': 1}):
                session.activate.side_effect = [before, mutation]
                with self.assertRaises(checks.CheckFailed):
                    suite.execute(session, program, {}, {}, {}, session.deadline)

    @unittest.skipIf(sys.platform == 'win32', 'POSIX parent-process and desktop API variants')
    def test_activation_uses_only_proven_owned_pid_and_observed_focus(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            adapter = object.__new__(runner.Local)
            adapter.process = mock.Mock(pid=123)
            adapter.alive = lambda: True
            session = self.session(root, adapter)
            session.snapshot = mock.Mock(return_value={'host_pid': 456, 'focus': True})
            for platform in ('darwin', 'linux'):
                results = [b'123\n', b'789\n', b''] if platform == 'linux' else [b'123\n', b'']
                with mock.patch.object(suite.sys, 'platform', platform), mock.patch.object(suite.shutil, 'which', return_value='xdotool'), \
                        mock.patch.object(runner, 'command', side_effect=results) as command:
                    self.assertTrue(session.activate()['focus'])
                    self.assertEqual(command.call_args_list[0].args[0], ['ps', '-o', 'ppid=', '-p', '456'])
                    self.assertEqual(session.snapshot.call_args.kwargs['until'], {'focus': True})
            with mock.patch.object(runner, 'command', return_value=b'999\n'):
                with self.assertRaises(ValueError):
                    session.activate()
            with mock.patch.object(suite.sys, 'platform', 'linux'), mock.patch.object(suite.shutil, 'which', return_value=None), \
                    mock.patch.object(runner, 'command', return_value=b'123\n'):
                with self.assertRaises(runner.NotTested):
                    session.activate()
            session.snapshot.return_value = {'host_pid': True}
            with self.assertRaises(ValueError):
                session.activate()

    def test_text_readbacks_cover_translation_and_original_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            adapter = Adapter({'schema': 1, 'text_hex': '4f7265', 'on_quest': True, 'active_record_offset': 4,
                               'text_blocks': [{'entry': 4060, 'address': 0x08A40640}]})
            session = self.session(Path(temp), adapter)
            rows = [{'coverage': 'covered', 'probe': [0x08A40640, 3, 3], 'expected_hex': '4f7265'},
                    {'coverage': 'original-fallback', 'probe': {'active_ref_delta': 200}, 'expected_hex': '4f7265'},
                    {'coverage': 'covered', 'probe': {'entry': 4060, 'base_offset': 128, 'ref': 44}, 'expected_hex': '4f7265'}]
            program = [{'action': 'text_fields', 'name': 'fields', 'as': 'text', 'timeout': 1}]
            context = {}
            self.assertEqual(suite.execute(session, program, context, {'fields': rows}, {}, session.deadline), 3)
            self.assertEqual(context['text'][1]['probe']['ref'], 204)
            self.assertEqual(context['text'][2]['probe']['base'], 0x08A406C0)
            for offset in (True, -1, 1024 * 1024):
                changed = [{**rows[2], 'probe': {**rows[2]['probe'], 'base_offset': offset}}]
                with self.assertRaises(ValueError):
                    suite.execute(session, program, {}, {'fields': changed}, {}, session.deadline)
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
            for command in ('quest start 204', 'quest one_hit on', 'quest health on', ' quest start 204', '\tquest one_hit on'):
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
