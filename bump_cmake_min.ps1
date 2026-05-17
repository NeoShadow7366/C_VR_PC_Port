$ErrorActionPreference = 'Stop'
$root = 'g:\Citra VR PC Port\externals'
$files = Get-ChildItem -Path $root -Recurse -Force -Filter CMakeLists.txt -File
$rx = [regex]'(?im)cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+(?:\.[0-9]+){0,3})([^)]*)\)'
$count = 0
foreach ($f in $files) {
    # Only skip our own CMake build dir.
    if ($f.FullName -like '*\build-vr\*') { continue }
    $orig = Get-Content -Raw -LiteralPath $f.FullName
    if ($null -eq $orig) { continue }
    $matches = $rx.Matches($orig)
    if ($matches.Count -eq 0) { continue }
    $sb = New-Object System.Text.StringBuilder
    $idx = 0
    $changed = $false
    foreach ($m in $matches) {
        [void]$sb.Append($orig.Substring($idx, $m.Index - $idx))
        $v = $m.Groups[1].Value
        $parts = @($v.Split('.'))
        while ($parts.Count -lt 2) { $parts += '0' }
        $ver = $null
        try { $ver = [version]($parts -join '.') } catch { }
        if ($null -ne $ver -and $ver -lt [version]'3.5') {
            [void]$sb.Append('cmake_minimum_required(VERSION 3.5)')
            $changed = $true
        } else {
            [void]$sb.Append($m.Value)
        }
        $idx = $m.Index + $m.Length
    }
    [void]$sb.Append($orig.Substring($idx))
    if ($changed) {
        Set-Content -LiteralPath $f.FullName -Value $sb.ToString() -NoNewline
        Write-Host "bumped: $($f.FullName.Substring($root.Length + 1))"
        $count++
    }
}
Write-Host "Total: $count"
