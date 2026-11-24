# SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Bundle the latest Kate (built via Craft) with this plugin on Windows.
#
# Result: packaging\dist\kate-bundle-<ver>-windows-<arch>.zip containing:
#   kateai.dll        # this plugin (MSVC ABI, matches Craft Kate)
#   install.ps1       # installs kateai.dll next to Kate (see packaging\windows)
#   README.txt        # pairs with the latest Kate installer built by Craft
# plus, when produced: the Craft Kate installer (kate-*.exe) from C:\CraftRoot\tmp
#
# Usage: powershell -ExecutionPolicy Bypass -File packaging\bundle-kate-windows.ps1 `
#            [-Out <file>] [-SkipKate]
[CmdletBinding()]
param(
    [string] $Out = "",
    [switch] $SkipKate,
    [string] $CraftRoot = "C:\CraftRoot"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Root
$startedAt = Get-Date

# Craft's MSVC toolchain aborts when sh.exe or a foreign gcc sits in PATH,
# and the same pollution breaks its Visual Studio detection. The hosted
# runner ships Git's sh (Git\bin and Git\usr\bin, depending on image),
# MinGW and Strawberry Perl gcc. Strip those entries (keeping Git\cmd for
# git.exe); Craft brings its own toolchain and the runner's cmake/ninja
# stay on PATH.
$env:PATH = ($env:PATH -split ';' | Where-Object {
    $_ -and $_ -notmatch '(?i)Git\\(bin|usr\\bin)' -and $_ -notmatch '(?i)mingw|strawberry'
}) -join ';'

$m = Select-String -Path (Join-Path $Root "CMakeLists.txt") -Pattern 'project\(kateai VERSION ([^ ]+)' | Select-Object -First 1
$version = "0.8.0"
if ($m) { $version = $m.Matches[0].Groups[1].Value }
$arch = $env:PROCESSOR_ARCHITECTURE
if (-not $arch) { $arch = "amd64" }
$outWasProvided = -not [string]::IsNullOrWhiteSpace($Out)

# Let the CI-tested Windows plugin build own the Craft bootstrap, iconv
# workaround, the KF6/KTextEditor stack, the optional Kate build and the plugin
# build. The bundle job used to duplicate that setup and then failed before
# Craft was ready; one code path means the bundle and plugin artifacts are
# produced by the same recipe. -WithKate is what makes this a bundle build.
$artifactsDir = Join-Path $Root "artifacts-windows-tmp"
Write-Host "==> Building Kate and the plugin with packaging\ci\windows-build.ps1"
& (Join-Path $Root "packaging\ci\windows-build.ps1") -BuildType Release -ArtifactsDir $artifactsDir -CraftRoot $CraftRoot -WithKate
if (-not (Test-Path (Join-Path $artifactsDir "kateai.dll"))) {
    throw "The Windows build did not produce kateai.dll."
}

$craftEnv = Join-Path $CraftRoot "craft\craftenv.ps1"
if (-not (Test-Path $craftEnv)) {
    throw "KDE Craft not found at $CraftRoot."
}
# Craft's craftenv.ps1 sanitizes the environment by removing a fixed list of
# variables (including GITHUB_ACTION_REF). On some GitHub runners that
# variable is not defined, which makes Remove-Item throw and abort the build.
foreach ($name in @("GITHUB_ACTION_REF", "GITHUB_ACTION", "GITHUB_ACTION_PATH", "GITHUB_ACTION_REPOSITORY", "GITHUB_BASE_REF", "GITHUB_HEAD_REF", "GITHUB_REF", "GITHUB_SHA")) {
    if (-not (Test-Path "Env:$name")) {
        Set-Item "Env:$name" ""
    }
}
# windows-build.ps1 already made craftenv's Remove-Item calls tolerant while
# it ran its own Craft setup. Do not apply that textual replacement a second
# time here: the second pass adds another -ErrorAction to the same command,
# and PowerShell rejects the duplicated parameter while dot-sourcing.
$content = Get-Content $craftEnv -Raw
$content = $content.Replace('$python = (Get-Command "$env:KDEROOT/dev-utils/bin/python3" -ErrorAction SilentlyContinue).Source', '$python = $env:CRAFT_PYTHON')
Set-Content $craftEnv $content

# Dot-source: the env script must mutate THIS session (PATH, KDEROOT, craft/cs
# functions). `&` would run it in a child scope and lose them.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    . $craftEnv
} finally {
    $ErrorActionPreference = $prevEap
}

# craftenv.ps1 rebuilds the environment from Craft's setup JSON; keep Python
# logs streaming and stop git from waiting on credentials for this session too.
$env:PYTHONUNBUFFERED = "1"
$env:GIT_TERMINAL_PROMPT = "0"

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Force (Join-Path $Root "artifacts-windows-tmp\kateai.dll") (Join-Path $stage "kateai.dll")
Copy-Item -Force (Join-Path $Root "packaging\windows\install.ps1") (Join-Path $stage "install.ps1")
@"
Kate AI plugin for Kate on Windows (v$version).

Pairs with the latest Kate. Either:
  1. Install Kate via Craft (craft kate), or download it from
     https://download.kde.org/stable/kate/ — then run:
       powershell -ExecutionPolicy Bypass -File .\install.ps1
  2. Or copy kateai.dll next to Kate's kf6\ktexteditor plugin directory.
"@ | Out-File -Encoding utf8 (Join-Path $stage "README.txt")

# Pick up the Craft-built Kate installer. That plus this zip is the full
# "latest Kate + plugin" set for the release. Craft's package output has
# moved between Craft versions, so search the whole tmp tree rather than
# assuming the installer sits directly in CraftRoot\tmp.
$kateInstallers = @()
$tmpDir = Join-Path $CraftRoot "tmp"
$findKateInstaller = {
    if (-not (Test-Path $tmpDir)) {
        return @()
    }
    return @(Get-ChildItem -Path $tmpDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match '^kate-.*\.(exe|msi)$' -and $_.LastWriteTime -ge $startedAt.AddMinutes(-1)
        } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1)
}
if (Test-Path $tmpDir) {
    $found = & $findKateInstaller
    if (-not $found -or $found.Count -eq 0) {
        Write-Host "No Craft Kate installer found; building one with: craft --ci-mode --package kate"
        craft --ci-mode --package kate
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "craft --package kate failed with exit code $LASTEXITCODE"
        }
        $found = & $findKateInstaller
    }
    if ($found -and $found.Count -gt 0) {
        $kateInstallers = @($found)
    }
}

# Only call this a Kate bundle when the Kate installer is actually present.
# Otherwise publish a plainly named plugin zip: a 1 MB plugin-only archive
# under a "kate-bundle" name is what made the previous release look like it
# contained an editor it did not.
$distDir = Join-Path $Root "packaging\dist"
if (-not $outWasProvided) {
    if ($kateInstallers.Count -gt 0) {
        $Out = Join-Path $distDir "kate-bundle-$version-windows-$arch.zip"
    } else {
        $Out = Join-Path $distDir "kateai-$version-windows-$arch.zip"
        Write-Warning "No Kate installer was produced; publishing plugin-only zip $Out"
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out
Get-Item $Out
Remove-Item -Recurse -Force $stage
Remove-Item -Recurse -Force (Join-Path $Root "artifacts-windows-tmp")
foreach ($installer in $kateInstallers) {
    if ($installer) {
        Copy-Item -Force $installer.FullName (Join-Path (Split-Path $Out) $installer.Name)
        Get-Item (Join-Path (Split-Path $Out) $installer.Name)
    }
}
if ($kateInstallers.Count -gt 0) {
    Write-Host "Bundle ready: $Out (with $($kateInstallers[0].Name))"
} else {
    Write-Host "Plugin zip ready: $Out (no Kate installer was produced)"
}
