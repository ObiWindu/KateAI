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
$version = "0.4.0"
if ($m) { $version = $m.Matches[0].Groups[1].Value }
$arch = $env:PROCESSOR_ARCHITECTURE
if (-not $arch) { $arch = "amd64" }
if (-not $Out) {
    $Out = Join-Path $Root "packaging\dist\kate-bundle-$version-windows-$arch.zip"
}

# Let the CI-tested Windows plugin build own the Craft bootstrap, iconv
# workaround, `craft kate`, and the plugin build. The bundle job used to
# duplicate that setup and then failed before Craft was ready; one code path
# means the bundle and plugin artifacts are produced by the same recipe.
$artifactsDir = Join-Path $Root "artifacts-windows-tmp"
Write-Host "==> Building Kate and the plugin with packaging\ci\windows-build.ps1"
& (Join-Path $Root "packaging\ci\windows-build.ps1") -BuildType Release -ArtifactsDir $artifactsDir -CraftRoot $CraftRoot
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

# Also pick up a Craft-built Kate installer when present: that plus this zip
# is the full "latest Kate bundled with the plugin" set for the release.
$kateInstallers = @()
$tmpDir = Join-Path $CraftRoot "tmp"
if (Test-Path $tmpDir) {
    $kateInstallers = @(Get-ChildItem -Path $tmpDir -Filter "kate-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
    if (-not $kateInstallers -or $kateInstallers.Count -eq 0) {
        Write-Host "No Craft Kate installer in $tmpDir; building one with: craft --package kate"
        craft --package kate
        $kateInstallers = @(Get-ChildItem -Path $tmpDir -Filter "kate-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
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
Write-Host "Bundle ready: $Out"
