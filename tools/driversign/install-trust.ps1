# ============================================================
# install-trust.ps1 - 把驱动签名证书导入本机信任库
# ------------------------------------------------------------
# 需要管理员权限。做两件事：
#   Root.cer   -> LocalMachine\Root              （受信任的根证书颁发机构）
#   Driver.cer -> LocalMachine\TrustedPublisher  （受信任的发布者）
#
# 这是“不开测试模式”方案的前提：Window 内核加载器会沿证书链
# 走到本机 Root 存储；发布者证书用于放行。
#
# 注意：Secure Boot 开启 + 较新的 Windows 仍可能拒绝非 WHQL 的
# 内核驱动（内核模式签名策略更严）。被拒时先关 Secure Boot 实测。
#
# 用法（管理员）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\driversign\install-trust.ps1
# 卸载：
#   ... -Remove
# ============================================================

param(
    [string]$CertDir = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

# 提权进程的工作目录不可靠，默认路径按脚本位置解析
if ($CertDir -eq '') {
    $CertDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\driversign'
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '需要管理员权限，请以管理员身份运行 PowerShell。' -ForegroundColor Red
    exit 1
}

$root = Join-Path $CertDir 'Root.cer'
$leaf = Join-Path $CertDir 'Driver.cer'
foreach ($f in @($root, $leaf)) {
    if (-not (Test-Path $f)) {
        Write-Host "missing $f (run gen-driver-cert.ps1 first)" -ForegroundColor Red
        exit 1
    }
}

if ($Remove) {
    & certutil -delstore Root 'SaylterOS Driver Root CA'
    & certutil -delstore TrustedPublisher 'SaylterOS Driver Signing'
    Write-Host 'removed.' -ForegroundColor Green
    exit 0
}

& certutil -addstore -f Root $root
if ($LASTEXITCODE -ne 0) { Write-Host 'add Root failed.' -ForegroundColor Red; exit 1 }
& certutil -addstore -f TrustedPublisher $leaf
if ($LASTEXITCODE -ne 0) { Write-Host 'add TrustedPublisher failed.' -ForegroundColor Red; exit 1 }

& certutil -store Root 'SaylterOS Driver Root CA'
& certutil -store TrustedPublisher 'SaylterOS Driver Signing'
Write-Host 'trust installed.' -ForegroundColor Green
