# SPDX-FileCopyrightText: 2026 CitraVR / NeoXR Citra authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# SteamShortcuts.psm1 — read/write Steam's binary shortcuts.vdf, with helpers
# to compute non-Steam app IDs and deploy Steam grid art.
#
# Public API:
#   Get-SteamPath
#   Get-SteamUserDirs                  -> @('<steam>\userdata\<id>',...)
#   Get-ShortcutAppId  -Exe -AppName   -> uint32  (used for shortcuts.vdf + grid)
#   Read-ShortcutsVdf  -Path           -> ordered hashtable
#   Write-ShortcutsVdf -Path -Data
#   Set-SteamShortcut  -Exe -AppName [-StartDir] [-Icon] [-LaunchOptions]
#                      [-OpenVr:$true|$false] [-Tags @('VR')]
#                      [-GridArtDir <dir>] [-AllUsers]
#   Remove-SteamShortcut -AppName [-AllUsers]
#
# The binary VDF format encoded here (types 0x00 map / 0x01 string / 0x02
# int32 LE / 0x08 end) matches what Steam itself writes; round-trip safe
# against vdfs created by Steam ROM Manager, SteamGridDB, and Steam itself.

Set-StrictMode -Version Latest

# --- CRC32 (IEEE 802.3) for app-id generation --------------------------------

$script:Crc32Table = $null
function Initialize-Crc32Table {
    if ($null -ne $script:Crc32Table) { return }
    # 0xEDB88320 overflows PS5.1's default int32 hex literal; parse as uint32.
    [uint32]$poly = [Convert]::ToUInt32('EDB88320', 16)
    $tbl = New-Object 'uint32[]' 256
    for ($i = 0; $i -lt 256; $i++) {
        [uint32]$c = [uint32]$i
        for ($j = 0; $j -lt 8; $j++) {
            if (($c -band 1) -ne 0) {
                $c = [uint32](($poly -bxor ($c -shr 1)) -band [uint32]::MaxValue)
            } else {
                $c = [uint32](($c -shr 1) -band [uint32]::MaxValue)
            }
        }
        $tbl[$i] = $c
    }
    $script:Crc32Table = $tbl
}
function Get-Crc32([byte[]]$bytes) {
    Initialize-Crc32Table
    [uint32]$crc = [uint32]::MaxValue
    foreach ($b in $bytes) {
        $idx = ($crc -bxor [uint32]$b) -band 0xFF
        $crc = [uint32]((($script:Crc32Table[$idx]) -bxor ($crc -shr 8)) -band [uint32]::MaxValue)
    }
    return ([uint32]($crc -bxor [uint32]::MaxValue))
}

function Get-ShortcutAppId {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$AppName)
    $unique = $Exe + $AppName
    $bytes  = [System.Text.Encoding]::UTF8.GetBytes($unique)
    [uint32]$crc = Get-Crc32 $bytes
    [uint32]$hi  = [Convert]::ToUInt32('80000000', 16)
    return ([uint32]($crc -bor $hi))
}

# --- Steam install discovery -------------------------------------------------

function Get-SteamPath {
    foreach ($view in 'Registry::HKEY_CURRENT_USER\Software\Valve\Steam',
                       'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam',
                       'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam') {
        try {
            $v = (Get-ItemProperty -Path $view -Name SteamPath -ErrorAction Stop).SteamPath
            if ($v -and (Test-Path $v)) { return ($v -replace '/', '\') }
        } catch {}
        try {
            $v = (Get-ItemProperty -Path $view -Name InstallPath -ErrorAction Stop).InstallPath
            if ($v -and (Test-Path $v)) { return ($v -replace '/', '\') }
        } catch {}
    }
    return $null
}

function Get-SteamUserDirs {
    $steam = Get-SteamPath
    if (-not $steam) { return @() }
    $userdata = Join-Path $steam 'userdata'
    if (-not (Test-Path $userdata)) { return @() }
    return Get-ChildItem -Path $userdata -Directory | Where-Object { $_.Name -match '^\d+$' } | ForEach-Object { $_.FullName }
}

# --- Binary VDF reader -------------------------------------------------------

function Read-NullString {
    param([System.IO.BinaryReader]$br)
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = $br.ReadByte()
        if ($b -eq 0) { break }
        [void]$bytes.Add($b)
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
}

function Read-VdfMap {
    param([System.IO.BinaryReader]$br)
    $map = [ordered]@{}
    while ($true) {
        if ($br.BaseStream.Position -ge $br.BaseStream.Length) { break }
        $type = $br.ReadByte()
        if ($type -eq 0x08) { break }
        $key = Read-NullString $br
        switch ($type) {
            0x00 { $map[$key] = Read-VdfMap $br }
            0x01 { $map[$key] = Read-NullString $br }
            0x02 {
                $raw = $br.ReadBytes(4)
                $map[$key] = [System.BitConverter]::ToUInt32($raw, 0)
            }
            0x07 { $map[$key] = [int64]$br.ReadInt64() }   # uint64 LE
            default {
                throw "Unknown VDF entry type 0x$($type.ToString('X2')) at offset $($br.BaseStream.Position - 1)"
            }
        }
    }
    return $map
}

function Read-ShortcutsVdf {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path $Path)) {
        return [ordered]@{ shortcuts = [ordered]@{} }
    }
    $fs = [System.IO.File]::OpenRead($Path)
    $br = New-Object System.IO.BinaryReader $fs
    try {
        $root = Read-VdfMap $br
    } finally {
        $br.Dispose(); $fs.Dispose()
    }
    if (-not $root.Contains('shortcuts')) { $root['shortcuts'] = [ordered]@{} }
    return $root
}

# --- Binary VDF writer -------------------------------------------------------

function Write-NullString {
    param([System.IO.BinaryWriter]$bw, [string]$s)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $bw.Write($bytes)
    $bw.Write([byte]0)
}

function Write-VdfMap {
    param([System.IO.BinaryWriter]$bw, $map)
    foreach ($k in $map.Keys) {
        $v = $map[$k]
        if ($v -is [System.Collections.IDictionary]) {
            $bw.Write([byte]0x00); Write-NullString $bw $k
            Write-VdfMap $bw $v
        } elseif ($v -is [string]) {
            $bw.Write([byte]0x01); Write-NullString $bw $k; Write-NullString $bw $v
        } elseif ($v -is [int64] -or $v -is [uint64]) {
            $bw.Write([byte]0x07); Write-NullString $bw $k; $bw.Write([uint64]$v)
        } else {
            # int / uint32 / bool -> int32 LE.  Cast through uint32 first to keep
            # appid bit pattern intact (Steam stores it as signed but the bits
            # are what matter).
            $iv = if ($v -is [bool]) { if ($v) { 1 } else { 0 } } else { $v }
            $u  = [uint32]$iv
            $bw.Write([byte]0x02); Write-NullString $bw $k
            # Write as raw little-endian 4 bytes so high-bit (>Int32.MaxValue) values survive.
            $bw.Write([System.BitConverter]::GetBytes($u))
        }
    }
    $bw.Write([byte]0x08)
}

function Write-ShortcutsVdf {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Data)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    if (Test-Path $Path) {
        $bak = "$Path.bak"
        if (-not (Test-Path $bak)) { Copy-Item -Force $Path $bak }
    }
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create)
    $bw = New-Object System.IO.BinaryWriter $fs
    try {
        Write-VdfMap $bw $Data
    } finally {
        $bw.Dispose(); $fs.Dispose()
    }
}

# --- Grid art deployment -----------------------------------------------------
# Steam looks for files in <userdata>\<id>\config\grid\ named:
#   <appid>.png           vertical capsule (600x900)
#   <appid>p.png          small capsule    (462x174)
#   <appid>_hero.png      hero banner      (1920x620)
#   <appid>_logo.png      transparent logo (1280x720)
#   <appid>_icon.png      32x32+ icon
# The same appid generated by Get-ShortcutAppId is reused.

function Deploy-GridArt {
    param(
        [Parameter(Mandatory)][string]$GridDir,
        [Parameter(Mandatory)][uint32]$AppId,
        [Parameter(Mandatory)][string]$ArtDir   # contains capsule_600x900.png etc.
    )
    if (-not (Test-Path $GridDir)) { New-Item -ItemType Directory -Force -Path $GridDir | Out-Null }
    $map = @{
        'capsule_600x900.png' = "$AppId.png"
        'p_462x174.png'       = "${AppId}p.png"
        'hero_1920x620.png'   = "${AppId}_hero.png"
        'logo_1280x720.png'   = "${AppId}_logo.png"
        'icon_256.png'        = "${AppId}_icon.png"
    }
    foreach ($k in $map.Keys) {
        $src = Join-Path $ArtDir $k
        if (Test-Path $src) {
            Copy-Item -Force $src (Join-Path $GridDir $map[$k])
        }
    }
}

# --- High-level shortcut upsert ---------------------------------------------

function New-DefaultShortcut {
    param([uint32]$AppId, [string]$AppName, [string]$Exe, [string]$StartDir, [string]$Icon, [string]$LaunchOptions, [bool]$OpenVr, [string[]]$Tags)
    $tagsMap = [ordered]@{}
    for ($i = 0; $i -lt $Tags.Count; $i++) { $tagsMap["$i"] = [string]$Tags[$i] }
    return [ordered]@{
        appid               = $AppId
        AppName             = $AppName
        Exe                 = $Exe
        StartDir            = $StartDir
        icon                = $Icon
        ShortcutPath        = ''
        LaunchOptions       = $LaunchOptions
        IsHidden            = 0
        AllowDesktopConfig  = 1
        AllowOverlay        = 1
        OpenVR              = if ($OpenVr) { 1 } else { 0 }
        Devkit              = 0
        DevkitGameID        = ''
        DevkitOverrideAppID = 0
        LastPlayTime        = 0
        FlatpakAppID        = ''
        tags                = $tagsMap
    }
}

function Set-SteamShortcut {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][string]$AppName,
        [string]$StartDir,
        [string]$Icon = '',
        [string]$LaunchOptions = '',
        [bool]$OpenVr = $false,
        [string[]]$Tags = @(),
        [string]$GridArtDir,
        [switch]$AllUsers
    )
    if (-not $StartDir) { $StartDir = Split-Path -Parent $Exe }

    # Steam expects Exe and StartDir wrapped in double quotes (literally).
    $exeQuoted      = '"' + $Exe + '"'
    $startDirQuoted = '"' + $StartDir + '"'
    $iconStr        = if ($Icon) { $Icon } else { '' }

    $appId = Get-ShortcutAppId -Exe $exeQuoted -AppName $AppName

    $userDirs = Get-SteamUserDirs
    if (-not $userDirs) {
        Write-Warning "No Steam user directories found under userdata\. Is Steam installed and has the user signed in at least once?"
        return $false
    }
    if (-not $AllUsers) {
        $userDirs = ,($userDirs | Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1)
    }

    foreach ($u in $userDirs) {
        $vdfPath = Join-Path $u 'config\shortcuts.vdf'
        $root = Read-ShortcutsVdf -Path $vdfPath
        if (-not $root.Contains('shortcuts')) { $root['shortcuts'] = [ordered]@{} }
        $shortcuts = $root['shortcuts']

        # Replace any existing entry with matching AppName (idempotent).
        $kept = [ordered]@{}
        $idx = 0
        foreach ($k in $shortcuts.Keys) {
            $entry = $shortcuts[$k]
            if ($entry -is [System.Collections.IDictionary] -and $entry['AppName'] -eq $AppName) { continue }
            $kept["$idx"] = $entry
            $idx++
        }
        $new = New-DefaultShortcut -AppId $appId -AppName $AppName -Exe $exeQuoted -StartDir $startDirQuoted -Icon $iconStr -LaunchOptions $LaunchOptions -OpenVr $OpenVr -Tags $Tags
        $kept["$idx"] = $new

        $root['shortcuts'] = $kept
        Write-ShortcutsVdf -Path $vdfPath -Data $root

        if ($GridArtDir -and (Test-Path $GridArtDir)) {
            $gridDir = Join-Path $u 'config\grid'
            Deploy-GridArt -GridDir $gridDir -AppId $appId -ArtDir $GridArtDir
        }
        Write-Host "  + ${AppName} -> shortcut id $appId in $vdfPath" -ForegroundColor Green
    }
    return $true
}

function Remove-SteamShortcut {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$AppName,
        [switch]$AllUsers
    )
    $userDirs = Get-SteamUserDirs
    if (-not $userDirs) { return $false }
    if (-not $AllUsers) {
        $userDirs = ,($userDirs | Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1)
    }
    foreach ($u in $userDirs) {
        $vdfPath = Join-Path $u 'config\shortcuts.vdf'
        if (-not (Test-Path $vdfPath)) { continue }
        $root = Read-ShortcutsVdf -Path $vdfPath
        $shortcuts = $root['shortcuts']
        $kept = [ordered]@{}
        $idx = 0
        $removedAppIds = @()
        foreach ($k in $shortcuts.Keys) {
            $entry = $shortcuts[$k]
            if ($entry -is [System.Collections.IDictionary] -and $entry['AppName'] -eq $AppName) {
                $removedAppIds += [uint32]($entry['appid'])
                continue
            }
            $kept["$idx"] = $entry
            $idx++
        }
        $root['shortcuts'] = $kept
        Write-ShortcutsVdf -Path $vdfPath -Data $root
        # Remove grid art too.
        $gridDir = Join-Path $u 'config\grid'
        if (Test-Path $gridDir) {
            foreach ($id in $removedAppIds) {
                Get-ChildItem $gridDir -File -Filter "$id*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
            }
        }
        Write-Host "  - ${AppName} removed from $vdfPath" -ForegroundColor Yellow
    }
    return $true
}

Export-ModuleMember -Function `
    Get-SteamPath, Get-SteamUserDirs, Get-ShortcutAppId, `
    Read-ShortcutsVdf, Write-ShortcutsVdf, `
    Set-SteamShortcut, Remove-SteamShortcut
