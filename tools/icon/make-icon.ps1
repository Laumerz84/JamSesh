# Draws the Jam Sesh app icon and writes Source\Resources\JamSesh.ico, which the app's version resource
# (Source\App\AppResources.rc.in) embeds. "J.S." in the New Rocker font, styled like the JAM SESH title in
# the Jam view (JamView.cpp paintTitle): slanted forward, amber to orange fill, light outline, orange
# glow and a stage light, on a dark rounded square. Small sizes drop the glow and outline so the letters
# stay readable. Uses .NET's System.Drawing, so it runs on any Windows PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\icon\make-icon.ps1 [-PreviewDir <dir>]
# -PreviewDir also saves every size as a PNG, and preview.png with all of them enlarged.
param([string]$PreviewDir = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fontFile = Join-Path $root "Source\Resources\Fonts\NewRocker-Regular.ttf"
$icoFile = Join-Path $root "Source\Resources\JamSesh.ico"
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

$fonts = New-Object System.Drawing.Text.PrivateFontCollection
$fonts.AddFontFile($fontFile)
$family = $fonts.Families[0]

function Rgb([int]$a, [int]$hex) { [System.Drawing.Color]::FromArgb($a, ($hex -shr 16) -band 255, ($hex -shr 8) -band 255, $hex -band 255) }
$amber = 0xffb347; $jam = 0xff7a3d; $pink = 0xff5c8a; $text = 0xd6e2ee   # Theme.h: Palette / Accent

function RoundedSquare([float]$x, [float]$w, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $x, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $x, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $x + $w - $d, $d, $d, 0, 90)
    $p.AddArc($x, $x + $w - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# "J.S." as an outline, slanted forward like the Jam view's title, fitted into a box of width w
# (and at most height h) centred on (cx, cy)
function Letters([float]$w, [float]$h, [float]$cx, [float]$cy) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddString("J.S.", $family, 0, 100.0, (New-Object System.Drawing.PointF(0, 0)), [System.Drawing.StringFormat]::GenericTypographic)
    $p.Transform((New-Object System.Drawing.Drawing2D.Matrix(1, 0, -0.12, 1, 0, 0)))
    $b = $p.GetBounds()
    $k = [Math]::Min($w / $b.Width, $h / $b.Height)
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate($cx, $cy)
    $m.Scale($k, $k)
    $m.Translate(-($b.X + $b.Width / 2), -($b.Y + $b.Height / 2))
    $p.Transform($m)
    return $p
}

function Draw([int]$size) {
    $ss = 4                                   # drawn 4x larger, then scaled down smoothly
    $S = [float]($size * $ss)
    $detail = $size -ge 48
    $small = $size -le 24

    $big = New-Object System.Drawing.Bitmap([int]$S, [int]$S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($big)
    $g.SmoothingMode = "AntiAlias"
    $g.PixelOffsetMode = "HighQuality"
    $g.CompositingQuality = "HighQuality"
    $g.Clear([System.Drawing.Color]::Transparent)

    # the slab: dark rounded square, a little lighter at the top, with a fine edge
    $inset = if ($small) { 0.0 } else { $S * 0.03 }
    $side = $S - 2 * $inset
    $slab = RoundedSquare $inset $side ($side * 0.22)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF($inset, $inset, $side, $side)),
                                                                   (Rgb 255 0x1b2230), (Rgb 255 0x07090d), 90.0)
    $g.FillPath($bg, $slab)
    $g.SetClip($slab)

    # stage light behind the letters
    if (-not $small) {
        $light = New-Object System.Drawing.Drawing2D.GraphicsPath
        $light.AddEllipse($S * 0.12, $S * 0.22, $S * 0.76, $S * 0.56)
        $lb = New-Object System.Drawing.Drawing2D.PathGradientBrush($light)
        $lb.CenterColor = Rgb 55 $jam
        $lb.SurroundColors = @((Rgb 0 $jam))
        $g.FillPath($lb, $light)
    }
    $g.ResetClip()

    # the letters: glow, gradient fill, light outline
    $fit = if ($small) { 0.94 } elseif ($detail) { 0.80 } else { 0.86 }
    $letters = Letters ($S * $fit) ($S * 0.80) ($S * 0.5) ($S * 0.52)
    $lbox = $letters.GetBounds()
    if (-not $small) {
        $glow = if ($detail) { @(@(0.085, 30), @(0.050, 55), @(0.024, 100)) } else { @(@(0.045, 50), @(0.022, 100)) }
        foreach ($gl in $glow) {
            $pen = New-Object System.Drawing.Pen((Rgb $gl[1] $jam), ($S * $gl[0]))
            $pen.LineJoin = "Round"
            $g.DrawPath($pen, $letters)
        }
    }
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF($lbox.X, ($lbox.Y - 1), $lbox.Width, ($lbox.Height + 2))),
                                                                     (Rgb 255 $amber), (Rgb 255 $jam), 90.0)
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend(3)
    $blend.Colors = @((Rgb 255 0xffd27a), (Rgb 255 $amber), (Rgb 255 $jam))
    $blend.Positions = @([float]0.0, [float]0.45, [float]1.0)
    $fill.InterpolationColors = $blend
    $g.FillPath($fill, $letters)
    if (-not $small) {
        $outline = New-Object System.Drawing.Pen((Rgb 220 $text), ($S * 0.012))
        $outline.LineJoin = "Round"
        $g.DrawPath($outline, $letters)
    }

    # scale down
    $img = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g2 = [System.Drawing.Graphics]::FromImage($img)
    $g2.InterpolationMode = "HighQualityBicubic"
    $g2.PixelOffsetMode = "HighQuality"
    $g2.CompositingMode = "SourceCopy"
    $g2.DrawImage($big, (New-Object System.Drawing.Rectangle(0, 0, $size, $size)))
    $g2.Dispose(); $g.Dispose(); $big.Dispose()
    return $img
}

# an icon image: PNG for 256, a 32-bit DIB (rows bottom-up, then an all-zero AND mask) below that
function IconImage([System.Drawing.Bitmap]$img) {
    $size = $img.Width
    $ms = New-Object System.IO.MemoryStream
    if ($size -ge 256) {
        $img.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return ,$ms.ToArray()   # the comma keeps PowerShell from returning the bytes one by one
    }
    $data = $img.LockBits((New-Object System.Drawing.Rectangle(0, 0, $size, $size)), "ReadOnly", "Format32bppArgb")
    $pixels = New-Object byte[] ($size * $size * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $img.UnlockBits($data)
    $w = New-Object System.IO.BinaryWriter($ms)
    $maskRow = [int]([Math]::Floor(($size + 31) / 32) * 4)
    $w.Write([int]40); $w.Write([int]$size); $w.Write([int]($size * 2)); $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]0); $w.Write([int]($size * $size * 4 + $maskRow * $size)); $w.Write([int]0); $w.Write([int]0)
    $w.Write([int]0); $w.Write([int]0)
    for ($y = $size - 1; $y -ge 0; $y--) { $w.Write($pixels, $y * $size * 4, $size * 4) }
    $w.Write((New-Object byte[] ($maskRow * $size)))
    $w.Flush()
    return ,$ms.ToArray()
}

$images = New-Object System.Collections.Generic.List[System.Drawing.Bitmap]
$blobs = New-Object System.Collections.Generic.List[byte[]]
foreach ($s in $sizes) {
    $img = Draw $s
    $images.Add($img)
    $blobs.Add((IconImage $img))
}

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([int16]1); $w.Write([int16]32); $w.Write([int]$blobs[$i].Length); $w.Write([int]$offset)
    $offset += $blobs[$i].Length
}
foreach ($b in $blobs) { $w.Write($b) }
$w.Flush()
[System.IO.File]::WriteAllBytes($icoFile, $out.ToArray())
"Wrote $icoFile ({0:N0} KB, sizes {1})" -f ($out.Length / 1KB), ($sizes -join ", ")

if ($PreviewDir) {
    New-Item -ItemType Directory -Force $PreviewDir | Out-Null
    for ($i = 0; $i -lt $sizes.Count; $i++) { $images[$i].Save((Join-Path $PreviewDir "icon-$($sizes[$i]).png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    # every size enlarged with square pixels, on a dark and a light strip (taskbar colours)
    $cell = 270
    $sheet = New-Object System.Drawing.Bitmap(($cell * 5), ($cell * 4))
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.InterpolationMode = "NearestNeighbor"
    $g.PixelOffsetMode = "Half"
    $g.Clear((Rgb 255 0x202020))
    $g.FillRectangle((New-Object System.Drawing.SolidBrush((Rgb 255 0xe8e8e8))), 0, ($cell * 2), ($cell * 5), ($cell * 2))
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $k = [Math]::Max(1, [Math]::Floor(240 / $s))
        $col = $i % 5; $row = [Math]::Floor($i / 5)
        foreach ($band in 0, 2) {
            $g.DrawImage($images[$i], (New-Object System.Drawing.Rectangle(($col * $cell + 10), (($row + $band) * $cell + 10), ($s * $k), ($s * $k))))
        }
    }
    $sheet.Save((Join-Path $PreviewDir "preview.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    "Preview: $(Join-Path $PreviewDir 'preview.png')"
}
