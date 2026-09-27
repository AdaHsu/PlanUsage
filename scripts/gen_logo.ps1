# Regenerates every embedded image asset from the three source files in
# images/:
#   images/Splash.png  ->  src/ui/splash_bitmap.h   TFT boot screen (RGB565)
#   images/Banner.png  ->  src/web/banner_png.h      admin page header (PNG)
#                       ->  src/web/logo_png.h        favicon / small icon (PNG),
#                           cropped from Banner's icon badge
#
# Run whenever any of those three source files change:
#   pwsh -File scripts/gen_logo.ps1
#
# Uses .NET's System.Drawing directly - no ImageMagick/PIL/ffmpeg needed,
# which is why this is PowerShell rather than a build-time Python step like
# upstream's embed_readme.py.

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot

function Resize-Bitmap($bmp, $w, $h) {
    $dst = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($bmp, 0, 0, $w, $h)
    $g.Dispose()
    return $dst
}

function Write-ByteArrayHeader($bytes, $path, $varName, $lenName, $comment) {
    $sb = New-Object System.Text.StringBuilder
    $sb.AppendLine("#pragma once")   | Out-Null
    $sb.AppendLine("#include <stdint.h>") | Out-Null
    $sb.AppendLine("#include <stddef.h>") | Out-Null
    $sb.AppendLine()                 | Out-Null
    foreach ($line in $comment) { $sb.AppendLine("// $line") | Out-Null }
    $sb.AppendLine("static const uint8_t $varName[] PROGMEM = {") | Out-Null

    $line = "    "
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $line += "0x{0:x2}," -f $bytes[$i]
        if (($i + 1) % 16 -eq 0) { $sb.AppendLine($line) | Out-Null; $line = "    " }
        else { $line += " " }
    }
    if ($line.Trim().Length -gt 0) { $sb.AppendLine($line) | Out-Null }
    $sb.AppendLine("};") | Out-Null
    $sb.AppendLine("static const size_t $lenName = sizeof($varName);") | Out-Null

    [System.IO.File]::WriteAllText($path, $sb.ToString())
}

function Png-Bytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

# --- Banner.png -> admin page header, served at /banner.png -----------------
# Kept as PNG with alpha - the browser composites it fine against the dark
# page background, no pre-blending needed the way the TFT bitmap requires.
$bannerSrc = [System.Drawing.Bitmap]::new((Join-Path $root "images\Banner.png"))
# Downscale from the 760-wide source to something reasonable to embed and
# transfer - full res buys nothing at the size it's actually displayed at.
$bannerW = 480
$bannerH = [int]([math]::Round($bannerSrc.Height * ($bannerW / $bannerSrc.Width)))
$banner  = Resize-Bitmap $bannerSrc $bannerW $bannerH
$bannerBytes = Png-Bytes $banner

Write-ByteArrayHeader $bannerBytes (Join-Path $root "src\web\banner_png.h") "BANNER_PNG" "BANNER_PNG_LEN" @(
    "${bannerW}x${bannerH} PNG resized from images/Banner.png, served at"
    "/banner.png. WebUi::brand() renders it as the admin page's header image."
    "Regenerate with scripts/gen_logo.ps1 if the source banner changes."
)
Write-Output "wrote src/web/banner_png.h (${bannerW}x${bannerH}, $($bannerBytes.Length) bytes)"

# --- Banner.png's icon badge, cropped -> favicon / small header icon --------
# Crop box found by hand against the 760x403 source: the icon's own rounded
# badge, without the outer banner's border bleeding in.
$iconCrop = New-Object System.Drawing.Rectangle 58, 42, 100, 100
$iconSrc  = $bannerSrc.Clone($iconCrop, $bannerSrc.PixelFormat)
$icon     = Resize-Bitmap $iconSrc 64 64
$iconBytes = Png-Bytes $icon

Write-ByteArrayHeader $iconBytes (Join-Path $root "src\web\logo_png.h") "LOGO_PNG" "LOGO_PNG_LEN" @(
    "64x64 PNG, cropped from the icon badge in images/Banner.png (crop box"
    "58,42,100,100 against the 760x403 source) and served at /logo.png - used"
    "as the browser tab favicon. Regenerate with scripts/gen_logo.ps1 if the"
    "source banner changes; re-check the crop box if the icon moves within it."
)
Write-Output "wrote src/web/logo_png.h (64x64, $($iconBytes.Length) bytes)"

$bannerSrc.Dispose(); $banner.Dispose(); $iconSrc.Dispose(); $icon.Dispose()

# --- Splash.png -> TFT boot screen, full-bleed RGB565 ------------------------
# RGB565 has no alpha channel, so the source's transparent rounded corners are
# alpha-composited against Ui::COLOR_BG (8,10,16) ahead of time here - skip
# this and the corners pick up a hard color-key fringe instead of blending
# into the actual boot-screen background.
$W = 320; $H = 170
$bgR = 8; $bgG = 10; $bgB = 16

$splashSrc = [System.Drawing.Bitmap]::new((Join-Path $root "images\Splash.png"))
$splash    = Resize-Bitmap $splashSrc $W $H

$sb = New-Object System.Text.StringBuilder
$sb.AppendLine("#pragma once") | Out-Null
$sb.AppendLine("#include <stdint.h>") | Out-Null
$sb.AppendLine() | Out-Null
$sb.AppendLine("// ${W}x${H} RGB565, resized from images/Splash.png and alpha-composited") | Out-Null
$sb.AppendLine("// against Ui::COLOR_BG (8,10,16) ahead of time - see the note in") | Out-Null
$sb.AppendLine("// scripts/gen_logo.ps1. Regenerate with that script if the splash changes.") | Out-Null
$sb.AppendLine("static const int SPLASH_BITMAP_W = $W;") | Out-Null
$sb.AppendLine("static const int SPLASH_BITMAP_H = $H;") | Out-Null
$sb.AppendLine("static const uint16_t SPLASH_BITMAP[] PROGMEM = {") | Out-Null

$line = "    "; $count = 0
for ($y = 0; $y -lt $H; $y++) {
    for ($x = 0; $x -lt $W; $x++) {
        $px = $splash.GetPixel($x, $y)
        $a  = $px.A / 255.0
        $r  = [int]([math]::Round($px.R * $a + $bgR * (1 - $a)))
        $gr = [int]([math]::Round($px.G * $a + $bgG * (1 - $a)))
        $b  = [int]([math]::Round($px.B * $a + $bgB * (1 - $a)))
        $rgb565 = (($r -band 0xF8) -shl 8) -bor (($gr -band 0xFC) -shl 3) -bor ($b -shr 3)
        $line += "0x{0:x4}," -f $rgb565
        $count++
        if ($count % 12 -eq 0) { $sb.AppendLine($line) | Out-Null; $line = "    " }
        else { $line += " " }
    }
}
if ($line.Trim().Length -gt 0) { $sb.AppendLine($line) | Out-Null }
$sb.AppendLine("};") | Out-Null

[System.IO.File]::WriteAllText((Join-Path $root "src\ui\splash_bitmap.h"), $sb.ToString())
Write-Output "wrote src/ui/splash_bitmap.h (${W}x${H})"

$splashSrc.Dispose(); $splash.Dispose()

# Drop the file the old (pre-Splash/Banner) icon-only design generated, if
# still present, so a stale header doesn't linger unreferenced.
Remove-Item (Join-Path $root "src\ui\logo_bitmap.h") -ErrorAction SilentlyContinue

Write-Output "done"
