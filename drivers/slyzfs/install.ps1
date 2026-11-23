# ============================================================
# install.ps1 - 安装 / 卸载 SlyzFs 驱动（需要管理员）
# ------------------------------------------------------------
# 前置条件：
#   1) drivers\slyzfs\build.ps1 -Sign 已生成并签名 SlyzFs-signed.sys
#   2) tools\driversign\install-trust.ps1 已把证书导入 Root/TrustedPublisher
#
# 用法（管理员 PowerShell）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File drivers\slyzfs\install.ps1
#   ... -Remove
#
# 安装后重启（或重新扫描磁盘）才会出现盘符；若加载被拒绝，
# 先关闭 Secure Boot 实测，仍失败再查事件查看器 7000/7026/1275。
# ============================================================

param(
    [string]$Repo = '',
    [switch]$Remove,
    [switch]$Demand
)

$ErrorActionPreference = 'Stop'

if ($Repo -eq '') {
    $Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$OutDir = Join-Path $Repo 'build\drivers'
$signed = Join-Path $OutDir 'SlyzFs-signed.sys'
$plain  = Join-Path $OutDir 'SlyzFs.sys'
$src = $null
if (Test-Path $signed) { $src = $signed } elseif (Test-Path $plain) { $src = $plain }
$dst = Join-Path $env:SystemRoot 'System32\drivers\SlyzFs.sys'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '需要管理员权限。' -ForegroundColor Red
    exit 1
}

if ($Remove) {
    & sc.exe stop SlyzFs | Out-Null
    & sc.exe delete SlyzFs | Out-Null
    if (Test-Path $dst) { Remove-Item $dst -Force }
    Write-Host '已卸载，重启后生效。' -ForegroundColor Green
    exit 0
}

if (-not $src) {
    Write-Host '找不到 build\drivers\SlyzFs.sys，请先运行 build.ps1。' -ForegroundColor Red
    exit 1
}
if ($src -eq $plain) {
    Write-Host '警告：正在安装未签名驱动，Windows x64 很可能拒绝加载。' -ForegroundColor Yellow
}

Copy-Item $src $dst -Force
Write-Host "installed file: $dst"

$startType = 'system'
if ($Demand) { $startType = 'demand' }

& sc.exe query SlyzFs | Out-Null
if ($LASTEXITCODE -eq 0) {
    & sc.exe config SlyzFs start= $startType
    & sc.exe config SlyzFs binPath= "`"$dst`""
    if ($LASTEXITCODE -ne 0) { Write-Host 'sc config failed.' -ForegroundColor Red; exit 1 }
} else {
    & sc.exe create SlyzFs type= filesys start= $startType error= normal binPath= "`"$dst`"" DisplayName= "SaylterOS SlyzFS"
    if ($LASTEXITCODE -ne 0) { Write-Host 'sc create failed.' -ForegroundColor Red; exit 1 }
}

& sc.exe start SlyzFs
if ($LASTEXITCODE -ne 0) {
    Write-Host '驱动启动失败：检查签名/信任库/Secure Boot（事件 1275/577）。' -ForegroundColor Yellow
} else {
    Write-Host '驱动已启动。' -ForegroundColor Green
}

Write-Host '重启后 SlyzFS 分区应出现盘符，根目录为 SYSTEM / RECOVERY / DATA。'
