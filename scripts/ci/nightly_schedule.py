#!/usr/bin/env python3
"""Plan trusted nightly runs using public release metadata, without build secrets."""

import hashlib
import json
import os
import re
import sys
from urllib.error import HTTPError
from urllib.request import Request, urlopen

REPOSITORY = 'TeamGDB/Yakumo'
CHANNEL = 'TeamGDB/Yakumo-nightly'
LATEST = 'https://api.github.com/repos/' + CHANNEL + '/releases/latest'
MAX_JSON = 128 * 1024
SUFFIXES = {'linux-x86_64': '.tar.gz', 'macos-arm64': '.zip',
            'windows-x86_64': '-portable.zip', 'android-arm64': '.apk'}


def require(condition):
    if not condition:
        raise ValueError('Invalid nightly planning metadata.')


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result)
        result[key] = value
    return result


def read_public(url):
    headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'Yakumo-nightly-scheduler'}
    # Use the source repository's read-only Actions token for the public API
    # quota; release downloads receive no authentication or build credentials.
    token = os.environ.get('NIGHTLY_METADATA_TOKEN')
    if url == LATEST and token:
        headers['Authorization'] = 'Bearer ' + token
    request = Request(url, headers=headers)
    with urlopen(request, timeout=30) as response:
        data = response.read(MAX_JSON + 1)
    require(0 < len(data) <= MAX_JSON)
    return data


def published_source(release, data):
    require(type(release) is dict and release.get('draft') is False and release.get('prerelease') is False)
    record = json.loads(data, object_pairs_hook=unique)
    require(type(record) is dict and type(record.get('schema')) is int and record['schema'] == 1)
    source = record.get('source')
    require(type(source) is str and re.fullmatch('[0-9a-f]{40}', source))
    tag = release.get('tag_name')
    require(type(tag) is str and re.fullmatch('nightly-[0-9]{8}-' + source[:12] + '-[1-9][0-9]*-[1-9][0-9]*', tag))
    assets = release.get('assets')
    require(type(assets) is list and len(assets) == 8)
    by_name = {item['name']: item for item in assets}
    require(len(by_name) == 8)
    packages = {'yakumo-nightly-' + source[:12] + '-' + platform + suffix for platform, suffix in SUFFIXES.items()}
    sources = set(by_name) - packages - {'build.json', 'SHA256SUMS'}
    require(len(sources) == 2 and all(re.fullmatch(r'ffmpeg-[0-9a-f.]+\.tar\.(?:gz|xz)', name) for name in sources))
    require(set(by_name) == packages | sources | {'build.json', 'SHA256SUMS'})
    manifest = by_name['build.json']
    require(manifest['size'] == len(data) and manifest['digest'] == 'sha256:' + hashlib.sha256(data).hexdigest())
    require(manifest['browser_download_url'] == 'https://github.com/' + CHANNEL + '/releases/download/' + tag + '/build.json')
    files = record.get('files')
    require(type(files) is list and len(files) == 6 and {entry['name'] for entry in files} == packages | sources)
    for entry in files:
        item = by_name[entry['name']]
        require(type(entry['size']) is int and 0 < entry['size'] <= 2 * 1024 ** 3)
        require(type(entry['sha256']) is str and re.fullmatch('[0-9a-f]{64}', entry['sha256']))
        require(item['state'] == 'uploaded' and item['size'] == entry['size'] and item['digest'] == 'sha256:' + entry['sha256'])
    require(all(item['state'] == 'uploaded' for item in assets))
    platforms = record.get('platforms')
    require(type(platforms) is list and len(platforms) == 4 and {p['platform'] for p in platforms} == set(SUFFIXES))
    require(all(p['source'] == source for p in platforms))
    return source


def latest_source():
    try:
        release = json.loads(read_public(LATEST), object_pairs_hook=unique)
    except HTTPError as error:
        error.close()
        if error.code == 404:
            return None
        raise
    assets = release['assets']
    require(type(assets) is list)
    manifests = [item for item in assets if item['name'] == 'build.json']
    require(len(manifests) == 1)
    tag = release['tag_name']
    require(type(tag) is str and re.fullmatch(r'nightly-[0-9]{8}-[0-9a-f]{12}-[1-9][0-9]*-[1-9][0-9]*', tag))
    url = 'https://github.com/' + CHANNEL + '/releases/download/' + tag + '/build.json'
    require(manifests[0]['browser_download_url'] == url)
    return published_source(release, read_public(url))


def plan(environment):
    require(environment['GITHUB_REPOSITORY'] == REPOSITORY and environment['GITHUB_REF'] == 'refs/heads/main')
    source = environment['GITHUB_SHA']
    require(re.fullmatch('[0-9a-f]{40}', source))
    event = environment['GITHUB_EVENT_NAME']
    require(event in ('schedule', 'workflow_dispatch'))
    if event == 'schedule':
        return source != latest_source(), True
    publish = environment['NIGHTLY_PUBLISH']
    require(publish in ('true', 'false'))
    return True, publish == 'true'


def main():
    try:
        build, publish = plan(os.environ)
        print('build=' + str(build).lower() + '\npublish=' + str(publish).lower())
        print('Nightly build requested.' if build else 'This main commit already has a complete nightly; skipped.', file=sys.stderr)
        return 0
    except Exception:
        print('Nightly planning failed; credentialed jobs remain blocked.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
