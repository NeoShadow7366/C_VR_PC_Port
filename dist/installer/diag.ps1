$ErrorActionPreference = 'Stop'
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
if (-not $steam) {
    $steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
}
Write-Host "Steam path: $steam"
if (-not $steam) { Write-Host "STEAM NOT FOUND"; exit 0 }

$ud = Join-Path $steam 'userdata'
if (-not (Test-Path $ud)) { Write-Host "userdata missing"; exit 0 }

Get-ChildItem $ud -Directory | ForEach-Object {
    Write-Host ""
    Write-Host "--- user $($_.Name) ---"
    $sv = Join-Path $_.FullName 'config\shortcuts.vdf'
    $grid = Join-Path $_.FullName 'config\grid'
    if (Test-Path $sv) {
        $f = Get-Item $sv
        Write-Host "  shortcuts.vdf: $($f.Length) bytes, mod=$($f.LastWriteTime)"
    } else {
        Write-Host "  shortcuts.vdf MISSING"
    }
    if (Test-Path $grid) {
        $imgs = Get-ChildItem $grid -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'png|jpg|ico' }
        Write-Host "  grid images: $($imgs.Count)"
        $imgs | Select-Object -First 30 | ForEach-Object { Write-Host "    $($_.Name) $($_.Length) bytes" }
    } else {
        Write-Host "  grid dir MISSING"
    }
}

Write-Host ""
Write-Host "--- Sheikah install dir ---"
$inst = "$env:LOCALAPPDATA\Programs\SheikahProtocol"
if (Test-Path $inst) {
    Get-ChildItem $inst -Filter '*.exe' | ForEach-Object { Write-Host "  $($_.Name) $($_.Length) bytes" }
} else {
    Write-Host "  NOT INSTALLED at $inst"
}
