# ============================================================
# run-uefi.ps1 - 用 OVMF 以 UEFI 方式启动 SaylterOS
# ------------------------------------------------------------
# 使用 msys2 自带的 edk2（OVMF）固件：
#   D:\msys2-64\ucrt64\share\qemu\edk2-x86_64-code.fd
#   D:\msys2-64\ucrt64\share\qemu\edk2-i386-vars.fd
#
# 关键点：
#   - UEFI 下没有 VBE/INT10，内核帧缓冲信息由 bootx64 用 GOP 填写；
#   - ESP 是镜像里的 FAT16 分区（tools\mkfat16.py 生成）；
#   - 磁盘按 drive 级 snapshot 启动，NVRAM(vars) 保留，便于注册证书。
# ============================================================

param(
    [switch]$Headless,
    [switch]$Secure,
    [switch]$FreshVars,
    [int]$MemoryMB = 256,
    [string]$Image = 'build\saylter-ide.img',
    [string]$Vars = 'build\ovmf\vars.fd'
)

$ErrorActionPreference = 'Stop'
$bld = 'build'

if (-not (Test-Path $Image)) {
    Write-Host "Image not found: $Image" -ForegroundColor Red
    exit 1
}

$fwDir = 'D:\msys2-64\ucrt64\share\qemu'
$codeName = if ($Secure) { 'edk2-x86_64-secure-code.fd' } else { 'edk2-x86_64-code.fd' }
$code = Join-Path $fwDir $codeName
$varsTpl = Join-Path $fwDir 'edk2-i386-vars.fd'
if (-not (Test-Path $code)) {
    Write-Host "OVMF code not found: $code" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $varsTpl)) {
    Write-Host "OVMF vars template not found: $varsTpl" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force (Split-Path $Vars) | Out-Null
if ($FreshVars -or -not (Test-Path $Vars)) {
    Copy-Item $varsTpl $Vars -Force
    Write-Host "Fresh OVMF NVRAM: $Vars"
}

$qemu = $null
$cmd = Get-Command 'qemu-system-x86_64' -ErrorAction SilentlyContinue
if ($cmd) { $qemu = $cmd.Source }
if (-not $qemu) {
    $fallback = 'D:\msys2-64\ucrt64\bin\qemu-system-x86_64.exe'
    if (Test-Path $fallback) { $qemu = $fallback }
}
if (-not $qemu) {
    Write-Host 'qemu-system-x86_64 not found.' -ForegroundColor Red
    exit 1
}

$serialLog = "$bld\serial-uefi.log"
Remove-Item $serialLog -ErrorAction SilentlyContinue

if ($Secure) {
    # Secure 固件 + secure pflash。先不用 SMM：i440fx 下变量服务是普通
    # DXE 实现，便于 Setup Mode 注册自定义证书。
    $qargs = @(
        '-machine', 'pc',
        '-cpu', 'qemu64',
        '-smp', '1',
        '-m', "$MemoryMB",
        '-global', 'driver=cfi.pflash01,property=secure,value=on',
        '-drive', "if=pflash,format=raw,readonly=on,file=$code",
        '-drive', "if=pflash,format=raw,file=$Vars",
        '-drive', "format=raw,file=$Image,if=ide,snapshot=on",
        '-device', 'e1000,netdev=n0',
        '-netdev', 'user,id=n0',
        '-serial', ("file:" + ($serialLog -replace '\\', '/'))
    )
} else {
    $qargs = @(
        '-machine', 'pc',
        '-cpu', 'qemu64',
        '-smp', '3',
        '-m', "$MemoryMB",
        '-boot', 'order=c',
        '-drive', "if=pflash,format=raw,readonly=on,file=$code",
        '-drive', "if=pflash,format=raw,file=$Vars",
        '-drive', "format=raw,file=$Image,if=ide,snapshot=on",
        '-device', 'e1000,netdev=n0',
        '-netdev', 'user,id=n0',
        '-serial', ("file:" + ($serialLog -replace '\\', '/'))
    )
}

if ($Headless) {
    $env:SDL_AUDIODRIVER = 'dummy'
    $qargs += @('-display', 'none', '-monitor', 'tcp:127.0.0.1:4470,server,nowait')
} else {
    $env:SDL_AUDIODRIVER = 'winmm'
    $qargs += @('-display', 'gtk,zoom-to-fit=on')
}

Write-Host ("Firmware: {0}" -f $codeName)
Write-Host ("Image   : {0} (snapshot)" -f $Image)
Write-Host ("NVRAM   : {0}" -f $Vars)
Write-Host ("Serial  : {0}" -f $serialLog)

& $qemu @qargs
