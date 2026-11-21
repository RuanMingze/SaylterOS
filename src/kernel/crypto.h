/* ============================================================
 * crypto.h - 密码学原语（无 libc / 无外部依赖）
 * ------------------------------------------------------------
 * TLS 1.3 客户端需要的最小集合：
 *   - SHA-1 / SHA-256 / SHA-384 / SHA-512（证书链签名）
 *   - HMAC-SHA256 与 HKDF（TLS 1.3 密钥调度）
 *   - AES-128-GCM（TLS_AES_128_GCM_SHA256）
 *   - X25519（密钥交换）
 *   - RSA PKCS#1 v1.5 / PSS 验证（证书与 CertificateVerify）
 *
 * 所有实现都是纯整数运算，不使用 SSE/x87。大数使用 32 位 limb，
 * 乘法依赖 x86_64 的 64 位乘法，不需要 libgcc。
 * ============================================================ */

#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>

/* ============================================================
 * 哈希
 * ============================================================ */

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t fill;
};

void sha256_init(struct Sha256Ctx *ctx);
void sha256_update(struct Sha256Ctx *ctx, const void *data, uint32_t len);
void sha256_final(struct Sha256Ctx *ctx, uint8_t out[32]);
void sha256_buf(const void *data, uint32_t len, uint8_t out[32]);

void sha1_buf(const void *data, uint32_t len, uint8_t out[20]);
void sha384_buf(const void *data, uint32_t len, uint8_t out[48]);
void sha512_buf(const void *data, uint32_t len, uint8_t out[64]);

/* ============================================================
 * HMAC / HKDF（TLS 1.3）
 * ============================================================ */

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *data, uint32_t data_len,
                 uint8_t out[32]);

/* HKDF-Extract(salt, ikm) -> prk */
void hkdf_extract(const uint8_t *salt, uint32_t salt_len,
                  const uint8_t *ikm, uint32_t ikm_len,
                  uint8_t prk[32]);

/* HKDF-Expand(prk, info, out_len)。out_len 上限 255*32。成功返回 1。 */
int hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t info_len,
                uint8_t *out, uint32_t out_len);

/* TLS 1.3 的 HKDF-Expand-Label。成功返回 1。 */
int tls13_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, uint32_t context_len,
                       uint8_t *out, uint32_t out_len);

/* HKDF-Extract 的 TLS 1.3 别名（salt 可以为空）。 */
void tls13_extract(const uint8_t *salt, uint32_t salt_len,
                   const uint8_t *ikm, uint32_t ikm_len,
                   uint8_t out[32]);

/* 随机数：基于 TSC 与内部状态混合，满足握手随机数与 PSS 盐需求。 */
void crypto_random(uint8_t *out, uint32_t len);

/* 常数时间比较，返回 1 表示相等。 */
int crypto_equal(const uint8_t *a, const uint8_t *b, uint32_t len);

/* ============================================================
 * Base64（PEM 证书解码）
 * ============================================================ */

/* 解码 base64；忽略空白。返回字节数，失败 -1。 */
int crypto_base64_decode(const char *in, uint32_t in_len,
                         uint8_t *out, uint32_t cap);

/* ============================================================
 * AES-128-GCM
 * ============================================================ */

struct Aes128 {
    uint8_t round_key[176];
};

void aes128_init(struct Aes128 *ctx, const uint8_t key[16]);
void aes128_encrypt_block(const struct Aes128 *ctx,
                          const uint8_t in[16], uint8_t out[16]);

/* 加密：ct 与 pt 可以相同。返回 1。 */
int aes128_gcm_encrypt(const uint8_t key[16], const uint8_t *iv, uint32_t iv_len,
                       const uint8_t *aad, uint32_t aad_len,
                       const uint8_t *pt, uint32_t pt_len,
                       uint8_t *ct, uint8_t tag[16]);

/* 解密并验证 tag，tag 不符返回 0 且不写 pt。 */
int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t *iv, uint32_t iv_len,
                       const uint8_t *aad, uint32_t aad_len,
                       const uint8_t *ct, uint32_t ct_len,
                       const uint8_t tag[16], uint8_t *pt);

/* ============================================================
 * X25519
 * ============================================================ */

/* 由 32 字节私钥（已 clamp 或未 clamp 均可）生成公钥。 */
void x25519_public(uint8_t out[32], const uint8_t scalar[32]);

/* 计算共享密钥。失败（结果为全零）返回 0。 */
int x25519_shared(uint8_t out[32], const uint8_t scalar[32],
                  const uint8_t peer[32]);

/* ============================================================
 * RSA 验证
 * ============================================================ */

#define CRYPTO_BN_LIMBS 128   /* 4096 bit */

/* 校验 PKCS#1 v1.5 签名。
 * n/e 为大端无符号整数，hash 为对应摘要，hash_id 见 CRYPTO_HASH_*。 */
#define CRYPTO_HASH_SHA1   1
#define CRYPTO_HASH_SHA256 2
#define CRYPTO_HASH_SHA384 3
#define CRYPTO_HASH_SHA512 4

int rsa_verify_pkcs1(const uint8_t *n, uint32_t n_len,
                     const uint8_t *e, uint32_t e_len,
                     const uint8_t *sig, uint32_t sig_len,
                     const uint8_t *digest, uint32_t digest_len,
                     int hash_id);

/* 校验 RSA-PSS（MGF1 + salt 长度等于摘要长度）。
 * 用于 TLS 1.3 CertificateVerify 的 rsa_pss_rsae_sha256。 */
int rsa_verify_pss_sha256(const uint8_t *n, uint32_t n_len,
                          const uint8_t *e, uint32_t e_len,
                          const uint8_t *sig, uint32_t sig_len,
                          const uint8_t digest[32]);

/* ============================================================
 * ECDSA P-256（prime256v1）验证
 * ------------------------------------------------------------
 * 输入都是大端 32 字节：r、s、摘要、公钥 X/Y。
 * 用于证书链签名与 TLS 1.3 CertificateVerify 的
 * ecdsa_secp256r1_sha256。
 * ============================================================ */
int ecdsa_p256_verify_digest(const uint8_t r_be[32], const uint8_t s_be[32],
                             const uint8_t digest[32],
                             const uint8_t qx_be[32], const uint8_t qy_be[32]);

/* ECDSA P-384（secp384r1）。 */
int ecdsa_p384_verify_digest(const uint8_t r_be[48], const uint8_t s_be[48],
                             const uint8_t digest[48],
                             const uint8_t qx_be[48], const uint8_t qy_be[48]);

/* 校验已知答案测试向量。返回失败项数（0 表示全部通过）。 */
int crypto_selftest(void);

#endif /* CRYPTO_H */
