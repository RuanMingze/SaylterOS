# ============================================================
# sign-driver.ps1 - 给内核驱动（.sys）做 Authenticode 签名
# ------------------------------------------------------------
# 使用 osslsigncode（msys2 clang64 版，带 openssl）。
# 必须用 clang64\bin 里的 osslsigncode：ucrt64 版缺 openssl DLL。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\driversign\sign-driver.ps1 `
#       -In build\drivers\SlyzFS.sys
# 输出：
#   默认写到同目录的 SlyzFS-signed.sys
# ============================================================

param(
    [Parameter(Mandatory = $true)][string]$In,
    [string]$Out = '',
    [string]$CertDir = 'build\driversign',
    [string]$Description = 'SaylterOS SlyzFS Driver'
)

$ErrorActionPreference = 'Stop'

$src = (Resolve-Path $In).Path
if ($Out -eq '') {
    $Out = [IO.Path]::Combine([IO.Path]::GetDirectoryName($src),
                              [IO.Path]::GetFileNameWithoutExtension($src) + '-signed.sys')
}

$ossl = 'D:\msys2-64\clang64\bin\osslsigncode.exe'
if (-not (Test-Path $ossl)) {
    $c = Get-Command osslsigncode -ErrorAction SilentlyContinue
    if ($c) { $ossl = $c.Source }
}
if (-not (Test-Path $ossl)) {
    Write-Host 'osslsigncode not found (expected D:\msys2-64\clang64\bin).' -ForegroundColor Red
    exit 1
}
$env:PATH = 'D:\msys2-64\clang64\bin;' + $env:PATH

$cert = Join-Path $CertDir 'Driver.pem'
$key  = Join-Path $CertDir 'Driver.key'
$root = Join-Path $CertDir 'Root.pem'
foreach ($f in @($cert, $key, $root)) {
    if (-not (Test-Path $f)) {
        Write-Host "missing $f (run tools\driversign\gen-driver-cert.ps1 first)" -ForegroundColor Red
        exit 1
    }
}

if (Test-Path $Out) { Remove-Item $Out -Force }

& $ossl sign -certs $cert -key $key -n $Description -h sha256 -in $src -out $Out
if ($LASTEXITCODE -ne 0) { Write-Host 'sign failed.' -ForegroundColor Red; exit 1 }
& $ossl verify -CAfile $root -in $Out
if ($LASTEXITCODE -ne 0) { Write-Host 'verify failed.' -ForegroundColor Red; exit 1 }

Write-Host "signed: $Out" -ForegroundColor Green
