# Register CitraVR as a SteamVR application so it appears in the SteamVR
# dashboard, in Steam Link's PC app list, and can be launched from inside
# the headset without alt-tabbing on the desktop.
#
# What this does:
#   1. Locates citra_vr.exe (next to this script, or in ../build-vr/bin/Release).
#   2. Writes an absolute-path copy of citra_vr.vrmanifest next to the exe
#      (SteamVR requires absolute binary paths).
#   3. Adds that manifest path to %LOCALAPPDATA%\openvr\appconfig.json
#      under "manifest_paths" so SteamVR picks it up on next launch.
#
# Re-run after moving / rebuilding citra_vr.exe.
# Use unregister_steamvr.ps1 to remove the entry again.
#
# Requires: PowerShell 5+ (Windows built-in). No admin needed.

[CmdletBinding()]
param(
    [string]$CitraVrExe = ""
)

$ErrorActionPreference = "Stop"

function Resolve-CitraVrExe {
    param([string]$Hint)
    if ($Hint -and (Test-Path -LiteralPath $Hint)) {
        return (Resolve-Path -LiteralPath $Hint).Path
    }
    $candidates = @(
        (Join-Path $PSScriptRoot 'citra_vr.exe'),
        (Join-Path $PSScriptRoot '..\..\build-vr\bin\Release\citra_vr.exe'),
        (Join-Path $PSScriptRoot '..\..\build\bin\Release\citra_vr.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "Could not find citra_vr.exe. Pass -CitraVrExe <path>."
}

$exePath = Resolve-CitraVrExe -Hint $CitraVrExe
$exeDir  = Split-Path -Parent $exePath
Write-Host "citra_vr.exe        : $exePath"

# 1) Write absolute-path manifest next to the exe.
$templatePath = Join-Path $PSScriptRoot 'citra_vr.vrmanifest'
if (-not (Test-Path -LiteralPath $templatePath)) {
    throw "Template not found: $templatePath"
}
$manifest = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
# SteamVR wants the absolute path with forward slashes.
$manifest.applications[0].binary_path_windows = ($exePath -replace '\\', '/')
$installedManifest = Join-Path $exeDir 'citra_vr.vrmanifest'
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $installedManifest -Encoding UTF8
Write-Host "Manifest written    : $installedManifest"

# 2) Register manifest with SteamVR via appconfig.json.
$appConfigDir  = Join-Path $env:LOCALAPPDATA 'openvr'
$appConfigPath = Join-Path $appConfigDir 'appconfig.json'
if (-not (Test-Path -LiteralPath $appConfigDir)) {
    throw "SteamVR config directory not found: $appConfigDir. Launch SteamVR at least once first."
}

if (Test-Path -LiteralPath $appConfigPath) {
    $appConfig = Get-Content -LiteralPath $appConfigPath -Raw | ConvertFrom-Json
} else {
    $appConfig = [PSCustomObject]@{ manifest_paths = @(); external_drivers = @() }
}
if (-not $appConfig.PSObject.Properties.Name -contains 'manifest_paths') {
    Add-Member -InputObject $appConfig -MemberType NoteProperty -Name manifest_paths -Value @()
}

$existing = @($appConfig.manifest_paths) | Where-Object { $_ -ne $null }
$normalized = $installedManifest -replace '\\', '/'
$alreadyHas = $existing | Where-Object {
    ($_ -replace '\\', '/') -ieq $normalized
}
if (-not $alreadyHas) {
    $appConfig.manifest_paths = @($existing + $installedManifest)
    $appConfig | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $appConfigPath -Encoding UTF8
    Write-Host "Registered with SteamVR (appconfig.json updated)."
} else {
    Write-Host "Already registered (appconfig.json unchanged)."
}

Write-Host ""
Write-Host "Done. Next steps:"
Write-Host "  1. Start SteamVR (or restart it if it was running)."
Write-Host "  2. CitraVR will appear in the SteamVR dashboard under your library."
Write-Host "  3. For Steam Link: pair your headset, then launch the SteamVR dashboard"
Write-Host "     from inside the stream and pick 'CitraVR (3DS)'."
