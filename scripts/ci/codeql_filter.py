"""Drops CodeQL results located in vendored third-party sources from SARIF files.

CodeQL's paths-ignore does not apply to C and C++, whose headers are analysed
wherever first-party code includes them. The vendored libraries are
unmodified upstream sources (docs/SOURCE_PROVENANCE.md) and their findings
belong upstream, so results whose primary location is in them are removed
before upload, as paths-ignore does for the other languages.

    codeql_filter.py SARIF_DIRECTORY [PREFIX ...]

SARIF_DIRECTORY must lie inside the working directory (the checkout in CI).
"""

import json
import os
import sys

VENDORED = ('third_party/asmjit/', 'profiles/mhp3rd/third_party/luau/')


def located_in(result, prefixes):
    for location in result.get('locations', [])[:1]:
        uri = location.get('physicalLocation', {}).get('artifactLocation', {}).get('uri', '')
        uri = uri.removeprefix('file://')
        if any(uri.startswith(prefix) or ('/' + prefix) in uri for prefix in prefixes):
            return True
    return False


def filter_sarif(document, prefixes=VENDORED):
    """Returns how many results were removed from `document`, in place."""
    removed = 0
    for run in document.get('runs', []):
        results = run.get('results', [])
        kept = [result for result in results if not located_in(result, prefixes)]
        removed += len(results) - len(kept)
        run['results'] = kept
    return removed


def main(argv):
    if not argv:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    base = os.path.realpath(os.getcwd())
    directory = os.path.realpath(argv[0])
    if not directory.startswith(base + os.sep):
        print('%s is not inside the working directory' % argv[0], file=sys.stderr)
        return 2
    prefixes = tuple(argv[1:]) or VENDORED
    for name in sorted(os.listdir(directory)):
        path = os.path.realpath(os.path.join(directory, name))
        if not name.endswith('.sarif') or not path.startswith(base + os.sep):
            continue
        with open(path, encoding='utf-8') as source:
            document = json.load(source)
        removed = filter_sarif(document, prefixes)
        with open(path, 'w', encoding='utf-8') as target:
            json.dump(document, target)
        print('%s: %d vendored result(s) removed' % (name, removed))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
