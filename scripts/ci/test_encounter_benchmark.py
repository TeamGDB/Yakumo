"""Public synthetic observations and owned child processes; no game-data timing."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'profiles/mhp3rd/tools'))
import encounter_benchmark as bench


def state():
    return {'schema': 1, 'preset': 'barioth-tundra', 'phase': 'ready', 'actor': 123, 'model': 456, 'in_view': True}


def sample():
    return {'on_quest': True, 'task': 'game_task.ovl', 'map': 'P_m04a07d.ovl', 'accepted_id': 507,
            'active_id': 507, 'online': False, 'quest_cheats': False, 'photo': False,
            'fast_forward': False, 'fast_loading': False, 'lock_on': True, 'lock_target': 123,
            'monsters': [{'address': 123, 'kind': 5, 'same_area': True, 'health': 2000}],
            'target_width': 480, 'target_height': 272, 'host_pid': 77}


FAKE_GAME = r'''
import json, os, pathlib, sys, time
root=pathlib.Path(os.environ['MHP3RD_DATA_DIR']).parent
debug=pathlib.Path(os.environ['MHP3RD_DEBUG_COMMANDS'])
inputs=pathlib.Path(os.environ['MHP3RD_INPUT_LIVE'])
print('Functions: 12345\nOverlay corpora: 355\n[gpu] Synthetic GPU, driver synthetic\n[gpu-compat] off',flush=True)
print('[debug] reading commands from synthetic',flush=True)
mode=sys.argv[1] if len(sys.argv)>1 else ''
if mode!='rtc':print('[device-test] RTC epoch 1780000000000000 us',flush=True)
phase='demo';offset=0;input_offset=0;ticks=90000;sequence=0;second=0
ready={'schema':1,'preset':'barioth-tundra','phase':'ready','actor':123,'model':456,'in_view':True}
while True:
 text=inputs.read_text()
 for line in text[input_offset:].splitlines():
  if 'pad b' in line: phase='village'
  if 'shot ' in line:
   name=line.split('shot ',1)[1]
   (root/'shots'/(name+'.bmp')).write_bytes(b'BM synthetic capture requiring review')
 input_offset=len(text)
 text=debug.read_text()
 for line in text[offset:].splitlines():
  if line=='encounter start barioth':phase='quest'
  if line.startswith('encounter '):
   observed={**ready,'in_view':not(mode=='invisible' and ticks<=89890)}
   print('[debug] encounter '+json.dumps(observed),flush=True)
  if line.startswith('observe '):
   request=int(line.split()[1]);second+=1;sequence+=5
   if phase=='quest':ticks-=10
   result={'schema':1,'request':request,'task':{'demo':'demo_task.ovl','village':'lobby_task.ovl','quest':'game_task.ovl'}[phase],
    'village_ready':phase=='village','on_quest':phase=='quest','map':'P_m04a07d.ovl','accepted_id':507,'active_id':507,
    'online':mode=='online' and phase=='quest','quest_cheats':False,'photo':False,'fast_forward':False,'fast_loading':False,
    'lock_on':True,'lock_target':123,'monsters':[{'address':123,'kind':5,'same_area':True,'health':2000}],
    'target_width':480,'target_height':272,'host_pid':os.getpid(),'quest_ticks':ticks,'vblanks':second*20,
    'perf_valid':True,'perf_second':second,'speed_percent':100,'game_fps':30,'presented_fps':30,
    'frame_sequence':sequence,'frame_ms':[33.0]*32,'guest_ms':4,'render_ms':6,'wait_ms':23,
    'pacing_ms':22,'gpu_valid':False,'gpu_ms':None}
   print('[debug] observation '+json.dumps(result),flush=True)
 offset=len(text)
 time.sleep(.01)
'''


class EncounterBenchmarkTests(unittest.TestCase):
    def test_frames_overlap_wrap_and_unknown(self):
        frames = bench.Frames({'frame_sequence': 7})
        frames.add({'frame_sequence': 9, 'frame_ms': [1, 2, 3, 4]})
        frames.add({'frame_sequence': 9, 'frame_ms': [99]})
        frames.add({'frame_sequence': 13, 'frame_ms': [5, 6, 7, 8]})
        self.assertEqual(frames.values, [3, 4, 5, 6, 7, 8])
        for bad in ({}, {'frame_sequence': True}, {'frame_sequence': -1}):
            with self.assertRaises(bench.checks.CheckFailed): bench.Frames(bad)
        for bad in ({'frame_sequence': 12, 'frame_ms': [1]}, {'frame_sequence': 99, 'frame_ms': [1]},
                    {'frame_sequence': 14, 'frame_ms': [0]}, {'frame_sequence': 14, 'frame_ms': [float('nan')]},
                    {'frame_sequence': 14, 'frame_ms': [True]}, {'frame_sequence': 14, 'frame_ms': None}):
            with self.assertRaises(bench.checks.CheckFailed): frames.add(bad)

    def test_complete_lines_and_schema(self):
        line = '[debug] encounter ' + json.dumps(state())
        self.assertEqual(bench.encounter_records(line), [])
        self.assertEqual(bench.encounter_records('other\n' + line + '\n' + line[:20]), [state()])
        for update in ({'schema': True}, {'schema': 2}, {'preset': 'unknown'}):
            with self.assertRaises(bench.checks.CheckFailed):
                bench.encounter_records('[debug] encounter ' + json.dumps({**state(), **update}) + '\n')

    def test_outcome_guards(self):
        expected = bench.live(sample(), state())
        for update in ({'online': True}, {'photo': True}, {'quest_cheats': True}, {'fast_forward': True},
                       {'fast_loading': True}, {'active_id': 508}, {'map': ''}, {'lock_target': 999},
                       {'lock_on': False}, {'monsters': []}, {'target_width': None}, {'host_pid': True}):
            with self.assertRaises(bench.checks.CheckFailed): bench.live({**sample(), **update}, state())
        for update in ({'phase': 'loading'}, {'model': 0}, {'actor': True}):
            with self.assertRaises(bench.checks.CheckFailed): bench.live(sample(), {**state(), **update})
        with self.assertRaises(bench.checks.CheckFailed): bench.live({**sample(), 'target_height': 544}, state(), expected)

    def test_shared_snapshot_waits_for_complete_observation(self):
        class Adapter:
            def __init__(self): self.output=''; self.reads=0
            def alive(self): return True
            def text(self):
                self.reads+=1
                return self.output + ('}\n' if self.reads>2 else '')
            def send(self, kind, value):
                self.output='[debug] observation {"schema":1,"request":'+value.split()[1]
        session=object.__new__(bench.suite.Session)
        session.device={'observations':True};session.deadline=time.monotonic()+5
        session.observation_ready=True;session.request=0;session.samples=[];session.adapter=Adapter()
        self.assertEqual(session.snapshot()['request'],1)
        self.assertGreater(session.adapter.reads,2)

    def test_owned_synthetic_session_and_failure_cleanup(self):
        for mode in ('', 'online', 'rtc', 'invisible'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                base = Path(temporary)
                (base/'game').mkdir(); (base/'saves').mkdir(); (base/'evidence').mkdir()
                for name in ('EBOOT.ELF', 'disc.iso'): (base/'game'/name).write_bytes(b'public synthetic input')
                (base/'saves'/'fixture').write_bytes(b'original fixture stays unchanged')
                (base/'fake.py').write_text(FAKE_GAME)
                (base/'settings.ini').write_text('video.frame_rate=30\nvideo.internal_scale=1\n')
                device = {'adapter': 'local', 'build': 'dev', 'observations': True, 'rtc_epoch_us': 1780000000000000,
                          'commit': 'a'*40, 'binary': sys.executable, 'args': [str(base/'fake.py'), mode],
                          'game_dir': str(base/'game'), 'saves': str(base/'saves'),
                          'settings': str(base/'settings.ini'),
                          'env': {'MHP3RD_NO_AUDIO': '1', 'MHP3RD_NO_SPARSE_VERTEX_DECODE': '1',
                                  'MHP3RD_NO_PLAIN_ALPHA_VARIANTS': '1'},
                          'suite_fixtures': {'load-character': [{'action': 'input', 'value': '0:pad b 4'}]}}
                sessions = []
                original = bench.suite.Session
                def session(*args):
                    result = original(*args); sessions.append(result); return result
                with mock.patch.object(bench.suite, 'Session', side_effect=session):
                    if mode:
                        with self.assertRaises(bench.checks.CheckFailed):
                            bench.run(device, base/'evidence', 20, 30, 89890)
                    else:
                        report = bench.run(device, base/'evidence', 20, 30, 89890)
                        self.assertEqual(report['status'], 'COLLECTED')
                        self.assertEqual(report['capture_review'], 'PENDING')
                        self.assertTrue(report['rtc_verified'])
                        self.assertEqual(report['performance_environment'], {'MHP3RD_NO_AUDIO': '1'})
                        self.assertEqual(report['measurement']['median_ms'], 33)
                        self.assertEqual(report['measurement']['present_frames'], 15)
                        self.assertTrue(all(row['gpu_ms'] is None for row in report['measurement']['seconds']))
                report = json.loads((base/'evidence/encounter-report.json').read_text())
                self.assertEqual(report['status'], 'FAIL' if mode else 'COLLECTED')
                self.assertFalse(sessions[0].started)
                self.assertIsNotNone(sessions[0].adapter.process.poll())
                self.assertEqual((base/'saves/fixture').read_bytes(), b'original fixture stays unchanged')

    def test_configuration_and_finite_cli(self):
        self.assertEqual(bench.timing_kind({'performance': 'alternate-alpha'}), 'alternating-diagnostic')
        self.assertEqual(bench.timing_kind({'env': {'MHP3RD_PERF_ALTERNATE': 'sparse'}}), 'alternating-diagnostic')
        self.assertEqual(bench.timing_kind({}), 'fixed')
        for device in ({'adapter': 'streaming'}, {'adapter': 'local', 'build': 'release'},
                       {'adapter': 'adb', 'build': 'dev', 'observations': True, 'rtc_epoch_us': 1}):
            with self.assertRaises((ValueError, bench.runner.NotTested)):
                bench.run(device, Path('unused'), 1, 30, 89890)
        with mock.patch('sys.stderr'):
            for argv in (['--seconds', '0'], ['--frames', '29'], ['--start-tick', '90000']):
                with self.assertRaises(SystemExit): bench.main(['--config', 'unused', '--device', 'x', *argv])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); config=root/'devices.json'
            device={'adapter':'local','build':'dev','commit':'a'*40}
            config.write_text(json.dumps({'devices':{'a':device,'b':device}}))
            report={'fixture_sha256':{},'game_sha256':'x','settings_sha256':'s','rtc_epoch_us':1,
                    'gpu':['synthetic'],'gpu_compatibility':['off'],'hardware':{},'language':'',
                    'source_commit':'a'*40,'measurement':{'target_size':[480,272],'median_ms':33,'p95_ms':34,'p99_ms':35}}
            order=[]
            def run(selected, evidence, *_):
                order.append(evidence.name); return copy.deepcopy(report)
            with mock.patch.object(bench,'run',side_effect=run):
                results=bench.main(['--config',str(config),'--device','b','--baseline','a','--repeat','2','--output-parent',temporary])
            self.assertEqual(order,['0-a','0-b','1-b','1-a'])
            self.assertEqual(len(results),4)
            config.write_text(json.dumps({'devices': {'a': device, 'b': {**device,
                'env': {'MHP3RD_PERF_ALTERNATE': 'alpha'}}}}))
            with mock.patch.object(bench, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'separately'):
                    bench.main(['--config', str(config), '--device', 'b', '--baseline', 'a',
                                '--output-parent', temporary])
                run.assert_not_called()
            config.write_text(json.dumps({'devices': {'a': device, 'b': device}}))
            count=0
            def mismatch(*_):
                nonlocal count
                count+=1;result=copy.deepcopy(report)
                if count>1: result['measurement']['target_size']=[960,544]
                return result
            with mock.patch.object(bench,'run',side_effect=mismatch), self.assertRaises(bench.checks.CheckFailed):
                bench.main(['--config',str(config),'--device','b','--repeat','2','--output-parent',temporary])


if __name__ == '__main__':
    unittest.main()
