# SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build (when Craft/KF6 is available) and install kateai.dll for Kate on Windows.
#   powershell -ExecutionPolicy Bypass -File packaging/windows/install.ps1
#   powershell -ExecutionPolicy Bypass -File packaging/windows/install.ps1 -Uninstall
#   powershell -ExecutionPolicy Bypass -File packaging/windows/install.ps1 -KateDir "C:\Program Files\Kate"

[CmdletBinding()]
param(
    [switch] $User,
    [switch] $System,
    [switch] $Uninstall,
    [switch] $SkipTests,
    [string] $KateDir,
    [string] $BuildType = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$PluginRel = "kf6\ktexteditor\kateai.dll"
$UserPluginDir = Join-Path $env:LOCALAPPDATA "KateAI\plugins"

function Find-KateRoot {
    if ($KateDir) {
        return (Resolve-Path $KateDir).Path
    }
    $candidates = @()
    if ($env:KDEROOT) { $candidates += $env:KDEROOT }
    $candidates += @(
        (Join-Path ${env:ProgramFiles} "Kate"),
        (Join-Path ${env:ProgramFiles} "KDE\Kate"),
        (Join-Path ${env:LOCALAPPDATA} "Programs\Kate")
    )
    $cmd = Get-Command kate.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $candidates += (Split-Path $cmd.Source -Parent)
        $candidates += (Split-Path (Split-Path $cmd.Source -Parent) -Parent)
    }
    foreach ($c in $candidates) {
        if (-not $c) { continue }
        $exe = Join-Path $c "bin\kate.exe"
        if (Test-Path $exe) { return $c }
        if (Test-Path (Join-Path $c "kate.exe")) { return $c }
    }
    return $null
}

function Find-KatePluginDir([string] $root) {
    if (-not $root) { return $null }
    $guesses = @(
        (Join-Path $root "bin\kf6\ktexteditor"),
        (Join-Path $root "kf6\ktexteditor"),
        (Join-Path $root "lib\plugins\kf6\ktexteditor"),
        (Join-Path $root "plugins\kf6\ktexteditor")
    )
    foreach ($g in $guesses) {
        if (Test-Path $g) { return $g }
    }
    $found = Get-ChildItem -Path $root -Recurse -Directory -Filter ktexteditor -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'kf6' } |
        Select-Object -First 1
    if ($found) { return $found.FullName }
    return $guesses[0]
}

function Add-UserPluginPath([string] $dir) {
    $current = [Environment]::GetEnvironmentVariable("QT_PLUGIN_PATH", "User")
    if ([string]::IsNullOrEmpty($current)) {
        [Environment]::SetEnvironmentVariable("QT_PLUGIN_PATH", $dir, "User")
        return
    }
    $parts = $current -split ';' | Where-Object { $_ -and ($_ -ne $dir) }
    $joined = (@($dir) + $parts) -join ';'
    [Environment]::SetEnvironmentVariable("QT_PLUGIN_PATH", $joined, "User")
}

function Remove-UserPluginPath([string] $dir) {
    $current = [Environment]::GetEnvironmentVariable("QT_PLUGIN_PATH", "User")
    if ([string]::IsNullOrEmpty($current)) { return }
    $parts = $current -split ';' | Where-Object { $_ -and ($_ -ne $dir) }
    $joined = $parts -join ';'
    if ([string]::IsNullOrEmpty($joined)) {
        [Environment]::SetEnvironmentVariable("QT_PLUGIN_PATH", $null, "User")
    } else {
        [Environment]::SetEnvironmentVariable("QT_PLUGIN_PATH", $joined, "User")
    }
}

if ($Uninstall) {
    $removed = $false
    $kateRoot = Find-KateRoot
    $dirs = @((Join-Path $UserPluginDir "kf6\ktexteditor"))
    if ($kateRoot) { $dirs += (Find-KatePluginDir $kateRoot) }
    foreach ($d in $dirs) {
        $so = Join-Path $d "kateai.dll"
        if (Test-Path $so) {
            Remove-Item -Force $so
            Write-Host "Removed $so"
            $removed = $true
        }
    }
    Remove-UserPluginPath $UserPluginDir
    if (-not $removed) {
        Write-Error "Kate AI plugin not found in the usual plugin directories."
    }
    Write-Host "Fully quit Kate and reopen it so the plugin list refreshes."
    exit 0
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Error @"
cmake was not found. On Windows, build Kate AI with KDE Craft so the plugin matches Kate:

  craft kateai

Copy packaging/craft/kateai.py into your Craft blueprints tree first.
Alternatively install Craft, enter the Craft shell, then re-run this script.
"@
}

$BuildDir = Join-Path $Root "build"
$configure = @(
    "-S", $Root, "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType"
)
if ($User -or -not $System) {
    $configure += "-DKATEAI_USER_INSTALL=ON"
} else {
    $configure += "-DKATEAI_USER_INSTALL=OFF"
    $configure += "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON"
}

Write-Host "==> Configuring ($BuildType)"
& cmake @configure
Write-Host "==> Building"
& cmake --build $BuildDir --config $BuildType -j $env:NUMBER_OF_PROCESSORS
if (-not $SkipTests -and $env:SKIP_TESTS -ne "1") {
    Write-Host "==> Tests"
    & ctest --test-dir $BuildDir -C $BuildType --output-on-failure
}

$built = Get-ChildItem -Path $BuildDir -Recurse -Filter kateai.dll | Select-Object -First 1
if (-not $built) {
    Write-Error "Build did not produce kateai.dll under $BuildDir"
}

$kateRoot = Find-KateRoot
$destDir = $null
if ($System -and $kateRoot) {
    $destDir = Find-KatePluginDir $kateRoot
} elseif ($kateRoot -and -not $User) {
    $pluginDir = Find-KatePluginDir $kateRoot
    if ($pluginDir -and (Test-Path $pluginDir) -and (Get-Acl $pluginDir).Access) {
        $destDir = $pluginDir
    }
}
if (-not $destDir) {
    $destDir = Join-Path $UserPluginDir "kf6\ktexteditor"
}

New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$dest = Join-Path $destDir "kateai.dll"
Copy-Item -Force $built.FullName $dest
Write-Host "Installed: $dest"

if ($destDir.StartsWith($UserPluginDir)) {
    Add-UserPluginPath $UserPluginDir
    Write-Host "User QT_PLUGIN_PATH now includes $UserPluginDir"
    Write-Host "Sign out or reboot so GUI Kate picks up the new plugin path, or start Kate from this session."
} else {
    Write-Host "Copied next to Kate. Fully quit Kate, then enable Kate AI under Settings → Plugins."
}
