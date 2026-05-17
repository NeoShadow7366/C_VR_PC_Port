# SPDX-FileCopyrightText: 2026 CitraVR / NeoXR Citra authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Add-SteamShortcuts.ps1
# ----------------------
# Adds two non-Steam shortcuts to the user's Steam library:
#   - "NeoXR Citra"     -> citra-qt.exe   (flat 3DS emulator)
#   - "NeoXR Citra VR"  -> citra_vr_launcher.exe (VR build via SteamVR)
#
# Deploys matching grid art (capsule / hero / logo / small capsule / icon)
# into <Steam>\userdata\<id>\config\grid\.
#
# Idempotent — re-running replaces existing entries with the same AppName
# instead of duplicating them.  Steam must be restarted after this script
# for new shortcuts and art to show up.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallDir,   # directory containing citra-qt.exe + citra_vr_launcher.exe
    [string]$ArtRoot = (Join-Path $PSScriptRoot '..\steam_art\out'),
    [string]$QtAppName = 'NeoXR Citra',
    [string]$VrAppName = 'NeoXR Citra VR',
    [switch]$AllUsers,
    [switch]$VrOnly,
    [switch]$QtOnly
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'SteamShortcuts.psm1') -Force

$qtExe       = Join-Path $InstallDir 'citra-qt.exe'
$vrLauncher  = Join-Path $InstallDir 'citra_vr_launcher.exe'

$qtArt = Join-Path (Resolve-Path $ArtRoot) 'qt'
$vrArt = Join-Path (Resolve-Path $ArtRoot) 'vr'
$qtIco = Join-Path $qtArt 'icon.ico'
$vrIco = Join-Path $vrArt 'icon.ico'

$any = $false

if (-not $VrOnly) {
    if (-not (Test-Path $qtExe)) {
        Write-Warning "citra-qt.exe not found at: $qtExe  (skipping flat shortcut)"
    } else {
        Write-Host "Registering '$QtAppName' -> $qtExe" -ForegroundColor Cyan
        $ok = Set-SteamShortcut `
            -Exe $qtExe `
            -AppName $QtAppName `
            -StartDir $InstallDir `
            -Icon $qtIco `
            -LaunchOptions '' `
            -OpenVr $false `
            -Tags @('NeoXR','Emulator') `
            -GridArtDir $qtArt `
            -AllUsers:$AllUsers
        if ($ok) { $any = $true }
    }
}

if (-not $QtOnly) {
    if (-not (Test-Path $vrLauncher)) {
        Write-Warning "citra_vr_launcher.exe not found at: $vrLauncher  (skipping VR shortcut)"
    } else {
        Write-Host "Registering '$VrAppName' -> $vrLauncher" -ForegroundColor Cyan
        $ok = Set-SteamShortcut `
            -Exe $vrLauncher `
            -AppName $VrAppName `
            -StartDir $InstallDir `
            -Icon $vrIco `
            -LaunchOptions '' `
            -OpenVr $true `
            -Tags @('VR','NeoXR','Emulator') `
            -GridArtDir $vrArt `
            -AllUsers:$AllUsers
        if ($ok) { $any = $true }
    }
}

if ($any) {
    Write-Host ""
    Write-Host "Done. Restart Steam for the new shortcuts to appear." -ForegroundColor Green
    exit 0
} else {
    Write-Warning "No shortcuts added."
    exit 1
}
