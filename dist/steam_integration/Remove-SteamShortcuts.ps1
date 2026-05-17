# SPDX-FileCopyrightText: 2026 CitraVR / Sheikah Protocol authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Remove-SteamShortcuts.ps1
# -------------------------
# Removes the Sheikah Protocol [VR] shortcuts and their grid art from every
# logged-in Steam user.  Invoked by the Inno Setup uninstaller.
#
# Always exits 0; missing Steam or missing entries are not failures.

[CmdletBinding()]
param(
    [string]$QtAppName = 'Sheikah Protocol',
    [string]$VrAppName = 'Sheikah Protocol VR',
    [switch]$AllUsers = $true     # default to ALL users for clean uninstall
)

$ErrorActionPreference = 'Continue'
try {
    Import-Module (Join-Path $PSScriptRoot 'SteamShortcuts.psm1') -Force
} catch {
    Write-Warning "Steam shortcut module not loadable: $_"
    exit 0
}

foreach ($name in @($QtAppName, $VrAppName)) {
    try {
        Remove-SteamShortcut -AppName $name -AllUsers:$AllUsers | Out-Null
    } catch {
        Write-Warning "Remove '$name' failed: $_"
    }
}
Write-Host "Steam shortcut removal complete." -ForegroundColor Green
exit 0
