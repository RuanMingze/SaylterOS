/* ============================================================
 * x509.h - X.509 证书解析、证书链验证与主机名校验
 * ------------------------------------------------------------
 * 只覆盖 HTTPS 客户端需要的部分：
 *   - DER 解析（TBSCertificate / SPKI / 扩展）
 *   - RSA 公钥提取与 PKCS#1 v1.5 签名验证
 *   - 信任库来自 system\certs\ca.pem（项目自带 Mozilla CA 列表）
 *   - SAN dNSName 主机名匹配（支持 *.example.com）
 *
 * 不支持 EC 公钥证书；遇到时返回明确错误，不静默放行。
 * ============================================================ */

#ifndef X509_H
#define X509_H

#include <stdint.h>

#define X509_MAX_CHAIN 4
#define X509_CA_PEM_MAX (256u * 1024u)
#define X509_DER_MAX 8192

enum {
    X509_KEY_UNKNOWN = 0,
    X509_KEY_RSA = 1,
    X509_KEY_EC = 2
};

struct X509Cert {
    const uint8_t *der;
    uint32_t der_len;

    const uint8_t *tbs;
    uint32_t tbs_len;

    const uint8_t *sig;
    uint32_t sig_len;
    int sig_hash;

    const uint8_t *issuer;
    uint32_t issuer_len;
    const uint8_t *subject;
    uint32_t subject_len;

    const uint8_t *spki;
    uint32_t spki_len;
    int key_type;

    int is_ca;
    uint32_t not_before;
    uint32_t not_after;

    const uint8_t *san;
    uint32_t san_len;

    const uint8_t *cn;
    uint32_t cn_len;
};

/* 解析一张 DER 证书。成功返回 1。 */
int x509_parse(struct X509Cert *cert, const uint8_t *der, uint32_t len);

/* 加载信任库（只加载一次）。返回信任的根数量。 */
int x509_trust_load(void);
int x509_trust_count(void);

/* 验证服务器下发的证书链。chain[0] 必须是叶子证书。
 * 成功返回 1，失败返回 0 并把原因写入 *reason（可为 NULL）。 */
int x509_verify_chain(struct X509Cert *chain, int count, const char *host,
                      const char **reason);

/* 用证书公钥验证签名（message 是待验证的原始消息，内部做摘要）。 */
int x509_verify_message(const struct X509Cert *cert,
                        const uint8_t *sig, uint32_t sig_len,
                        const uint8_t *message, uint32_t message_len,
                        int sig_alg);

/* 主机名匹配。 */
int x509_hostname_match(const struct X509Cert *cert, const char *host);

/* 当前 UTC 时间（CMOS RTC，秒）。 */
uint32_t x509_now_unix(void);

/* 签名算法编号（TLS 1.3 signature_scheme）。 */
#define X509_SIG_RSA_PSS_RSAE_SHA256 0x0804
#define X509_SIG_RSA_PKCS1_SHA256    0x0401
#define X509_SIG_ECDSA_P256_SHA256   0x0403
#define X509_SIG_ECDSA_P384_SHA384   0x0503

#endif /* X509_H */
