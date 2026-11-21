/* ============================================================
 * x509.c - X.509 解析与证书链验证
 * ------------------------------------------------------------
 * 只支持 RSA 公钥证书链（绝大多数 HTTPS 站点）。EC 证书会返回
 * 明确错误，绝不降级为"跳过验证"。
 * ============================================================ */

#include "x509.h"
#include "crypto.h"
#include "io.h"

#ifdef X509_HOST_TEST
#include <stdio.h>
#define XLOG(...) printf(__VA_ARGS__)
#else
#define XLOG(...) do { } while (0)
#endif

/* SlyzFS 接口（C++ 侧导出 C 符号） */
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

static void x_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- != 0) *d++ = *s++;
}

static void x_memzero(void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- != 0) *d++ = 0;
}

static int x_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static int x_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int x_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int x_strncasecmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        int ca = x_lower((unsigned char)a[i]);
        int cb = x_lower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

/* ============================================================
 * DER 基础
 * ============================================================ */

static int der_next(const uint8_t *p, uint32_t avail, uint32_t *tag,
                    const uint8_t **val, uint32_t *vlen, uint32_t *total) {
    if (avail < 2) return 0;

    uint32_t off = 0;
    uint32_t t = p[off++];
    uint32_t l = p[off++];

    if (l & 0x80u) {
        uint32_t n = l & 0x7Fu;
        if (n == 0 || n > 3 || off + n > avail) return 0;
        l = 0;
        for (uint32_t i = 0; i < n; i++) l = (l << 8) | p[off++];
    }

    if (off + l > avail) return 0;
    if (tag) *tag = t;
    if (val) *val = p + off;
    if (vlen) *vlen = l;
    if (total) *total = off + l;
    return 1;
}

static int der_expect(const uint8_t *p, uint32_t avail, uint32_t want,
                      const uint8_t **val, uint32_t *vlen, uint32_t *total) {
    uint32_t tag = 0;
    if (!der_next(p, avail, &tag, val, vlen, total)) return 0;
    return tag == want;
}

static int oid_eq(const uint8_t *val, uint32_t len, const uint8_t *want,
                  uint32_t want_len) {
    return len == want_len && x_memcmp(val, want, len) == 0;
}

static const uint8_t OID_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01
};
static const uint8_t OID_SHA1_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x05
};
static const uint8_t OID_SHA256_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B
};
static const uint8_t OID_SHA384_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C
};
static const uint8_t OID_SHA512_RSA[] = {
    0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D
};
static const uint8_t OID_EC_PUBLIC[] = {
    0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01
};
static const uint8_t OID_PRIME256V1[] = {
    0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07
};
static const uint8_t OID_SECP384R1[] = {
    0x2B,0x81,0x04,0x00,0x22
};
static const uint8_t OID_ECDSA_SHA256[] = {
    0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02
};
static const uint8_t OID_ECDSA_SHA384[] = {
    0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03
};
static const uint8_t OID_ECDSA_SHA512[] = {
    0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04
};
static const uint8_t OID_BASIC_CONSTRAINTS[] = { 0x55,0x1D,0x13 };
static const uint8_t OID_SAN[] = { 0x55,0x1D,0x11 };
static const uint8_t OID_CN[] = { 0x55,0x04,0x03 };

/* ============================================================
 * 时间
 * ============================================================ */

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd_value(uint8_t v) {
    if (v == 0xFF) return 0;
    return (int)(v & 0x0F) + (int)((v >> 4) * 10);
}

static int days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t x509_now_unix(void) {
#ifdef X509_HOST_TEST
    return 1795000000u;
#else
    while (cmos_read(0x0A) & 0x80u) {}

    uint8_t reg_b = cmos_read(0x0B);
    uint8_t raw_sec = cmos_read(0x00);
    uint8_t raw_min = cmos_read(0x02);
    uint8_t raw_hour = cmos_read(0x04);
    uint8_t raw_day = cmos_read(0x07);
    uint8_t raw_mon = cmos_read(0x08);
    uint8_t raw_year = cmos_read(0x09);

    int sec, min, hour, day, mon, year;

    if (reg_b & 0x04u) {
        sec = raw_sec;
        min = raw_min;
        hour = raw_hour;
        day = raw_day;
        mon = raw_mon;
        year = raw_year;
    } else {
        sec = bcd_value(raw_sec);
        min = bcd_value(raw_min);
        hour = bcd_value(raw_hour & 0x7Fu);
        day = bcd_value(raw_day);
        mon = bcd_value(raw_mon);
        year = bcd_value(raw_year);
    }

    if (!(reg_b & 0x02u)) {
        int pm = (raw_hour & 0x80u) != 0;
        hour = hour % 12;
        if (pm) hour += 12;
    }

    year += 2000;

    if (mon < 1 || mon > 12 || day < 1 || day > 31) {
        mon = 1;
        day = 1;
    }

    int days = days_from_civil(year, mon, day);
    int64_t secs = (int64_t)days * 86400 + hour * 3600 + min * 60 + sec;
    if (secs < 0) return 0;
    return (uint32_t)secs;
#endif
}

static int parse_time_value(uint32_t tag, const uint8_t *v, uint32_t len,
                            uint32_t *out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    for (uint32_t i = 0; i < len; i++) {
        if (v[i] == 'Z' || v[i] == '+' || v[i] == '-') break;
        if (v[i] < '0' || v[i] > '9') return 0;
    }

    if (tag == 0x17) {
        if (len < 12) return 0;
        y = (v[0] - '0') * 10 + (v[1] - '0');
        y += (y < 50) ? 2000 : 1900;
        mo = (v[2] - '0') * 10 + (v[3] - '0');
        d  = (v[4] - '0') * 10 + (v[5] - '0');
        h  = (v[6] - '0') * 10 + (v[7] - '0');
        mi = (v[8] - '0') * 10 + (v[9] - '0');
        s  = (v[10] - '0') * 10 + (v[11] - '0');
    } else if (tag == 0x18) {
        if (len < 14) return 0;
        y = (v[0] - '0') * 1000 + (v[1] - '0') * 100 +
            (v[2] - '0') * 10 + (v[3] - '0');
        mo = (v[4] - '0') * 10 + (v[5] - '0');
        d  = (v[6] - '0') * 10 + (v[7] - '0');
        h  = (v[8] - '0') * 10 + (v[9] - '0');
        mi = (v[10] - '0') * 10 + (v[11] - '0');
        s  = (v[12] - '0') * 10 + (v[13] - '0');
    } else {
        return 0;
    }

    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    int days = days_from_civil(y, mo, d);
    int64_t secs = (int64_t)days * 86400 + h * 3600 + mi * 60 + s;
    if (secs < 0) return 0;
    *out = (uint32_t)secs;
    return 1;
}

/* ============================================================
 * 证书解析
 * ============================================================ */

static void parse_extensions(const uint8_t *val, uint32_t vlen,
                             struct X509Cert *cert) {
    const uint8_t *list = 0;
    uint32_t list_len = 0;
    uint32_t total = 0;

    if (!der_expect(val, vlen, 0x30, &list, &list_len, &total)) return;

    uint32_t off = 0;
    while (off < list_len) {
        const uint8_t *ext = 0;
        uint32_t ext_len = 0;
        uint32_t ext_total = 0;
        if (!der_expect(list + off, list_len - off, 0x30,
                        &ext, &ext_len, &ext_total)) break;

        const uint8_t *oid = 0;
        uint32_t oid_len = 0;
        uint32_t oid_total = 0;
        if (!der_expect(ext, ext_len, 0x06, &oid, &oid_len, &oid_total)) {
            off += ext_total;
            continue;
        }

        uint32_t p = oid_total;
        uint32_t tag = 0;
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;

        if (p < ext_len && der_next(ext + p, ext_len - p, &tag, &v, &vl, &vt)) {
            if (tag == 0x01) p += vt;
        }

        if (p < ext_len && der_expect(ext + p, ext_len - p, 0x04,
                                      &v, &vl, &vt)) {
            if (oid_eq(oid, oid_len, OID_BASIC_CONSTRAINTS,
                       sizeof(OID_BASIC_CONSTRAINTS))) {
                cert->is_ca = 0;
                const uint8_t *bc = 0;
                uint32_t bc_len = 0;
                uint32_t bc_total = 0;
                if (der_expect(v, vl, 0x30, &bc, &bc_len, &bc_total)) {
                    const uint8_t *b = 0;
                    uint32_t b_len = 0;
                    uint32_t b_total = 0;
                    uint32_t bt = 0;
                    if (der_next(bc, bc_len, &bt, &b, &b_len, &b_total) &&
                        bt == 0x01 && b_len == 1 && b[0] != 0) {
                        cert->is_ca = 1;
                    }
                }
            } else if (oid_eq(oid, oid_len, OID_SAN, sizeof(OID_SAN))) {
                cert->san = v;
                cert->san_len = vl;
            }
        }

        off += ext_total;
    }
}

/* 在 Name 里找 CN（没有 SAN 时的回退） */
static void parse_subject_cn(const uint8_t *val, uint32_t vlen,
                             struct X509Cert *cert) {
    uint32_t off = 0;
    while (off < vlen) {
        const uint8_t *set = 0;
        uint32_t set_len = 0;
        uint32_t set_total = 0;
        if (!der_expect(val + off, vlen - off, 0x31,
                        &set, &set_len, &set_total)) break;

        uint32_t inner = 0;
        while (inner < set_len) {
            const uint8_t *seq = 0;
            uint32_t seq_len = 0;
            uint32_t seq_total = 0;
            if (!der_expect(set + inner, set_len - inner, 0x30,
                            &seq, &seq_len, &seq_total)) break;

            const uint8_t *oid = 0;
            uint32_t oid_len = 0;
            uint32_t oid_total = 0;
            if (der_expect(seq, seq_len, 0x06, &oid, &oid_len, &oid_total) &&
                oid_eq(oid, oid_len, OID_CN, sizeof(OID_CN))) {
                uint32_t tag = 0;
                const uint8_t *name = 0;
                uint32_t name_len = 0;
                uint32_t name_total = 0;
                if (der_next(seq + oid_total, seq_len - oid_total,
                             &tag, &name, &name_len, &name_total)) {
                    cert->cn = name;
                    cert->cn_len = name_len;
                    return;
                }
            }

            inner += seq_total;
        }

        off += set_total;
    }
}

int x509_parse(struct X509Cert *cert, const uint8_t *der, uint32_t len) {
    if (!cert || !der) return 0;
    x_memzero(cert, (uint32_t)sizeof(*cert));

    cert->der = der;
    cert->der_len = len;

    const uint8_t *outer = 0;
    uint32_t outer_len = 0;
    uint32_t outer_total = 0;
    if (!der_expect(der, len, 0x30, &outer, &outer_len, &outer_total)) return 0;

    const uint8_t *tbs = 0;
    uint32_t tbs_len = 0;
    uint32_t tbs_total = 0;
    if (!der_expect(outer, outer_len, 0x30, &tbs, &tbs_len, &tbs_total)) return 0;
    cert->tbs = outer;
    cert->tbs_len = tbs_total;

    uint32_t off = tbs_total;

    const uint8_t *alg = 0;
    uint32_t alg_len = 0;
    uint32_t alg_total = 0;
    if (!der_expect(outer + off, outer_len - off, 0x30,
                    &alg, &alg_len, &alg_total)) return 0;
    off += alg_total;

    {
        const uint8_t *oid = 0;
        uint32_t oid_len = 0;
        uint32_t oid_total = 0;
        if (der_expect(alg, alg_len, 0x06, &oid, &oid_len, &oid_total)) {
            if (oid_eq(oid, oid_len, OID_SHA256_RSA, sizeof(OID_SHA256_RSA))) {
                cert->sig_hash = CRYPTO_HASH_SHA256;
            } else if (oid_eq(oid, oid_len, OID_SHA384_RSA,
                              sizeof(OID_SHA384_RSA))) {
                cert->sig_hash = CRYPTO_HASH_SHA384;
            } else if (oid_eq(oid, oid_len, OID_SHA512_RSA,
                              sizeof(OID_SHA512_RSA))) {
                cert->sig_hash = CRYPTO_HASH_SHA512;
            } else if (oid_eq(oid, oid_len, OID_SHA1_RSA,
                              sizeof(OID_SHA1_RSA))) {
                cert->sig_hash = CRYPTO_HASH_SHA1;
            } else if (oid_eq(oid, oid_len, OID_ECDSA_SHA256,
                              sizeof(OID_ECDSA_SHA256))) {
                cert->sig_hash = CRYPTO_HASH_SHA256;
            } else if (oid_eq(oid, oid_len, OID_ECDSA_SHA384,
                              sizeof(OID_ECDSA_SHA384))) {
                cert->sig_hash = CRYPTO_HASH_SHA384;
            } else if (oid_eq(oid, oid_len, OID_ECDSA_SHA512,
                              sizeof(OID_ECDSA_SHA512))) {
                cert->sig_hash = CRYPTO_HASH_SHA512;
            } else {
                cert->sig_hash = 0;
            }
        }
    }

    const uint8_t *sig_bits = 0;
    uint32_t sig_bits_len = 0;
    uint32_t sig_total = 0;
    if (!der_expect(outer + off, outer_len - off, 0x03,
                    &sig_bits, &sig_bits_len, &sig_total)) return 0;
    if (sig_bits_len < 2) return 0;
    cert->sig = sig_bits + 1;   /* 跳过 unused-bits 字节 */
    cert->sig_len = sig_bits_len - 1;

    /* ---- 解析 TBSCertificate 内容 ---- */
    uint32_t p = 0;

    if (p < tbs_len && (tbs[p] & 0xE0u) == 0xA0u) {
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        uint32_t tag = 0;
        if (der_next(tbs + p, tbs_len - p, &tag, &v, &vl, &vt)) p += vt;
    }

    {
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        if (!der_expect(tbs + p, tbs_len - p, 0x02, &v, &vl, &vt)) return 0;
        p += vt;
    }

    {
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        if (!der_expect(tbs + p, tbs_len - p, 0x30, &v, &vl, &vt)) return 0;
        p += vt;
    }

    {
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        if (!der_expect(tbs + p, tbs_len - p, 0x30, &v, &vl, &vt)) return 0;
        cert->issuer = tbs + p;
        cert->issuer_len = vt;
        p += vt;
    }

    {
        const uint8_t *validity = 0;
        uint32_t validity_len = 0;
        uint32_t validity_total = 0;
        if (!der_expect(tbs + p, tbs_len - p, 0x30,
                        &validity, &validity_len, &validity_total)) return 0;

        uint32_t q = 0;
        uint32_t tag = 0;
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;

        if (der_next(validity + q, validity_len - q, &tag, &v, &vl, &vt)) {
            parse_time_value(tag, v, vl, &cert->not_before);
            q += vt;
        }
        if (der_next(validity + q, validity_len - q, &tag, &v, &vl, &vt)) {
            parse_time_value(tag, v, vl, &cert->not_after);
        }

        p += validity_total;
    }

    {
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        if (!der_expect(tbs + p, tbs_len - p, 0x30, &v, &vl, &vt)) return 0;
        cert->subject = tbs + p;
        cert->subject_len = vt;
        parse_subject_cn(v, vl, cert);
        p += vt;
    }

    {
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        if (!der_expect(tbs + p, tbs_len - p, 0x30, &v, &vl, &vt)) return 0;
        cert->spki = tbs + p;
        cert->spki_len = vt;

        /* SPKI 内容：AlgorithmIdentifier + BIT STRING */
        const uint8_t *alg2 = 0;
        uint32_t alg2_len = 0;
        uint32_t alg2_total = 0;
        if (der_expect(v, vl, 0x30, &alg2, &alg2_len, &alg2_total)) {
            const uint8_t *oid = 0;
            uint32_t oid_len = 0;
            uint32_t oid_total = 0;
            if (der_expect(alg2, alg2_len, 0x06, &oid, &oid_len, &oid_total)) {
                if (oid_eq(oid, oid_len, OID_RSA, sizeof(OID_RSA))) {
                    cert->key_type = X509_KEY_RSA;
                } else if (oid_eq(oid, oid_len, OID_EC_PUBLIC,
                                  sizeof(OID_EC_PUBLIC))) {
                    cert->key_type = X509_KEY_EC;
                }
            }
        }

        p += vt;
    }

    while (p < tbs_len) {
        uint32_t tag = 0;
        const uint8_t *v = 0;
        uint32_t vl = 0;
        uint32_t vt = 0;
        if (!der_next(tbs + p, tbs_len - p, &tag, &v, &vl, &vt)) break;
        if (tag == 0xA3) {
            parse_extensions(v, vl, cert);
        }
        p += vt;
    }

    return 1;
}

/* ============================================================
 * 公钥与签名验证
 * ============================================================ */

static int rsa_public_key(const struct X509Cert *cert,
                          const uint8_t **n, uint32_t *n_len,
                          const uint8_t **e, uint32_t *e_len) {
    if (cert->key_type != X509_KEY_RSA || !cert->spki) return 0;

    const uint8_t *spki = 0;
    uint32_t spki_len = 0;
    uint32_t spki_total = 0;
    if (!der_expect(cert->spki, cert->spki_len, 0x30,
                    &spki, &spki_len, &spki_total)) return 0;

    const uint8_t *alg = 0;
    uint32_t alg_len = 0;
    uint32_t alg_total = 0;
    if (!der_expect(spki, spki_len, 0x30, &alg, &alg_len, &alg_total)) return 0;

    const uint8_t *bits = 0;
    uint32_t bits_len = 0;
    uint32_t bits_total = 0;
    if (!der_expect(spki + alg_total, spki_len - alg_total, 0x03,
                    &bits, &bits_len, &bits_total)) return 0;
    if (bits_len < 2) return 0;
    bits++;
    bits_len--;

    const uint8_t *seq = 0;
    uint32_t seq_len = 0;
    uint32_t seq_total = 0;
    if (!der_expect(bits, bits_len, 0x30, &seq, &seq_len, &seq_total)) return 0;

    const uint8_t *nn = 0;
    uint32_t nn_len = 0;
    uint32_t nn_total = 0;
    if (!der_expect(seq, seq_len, 0x02, &nn, &nn_len, &nn_total)) return 0;

    const uint8_t *ee = 0;
    uint32_t ee_len = 0;
    uint32_t ee_total = 0;
    if (!der_expect(seq + nn_total, seq_len - nn_total, 0x02,
                    &ee, &ee_len, &ee_total)) return 0;

    while (nn_len > 0 && nn[0] == 0) { nn++; nn_len--; }
    while (ee_len > 0 && ee[0] == 0) { ee++; ee_len--; }

    *n = nn;
    *n_len = nn_len;
    *e = ee;
    *e_len = ee_len;
    return 1;
}

static int ec_public_key(const struct X509Cert *cert, int *curve,
                         uint8_t qx[48], uint8_t qy[48]) {
    if (cert->key_type != X509_KEY_EC || !cert->spki) return 0;

    const uint8_t *spki = 0;
    uint32_t spki_len = 0;
    uint32_t spki_total = 0;
    if (!der_expect(cert->spki, cert->spki_len, 0x30,
                    &spki, &spki_len, &spki_total)) return 0;

    const uint8_t *alg = 0;
    uint32_t alg_len = 0;
    uint32_t alg_total = 0;
    if (!der_expect(spki, spki_len, 0x30, &alg, &alg_len, &alg_total)) return 0;

    const uint8_t *oid = 0;
    uint32_t oid_len = 0;
    uint32_t oid_total = 0;
    if (!der_expect(alg, alg_len, 0x06, &oid, &oid_len, &oid_total)) return 0;
    if (!oid_eq(oid, oid_len, OID_EC_PUBLIC, sizeof(OID_EC_PUBLIC))) return 0;

    const uint8_t *curve_oid = 0;
    uint32_t curve_len = 0;
    uint32_t curve_total = 0;
    if (!der_expect(alg + oid_total, alg_len - oid_total, 0x06,
                    &curve_oid, &curve_len, &curve_total)) return 0;

    uint32_t coord = 0;
    if (oid_eq(curve_oid, curve_len, OID_PRIME256V1,
               sizeof(OID_PRIME256V1))) {
        coord = 32;
        *curve = 256;
    } else if (oid_eq(curve_oid, curve_len, OID_SECP384R1,
                      sizeof(OID_SECP384R1))) {
        coord = 48;
        *curve = 384;
    } else {
        return 0;
    }

    const uint8_t *bits = 0;
    uint32_t bits_len = 0;
    uint32_t bits_total = 0;
    if (!der_expect(spki + alg_total, spki_len - alg_total, 0x03,
                    &bits, &bits_len, &bits_total)) return 0;

    if (bits_len != 2 + coord * 2 || bits[0] != 0 || bits[1] != 0x04) {
        return 0;
    }
    x_memcpy(qx, bits + 2, coord);
    x_memcpy(qy, bits + 2 + coord, coord);
    return 1;
}

static int parse_ecdsa_signature(const uint8_t *sig, uint32_t len,
                                 uint8_t *r, uint8_t *s, uint32_t coord) {
    const uint8_t *seq = 0;
    uint32_t seq_len = 0;
    uint32_t seq_total = 0;
    if (!der_expect(sig, len, 0x30, &seq, &seq_len, &seq_total)) return 0;

    const uint8_t *rv = 0;
    uint32_t rlen = 0;
    uint32_t rtotal = 0;
    if (!der_expect(seq, seq_len, 0x02, &rv, &rlen, &rtotal)) return 0;

    const uint8_t *sv = 0;
    uint32_t slen = 0;
    uint32_t stotal = 0;
    if (!der_expect(seq + rtotal, seq_len - rtotal, 0x02,
                    &sv, &slen, &stotal)) return 0;

    while (rlen > 0 && rv[0] == 0) { rv++; rlen--; }
    while (slen > 0 && sv[0] == 0) { sv++; slen--; }
    if (rlen > coord || slen > coord) return 0;

    x_memzero(r, coord);
    x_memzero(s, coord);
    x_memcpy(r + coord - rlen, rv, rlen);
    x_memcpy(s + coord - slen, sv, slen);
    return 1;
}

static void hash_by_id(int id, const uint8_t *data, uint32_t len,
                       uint8_t *out, uint32_t *out_len) {
    switch (id) {
    case CRYPTO_HASH_SHA1:
        sha1_buf(data, len, out);
        *out_len = 20;
        break;
    case CRYPTO_HASH_SHA384:
        sha384_buf(data, len, out);
        *out_len = 48;
        break;
    case CRYPTO_HASH_SHA512:
        sha512_buf(data, len, out);
        *out_len = 64;
        break;
    case CRYPTO_HASH_SHA256:
    default:
        sha256_buf(data, len, out);
        *out_len = 32;
        break;
    }
}

static int cert_signed_by(const struct X509Cert *child,
                          const struct X509Cert *issuer) {
    if (!child || !issuer || child->sig_hash == 0) {
        XLOG("  [x509] child/issuer/sig_hash invalid (%d)\n", child->sig_hash);
        return 0;
    }

    uint8_t digest[64];
    uint32_t digest_len = 0;
    hash_by_id(child->sig_hash, child->tbs, child->tbs_len, digest, &digest_len);

    if (issuer->key_type == X509_KEY_RSA) {
        const uint8_t *n = 0;
        const uint8_t *e = 0;
        uint32_t n_len = 0;
        uint32_t e_len = 0;
        if (!rsa_public_key(issuer, &n, &n_len, &e, &e_len)) {
            XLOG("  [x509] rsa_public_key failed\n");
            return 0;
        }

        int ok = rsa_verify_pkcs1(n, n_len, e, e_len, child->sig, child->sig_len,
                                  digest, digest_len, child->sig_hash);
        XLOG("  [x509] RSA verify hash=%d n=%u sig=%u => %d\n",
             child->sig_hash, n_len, child->sig_len, ok);
        return ok;
    }

    if (issuer->key_type == X509_KEY_EC) {
        int curve = 0;
        uint8_t qx[48];
        uint8_t qy[48];
        uint8_t r[48];
        uint8_t s[48];
        if (!ec_public_key(issuer, &curve, qx, qy)) {
            XLOG("  [x509] ec_public_key failed\n");
            return 0;
        }
        uint32_t coord = (curve == 256) ? 32u : 48u;
        if (!parse_ecdsa_signature(child->sig, child->sig_len, r, s, coord)) {
            XLOG("  [x509] ecdsa signature DER parse failed\n");
            return 0;
        }

        int ok;
        if (curve == 256) {
            ok = ecdsa_p256_verify_digest(r, s, digest, qx, qy);
        } else {
            ok = ecdsa_p384_verify_digest(r, s, digest, qx, qy);
        }
        XLOG("  [x509] ECDSA-P%d verify hash=%d => %d\n", curve,
             child->sig_hash, ok);
        return ok;
    }

    XLOG("  [x509] unsupported issuer key type %d\n", issuer->key_type);
    return 0;
}

int x509_verify_message(const struct X509Cert *cert,
                        const uint8_t *sig, uint32_t sig_len,
                        const uint8_t *message, uint32_t message_len,
                        int sig_alg) {
    if (!cert || !sig || !message) return 0;

    if (sig_alg == X509_SIG_RSA_PSS_RSAE_SHA256 &&
        cert->key_type == X509_KEY_RSA) {
        const uint8_t *n = 0;
        const uint8_t *e = 0;
        uint32_t n_len = 0;
        uint32_t e_len = 0;
        if (!rsa_public_key(cert, &n, &n_len, &e, &e_len)) return 0;
        uint8_t digest[32];
        sha256_buf(message, message_len, digest);
        return rsa_verify_pss_sha256(n, n_len, e, e_len, sig, sig_len, digest);
    }

    if (sig_alg == X509_SIG_RSA_PKCS1_SHA256 &&
        cert->key_type == X509_KEY_RSA) {
        const uint8_t *n = 0;
        const uint8_t *e = 0;
        uint32_t n_len = 0;
        uint32_t e_len = 0;
        if (!rsa_public_key(cert, &n, &n_len, &e, &e_len)) return 0;
        uint8_t digest[32];
        sha256_buf(message, message_len, digest);
        return rsa_verify_pkcs1(n, n_len, e, e_len, sig, sig_len,
                                digest, 32, CRYPTO_HASH_SHA256);
    }

    if (sig_alg == X509_SIG_ECDSA_P256_SHA256 &&
        cert->key_type == X509_KEY_EC) {
        int curve = 0;
        uint8_t qx[48];
        uint8_t qy[48];
        uint8_t r[48];
        uint8_t s[48];
        if (!ec_public_key(cert, &curve, qx, qy) || curve != 256) return 0;
        if (!parse_ecdsa_signature(sig, sig_len, r, s, 32)) return 0;
        uint8_t digest[32];
        sha256_buf(message, message_len, digest);
        return ecdsa_p256_verify_digest(r, s, digest, qx, qy);
    }

    if (sig_alg == X509_SIG_ECDSA_P384_SHA384 &&
        cert->key_type == X509_KEY_EC) {
        int curve = 0;
        uint8_t qx[48];
        uint8_t qy[48];
        uint8_t r[48];
        uint8_t s[48];
        if (!ec_public_key(cert, &curve, qx, qy) || curve != 384) return 0;
        if (!parse_ecdsa_signature(sig, sig_len, r, s, 48)) return 0;
        uint8_t digest[48];
        sha384_buf(message, message_len, digest);
        return ecdsa_p384_verify_digest(r, s, digest, qx, qy);
    }

    return 0;
}

/* ============================================================
 * 主机名
 * ============================================================ */

static int dns_name_match(const char *pattern, uint32_t plen, const char *host) {
    int hlen = x_strlen(host);
    if (hlen != (int)plen) {
        if (plen < 3 || pattern[0] != '*' || pattern[1] != '.') return 0;

        uint32_t suffix_len = plen - 1;   /* ".example.com" */
        if ((uint32_t)hlen <= suffix_len) return 0;
        if (!x_strncasecmp(host + hlen - (int)suffix_len,
                           pattern + 1, (int)suffix_len)) {
            return 0;
        }

        int label = hlen - (int)suffix_len;
        for (int i = 0; i < label; i++) {
            if (host[i] == '.') return 0;
        }
        return 1;
    }

    return x_strncasecmp(pattern, host, (int)plen) == 0;
}

int x509_hostname_match(const struct X509Cert *cert, const char *host) {
    if (!cert || !host) return 0;

    if (cert->san && cert->san_len > 0) {
        const uint8_t *seq = 0;
        uint32_t seq_len = 0;
        uint32_t seq_total = 0;
        if (der_expect(cert->san, cert->san_len, 0x30,
                       &seq, &seq_len, &seq_total)) {
            uint32_t off = 0;
            while (off < seq_len) {
                uint32_t tag = 0;
                const uint8_t *v = 0;
                uint32_t vl = 0;
                uint32_t vt = 0;
                if (!der_next(seq + off, seq_len - off, &tag, &v, &vl, &vt)) break;
                if (tag == 0x82) {
                    if (dns_name_match((const char *)v, vl, host)) return 1;
                }
                off += vt;
            }
        }
    }

    if (cert->cn && cert->cn_len > 0) {
        return dns_name_match((const char *)cert->cn, cert->cn_len, host);
    }

    return 0;
}

/* ============================================================
 * 信任库
 * ============================================================ */

static uint8_t g_ca_pem[X509_CA_PEM_MAX];
static uint32_t g_ca_pem_len = 0;
static int g_ca_loaded = 0;
static int g_ca_count = 0;

static int read_whole_file(const char *path, uint8_t *buf, uint32_t cap) {
    void *fh = slyzfs_open(path, "r");
    if (!fh) return -1;

    uint32_t total = 0;
    while (total < cap) {
        int got = slyzfs_read(fh, buf + total, (int)(cap - total));
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    slyzfs_close(fh);
    return (int)total;
}

static const char *find_text(const char *hay, uint32_t hay_len,
                             const char *needle) {
    int nl = x_strlen(needle);
    if (nl == 0) return hay;
    for (uint32_t i = 0; i + (uint32_t)nl <= hay_len; i++) {
        int ok = 1;
        for (int k = 0; k < nl; k++) {
            if (hay[i + (uint32_t)k] != needle[k]) { ok = 0; break; }
        }
        if (ok) return hay + i;
    }
    return 0;
}

int x509_trust_load(void) {
    if (g_ca_loaded) return g_ca_count;
    g_ca_loaded = 1;
    g_ca_count = 0;

    int n = read_whole_file("system\\certs\\ca.pem", g_ca_pem,
                            (uint32_t)sizeof(g_ca_pem));
    if (n <= 0) return 0;
    g_ca_pem_len = (uint32_t)n;

    const char *begin_marker = "-----BEGIN CERTIFICATE-----";
    const char *end_marker = "-----END CERTIFICATE-----";

    const char *p = (const char *)g_ca_pem;
    uint32_t remaining = g_ca_pem_len;

    while (remaining > 0) {
        const char *b = find_text(p, remaining, begin_marker);
        if (!b) break;
        uint32_t used = (uint32_t)(b - p) + (uint32_t)x_strlen(begin_marker);
        if (used >= remaining) break;
        p = b + x_strlen(begin_marker);
        remaining -= used;

        const char *e = find_text(p, remaining, end_marker);
        if (!e) break;

        uint8_t der[X509_DER_MAX];
        int der_len = crypto_base64_decode(p, (uint32_t)(e - p),
                                           der, (uint32_t)sizeof(der));
        if (der_len > 0) {
            struct X509Cert cert;
            if (x509_parse(&cert, der, (uint32_t)der_len)) {
                g_ca_count++;
            }
        }

        uint32_t consumed = (uint32_t)(e - p) + (uint32_t)x_strlen(end_marker);
        p = e + x_strlen(end_marker);
        remaining -= consumed;
    }

    return g_ca_count;
}

int x509_trust_count(void) {
    return g_ca_loaded ? g_ca_count : x509_trust_load();
}

/* 在信任库中查找 subject 与 issuer 匹配的根证书。
 * 命中时把根证书解析到 out（DER 缓冲使用内部静态区）。 */
static int trust_find(const uint8_t *issuer, uint32_t issuer_len,
                      struct X509Cert *out) {
    if (x509_trust_load() <= 0) return 0;

    static uint8_t der[X509_DER_MAX];

    const char *begin_marker = "-----BEGIN CERTIFICATE-----";
    const char *end_marker = "-----END CERTIFICATE-----";

    const char *p = (const char *)g_ca_pem;
    uint32_t remaining = g_ca_pem_len;

    while (remaining > 0) {
        const char *b = find_text(p, remaining, begin_marker);
        if (!b) break;
        uint32_t used = (uint32_t)(b - p) + (uint32_t)x_strlen(begin_marker);
        if (used >= remaining) break;
        p = b + x_strlen(begin_marker);
        remaining -= used;

        const char *e = find_text(p, remaining, end_marker);
        if (!e) break;

        int der_len = crypto_base64_decode(p, (uint32_t)(e - p),
                                           der, (uint32_t)sizeof(der));
        if (der_len > 0) {
            struct X509Cert cert;
            if (x509_parse(&cert, der, (uint32_t)der_len) &&
                cert.subject_len == issuer_len &&
                x_memcmp(cert.subject, issuer, issuer_len) == 0) {
                if (out) *out = cert;
                return 1;
            }
        }

        uint32_t consumed = (uint32_t)(e - p) + (uint32_t)x_strlen(end_marker);
        p = e + x_strlen(end_marker);
        remaining -= consumed;
    }

    return 0;
}

/* ============================================================
 * 证书链验证
 * ============================================================ */

int x509_verify_chain(struct X509Cert *chain, int count, const char *host,
                      const char **reason) {
    if (reason) *reason = "";

    if (!chain || count <= 0 || count > X509_MAX_CHAIN) {
        if (reason) *reason = "empty certificate chain";
        return 0;
    }

    uint32_t now = x509_now_unix();
    const uint32_t slack = 86400u;

    for (int i = 0; i < count; i++) {
        if (chain[i].not_before != 0 && now + slack < chain[i].not_before) {
            if (reason) *reason = "certificate not yet valid";
            return 0;
        }
        if (chain[i].not_after != 0 && now > chain[i].not_after + slack) {
            if (reason) *reason = "certificate expired";
            return 0;
        }
        if (chain[i].key_type == X509_KEY_UNKNOWN) {
            if (reason) *reason = "unsupported certificate key";
            return 0;
        }
    }

    for (int i = 0; i + 1 < count; i++) {
        if (chain[i].issuer_len != chain[i + 1].subject_len ||
            x_memcmp(chain[i].issuer, chain[i + 1].subject,
                     chain[i].issuer_len) != 0) {
            if (reason) *reason = "certificate issuer mismatch";
            return 0;
        }
        if (!chain[i + 1].is_ca) {
            if (reason) *reason = "issuer is not a CA certificate";
            return 0;
        }
        if (!cert_signed_by(&chain[i], &chain[i + 1])) {
            if (reason) *reason = "certificate signature invalid";
            return 0;
        }
    }

    {
        const struct X509Cert *last = &chain[count - 1];
        struct X509Cert root;

        if (trust_find(last->issuer, last->issuer_len, &root)) {
            if (!cert_signed_by(last, &root)) {
                if (reason) *reason = "root signature invalid";
                return 0;
            }
        } else {
            /* 交叉签名的根证书：服务器下发的最后一张证书可能由另一个
             * 老根签名（例如 SSL.com ECC Root 由已下架的 AAA 根交叉
             * 签名）。只要它的 subject 与 SPKI 与信任库里的自签名根
             * 完全一致，它就是同一个信任锚，直接接受。 */
            struct X509Cert anchor;
            if (!trust_find(last->subject, last->subject_len, &anchor)) {
                if (reason) *reason = "untrusted certificate chain";
                return 0;
            }
            if (anchor.spki_len != last->spki_len ||
                x_memcmp(anchor.spki, last->spki, last->spki_len) != 0) {
                if (reason) *reason = "untrusted certificate chain";
                return 0;
            }
        }
    }

    if (host && host[0]) {
        if (!x509_hostname_match(&chain[0], host)) {
            if (reason) *reason = "certificate hostname mismatch";
            return 0;
        }
    }

    return 1;
}
