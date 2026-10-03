#!/usr/bin/env python3
"""Build the hash-pinned Cppcheck CLI locally, without Qt or upstream tests."""

import argparse
import hashlib
import subprocess
from pathlib import Path

VERSION = '2.17.1'
ARCHIVE_SHA256 = 'bfd681868248ec03855ca7c2aea7bcb1f39b8b18860d76aec805a92a967b966c'
URL = f'https://github.com/cppcheck-opensource/cppcheck/archive/refs/tags/{VERSION}.tar.gz'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / 'source.tar.gz'
    subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error',
                    '--max-time', '120', URL, '--output', str(archive)], check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise SystemExit('Cppcheck source checksum mismatch')
    subprocess.run(['tar', '-xzf', str(archive), '-C', str(directory)], check=True)
    build = directory / 'build'
    install = directory / 'install'
    subprocess.run(['cmake', '-S', str(directory / f'cppcheck-{VERSION}'), '-B', str(build),
                    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_GUI=OFF',
                    '-DBUILD_TESTS=OFF', '-DUSE_MATCHCOMPILER=Off',
                    f'-DCMAKE_INSTALL_PREFIX={install}'], check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'cppcheck', '-j2'], check=True)
    subprocess.run(['cmake', '--install', str(build)], check=True)
    subprocess.run([str(install / 'bin/cppcheck'), '--version'], check=True)


if __name__ == '__main__':
    main()
