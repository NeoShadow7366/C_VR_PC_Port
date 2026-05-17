# Remove the CitraVR entry from %LOCALAPPDATA%\openvr\appconfig.json.
# Inverse of register_steamvr.ps1.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$appConfigPath = Join-Path $env:LOCALAPPDATA 'openvr\appconfig.json'
if (-not (Test-Path -LiteralPath $appConfigPath)) {
    Write-Host "No appconfig.json found - nothing to unregister."
    return
}

$appConfig = Get-Content -LiteralPath $appConfigPath -Raw | ConvertFrom-Json
if (-not ($appConfig.PSObject.Properties.Name -contains 'manifest_paths')) {
    Write-Host "No manifest_paths entry - nothing to unregister."
    return
}

$before  = @($appConfig.manifest_paths)
$after   = @($before | Where-Object {
    -not (($_ -as [string]) -match '(?i)citra_vr\.vrmanifest$')
})
if ($before.Count -eq $after.Count) {
    Write-Host "CitraVR manifest not present - nothing changed."
    return
}

$appConfig.manifest_paths = $after
$appConfig | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $appConfigPath -Encoding UTF8
Write-Host "Removed $($before.Count - $after.Count) CitraVR entry/entries from $appConfigPath."
Write-Host "Restart SteamVR for the change to take effect."
