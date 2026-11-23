# ============================================================
# mkusb.ps1 - 把 SaylterOS 磁盘镜像原样写入 U 盘
# ------------------------------------------------------------
# 安全约束（针对"误写数据盘"）：
#   1. 只列出 BusType = USB 的物理磁盘；
#   2. 重新校验用户选择的盘号确实是 USB，且不是系统盘/启动盘；
#   3. 必须手动输入 WRITE 才会开始写；
#   4. 写入前把目标盘下线，解除卷挂载，避免缓存写回冲突。
#
# 用法（管理员 PowerShell）：
#   powershell -ExecutionPolicy Bypass -File tools\mkusb.ps1
#   powershell -ExecutionPolicy Bypass -File tools\mkusb.ps1 -Image build\saylter-ide.img
#
# 注意：写入会覆盖 U 盘的分区表和前 32MB 内容，U 盘原有数据不可恢复。
# ============================================================

param(
    [string]$Image = 'build\saylter-ide.img'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    # 用 throw 而不是 exit：显式 exit 会终止整个 PowerShell 会话，
    # 即使启动时加了 -NoExit，窗口也会立刻消失，看不到错误原因。
    Write-Host $message -ForegroundColor Red
    throw $message
}

# ---- 管理员检查 ----
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail '需要管理员权限：请以管理员身份打开 PowerShell 再运行。'
}

# ---- 镜像检查 ----
if (-not (Test-Path $Image)) {
    Fail "镜像不存在：$Image（请先运行 build.ps1）"
}
$imageItem = Get-Item $Image
$imageBytes = $imageItem.Length
if ($imageBytes -lt (4 * 1024 * 1024)) {
    Fail ("镜像大小异常：{0} 字节" -f $imageBytes)
}
if ($imageBytes % 512 -ne 0) {
    Fail ("镜像不是 512 字节的整数倍：{0}" -f $imageBytes)
}

# ---- 只枚举 USB 物理磁盘 ----
$usbDisks = @(Get-Disk | Where-Object { $_.BusType -eq 'USB' })
if ($usbDisks.Count -eq 0) {
    Fail '没有检测到 USB 磁盘。请插入 U 盘后重试。'
}

Write-Host ''
Write-Host '检测到的 USB 磁盘：' -ForegroundColor Cyan
foreach ($d in $usbDisks) {
    $gb = [math]::Round($d.Size / 1GB, 2)
    $sys = if ($d.IsSystem -or $d.IsBoot) { '  [系统盘，禁止写入]' } else { '' }
    Write-Host ("  [{0}] {1}  {2} GB  序列号={3}  状态={4}{5}" -f `
        $d.Number, $d.FriendlyName, $gb, $d.SerialNumber, $d.OperationalStatus, $sys)
}
Write-Host ''

$raw = Read-Host '输入要写入的 USB 磁盘号（例如 2；直接回车取消）'
if ($raw -notmatch '^\d+$') {
    Write-Host '已取消。'
    return
}
$diskNumber = [int]$raw

# ---- 重新校验目标 ----
$target = Get-Disk -Number $diskNumber -ErrorAction SilentlyContinue
if (-not $target) {
    Fail "磁盘 $diskNumber 不存在。"
}
if ($target.BusType -ne 'USB') {
    Fail ("拒绝写入：磁盘 {0} 的 BusType 是 {1}，不是 USB。" -f $diskNumber, $target.BusType)
}
if ($target.IsSystem -or $target.IsBoot) {
    Fail ("拒绝写入：磁盘 {0} 是系统盘/启动盘。" -f $diskNumber)
}
if ($target.Size -lt $imageBytes) {
    Fail ("U 盘容量不足：{0} 字节 < 镜像 {1} 字节" -f $target.Size, $imageBytes)
}

Write-Host ''
Write-Host '即将写入：' -ForegroundColor Yellow
Write-Host ("  目标磁盘 : [{0}] {1}" -f $target.Number, $target.FriendlyName)
Write-Host ("  序列号   : {0}" -f $target.SerialNumber)
Write-Host ("  容量     : {0} GB" -f [math]::Round($target.Size / 1GB, 2))
Write-Host ("  镜像     : {0} ({1} 字节)" -f $imageItem.FullName, $imageBytes)
Write-Host ''
Write-Host '警告：U 盘分区表和前 32MB 内容将被覆盖，原有数据不可恢复。' -ForegroundColor Red
$confirm = Read-Host '确认请输入 WRITE（全大写），其他输入取消'
if ($confirm -ne 'WRITE') {
    Write-Host '已取消。'
    return
}

# ---- 解除占用 ----
# 可移动介质不支持 Set-Disk -IsOffline（会报 "Removable media cannot be
# set to offline"）。正确做法是清掉分区表，让 Windows 解除卷挂载，
# 之后才能用 FileShare.None 独占打开 \\.\PhysicalDriveN。
try {
    Set-Disk -Number $diskNumber -IsReadOnly $false -ErrorAction Stop
} catch {
    Write-Host ("  解除只读失败（继续）：{0}" -f $_.Exception.Message) -ForegroundColor Yellow
}
if ($target.IsOffline) {
    try {
        Set-Disk -Number $diskNumber -IsOffline $false -ErrorAction Stop
    } catch {
        Write-Host ("  切换在线失败（继续）：{0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

Write-Host '  清除分区表并释放卷占用...' -ForegroundColor DarkGray
try {
    Clear-Disk -Number $diskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
} catch {
    Write-Host ("  Clear-Disk 未完成（继续尝试写入）：{0}" -f $_.Exception.Message) -ForegroundColor Yellow
}
Update-HostStorageCache -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$src = $null
$dst = $null
try {
    $src = [System.IO.File]::OpenRead($imageItem.FullName)
    # 物理磁盘必须以 FileShare.None 独占打开；File.OpenWrite 默认是
    # FileShare.Read，会被 Windows 拒绝为 ERROR_INVALID_PARAMETER。
    $dst = New-Object System.IO.FileStream(
        ("\\.\PhysicalDrive{0}" -f $diskNumber),
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None)

    $buffer = New-Object byte[] (1024 * 1024)
    $written = 0
    while (($read = $src.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $dst.Write($buffer, 0, $read)
        $written += $read
        $percent = [int](($written * 100) / $imageBytes)
        Write-Progress -Activity '写入 SaylterOS 镜像' -Status ("{0} / {1} 字节" -f $written, $imageBytes) -PercentComplete $percent
    }
    $dst.Flush()
    Write-Progress -Activity '写入 SaylterOS 镜像' -Completed

    if ($written -ne $imageBytes) {
        throw ("写入长度不一致：{0} != {1}" -f $written, $imageBytes)
    }

    Write-Host ''
    Write-Host ("写入完成：{0} 字节" -f $written) -ForegroundColor Green
    Write-Host '接下来：' -ForegroundColor Cyan
    Write-Host '  1. 重启进入 BIOS，开启 CSM/Legacy Support，关闭 Secure Boot'
    Write-Host '  2. 选择 USB 启动（Legacy 模式）'
    Write-Host '  3. 开机日志里查看 [GPU] / [gpu] 行；进入登录提示后即可关机'
}
finally {
    if ($dst) { $dst.Dispose() }
    if ($src) { $src.Dispose() }
}
