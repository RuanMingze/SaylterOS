# ============================================================
# SaylterOS 启动脚本（IDE 镜像）
# ------------------------------------------------------------
# 默认行为：
#   - 从 build\saylter-ide.img 启动（-snapshot，改动不落盘）
#   - 允许 guest 执行 reboot（切换 GUI/TTY 启动模式后必须能重启）
#   - 显式挂 e1000 + user 网络
#   - 串口日志写入 build\serial-run.log
#   - GTK 窗口按屏幕自动缩放
#
# GUI 测试流程：
#   1. .\run-ide.ps1
#   2. 登录 root
#   3. bootmode gui        （或 nano etc\system.cfg 把 boot.mode 改成 gui）
#   4. reboot
#   5. 重启后显示 GUI 启动画面 -> 图形登录 -> 桌面
#   6. 鼠标点击画面可捕获，Ctrl+Alt+G 释放
#
# 持久化测试：
#   .\run-ide.ps1 -Persist
#     使用 build\saylter-test.img，改动保存到该镜像；
#     build.ps1 重新生成 saylter-ide.img 后会自动复制一份新的测试盘。
#
# 其他参数：
#   -MemoryMB 128     调整内存
#   -NoReboot         禁止 guest 重启（QEMU 在 reboot 时退出）
#   -Headless         无窗口 + monitor tcp:4470（自动化用）
#   -Image <path>     直接指定镜像
# ============================================================

param(
    [switch]$Persist,
    [switch]$NoReboot,
    [switch]$Headless,
    [int]$MemoryMB = 64,
    [int]$Smp = 3,
    [string]$Image = ''
)

$ErrorActionPreference = 'Stop'
$bld = 'build'

if (-not (Test-Path "$bld\saylter-ide.img")) {
    Write-Host 'IDE image not found, rebuilding...'
    powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
    if (-not (Test-Path "$bld\saylter-ide.img")) {
        Write-Host 'Build failed: build\saylter-ide.img was not produced.' -ForegroundColor Red
        exit 1
    }
}

if ($Image -eq '') {
    if ($Persist) {
        $Image = "$bld\saylter-test.img"
        $src = "$bld\saylter-ide.img"
        $needCopy = $true
        if (Test-Path $Image) {
            $needCopy = (Get-Item $src).LastWriteTime -gt (Get-Item $Image).LastWriteTime
        }
        if ($needCopy) {
            Copy-Item $src $Image -Force
            Write-Host "Created fresh test image: $Image"
        }
    } else {
        $Image = "$bld\saylter-ide.img"
    }
}

if (-not (Test-Path $Image)) {
    Write-Host "Image not found: $Image" -ForegroundColor Red
    exit 1
}

$qemu = $null
$cmd = Get-Command 'qemu-system-x86_64' -ErrorAction SilentlyContinue
if ($cmd) { $qemu = $cmd.Source }
if (-not $qemu) {
    $fallback = 'D:\msys2-64\ucrt64\bin\qemu-system-x86_64.exe'
    if (Test-Path $fallback) { $qemu = $fallback }
}
if (-not $qemu) {
    Write-Host 'qemu-system-x86_64 not found (PATH or D:\msys2-64\ucrt64\bin).' -ForegroundColor Red
    exit 1
}

$serialLog = "$bld\serial-run.log"
Remove-Item $serialLog -ErrorAction SilentlyContinue

$qargs = @(
    '-machine','pc',
    '-cpu','qemu64',
    '-smp',"$Smp",
    '-m',"$MemoryMB",
    '-boot','c',
    '-drive',"format=raw,file=$Image,if=ide",
    '-device','e1000,netdev=n0',
    '-netdev','user,id=n0',
    '-serial',"file:$($serialLog -replace '\\','/')"
)

# AC'97 声卡：
#   交互模式用 SDL 的 winmm 后端（Windows waveOut，兼容性比
#   DirectSound 更好）；自动化模式用 SDL dummy，保证 DMA 按真实
#   时间推进但不占用宿主音频设备。
if ($Headless) {
    $env:SDL_AUDIODRIVER = 'dummy'
} else {
    $env:SDL_AUDIODRIVER = 'winmm'
}
$qargs += @('-audiodev','sdl,id=snd0')
$qargs += @('-device','AC97,audiodev=snd0')

if ($Headless) {
    $qargs += @('-display','none','-monitor','tcp:127.0.0.1:4470,server,nowait')
} else {
    $qargs += @('-display','gtk,zoom-to-fit=on')
}

if (-not $Persist) {
    $qargs += '-snapshot'
}
if ($NoReboot) {
    $qargs += '-no-reboot'
}

$modeText = 'snapshot (changes discarded)'
if ($Persist) { $modeText = 'persistent' }

Write-Host ("Image  : {0}" -f $Image)
Write-Host ("Memory : {0} MB" -f $MemoryMB)
Write-Host ("Serial : {0}" -f $serialLog)
Write-Host ("Mode   : {0}" -f $modeText)
Write-Host 'GUI: after login run  bootmode gui  then  reboot.'
Write-Host 'Mouse: click the window to capture, Ctrl+Alt+G releases.'

& $qemu @qargs
