# Build Steam grid art for Citra & NeoXR Citra VR from source images.
# Outputs into dist\steam_art\out\{qt,vr}\ in the exact dimensions Steam expects
# when art is dropped into <Steam>\userdata\<id>\config\grid\.
#
# Final files per app:
#   capsule_600x900.png  (vertical capsule)
#   hero_1920x620.png    (page banner)
#   logo_1280x720.png    (transparent overlay on hero)
#   p_462x174.png        (small horizontal capsule)
#   icon_256.png         + icon.ico (multi-res)
#
# No external deps; uses System.Drawing (built-in on Windows PowerShell 5.1).
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root  = Split-Path -Parent $PSCommandPath
$src   = Join-Path $root 'source'
$outQt = Join-Path $root 'out\qt'
$outVr = Join-Path $root 'out\vr'
New-Item -ItemType Directory -Force -Path $outQt, $outVr | Out-Null

# --- Source mapping (rename-tolerant: take first match per role) ---
function Pick($pattern) {
    $f = Get-ChildItem -Path $src -Filter $pattern -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $f) { return $null } else { return $f.FullName }
}

# Explicit mapping by current random filenames; falls back to dimension heuristic.
$mapExplicit = @{
    icon       = '7L14i.jpg'   # rounded-square icon
    capsule    = 'GP5cj.jpg'   # tall ornamental wall
    heroVr     = 'et3Ce.jpg'   # legacy: has old project name + Steam logo baked in; REPLACE before release
    heroQt     = 'tOPhD.jpg'   # data streams (no Steam logo)
    logoBg     = 'jXIxZ.jpg'   # flat tablet pattern
}

$resolved = @{}
foreach ($k in $mapExplicit.Keys) {
    $p = Join-Path $src $mapExplicit[$k]
    if (Test-Path $p) { $resolved[$k] = $p }
}

if ($resolved.Count -lt 5) {
    Write-Warning "Explicit source filenames not all found; falling back to dimension heuristic."
    $all = Get-ChildItem -Path $src -File | Where-Object { $_.Extension -match '\.(jpg|jpeg|png)$' }
    foreach ($f in $all) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $w = $img.Width; $h = $img.Height; $img.Dispose()
        $ratio = $w / $h
        if (-not $resolved.icon    -and $ratio -lt 0.75 -and $w -lt 800) { $resolved.icon = $f.FullName; continue }
        if (-not $resolved.capsule -and $ratio -lt 0.75)                 { $resolved.capsule = $f.FullName; continue }
        if (-not $resolved.heroVr  -and $ratio -ge 1.4)                  { $resolved.heroVr  = $f.FullName; continue }
        if (-not $resolved.heroQt  -and $ratio -ge 1.4)                  { $resolved.heroQt  = $f.FullName; continue }
        if (-not $resolved.logoBg)                                       { $resolved.logoBg  = $f.FullName }
    }
}

Write-Host "Resolved sources:" -ForegroundColor Cyan
$resolved.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-9} => {1}" -f $_.Key, (Split-Path -Leaf $_.Value)) }

# --- Helpers ---

function Load-Image([string]$path) {
    return [System.Drawing.Image]::FromFile($path)
}

# Center-crop $src to target aspect, then resize to (tw,th) and save as PNG.
function Save-FillCrop {
    param([System.Drawing.Image]$srcImg, [int]$tw, [int]$th, [string]$outPath)
    $tAspect = $tw / $th
    $sAspect = $srcImg.Width / $srcImg.Height
    if ($sAspect -gt $tAspect) {
        # too wide -> crop sides
        $newW = [int]([math]::Round($srcImg.Height * $tAspect))
        $newH = $srcImg.Height
        $x = [int](($srcImg.Width - $newW) / 2); $y = 0
    } else {
        # too tall -> crop top/bottom
        $newW = $srcImg.Width
        $newH = [int]([math]::Round($srcImg.Width / $tAspect))
        $x = 0; $y = [int](($srcImg.Height - $newH) / 2)
    }
    $bmp = New-Object System.Drawing.Bitmap $tw, $th
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality= [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $destRect = New-Object System.Drawing.Rectangle 0, 0, $tw, $th
    $g.DrawImage($srcImg, $destRect, $x, $y, $newW, $newH, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Draw text centred on bottom band of an existing image (used to brand hero with title).
function Add-TitleBand {
    param([string]$path, [string]$title, [string]$subtitle = $null)
    $img = Load-Image $path
    $bmp = New-Object System.Drawing.Bitmap $img
    $img.Dispose()
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

    # Band scales with image size; taller for tall capsules so two-line titles breathe.
    $aspectTall = ($bmp.Height -gt $bmp.Width)
    $bandFrac = if ($aspectTall) { 0.26 } else { 0.22 }
    $bandH = [int]($bmp.Height * $bandFrac)
    $bandY = $bmp.Height - $bandH
    $bandBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point 0, $bandY),
        (New-Object System.Drawing.Point 0, $bmp.Height),
        [System.Drawing.Color]::FromArgb(0, 0, 0, 0),
        [System.Drawing.Color]::FromArgb(230, 0, 0, 0))
    $g.FillRectangle($bandBrush, 0, $bandY, $bmp.Width, $bandH)

    $teal = [System.Drawing.Color]::FromArgb(255, 90, 210, 220)

    # Auto-fit title: shrink font until it fits within (90% width, $titleH px height).
    $titleAreaH = if ($subtitle) { [int]($bandH * 0.62) } else { [int]($bandH * 0.85) }
    $titleAreaW = [int]($bmp.Width * 0.92)
    $titleX = ($bmp.Width - $titleAreaW) / 2
    $titleY = $bandY + [int]($bandH * 0.06)

    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment     = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $sf.FormatFlags   = [System.Drawing.StringFormatFlags]::NoClip
    $sf.Trimming      = [System.Drawing.StringTrimming]::None

    # Start from a generous size and shrink. Cap by image min dim too.
    $maxFont = [math]::Min($titleAreaH * 0.85, $bmp.Width * 0.16)
    $minFont = 18.0
    $titleFont = $null
    for ($fs = [math]::Max($maxFont, $minFont); $fs -ge $minFont; $fs -= 2) {
        $f = New-Object System.Drawing.Font 'Segoe UI Semibold', $fs, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $measureSize = New-Object System.Drawing.SizeF $titleAreaW, ($titleAreaH * 4)
        $sz = $g.MeasureString($title, $f, $measureSize, $sf)
        if ($sz.Height -le $titleAreaH -and $sz.Width -le $titleAreaW) {
            $titleFont = $f
            break
        }
        $f.Dispose()
    }
    if (-not $titleFont) {
        $titleFont = New-Object System.Drawing.Font 'Segoe UI Semibold', $minFont, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    }

    $titleRect = New-Object System.Drawing.RectangleF $titleX, $titleY, $titleAreaW, $titleAreaH

    # Glow
    for ($r = 6; $r -ge 1; $r--) {
        $glow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([math]::Min(60, 20 + 5*$r), $teal))
        $rect2 = New-Object System.Drawing.RectangleF ($titleRect.X - $r), ($titleRect.Y - $r), ($titleRect.Width + 2*$r), ($titleRect.Height + 2*$r)
        $g.DrawString($title, $titleFont, $glow, $rect2, $sf)
    }
    $titleBrush = New-Object System.Drawing.SolidBrush $teal
    $g.DrawString($title, $titleFont, $titleBrush, $titleRect, $sf)

    if ($subtitle) {
        $subFontSize = [math]::Max(14.0, [math]::Min($bmp.Height * 0.035, $bmp.Width * 0.04))
        $subFont = New-Object System.Drawing.Font 'Segoe UI', $subFontSize, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
        $subRect = New-Object System.Drawing.RectangleF 0, ($bandY + $bandH * 0.70), $bmp.Width, ($bandH * 0.28)
        $subBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(220, 220, 230, 240))
        $g.DrawString($subtitle, $subFont, $subBrush, $subRect, $sf)
    }
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Build a transparent logo PNG (1280x720) by placing the icon image centered with title beneath.
function Build-Logo {
    param([string]$iconSrc, [string]$title, [string]$outPath)
    $bmp = New-Object System.Drawing.Bitmap 1280, 720
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    # Crop icon source to square then draw centred-upper.
    $img = Load-Image $iconSrc
    $side = [math]::Min($img.Width, $img.Height)
    $sx = [int](($img.Width  - $side) / 2)
    $sy = [int](($img.Height - $side) / 2)
    $iconSize = 460
    $iconX = ($bmp.Width - $iconSize) / 2
    $iconY = 30
    $destR = New-Object System.Drawing.Rectangle $iconX, $iconY, $iconSize, $iconSize
    $g.DrawImage($img, $destR, $sx, $sy, $side, $side, [System.Drawing.GraphicsUnit]::Pixel)
    $img.Dispose()

    $teal = [System.Drawing.Color]::FromArgb(255, 90, 210, 220)
    $titleFont = New-Object System.Drawing.Font 'Segoe UI Semibold', 96.0, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment     = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Near
    $titleRect = New-Object System.Drawing.RectangleF 0, ($iconY + $iconSize + 20), $bmp.Width, 140
    for ($r = 8; $r -ge 1; $r--) {
        $glow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([math]::Min(70, 16 + 6*$r), $teal))
        $rect2 = New-Object System.Drawing.RectangleF ($titleRect.X - $r), ($titleRect.Y - $r), ($titleRect.Width + 2*$r), ($titleRect.Height + 2*$r)
        $g.DrawString($title, $titleFont, $glow, $rect2, $sf)
    }
    $titleBrush = New-Object System.Drawing.SolidBrush $teal
    $g.DrawString($title, $titleFont, $titleBrush, $titleRect, $sf)

    $g.Dispose()
    $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Multi-resolution .ico from a square source PNG.
function Build-Ico {
    param([string]$sourcePng, [string]$outIco)
    $sizes = 16, 24, 32, 48, 64, 128, 256
    $srcImg = Load-Image $sourcePng
    $side = [math]::Min($srcImg.Width, $srcImg.Height)
    $sx = [int](($srcImg.Width  - $side) / 2)
    $sy = [int](($srcImg.Height - $side) / 2)

    $entries = @()
    foreach ($s in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap $s, $s
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $destR = New-Object System.Drawing.Rectangle 0, 0, $s, $s
        $g.DrawImage($srcImg, $destR, $sx, $sy, $side, $side, [System.Drawing.GraphicsUnit]::Pixel)
        $g.Dispose()
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $entries += [pscustomobject]@{ Size = $s; PngBytes = $ms.ToArray() }
        $ms.Dispose()
    }
    $srcImg.Dispose()

    $fs = [System.IO.File]::Open($outIco, [System.IO.FileMode]::Create)
    $bw = New-Object System.IO.BinaryWriter $fs
    # ICONDIR
    $bw.Write([uint16]0)              # reserved
    $bw.Write([uint16]1)              # type = ICO
    $bw.Write([uint16]$entries.Count) # count
    $headerSize = 6 + 16 * $entries.Count
    $offset = $headerSize
    foreach ($e in $entries) {
        $w = if ($e.Size -ge 256) { 0 } else { [byte]$e.Size }
        $h = $w
        $bw.Write([byte]$w)            # width
        $bw.Write([byte]$h)            # height
        $bw.Write([byte]0)             # palette
        $bw.Write([byte]0)             # reserved
        $bw.Write([uint16]1)           # color planes
        $bw.Write([uint16]32)          # bpp
        $bw.Write([uint32]$e.PngBytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $e.PngBytes.Length
    }
    foreach ($e in $entries) { $bw.Write($e.PngBytes) }
    $bw.Flush(); $bw.Close(); $fs.Close()
}

# --- Produce outputs for each app ---

function Build-AppArt {
    param([string]$outDir, [string]$titleHero, [string]$subtitleHero, [string]$heroSrc, [string]$capsuleSrc, [string]$iconSrc, [string]$logoText, [bool]$preserveHero)
    Write-Host "Building art => $outDir" -ForegroundColor Yellow

    # Hero
    $heroImg = Load-Image $heroSrc
    Save-FillCrop $heroImg 1920 620 (Join-Path $outDir 'hero_1920x620.png')
    $heroImg.Dispose()
    if (-not $preserveHero) {
        Add-TitleBand -path (Join-Path $outDir 'hero_1920x620.png') -title $titleHero -subtitle $subtitleHero
    }

    # Vertical capsule 600x900
    $capImg = Load-Image $capsuleSrc
    Save-FillCrop $capImg 600 900 (Join-Path $outDir 'capsule_600x900.png')
    $capImg.Dispose()
    Add-TitleBand -path (Join-Path $outDir 'capsule_600x900.png') -title $titleHero -subtitle $null

    # Small capsule 462x174 (from hero source)
    $heroImg2 = Load-Image $heroSrc
    Save-FillCrop $heroImg2 462 174 (Join-Path $outDir 'p_462x174.png')
    $heroImg2.Dispose()
    Add-TitleBand -path (Join-Path $outDir 'p_462x174.png') -title $titleHero -subtitle $null

    # Logo (transparent)
    Build-Logo -iconSrc $iconSrc -title $logoText -outPath (Join-Path $outDir 'logo_1280x720.png')

    # Icon files
    $iconImg = Load-Image $iconSrc
    Save-FillCrop $iconImg 256 256 (Join-Path $outDir 'icon_256.png')
    $iconImg.Dispose()
    Build-Ico -sourcePng (Join-Path $outDir 'icon_256.png') -outIco (Join-Path $outDir 'icon.ico')
}

# NeoXR Citra (non-VR / Qt)
Build-AppArt -outDir $outQt `
    -titleHero  'NeoXR Citra' `
    -subtitleHero '3DS Emulator' `
    -heroSrc    $resolved.heroQt `
    -capsuleSrc $resolved.capsule `
    -iconSrc    $resolved.icon `
    -logoText   'NeoXR Citra' `
    -preserveHero $false

# NeoXR Citra VR (legacy source still has old project name + Steam logo baked in; replace before release)
Build-AppArt -outDir $outVr `
    -titleHero  'NeoXR Citra VR' `
    -subtitleHero 'SteamVR build' `
    -heroSrc    $resolved.heroVr `
    -capsuleSrc $resolved.capsule `
    -iconSrc    $resolved.icon `
    -logoText   'NeoXR Citra VR' `
    -preserveHero $true   # source already branded; just crop/resize

Write-Host "`nDone. Outputs:" -ForegroundColor Green
Get-ChildItem -Recurse -File (Split-Path -Parent $outQt) |
    Select-Object @{n='File';e={Resolve-Path -Relative $_.FullName}}, @{n='KB';e={[int]($_.Length/1024)}} |
    Format-Table -AutoSize
