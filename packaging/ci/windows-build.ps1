# SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Windows CI/local plugin build with KDE Craft (MSVC ABI matching Kate).
#
# Flow (mirrors https://develop.kde.org/docs/getting-started/building/craft/):
#   1. Bootstrap Craft if missing (accepts installer defaults).
#   2. `craft kate`              -> latest Kate + Qt/KF6 stack in C:\CraftRoot.
#   3. cmake build of this repo against C:\CraftRoot + ctest.
#   4. Stage artifacts\kateai.dll (+ zip when 7-Zip is present).
#
# Usage: powershell -ExecutionPolicy Bypass -File packaging\ci\windows-build.ps1 `
#            [-BuildType Release] [-ArtifactsDir artifacts]
[CmdletBinding()]
param(
    [string] $BuildType = "Release",
    [string] $ArtifactsDir = "artifacts",
    [string] $CraftRoot = "C:\CraftRoot"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
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

# CraftBootstrap.isAndroid() keys off these env vars; on a Windows runner
# they may be set and would produce an Android ABI. KateAI does not build
# for Android, so drop them.
Remove-Item Env:ANDROID_SDK_ROOT -ErrorAction SilentlyContinue
Remove-Item Env:ANDROID_NDK -ErrorAction SilentlyContinue

# Diagnostics for Craft's VS detection - reduced logging
$sysVsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if ($env:CI_DEBUG -eq 'true') {
    Write-Host "==> Visual Studio discovery diagnostics"
    if (Test-Path $sysVsWhere) {
        & $sysVsWhere -property installationPath -nologo -latest -products "*" -version "[17,18)"
    }
}

function Invoke-CraftEnv {
    $craftEnv = Join-Path $CraftRoot "craft\craftenv.ps1"
    if (-not (Test-Path $craftEnv)) {
        throw "craftenv.ps1 not found at $craftEnv"
    }
    # Craft's craftenv.ps1 sanitizes the environment by removing a fixed list
    # of variables (including GITHUB_ACTION_REF). On some GitHub runners that
    # variable is not defined, and Remove-Item then throws under
    # $ErrorActionPreference = "Stop", aborting the whole build. Pre-create
    # any such variable as an empty string so the removal succeeds.
    foreach ($name in @("GITHUB_ACTION_REF", "GITHUB_ACTION", "GITHUB_ACTION_PATH", "GITHUB_ACTION_REPOSITORY")) {
        if (-not (Test-Path "Env:$name")) {
            Set-Item "Env:$name" ""
        }
    }
    # Dot-source: the env script must mutate THIS session (PATH, KDEROOT,
    # craft/cs functions). `&` would run it in a child scope and lose them.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        . $craftEnv
    } finally {
        $ErrorActionPreference = $prevEap
    }
}

if (-not (Test-Path (Join-Path $CraftRoot "craft\craftenv.ps1"))) {
    if ($env:CI_DEBUG -eq 'true') { Write-Host "==> Bootstrapping KDE Craft (accepting defaults)" }
    try {
        Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force -ErrorAction SilentlyContinue
    } catch { }
    python.exe -m pip install --upgrade certifi -q 2>&1 | Out-Null
    if ($env:CI -eq 'true' -and (Test-Path $CraftRoot)) {
        Remove-Item -Recurse -Force $CraftRoot -ErrorAction SilentlyContinue
    }
    $bootstrap = Join-Path ([IO.Path]::GetTempPath()) "CraftBootstrap.py"
    (New-Object net.webclient).DownloadFile(
        'https://raw.githubusercontent.com/KDE/craft/master/setup/CraftBootstrap.py', $bootstrap)
    & python.exe $bootstrap --prefix $CraftRoot --use-defaults
}

Invoke-CraftEnv

# Build deps and kate with filtered output
try { craft libs/libiconv 2>&1 | Where-Object { $_ -match "error|Error|ERROR|failed|Failed|FAILED|LINK.*error|LNK" } | Out-Host } catch { }
craft kate 2>&1 | Where-Object { $_ -match "error|Error|ERROR|failed|Failed|FAILED|LINK.*error|LNK" } | Out-Host

$BuildDir = Join-Path $Root "build"
$configure = @(
    "-S", $Root, "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_PREFIX_PATH=$CraftRoot",
    "-DKATEAI_USER_INSTALL=OFF",
    "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON"
)
& cmake @configure 2>&1 | Where-Object { $_ -match "error|Error|ERROR|CMake Error" } | Out-Host
$cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
& cmake --build $BuildDir --config $BuildType -j $cores 2>&1 | Where-Object { $_ -match "error|Error|ERROR|failed|Failed|FAILED|LINK.*error|LNK[0-9]+|fatal error" } | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }

& ctest --test-dir $BuildDir -C $BuildType --output-on-failure 2>&1 | Where-Object { $_ -match "Failed|failed|ERROR|Error|TOTAL.*failed" } | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ctest failed with exit code $LASTEXITCODE" }
New-Item -ItemType Directory -Force -Path $ArtifactsDir | Out-Null
$built = Get-ChildItem -Path $BuildDir -Recurse -Filter kateai.dll | Select-Object -First 1
if (-not $built) {
    throw "Build did not produce kateai.dll under $BuildDir"
}
Copy-Item -Force $built.FullName (Join-Path $ArtifactsDir "kateai.dll")
Get-Item (Join-Path $ArtifactsDir "kateai.dll")

$zip = Join-Path $ArtifactsDir "kateai-windows-$env:PROCESSOR_ARCHITECTURE.zip"
if (Get-Command 7z -ErrorAction SilentlyContinue) {
    & 7z a -tzip $zip (Join-Path $ArtifactsDir "kateai.dll") | Out-Null
    Get-Item $zip
} else {
    Compress-Archive -Force -Path (Join-Path $ArtifactsDir "kateai.dll") -DestinationPath $zip
    Get-Item $zip
}
