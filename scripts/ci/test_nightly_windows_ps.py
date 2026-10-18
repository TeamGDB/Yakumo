"""Execute the production PowerShell Bash locator against public fake layouts."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which('pwsh')
HARNESS = r'''
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($env:LOCATOR_SOURCE, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Production Windows wrapper failed PowerShell parsing.' }
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Find-GitBash'
}, $true)
if ($null -eq $definition) { throw 'Production Bash locator is missing.' }
. ([ScriptBlock]::Create($definition.Extent.Text))
$roots = @(ConvertFrom-Json $env:LOCATOR_ROOTS)
$found = Find-GitBash -GitPath $env:LOCATOR_GIT -ProgramRoots $roots
@{ found = $found } | ConvertTo-Json -Compress
'''


@unittest.skipUnless(POWERSHELL, 'PowerShell 7 is required; the Windows hosted unit job runs this contract')
class PowerShellBashLocatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='yakumo public Git layout ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def file(self, path):
        result = self.root / path
        result.parent.mkdir(parents=True, exist_ok=True)
        result.touch()
        return result

    def locate(self, git, roots=None):
        result = subprocess.run([POWERSHELL, '-NoProfile', '-NonInteractive', '-Command', HARNESS],
                                env={**os.environ, 'LOCATOR_SOURCE': str(ROOT / 'scripts/ci/nightly_windows.ps1'),
                                     'LOCATOR_GIT': str(git), 'LOCATOR_ROOTS': json.dumps([str(p) for p in roots or []])},
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20, check=True)
        found = json.loads(result.stdout.decode('utf-8'))['found']
        return Path(found) if found else None

    def test_git_cmd_bin_and_mingw64_layouts_find_the_same_native_bash(self):
        for name in ['cmd/git.exe', 'bin/git.exe', 'mingw64/bin/git.exe']:
            with self.subTest(layout=name):
                git = self.file('Git with spaces/' + name)
                bash = self.file('Git with spaces/bin/bash.exe')
                self.assertEqual(self.locate(git), bash)

    def test_visual_studio_bundled_git_falls_back_to_known_git_installation(self):
        git = self.file('VS/Common7/IDE/TeamExplorer/Git/cmd/git.exe')
        programs = self.root / 'Program Files'
        bash = self.file('Program Files/Git/bin/bash.exe')
        self.assertEqual(self.locate(git, [programs]), bash)

    def test_usr_bin_native_bash_is_accepted_but_directories_are_not_executables(self):
        git = self.file('Portable Git/mingw64/bin/git.exe')
        (self.root / 'Portable Git/bin/bash.exe').mkdir(parents=True)
        bash = self.file('Portable Git/usr/bin/bash.exe')
        self.assertEqual(self.locate(git), bash)

    def test_search_is_bounded_and_does_not_choose_windows_wsl_bash(self):
        git = self.file('deep/a/b/c/d/git.exe')
        self.file('deep/bin/bash.exe')  # Outside the four inspected ancestors.
        self.assertIsNone(self.locate(git))
        windows_git = self.file('Windows/System32/git.exe')
        self.file('Windows/System32/bash.exe')  # WSL launcher, not Git Bash.
        self.assertIsNone(self.locate(windows_git, [self.root / 'Windows']))

    def test_missing_installation_returns_no_candidate(self):
        git = self.file('missing/cmd/git.exe')
        self.assertIsNone(self.locate(git, [self.root / 'missing Program Files']))


if __name__ == '__main__':
    unittest.main()
