/* ============================================================
 * tls.c - TLS 1.3 客户端
 * ------------------------------------------------------------
 * 设计：
 *   - 只实现 TLS 1.3 + TLS_AES_128_GCM_SHA256 + X25519；
 *   - 证书链验证交给 x509.c，信任库来自 system\certs\ca.pem；
 *   - 记录层严格按 seq 递增，握手阶段与应用阶段的密钥分开；
 *   - 不实现会话恢复（PSK / 0-RTT），每次全量握手。
 *
 * 边界：服务器若只支持 TLS 1.2、ECDSA 证书或非 X25519 组，
 * 会明确报错，不会降级到不安全的连接。
 * ============================================================ */

#include "tls.h"
#include "crypto.h"
#include "x509.h"
#include "net.h"
#include "tty.h"

#define TLS_RECORD_MAX      18432u
#define TLS_HANDSHAKE_MAX   40960u
#define TLS_TRANSCRIPT_MAX  49152u
#define TLS_CERT_BODY_MAX   24576u

#define TLS_CT_HANDSHAKE    22
#define TLS_CT_ALERT        21
#define TLS_CT_CHANGE_CIPHER 20
#define TLS_CT_APPLICATION  23

#define TLS_HS_CLIENT_HELLO  1
#define TLS_HS_SERVER_HELLO  2
#define TLS_HS_ENCRYPTED_EXT 8
#define TLS_HS_CERTIFICATE   11
#define TLS_HS_CERT_REQUEST  13
#define TLS_HS_CERT_VERIFY   15
#define TLS_HS_FINISHED      20
#define TLS_HS_NEW_TICKET    4

static int g_open = 0;
static int g_verify = 1;
static int g_debug = 0;
static int g_in_app_keys = 0;
static int g_out_app_keys = 0;
static int g_server_hs_ready = 0;

static uint8_t g_client_hs_key[16];
static uint8_t g_client_hs_iv[12];
static uint8_t g_server_hs_key[16];
static uint8_t g_server_hs_iv[12];
static uint8_t g_client_app_key[16];
static uint8_t g_client_app_iv[12];
static uint8_t g_server_app_key[16];
static uint8_t g_server_app_iv[12];
static uint8_t g_client_hs_secret[32];
static uint8_t g_server_hs_secret[32];

static uint64_t g_client_seq = 0;
static uint64_t g_server_seq = 0;

static uint8_t g_tr[TLS_TRANSCRIPT_MAX];
static uint32_t g_tr_len = 0;

static uint8_t g_hs[TLS_HANDSHAKE_MAX];
static uint32_t g_hs_len = 0;

static uint8_t g_record[TLS_RECORD_MAX];
static uint8_t g_plain[TLS_RECORD_MAX];
static uint8_t g_outbuf[TLS_RECORD_MAX];

static uint8_t g_cert_body[TLS_CERT_BODY_MAX];

static uint8_t g_app_pending[TLS_RECORD_MAX];
static uint32_t g_app_pending_len = 0;
static uint32_t g_app_pending_off = 0;

static char g_error[96];

static void tls_set_error(const char *text) {
    int i = 0;
    while (text && text[i] && i < (int)sizeof(g_error) - 1) {
        g_error[i] = text[i];
        i++;
    }
    g_error[i] = '\0';
}

static void tls_dump(const char *label, const uint8_t *p, uint32_t n) {
    printk("* %s=", label);
    for (uint32_t i = 0; i < n; i++) printk("%x ", (unsigned int)p[i]);
    printk("\n");
}

static void tls_memzero(void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- != 0) *d++ = 0;
}

static void tls_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- != 0) *d++ = *s++;
}

static uint32_t rd16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t rd24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* ============================================================
 * TCP 读写
 * ============================================================ */

static int tcp_send_all(const uint8_t *data, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > 4096u) chunk = 4096u;
        if (net_tcp_send(data + off, chunk, 10000u) < 0) return 0;
        off += chunk;
    }
    return 1;
}

static int tcp_recv_exact(uint8_t *buf, uint32_t len, uint32_t timeout_ms) {
    uint32_t got = 0;
    while (got < len) {
        int n = net_tcp_recv(buf + got, len - got, timeout_ms);
        if (n <= 0) return 0;
        got += (uint32_t)n;
    }
    return 1;
}

/* ============================================================
 * 记录层
 * ============================================================ */

static void make_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    for (int i = 0; i < 12; i++) nonce[i] = iv[i];
    for (int i = 0; i < 8; i++) {
        nonce[11 - i] ^= (uint8_t)(seq >> (i * 8));
    }
}

static int send_record(uint8_t inner_type, const uint8_t *data, uint32_t len,
                       int encrypted, const uint8_t *key, const uint8_t *iv,
                       uint64_t *seq) {
    if (!encrypted) {
        if (len + 5 > TLS_RECORD_MAX) return 0;
        g_outbuf[0] = inner_type;
        g_outbuf[1] = 0x03;
        g_outbuf[2] = 0x03;
        wr16(g_outbuf + 3, len);
        tls_memcpy(g_outbuf + 5, data, len);
        return tcp_send_all(g_outbuf, len + 5);
    }

    uint32_t pt_len = len + 1;
    if (pt_len + 5 + 16 > TLS_RECORD_MAX) return 0;

    if (pt_len > TLS_RECORD_MAX) return 0;

    static uint8_t pt[TLS_RECORD_MAX];
    tls_memcpy(pt, data, len);
    pt[len] = inner_type;

    g_outbuf[0] = TLS_CT_APPLICATION;
    g_outbuf[1] = 0x03;
    g_outbuf[2] = 0x03;
    wr16(g_outbuf + 3, pt_len + 16);

    uint8_t nonce[12];
    make_nonce(iv, *seq, nonce);

    uint8_t tag[16];
    if (!aes128_gcm_encrypt(key, nonce, 12, g_outbuf, 5,
                            pt, pt_len, g_outbuf + 5, tag)) {
        return 0;
    }
    tls_memcpy(g_outbuf + 5 + pt_len, tag, 16);

    (*seq)++;
    return tcp_send_all(g_outbuf, 5 + pt_len + 16);
}

static int read_raw_record(uint8_t *type, uint8_t *payload, uint32_t *len) {
    uint8_t hdr[5];
    if (!tcp_recv_exact(hdr, 5, 15000u)) return 0;

    uint32_t l = rd16(hdr + 3);
    if (l > TLS_RECORD_MAX) {
        tls_set_error("TLS record too large");
        return -1;
    }
    if (!tcp_recv_exact(payload, l, 15000u)) return 0;

    *type = hdr[0];
    *len = l;
    return 1;
}

/* 读取一条记录；自动忽略 ChangeCipherSpec 与告警。
 * 返回 1 成功，0 对端关闭，-1 错误。 */
static int read_record(uint8_t *type, uint8_t *payload, uint32_t *len) {
    for (;;) {
        uint8_t hdr[5];
        if (!tcp_recv_exact(hdr, 5, 20000u)) {
            tls_set_error("connection closed");
            return 0;
        }

        uint32_t l = rd16(hdr + 3);
        if (l > TLS_RECORD_MAX) {
            tls_set_error("TLS record too large");
            return -1;
        }
        if (!tcp_recv_exact(payload, l, 20000u)) {
            tls_set_error("connection closed");
            return 0;
        }

        uint8_t outer = hdr[0];
        if (outer == TLS_CT_CHANGE_CIPHER) {
            continue;
        }
        if (outer == TLS_CT_ALERT) {
            if (l >= 2) {
                uint32_t d = payload[1];
                const char *name = "TLS alert";
                switch (d) {
                case 0:   name = "close_notify"; break;
                case 10:  name = "unexpected_message"; break;
                case 20:  name = "bad_record_mac"; break;
                case 22:  name = "record_overflow"; break;
                case 40:  name = "handshake_failure"; break;
                case 42:  name = "bad_certificate"; break;
                case 43:  name = "unsupported_certificate"; break;
                case 44:  name = "certificate_revoked"; break;
                case 45:  name = "certificate_expired"; break;
                case 46:  name = "certificate_unknown"; break;
                case 47:  name = "illegal_parameter"; break;
                case 48:  name = "unknown_ca"; break;
                case 49:  name = "access_denied"; break;
                case 50:  name = "decode_error"; break;
                case 51:  name = "decrypt_error"; break;
                case 70:  name = "protocol_version"; break;
                case 71:  name = "insufficient_security"; break;
                case 80:  name = "internal_error"; break;
                case 86:  name = "inappropriate_fallback"; break;
                case 90:  name = "user_canceled"; break;
                case 100: name = "no_renegotiation"; break;
                default:  name = "TLS alert"; break;
                }
                tls_set_error(name);
            } else {
                tls_set_error("TLS alert");
            }
            return -1;
        }

        if (outer == TLS_CT_APPLICATION && g_in_app_keys) {
            if (l < 17) {
                tls_set_error("short encrypted record");
                return -1;
            }

            uint8_t nonce[12];
            make_nonce(g_server_app_iv, g_server_seq, nonce);

            if (!aes128_gcm_decrypt(g_server_app_key, nonce, 12,
                                    hdr, 5, payload, l - 16,
                                    payload + l - 16, g_plain)) {
                tls_set_error("TLS record authentication failed");
                return -1;
            }
            g_server_seq++;

            uint32_t pt_len = l - 16;
            uint8_t inner = g_plain[pt_len - 1];
            *type = inner;
            *len = pt_len - 1;
            tls_memcpy(payload, g_plain, pt_len - 1);
            return 1;
        }

        if (outer == TLS_CT_APPLICATION && !g_in_app_keys &&
            g_server_hs_ready) {
            /* 握手阶段的加密记录 */
            if (l < 17) {
                tls_set_error("short encrypted record");
                return -1;
            }

            uint8_t nonce[12];
            make_nonce(g_server_hs_iv, g_server_seq, nonce);

            if (!aes128_gcm_decrypt(g_server_hs_key, nonce, 12,
                                    hdr, 5, payload, l - 16,
                                    payload + l - 16, g_plain)) {
                tls_set_error("handshake record authentication failed");
                return -1;
            }
            g_server_seq++;

            uint32_t pt_len = l - 16;
            uint8_t inner = g_plain[pt_len - 1];
            *type = inner;
            *len = pt_len - 1;
            tls_memcpy(payload, g_plain, pt_len - 1);
            return 1;
        }

        *type = outer;
        *len = l;
        return 1;
    }
}

/* ============================================================
 * 握手消息组装与转录
 * ============================================================ */

static void transcript_add(const uint8_t *msg, uint32_t len) {
    if (g_tr_len + len > TLS_TRANSCRIPT_MAX) return;
    tls_memcpy(g_tr + g_tr_len, msg, len);
    g_tr_len += len;
}

static void transcript_hash(uint8_t out[32]) {
    sha256_buf(g_tr, g_tr_len, out);
}

static int hs_fill(void) {
    uint8_t type = 0;
    uint32_t len = 0;
    if (g_hs_len >= TLS_HANDSHAKE_MAX) return -1;

    int rc = read_record(&type, g_hs + g_hs_len, &len);
    if (rc <= 0) return rc;
    if (type != TLS_CT_HANDSHAKE) {
        tls_set_error("expected handshake record");
        return -1;
    }
    g_hs_len += len;
    return 1;
}

static int hs_next(uint8_t *msg_type, uint8_t **msg, uint32_t *msg_len) {
    for (;;) {
        while (g_hs_len < 4) {
            int rc = hs_fill();
            if (rc <= 0) return rc;
        }

        uint32_t body_len = rd24(g_hs + 1);
        if (body_len + 4u > TLS_HANDSHAKE_MAX) {
            tls_set_error("handshake message too large");
            return -1;
        }

        while (g_hs_len < body_len + 4u) {
            int rc = hs_fill();
            if (rc <= 0) return rc;
        }

        *msg_type = g_hs[0];
        *msg = g_hs;
        *msg_len = body_len + 4u;
        return 1;
    }
}

static void hs_consume(uint32_t len) {
    if (len >= g_hs_len) {
        g_hs_len = 0;
        return;
    }
    uint32_t remain = g_hs_len - len;
    for (uint32_t i = 0; i < remain; i++) g_hs[i] = g_hs[len + i];
    g_hs_len = remain;
}

/* ============================================================
 * ClientHello
 * ============================================================ */

static uint8_t g_client_priv[32];
static uint8_t g_client_pub[32];

static uint32_t build_client_hello(uint8_t *out, const char *host) {
    uint32_t host_len = 0;
    while (host[host_len]) host_len++;

    uint32_t p = 0;
    out[p++] = TLS_HS_CLIENT_HELLO;
    out[p++] = 0; out[p++] = 0; out[p++] = 0;   /* 长度稍后回填 */

    out[p++] = 0x03; out[p++] = 0x03;           /* legacy_version */

    crypto_random(out + p, 32);
    p += 32;

    out[p++] = 32;                              /* legacy_session_id */
    crypto_random(out + p, 32);
    p += 32;

    wr16(out + p, 2); p += 2;                   /* cipher_suites */
    out[p++] = 0x13; out[p++] = 0x01;           /* TLS_AES_128_GCM_SHA256 */

    out[p++] = 1;                               /* compression_methods */
    out[p++] = 0;

    uint32_t ext_len_pos = p;
    wr16(out + p, 0); p += 2;
    uint32_t ext_start = p;

    /* server_name */
    wr16(out + p, 0x0000); p += 2;
    wr16(out + p, (uint32_t)(host_len + 5)); p += 2;
    wr16(out + p, (uint32_t)(host_len + 3)); p += 2;
    out[p++] = 0;
    wr16(out + p, host_len); p += 2;
    for (uint32_t i = 0; i < host_len; i++) out[p++] = (uint8_t)host[i];

    /* supported_groups: x25519 */
    wr16(out + p, 0x000A); p += 2;
    wr16(out + p, 4); p += 2;
    wr16(out + p, 2); p += 2;
    wr16(out + p, 0x001D); p += 2;

    /* signature_algorithms */
    wr16(out + p, 0x000D); p += 2;
    wr16(out + p, 8); p += 2;                   /* 2 字节列表长度 + 6 字节算法 */
    wr16(out + p, 6); p += 2;
    wr16(out + p, 0x0403); p += 2;              /* ecdsa_secp256r1_sha256 */
    wr16(out + p, 0x0503); p += 2;              /* ecdsa_secp384r1_sha384 */
    wr16(out + p, 0x0804); p += 2;              /* rsa_pss_rsae_sha256 */

    /* supported_versions: TLS 1.3 */
    wr16(out + p, 0x002B); p += 2;
    wr16(out + p, 3); p += 2;
    out[p++] = 2;
    wr16(out + p, 0x0304); p += 2;

    /* key_share: x25519 */
    wr16(out + p, 0x0033); p += 2;
    wr16(out + p, 38); p += 2;
    wr16(out + p, 36); p += 2;
    wr16(out + p, 0x001D); p += 2;
    wr16(out + p, 32); p += 2;
    tls_memcpy(out + p, g_client_pub, 32);
    p += 32;

    wr16(out + ext_len_pos, p - ext_start);

    uint32_t body_len = p - 4;
    out[1] = (uint8_t)(body_len >> 16);
    out[2] = (uint8_t)(body_len >> 8);
    out[3] = (uint8_t)body_len;

    return p;
}

/* ============================================================
 * 密钥调度
 * ============================================================ */

static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t th[32], uint8_t out[32]) {
    tls13_expand_label(secret, label, th, 32, out, 32);
}

static void traffic_keys(const uint8_t secret[32],
                         uint8_t key[16], uint8_t iv[12]) {
    tls13_expand_label(secret, "key", 0, 0, key, 16);
    tls13_expand_label(secret, "iv", 0, 0, iv, 12);
}

/* ============================================================
 * ServerHello
 * ============================================================ */

static int is_hello_retry(const uint8_t *random) {
    static const uint8_t hrr[32] = {
        0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,
        0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,
        0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C
    };
    return crypto_equal(random, hrr, 32);
}

static int parse_server_hello(const uint8_t *msg, uint32_t msg_len,
                              uint8_t *shared, int *have_shared) {
    if (msg_len < 4 + 2 + 32 + 1) return 0;

    const uint8_t *p = msg + 4;
    uint32_t left = msg_len - 4;

    p += 2;
    left -= 2;

    const uint8_t *random = p;
    p += 32;
    left -= 32;

    if (is_hello_retry(random)) {
        tls_set_error("server requested hello retry (unsupported group)");
        return 0;
    }

    uint32_t sid_len = *p++;
    left--;
    if (sid_len > left) return 0;
    p += sid_len;
    left -= sid_len;

    if (left < 3) return 0;
    uint32_t cipher = rd16(p);
    p += 2;
    left -= 2;
    p++;
    left--;

    if (cipher != 0x1301) {
        tls_set_error("server selected an unsupported cipher suite");
        return 0;
    }

    if (left < 2) return 0;
    uint32_t ext_total = rd16(p);
    p += 2;
    left -= 2;
    if (ext_total > left) return 0;

    uint32_t off = 0;
    int version_ok = 0;
    int group = 0;

    while (off + 4 <= ext_total) {
        uint32_t type = rd16(p + off);
        uint32_t len = rd16(p + off + 2);
        off += 4;
        if (off + len > ext_total) return 0;

        const uint8_t *v = p + off;

        if (type == 0x002B) {
            if (len == 2 && rd16(v) == 0x0304) version_ok = 1;
        } else if (type == 0x0033) {
            if (len < 4) return 0;
            group = (int)rd16(v);
            uint32_t key_len = rd16(v + 2);
            if (group != 0x001D || key_len != 32 || len != 4 + 32) {
                tls_set_error("server selected an unsupported key share group");
                return 0;
            }
            if (!x25519_shared(shared, g_client_priv, v + 4)) {
                tls_set_error("invalid x25519 key share");
                return 0;
            }
            *have_shared = 1;
        }

        off += len;
    }

    if (!version_ok) {
        tls_set_error("server did not negotiate TLS 1.3");
        return 0;
    }
    if (!*have_shared) {
        tls_set_error("server did not send an x25519 key share");
        return 0;
    }

    (void)group;
    return 1;
}

/* ============================================================
 * 证书链解析
 * ============================================================ */

static int parse_certificate(uint8_t *body, uint32_t body_len,
                             struct X509Cert *chain, int *chain_count) {
    if (g_debug) {
        printk("* Certificate body=%u\n", body_len);
        if (body_len >= 32) {
            tls_dump("cbody[0..15]", body, 16);
        }
        if (body_len >= 1032) {
            tls_dump("cbody[1000..1031]", body + 1000, 32);
        }
    }
    if (body_len < 4) return 0;

    uint32_t ctx_len = body[0];
    if (1 + ctx_len + 3 > body_len) return 0;
    uint32_t p = 1 + ctx_len;

    uint32_t list_len = rd24(body + p);
    p += 3;
    if (p + list_len > body_len) return 0;

    int count = 0;
    uint32_t off = 0;

    while (off + 3 <= list_len && count < X509_MAX_CHAIN) {
        uint32_t clen = rd24(body + p + off);
        off += 3;
        if (off + clen > list_len) return 0;

        int parsed = x509_parse(&chain[count], body + p + off, clen);
        if (g_debug) {
            printk("* cert[%d] off=%u len=%u parse=%d\n", count,
                   (unsigned int)(off - 3), (unsigned int)clen, parsed);
        }
        if (!parsed) {
            tls_set_error("malformed certificate");
            return 0;
        }
        count++;
        off += clen;

        /* TLS 1.3 CertificateEntry 在证书数据之后还有一个
         * 2 字节的 extensions 长度字段（通常为 0）。 */
        if (off + 2 > list_len) break;
        uint32_t ext_len = rd16(body + p + off);
        off += 2;
        if (off + ext_len > list_len) {
            tls_set_error("malformed certificate entry");
            return 0;
        }
        off += ext_len;
    }

    if (count == 0) {
        tls_set_error("server sent an empty certificate chain");
        return 0;
    }

    *chain_count = count;
    return 1;
}

/* ============================================================
 * 连接
 * ============================================================ */

static uint32_t g_cert_request_ctx_len = 0;
static uint8_t g_cert_request_ctx[32];
static int g_cert_requested = 0;

static void reset_state(void) {
    tls_memzero(g_client_hs_key, sizeof(g_client_hs_key));
    tls_memzero(g_client_hs_iv, sizeof(g_client_hs_iv));
    tls_memzero(g_server_hs_key, sizeof(g_server_hs_key));
    tls_memzero(g_server_hs_iv, sizeof(g_server_hs_iv));
    tls_memzero(g_client_app_key, sizeof(g_client_app_key));
    tls_memzero(g_client_app_iv, sizeof(g_client_app_iv));
    tls_memzero(g_server_app_key, sizeof(g_server_app_key));
    tls_memzero(g_server_app_iv, sizeof(g_server_app_iv));
    tls_memzero(g_client_hs_secret, sizeof(g_client_hs_secret));
    tls_memzero(g_server_hs_secret, sizeof(g_server_hs_secret));
    g_client_seq = 0;
    g_server_seq = 0;
    g_tr_len = 0;
    g_hs_len = 0;
    g_app_pending_len = 0;
    g_app_pending_off = 0;
    g_cert_request_ctx_len = 0;
    g_cert_requested = 0;
    g_server_hs_ready = 0;
    g_in_app_keys = 0;
    g_out_app_keys = 0;
    g_error[0] = '\0';
}

int tls_connect(const char *host, uint16_t port, int verify) {
    uint32_t ip = 0;
    if (!net_dns_resolve(host, &ip, 8000u)) {
        tls_close();
        reset_state();
        tls_set_error("DNS resolution failed");
        return TLS_ERR_IO;
    }
    return tls_connect_addr(host, ip, port, verify);
}

int tls_connect_addr(const char *host, uint32_t ip, uint16_t port, int verify) {
    tls_close();
    reset_state();

    g_verify = verify;

    if (net_tcp_connect(ip, port, 8000u) != 0) {
        tls_set_error("TCP connection failed");
        return TLS_ERR_IO;
    }

    crypto_random(g_client_priv, 32);
    x25519_public(g_client_pub, g_client_priv);

    uint8_t ch[TLS_HANDSHAKE_MAX];
    uint32_t ch_len = build_client_hello(ch, host);

    if (g_debug) {
        printk("* ClientHello len=%u\n* ", ch_len);
        for (uint32_t i = 0; i < ch_len; i++) {
            printk("%x ", (unsigned int)ch[i]);
        }
        printk("\n");
    }

    if (!send_record(TLS_CT_HANDSHAKE, ch, ch_len, 0, 0, 0, 0)) {
        tls_set_error("failed to send ClientHello");
        net_tcp_close();
        return TLS_ERR_IO;
    }
    transcript_add(ch, ch_len);

    /* TLS 1.3 兼容性 ChangeCipherSpec：部分中间设备需要看到它。 */
    {
        static const uint8_t ccs[1] = { 0x01 };
        if (!send_record(TLS_CT_CHANGE_CIPHER, ccs, 1, 0, 0, 0, 0)) {
            tls_set_error("failed to send ChangeCipherSpec");
            net_tcp_close();
            return TLS_ERR_IO;
        }
    }

    /* ---- ServerHello ---- */
    uint8_t msg_type = 0;
    uint8_t *msg = 0;
    uint32_t msg_len = 0;

    int rc = hs_next(&msg_type, &msg, &msg_len);
    if (rc <= 0) {
        net_tcp_close();
        return TLS_ERR_IO;
    }
    if (msg_type != TLS_HS_SERVER_HELLO) {
        tls_set_error("expected ServerHello");
        net_tcp_close();
        return TLS_ERR_PROTOCOL;
    }

    uint8_t shared[32];
    int have_shared = 0;

    if (g_debug) {
        printk("* ServerHello len=%u\n* ", msg_len);
        for (uint32_t i = 0; i < msg_len; i++) {
            printk("%x ", (unsigned int)msg[i]);
        }
        printk("\n");
    }

    if (!parse_server_hello(msg, msg_len, shared, &have_shared)) {
        net_tcp_close();
        return TLS_ERR_UNSUPPORTED;
    }

    transcript_add(msg, msg_len);
    hs_consume(msg_len);

    /* ---- 握手密钥 ---- */
    uint8_t zeros[32];
    tls_memzero(zeros, 32);

    uint8_t early[32];
    hkdf_extract(0, 0, zeros, 32, early);

    uint8_t empty_hash[32];
    sha256_buf("", 0, empty_hash);

    uint8_t derived[32];
    derive_secret(early, "derived", empty_hash, derived);

    uint8_t handshake_secret[32];
    hkdf_extract(derived, 32, shared, 32, handshake_secret);

    uint8_t th[32];
    transcript_hash(th);

    derive_secret(handshake_secret, "c hs traffic", th, g_client_hs_secret);
    derive_secret(handshake_secret, "s hs traffic", th, g_server_hs_secret);

    traffic_keys(g_client_hs_secret, g_client_hs_key, g_client_hs_iv);
    traffic_keys(g_server_hs_secret, g_server_hs_key, g_server_hs_iv);
    g_server_hs_ready = 1;

    if (g_debug) {
        tls_dump("x25519 shared", shared, 32);
        tls_dump("transcript hash", th, 32);
        tls_dump("s hs secret", g_server_hs_secret, 32);
        tls_dump("s hs key", g_server_hs_key, 16);
        tls_dump("s hs iv", g_server_hs_iv, 12);
    }

    g_client_seq = 0;
    g_server_seq = 0;

    /* ---- 服务器握手消息 ---- */
    struct X509Cert chain[X509_MAX_CHAIN];
    int chain_count = 0;
    int server_finished = 0;
    int cert_seen = 0;

    while (!server_finished) {
        rc = hs_next(&msg_type, &msg, &msg_len);
        if (rc <= 0) {
            net_tcp_close();
            return TLS_ERR_IO;
        }

        if (msg_type == TLS_HS_ENCRYPTED_EXT) {
            transcript_add(msg, msg_len);
            hs_consume(msg_len);
            continue;
        }

        if (msg_type == TLS_HS_CERT_REQUEST) {
            g_cert_requested = 1;
            g_cert_request_ctx_len = 0;
            if (msg_len > 5) {
                uint32_t clen = msg[4];
                if (clen > 32) clen = 32;
                if (5 + clen <= msg_len) {
                    g_cert_request_ctx_len = clen;
                    for (uint32_t i = 0; i < clen; i++) {
                        g_cert_request_ctx[i] = msg[5 + i];
                    }
                }
            }
            transcript_add(msg, msg_len);
            hs_consume(msg_len);
            continue;
        }

        if (msg_type == TLS_HS_CERTIFICATE) {
            uint32_t body_len = msg_len - 4;
            if (body_len > TLS_CERT_BODY_MAX) {
                tls_set_error("certificate message too large");
                net_tcp_close();
                return TLS_ERR_PROTOCOL;
            }
            tls_memcpy(g_cert_body, msg + 4, body_len);

            if (!parse_certificate(g_cert_body, body_len, chain, &chain_count)) {
                net_tcp_close();
                return TLS_ERR_VERIFY;
            }
            cert_seen = 1;
            transcript_add(msg, msg_len);
            hs_consume(msg_len);
            continue;
        }

        if (msg_type == TLS_HS_CERT_VERIFY) {
            if (!cert_seen) {
                tls_set_error("CertificateVerify without certificate");
                net_tcp_close();
                return TLS_ERR_PROTOCOL;
            }

            uint32_t body_len = msg_len - 4;
            const uint8_t *p = msg + 4;
            if (body_len < 4) {
                tls_set_error("malformed CertificateVerify");
                net_tcp_close();
                return TLS_ERR_PROTOCOL;
            }

            int sig_alg = (int)rd16(p);
            uint32_t sig_len = rd16(p + 2);
            if (sig_len + 4 > body_len) {
                tls_set_error("malformed CertificateVerify signature");
                net_tcp_close();
                return TLS_ERR_PROTOCOL;
            }
            const uint8_t *sig = p + 4;

            uint8_t expect_hash[32];
            transcript_hash(expect_hash);

            static const char context[] = "TLS 1.3, server CertificateVerify";
            uint8_t content[64 + 33 + 1 + 32];
            for (int i = 0; i < 64; i++) content[i] = 0x20;
            for (int i = 0; i < 33; i++) content[64 + i] = (uint8_t)context[i];
            content[64 + 33] = 0x00;
            tls_memcpy(content + 64 + 34, expect_hash, 32);

            if (!x509_verify_message(&chain[0], sig, sig_len,
                                     content, (uint32_t)sizeof(content),
                                     sig_alg)) {
                tls_set_error("CertificateVerify signature invalid");
                net_tcp_close();
                return TLS_ERR_VERIFY;
            }

            transcript_add(msg, msg_len);
            hs_consume(msg_len);
            continue;
        }

        if (msg_type == TLS_HS_FINISHED) {
            uint32_t body_len = msg_len - 4;
            if (body_len != 32) {
                tls_set_error("malformed Finished message");
                net_tcp_close();
                return TLS_ERR_PROTOCOL;
            }

            uint8_t tr_hash[32];
            transcript_hash(tr_hash);

            uint8_t finished_key[32];
            tls13_expand_label(g_server_hs_secret, "finished", 0, 0,
                               finished_key, 32);

            uint8_t expect[32];
            hmac_sha256(finished_key, 32, tr_hash, 32, expect);

            if (!crypto_equal(expect, msg + 4, 32)) {
                tls_set_error("server Finished verification failed");
                net_tcp_close();
                return TLS_ERR_VERIFY;
            }

            transcript_add(msg, msg_len);
            hs_consume(msg_len);
            server_finished = 1;
            continue;
        }

        tls_set_error("unexpected handshake message");
        net_tcp_close();
        return TLS_ERR_PROTOCOL;
    }

    /* 证书链验证放在 Finished 之后：先确认握手完整性，再检查身份。 */
    if (g_verify) {
        const char *reason = "";
        if (!x509_verify_chain(chain, chain_count, host, &reason)) {
            tls_set_error(reason && reason[0] ? reason : "certificate verification failed");
            net_tcp_close();
            return TLS_ERR_VERIFY;
        }
    }

    /* ---- 应用密钥 ---- */
    uint8_t master_derived[32];
    derive_secret(handshake_secret, "derived", empty_hash, master_derived);

    uint8_t master[32];
    hkdf_extract(master_derived, 32, zeros, 32, master);

    uint8_t app_th[32];
    transcript_hash(app_th);

    uint8_t client_app_secret[32];
    uint8_t server_app_secret[32];
    derive_secret(master, "c ap traffic", app_th, client_app_secret);
    derive_secret(master, "s ap traffic", app_th, server_app_secret);

    traffic_keys(client_app_secret, g_client_app_key, g_client_app_iv);
    traffic_keys(server_app_secret, g_server_app_key, g_server_app_iv);

    /* ---- 客户端认证（无证书时发送空 Certificate） ---- */
    if (g_cert_requested) {
        uint8_t cert_msg[8];
        uint32_t p = 0;
        cert_msg[p++] = TLS_HS_CERTIFICATE;
        cert_msg[p++] = 0; cert_msg[p++] = 0; cert_msg[p++] = 0;
        cert_msg[p++] = (uint8_t)g_cert_request_ctx_len;
        for (uint32_t i = 0; i < g_cert_request_ctx_len; i++) {
            cert_msg[p++] = g_cert_request_ctx[i];
        }
        cert_msg[p++] = 0; cert_msg[p++] = 0; cert_msg[p++] = 0;

        uint32_t body = p - 4;
        cert_msg[1] = (uint8_t)(body >> 16);
        cert_msg[2] = (uint8_t)(body >> 8);
        cert_msg[3] = (uint8_t)body;

        if (!send_record(TLS_CT_HANDSHAKE, cert_msg, p, 1,
                         g_client_hs_key, g_client_hs_iv, &g_client_seq)) {
            tls_set_error("failed to send client Certificate");
            net_tcp_close();
            return TLS_ERR_IO;
        }
        transcript_add(cert_msg, p);
    }

    /* ---- 客户端 Finished ---- */
    {
        uint8_t tr_hash2[32];
        transcript_hash(tr_hash2);

        uint8_t finished_key[32];
        tls13_expand_label(g_client_hs_secret, "finished", 0, 0,
                           finished_key, 32);

        uint8_t verify_data[32];
        hmac_sha256(finished_key, 32, tr_hash2, 32, verify_data);

        uint8_t fin[4 + 32];
        fin[0] = TLS_HS_FINISHED;
        fin[1] = 0; fin[2] = 0; fin[3] = 32;
        tls_memcpy(fin + 4, verify_data, 32);

        if (!send_record(TLS_CT_HANDSHAKE, fin, sizeof(fin), 1,
                         g_client_hs_key, g_client_hs_iv, &g_client_seq)) {
            tls_set_error("failed to send client Finished");
            net_tcp_close();
            return TLS_ERR_IO;
        }
        transcript_add(fin, sizeof(fin));
    }

    /* 握手完成：切换到应用密钥。seq 在新密钥下从 0 开始。 */
    g_server_seq = 0;
    g_client_seq = 0;
    g_in_app_keys = 1;
    g_out_app_keys = 1;
    g_open = 1;

    return TLS_OK;
}

/* ============================================================
 * 应用数据
 * ============================================================ */

int tls_write(const void *data, uint32_t len) {
    if (!g_open) {
        tls_set_error("TLS is not connected");
        return -1;
    }

    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;

    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > 16384u) chunk = 16384u;

        if (!send_record(TLS_CT_APPLICATION, p + off, chunk, 1,
                         g_client_app_key, g_client_app_iv, &g_client_seq)) {
            tls_set_error("failed to send application data");
            return -1;
        }
        off += chunk;
    }

    return (int)len;
}

int tls_read(void *buf, uint32_t len) {
    if (!g_open) {
        tls_set_error("TLS is not connected");
        return -1;
    }
    if (len == 0) return 0;

    uint8_t *out = (uint8_t *)buf;

    if (g_app_pending_off < g_app_pending_len) {
        uint32_t avail = g_app_pending_len - g_app_pending_off;
        uint32_t take = len < avail ? len : avail;
        tls_memcpy(out, g_app_pending + g_app_pending_off, take);
        g_app_pending_off += take;
        return (int)take;
    }

    for (;;) {
        uint8_t type = 0;
        uint32_t rlen = 0;
        int rc = read_record(&type, g_record, &rlen);
        if (rc <= 0) return rc == 0 ? 0 : -1;

        if (type == TLS_CT_APPLICATION) {
            tls_memcpy(out, g_record, rlen);
            if (rlen > len) {
                /* 调用方缓冲不够：多出来的部分留到下次。 */
                g_app_pending_len = rlen;
                tls_memcpy(g_app_pending, g_record, rlen);
                g_app_pending_off = len;
                return (int)len;
            }
            return (int)rlen;
        }

        if (type == TLS_CT_HANDSHAKE) {
            /* NewSessionTicket 等握手后消息：整条丢弃。 */
            continue;
        }

        if (type == TLS_CT_ALERT) {
            tls_set_error("server sent an alert");
            return -1;
        }
    }
}

void tls_close(void) {
    if (g_open) {
        g_open = 0;
        net_tcp_close();
    }
    g_open = 0;
}

int tls_is_open(void) {
    return g_open;
}

const char *tls_error_text(void) {
    return g_error[0] ? g_error : "TLS error";
}

void tls_set_debug(int enabled) {
    g_debug = enabled;
}
