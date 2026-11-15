# ============================================================
# SaylterOS 安装器启动脚本
# ------------------------------------------------------------
# 介质：build\saylter-installer.img（1.44MB 软盘镜像）
# 目标：-Target 指定的虚拟磁盘（默认 build\saylter-target.img）
#
# 用法：
#   .\run-install.ps1                                  # 默认目标盘
#   .\run-install.ps1 -Target D:\vms\saylter.img       # 指定其他虚拟磁盘
#   .\run-install.ps1 -Target D:\vms\saylter.img -SizeMB 64
#   .\run-install.ps1 -Target build\disk.img -MemoryMB 128
#
# 安装器流程：检测硬盘 -> Y 确认 -> 写入并逐扇区校验 -> 按任意键
#            直接从目标盘引导（无需重启、无需手动移除软盘）
# ============================================================

param(
    [string]$Target = 'build\saylter-target.img',
    [int]$SizeMB = 32,
    [int]$MemoryMB = 64
)

$installer = 'build\saylter-installer.img'
$minBytes  = 2064 * 512

# qemu 解析：优先 PATH，其次 msys2 固定安装位置
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

if (-not (Test-Path $installer)) {
    Write-Host 'Installer image missing: build\saylter-installer.img. Run .\build.ps1 first.' -ForegroundColor Red
    exit 1
}

$targetDir = Split-Path -Parent $Target
if ($targetDir -and -not (Test-Path $targetDir)) {
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
}

if (-not (Test-Path $Target)) {
    if ($SizeMB -lt 2) {
        Write-Host 'SizeMB must be at least 2 (payload + filesystem header).' -ForegroundColor Red
        exit 1
    }
    $sizeBytes = [int64]$SizeMB * 1MB
    Write-Host ("Creating target disk: {0} ({1} MB)" -f $Target, $SizeMB)
    & fsutil file createnew $Target $sizeBytes 2>$null | Out-Null
    & fsutil sparse setflag $Target 2>$null | Out-Null
} else {
    $len = (Get-Item $Target).Length
    if ($len -lt $minBytes) {
        Write-Host ("Target disk too small: {0} ({1} bytes, need >= {2})." -f $Target, $len, $minBytes) -ForegroundColor Red
        exit 1
    }
    Write-Host ("Target disk exists: {0} ({1} bytes). The installer will overwrite LBA0..payload and clear LBA2048..2063." -f $Target, $len)
}

Write-Host ("Installer : {0}" -f $installer)
Write-Host ("Target    : {0}" -f $Target)
Write-Host ("QEMU      : {0}  (-m {1} MB)" -f $qemu, $MemoryMB)

# 注意：这里不能加 -snapshot，否则安装结果不会写回目标镜像。
# 软盘显式指定 format=raw，避免 QEMU 自动探测格式时对块 0 加写限制。
& $qemu -machine pc -cpu qemu64 -m $MemoryMB -display gtk `
  -drive "if=floppy,index=0,format=raw,file=$installer" `
  -drive "format=raw,file=$Target,if=ide" `
  -boot a -no-reboot
