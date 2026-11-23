/* ============================================================
 * inflate.c - zlib / DEFLATE 解压（PNG 使用）
 * ------------------------------------------------------------
 * 只实现 RFC 1950/1951 的解压方向：
 *   - zlib 头（CMF/FLG）校验；
 *   - stored / fixed Huffman / dynamic Huffman 三种块；
 *   - LZ77 回溯直接在当前输出缓冲里复制，不需要额外 32KB 窗口。
 * 输出大小由调用方预先算好（PNG 扫描线长度固定），因此不实现
 * 增量流式接口，也不依赖动态扩容。
 * ============================================================ */

#include "imgcodec.h"

typedef struct {
    const uint8_t *in;
    int in_len;
    int in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    int out_cap;
    int out_pos;
    int err;
} Inf;

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} InfHuff;

static int inf_getbits(Inf *s, int need) {
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) {
            s->err = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1u));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

static void inf_align(Inf *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
}

/* 由码长表构建 canonical Huffman 解码表。 */
static int huff_build(InfHuff *h, const uint8_t *lens, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i] & 15]++;
    h->count[0] = 0;

    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return 0;
    }

    uint16_t offs[16];
    offs[0] = 0;
    for (int len = 1; len < 16; len++) {
        offs[len] = (uint16_t)(offs[len - 1] + h->count[len - 1]);
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i] & 15;
        if (l) h->symbol[offs[l]++] = (uint16_t)i;
    }
    return 1;
}

static int huff_decode(Inf *s, const InfHuff *h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_getbits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int inf_block(Inf *s, const InfHuff *lit, const InfHuff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) { s->err = 1; return 0; }
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return 0; }
            int len = len_base[sym] + inf_getbits(s, len_extra[sym]);
            int dsym = huff_decode(s, dist);
            if (dsym < 0 || dsym >= 30) { s->err = 1; return 0; }
            int d = dist_base[dsym] + inf_getbits(s, dist_extra[dsym]);
            if (s->err) return 0;
            if (d <= 0 || d > s->out_pos) { s->err = 1; return 0; }
            if (s->out_pos + len > s->out_cap) { s->err = 1; return 0; }
            int from = s->out_pos - d;
            for (int i = 0; i < len; i++) {
                s->out[s->out_pos++] = s->out[from + i];
            }
        }
    }
}

static int inf_fixed(Inf *s) {
    uint8_t lens[288];
    for (int i = 0; i < 144; i++) lens[i] = 8;
    for (int i = 144; i < 256; i++) lens[i] = 9;
    for (int i = 256; i < 280; i++) lens[i] = 7;
    for (int i = 280; i < 288; i++) lens[i] = 8;
    InfHuff lit;
    if (!huff_build(&lit, lens, 288)) return 0;

    uint8_t dl[30];
    for (int i = 0; i < 30; i++) dl[i] = 5;
    InfHuff dist;
    if (!huff_build(&dist, dl, 30)) return 0;
    return inf_block(s, &lit, &dist);
}

static int inf_dynamic(Inf *s) {
    int hlit = inf_getbits(s, 5) + 257;
    int hdist = inf_getbits(s, 5) + 1;
    int hclen = inf_getbits(s, 4) + 4;
    if (s->err || hlit > 286 || hdist > 30) return 0;

    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t clen[19];
    for (int i = 0; i < 19; i++) clen[i] = 0;
    for (int i = 0; i < hclen; i++) clen[order[i]] = (uint8_t)inf_getbits(s, 3);
    if (s->err) return 0;

    InfHuff ch;
    if (!huff_build(&ch, clen, 19)) return 0;

    uint8_t lens[320];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(s, &ch);
        if (sym < 0) return 0;
        if (sym < 16) {
            lens[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return 0;
            int rep = 3 + inf_getbits(s, 2);
            uint8_t prev = lens[n - 1];
            while (rep-- && n < hlit + hdist) lens[n++] = prev;
        } else if (sym == 17) {
            int rep = 3 + inf_getbits(s, 3);
            while (rep-- && n < hlit + hdist) lens[n++] = 0;
        } else {
            int rep = 11 + inf_getbits(s, 7);
            while (rep-- && n < hlit + hdist) lens[n++] = 0;
        }
        if (s->err) return 0;
    }

    InfHuff lit, dist;
    if (!huff_build(&lit, lens, hlit)) return 0;
    if (!huff_build(&dist, lens + hlit, hdist)) return 0;
    return inf_block(s, &lit, &dist);
}

int img_inflate(const uint8_t *src, int src_len,
                uint8_t *dst, int dst_cap, int *dst_len) {
    if (!src || src_len < 2 || !dst || dst_cap <= 0) return 0;

    /* zlib 头：CM=8，CINFO<=7，头两字节按 16 位大端可被 31 整除。 */
    if ((src[0] & 0x0F) != 8) return 0;
    if (((int)src[0] * 256 + src[1]) % 31 != 0) return 0;
    if (src[1] & 0x20) return 0;   /* FDICT 不支持 */

    Inf s;
    s.in = src;
    s.in_len = src_len;
    s.in_pos = 2;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.out = dst;
    s.out_cap = dst_cap;
    s.out_pos = 0;
    s.err = 0;

    for (;;) {
        int final = inf_getbits(&s, 1);
        int type = inf_getbits(&s, 2);
        if (s.err) return 0;

        if (type == 0) {
            inf_align(&s);
            if (s.in_pos + 4 > s.in_len) return 0;
            int len = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
            int nlen = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if ((len ^ 0xFFFF) != nlen) return 0;
            if (s.in_pos + len > s.in_len) return 0;
            if (s.out_pos + len > s.out_cap) return 0;
            for (int i = 0; i < len; i++) dst[s.out_pos++] = s.in[s.in_pos++];
        } else if (type == 1) {
            if (!inf_fixed(&s)) return 0;
        } else if (type == 2) {
            if (!inf_dynamic(&s)) return 0;
        } else {
            return 0;
        }

        if (s.err) return 0;
        if (final) break;
    }

    if (dst_len) *dst_len = s.out_pos;
    return 1;
}
