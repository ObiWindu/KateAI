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

function Invoke-CraftEnv {
    $craftEnv = Join-Path $CraftRoot "craft\craftenv.ps1"
    if (-not (Test-Path $craftEnv)) {
        throw "craftenv.ps1 not found at $craftEnv"
    }
    # Dot-source: the env script must mutate THIS session (PATH, KDEROOT,
    # craft/cs functions). `&` would run it in a child scope and lose them.
    . $craftEnv
}

if (-not (Test-Path (Join-Path $CraftRoot "craft\craftenv.ps1"))) {
    Write-Host "==> Bootstrapping KDE Craft (accepting defaults)"
    Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force
    python.exe -m pip install --upgrade certifi
    iex ((New-Object net.webclient).DownloadString('https://raw.githubusercontent.com/KDE/craft/master/setup/install_craft.ps1'))
}

Write-Host "==> Loading Craft environment"
Invoke-CraftEnv

Write-Host "==> Installing latest Kate + deps via Craft (binary cache first; long on first run)"
craft kate

Write-Host "==> Configuring plugin ($BuildType) against $CraftRoot"
$BuildDir = Join-Path $Root "build"
$configure = @(
    "-S", $Root, "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_PREFIX_PATH=$CraftRoot",
    "-DKATEAI_USER_INSTALL=OFF",
    "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON"
)
& cmake @configure
Write-Host "==> Building"
$cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
& cmake --build $BuildDir --config $BuildType -j $cores

Write-Host "==> Tests"
& ctest --test-dir $BuildDir -C $BuildType --output-on-failure

Write-Host "==> Staging artifacts"
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
