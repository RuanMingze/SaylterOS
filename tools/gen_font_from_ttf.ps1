# ============================================================
# gen_font_from_ttf.ps1 - 把等宽 TTF 栅格化成 SaylterOS 的 16x32 终端字模
# ------------------------------------------------------------
# 输出格式与 src/kernel/vbe.c 的 vbe_draw_char 完全一致：
#   const uint8_t font16x32[256][32][2]
#   glyph[row][0] = 左 8 像素，glyph[row][1] = 右 8 像素，MSB 为最左像素
#
# 依赖：Windows PowerShell 5.1 + System.Drawing（GDI+，系统自带）
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\gen_font_from_ttf.ps1
#   可选参数：-FontPath -OutFile -Preview -Threshold -PixelSize
#
# 许可：默认字体为 Ubuntu Mono (c) Canonical Ltd.，Ubuntu Font Licence 1.0。
# 转换产物属于修改版，因此生成的字模命名为 "Saylter Mono"，
# 许可证原文随字体一起放在 fonts/ubuntu-mono/UFL.txt。
# ============================================================

param(
    [string]$FontPath = "$PSScriptRoot\..\fonts\ubuntu-mono\UbuntuMono-Regular.ttf",
    [string]$OutFile   = "$PSScriptRoot\..\src\kernel\font16x32.c",
    [string]$Preview   = "$PSScriptRoot\..\fonts\ubuntu-mono\preview.png",
    [int]$Threshold    = 100,
    [int]$PixelSize    = 32,
    [string]$FontLabel = "Ubuntu Mono"
)

Add-Type -AssemblyName System.Drawing

$CellW = 16
$CellH = 32
$FirstCode = 0x20
$LastCode  = 0x7E

function Get-InkBounds([System.Drawing.Bitmap]$bmp, [int]$threshold) {
    $minX = 9999; $minY = 9999; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if ($c.R -ge $threshold) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) { return $null }
    return @{ MinX = $minX; MinY = $minY; MaxX = $maxX; MaxY = $maxY }
}

# ---- 加载字体 ----
$path = (Resolve-Path $FontPath).Path
$pfc = New-Object System.Drawing.Text.PrivateFontCollection
$pfc.AddFontFile($path)
if ($pfc.Families.Count -eq 0) { throw "cannot load font: $path" }
$family = $pfc.Families[0]
# 注意：不要用 4 参数的 Font 构造。PowerShell 会命中
# Font(FontFamily, float, FontStyle, GraphicsUnit) 重载，
# 把 GraphicsUnit.Pixel 当成 FontStyle(Bold) 传入，Unit 变成 World，
# 字形会被横向撑宽 1.35 倍。
$font = New-Object System.Drawing.Font($family, [float]$PixelSize, [System.Drawing.GraphicsUnit]::Pixel)
if ($font.Unit -ne [System.Drawing.GraphicsUnit]::Pixel) { throw "font unit is not Pixel" }

# 工作位图比单元大，给上下留出安全边距，随后统一做全局平移
$WorkW = 48
$WorkH = 64
$DrawX = 4
$DrawY = 6

# ---- 逐字符渲染到工作位图 ----
$glyphBitmaps = @{}
$sf = [System.Drawing.StringFormat]::GenericTypographic
$sf.FormatFlags = $sf.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
$sf.FormatFlags = $sf.FormatFlags -bor [System.Drawing.StringFormatFlags]::NoClip

$globalMinX = 9999; $globalMinY = 9999; $globalMaxX = -1; $globalMaxY = -1

foreach ($code in $FirstCode..$LastCode) {
    $bmp = New-Object System.Drawing.Bitmap($WorkW, $WorkH)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.TextContrast = 0
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $ch = [string][char]$code
    $g.DrawString($ch, $font, [System.Drawing.Brushes]::White, [float]$DrawX, [float]$DrawY, $sf)
    $g.Dispose()

    $bounds = Get-InkBounds $bmp $Threshold
    if ($bounds) {
        if ($bounds.MinX -lt $globalMinX) { $globalMinX = $bounds.MinX }
        if ($bounds.MinY -lt $globalMinY) { $globalMinY = $bounds.MinY }
        if ($bounds.MaxX -gt $globalMaxX) { $globalMaxX = $bounds.MaxX }
        if ($bounds.MaxY -gt $globalMaxY) { $globalMaxY = $bounds.MaxY }
        if (($bounds.MaxX - $bounds.MinX) -gt 15 -or $bounds.MinX -lt 2) {
            Write-Host ("  wide glyph 0x{0:X2} '{1}' inkX={2}..{3} inkY={4}..{5}" -f `
                $code, [char]$code, $bounds.MinX, $bounds.MaxX, $bounds.MinY, $bounds.MaxY)
        }
    }
    $glyphBitmaps[$code] = $bmp
}

if ($globalMaxX -lt 0) { throw "no glyph ink rendered" }

# ---- 计算统一平移：让全部字形墨迹落在 16x32 单元格内 ----
# 水平：以 '_'/'X' 之类的基线字符为准，整体左对齐并保留 1px 边距
$shiftX = 1 - $globalMinX
# 垂直：底部保留 1px，顶部尽量保留 1px
$inkH = $globalMaxY - $globalMinY + 1
$shiftY = $CellH - 1 - $globalMaxY
if ($inkH -gt ($CellH - 2)) { $shiftY = 1 - $globalMinY }

Write-Host ("font={0} size={1}px  ink=({2},{3})..({4},{5})  shift=({6},{7})" -f `
    $font.Name, $PixelSize, $globalMinX, $globalMinY, $globalMaxX, $globalMaxY, $shiftX, $shiftY)

# ---- 写入 C 文件 ----
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* Auto-generated from $FontLabel by tools/gen_font_from_ttf.ps1 */")
[void]$sb.AppendLine("/* Original typeface: $FontLabel (see fonts/ubuntu-mono/UFL.txt) */")
[void]$sb.AppendLine("/* Modified bitmap form is named Saylter Mono; distributed under UFL 1.0. */")
[void]$sb.AppendLine("/* MSB = leftmost pixel, row order top=0, 16x32 cell */")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#include <stdint.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const uint8_t font16x32[256][32][2] = {")

for ($code = 0; $code -lt 256; $code++) {
    $rows = @()
    if ($glyphBitmaps.ContainsKey($code)) {
        $bmp = $glyphBitmaps[$code]
        for ($row = 0; $row -lt $CellH; $row++) {
            $b0 = 0; $b1 = 0
            $srcY = $row - $shiftY
            for ($col = 0; $col -lt $CellW; $col++) {
                $srcX = $col - $shiftX
                $on = 0
                if ($srcX -ge 0 -and $srcX -lt $WorkW -and $srcY -ge 0 -and $srcY -lt $WorkH) {
                    $c = $bmp.GetPixel($srcX, $srcY)
                    if ($c.R -ge $Threshold) { $on = 1 }
                }
                if ($on) {
                    if ($col -lt 8) { $b0 = $b0 -bor (0x80 -shr $col) }
                    else { $b1 = $b1 -bor (0x80 -shr ($col - 8)) }
                }
            }
            $rows += ("0x{0:X2},0x{1:X2}" -f $b0, $b1)
        }
    } else {
        for ($row = 0; $row -lt $CellH; $row++) { $rows += "0x00,0x00" }
    }
    $comment = if ($code -ge 0x20 -and $code -le 0x7E) { "'" + [char]$code + "'" } else { "." }
    [void]$sb.AppendLine(("    /* 0x{0:X2} {1} */ {{ {2} }}," -f $code, $comment, ($rows -join ",")))
}
[void]$sb.AppendLine("};")

[IO.File]::WriteAllText((Resolve-Path -LiteralPath (Split-Path $OutFile -Parent)).Path + "\" + (Split-Path $OutFile -Leaf),
    $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

# ---- 预览图：可打印 ASCII 的 10 列网格，2x 放大 ----
$cols = 10
$rowsCount = [int][math]::Ceiling(($LastCode - $FirstCode + 1) / $cols)
$pad = 1
$gridW = [int]($cols * ($CellW + $pad) + $pad)
$gridH = [int]($rowsCount * ($CellH + $pad) + $pad)
$grid = New-Object System.Drawing.Bitmap($gridW, $gridH)
$gg = [System.Drawing.Graphics]::FromImage($grid)
$gg.Clear([System.Drawing.Color]::FromArgb(0, 0, 160))
$gg.Dispose()

$i = 0
foreach ($code in $FirstCode..$LastCode) {
    $bmp = $glyphBitmaps[$code]
    $cx = $pad + ($i % $cols) * ($CellW + $pad)
    $cy = $pad + [math]::Floor($i / $cols) * ($CellH + $pad)
    for ($row = 0; $row -lt $CellH; $row++) {
        $srcY = $row - $shiftY
        for ($col = 0; $col -lt $CellW; $col++) {
            $srcX = $col - $shiftX
            $on = $false
            if ($srcX -ge 0 -and $srcX -lt $WorkW -and $srcY -ge 0 -and $srcY -lt $WorkH) {
                $c = $bmp.GetPixel($srcX, $srcY)
                if ($c.R -ge $Threshold) { $on = $true }
            }
            if ($on) { $grid.SetPixel($cx + $col, $cy + $row, [System.Drawing.Color]::White) }
        }
    }
    $i++
}

$previewBmp = New-Object System.Drawing.Bitmap(([int]$gridW * 2), ([int]$gridH * 2))
$pg = [System.Drawing.Graphics]::FromImage($previewBmp)
$pg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$pg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$pg.DrawImage($grid, 0, 0, $previewBmp.Width, $previewBmp.Height)
$pg.Dispose()
$previewBmp.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png)

$nonZero = 0
foreach ($code in $FirstCode..$LastCode) {
    $bmp = $glyphBitmaps[$code]
    $found = $false
    for ($y = 0; $y -lt $WorkH -and -not $found; $y++) {
        for ($x = 0; $x -lt $WorkW; $x++) {
            if ($bmp.GetPixel($x, $y).R -ge $Threshold) { $found = $true; break }
        }
    }
    if ($found) { $nonZero++ }
}

Write-Host ("generated {0}: {1} bytes, non-empty glyphs {2}/{3}, preview {4}" -f `
    $OutFile, (Get-Item $OutFile).Length, $nonZero, ($LastCode - $FirstCode + 1), $Preview)
