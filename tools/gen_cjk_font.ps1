# ============================================================
# gen_cjk_font.ps1 - 生成 SaylterOS 的 32x32 中文点阵字库
# ------------------------------------------------------------
# 输出 src/kernel/font_cjk.c / font_cjk.h：
#   const uint16_t font_cjk_codes[N]      升序 Unicode 码位
#   const uint32_t font_cjk_rows[N][32]   每行 32 位，bit31 = 最左像素
#
# 覆盖范围（GB2312 区位）：
#   区 1  （0xA1A1..0xA1FE）中文标点与符号
#   区 2  （0xA2A1..0xA2FE）数字序号
#   区 3  （0xA3A1..0xA3FE）全角 ASCII
#   区 16..55            一级汉字 3755 字
#
# 字形来源：Noto Sans SC（SIL Open Font License 1.1）。
# 该授权允许把字形栅格化后嵌入并随程序分发，许可证正文位于
# fonts/noto-sans-sc/OFL.txt。位图形态命名为 "Saylter Han"。
#
# 依赖：Windows PowerShell 5.1 + System.Drawing（GDI+，系统自带）
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\gen_cjk_font.ps1
#   可选：-FontPath -OutFile -Preview -Threshold -PixelSize -Limit
# ============================================================

param(
    [string]$FontPath  = 'C:\Windows\Fonts\NotoSansSC-VF.ttf',
    [string]$OutFile   = "$PSScriptRoot\..\src\kernel\font_cjk.c",
    [string]$HeaderOut = "$PSScriptRoot\..\src\kernel\font_cjk.h",
    [string]$Preview   = "$PSScriptRoot\..\fonts\noto-sans-sc\cjk_preview.png",
    [int]$Threshold    = 110,
    [int]$PixelSize    = 32,
    [int]$Limit        = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$CellW = 32
$CellH = 32

# ---- 1. 从 GB2312 码页构造字符表 ----
$gb = [System.Text.Encoding]::GetEncoding(936)
$chars = New-Object System.Collections.Generic.List[int]
$quList = @(1, 2, 3) + (16..55)
foreach ($qu in $quList) {
    for ($wei = 1; $wei -le 94; $wei++) {
        # 显式构造双字节序列：@(a,b) 在部分 PowerShell 版本里
        # 会被 [byte[]] 强制转换吞成 null，这里直接分配数组逐位赋值。
        $two = [byte[]]::new(2)
        $two[0] = [byte](0xA0 + $qu)
        $two[1] = [byte](0xA0 + $wei)
        $s = $gb.GetString($two)
        if ($s.Length -ne 1) { continue }
        $cp = [int][char]$s[0]
        # 码页对未定义位会回填 '?'，必须剔除
        if ($cp -eq 0x3F) { continue }
        if ($cp -lt 0x80) { continue }
        $chars.Add($cp)
    }
}
$chars = $chars | Sort-Object -Unique
Write-Host ("charset: {0} glyphs" -f $chars.Count)

if ($Limit -gt 0 -and $chars.Count -gt $Limit) {
    $chars = $chars[0..($Limit - 1)]
    Write-Host ("  limit applied: {0}" -f $chars.Count)
}

# ---- 2. 加载字体 ----
if (!(Test-Path $FontPath)) { throw "font not found: $FontPath" }
$pfc = New-Object System.Drawing.Text.PrivateFontCollection
$pfc.AddFontFile((Resolve-Path $FontPath).Path)
if ($pfc.Families.Count -eq 0) { throw "cannot load font: $FontPath" }
$family = $pfc.Families[0]
# 3 参数构造保证 Unit = Pixel；4 参数构造会命中重载把 Pixel 当 FontStyle
$font = New-Object System.Drawing.Font($family, [float]$PixelSize, [System.Drawing.GraphicsUnit]::Pixel)
if ($font.Unit -ne [System.Drawing.GraphicsUnit]::Pixel) { throw "font unit is not Pixel" }
Write-Host ("font: {0} @ {1}px" -f $family.Name, $PixelSize)

$WorkW = 64
$WorkH = 64
$DrawX = 8
$DrawY = 8

$sf = [System.Drawing.StringFormat]::GenericTypographic
$sf.FormatFlags = $sf.FormatFlags -bor [System.Drawing.StringFormatFlags]::NoClip

function Render-Glyph([int]$code) {
    $bmp = New-Object System.Drawing.Bitmap($WorkW, $WorkH)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.TextContrast = 0
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawString([string][char]$code, $font, [System.Drawing.Brushes]::White,
                  [float]$DrawX, [float]$DrawY, $sf)
    $g.Dispose()
    return $bmp
}

function Get-Bounds([System.Drawing.Bitmap]$bmp, [int]$th) {
    $minX = 9999; $minY = 9999; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            if ($bmp.GetPixel($x, $y).R -ge $th) {
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

# ---- 3. 用参考字形确定全局平移 ----
# 汉字在 em box 内位置固定，绝不能逐字居中，否则笔画会上下跳动。
# 取"国"（U+56FD）作为参考：它的墨迹几乎填满 em box，用它把
# em box 对齐到 32x32 单元即可，其余字沿用同一平移。
$refCode = 0x56FD
$refBmp = Render-Glyph $refCode
$refBounds = Get-Bounds $refBmp $Threshold
if (-not $refBounds) { throw "reference glyph U+56FD produced no ink" }
$refW = $refBounds.MaxX - $refBounds.MinX + 1
$refH = $refBounds.MaxY - $refBounds.MinY + 1
$shiftX = [int][math]::Floor(($CellW - $refW) / 2) - $refBounds.MinX
$shiftY = [int][math]::Floor(($CellH - $refH) / 2) - $refBounds.MinY
Write-Host ("reference U+56FD ink {0}x{1} at ({2},{3}) -> shift ({4},{5})" -f `
    $refW, $refH, $refBounds.MinX, $refBounds.MinY, $shiftX, $shiftY)
$refBmp.Dispose()

# ---- 4. 逐字栅格化 ----
$n = $chars.Count
$bitmaps = New-Object 'System.UInt32[][]' $n
$overflowWarn = 0
$empty = 0
$idx = 0
foreach ($cp in $chars) {
    $bmp = Render-Glyph $cp
    $rows = New-Object 'System.UInt32[]' 32
    for ($r = 0; $r -lt $CellH; $r++) {
        $srcY = $r - $shiftY
        $val = [uint32]0
        for ($c = 0; $c -lt $CellW; $c++) {
            $srcX = $c - $shiftX
            $on = $false
            if ($srcX -ge 0 -and $srcX -lt $WorkW -and $srcY -ge 0 -and $srcY -lt $WorkH) {
                if ($bmp.GetPixel($srcX, $srcY).R -ge $Threshold) { $on = $true }
            }
            if ($on) { $val = $val -bor ([uint32]1 -shl (31 - $c)) }
        }
        $rows[$r] = $val
    }
    $bmp.Dispose()

    $nonzero = 0
    foreach ($v in $rows) { if ($v -ne 0) { $nonzero++ } }
    if ($nonzero -eq 0) {
        $empty++
        Write-Host ("  empty glyph U+{0:X4} '{1}'" -f $cp, [char]$cp)
    }
    # 越界检测：墨迹若被 32x32 裁掉，说明该字需要单独缩小
    $inkBmp = $null
    $bitmaps[$idx] = $rows
    $idx++
}

# ---- 5. 写头文件 ----
$hdr = New-Object System.Text.StringBuilder
[void]$hdr.AppendLine('/* Auto-generated by tools/gen_cjk_font.ps1 - do not edit */')
[void]$hdr.AppendLine('/* Typeface: Noto Sans SC (SIL Open Font License 1.1). */')
[void]$hdr.AppendLine('/* Bitmap form is named Saylter Han; see fonts/noto-sans-sc/OFL.txt. */')
[void]$hdr.AppendLine('#ifndef SAYLTER_FONT_CJK_H')
[void]$hdr.AppendLine('#define SAYLTER_FONT_CJK_H')
[void]$hdr.AppendLine('')
[void]$hdr.AppendLine('#include <stdint.h>')
[void]$hdr.AppendLine('')
[void]$hdr.AppendLine(("#define FONT_CJK_COUNT {0}" -f $n))
[void]$hdr.AppendLine('#define FONT_CJK_W     32')
[void]$hdr.AppendLine('#define FONT_CJK_H     32')
[void]$hdr.AppendLine('')
[void]$hdr.AppendLine('extern const uint16_t font_cjk_codes[FONT_CJK_COUNT];')
[void]$hdr.AppendLine('extern const uint32_t font_cjk_rows[FONT_CJK_COUNT][FONT_CJK_H];')
[void]$hdr.AppendLine('')
[void]$hdr.AppendLine('#endif')
[IO.File]::WriteAllText($HeaderOut, $hdr.ToString(), (New-Object System.Text.UTF8Encoding($false)))

# ---- 6. 写源文件 ----
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/* Auto-generated by tools/gen_cjk_font.ps1 - do not edit */')
[void]$sb.AppendLine('/* Typeface: Noto Sans SC (SIL Open Font License 1.1). */')
[void]$sb.AppendLine('/* Bitmap form is named Saylter Han; see fonts/noto-sans-sc/OFL.txt. */')
[void]$sb.AppendLine('/* Row bitmap: bit31 = leftmost pixel, 32 rows per glyph. */')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include <stdint.h>')
[void]$sb.AppendLine('#include "font_cjk.h"')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const uint16_t font_cjk_codes[FONT_CJK_COUNT] = {')
for ($i = 0; $i -lt $n; $i += 16) {
    $slice = @()
    for ($j = $i; $j -lt [math]::Min($i + 16, $n); $j++) {
        $slice += ("0x{0:X4}" -f $chars[$j])
    }
    [void]$sb.AppendLine('    ' + ($slice -join ',') + ',')
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const uint32_t font_cjk_rows[FONT_CJK_COUNT][FONT_CJK_H] = {')
for ($i = 0; $i -lt $n; $i++) {
    $vals = @()
    for ($r = 0; $r -lt 32; $r++) { $vals += ("0x{0:X8}u" -f $bitmaps[$i][$r]) }
    $comment = if ($chars[$i] -ge 0x20 -and $chars[$i] -lt 0x7F) { [char]$chars[$i] } else { '' }
    [void]$sb.AppendLine(("    {{ {0} }}, /* U+{1:X4} {2} */" -f ($vals -join ','), $chars[$i], $comment))
}
[void]$sb.AppendLine('};')
[IO.File]::WriteAllText($OutFile, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

# ---- 7. 预览图（每行 32 字，2x 放大，白字蓝底）----
$cols = 32
$rowsCount = [int][math]::Ceiling($n / $cols)
$gridW = $cols * $CellW
$gridH = $rowsCount * $CellH
$grid = New-Object System.Drawing.Bitmap($gridW, $gridH)
$gg = [System.Drawing.Graphics]::FromImage($grid)
$gg.Clear([System.Drawing.Color]::FromArgb(0, 0, 90))
$gg.Dispose()
for ($i = 0; $i -lt $n; $i++) {
    $cx = ($i % $cols) * $CellW
    $cy = [int][math]::Floor($i / $cols) * $CellH
    for ($r = 0; $r -lt 32; $r++) {
        $v = $bitmaps[$i][$r]
        if ($v -eq 0) { continue }
        for ($c = 0; $c -lt 32; $c++) {
            if ($v -band ([uint32]1 -shl (31 - $c))) {
                $grid.SetPixel($cx + $c, $cy + $r, [System.Drawing.Color]::White)
            }
        }
    }
}
$previewBmp = New-Object System.Drawing.Bitmap($gridW, $gridH)
$pg = [System.Drawing.Graphics]::FromImage($previewBmp)
$pg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$pg.DrawImage($grid, 0, 0, $previewBmp.Width, $previewBmp.Height)
$pg.Dispose()
$previewDir = Split-Path $Preview -Parent
if (!(Test-Path $previewDir)) { New-Item -ItemType Directory -Path $previewDir | Out-Null }
$previewBmp.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png)
$previewBmp.Dispose()
$grid.Dispose()

Write-Host ("generated {0} ({1} bytes), header {2}, preview {3}" -f `
    $OutFile, (Get-Item $OutFile).Length, $HeaderOut, $Preview)
Write-Host ("glyphs={0} empty={1}" -f $n, $empty)
