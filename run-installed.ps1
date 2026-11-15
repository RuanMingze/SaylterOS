# ============================================================
# 启动"已安装"的 SaylterOS
# ------------------------------------------------------------
# 与 run-install.ps1 配合：安装器把系统写入目标盘之后，
# 用本脚本从该盘引导。镜像以 -snapshot 方式启动，改动不落盘。
#
# 用法：
#   .\run-installed.ps1                                # 默认 build\saylter-target.img
#   .\run-installed.ps1 -Target D:\vms\saylter.img     # 指定其他已安装的虚拟磁盘
#   .\run-installed.ps1 -Target D:\vms\saylter.img -MemoryMB 128
# ============================================================

param(
    [string]$Target = 'build\saylter-target.img',
    [int]$MemoryMB = 64
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

Write-Host ("Booting installed system from {0} (-snapshot, changes are discarded)." -f $Target)

& $qemu -machine pc -cpu qemu64 -m $MemoryMB -display gtk `
  -drive "format=raw,file=$Target,if=ide" `
  -boot c -snapshot -no-reboot
