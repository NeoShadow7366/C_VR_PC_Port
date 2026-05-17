# Copies the latest citra_vr.exe next to citra-qt.exe so the
# `--vr` launcher / "Emulation > Launch in VR" menu can find it.
#
# Usage:
#   .\sync_vr_to_qt.ps1                # just sync
#   .\sync_vr_to_qt.ps1 -Launch        # sync, then launch citra-qt.exe
#   .\sync_vr_to_qt.ps1 -Launch -Rom 'G:\path\to\game.3ds'
#                                      # sync, then citra-qt.exe --vr <rom>
[CmdletBinding()]
param(
    [switch]$Launch,
    [string]$Rom
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSCommandPath
$src   = Join-Path $root 'build-vr\bin\Release\citra_vr.exe'
$qtDir = Join-Path $root 'build\bin\Release'
$dst   = Join-Path $qtDir 'citra_vr.exe'

if (-not (Test-Path $src))   { throw "Source not found: $src (build target citra_vr first)" }
if (-not (Test-Path $qtDir)) { throw "Qt build dir not found: $qtDir" }

$srcInfo = Get-Item $src
$dstInfo = Get-Item $dst -ErrorAction SilentlyContinue
if ($null -eq $dstInfo -or $srcInfo.LastWriteTime -gt $dstInfo.LastWriteTime) {
    Copy-Item $src $dst -Force
    Write-Host "Copied citra_vr.exe -> $dst" -ForegroundColor Green
} else {
    Write-Host "citra_vr.exe already up to date in Qt build dir." -ForegroundColor DarkGray
}

if ($Launch) {
    Push-Location $qtDir
    try {
        $args = @()
        if ($Rom) { $args += '--vr'; $args += $Rom }
        Write-Host "Launching: citra-qt.exe $($args -join ' ')" -ForegroundColor Cyan
        & .\citra-qt.exe @args
    } finally {
        Pop-Location
    }
}
