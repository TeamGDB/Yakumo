# Run only inside nightly.py build-platform: compiler output and paths are private.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Find-GitBash {
    param([string]$GitPath, [string[]]$ProgramRoots)

    # Git can be found through cmd, bin or mingw64/bin. VsDevCmd may instead
    # prepend Visual Studio's bundled Git, which does not include Bash. Inspect
    # a bounded set of Git-relative paths and known installation roots only.
    $candidates = @()
    $parent = Split-Path $GitPath -Parent
    for ($depth = 0; $depth -lt 4 -and $parent; $depth++) {
        $candidates += Join-Path $parent 'bin/bash.exe'
        $candidates += Join-Path $parent 'usr/bin/bash.exe'
        $parent = Split-Path $parent -Parent
    }
    foreach ($root in $ProgramRoots) {
        if ($root) {
            $candidates += Join-Path $root 'Git/bin/bash.exe'
            $candidates += Join-Path $root 'Git/usr/bin/bash.exe'
        }
    }
    return $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

if (-not $IsWindows -or [Environment]::Is64BitProcess -ne $true) {
    throw 'The Windows nightly requires a native x64 Windows runner.'
}
foreach ($name in @('YAKUMO_PRIVATE_INPUTS', 'YAKUMO_WORK', 'NIGHTLY_VERSION')) {
    if (-not [Environment]::GetEnvironmentVariable($name)) {
        throw "Missing required nightly setting: $name"
    }
}

# Use the runner's installed VS component, not an action that modifies PATH or
# downloads a mutable compiler. Fail if the cache's MSVC minor is unavailable.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio discovery tool is unavailable.'
}
$installation = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw 'Visual Studio 2022 x64 build tools are unavailable.'
}
$devCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$settings = & $env:ComSpec /d /s /c "call `"$devCommand`" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver=14.44 >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw 'MSVC 14.44 environment initialization failed.'
}
foreach ($line in $settings) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
if ($env:VCToolsVersion -notmatch '^14\.44\.') {
    throw 'The overlay cache requires MSVC 14.44.'
}
$env:VSLANG = '1033'

# Repository shell/CMake generators invoke python3. The official Python Windows
# distribution provides python.exe; a private shim avoids the Store alias and
# preserves that interpreter's standard library/venv rather than copying it.
$python = if ($env:YAKUMO_PYTHON) { $env:YAKUMO_PYTHON } else { (Get-Command python.exe).Source }
$python = (Resolve-Path -LiteralPath $python).Path
if ($python.Contains('"') -or $python.Contains("`r") -or $python.Contains("`n")) {
    throw 'Invalid Python interpreter path.'
}
$shimDirectory = Join-Path $env:YAKUMO_WORK 'tools'
New-Item -ItemType Directory -Path $shimDirectory -Force | Out-Null
Set-Content -LiteralPath (Join-Path $shimDirectory 'python3.cmd') `
    -Value "@`"$python`" %*" -Encoding ascii
$shellPython = $python.Replace('\', '/').Replace("'", ("'" + '"' + "'" + '"' + "'"))
[IO.File]::WriteAllText((Join-Path $shimDirectory 'python3'), `
    "#!/bin/sh`nexec '$shellPython' `"`$@`"`n", [Text.UTF8Encoding]::new($false))
$env:PATH = "$shimDirectory;$env:PATH"
$env:YAKUMO_PYTHON = $python

# Public PR probes prepare only independently generated fixtures after MSVC is
# initialized. Trusted real dispatches retrieve approved inputs before running
# this wrapper and do not set NIGHTLY_PROBE.
if ($env:NIGHTLY_PROBE -eq '1') {
    & $python (Join-Path $PSScriptRoot 'nightly_probe.py') --platform windows-x86_64 `
        --output $env:YAKUMO_PRIVATE_INPUTS --install
    if ($LASTEXITCODE -ne 0) {
        throw 'Public Windows probe preparation failed.'
    }
}

$git = (Get-Command git.exe).Source
$bash = Find-GitBash -GitPath $git -ProgramRoots @($env:ProgramFiles, ${env:ProgramFiles(x86)})
if (-not $bash) {
    throw 'Git for Windows Bash is unavailable.'
}
# Git Bash starts without profile files and inherits the verified MSVC tools.
$script = Join-Path $PSScriptRoot 'nightly_windows.sh'
& $bash --noprofile --norc $script
if ($LASTEXITCODE -ne 0) {
    throw 'The private Windows build failed.'
}
