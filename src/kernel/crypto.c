/* ============================================================
 * crypto.c - 密码学原语实现
 * ------------------------------------------------------------
 * 约束：
 *   - 不依赖 libc / libgcc，不使用 SSE 与 x87；
 *   - 只做 TLS 1.3 客户端需要的方向（加密用 AES-GCM，
 *     签名只做验证，密钥交换用 X25519）；
 *   - 所有临时缓冲要么是静态区，要么在栈上控制得足够小。
 * ============================================================ */

#include "crypto.h"

/* ============================================================
 * 内存工具
 * ============================================================ */

static void c_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- != 0) *d++ = *s++;
}

static void c_memset(void *dst, int value, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- != 0) *d++ = (uint8_t)value;
}

static void c_memzero(void *dst, uint32_t n) {
    c_memset(dst, 0, n);
}

static int c_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static uint32_t rd32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint64_t rd64_be(const uint8_t *p) {
    return ((uint64_t)rd32_be(p) << 32) | (uint64_t)rd32_be(p + 4);
}

static uint64_t rd64_le(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static void wr64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static void wr64_be(uint8_t *p, uint64_t v) {
    wr32_be(p, (uint32_t)(v >> 32));
    wr32_be(p + 4, (uint32_t)v);
}

static uint32_t rotr32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

static uint64_t rotr64(uint64_t v, int n) {
    return (v >> n) | (v << (64 - n));
}

/* ============================================================
 * SHA-256
 * ============================================================ */

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_block(struct Sha256Ctx *ctx, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = rd32_be(p + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(struct Sha256Ctx *ctx) {
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->bits = 0;
    ctx->fill = 0;
}

void sha256_update(struct Sha256Ctx *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8u;

    while (len > 0) {
        uint32_t room = 64u - ctx->fill;
        uint32_t take = len < room ? len : room;
        c_memcpy(ctx->buf + ctx->fill, p, take);
        ctx->fill += take;
        p += take;
        len -= take;
        if (ctx->fill == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->fill = 0;
        }
    }
}

void sha256_final(struct Sha256Ctx *ctx, uint8_t out[32]) {
    uint64_t bits = ctx->bits;

    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while (ctx->fill != 56) sha256_update(ctx, &zero, 1);

    uint8_t lenbuf[8];
    wr64_be(lenbuf, bits);
    sha256_update(ctx, lenbuf, 8);

    for (int i = 0; i < 8; i++) wr32_be(out + i * 4, ctx->h[i]);
}

void sha256_buf(const void *data, uint32_t len, uint8_t out[32]) {
    struct Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ============================================================
 * SHA-1（旧证书链仍在使用）
 * ============================================================ */

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rd32_be(p + i * 4);
    for (int i = 16; i < 80; i++) {
        w[i] = ((w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]) << 1) |
               ((w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]) >> 31);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_buf(const void *data, uint32_t len, uint8_t out[20]) {
    uint32_t h[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    };

    const uint8_t *p = (const uint8_t *)data;
    uint32_t full = len / 64u;
    for (uint32_t i = 0; i < full; i++) sha1_block(h, p + i * 64u);

    uint8_t tail[128];
    uint32_t rem = len - full * 64u;
    c_memzero(tail, sizeof(tail));
    c_memcpy(tail, p + full * 64u, rem);
    tail[rem] = 0x80;

    uint32_t tail_len = (rem + 1u + 8u <= 64u) ? 64u : 128u;
    uint64_t bits = (uint64_t)len * 8u;
    wr64_be(tail + tail_len - 8, bits);

    sha1_block(h, tail);
    if (tail_len == 128u) sha1_block(h, tail + 64);

    for (int i = 0; i < 5; i++) wr32_be(out + i * 4, h[i]);
}

/* ============================================================
 * SHA-512 / SHA-384
 * ============================================================ */

static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

struct Sha512Ctx {
    uint64_t h[8];
    uint64_t bits;
    uint8_t  buf[128];
    uint32_t fill;
};

static void sha512_block(struct Sha512Ctx *ctx, const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rd64_be(p + i * 8);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^
                      (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^
                      (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint64_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + SHA512_K[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha512_init(struct Sha512Ctx *ctx, int is384) {
    static const uint64_t iv512[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    static const uint64_t iv384[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
        0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
        0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
    };

    for (int i = 0; i < 8; i++) ctx->h[i] = is384 ? iv384[i] : iv512[i];
    ctx->bits = 0;
    ctx->fill = 0;
}

static void sha512_update(struct Sha512Ctx *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8u;

    while (len > 0) {
        uint32_t room = 128u - ctx->fill;
        uint32_t take = len < room ? len : room;
        c_memcpy(ctx->buf + ctx->fill, p, take);
        ctx->fill += take;
        p += take;
        len -= take;
        if (ctx->fill == 128) {
            sha512_block(ctx, ctx->buf);
            ctx->fill = 0;
        }
    }
}

static void sha512_final(struct Sha512Ctx *ctx, uint8_t *out, int out_len) {
    uint64_t bits = ctx->bits;

    uint8_t pad = 0x80;
    sha512_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while (ctx->fill != 112) sha512_update(ctx, &zero, 1);

    uint8_t lenbuf[16];
    wr64_be(lenbuf, 0);
    wr64_be(lenbuf + 8, bits);
    sha512_update(ctx, lenbuf, 16);

    for (int i = 0; i < out_len / 8; i++) wr64_be(out + i * 8, ctx->h[i]);
}

void sha512_buf(const void *data, uint32_t len, uint8_t out[64]) {
    struct Sha512Ctx ctx;
    sha512_init(&ctx, 0);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out, 64);
}

void sha384_buf(const void *data, uint32_t len, uint8_t out[48]) {
    struct Sha512Ctx ctx;
    sha512_init(&ctx, 1);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out, 48);
}

/* ============================================================
 * HMAC / HKDF
 * ============================================================ */

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                 const uint8_t *data, uint32_t data_len,
                 uint8_t out[32]) {
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    c_memset(k, 0, sizeof(k));

    if (key_len > 64) {
        sha256_buf(key, key_len, k);
    } else {
        c_memcpy(k, key, key_len);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    struct Sha256Ctx ctx;
    uint8_t inner[32];

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);

    c_memzero(k, sizeof(k));
    c_memzero(ipad, sizeof(ipad));
    c_memzero(opad, sizeof(opad));
}

void hkdf_extract(const uint8_t *salt, uint32_t salt_len,
                  const uint8_t *ikm, uint32_t ikm_len,
                  uint8_t prk[32]) {
    uint8_t zero[32];
    if (!salt || salt_len == 0) {
        c_memzero(zero, sizeof(zero));
        salt = zero;
        salt_len = 32;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void tls13_extract(const uint8_t *salt, uint32_t salt_len,
                   const uint8_t *ikm, uint32_t ikm_len,
                   uint8_t out[32]) {
    hkdf_extract(salt, salt_len, ikm, ikm_len, out);
}

int hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t info_len,
                uint8_t *out, uint32_t out_len) {
    uint8_t t[32];
    uint32_t t_len = 0;
    uint32_t done = 0;
    uint8_t counter = 1;
    uint8_t data[32 + 384 + 1];

    if (info_len > 384u) return 0;

    while (done < out_len) {
        uint32_t n = 0;

        if (t_len != 0) {
            c_memcpy(data + n, t, t_len);
            n += t_len;
        }
        if (info && info_len) {
            c_memcpy(data + n, info, info_len);
            n += info_len;
        }
        data[n++] = counter;

        hmac_sha256(prk, 32, data, n, t);
        t_len = 32;

        uint32_t take = out_len - done;
        if (take > 32) take = 32;
        c_memcpy(out + done, t, take);
        done += take;
        counter++;
        if (counter == 0 && done < out_len) return 0;
    }

    c_memzero(t, sizeof(t));
    c_memzero(data, sizeof(data));
    return 1;
}

int tls13_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, uint32_t context_len,
                       uint8_t *out, uint32_t out_len) {
    uint8_t info[128];
    uint32_t n = 0;

    uint32_t label_len = 0;
    while (label[label_len]) label_len++;

    info[n++] = (uint8_t)(out_len >> 8);
    info[n++] = (uint8_t)out_len;
    info[n++] = (uint8_t)(6u + label_len);

    const char *prefix = "tls13 ";
    for (int i = 0; i < 6; i++) info[n++] = (uint8_t)prefix[i];
    for (uint32_t i = 0; i < label_len; i++) info[n++] = (uint8_t)label[i];

    info[n++] = (uint8_t)context_len;
    for (uint32_t i = 0; i < context_len; i++) info[n++] = context[i];

    return hkdf_expand(secret, info, n, out, out_len);
}

/* ============================================================
 * 随机数与工具
 * ============================================================ */

static uint64_t g_rng_state = 0;

static uint64_t crypto_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void crypto_random(uint8_t *out, uint32_t len) {
    if (g_rng_state == 0) {
        g_rng_state = crypto_rdtsc() ^ 0x9E3779B97F4A7C15ULL;
    }

    for (uint32_t i = 0; i < len; i++) {
        g_rng_state ^= crypto_rdtsc();
        g_rng_state *= 6364136223846793005ULL;
        g_rng_state ^= g_rng_state >> 33;
        out[i] = (uint8_t)(g_rng_state >> 24);
    }
}

int crypto_equal(const uint8_t *a, const uint8_t *b, uint32_t len) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int crypto_base64_decode(const char *in, uint32_t in_len,
                         uint8_t *out, uint32_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    uint32_t written = 0;

    for (uint32_t i = 0; i < in_len; i++) {
        int c = (unsigned char)in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (c == '=') break;
            continue;
        }
        int v = b64_value(c);
        if (v < 0) return -1;

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= cap) return -1;
            out[written++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }

    return (int)written;
}

/* ============================================================
 * AES-128
 * ============================================================ */

static const uint8_t AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t AES_RCON[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static uint8_t aes_xtime(uint8_t v) {
    return (uint8_t)((v << 1) ^ ((v & 0x80u) ? 0x1bu : 0x00u));
}

static uint8_t aes_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1u) r ^= a;
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

void aes128_init(struct Aes128 *ctx, const uint8_t key[16]) {
    c_memcpy(ctx->round_key, key, 16);

    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        c_memcpy(t, ctx->round_key + (i - 1) * 4, 4);

        if ((i & 3) == 0) {
            uint8_t tmp = t[0];
            t[0] = AES_SBOX[t[1]];
            t[1] = AES_SBOX[t[2]];
            t[2] = AES_SBOX[t[3]];
            t[3] = AES_SBOX[tmp];
            t[0] ^= AES_RCON[i / 4];
        }

        ctx->round_key[i * 4 + 0] =
            (uint8_t)(ctx->round_key[(i - 4) * 4 + 0] ^ t[0]);
        ctx->round_key[i * 4 + 1] =
            (uint8_t)(ctx->round_key[(i - 4) * 4 + 1] ^ t[1]);
        ctx->round_key[i * 4 + 2] =
            (uint8_t)(ctx->round_key[(i - 4) * 4 + 2] ^ t[2]);
        ctx->round_key[i * 4 + 3] =
            (uint8_t)(ctx->round_key[(i - 4) * 4 + 3] ^ t[3]);
    }
}

void aes128_encrypt_block(const struct Aes128 *ctx,
                          const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    c_memcpy(s, in, 16);

    for (int i = 0; i < 16; i++) s[i] ^= ctx->round_key[i];

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];

        /* ShiftRows：状态按列存放（s[row + 4*col]）。
         * 第 0 行不动，第 1/2/3 行分别左移 1/2/3 个字节。 */
        uint8_t row0 = s[0], row1_0 = s[1], row2_0 = s[2], row3_0 = s[3];
        uint8_t r1b = s[5], r2b = s[6], r3b = s[7];
        uint8_t r1c = s[9], r2c = s[10], r3c = s[11];
        uint8_t r1d = s[13], r2d = s[14], r3d = s[15];

        s[0] = row0;

        s[1] = r1b; s[5] = r1c; s[9] = r1d; s[13] = row1_0;

        s[2] = r2c; s[6] = r2d; s[10] = row2_0; s[14] = r2b;

        s[3] = r3d; s[7] = row3_0; s[11] = r3b; s[15] = r3c;

        if (round != 10) {
            for (int col = 0; col < 4; col++) {
                uint8_t *p = s + col * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(aes_mul(a0, 2) ^ aes_mul(a1, 3) ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ aes_mul(a1, 2) ^ aes_mul(a2, 3) ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ aes_mul(a2, 2) ^ aes_mul(a3, 3));
                p[3] = (uint8_t)(aes_mul(a0, 3) ^ a1 ^ a2 ^ aes_mul(a3, 2));
            }
        }

        for (int i = 0; i < 16; i++) s[i] ^= ctx->round_key[round * 16 + i];
    }

    c_memcpy(out, s, 16);
}

/* ============================================================
 * GCM
 * ============================================================ */

static void gcm_gf_mul(uint8_t x[16], const uint8_t y[16]) {
    uint64_t vh = rd64_be(y);
    uint64_t vl = rd64_be(y + 8);
    uint64_t zh = 0;
    uint64_t zl = 0;

    for (int i = 0; i < 128; i++) {
        uint64_t bit = (uint64_t)((x[i >> 3] >> (7 - (i & 7))) & 1u);
        uint64_t mask = (uint64_t)0 - bit;
        zh ^= vh & mask;
        zl ^= vl & mask;

        uint64_t lsb = vl & 1u;
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ (0xE100000000000000ULL & ((uint64_t)0 - lsb));
    }

    wr64_be(x, zh);
    wr64_be(x + 8, zl);
}

struct GcmState {
    uint8_t h[16];
    uint8_t y[16];
};

static void gcm_ghash_init(struct GcmState *st, const uint8_t h[16]) {
    c_memzero(st->y, 16);
    c_memcpy(st->h, h, 16);
}

static void gcm_ghash_block(struct GcmState *st, const uint8_t block[16]) {
    uint8_t x[16];
    for (int i = 0; i < 16; i++) x[i] = (uint8_t)(st->y[i] ^ block[i]);
    gcm_gf_mul(x, st->h);
    c_memcpy(st->y, x, 16);
}

static void gcm_ghash_update(struct GcmState *st, const uint8_t *data,
                             uint32_t len) {
    while (len >= 16) {
        gcm_ghash_block(st, data);
        data += 16;
        len -= 16;
    }
    if (len != 0) {
        uint8_t block[16];
        c_memzero(block, 16);
        c_memcpy(block, data, len);
        gcm_ghash_block(st, block);
    }
}

static void gcm_ctr32_inc(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i] != 0) break;
    }
}

int aes128_gcm_encrypt(const uint8_t key[16], const uint8_t *iv, uint32_t iv_len,
                       const uint8_t *aad, uint32_t aad_len,
                       const uint8_t *pt, uint32_t pt_len,
                       uint8_t *ct, uint8_t tag[16]) {
    if (!key || !iv || iv_len != 12 || (pt_len != 0 && (!pt || !ct))) return 0;

    struct Aes128 aes;
    aes128_init(&aes, key);

    uint8_t zero[16];
    c_memzero(zero, 16);
    uint8_t h[16];
    aes128_encrypt_block(&aes, zero, h);

    uint8_t j0[16];
    c_memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    uint8_t ctr[16];
    c_memcpy(ctr, j0, 16);
    gcm_ctr32_inc(ctr);

    for (uint32_t off = 0; off < pt_len; off += 16) {
        uint8_t stream[16];
        aes128_encrypt_block(&aes, ctr, stream);
        uint32_t n = pt_len - off;
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) ct[off + i] = (uint8_t)(pt[off + i] ^ stream[i]);
        gcm_ctr32_inc(ctr);
    }

    struct GcmState st;
    gcm_ghash_init(&st, h);
    if (aad_len != 0) gcm_ghash_update(&st, aad, aad_len);
    if (pt_len != 0) gcm_ghash_update(&st, ct, pt_len);

    uint8_t lens[16];
    wr64_be(lens, (uint64_t)aad_len * 8u);
    wr64_be(lens + 8, (uint64_t)pt_len * 8u);
    gcm_ghash_block(&st, lens);

    uint8_t ek[16];
    aes128_encrypt_block(&aes, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(st.y[i] ^ ek[i]);

    return 1;
}

int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t *iv, uint32_t iv_len,
                       const uint8_t *aad, uint32_t aad_len,
                       const uint8_t *ct, uint32_t ct_len,
                       const uint8_t tag[16], uint8_t *pt) {
    if (!key || !iv || iv_len != 12 || (ct_len != 0 && !ct)) return 0;

    struct Aes128 aes;
    aes128_init(&aes, key);

    uint8_t zero[16];
    c_memzero(zero, 16);
    uint8_t h[16];
    aes128_encrypt_block(&aes, zero, h);

    uint8_t j0[16];
    c_memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    struct GcmState st;
    gcm_ghash_init(&st, h);
    if (aad_len != 0) gcm_ghash_update(&st, aad, aad_len);
    if (ct_len != 0) gcm_ghash_update(&st, ct, ct_len);

    uint8_t lens[16];
    wr64_be(lens, (uint64_t)aad_len * 8u);
    wr64_be(lens + 8, (uint64_t)ct_len * 8u);
    gcm_ghash_block(&st, lens);

    uint8_t ek[16];
    aes128_encrypt_block(&aes, j0, ek);

    uint8_t calc[16];
    for (int i = 0; i < 16; i++) calc[i] = (uint8_t)(st.y[i] ^ ek[i]);
    if (!crypto_equal(calc, tag, 16)) return 0;

    uint8_t ctr[16];
    c_memcpy(ctr, j0, 16);
    gcm_ctr32_inc(ctr);

    for (uint32_t off = 0; off < ct_len; off += 16) {
        uint8_t stream[16];
        aes128_encrypt_block(&aes, ctr, stream);
        uint32_t n = ct_len - off;
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) pt[off + i] = (uint8_t)(ct[off + i] ^ stream[i]);
        gcm_ctr32_inc(ctr);
    }

    return 1;
}

/* ============================================================
 * X25519
 * ============================================================ */

#define FE_MASK51 0x7FFFFFFFFFFFFULL

struct Fe {
    uint64_t v[5];
};

static void fe_copy(struct Fe *h, const struct Fe *f) {
    for (int i = 0; i < 5; i++) h->v[i] = f->v[i];
}

static void fe_zero(struct Fe *h) {
    for (int i = 0; i < 5; i++) h->v[i] = 0;
}

static void fe_one(struct Fe *h) {
    fe_zero(h);
    h->v[0] = 1;
}

static void fe_add(struct Fe *h, const struct Fe *f, const struct Fe *g) {
    for (int i = 0; i < 5; i++) h->v[i] = f->v[i] + g->v[i];
}

static void fe_sub(struct Fe *h, const struct Fe *f, const struct Fe *g) {
    static const uint64_t two_p[5] = {
        0xFFFFFFFFFFFDAULL, 0xFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFEULL,
        0xFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFEULL
    };
    for (int i = 0; i < 5; i++) h->v[i] = f->v[i] + two_p[i] - g->v[i];
}

static void fe_carry(struct Fe *h) {
    for (int i = 0; i < 4; i++) {
        uint64_t c = h->v[i] >> 51;
        h->v[i] &= FE_MASK51;
        h->v[i + 1] += c;
    }
    uint64_t c = h->v[4] >> 51;
    h->v[4] &= FE_MASK51;
    h->v[0] += 19u * c;
    c = h->v[0] >> 51;
    h->v[0] &= FE_MASK51;
    h->v[1] += c;
}

static void fe_mul(struct Fe *h, const struct Fe *f, const struct Fe *g) {
    uint64_t f0 = f->v[0], f1 = f->v[1], f2 = f->v[2], f3 = f->v[3], f4 = f->v[4];
    uint64_t g0 = g->v[0], g1 = g->v[1], g2 = g->v[2], g3 = g->v[3], g4 = g->v[4];

    unsigned __int128 t0 = (unsigned __int128)f0 * g0 +
        (unsigned __int128)19u * ((unsigned __int128)f1 * g4 +
                                  (unsigned __int128)f2 * g3 +
                                  (unsigned __int128)f3 * g2 +
                                  (unsigned __int128)f4 * g1);
    unsigned __int128 t1 = (unsigned __int128)f0 * g1 +
        (unsigned __int128)f1 * g0 +
        (unsigned __int128)19u * ((unsigned __int128)f2 * g4 +
                                  (unsigned __int128)f3 * g3 +
                                  (unsigned __int128)f4 * g2);
    unsigned __int128 t2 = (unsigned __int128)f0 * g2 +
        (unsigned __int128)f1 * g1 +
        (unsigned __int128)f2 * g0 +
        (unsigned __int128)19u * ((unsigned __int128)f3 * g4 +
                                  (unsigned __int128)f4 * g3);
    unsigned __int128 t3 = (unsigned __int128)f0 * g3 +
        (unsigned __int128)f1 * g2 +
        (unsigned __int128)f2 * g1 +
        (unsigned __int128)f3 * g0 +
        (unsigned __int128)19u * ((unsigned __int128)f4 * g4);
    unsigned __int128 t4 = (unsigned __int128)f0 * g4 +
        (unsigned __int128)f1 * g3 +
        (unsigned __int128)f2 * g2 +
        (unsigned __int128)f3 * g1 +
        (unsigned __int128)f4 * g0;

    uint64_t c;
    c = (uint64_t)(t0 >> 51); t1 += c; h->v[0] = (uint64_t)t0 & FE_MASK51;
    c = (uint64_t)(t1 >> 51); t2 += c; h->v[1] = (uint64_t)t1 & FE_MASK51;
    c = (uint64_t)(t2 >> 51); t3 += c; h->v[2] = (uint64_t)t2 & FE_MASK51;
    c = (uint64_t)(t3 >> 51); t4 += c; h->v[3] = (uint64_t)t3 & FE_MASK51;
    c = (uint64_t)(t4 >> 51); h->v[4] = (uint64_t)t4 & FE_MASK51;
    h->v[0] += 19u * c;

    c = h->v[0] >> 51; h->v[0] &= FE_MASK51; h->v[1] += c;
}

static void fe_sq(struct Fe *h, const struct Fe *f) {
    fe_mul(h, f, f);
}

static void fe_mul_small(struct Fe *h, const struct Fe *f, uint32_t m) {
    struct Fe g;
    fe_zero(&g);
    g.v[0] = m;
    fe_mul(h, f, &g);
}

static void fe_frombytes(struct Fe *h, const uint8_t s[32]) {
    /* X25519 的坐标是小端字节序。 */
    uint64_t t0 = rd64_le(s + 0);
    uint64_t t1 = rd64_le(s + 8);
    uint64_t t2 = rd64_le(s + 16);
    uint64_t t3 = rd64_le(s + 24);

    h->v[0] = t0 & FE_MASK51;
    h->v[1] = ((t0 >> 51) | (t1 << 13)) & FE_MASK51;
    h->v[2] = ((t1 >> 38) | (t2 << 26)) & FE_MASK51;
    h->v[3] = ((t2 >> 25) | (t3 << 39)) & FE_MASK51;
    h->v[4] = (t3 >> 12) & FE_MASK51;
}

static void fe_tobytes(uint8_t s[32], const struct Fe *f) {
    struct Fe t;
    fe_copy(&t, f);
    fe_carry(&t);

    uint64_t q = (t.v[0] + 19u) >> 51;
    q = (t.v[1] + q) >> 51;
    q = (t.v[2] + q) >> 51;
    q = (t.v[3] + q) >> 51;
    q = (t.v[4] + q) >> 51;

    t.v[0] += 19u * q;
    uint64_t c = t.v[0] >> 51; t.v[0] &= FE_MASK51; t.v[1] += c;
    c = t.v[1] >> 51; t.v[1] &= FE_MASK51; t.v[2] += c;
    c = t.v[2] >> 51; t.v[2] &= FE_MASK51; t.v[3] += c;
    c = t.v[3] >> 51; t.v[3] &= FE_MASK51; t.v[4] += c;
    t.v[4] &= FE_MASK51;

    wr64_le(s + 0, t.v[0] | (t.v[1] << 51));
    wr64_le(s + 8, (t.v[1] >> 13) | (t.v[2] << 38));
    wr64_le(s + 16, (t.v[2] >> 26) | (t.v[3] << 25));
    wr64_le(s + 24, (t.v[3] >> 39) | (t.v[4] << 12));
}

static void fe_cswap(struct Fe *a, struct Fe *b, uint64_t swap) {
    uint64_t mask = (uint64_t)0 - swap;
    for (int i = 0; i < 5; i++) {
        uint64_t t = (a->v[i] ^ b->v[i]) & mask;
        a->v[i] ^= t;
        b->v[i] ^= t;
    }
}

static void fe_invert(struct Fe *out, const struct Fe *z) {
    /* 计算 z^(2^255 - 21)：指数位 5..254 全为 1，低 5 位为 01011。 */
    struct Fe r;
    fe_one(&r);

    for (int i = 254; i >= 0; i--) {
        fe_sq(&r, &r);
        int bit = (i >= 5) ? 1 : ((0x0B >> i) & 1);
        if (bit) fe_mul(&r, &r, z);
    }

    fe_copy(out, &r);
}

static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                              const uint8_t point[32]) {
    uint8_t e[32];
    c_memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    struct Fe x1;
    fe_frombytes(&x1, point);
    fe_carry(&x1);

    struct Fe x2, z2, x3, z3;
    fe_one(&x2);
    fe_zero(&z2);
    fe_copy(&x3, &x1);
    fe_one(&z3);

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t b = (uint64_t)((e[pos >> 3] >> (pos & 7)) & 1);
        swap ^= b;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = b;

        struct Fe a, bb, aa, b2, e1, c, d, da, cb, t;

        fe_add(&a, &x2, &z2);
        fe_sub(&bb, &x2, &z2);
        fe_sq(&aa, &a);
        fe_sq(&b2, &bb);
        fe_sub(&e1, &aa, &b2);
        fe_add(&c, &x3, &z3);
        fe_sub(&d, &x3, &z3);
        fe_mul(&da, &d, &a);
        fe_mul(&cb, &c, &bb);

        fe_add(&t, &da, &cb);
        fe_sq(&x3, &t);

        fe_sub(&t, &da, &cb);
        fe_sq(&z3, &t);
        fe_mul(&z3, &z3, &x1);

        fe_mul(&x2, &aa, &b2);

        fe_mul_small(&t, &e1, 121665u);
        fe_add(&t, &aa, &t);
        fe_mul(&z2, &e1, &t);
    }

    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);

    struct Fe inv, res;
    fe_invert(&inv, &z2);
    fe_mul(&res, &x2, &inv);
    fe_tobytes(out, &res);
}

void x25519_public(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32];
    c_memzero(base, 32);
    base[0] = 9;
    x25519_scalarmult(out, scalar, base);
}

int x25519_shared(uint8_t out[32], const uint8_t scalar[32],
                  const uint8_t peer[32]) {
    uint8_t zero[32];
    c_memzero(zero, 32);
    x25519_scalarmult(out, scalar, peer);
    if (crypto_equal(out, zero, 32)) return 0;
    return 1;
}

/* ============================================================
 * 大数（RSA）
 * ============================================================ */

static void bn_zero(uint32_t *a, int n) {
    for (int i = 0; i < n; i++) a[i] = 0;
}

static void bn_copy(uint32_t *dst, const uint32_t *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

static int bn_cmp(const uint32_t *a, const uint32_t *b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static void bn_sub(uint32_t *a, const uint32_t *b, int n) {
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t av = (uint64_t)a[i];
        uint64_t bv = (uint64_t)b[i] + borrow;
        a[i] = (uint32_t)(av - bv);
        borrow = (av < bv) ? 1u : 0u;
    }
}

static void bn_from_be(uint32_t *a, const uint8_t *p, uint32_t len, int n) {
    bn_zero(a, n);
    for (uint32_t i = 0; i < len && (int)(i / 4) < n; i++) {
        uint32_t byte = p[len - 1 - i];
        a[i / 4] |= byte << ((i % 4) * 8);
    }
}

static void bn_to_be(uint8_t *out, uint32_t out_len, const uint32_t *a, int n) {
    for (uint32_t i = 0; i < out_len; i++) {
        uint32_t limb = (i / 4) < (uint32_t)n ? a[i / 4] : 0;
        out[out_len - 1 - i] = (uint8_t)(limb >> ((i % 4) * 8));
    }
}

static uint32_t bn_n0inv(const uint32_t *n) {
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) {
        inv *= 2u - n[0] * inv;
    }
    return (uint32_t)0 - inv;
}

static void bn_dbl_mod(uint32_t *a, int n, const uint32_t *mod) {
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint32_t next = a[i] >> 31;
        a[i] = (a[i] << 1) | carry;
        carry = next;
    }
    if (carry || bn_cmp(a, mod, n) >= 0) bn_sub(a, mod, n);
}

static void bn_to_mont(uint32_t *out, const uint32_t *a, int n,
                       const uint32_t *mod) {
    bn_copy(out, a, n);
    for (int i = 0; i < 32 * n; i++) bn_dbl_mod(out, n, mod);
}

static void bn_mont_one(uint32_t *out, int n, const uint32_t *mod) {
    bn_zero(out, n);
    out[0] = 1;
    for (int i = 0; i < 32 * n; i++) bn_dbl_mod(out, n, mod);
}

static uint32_t g_mont_tmp[2 * CRYPTO_BN_LIMBS + 4];

static void bn_mont_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
                        int n, const uint32_t *mod, uint32_t n0inv) {
    int total = 2 * n + 2;
    bn_zero(g_mont_tmp, total);

    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t s = (uint64_t)g_mont_tmp[i + j] +
                         (uint64_t)a[j] * (uint64_t)b[i] + carry;
            g_mont_tmp[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        int k = i + n;
        while (carry != 0) {
            uint64_t s = (uint64_t)g_mont_tmp[k] + carry;
            g_mont_tmp[k] = (uint32_t)s;
            carry = s >> 32;
            k++;
        }
    }

    for (int i = 0; i < n; i++) {
        uint32_t m = g_mont_tmp[i] * n0inv;
        uint64_t carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t s = (uint64_t)g_mont_tmp[i + j] +
                         (uint64_t)m * (uint64_t)mod[j] + carry;
            g_mont_tmp[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        int k = i + n;
        while (carry != 0) {
            uint64_t s = (uint64_t)g_mont_tmp[k] + carry;
            g_mont_tmp[k] = (uint32_t)s;
            carry = s >> 32;
            k++;
        }
    }

    for (int i = 0; i < n; i++) out[i] = g_mont_tmp[n + i];

    /* n 的最高位为 1 时 REDC 结果可能落在 [R, 2n)，此时最高 limb
     * 会溢出到 t[2n]。丢掉 R 后直接减一次 n 得到正确结果。 */
    if (g_mont_tmp[2 * n] != 0 || bn_cmp(out, mod, n) >= 0) {
        bn_sub(out, mod, n);
    }
}

static void rsa_modexp(uint8_t *out, uint32_t out_len,
                       const uint8_t *sig, uint32_t sig_len,
                       const uint8_t *exp, uint32_t exp_len,
                       const uint8_t *mod, uint32_t mod_len) {
    int n = (int)((mod_len + 3u) / 4u);
    if (n > CRYPTO_BN_LIMBS) n = CRYPTO_BN_LIMBS;

    static uint32_t bn_mod[CRYPTO_BN_LIMBS];
    static uint32_t bn_base[CRYPTO_BN_LIMBS];
    static uint32_t bn_acc[CRYPTO_BN_LIMBS];
    static uint32_t bn_tmp[CRYPTO_BN_LIMBS];

    bn_from_be(bn_mod, mod, mod_len, n);

    uint8_t sigbuf[CRYPTO_BN_LIMBS * 4];
    c_memzero(sigbuf, sizeof(sigbuf));
    uint32_t copy = sig_len;
    if (copy > sizeof(sigbuf)) copy = sizeof(sigbuf);
    c_memcpy(sigbuf + sizeof(sigbuf) - copy, sig, copy);

    bn_from_be(bn_base, sigbuf, sizeof(sigbuf), n);
    if (bn_cmp(bn_base, bn_mod, n) >= 0) bn_sub(bn_base, bn_mod, n);

    uint32_t n0inv = bn_n0inv(bn_mod);

    bn_to_mont(bn_base, bn_base, n, bn_mod);
    bn_mont_one(bn_acc, n, bn_mod);

    for (int i = (int)exp_len * 8 - 1; i >= 0; i--) {
        bn_mont_mul(bn_tmp, bn_acc, bn_acc, n, bn_mod, n0inv);
        bn_copy(bn_acc, bn_tmp, n);
        if ((exp[(exp_len - 1) - (uint32_t)(i / 8)] >> (i & 7)) & 1u) {
            bn_mont_mul(bn_tmp, bn_acc, bn_base, n, bn_mod, n0inv);
            bn_copy(bn_acc, bn_tmp, n);
        }
    }

    bn_zero(bn_tmp, n);
    bn_tmp[0] = 1;
    bn_mont_mul(bn_base, bn_acc, bn_tmp, n, bn_mod, n0inv);

    bn_to_be(out, out_len, bn_base, n);
}

static uint32_t rsa_bit_length(const uint8_t *n, uint32_t n_len) {
    uint32_t bits = n_len * 8u;
    for (uint32_t i = 0; i < n_len; i++) {
        if (n[i] == 0) {
            bits -= 8u;
            continue;
        }
        uint8_t v = n[i];
        while ((v & 0x80u) == 0) {
            v <<= 1;
            bits--;
        }
        break;
    }
    return bits;
}

static const uint8_t *digest_info_prefix(int hash_id, uint32_t *len) {
    static const uint8_t p_sha1[] = {
        0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,
        0x05,0x00,0x04,0x14
    };
    static const uint8_t p_sha256[] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,
        0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
    };
    static const uint8_t p_sha384[] = {
        0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,
        0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30
    };
    static const uint8_t p_sha512[] = {
        0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,
        0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40
    };

    switch (hash_id) {
    case CRYPTO_HASH_SHA1:
        *len = sizeof(p_sha1);
        return p_sha1;
    case CRYPTO_HASH_SHA256:
        *len = sizeof(p_sha256);
        return p_sha256;
    case CRYPTO_HASH_SHA384:
        *len = sizeof(p_sha384);
        return p_sha384;
    case CRYPTO_HASH_SHA512:
        *len = sizeof(p_sha512);
        return p_sha512;
    default:
        *len = 0;
        return 0;
    }
}

int rsa_verify_pkcs1(const uint8_t *n, uint32_t n_len,
                     const uint8_t *e, uint32_t e_len,
                     const uint8_t *sig, uint32_t sig_len,
                     const uint8_t *digest, uint32_t digest_len,
                     int hash_id) {
    if (!n || !e || !sig || !digest) return 0;
    if (n_len == 0 || n_len > CRYPTO_BN_LIMBS * 4 || sig_len > n_len) return 0;

    uint8_t em[CRYPTO_BN_LIMBS * 4];
    rsa_modexp(em, n_len, sig, sig_len, e, e_len, n, n_len);

    uint32_t prefix_len = 0;
    const uint8_t *prefix = digest_info_prefix(hash_id, &prefix_len);
    if (!prefix) return 0;

    uint32_t t_len = prefix_len + digest_len;
    if (t_len + 11u > n_len) return 0;

    if (em[0] != 0x00 || em[1] != 0x01) return 0;

    uint32_t pos = 2;
    while (pos < n_len && em[pos] == 0xFF) pos++;
    if (pos < 10 || pos >= n_len || em[pos] != 0x00) return 0;

    uint32_t body = n_len - (pos + 1);
    if (body != t_len) return 0;
    if (c_memcmp(em + pos + 1, prefix, prefix_len) != 0) return 0;
    if (c_memcmp(em + pos + 1 + prefix_len, digest, digest_len) != 0) return 0;

    return 1;
}

static void mgf1_sha256(const uint8_t *seed, uint32_t seed_len,
                        uint8_t *out, uint32_t out_len) {
    uint32_t done = 0;
    uint32_t counter = 0;
    while (done < out_len) {
        struct Sha256Ctx ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, seed, seed_len);
        uint8_t c[4];
        wr32_be(c, counter);
        sha256_update(&ctx, c, 4);
        uint8_t block[32];
        sha256_final(&ctx, block);
        uint32_t take = out_len - done;
        if (take > 32) take = 32;
        c_memcpy(out + done, block, take);
        done += take;
        counter++;
    }
}

int rsa_verify_pss_sha256(const uint8_t *n, uint32_t n_len,
                          const uint8_t *e, uint32_t e_len,
                          const uint8_t *sig, uint32_t sig_len,
                          const uint8_t digest[32]) {
    if (!n || !e || !sig || !digest) return 0;
    if (n_len == 0 || n_len > CRYPTO_BN_LIMBS * 4) return 0;

    uint32_t mod_bits = rsa_bit_length(n, n_len);
    uint32_t em_bits = mod_bits - 1u;
    uint32_t em_len = (em_bits + 7u) / 8u;
    if (em_len > n_len || em_len < 32u + 2u) return 0;

    uint8_t em[CRYPTO_BN_LIMBS * 4];
    rsa_modexp(em, n_len, sig, sig_len, e, e_len, n, n_len);

    uint32_t offset = n_len - em_len;
    const uint8_t *p = em + offset;

    if ((p[0] >> (8u - (8u * em_len - em_bits))) != 0) return 0;

    uint32_t h_len = 32;
    uint32_t db_len = em_len - h_len - 1u;
    uint8_t h[32];
    c_memcpy(h, p + db_len, 32);
    uint8_t db[CRYPTO_BN_LIMBS * 4];
    c_memcpy(db, p, db_len);

    db[0] &= (uint8_t)(0xFFu >> (8u * em_len - em_bits));

    uint8_t mask[CRYPTO_BN_LIMBS * 4];
    mgf1_sha256(h, 32, mask, db_len);
    for (uint32_t i = 0; i < db_len; i++) db[i] ^= mask[i];
    db[0] &= (uint8_t)(0xFFu >> (8u * em_len - em_bits));

    uint32_t i = 0;
    while (i < db_len && db[i] == 0) i++;
    if (i >= db_len || db[i] != 0x01) return 0;
    i++;

    if (db_len - i != 32) return 0;
    const uint8_t *salt = db + i;

    uint8_t mprime[8 + 32 + 32];
    c_memzero(mprime, 8);
    c_memcpy(mprime + 8, digest, 32);
    c_memcpy(mprime + 40, salt, 32);

    uint8_t h2[32];
    sha256_buf(mprime, sizeof(mprime), h2);

    return crypto_equal(h, h2, 32);
}

/* ============================================================
 * ECDSA P-256（prime256v1）
 * ------------------------------------------------------------
 * 域元素用 8 个 32 位 limb 表示，模乘走 Montgomery；点运算用
 * Jacobian 坐标，避免每一步都做模逆。验证只做公开数据运算，
 * 不需要常数时间。
 * ============================================================ */

static const uint8_t P256_P_BE[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
};

static const uint8_t P256_N_BE[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,
    0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51
};

static const uint8_t P256_GX_BE[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,
    0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,
    0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96
};

static const uint8_t P256_GY_BE[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,
    0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,
    0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5
};

static const uint8_t P256_B_BE[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,
    0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
    0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,
    0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B
};

static const uint8_t P256_P_MINUS_2[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFD
};

static const uint8_t P256_N_MINUS_2[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,
    0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x4F
};

#define P256_LIMBS 8

static uint32_t g_fp_p[P256_LIMBS];
static uint32_t g_fp_n[P256_LIMBS];
static uint32_t g_fp_p_n0;
static uint32_t g_fp_n_n0;

/* 常用小常数在 Montgomery 域下的表示，首次验证时算一次。 */
static uint32_t g_fp_two[P256_LIMBS];
static uint32_t g_fp_three[P256_LIMBS];
static uint32_t g_fp_four[P256_LIMBS];
static uint32_t g_fp_eight[P256_LIMBS];
static int g_fp_consts_ready = 0;

static void fp_copy(uint32_t *dst, const uint32_t *src) {
    for (int i = 0; i < P256_LIMBS; i++) dst[i] = src[i];
}

static void fp_add(uint32_t *out, const uint32_t *a, const uint32_t *b) {
    uint64_t carry = 0;
    for (int i = 0; i < P256_LIMBS; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry || bn_cmp(out, g_fp_p, P256_LIMBS) >= 0) {
        bn_sub(out, g_fp_p, P256_LIMBS);
    }
}

static void fp_sub(uint32_t *out, const uint32_t *a, const uint32_t *b) {
    uint64_t borrow = 0;
    for (int i = 0; i < P256_LIMBS; i++) {
        uint64_t av = (uint64_t)a[i];
        uint64_t bv = (uint64_t)b[i] + borrow;
        out[i] = (uint32_t)(av - bv);
        borrow = (av < bv) ? 1u : 0u;
    }
    if (borrow) {
        uint64_t carry = 0;
        for (int i = 0; i < P256_LIMBS; i++) {
            uint64_t s = (uint64_t)out[i] + g_fp_p[i] + carry;
            out[i] = (uint32_t)s;
            carry = s >> 32;
        }
    }
}

static void fp_mul(uint32_t *out, const uint32_t *a, const uint32_t *b) {
    bn_mont_mul(out, a, b, P256_LIMBS, g_fp_p, g_fp_p_n0);
}

static void fp_sqr(uint32_t *out, const uint32_t *a) {
    fp_mul(out, a, a);
}

static void fp_from_be(uint32_t *out, const uint8_t be[32]) {
    uint32_t normal[P256_LIMBS];
    bn_from_be(normal, be, 32, P256_LIMBS);
    bn_to_mont(out, normal, P256_LIMBS, g_fp_p);
}

static void fp_to_be(uint8_t be[32], const uint32_t *a) {
    uint32_t one[P256_LIMBS];
    uint32_t normal[P256_LIMBS];
    bn_zero(one, P256_LIMBS);
    one[0] = 1;
    bn_mont_mul(normal, a, one, P256_LIMBS, g_fp_p, g_fp_p_n0);
    bn_to_be(be, 32, normal, P256_LIMBS);
}

static void fp_small(uint32_t *out, uint32_t value) {
    uint32_t normal[P256_LIMBS];
    bn_zero(normal, P256_LIMBS);
    normal[0] = value;
    bn_to_mont(out, normal, P256_LIMBS, g_fp_p);
}

static void fp_inv(uint32_t *out, const uint32_t *a) {
    uint32_t r[P256_LIMBS];
    bn_mont_one(r, P256_LIMBS, g_fp_p);

    for (int i = 255; i >= 0; i--) {
        fp_sqr(r, r);
        if ((P256_P_MINUS_2[31 - (i / 8)] >> (i & 7)) & 1u) {
            fp_mul(r, r, a);
        }
    }

    fp_copy(out, r);
}

struct P256Point {
    uint32_t x[P256_LIMBS];
    uint32_t y[P256_LIMBS];
    uint32_t z[P256_LIMBS];
    int inf;
};

static void pt_infinity(struct P256Point *p) {
    p->inf = 1;
    bn_zero(p->x, P256_LIMBS);
    bn_zero(p->y, P256_LIMBS);
    bn_zero(p->z, P256_LIMBS);
}

static void pt_copy(struct P256Point *dst, const struct P256Point *src) {
    dst->inf = src->inf;
    fp_copy(dst->x, src->x);
    fp_copy(dst->y, src->y);
    fp_copy(dst->z, src->z);
}

static int fp_is_zero(const uint32_t *a) {
    for (int i = 0; i < P256_LIMBS; i++) {
        if (a[i] != 0) return 0;
    }
    return 1;
}

static void pt_double(struct P256Point *r, const struct P256Point *p) {
    if (p->inf || fp_is_zero(p->y)) {
        pt_infinity(r);
        return;
    }

    uint32_t delta[P256_LIMBS], gamma[P256_LIMBS], beta[P256_LIMBS];
    uint32_t alpha[P256_LIMBS], t1[P256_LIMBS], t2[P256_LIMBS];
    uint32_t x3[P256_LIMBS], y3[P256_LIMBS], z3[P256_LIMBS];

    fp_sqr(delta, p->z);
    fp_sqr(gamma, p->y);
    fp_mul(beta, p->x, gamma);

    fp_sub(t1, p->x, delta);
    fp_add(t2, p->x, delta);
    fp_mul(alpha, t1, t2);
    fp_mul(alpha, alpha, g_fp_three);

    fp_sqr(x3, alpha);
    fp_mul(t1, beta, g_fp_eight);
    fp_sub(x3, x3, t1);

    fp_add(t1, p->y, p->z);
    fp_sqr(z3, t1);
    fp_sub(z3, z3, gamma);
    fp_sub(z3, z3, delta);

    fp_mul(t1, beta, g_fp_four);
    fp_sub(t1, t1, x3);
    fp_mul(y3, alpha, t1);
    fp_sqr(t2, gamma);
    fp_mul(t2, t2, g_fp_eight);
    fp_sub(y3, y3, t2);

    fp_copy(r->x, x3);
    fp_copy(r->y, y3);
    fp_copy(r->z, z3);
    r->inf = 0;
}

/* r += q，q 是仿射点（z = 1） */
static void pt_add_affine(struct P256Point *r, const struct P256Point *q) {
    if (r->inf) {
        pt_copy(r, q);
        r->inf = 0;
        return;
    }

    uint32_t z1z1[P256_LIMBS], u2[P256_LIMBS], s2[P256_LIMBS];
    uint32_t h[P256_LIMBS], hh[P256_LIMBS], i4[P256_LIMBS], j[P256_LIMBS];
    uint32_t rr[P256_LIMBS], v[P256_LIMBS], t1[P256_LIMBS], t2[P256_LIMBS];
    uint32_t x3[P256_LIMBS], y3[P256_LIMBS], z3[P256_LIMBS];

    fp_sqr(z1z1, r->z);
    fp_mul(u2, q->x, z1z1);
    fp_mul(s2, q->y, r->z);
    fp_mul(s2, s2, z1z1);

    fp_sub(h, u2, r->x);
    if (fp_is_zero(h)) {
        fp_sub(t1, s2, r->y);
        if (fp_is_zero(t1)) {
            pt_double(r, r);
        } else {
            pt_infinity(r);
        }
        return;
    }

    fp_sqr(hh, h);
    fp_mul(i4, hh, g_fp_four);
    fp_mul(j, h, i4);

    fp_sub(t1, s2, r->y);
    fp_mul(rr, t1, g_fp_two);

    fp_mul(v, r->x, i4);

    fp_sqr(x3, rr);
    fp_sub(x3, x3, j);
    fp_mul(t1, v, g_fp_two);
    fp_sub(x3, x3, t1);

    fp_sub(t1, v, x3);
    fp_mul(y3, rr, t1);
    fp_mul(t2, r->y, j);
    fp_mul(t2, t2, g_fp_two);
    fp_sub(y3, y3, t2);

    fp_add(t1, r->z, h);
    fp_sqr(z3, t1);
    fp_sub(z3, z3, z1z1);
    fp_sub(z3, z3, hh);

    fp_copy(r->x, x3);
    fp_copy(r->y, y3);
    fp_copy(r->z, z3);
    r->inf = 0;
}

/* r = a + b（两个 Jacobian 点） */
static void pt_add(struct P256Point *r, const struct P256Point *a,
                   const struct P256Point *b) {
    if (a->inf) { pt_copy(r, b); return; }
    if (b->inf) { pt_copy(r, a); return; }

    uint32_t z1z1[P256_LIMBS], z2z2[P256_LIMBS];
    uint32_t u1[P256_LIMBS], u2[P256_LIMBS], s1[P256_LIMBS], s2[P256_LIMBS];
    uint32_t h[P256_LIMBS], rr[P256_LIMBS], i4[P256_LIMBS], j[P256_LIMBS];
    uint32_t v[P256_LIMBS], t1[P256_LIMBS], t2[P256_LIMBS];
    uint32_t x3[P256_LIMBS], y3[P256_LIMBS], z3[P256_LIMBS];

    fp_sqr(z1z1, a->z);
    fp_sqr(z2z2, b->z);
    fp_mul(u1, a->x, z2z2);
    fp_mul(u2, b->x, z1z1);
    fp_mul(s1, a->y, b->z);
    fp_mul(s1, s1, z2z2);
    fp_mul(s2, b->y, a->z);
    fp_mul(s2, s2, z1z1);

    fp_sub(h, u2, u1);
    if (fp_is_zero(h)) {
        fp_sub(t1, s2, s1);
        if (fp_is_zero(t1)) {
            pt_double(r, a);
        } else {
            pt_infinity(r);
        }
        return;
    }

    fp_mul(i4, h, g_fp_two);
    fp_sqr(i4, i4);
    fp_mul(j, h, i4);

    fp_sub(t1, s2, s1);
    fp_mul(rr, t1, g_fp_two);

    fp_mul(v, u1, i4);

    fp_sqr(x3, rr);
    fp_sub(x3, x3, j);
    fp_mul(t1, v, g_fp_two);
    fp_sub(x3, x3, t1);

    fp_sub(t1, v, x3);
    fp_mul(y3, rr, t1);
    fp_mul(t2, s1, j);
    fp_mul(t2, t2, g_fp_two);
    fp_sub(y3, y3, t2);

    fp_add(t1, a->z, b->z);
    fp_sqr(t1, t1);
    fp_sub(t1, t1, z1z1);
    fp_sub(t1, t1, z2z2);
    fp_mul(z3, t1, h);

    fp_copy(r->x, x3);
    fp_copy(r->y, y3);
    fp_copy(r->z, z3);
    r->inf = 0;
}

static void pt_mul(struct P256Point *r, const uint8_t scalar[32],
                   const struct P256Point *base) {
    struct P256Point acc;
    pt_infinity(&acc);

    for (int i = 255; i >= 0; i--) {
        if (!acc.inf) pt_double(&acc, &acc);
        if ((scalar[31 - (i / 8)] >> (i & 7)) & 1u) {
            pt_add_affine(&acc, base);
        }
    }

    pt_copy(r, &acc);
}

static void pt_to_affine(uint8_t x_be[32], const struct P256Point *p) {
    uint32_t zinv[P256_LIMBS], z2[P256_LIMBS], ax[P256_LIMBS];

    fp_inv(zinv, p->z);
    fp_sqr(z2, zinv);
    fp_mul(ax, p->x, z2);
    fp_to_be(x_be, ax);
}

int ecdsa_p256_verify_digest(const uint8_t r_be[32], const uint8_t s_be[32],
                             const uint8_t digest[32],
                             const uint8_t qx_be[32], const uint8_t qy_be[32]) {
    bn_from_be(g_fp_p, P256_P_BE, 32, P256_LIMBS);
    bn_from_be(g_fp_n, P256_N_BE, 32, P256_LIMBS);
    g_fp_p_n0 = bn_n0inv(g_fp_p);
    g_fp_n_n0 = bn_n0inv(g_fp_n);

    if (!g_fp_consts_ready) {
        fp_small(g_fp_two, 2);
        fp_small(g_fp_three, 3);
        fp_small(g_fp_four, 4);
        fp_small(g_fp_eight, 8);
        g_fp_consts_ready = 1;
    }

    uint32_t r[P256_LIMBS], s[P256_LIMBS];
    bn_from_be(r, r_be, 32, P256_LIMBS);
    bn_from_be(s, s_be, 32, P256_LIMBS);

    uint32_t zero[P256_LIMBS];
    bn_zero(zero, P256_LIMBS);

    if (bn_cmp(r, zero, P256_LIMBS) == 0 ||
        bn_cmp(s, zero, P256_LIMBS) == 0) {
        return 0;
    }
    if (bn_cmp(r, g_fp_n, P256_LIMBS) >= 0 ||
        bn_cmp(s, g_fp_n, P256_LIMBS) >= 0) {
        return 0;
    }

    /* 公钥点必须在曲线上：y^2 = x^3 - 3x + b */
    uint32_t qx[P256_LIMBS], qy[P256_LIMBS];
    fp_from_be(qx, qx_be);
    fp_from_be(qy, qy_be);

    uint32_t y2[P256_LIMBS], x2[P256_LIMBS], x3[P256_LIMBS];
    uint32_t three_x[P256_LIMBS], b[P256_LIMBS], rhs[P256_LIMBS];
    uint32_t three[P256_LIMBS];

    fp_sqr(y2, qy);
    fp_sqr(x2, qx);
    fp_mul(x3, x2, qx);
    fp_small(three, 3);
    fp_mul(three_x, qx, three);
    fp_from_be(b, P256_B_BE);

    fp_sub(rhs, x3, three_x);
    fp_add(rhs, rhs, b);

    if (c_memcmp(y2, rhs, sizeof(y2)) != 0) return 0;

    /* e = digest mod n（digest < 2^256 < 2n） */
    uint32_t e[P256_LIMBS];
    bn_from_be(e, digest, 32, P256_LIMBS);
    if (bn_cmp(e, g_fp_n, P256_LIMBS) >= 0) bn_sub(e, g_fp_n, P256_LIMBS);

    /* w = s^-1 mod n */
    uint32_t w[P256_LIMBS];
    {
        uint32_t base[P256_LIMBS], acc[P256_LIMBS], one[P256_LIMBS];
        bn_to_mont(base, s, P256_LIMBS, g_fp_n);
        bn_mont_one(acc, P256_LIMBS, g_fp_n);
        for (int i = 255; i >= 0; i--) {
            bn_mont_mul(acc, acc, acc, P256_LIMBS, g_fp_n, g_fp_n_n0);
            if ((P256_N_MINUS_2[31 - (i / 8)] >> (i & 7)) & 1u) {
                bn_mont_mul(acc, acc, base, P256_LIMBS, g_fp_n, g_fp_n_n0);
            }
        }
        bn_zero(one, P256_LIMBS);
        one[0] = 1;
        bn_mont_mul(w, acc, one, P256_LIMBS, g_fp_n, g_fp_n_n0);
    }

    /* u1 = e*w mod n, u2 = r*w mod n */
    uint32_t u1[P256_LIMBS], u2[P256_LIMBS];
    {
        uint32_t a1[P256_LIMBS], b1[P256_LIMBS], t[P256_LIMBS], one[P256_LIMBS];
        bn_zero(one, P256_LIMBS);
        one[0] = 1;

        bn_to_mont(a1, e, P256_LIMBS, g_fp_n);
        bn_to_mont(b1, w, P256_LIMBS, g_fp_n);
        bn_mont_mul(t, a1, b1, P256_LIMBS, g_fp_n, g_fp_n_n0);
        bn_mont_mul(u1, t, one, P256_LIMBS, g_fp_n, g_fp_n_n0);

        bn_to_mont(a1, r, P256_LIMBS, g_fp_n);
        bn_mont_mul(t, a1, b1, P256_LIMBS, g_fp_n, g_fp_n_n0);
        bn_mont_mul(u2, t, one, P256_LIMBS, g_fp_n, g_fp_n_n0);
    }

    struct P256Point g, q, p1, p2, sum;
    fp_from_be(g.x, P256_GX_BE);
    fp_from_be(g.y, P256_GY_BE);
    fp_small(g.z, 1);
    g.inf = 0;

    q.x[0] = qx[0]; q.x[1] = qx[1]; q.x[2] = qx[2]; q.x[3] = qx[3];
    q.x[4] = qx[4]; q.x[5] = qx[5]; q.x[6] = qx[6]; q.x[7] = qx[7];
    q.y[0] = qy[0]; q.y[1] = qy[1]; q.y[2] = qy[2]; q.y[3] = qy[3];
    q.y[4] = qy[4]; q.y[5] = qy[5]; q.y[6] = qy[6]; q.y[7] = qy[7];
    fp_small(q.z, 1);
    q.inf = 0;

    uint8_t u1_be[32], u2_be[32];
    bn_to_be(u1_be, 32, u1, P256_LIMBS);
    bn_to_be(u2_be, 32, u2, P256_LIMBS);

    pt_mul(&p1, u1_be, &g);
    pt_mul(&p2, u2_be, &q);
    pt_add(&sum, &p1, &p2);

    if (sum.inf) return 0;

    uint8_t x_be[32];
    pt_to_affine(x_be, &sum);

    uint32_t v[P256_LIMBS];
    bn_from_be(v, x_be, 32, P256_LIMBS);
    if (bn_cmp(v, g_fp_n, P256_LIMBS) >= 0) bn_sub(v, g_fp_n, P256_LIMBS);

    return c_memcmp(v, r, sizeof(v)) == 0;
}

/* ============================================================
 * ECDSA P-384（secp384r1）
 * ------------------------------------------------------------
 * 大多数现代 CA 的中间证书使用 P-384（例如 SSL.com Transit、
 * Let's Encrypt 的上级链）。实现方式与 P-256 相同，只是 limb
 * 数从 8 增加到 12。
 * ============================================================ */

static const uint8_t P384_P_BE[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF
};

static const uint8_t P384_N_BE[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
    0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,
    0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73
};

static const uint8_t P384_GX_BE[48] = {
    0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,
    0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,
    0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,
    0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,
    0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,
    0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7
};

static const uint8_t P384_GY_BE[48] = {
    0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,
    0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,
    0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,
    0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,
    0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,
    0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F
};

static const uint8_t P384_B_BE[48] = {
    0xB3,0x31,0x2F,0xA7,0xE2,0x3E,0xE7,0xE4,
    0x98,0x8E,0x05,0x6B,0xE3,0xF8,0x2D,0x19,
    0x18,0x1D,0x9C,0x6E,0xFE,0x81,0x41,0x12,
    0x03,0x14,0x08,0x8F,0x50,0x13,0x87,0x5A,
    0xC6,0x56,0x39,0x8D,0x8A,0x2E,0xD1,0x9D,
    0x2A,0x85,0xC8,0xED,0xD3,0xEC,0x2A,0xEF
};

static const uint8_t P384_P_MINUS_2[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFD
};

static const uint8_t P384_N_MINUS_2[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
    0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,
    0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x71
};

#define P384_LIMBS 12

static uint32_t g_p3_p[P384_LIMBS];
static uint32_t g_p3_n[P384_LIMBS];
static uint32_t g_p3_p_n0;
static uint32_t g_p3_n_n0;
static uint32_t g_p3_two[P384_LIMBS];
static uint32_t g_p3_three[P384_LIMBS];
static uint32_t g_p3_four[P384_LIMBS];
static uint32_t g_p3_eight[P384_LIMBS];
static int g_p3_ready = 0;

static void p3_copy(uint32_t *dst, const uint32_t *src) {
    for (int i = 0; i < P384_LIMBS; i++) dst[i] = src[i];
}

static int p3_is_zero(const uint32_t *a) {
    for (int i = 0; i < P384_LIMBS; i++) {
        if (a[i] != 0) return 0;
    }
    return 1;
}

static void p3_add(uint32_t *out, const uint32_t *a, const uint32_t *b) {
    uint64_t carry = 0;
    for (int i = 0; i < P384_LIMBS; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry || bn_cmp(out, g_p3_p, P384_LIMBS) >= 0) {
        bn_sub(out, g_p3_p, P384_LIMBS);
    }
}

static void p3_sub(uint32_t *out, const uint32_t *a, const uint32_t *b) {
    uint64_t borrow = 0;
    for (int i = 0; i < P384_LIMBS; i++) {
        uint64_t av = (uint64_t)a[i];
        uint64_t bv = (uint64_t)b[i] + borrow;
        out[i] = (uint32_t)(av - bv);
        borrow = (av < bv) ? 1u : 0u;
    }
    if (borrow) {
        uint64_t carry = 0;
        for (int i = 0; i < P384_LIMBS; i++) {
            uint64_t s = (uint64_t)out[i] + g_p3_p[i] + carry;
            out[i] = (uint32_t)s;
            carry = s >> 32;
        }
    }
}

static void p3_mul(uint32_t *out, const uint32_t *a, const uint32_t *b) {
    bn_mont_mul(out, a, b, P384_LIMBS, g_p3_p, g_p3_p_n0);
}

static void p3_sqr(uint32_t *out, const uint32_t *a) {
    p3_mul(out, a, a);
}

static void p3_from_be(uint32_t *out, const uint8_t be[48]) {
    uint32_t normal[P384_LIMBS];
    bn_from_be(normal, be, 48, P384_LIMBS);
    bn_to_mont(out, normal, P384_LIMBS, g_p3_p);
}

static void p3_to_be(uint8_t be[48], const uint32_t *a) {
    uint32_t one[P384_LIMBS];
    uint32_t normal[P384_LIMBS];
    bn_zero(one, P384_LIMBS);
    one[0] = 1;
    bn_mont_mul(normal, a, one, P384_LIMBS, g_p3_p, g_p3_p_n0);
    bn_to_be(be, 48, normal, P384_LIMBS);
}

static void p3_small(uint32_t *out, uint32_t value) {
    uint32_t normal[P384_LIMBS];
    bn_zero(normal, P384_LIMBS);
    normal[0] = value;
    bn_to_mont(out, normal, P384_LIMBS, g_p3_p);
}

static void p3_inv(uint32_t *out, const uint32_t *a) {
    uint32_t r[P384_LIMBS];
    bn_mont_one(r, P384_LIMBS, g_p3_p);

    for (int i = 383; i >= 0; i--) {
        p3_sqr(r, r);
        if ((P384_P_MINUS_2[47 - (i / 8)] >> (i & 7)) & 1u) {
            p3_mul(r, r, a);
        }
    }

    p3_copy(out, r);
}

struct P384Point {
    uint32_t x[P384_LIMBS];
    uint32_t y[P384_LIMBS];
    uint32_t z[P384_LIMBS];
    int inf;
};

static void p3_inf(struct P384Point *p) {
    p->inf = 1;
    bn_zero(p->x, P384_LIMBS);
    bn_zero(p->y, P384_LIMBS);
    bn_zero(p->z, P384_LIMBS);
}

static void p3_pt_copy(struct P384Point *dst, const struct P384Point *src) {
    dst->inf = src->inf;
    p3_copy(dst->x, src->x);
    p3_copy(dst->y, src->y);
    p3_copy(dst->z, src->z);
}

static void p3_double(struct P384Point *r, const struct P384Point *p) {
    if (p->inf || p3_is_zero(p->y)) {
        p3_inf(r);
        return;
    }

    uint32_t delta[P384_LIMBS], gamma[P384_LIMBS], beta[P384_LIMBS];
    uint32_t alpha[P384_LIMBS], t1[P384_LIMBS], t2[P384_LIMBS];
    uint32_t x3[P384_LIMBS], y3[P384_LIMBS], z3[P384_LIMBS];

    p3_sqr(delta, p->z);
    p3_sqr(gamma, p->y);
    p3_mul(beta, p->x, gamma);

    p3_sub(t1, p->x, delta);
    p3_add(t2, p->x, delta);
    p3_mul(alpha, t1, t2);
    p3_mul(alpha, alpha, g_p3_three);

    p3_sqr(x3, alpha);
    p3_mul(t1, beta, g_p3_eight);
    p3_sub(x3, x3, t1);

    p3_add(t1, p->y, p->z);
    p3_sqr(z3, t1);
    p3_sub(z3, z3, gamma);
    p3_sub(z3, z3, delta);

    p3_mul(t1, beta, g_p3_four);
    p3_sub(t1, t1, x3);
    p3_mul(y3, alpha, t1);
    p3_sqr(t2, gamma);
    p3_mul(t2, t2, g_p3_eight);
    p3_sub(y3, y3, t2);

    p3_copy(r->x, x3);
    p3_copy(r->y, y3);
    p3_copy(r->z, z3);
    r->inf = 0;
}

static void p3_add_affine(struct P384Point *r, const struct P384Point *q) {
    if (r->inf) {
        p3_pt_copy(r, q);
        r->inf = 0;
        return;
    }

    uint32_t z1z1[P384_LIMBS], u2[P384_LIMBS], s2[P384_LIMBS];
    uint32_t h[P384_LIMBS], hh[P384_LIMBS], i4[P384_LIMBS], j[P384_LIMBS];
    uint32_t rr[P384_LIMBS], v[P384_LIMBS], t1[P384_LIMBS], t2[P384_LIMBS];
    uint32_t x3[P384_LIMBS], y3[P384_LIMBS], z3[P384_LIMBS];

    p3_sqr(z1z1, r->z);
    p3_mul(u2, q->x, z1z1);
    p3_mul(s2, q->y, r->z);
    p3_mul(s2, s2, z1z1);

    p3_sub(h, u2, r->x);
    if (p3_is_zero(h)) {
        p3_sub(t1, s2, r->y);
        if (p3_is_zero(t1)) {
            p3_double(r, r);
        } else {
            p3_inf(r);
        }
        return;
    }

    p3_sqr(hh, h);
    p3_mul(i4, hh, g_p3_four);
    p3_mul(j, h, i4);

    p3_sub(t1, s2, r->y);
    p3_mul(rr, t1, g_p3_two);

    p3_mul(v, r->x, i4);

    p3_sqr(x3, rr);
    p3_sub(x3, x3, j);
    p3_mul(t1, v, g_p3_two);
    p3_sub(x3, x3, t1);

    p3_sub(t1, v, x3);
    p3_mul(y3, rr, t1);
    p3_mul(t2, r->y, j);
    p3_mul(t2, t2, g_p3_two);
    p3_sub(y3, y3, t2);

    p3_add(t1, r->z, h);
    p3_sqr(z3, t1);
    p3_sub(z3, z3, z1z1);
    p3_sub(z3, z3, hh);

    p3_copy(r->x, x3);
    p3_copy(r->y, y3);
    p3_copy(r->z, z3);
    r->inf = 0;
}

static void p3_add_points(struct P384Point *r, const struct P384Point *a,
                          const struct P384Point *b) {
    if (a->inf) { p3_pt_copy(r, b); return; }
    if (b->inf) { p3_pt_copy(r, a); return; }

    uint32_t z1z1[P384_LIMBS], z2z2[P384_LIMBS];
    uint32_t u1[P384_LIMBS], u2[P384_LIMBS], s1[P384_LIMBS], s2[P384_LIMBS];
    uint32_t h[P384_LIMBS], rr[P384_LIMBS], i4[P384_LIMBS], j[P384_LIMBS];
    uint32_t v[P384_LIMBS], t1[P384_LIMBS], t2[P384_LIMBS];
    uint32_t x3[P384_LIMBS], y3[P384_LIMBS], z3[P384_LIMBS];

    p3_sqr(z1z1, a->z);
    p3_sqr(z2z2, b->z);
    p3_mul(u1, a->x, z2z2);
    p3_mul(u2, b->x, z1z1);
    p3_mul(s1, a->y, b->z);
    p3_mul(s1, s1, z2z2);
    p3_mul(s2, b->y, a->z);
    p3_mul(s2, s2, z1z1);

    p3_sub(h, u2, u1);
    if (p3_is_zero(h)) {
        p3_sub(t1, s2, s1);
        if (p3_is_zero(t1)) {
            p3_double(r, a);
        } else {
            p3_inf(r);
        }
        return;
    }

    p3_mul(i4, h, g_p3_two);
    p3_sqr(i4, i4);
    p3_mul(j, h, i4);

    p3_sub(t1, s2, s1);
    p3_mul(rr, t1, g_p3_two);

    p3_mul(v, u1, i4);

    p3_sqr(x3, rr);
    p3_sub(x3, x3, j);
    p3_mul(t1, v, g_p3_two);
    p3_sub(x3, x3, t1);

    p3_sub(t1, v, x3);
    p3_mul(y3, rr, t1);
    p3_mul(t2, s1, j);
    p3_mul(t2, t2, g_p3_two);
    p3_sub(y3, y3, t2);

    p3_add(t1, a->z, b->z);
    p3_sqr(t1, t1);
    p3_sub(t1, t1, z1z1);
    p3_sub(t1, t1, z2z2);
    p3_mul(z3, t1, h);

    p3_copy(r->x, x3);
    p3_copy(r->y, y3);
    p3_copy(r->z, z3);
    r->inf = 0;
}

static void p3_pt_mul(struct P384Point *r, const uint8_t scalar[48],
                      const struct P384Point *base) {
    struct P384Point acc;
    p3_inf(&acc);

    for (int i = 383; i >= 0; i--) {
        if (!acc.inf) p3_double(&acc, &acc);
        if ((scalar[47 - (i / 8)] >> (i & 7)) & 1u) {
            p3_add_affine(&acc, base);
        }
    }

    p3_pt_copy(r, &acc);
}

static void p3_to_affine(uint8_t x_be[48], const struct P384Point *p) {
    uint32_t zinv[P384_LIMBS], z2[P384_LIMBS], ax[P384_LIMBS];

    p3_inv(zinv, p->z);
    p3_sqr(z2, zinv);
    p3_mul(ax, p->x, z2);
    p3_to_be(x_be, ax);
}

int ecdsa_p384_verify_digest(const uint8_t r_be[48], const uint8_t s_be[48],
                             const uint8_t digest[48],
                             const uint8_t qx_be[48], const uint8_t qy_be[48]) {
    bn_from_be(g_p3_p, P384_P_BE, 48, P384_LIMBS);
    bn_from_be(g_p3_n, P384_N_BE, 48, P384_LIMBS);
    g_p3_p_n0 = bn_n0inv(g_p3_p);
    g_p3_n_n0 = bn_n0inv(g_p3_n);

    if (!g_p3_ready) {
        p3_small(g_p3_two, 2);
        p3_small(g_p3_three, 3);
        p3_small(g_p3_four, 4);
        p3_small(g_p3_eight, 8);
        g_p3_ready = 1;
    }

    uint32_t r[P384_LIMBS], s[P384_LIMBS], zero[P384_LIMBS];
    bn_from_be(r, r_be, 48, P384_LIMBS);
    bn_from_be(s, s_be, 48, P384_LIMBS);
    bn_zero(zero, P384_LIMBS);

    if (bn_cmp(r, zero, P384_LIMBS) == 0 ||
        bn_cmp(s, zero, P384_LIMBS) == 0) {
        return 0;
    }
    if (bn_cmp(r, g_p3_n, P384_LIMBS) >= 0 ||
        bn_cmp(s, g_p3_n, P384_LIMBS) >= 0) {
        return 0;
    }

    uint32_t qx[P384_LIMBS], qy[P384_LIMBS];
    p3_from_be(qx, qx_be);
    p3_from_be(qy, qy_be);

    uint32_t y2[P384_LIMBS], x2[P384_LIMBS], x3[P384_LIMBS];
    uint32_t three_x[P384_LIMBS], b[P384_LIMBS], rhs[P384_LIMBS];

    p3_sqr(y2, qy);
    p3_sqr(x2, qx);
    p3_mul(x3, x2, qx);
    p3_mul(three_x, qx, g_p3_three);
    p3_from_be(b, P384_B_BE);

    p3_sub(rhs, x3, three_x);
    p3_add(rhs, rhs, b);

    if (c_memcmp(y2, rhs, sizeof(y2)) != 0) return 0;

    uint32_t e[P384_LIMBS];
    bn_from_be(e, digest, 48, P384_LIMBS);
    if (bn_cmp(e, g_p3_n, P384_LIMBS) >= 0) bn_sub(e, g_p3_n, P384_LIMBS);

    uint32_t w[P384_LIMBS];
    {
        uint32_t base[P384_LIMBS], acc[P384_LIMBS], one[P384_LIMBS];
        bn_to_mont(base, s, P384_LIMBS, g_p3_n);
        bn_mont_one(acc, P384_LIMBS, g_p3_n);
        for (int i = 383; i >= 0; i--) {
            bn_mont_mul(acc, acc, acc, P384_LIMBS, g_p3_n, g_p3_n_n0);
            if ((P384_N_MINUS_2[47 - (i / 8)] >> (i & 7)) & 1u) {
                bn_mont_mul(acc, acc, base, P384_LIMBS, g_p3_n, g_p3_n_n0);
            }
        }
        bn_zero(one, P384_LIMBS);
        one[0] = 1;
        bn_mont_mul(w, acc, one, P384_LIMBS, g_p3_n, g_p3_n_n0);
    }

    uint32_t u1[P384_LIMBS], u2[P384_LIMBS];
    {
        uint32_t a1[P384_LIMBS], b1[P384_LIMBS], t[P384_LIMBS], one[P384_LIMBS];
        bn_zero(one, P384_LIMBS);
        one[0] = 1;

        bn_to_mont(a1, e, P384_LIMBS, g_p3_n);
        bn_to_mont(b1, w, P384_LIMBS, g_p3_n);
        bn_mont_mul(t, a1, b1, P384_LIMBS, g_p3_n, g_p3_n_n0);
        bn_mont_mul(u1, t, one, P384_LIMBS, g_p3_n, g_p3_n_n0);

        bn_to_mont(a1, r, P384_LIMBS, g_p3_n);
        bn_mont_mul(t, a1, b1, P384_LIMBS, g_p3_n, g_p3_n_n0);
        bn_mont_mul(u2, t, one, P384_LIMBS, g_p3_n, g_p3_n_n0);
    }

    struct P384Point g, q, p1, p2, sum;
    p3_from_be(g.x, P384_GX_BE);
    p3_from_be(g.y, P384_GY_BE);
    p3_small(g.z, 1);
    g.inf = 0;

    p3_copy(q.x, qx);
    p3_copy(q.y, qy);
    p3_small(q.z, 1);
    q.inf = 0;

    uint8_t u1_be[48], u2_be[48];
    bn_to_be(u1_be, 48, u1, P384_LIMBS);
    bn_to_be(u2_be, 48, u2, P384_LIMBS);

    p3_pt_mul(&p1, u1_be, &g);
    p3_pt_mul(&p2, u2_be, &q);
    p3_add_points(&sum, &p1, &p2);

    if (sum.inf) return 0;

    uint8_t x_be[48];
    p3_to_affine(x_be, &sum);

    uint32_t v[P384_LIMBS];
    bn_from_be(v, x_be, 48, P384_LIMBS);
    if (bn_cmp(v, g_p3_n, P384_LIMBS) >= 0) bn_sub(v, g_p3_n, P384_LIMBS);

    return c_memcmp(v, r, sizeof(v)) == 0;
}

/* ============================================================
 * 已知答案自检
 * ============================================================ */

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, uint32_t cap) {
    uint32_t n = 0;
    while (hex[0] && hex[1]) {
        int hi = hex_digit(hex[0]);
        int lo = hex_digit(hex[1]);
        if (hi < 0 || lo < 0 || n >= cap) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return (int)n;
}

int crypto_selftest(void) {
    int failed = 0;

    static const char *sha256_abc =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char *sha1_abc =
        "a9993e364706816aba3e25717850c26c9cd0d89d";
    static const char *sha384_abc =
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
        "8086072ba1e7cc2358baeca134c825a7";
    static const char *sha512_abc =
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    static const char *hmac_rfc4231 =
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    static const char *aes_block =
        "69c4e0d86a7b0430d8cdb78070b4c55a";
    static const char *gcm_tag_empty =
        "58e2fccefa7e3061367f1d57a4e7455a";
    static const char *gcm_ct =
        "0388dace60b6a392f328c2b971b2fe78";
    static const char *gcm_tag =
        "ab6e47d42cec13bdf53a67b21257bddf";
    static const char *x25519_expected =
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";

    uint8_t expect[64];
    uint8_t got[64];

    sha256_buf("abc", 3, got);
    hex_decode(sha256_abc, expect, sizeof(expect));
    if (!crypto_equal(got, expect, 32)) failed++;

    sha1_buf("abc", 3, got);
    hex_decode(sha1_abc, expect, sizeof(expect));
    if (!crypto_equal(got, expect, 20)) failed++;

    sha384_buf("abc", 3, got);
    hex_decode(sha384_abc, expect, sizeof(expect));
    if (!crypto_equal(got, expect, 48)) failed++;

    sha512_buf("abc", 3, got);
    hex_decode(sha512_abc, expect, sizeof(expect));
    if (!crypto_equal(got, expect, 64)) failed++;

    {
        uint8_t key[20];
        c_memset(key, 0x0B, 20);
        hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, got);
        hex_decode(hmac_rfc4231, expect, sizeof(expect));
        if (!crypto_equal(got, expect, 32)) failed++;
    }

    {
        uint8_t key[16];
        uint8_t pt[16];
        uint8_t ct[16];
        hex_decode("000102030405060708090a0b0c0d0e0f", key, 16);
        hex_decode("00112233445566778899aabbccddeeff", pt, 16);

        struct Aes128 aes;
        aes128_init(&aes, key);
        aes128_encrypt_block(&aes, pt, ct);
        hex_decode(aes_block, expect, sizeof(expect));
        if (!crypto_equal(ct, expect, 16)) failed++;
    }

    {
        uint8_t key[16];
        uint8_t iv[12];
        uint8_t tag[16];
        c_memset(key, 0, 16);
        c_memset(iv, 0, 12);

        aes128_gcm_encrypt(key, iv, 12, 0, 0, 0, 0, 0, tag);
        hex_decode(gcm_tag_empty, expect, sizeof(expect));
        if (!crypto_equal(tag, expect, 16)) failed++;
    }

    {
        uint8_t key[16];
        uint8_t iv[12];
        uint8_t pt[16];
        uint8_t ct[16];
        uint8_t tag[16];
        c_memset(key, 0, 16);
        c_memset(iv, 0, 12);
        c_memset(pt, 0, 16);

        aes128_gcm_encrypt(key, iv, 12, 0, 0, pt, 16, ct, tag);
        hex_decode(gcm_ct, expect, sizeof(expect));
        if (!crypto_equal(ct, expect, 16)) failed++;
        hex_decode(gcm_tag, expect, sizeof(expect));
        if (!crypto_equal(tag, expect, 16)) failed++;
    }

    {
        /* NIST GCM 测试用例 3：64 字节明文 + AAD，覆盖多块 CTR/GHASH */
        uint8_t key[16], iv[12], aad[20], pt[64], ct[64], tag[16];
        hex_decode("feffe9928665731c6d6a8f9467308308", key, 16);
        hex_decode("cafebabefacedbaddecaf888", iv, 12);
        hex_decode("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
        int pl = hex_decode("d9313225f88406e5a55909c5aff5269a"
                            "86a7a9531534f7da2e4c303d8a318a72"
                            "1c3c0c95956809532fcf0e2449a6b525"
                            "b16aedf5aa0de657ba637b391aafd255", pt, 64);

        aes128_gcm_encrypt(key, iv, 12, aad, 20, pt, (uint32_t)pl, ct, tag);

        hex_decode("42831ec2217774244b7221b784d0d49c"
                   "e3aa212f2c02a4e035c17e2329aca12e"
                   "21d514b25466931c7d8f6a5aac84aa05"
                   "1ba30b396a0aac973d58e091473f5985", expect, sizeof(expect));
        if (!crypto_equal(ct, expect, (uint32_t)pl)) failed++;

        hex_decode("4d5c2af327cd64a62cf35abd2ba6fab4", expect, sizeof(expect));
        if (!crypto_equal(tag, expect, 16)) failed++;

        uint8_t back[64];
        if (!aes128_gcm_decrypt(key, iv, 12, aad, 20, ct, (uint32_t)pl,
                                tag, back)) {
            failed++;
        } else if (!crypto_equal(back, pt, (uint32_t)pl)) {
            failed++;
        }
    }

    {
        uint8_t ikm[22];
        uint8_t salt[13];
        uint8_t info[10];
        uint8_t prk[32];
        uint8_t okm[42];

        c_memset(ikm, 0x0B, 22);
        hex_decode("000102030405060708090a0b0c", salt, 13);
        hex_decode("f0f1f2f3f4f5f6f7f8f9", info, 10);

        hkdf_extract(salt, 13, ikm, 22, prk);
        hex_decode("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
                   expect, sizeof(expect));
        if (!crypto_equal(prk, expect, 32)) failed++;

        if (!hkdf_expand(prk, info, 10, okm, 42)) {
            failed++;
        } else {
            hex_decode("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                       "34007208d5b887185865",
                       expect, sizeof(expect));
            if (!crypto_equal(okm, expect, 42)) failed++;
        }
    }

    {
        uint8_t scalar[32];
        uint8_t point[32];
        uint8_t shared[32];

        hex_decode("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                   scalar, 32);
        hex_decode("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                   point, 32);

        if (!x25519_shared(shared, scalar, point)) {
            failed++;
        } else {
            hex_decode(x25519_expected, expect, sizeof(expect));
            if (!crypto_equal(shared, expect, 32)) failed++;
        }
    }

    {
        /* RFC 6979 A.2.5：P-256 / SHA-256 / "sample" 的确定性 ECDSA 向量 */
        uint8_t qx[32], qy[32], r[32], s[32], dg[32];
        hex_decode("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6",
                   qx, 32);
        hex_decode("7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299",
                   qy, 32);
        hex_decode("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716",
                   r, 32);
        hex_decode("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8",
                   s, 32);
        hex_decode("AF2BDBE1AA9B6EC1E2ADE1D694F41FC71A831D0268E9891562113D8A62ADD1BF",
                   dg, 32);

        if (!ecdsa_p256_verify_digest(r, s, dg, qx, qy)) failed++;

        /* 篡改摘要后必须拒绝 */
        dg[0] ^= 0x01;
        if (ecdsa_p256_verify_digest(r, s, dg, qx, qy)) failed++;
    }

    return failed;
}
