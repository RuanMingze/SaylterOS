# ============================================================
# gen-certs.ps1 - 生成可导入 BIOS 的 UEFI 安全启动三大证书
# ------------------------------------------------------------
# 三件套（标准 UEFI Secure Boot 层级）：
#   PK  - Platform Key        平台密钥，最高级，自签
#   KEK - Key Exchange Key    由 PK 签发，用于轮换 db
#   db  - Signature Database 由 PK 签发，用于签名引导程序
#
# 输出：
#   build\secureboot\PK.cer / KEK.cer / db.cer   DER 格式，BIOS 可直接导入
#   build\secureboot\*.pem / *.key               PEM 证书与私钥（签名用）
#   build\secureboot\README.txt                  导入顺序说明
#
# 导入顺序必须是 db -> KEK -> PK：PK 一旦写入，固件就退出 Setup Mode，
# 之后再写 KEK/db 就需要签名认证。先写低层级可以全程免认证。
# ============================================================

param(
    [string]$Out = 'build\secureboot',
    [int]$Days = 7300
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null

$openssl = $null
$cmd = Get-Command openssl -ErrorAction SilentlyContinue
if ($cmd) { $openssl = $cmd.Source }
if (-not $openssl -and (Test-Path 'D:\msys2-64\ucrt64\bin\openssl.exe')) {
    $openssl = 'D:\msys2-64\ucrt64\bin\openssl.exe'
}
if (-not $openssl) {
    Write-Host 'openssl not found (PATH or D:\msys2-64\ucrt64\bin).' -ForegroundColor Red
    exit 1
}

function Write-Text($path, $text) {
    [IO.File]::WriteAllText($path, $text, [Text.Encoding]::ASCII)
}

# ---- PK 自签 CA ----
Write-Text "$Out\pk.cnf" @"
[req]
distinguished_name = dn
x509_extensions = v3_ca
prompt = no

[dn]
CN = SaylterOS Platform Key
O = SaylterOS
C = CN

[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign,digitalSignature
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
"@

& $openssl req -new -x509 -newkey rsa:3072 -sha256 -days $Days -nodes `
    -keyout "$Out\PK.key" -out "$Out\PK.pem" -config "$Out\pk.cnf" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host 'PK generation failed.' -ForegroundColor Red; exit 1 }
& $openssl x509 -in "$Out\PK.pem" -outform DER -out "$Out\PK.cer"

# ---- KEK 与 db：CSR + PK 签发 ----
Write-Text "$Out\ca-ext.cnf" @"
[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign,digitalSignature
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
"@

$entities = @(
    @{ Name = 'KEK'; CN = 'SaylterOS Key Exchange Key' },
    @{ Name = 'db';  CN = 'SaylterOS UEFI Signing CA' }
)

foreach ($e in $entities) {
    $n = $e.Name
    $cn = $e.CN
    Write-Text "$Out\$n-req.cnf" @"
[req]
distinguished_name = dn
prompt = no

[dn]
CN = $cn
O = SaylterOS
C = CN
"@

    & $openssl req -new -newkey rsa:3072 -sha256 -nodes `
        -keyout "$Out\$n.key" -out "$Out\$n.csr" -config "$Out\$n-req.cnf" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "$n CSR failed." -ForegroundColor Red; exit 1 }

    & $openssl x509 -req -in "$Out\$n.csr" -CA "$Out\PK.pem" -CAkey "$Out\PK.key" `
        -CAcreateserial -days $Days -sha256 -extfile "$Out\ca-ext.cnf" -extensions v3_ca `
        -out "$Out\$n.pem" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "$n signing failed." -ForegroundColor Red; exit 1 }
    & $openssl x509 -in "$Out\$n.pem" -outform DER -out "$Out\$n.cer"
}

Write-Text "$Out\README.txt" (Get-Content 'tools\uefi\SECUREBOOT-README.txt' -Raw -Encoding UTF8)

Write-Host 'Secure Boot certificates generated:'
Get-ChildItem "$Out\*.cer" | ForEach-Object { Write-Host ("  {0}  {1} bytes" -f $_.Name, $_.Length) }
Write-Host ("  private keys: {0}\*.key" -f $Out)

