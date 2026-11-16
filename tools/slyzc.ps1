# ============================================================
# slyzc.ps1 - Slyz GUI 程序编译器（Windows 端工具链）
# ------------------------------------------------------------
# 输入：.slyz 源码（首行必须是 #gui）
# 输出：同名 .slyzapp 打包文件（规范化文本，可直接放进 SlyzFS）
#
# 用法：
#   .\tools\slyzc.ps1 samples\gui-demo.slyz
#   .\tools\slyzc.ps1 samples\gui-demo.slyz -Out build\demo.slyzapp
#   .\tools\slyzc.ps1 samples\gui-demo.slyz -Check      # 只校验
#
# 校验规则（与内核解析器 slz_parse 保持一致）：
#   - 第一行必须是 #gui
#   - 指令只允许 title / size / label / input / button / space / hr
#   - 字符串必须用双引号，且同一行内闭合
#   - button 必须带 "-> 动作"
#   - 控件总数 <= 32
#   - size 宽高在 360x260 .. 2400x1600 之间
# 任何一条不满足都会以非零退出码结束，并指出行号。
# ============================================================

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Source,

    [string]$Out = '',

    [switch]$Check
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Source)) {
    Write-Host ("slyzc: source not found: {0}" -f $Source) -ForegroundColor Red
    exit 1
}

$lines = Get-Content -Path $Source -Encoding UTF8

if ($lines.Count -eq 0) {
    Write-Host 'slyzc: empty source file' -ForegroundColor Red
    exit 1
}

# ---- 第一行检查 ----
$first = $lines[0].Trim().TrimStart([char]0xFEFF)
if ($first -ne '#gui') {
    Write-Host 'slyzc: line 1: the first line must be exactly "#gui"' -ForegroundColor Red
    exit 1
}

$allowed = @('title', 'size', 'label', 'input', 'button', 'space', 'hr')
$controls = 0
$buttons = 0
$inputs = 0
$labels = 0
$winW = 560
$winH = 480
$title = ''

function Fail([int]$lineNo, [string]$message) {
    Write-Host ("slyzc: line {0}: {1}" -f $lineNo, $message) -ForegroundColor Red
    exit 1
}

# 提取双引号字符串；返回 @{ Value; Rest } 或 $null
function Read-Quoted([string]$text) {
    $t = $text.TrimStart()
    if ($t.Length -eq 0 -or $t[0] -ne '"') { return $null }
    $end = $t.IndexOf('"', 1)
    if ($end -lt 0) { return $null }
    $value = $t.Substring(1, $end - 1)
    $rest = ''
    if ($end + 1 -lt $t.Length) { $rest = $t.Substring($end + 1) }
    return @{ Value = $value; Rest = $rest }
}

$normalized = New-Object System.Collections.Generic.List[string]
$normalized.Add('#gui')

for ($i = 1; $i -lt $lines.Count; $i++) {
    $lineNo = $i + 1
    $line = $lines[$i].Trim()

    if ($line.Length -eq 0) { continue }
    if ($line.StartsWith('#')) { continue }

    $parts = $line -split '\s+', 2
    $op = $parts[0].ToLowerInvariant()
    $rest = ''
    if ($parts.Count -gt 1) { $rest = $parts[1] }

    if ($allowed -notcontains $op) {
        Fail $lineNo ("unknown directive '{0}' (allowed: {1})" -f $op, ($allowed -join ', '))
    }

    switch ($op) {
        'title' {
            $q = Read-Quoted $rest
            if ($null -eq $q) { Fail $lineNo 'title needs a double-quoted string' }
            if ($q.Value.Length -eq 0) { Fail $lineNo 'title cannot be empty' }
            if ($q.Value.Length -gt 60) { Fail $lineNo 'title is too long (max 60)' }
            $title = $q.Value
            $normalized.Add(('title "{0}"' -f $title))
        }
        'size' {
            $nums = $rest.Trim() -split '\s+'
            if ($nums.Count -lt 2) { Fail $lineNo 'size needs: size <width> <height>' }
            $w = 0; $h = 0
            if (-not [int]::TryParse($nums[0], [ref]$w)) { Fail $lineNo 'width must be an integer' }
            if (-not [int]::TryParse($nums[1], [ref]$h)) { Fail $lineNo 'height must be an integer' }
            if ($w -lt 360 -or $w -gt 2400) { Fail $lineNo 'width must be 360..2400' }
            if ($h -lt 260 -or $h -gt 1600) { Fail $lineNo 'height must be 260..1600' }
            $winW = $w
            $winH = $h
            $normalized.Add(('size {0} {1}' -f $w, $h))
        }
        'label' {
            $q = Read-Quoted $rest
            if ($null -eq $q) { Fail $lineNo 'label needs a double-quoted string' }
            if ($q.Value.Length -gt 90) { Fail $lineNo 'label is too long (max 90)' }
            $labels++
            $controls++
            $normalized.Add(('label "{0}"' -f $q.Value))
        }
        'input' {
            $nameRest = $rest.Trim()
            if ($nameRest.Length -eq 0) { Fail $lineNo 'input needs a name: input <name> ["placeholder"]' }
            $sp = $nameRest.IndexOfAny([char[]]" `t")
            if ($sp -lt 0) {
                $name = $nameRest
                $ph = ''
            } else {
                $name = $nameRest.Substring(0, $sp)
                $ph = $nameRest.Substring($sp).Trim()
            }
            if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
                Fail $lineNo ("input name must match [A-Za-z_][A-Za-z0-9_]* (got '{0}')" -f $name)
            }
            if ($name.Length -gt 30) { Fail $lineNo 'input name is too long (max 30)' }

            if ($ph.Length -gt 0) {
                $q = Read-Quoted $ph
                if ($null -eq $q) { Fail $lineNo 'input placeholder must be double-quoted' }
                $ph = $q.Value
            }
            if ($ph.Length -gt 60) { Fail $lineNo 'input placeholder is too long (max 60)' }

            $inputs++
            $controls++
            if ($ph.Length -gt 0) {
                $normalized.Add(('input {0} "{1}"' -f $name, $ph))
            } else {
                $normalized.Add(('input {0}' -f $name))
            }
        }
        'button' {
            $q = Read-Quoted $rest
            if ($null -eq $q) { Fail $lineNo 'button needs a quoted label: button "Text" -> action' }
            if ($q.Value.Length -eq 0) { Fail $lineNo 'button label cannot be empty' }
            if ($q.Value.Length -gt 60) { Fail $lineNo 'button label is too long (max 60)' }

            $tail = $q.Rest.TrimStart()
            if (-not $tail.StartsWith('->')) {
                Fail $lineNo 'button must be followed by "-> action"'
            }
            $action = $tail.Substring(2).Trim()
            if ($action.Length -eq 0) { Fail $lineNo 'button action cannot be empty' }
            if ($action.Length -gt 70) { Fail $lineNo 'button action is too long (max 70)' }

            $known = @('close', 'set:', 'open:', 'shell:')
            $ok = $false
            foreach ($k in $known) {
                if ($action -eq $k -or $action.StartsWith($k)) { $ok = $true; break }
            }
            if (-not $ok) {
                Fail $lineNo ("unknown action '{0}' (allowed: close, set:, open:, shell:)" -f $action)
            }

            $buttons++
            $controls++
            $normalized.Add(('button "{0}" -> {1}' -f $q.Value, $action))
        }
        'space' {
            $v = 0
            if (-not [int]::TryParse($rest.Trim(), [ref]$v)) { Fail $lineNo 'space needs an integer' }
            if ($v -lt 0 -or $v -gt 200) { Fail $lineNo 'space must be 0..200' }
            $controls++
            $normalized.Add(('space {0}' -f $v))
        }
        'hr' {
            $controls++
            $normalized.Add('hr')
        }
    }

    if ($controls -gt 32) {
        Fail $lineNo 'too many controls (max 32)'
    }
}

if ($buttons -eq 0) {
    Write-Host 'slyzc: warning: program has no button, it will be read-only' -ForegroundColor Yellow
}

$summary = "slyzc: OK  controls={0} labels={1} inputs={2} buttons={3} size={4}x{5}" -f `
    $controls, $labels, $inputs, $buttons, $winW, $winH
Write-Host $summary -ForegroundColor Green

if ($Check) {
    exit 0
}

if ($Out -eq '') {
    $dir = Split-Path -Parent $Source
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Source)
    if ($dir -eq '') {
        $Out = "$base.slyzapp"
    } else {
        $Out = Join-Path $dir "$base.slyzapp"
    }
}

$outDir = Split-Path -Parent $Out
if ($outDir -ne '' -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

# 输出为 UTF-8 无 BOM，换行统一 LF，便于内核逐行解析
$text = ($normalized -join "`n") + "`n"
$utf8 = New-Object System.Text.UTF8Encoding($false)

if ([System.IO.Path]::IsPathRooted($Out)) {
    $full = $Out
} else {
    $full = [System.IO.Path]::Combine((Get-Location).Path, $Out)
}

[System.IO.File]::WriteAllText($full, $text, $utf8)

Write-Host ("slyzc: wrote {0}" -f $full) -ForegroundColor Green
