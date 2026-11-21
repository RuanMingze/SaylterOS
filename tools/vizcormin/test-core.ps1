# ============================================================
# test-core.ps1 - 编译并运行 VizCormin 核心测试
# ------------------------------------------------------------
# 只依赖宿主 GCC，不进入内核构建，也不会改动系统镜像。
# ============================================================

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$gcc = 'D:\msys2-64\ucrt64\bin\gcc.exe'
$core = Join-Path $root 'src\apps\vizcormin\core'
$app = Join-Path $root 'src\apps\vizcormin\app'
$incs = @("-I$core", "-I$app")

function Build-And-Run($name, $sources) {
    $out = Join-Path $root "build\$name.exe"
    $args = @('-std=c11', '-O2', '-Wall', '-Wextra') + $incs + $sources + @('-o', $out)
    & $gcc @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host "${name}: build failed" -ForegroundColor Red
        exit 1
    }
    & $out all
    if ($LASTEXITCODE -ne 0) {
        Write-Host "${name}: tests failed" -ForegroundColor Red
        exit 1
    }
}

$alloc = Join-Path $core 'vc_alloc.c'

Build-And-Run 'vc_core_test' @(
    $alloc,
    (Join-Path $core 'vc_text.c'),
    (Join-Path $root 'tools\vizcormin\core_test.c')
)

Build-And-Run 'vc_view_test' @(
    $alloc,
    (Join-Path $core 'vc_text.c'),
    (Join-Path $core 'vc_view.c'),
    (Join-Path $core 'vc_atlas.c'),
    (Join-Path $root 'tools\vizcormin\view_test.c')
)

Build-And-Run 'vc_render_test' @(
    $alloc,
    (Join-Path $core 'vc_text.c'),
    (Join-Path $core 'vc_view.c'),
    (Join-Path $core 'vc_atlas.c'),
    (Join-Path $core 'vc_render.c'),
    (Join-Path $root 'tools\vizcormin\render_test.c')
)

Build-And-Run 'vc_session_test' @(
    $alloc,
    (Join-Path $core 'vc_text.c'),
    (Join-Path $core 'vc_view.c'),
    (Join-Path $core 'vc_session.c'),
    (Join-Path $core 'vc_find.c'),
    (Join-Path $root 'tools\vizcormin\session_test.c')
)

Build-And-Run 'vc_command_test' @(
    $alloc,
    (Join-Path $core 'vc_text.c'),
    (Join-Path $core 'vc_view.c'),
    (Join-Path $core 'vc_session.c'),
    (Join-Path $core 'vc_find.c'),
    (Join-Path $core 'vc_command.c'),
    (Join-Path $root 'tools\vizcormin\command_find_test.c')
)

Build-And-Run 'vc_app_test' @(
    $alloc,
    (Join-Path $core 'vc_text.c'),
    (Join-Path $core 'vc_view.c'),
    (Join-Path $core 'vc_session.c'),
    (Join-Path $core 'vc_find.c'),
    (Join-Path $core 'vc_command.c'),
    (Join-Path $core 'vc_render.c'),
    (Join-Path $app 'vc_app.c'),
    (Join-Path $root 'tools\vizcormin\app_test.c')
)

Write-Host 'VIZCORMIN CORE: OK' -ForegroundColor Green
