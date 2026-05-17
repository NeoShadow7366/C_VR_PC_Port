# SPDX-FileCopyrightText: 2026 CitraVR / Sheikah Protocol authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build-Installer.ps1
# -------------------
# One-shot build of the full Sheikah Protocol Windows installer:
#   1. (Re)build Steam grid art from dist\steam_art\source\
#   2. Verify Qt + VR build outputs are present
#   3. Invoke ISCC.exe to compile dist\installer\sheikah_protocol.iss
#
# Output: dist\installer\out\SheikahProtocol-Setup-<ver>.exe
#
# Requires: Inno Setup 6 (ISCC.exe on PATH or under "%PROGRAMFILES(X86)%\Inno Setup 6\").
#           Get it from https://jrsoftware.org/isinfo.php   (free, no install needed for build).

[CmdletBinding()]
param(
    [string]$RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildQt    = $null,
    [string]$BuildVr    = $null,
    [string]$AppVersion = '0.1.0',
    [switch]$SkipArt
)

$ErrorActionPreference = 'Stop'

if (-not $BuildQt) { $BuildQt = Join-Path $RepoRoot 'build\bin\Release' }
if (-not $BuildVr) { $BuildVr = Join-Path $RepoRoot 'build-vr\bin\Release' }

Write-Host "RepoRoot   = $RepoRoot"
Write-Host "BuildQt    = $BuildQt"
Write-Host "BuildVr    = $BuildVr"
Write-Host "AppVersion = $AppVersion"
Write-Host ""

# --- Step 1: art ---
if (-not $SkipArt) {
    Write-Host "[1/3] Building Steam grid art..." -ForegroundColor Cyan
    & (Join-Path $RepoRoot 'dist\steam_art\build_art.ps1')
}

# --- Step 2: verify payloads ---
Write-Host ""
Write-Host "[2/3] Verifying build outputs..." -ForegroundColor Cyan
$qt = Join-Path $BuildQt 'citra-qt.exe'
$vr = Join-Path $BuildVr 'citra_vr.exe'
$vl = Join-Path $BuildVr 'citra_vr_launcher.exe'
foreach ($p in @($qt, $vr, $vl)) {
    if (-not (Test-Path $p)) {
        throw "Required binary not found: $p`nBuild it first (cmake --build --preset qt / --preset vr)."
    }
    Write-Host "  ok: $p"
}

# --- Step 3: ISCC ---
Write-Host ""
Write-Host "[3/3] Running Inno Setup compiler..." -ForegroundColor Cyan
$iscc = $null
foreach ($candidate in @(
    'ISCC.exe',
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe")) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Path; break }
    if (Test-Path $candidate) { $iscc = $candidate; break }
}
if (-not $iscc) {
    throw "ISCC.exe not found. Install Inno Setup 6 from https://jrsoftware.org/isinfo.php"
}
Write-Host "  using: $iscc"

$iss = Join-Path $RepoRoot 'dist\installer\sheikah_protocol.iss'
$outDir = Join-Path $RepoRoot 'dist\installer\out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$arglist = @(
    "/DAPP_VERSION=`"$AppVersion`"",
    "/DBUILD_QT=`"$BuildQt`"",
    "/DBUILD_VR=`"$BuildVr`"",
    "/DREPO_ROOT=`"$RepoRoot`"",
    "`"$iss`""
)
Write-Host "  args: $($arglist -join ' ')"
$proc = Start-Process -FilePath $iscc -ArgumentList $arglist -NoNewWindow -PassThru -Wait
if ($proc.ExitCode -ne 0) {
    throw "ISCC failed with exit code $($proc.ExitCode)"
}

$out = Get-ChildItem -Path $outDir -Filter "SheikahProtocol-Setup-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($out) {
    Write-Host ""
    Write-Host "Installer built:" -ForegroundColor Green
    Write-Host "  $($out.FullName)  ($([int]($out.Length/1MB)) MB)"
}
