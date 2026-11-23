# ============================================================
# gen-driver-cert.ps1 - 生成 Windows 内核驱动代码签名证书
# ------------------------------------------------------------
# 两级结构（便于把根证书单独放进“受信任的根证书颁发机构”）：
#   Root   - 自签根 CA，CA:TRUE，只用来签发下面的代码签名证书
#   Driver - 代码签名证书，EKU=1.3.6.1.5.5.7.3.3，CA:FALSE
#
# 输出到 build\driversign\：
#   Root.pem / Root.key / Root.cer    根证书（cer 导入 Root 存储）
#   Driver.pem / Driver.key / Driver.cer  签名证书（cer 导入 TrustedPublisher）
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\driversign\gen-driver-cert.ps1
# ============================================================

param(
    [string]$Out = 'build\driversign',
    [int]$RootDays = 7300,
    [int]$LeafDays = 3650
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

# ---- 根 CA（只用于签发驱动签名证书）----
Write-Text "$Out\root.cnf" @"
[req]
distinguished_name = dn
x509_extensions = v3_root
prompt = no

[dn]
CN = SaylterOS Driver Root CA
O = SaylterOS
C = CN

[v3_root]
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
"@

& $openssl req -new -x509 -newkey rsa:3072 -sha256 -days $RootDays -nodes `
    -keyout "$Out\Root.key" -out "$Out\Root.pem" -config "$Out\root.cnf"
if ($LASTEXITCODE -ne 0) { Write-Host 'Root generation failed.' -ForegroundColor Red; exit 1 }
& $openssl x509 -in "$Out\Root.pem" -outform DER -out "$Out\Root.cer"

# ---- 代码签名证书（EKU 1.3.6.1.5.5.7.3.3）----
Write-Text "$Out\leaf-req.cnf" @"
[req]
distinguished_name = dn
prompt = no

[dn]
CN = SaylterOS Driver Signing
O = SaylterOS
C = CN
"@

Write-Text "$Out\leaf-ext.cnf" @"
[v3_code]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
"@

& $openssl req -new -newkey rsa:3072 -sha256 -nodes `
    -keyout "$Out\Driver.key" -out "$Out\Driver.csr" -config "$Out\leaf-req.cnf"
if ($LASTEXITCODE -ne 0) { Write-Host 'Driver CSR failed.' -ForegroundColor Red; exit 1 }

& $openssl x509 -req -in "$Out\Driver.csr" -CA "$Out\Root.pem" -CAkey "$Out\Root.key" `
    -CAcreateserial -days $LeafDays -sha256 -extfile "$Out\leaf-ext.cnf" -extensions v3_code `
    -out "$Out\Driver.pem"
if ($LASTEXITCODE -ne 0) { Write-Host 'Driver signing failed.' -ForegroundColor Red; exit 1 }
& $openssl x509 -in "$Out\Driver.pem" -outform DER -out "$Out\Driver.cer"

& $openssl verify -CAfile "$Out\Root.pem" "$Out\Driver.pem"
if ($LASTEXITCODE -ne 0) { Write-Host 'Chain verify failed.' -ForegroundColor Red; exit 1 }

Remove-Item "$Out\Driver.csr" -ErrorAction SilentlyContinue

Write-Host 'Driver signing certificates generated:' -ForegroundColor Green
Get-ChildItem "$Out\*.cer" | ForEach-Object { Write-Host ("  {0}  {1} bytes" -f $_.Name, $_.Length) }
