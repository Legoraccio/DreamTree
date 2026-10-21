# Generates the app icons, a gold commit-graph tree on a deep navy tile:
#   src\windows\mangrove.ico   PNG-compressed multi-size icon for Windows
#   src\macos\AppIcon.png       1024 x 1024 image for macOS (build-mac.sh makes the .icns from it), with the
#                               margins and corner radius of macOS app icons
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Join-Path $PSScriptRoot '..'
$ico = Join-Path $root 'src\windows\mangrove.ico'
$mac = Join-Path $root 'src\macos\AppIcon.png'
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256

# The tile and the graph in the square (x, y, s) of a canvas of `size` pixels.
function Draw-Icon($g, [float]$x, [float]$y, [float]$s, [float]$inset, [float]$radius, [float]$border) {
    $rect = New-Object System.Drawing.RectangleF ($x + $inset), ($y + $inset), ($s - 2 * $inset), ($s - 2 * $inset)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 0x1B, 0x2E, 0x5E)), ([System.Drawing.Color]::FromArgb(255, 0x09, 0x12, 0x2A)), 90
    $g.FillPath($bg, $path)
    $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 0xE3, 0xB3, 0x41)), $border), $path)

    # gold graph
    $gold = [System.Drawing.Color]::FromArgb(255, 0xE3, 0xB3, 0x41)
    $goldHi = [System.Drawing.Color]::FromArgb(255, 0xF6, 0xD3, 0x75)
    $lw = [Math]::Max(1.6, $s * 0.085)
    $pen = New-Object System.Drawing.Pen $gold, $lw
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $tx = $x + $s * 0.38
    $g.DrawLine($pen, $tx, $y + $s * 0.78, $tx, $y + $s * 0.26)
    $g.DrawBezier($pen, $tx, $y + $s * 0.64, $tx, $y + $s * 0.48, $x + $s * 0.68, $y + $s * 0.58, $x + $s * 0.68, $y + $s * 0.40)
    $nodes = @(@($tx, 0.25, 0.095, $gold), @($tx, 0.77, 0.085, $gold), @(($x + $s * 0.68), 0.37, 0.09, $goldHi))
    foreach ($n in $nodes) {
        $r = [Math]::Max(1.6, $s * $n[2])
        $g.FillEllipse((New-Object System.Drawing.SolidBrush $n[3]), [float]($n[0] - $r), [float]($y + $s * $n[1] - $r), [float](2 * $r), [float](2 * $r))
    }
}

function New-Canvas([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    return $bmp, $g
}

function New-IconPng([int]$s) {
    $bmp, $g = New-Canvas $s
    Draw-Icon $g 0 0 $s ([Math]::Max(0.5, $s * 0.04)) ($s * 0.22) ([Math]::Max(1.0, $s * 0.025))
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

# ---------------------------------------------------------------- Windows
$images = @($sizes | ForEach-Object { , (New-IconPng $_) })
$fs = [System.IO.File]::Create($ico)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Close()
"Wrote $ico ($((Get-Item $ico).Length) bytes)"

# ---------------------------------------------------------------- macOS: an 824-pixel tile centered on 1024, with a soft shadow
$bmp, $g = New-Canvas 1024
for ($k = 12; $k -ge 1; $k--) {
    $e = $k * 2.0
    $shadow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $sr = New-Object System.Drawing.RectangleF (100 - $e), (112 - $e), (824 + 2 * $e), (824 + 2 * $e)
    $sd = (185 + $e) * 2
    $shadow.AddArc($sr.X, $sr.Y, $sd, $sd, 180, 90)
    $shadow.AddArc($sr.Right - $sd, $sr.Y, $sd, $sd, 270, 90)
    $shadow.AddArc($sr.Right - $sd, $sr.Bottom - $sd, $sd, $sd, 0, 90)
    $shadow.AddArc($sr.X, $sr.Bottom - $sd, $sd, $sd, 90, 90)
    $shadow.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(5, 0, 0, 0))), $shadow)
}
Draw-Icon $g 100 100 824 0 185 7
$g.Dispose()
$bmp.Save($mac, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"Wrote $mac ($((Get-Item $mac).Length) bytes)"
