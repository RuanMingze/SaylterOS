# ============================================================
# 启动"已安装"的 SaylterOS
# ------------------------------------------------------------
# 与 run-install.ps1 配合：安装器把系统写入目标盘之后，
# 用本脚本从该盘引导。
#
# 默认：-snapshot（改动不落盘），允许 guest reboot。
# GUI 测试：
#   登录 root -> bootmode gui -> reboot
#
# 用法：
#   .\run-installed.ps1                                # 默认 build\saylter-target.img
#   .\run-installed.ps1 -Target D:\vms\saylter.img     # 指定其他已安装的虚拟磁盘
#   .\run-installed.ps1 -Target D:\vms\saylter.img -MemoryMB 128
#   .\run-installed.ps1 -Target build\disk.img -Persist
#     -Persist 直接写目标盘，配置修改与文件改动都会保留。
# ============================================================

param(
    [string]$Target = 'build\saylter-target.img',
    [int]$MemoryMB = 64,
    [int]$Smp = 3,
    [switch]$Persist,
    [switch]$NoReboot,
    [switch]$Headless
)

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

if (-not (Test-Path $Target)) {
    Write-Host ("No installed system found at {0}. Run .\run-install.ps1 -Target {0} first." -f $Target)
    exit 1
}

$serialLog = 'build\serial-installed.log'
Remove-Item $serialLog -ErrorAction SilentlyContinue

$qargs = @(
    '-machine','pc',
    '-cpu','qemu64',
    '-smp',"$Smp",
    '-m',"$MemoryMB",
    '-boot','c',
    '-drive',"format=raw,file=$Target,if=ide",
    '-device','e1000,netdev=n0',
    '-netdev','user,id=n0',
    '-serial',"file:$($serialLog -replace '\\','/')"
)

if ($Headless) {
    $qargs += @('-display','none','-monitor','tcp:127.0.0.1:4471,server,nowait')
} else {
    $qargs += @('-display','gtk,zoom-to-fit=on')
}

$modeText = 'snapshot (changes discarded)'
if ($Persist) {
    $modeText = 'persistent'
} else {
    $qargs += '-snapshot'
}
if ($NoReboot) {
    $qargs += '-no-reboot'
}

Write-Host ("Target : {0}" -f $Target)
Write-Host ("Memory : {0} MB" -f $MemoryMB)
Write-Host ("Serial : {0}" -f $serialLog)
Write-Host ("Mode   : {0}" -f $modeText)
Write-Host 'GUI: after login run  bootmode gui  then  reboot.'

& $qemu @qargs
