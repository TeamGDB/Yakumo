"""Public contracts for nightly scheduling, deduplication and credential guards."""

import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import re
import unittest
from unittest import mock
from urllib.error import HTTPError

import nightly_schedule as schedule

ROOT = Path(__file__).resolve().parents[2]
SOURCE = '0123456789abcdef0123456789abcdef01234567'


def fixture():
    files = [{'name': 'yakumo-nightly-' + SOURCE[:12] + '-' + platform + suffix,
              'size': 7, 'sha256': hashlib.sha256(platform.encode()).hexdigest()}
             for platform, suffix in schedule.SUFFIXES.items()]
    files += [{'name': name, 'size': 7, 'sha256': hashlib.sha256(name.encode()).hexdigest()}
              for name in ('ffmpeg-7.1.5.tar.xz', 'ffmpeg-' + 'f' * 40 + '.tar.gz')]
    record = {'schema': 1, 'source': SOURCE, 'files': files,
              'platforms': [{'platform': p, 'source': SOURCE} for p in schedule.SUFFIXES]}
    data = json.dumps(record).encode()
    tag = 'nightly-20261018-' + SOURCE[:12] + '-1234-1'
    assets = [{'name': f['name'], 'size': f['size'], 'digest': 'sha256:' + f['sha256'], 'state': 'uploaded'} for f in files]
    assets += [{'name': 'SHA256SUMS', 'size': 1, 'digest': 'sha256:' + 'a' * 64, 'state': 'uploaded'},
               {'name': 'build.json', 'size': len(data), 'digest': 'sha256:' + hashlib.sha256(data).hexdigest(),
                'state': 'uploaded', 'browser_download_url': 'https://github.com/' + schedule.CHANNEL
                + '/releases/download/' + tag + '/build.json'}]
    return {'draft': False, 'prerelease': False, 'tag_name': tag, 'assets': assets}, data


def environment(event='schedule', **updates):
    return {'GITHUB_REPOSITORY': schedule.REPOSITORY, 'GITHUB_REF': 'refs/heads/main',
            'GITHUB_SHA': SOURCE, 'GITHUB_EVENT_NAME': event, 'NIGHTLY_PUBLISH': 'true', **updates}


class ReleaseMetadataTests(unittest.TestCase):
    def test_complete_published_four_platform_nightly_returns_its_source(self):
        self.assertEqual(schedule.published_source(*fixture()), SOURCE)

    def test_incomplete_wrong_or_ambiguous_release_cannot_suppress_a_build(self):
        release, data = fixture()
        mutations = [lambda r: r.update(draft=True), lambda r: r.update(prerelease=True),
                     lambda r: r.update(tag_name='not-a-nightly'), lambda r: r['assets'].pop(),
                     lambda r: r['assets'].append(r['assets'][0]),
                     lambda r: r['assets'][0].update(name='unexpected-file'),
                     lambda r: r['assets'][0].update(state='new'),
                     lambda r: r['assets'][0].update(size=8),
                     lambda r: r['assets'][0].update(digest='sha256:' + '0' * 64),
                     lambda r: r['assets'][-1].update(size=1),
                     lambda r: r['assets'][-1].update(digest='sha256:' + '0' * 64),
                     lambda r: r['assets'][-1].update(browser_download_url='https://example.invalid/build.json')]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                candidate = copy.deepcopy(release)
                mutate(candidate)
                with self.assertRaises(ValueError):
                    schedule.published_source(candidate, data)
        record = json.loads(data)
        for change in [{'schema': True}, {'source': 'bad'}, {'files': []}, {'platforms': []},
                       {'platforms': [{**p, 'source': 'f' * 40} for p in record['platforms']]}]:
            with self.subTest(change=change):
                with self.assertRaises(ValueError):
                    schedule.published_source(release, json.dumps({**record, **change}).encode())
        with self.assertRaises(ValueError):
            schedule.published_source(release, b'{"source":"a","source":"b"}')

    def test_latest_metadata_is_loaded_only_from_the_fixed_public_channel(self):
        release, data = fixture()
        with mock.patch.object(schedule, 'read_public', side_effect=[json.dumps(release).encode(), data]) as read:
            self.assertEqual(schedule.latest_source(), SOURCE)
        self.assertEqual([c.args[0] for c in read.call_args_list],
                         [schedule.LATEST, release['assets'][-1]['browser_download_url']])

    def test_first_release_is_eligible_but_api_failure_blocks_planning(self):
        for code in [404, 403, 500]:
            with self.subTest(code=code), mock.patch.object(schedule, 'read_public',
                    side_effect=HTTPError(schedule.LATEST, code, 'public fixture', None, io.BytesIO())):
                if code == 404:
                    self.assertIsNone(schedule.latest_source())
                else:
                    with self.assertRaises(HTTPError):
                        schedule.latest_source()

    def test_redirected_missing_or_duplicate_manifest_is_rejected_before_download(self):
        release, _ = fixture()
        for change in ['missing', 'duplicate', 'url', 'tag']:
            with self.subTest(change=change):
                candidate = copy.deepcopy(release)
                if change == 'missing':
                    candidate['assets'].pop()
                elif change == 'duplicate':
                    candidate['assets'].append(candidate['assets'][-1])
                elif change == 'url':
                    candidate['assets'][-1]['browser_download_url'] = 'https://example.invalid/private'
                else:
                    candidate['tag_name'] = '../unsafe'
                with mock.patch.object(schedule, 'read_public', return_value=json.dumps(candidate).encode()) as read:
                    with self.assertRaises(ValueError):
                        schedule.latest_source()
                    self.assertEqual(read.call_count, 1)

    def test_transport_is_bounded_and_authenticates_only_the_github_api(self):
        for url, auth in [(schedule.LATEST, 'Bearer PUBLIC-TOKEN-FIXTURE'),
                          ('https://github.com/' + schedule.CHANNEL + '/releases/download/tag/build.json', None)]:
            with self.subTest(url=url), mock.patch.dict(os.environ, {'NIGHTLY_METADATA_TOKEN': 'PUBLIC-TOKEN-FIXTURE'}):
                with mock.patch.object(schedule, 'urlopen', return_value=io.BytesIO(b'{}')) as open_url:
                    self.assertEqual(schedule.read_public(url), b'{}')
                request = open_url.call_args.args[0]
                self.assertEqual(request.full_url, url)
                self.assertEqual(request.get_header('Authorization'), auth)
                self.assertEqual(open_url.call_args.kwargs['timeout'], 30)
        for data in [b'', b'x' * (schedule.MAX_JSON + 1)]:
            with mock.patch.object(schedule, 'urlopen', return_value=io.BytesIO(data)):
                with self.assertRaises(ValueError):
                    schedule.read_public(schedule.LATEST)


class PlanningTests(unittest.TestCase):
    def test_scheduled_builds_publish_and_skip_only_the_already_published_commit(self):
        for previous, expected in [(None, True), ('f' * 40, True), (SOURCE, False)]:
            with self.subTest(previous=previous), mock.patch.object(schedule, 'latest_source', return_value=previous):
                self.assertEqual(schedule.plan(environment(NIGHTLY_PUBLISH='')), (expected, True))

    def test_manual_runs_can_force_a_rebuild_or_request_build_only(self):
        with mock.patch.object(schedule, 'latest_source', side_effect=AssertionError('manual runs must not query releases')):
            for value in ['true', 'false']:
                self.assertEqual(schedule.plan(environment('workflow_dispatch', NIGHTLY_PUBLISH=value)),
                                 (True, value == 'true'))
            with self.assertRaises(ValueError):
                schedule.plan(environment('workflow_dispatch', NIGHTLY_PUBLISH='yes'))

    def test_untrusted_contexts_fail_before_any_release_lookup(self):
        for change in [{'GITHUB_REPOSITORY': 'other/Yakumo'}, {'GITHUB_REF': 'refs/heads/feature'},
                       {'GITHUB_REF': 'refs/tags/main'}, {'GITHUB_SHA': 'bad'},
                       {'GITHUB_EVENT_NAME': 'pull_request'}, {'GITHUB_EVENT_NAME': 'pull_request_target'},
                       {'GITHUB_EVENT_NAME': 'workflow_run'}]:
            with self.subTest(change=change), mock.patch.object(schedule, 'latest_source') as latest:
                with self.assertRaises(ValueError):
                    schedule.plan(environment(**change))
                latest.assert_not_called()

    def test_cli_writes_only_fixed_boolean_outputs_and_keeps_failures_generic(self):
        for decision in [(True, True), (False, True), (True, False)]:
            output = io.StringIO()
            with mock.patch.object(schedule, 'plan', return_value=decision), \
                 contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(schedule.main(), 0)
            self.assertEqual(output.getvalue(), 'build=' + str(decision[0]).lower()
                             + '\npublish=' + str(decision[1]).lower() + '\n')
        output, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(schedule, 'plan', side_effect=RuntimeError('PRIVATE-ERROR-CANARY')), \
             contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(output):
            self.assertEqual(schedule.main(), 1)
        self.assertNotIn('PRIVATE-ERROR-CANARY', stderr.getvalue())
        self.assertEqual(output.getvalue(), '')


class WorkflowPolicyTests(unittest.TestCase):
    def setUp(self):
        self.workflow = (ROOT / '.github/workflows/nightly.yml').read_text()

    def job(self, name):
        return re.search(r'^  ' + name + r':\n(.*?)(?=^  [a-z]+:|\Z)', self.workflow, re.M | re.S)[1]

    def evaluate(self, expression, event, repository=schedule.REPOSITORY, ref='refs/heads/main', build='true', publish='true'):
        values = {'github.event_name': event, 'github.repository': repository, 'github.ref': ref,
                  'needs.plan.outputs.build': build, 'needs.plan.outputs.publish': publish}
        expression = re.sub(r'\b(?:github|needs)\.[a-z_.]+', lambda match: repr(values[match[0]]), expression)
        return eval(expression.replace('&&', ' and ').replace('||', ' or '), {'__builtins__': {}})

    def test_every_credentialed_job_allows_only_main_schedule_or_manual_events(self):
        for name in ['plan', 'build', 'sign', 'publish']:
            expression = re.search(r'^    if: (.*)$', self.job(name), re.M)[1]
            for event in ['schedule', 'workflow_dispatch', 'pull_request', 'pull_request_target', 'workflow_run']:
                self.assertEqual(bool(self.evaluate(expression, event)), event in ('schedule', 'workflow_dispatch'))
                self.assertFalse(self.evaluate(expression, event, repository='fork/Yakumo'))
                self.assertFalse(self.evaluate(expression, event, ref='refs/heads/feature'))
                self.assertFalse(self.evaluate(expression, event, ref='refs/tags/main'))
        self.assertIn('needs: plan', self.job('build'))
        self.assertIn('needs: [plan, build, sign]', self.job('publish'))
        self.assertIn('needs: build', self.job('sign'))
        self.assertFalse(self.evaluate(re.search(r'^    if: (.*)$', self.job('build'), re.M)[1], 'schedule', build='false'))
        self.assertFalse(self.evaluate(re.search(r'^    if: (.*)$', self.job('publish'), re.M)[1], 'workflow_dispatch', publish='false'))

    def test_cron_concurrency_and_release_status_match_the_automatic_nightly_contract(self):
        self.assertIn("cron: '0 0 * * *'", self.workflow)
        self.assertIn('default: true', self.workflow)
        expression = re.search(r'^  group: nightly-\$\{\{ (.*) \}\}$', self.workflow, re.M)[1]
        self.assertEqual(self.evaluate(expression, 'schedule'), self.evaluate(expression, 'workflow_dispatch'))
        self.assertNotEqual(self.evaluate(expression, 'pull_request', ref='refs/pull/1/merge'),
                            self.evaluate(expression, 'pull_request', ref='refs/pull/2/merge'))
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}", self.workflow)
        self.assertNotIn('environment:', self.job('plan'))
        self.assertNotRegex(self.job('plan'), r'\$\{\{[^\n]*\bsecrets\.')
        self.assertIn('gh release create', self.job('publish'))
        self.assertIn('--latest --title', self.job('publish'))
        self.assertNotIn('--prerelease', self.job('publish'))


if __name__ == '__main__':
    unittest.main()
