SaylterOS UEFI Secure Boot 三大证书
===================================

文件：
  PK.cer   平台密钥（Platform Key）
  KEK.cer  密钥交换密钥（Key Exchange Key）
  db.cer   签名数据库（Signature Database）

导入到主板 BIOS/UEFI：
  1. 进入 BIOS 的 Security / Secure Boot / Key Management；
  2. 切换到 Custom / Setup Mode（清除默认密钥）；
  3. 按 db -> KEK -> PK 的顺序导入对应的 .cer（DER 格式）；
  4. 保存并重启，Secure Boot 状态应为 Enabled。

注意：
  * 顺序不能反：PK 写入后固件退出 Setup Mode，KEK/db 将需要签名认证。
  * 私钥（*.key）只用于签名引导程序，不要导入 BIOS，也不要外传。
  * 用 db.key 对 EFI\BOOT\BOOTX64.EFI 做 Authenticode 签名后，固件才会放行。
