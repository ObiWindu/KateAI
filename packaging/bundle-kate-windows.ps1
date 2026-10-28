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
$version = "0.1.0"
if ($m) { $version = $m.Matches[0].Groups[1].Value }
$arch = $env:PROCESSOR_ARCHITECTURE
if (-not $arch) { $arch = "amd64" }
if (-not $Out) {
    $Out = Join-Path $Root "packaging\dist\kate-bundle-$version-windows-$arch.zip"
}

$craftEnv = Join-Path $CraftRoot "craft\craftenv.ps1"
if (-not (Test-Path $craftEnv)) {
    # Each CI job runs on a fresh VM, so the Craft stack built by
    # windows-build.ps1 is not here: bootstrap it on demand.
    Write-Host "==> Bootstrapping KDE Craft (accepting defaults)"
    # The hosted runner already enforces Bypass via a higher-precedence
    # scope, which makes this assignment throw: not fatal, Bypass is
    # exactly what we need to run the installer below.
    try {
        Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force
    } catch {
        Write-Host "Execution policy is host-managed; continuing with the effective Bypass policy."
    }
    python.exe -m pip install --upgrade certifi
    # -root skips the install-path prompt; -use-defaults is forwarded to
    # CraftBootstrap.py (arch/compiler auto-detected). Downloaded first so
    # the exact command line shows in the log instead of hiding in iex.
    $installer = Join-Path ([IO.Path]::GetTempPath()) "install_craft.ps1"
    (New-Object net.webclient).DownloadFile(
        'https://raw.githubusercontent.com/KDE/craft/master/setup/install_craft.ps1', $installer)
    if ($env:CI -eq 'true' -and (Test-Path $CraftRoot)) {
        # The installer loops forever on a non-empty target dir when there
        # is no console to answer its prompt; CI VMs are disposable.
        Write-Host "Removing pre-existing $CraftRoot for a clean Craft bootstrap."
        Remove-Item -Recurse -Force $CraftRoot
    }
    & $installer -root $CraftRoot -use-defaults
}
if (-not (Test-Path $craftEnv)) {
    throw "KDE Craft not found at $CraftRoot. Install it first, see https://develop.kde.org/docs/getting-started/building/craft/"
}
# Craft's craftenv.ps1 sanitizes the environment by removing a fixed list of
# variables (including GITHUB_ACTION_REF). On some GitHub runners that
# variable is not defined, which makes Remove-Item throw and abort the build.
foreach ($name in @("GITHUB_ACTION_REF", "GITHUB_ACTION", "GITHUB_ACTION_PATH", "GITHUB_ACTION_REPOSITORY", "GITHUB_BASE_REF", "GITHUB_HEAD_REF", "GITHUB_REF", "GITHUB_SHA")) {
    if (-not (Test-Path "Env:$name")) {
        Set-Item "Env:$name" ""
    }
}
if (Test-Path $craftEnv) {
    $content = Get-Content $craftEnv -Raw
    $content = $content.Replace('Remove-Item ("ENV:{0}" -f ${_}.Name)', 'Remove-Item ("ENV:{0}" -f ${_}.Name) -ErrorAction SilentlyContinue')
    $content = $content.Replace('$python = (Get-Command "$env:KDEROOT/dev-utils/bin/python3" -ErrorAction SilentlyContinue).Source', '$python = $env:CRAFT_PYTHON')
    Set-Content $craftEnv $content
}
# Dot-source: the env script must mutate THIS session (PATH, KDEROOT, craft/cs
# functions). `&` would run it in a child scope and lose them.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    . $craftEnv
} finally {
    $ErrorActionPreference = $prevEap
}

# gettext's AM_ICONV runs a runtime probe that often fails to execute on the
# Windows/Craft host, making HAVE_ICONV=0 and breaking the libtextstyle link
# (LNK2001: iconv_ostream_create). Preset the cache variables and build
# iconv first, as in packaging\ci\windows-build.ps1.
$env:am_cv_func_iconv = "yes"
$env:am_cv_func_iconv_works = "yes"
$env:am_cv_lib_iconv = "yes"

# Make the func/lib cache variables explicit in Craft's gettext blueprint too
# (it only presets works), so a failed link probe cannot reset HAVE_ICONV.
$gettextBlueprint = Get-ChildItem -Path $CraftRoot -Recurse -Filter gettext.py -ErrorAction SilentlyContinue | Select-Object -First 1
if ($gettextBlueprint) {
    $bp = Get-Content $gettextBlueprint.FullName -Raw
    if ($bp -notmatch 'am_cv_func_iconv=yes') {
        $bp = $bp -replace '"am_cv_func_iconv_works=yes"', '"am_cv_func_iconv=yes", "am_cv_func_iconv_works=yes", "am_cv_lib_iconv=yes"'
        Set-Content -Path $gettextBlueprint.FullName -Value $bp
    }
}
Get-ChildItem -Path $CraftRoot -Recurse -Filter config.cache -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'gettext' } |
    Remove-Item -Force -ErrorAction SilentlyContinue

craft libs/iconv
if (-not $?) { throw "craft libs/iconv failed" }

if (-not $SkipKate) {
    Write-Host "==> Building latest Kate via Craft"
    craft kate
    if (-not $?) { throw "craft kate failed" }
}

# Build the plugin against the Craft prefix (same flow as windows-build.ps1).
Write-Host "==> Building plugin against $CraftRoot"
& (Join-Path $Root "packaging\ci\windows-build.ps1") -BuildType Release -ArtifactsDir (Join-Path $Root "artifacts-windows-tmp")

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
