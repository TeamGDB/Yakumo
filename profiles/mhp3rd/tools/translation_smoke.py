#!/usr/bin/env python3
"""Prepare and launch an isolated, bounded quest translation visual test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--data-dir', type=Path, required=True, help='Read-only save/settings source')
    parser.add_argument('--overlays', type=Path, required=True)
    parser.add_argument('--translation', type=Path, required=True)
    parser.add_argument('--language', required=True, help='Language code declared by the .lang file')
    parser.add_argument('--font', type=Path, help='Optional local font used by the build')
    parser.add_argument('--commit', required=True, help='Source commit used to build the executable')
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--output-parent', type=Path)
    parser.add_argument('--unlimited', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600:
        parser.error('--seconds must be between 1 and 600')
    for source in [args.binary, args.translation, args.game_dir / 'EBOOT.ELF', args.game_dir / 'disc.iso']:
        if not source.is_file():
            parser.error(f'Missing input: {source}')
    if not args.overlays.is_dir() or not (args.data_dir / 'ms0').is_dir():
        parser.error('An overlay directory and a save source containing ms0 are required')
    root = Path(tempfile.mkdtemp(prefix='yakumo-quest-translation-', dir=args.output_parent))
    data = root / 'data'
    data.mkdir()
    shutil.copytree(args.data_dir / 'ms0', data / 'ms0')
    if (args.data_dir / 'settings.ini').is_file():
        shutil.copy2(args.data_dir / 'settings.ini', data / 'settings.ini')
    for name in ['EBOOT.ELF', 'disc.iso']:
        (data / name).symlink_to((args.game_dir / name).resolve())
    translations = root / 'translations'
    translations.mkdir()
    shutil.copy2(args.translation, translations / args.translation.name)
    live = root / 'input-live.txt'
    live.write_text('', encoding='utf-8')
    shots = root / 'shots'
    shots.mkdir()
    env = {key: value for key, value in os.environ.items() if not key.startswith('MHP3RD_')}
    env.update(MHP3RD_GAME_DIR=str(data), MHP3RD_DATA_DIR=str(data),
               MHP3RD_OVERLAY_DIR=str(args.overlays.resolve()),
               MHP3RD_TRANSLATIONS_DIR=str(translations), MHP3RD_LANGUAGE=args.language,
               MHP3RD_INPUT_LIVE=str(live), MHP3RD_SCREENSHOT_DIR=str(shots),
               MHP3RD_SCREENSHOT_EVERY='0', MHP3RD_PERF='log', MHP3RD_TRACE_TEXT='1',
               MHP3RD_WINDOW_TITLE='Yakumo quest translation regression')
    if args.font:
        if not args.font.is_file():
            parser.error('The font must be an existing local file')
        env['MHP3RD_FONT'] = str(args.font.resolve())
    if args.unlimited:
        env['MHP3RD_TEXT_SEARCH_UNLIMITED'] = '1'
    review = '# Quest translation visual review\n\n'
    review += f'Commit: {args.commit}\n\nMode: {"unlimited diagnostic" if args.unlimited else "normal"}\n\n'
    review += '| Capture | Verdict | Quest / expected / observed |\n| --- | --- | --- |\n'
    for step in ['01-menu', '02-village-list', '03-village-details', '04-reopen',
                 '05-hall-list', '06-hall-details', '07-active-quest', '08-original']:
        review += f'| {step} | NOT TESTED | |\n'
    (root / 'review.md').write_text(review, encoding='utf-8')
    metadata = {'commit': args.commit, 'binary_sha256': digest(args.binary),
                'translation_sha256': digest(args.translation), 'language': args.language,
                'unlimited': args.unlimited, 'deadline_seconds': args.seconds,
                'visual_verdict': 'NOT TESTED'}
    print(f'Local run directory: {root}', flush=True)
    started = time.monotonic()
    with (root / 'run.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(args.binary.resolve())], cwd=args.binary.resolve().parent,
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        metadata['pid'] = process.pid
        (root / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        try:
            metadata['returncode'] = process.wait(timeout=args.seconds)
            metadata['timed_out'] = False
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            metadata['timed_out'] = isinstance(error, subprocess.TimeoutExpired)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            metadata['returncode'] = process.returncode
        finally:
            metadata['duration_seconds'] = round(time.monotonic() - started, 3)
            metadata['captures'] = sorted(path.name for path in shots.glob('*.bmp'))
            (root / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    print('Review screenshots and fill review.md; the launcher does not infer a visual pass.')


if __name__ == '__main__':
    main()
