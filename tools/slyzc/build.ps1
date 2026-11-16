# ============================================================
# tools/slyzc/build.ps1 - 编译 Windows 端 SLYZ 工具链
# ------------------------------------------------------------
# 产出 tools/slyzc/build/slyzc.exe，它同时包含：
#   - 编译器（.slyzcode -> .slyz）
#   - 反汇编器（dump）
#   - 宿主运行时（run）
#
# 运行时链接的是 src/kernel/slyzvm.c 本身，因此 Windows 上验证
# 通过的字节码，在内核里走的是同一套指令实现。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\slyzc\build.ps1
# ============================================================

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$outDir = Join-Path $here 'build'

if (!(Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$gcc = 'D:\msys2-64\ucrt64\bin\gcc.exe'
if (!(Test-Path $gcc)) {
    Write-Host "slyzc: gcc not found at $gcc" -ForegroundColor Red
    exit 1
}

$sources = @(
    (Join-Path $here 'slyzc_main.c'),
    (Join-Path $here 'slyzc_lex.c'),
    (Join-Path $here 'slyzc_parse.c'),
    (Join-Path $here 'slyzc_gen.c'),
    (Join-Path $here 'slyzc_util.c'),
    (Join-Path $here 'slyzrt_host.c'),
    (Join-Path $root 'src\kernel\slyzvm.c')
)

$args = @(
    '-std=c11',
    '-O2',
    '-Wall',
    '-Wextra',
    '-DSLYZ_HOST',
    "-I$root\src\include",
    "-I$root\src\kernel",
    '-o',
    (Join-Path $outDir 'slyzc.exe')
) + $sources

& $gcc @args
if ($LASTEXITCODE -ne 0) {
    Write-Host 'slyzc: build failed' -ForegroundColor Red
    exit 1
}

$exe = Join-Path $outDir 'slyzc.exe'
$size = (Get-Item $exe).Length
Write-Host ("  OK slyzc.exe ({0} bytes)" -f $size) -ForegroundColor Green
