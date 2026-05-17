$log = "$env:APPDATA\Citra\log\citra_log.txt"
if (-not (Test-Path $log)) { Write-Host "NO LOG at $log"; exit 0 }
$f = Get-Item $log
Write-Host "Log: $($f.Length) bytes, mod=$($f.LastWriteTime)"
Write-Host ""
Write-Host "=== HMD / refresh / layer / vr_config ==="
Select-String -Path $log -Pattern 'HMD|refresh|vr_config|maxLayerCount|Layer count|Recommended per-eye|Beyond|System' |
    Select-Object -Last 50 |
    ForEach-Object { $_.Line }
Write-Host ""
Write-Host "=== Last 30 lines ==="
Get-Content $log -Tail 30
