/* ============================================================
 * jpeg_dec.c - baseline JPEG 解码
 * ------------------------------------------------------------
 * 支持 SOF0（baseline Huffman）、4:4:4 / 4:2:2 / 4:2:0 / 灰度、
 * DRI 重启间隔、任意量化表。渐进式（SOF2）与算术编码不支持。
 *
 * 大图处理：
 *   超过约 400 万像素时按 1/8 DCT 缩放解码（每个 8x8 块只取 DC），
 *   8256x5504 的壁纸解码后约 1032x688，内存与时间都可控；
 *   小图走完整 8x8 整数 IDCT。
 * ============================================================ */

#include "imgcodec.h"

typedef struct {
    uint8_t  id;
    uint8_t  h, v;
    uint8_t  tq;
    uint8_t  td, ta;
    int      dc_pred;
    int      cw, ch;
    int      bw, bh;
    uint8_t *plane;
} JComp;

typedef struct {
    uint16_t count[17];
    uint8_t  symbol[256];
} JHuff;

typedef struct {
    const uint8_t *data;
    int len;
    int pos;
    uint32_t buf;
    int bits;
    int err;
} JBits;

typedef struct {
    const uint8_t *data;
    int len;
    int pos;
    int width, height;
    int ncomp;
    int hmax, vmax;
    int mcu_cols, mcu_rows;
    int restart_interval;
    int scale;              /* 1 或 8 */
    uint16_t q[4][64];
    JHuff dc[4];
    JHuff ac[4];
    int dc_set[4], ac_set[4], q_set[4];
    JComp comp[4];
} Jpeg;

static const uint8_t zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const int32_t jcos[8][8] = {
    { 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384 },
    { 16069, 13623, 9102, 3196, -3196, -9102, -13623, -16069 },
    { 15137, 6270, -6270, -15137, -15137, -6270, 6270, 15137 },
    { 13623, -3196, -16069, -9102, 9102, 16069, 3196, -13623 },
    { 11585, -11585, -11585, 11585, 11585, -11585, -11585, 11585 },
    { 9102, -16069, 3196, 13623, -13623, -3196, 16069, -9102 },
    { 6270, -15137, 15137, -6270, -6270, 15137, -15137, 6270 },
    { 3196, -9102, 13623, -16069, 16069, -13623, 9102, -3196 }
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static int jgetb(JBits *b) {
    if (b->bits == 0) {
        if (b->pos >= b->len) { b->err = 1; return 0; }
        uint8_t c = b->data[b->pos++];
        if (c == 0xFF) {
            if (b->pos >= b->len) { b->err = 1; return 0; }
            uint8_t m = b->data[b->pos];
            if (m == 0x00) {
                b->pos++;
            } else {
                /* 熵数据里出现裸 marker：到重启边界时应由外部先对齐处理 */
                b->err = 1;
                return 0;
            }
        }
        b->buf = c;
        b->bits = 8;
    }
    b->bits--;
    return (int)((b->buf >> b->bits) & 1u);
}

static int jgetbits(JBits *b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | jgetb(b);
    return v;
}

static int jhuff_decode(JBits *b, const JHuff *h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | jgetb(b);
        if (b->err) return -1;
        int first = 0;
        for (int l = 1; l < len; l++) first = (first + h->count[l]) << 1;
        if (code - first < h->count[len]) {
            int idx = 0;
            for (int l = 1; l < len; l++) idx += h->count[l];
            return h->symbol[idx + (code - first)];
        }
    }
    return -1;
}

static int jextend(int v, int s) {
    if (s == 0) return 0;
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
}

/* 8x8 整数 IDCT（Q14 余弦表，两次一维变换）。 */
static void jidct(const int32_t *in, uint8_t *out, int stride) {
    int32_t tmp[64];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++) {
                int32_t c = in[y * 8 + u];
                if (u == 0) c = (c * 11585) >> 14;
                sum += (int64_t)c * jcos[u][x];
            }
            tmp[y * 8 + x] = (int32_t)(sum >> 15);
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int v = 0; v < 8; v++) {
                int32_t c = tmp[v * 8 + x];
                if (v == 0) c = (c * 11585) >> 14;
                sum += (int64_t)c * jcos[v][y];
            }
            int32_t val = (int32_t)(sum >> 15);
            out[y * stride + x] = (uint8_t)clamp255((int)val + 128);
        }
    }
}

static int jparse_dqt(Jpeg *j, const uint8_t *p, int len) {
    int pos = 0;
    while (pos < len) {
        int pq = p[pos] >> 4;
        int tq = p[pos] & 15;
        pos++;
        if (tq > 3) return 0;
        if (pq == 0) {
            if (pos + 64 > len) return 0;
            for (int i = 0; i < 64; i++) j->q[tq][zz[i]] = p[pos + i];
            pos += 64;
        } else {
            if (pos + 128 > len) return 0;
            for (int i = 0; i < 64; i++) {
                j->q[tq][zz[i]] = be16(p + pos + i * 2);
            }
            pos += 128;
        }
        j->q_set[tq] = 1;
    }
    return 1;
}

static int jparse_dht(Jpeg *j, const uint8_t *p, int len) {
    int pos = 0;
    while (pos < len) {
        int tc = p[pos] >> 4;
        int th = p[pos] & 15;
        pos++;
        if (tc > 1 || th > 3) return 0;
        if (pos + 16 > len) return 0;
        int total = 0;
        for (int i = 1; i <= 16; i++) total += p[pos + i - 1];
        if (total > 256 || pos + 16 + total > len) return 0;

        JHuff *h = tc ? &j->ac[th] : &j->dc[th];
        h->count[0] = 0;
        for (int i = 1; i <= 16; i++) h->count[i] = p[pos + i - 1];
        pos += 16;
        for (int i = 0; i < total; i++) h->symbol[i] = p[pos + i];
        pos += total;
        if (tc) j->ac_set[th] = 1;
        else j->dc_set[th] = 1;
    }
    return 1;
}

/* 解码一个 8x8 块；scale=8 时只输出 DC 到平面。 */
static int jdecode_block(Jpeg *j, JBits *b, JComp *c, int bx, int by) {
    int32_t coef[64];
    for (int i = 0; i < 64; i++) coef[i] = 0;

    int t = jhuff_decode(b, &j->dc[c->td]);
    if (t < 0 || t > 15) return 0;
    int diff = jextend(jgetbits(b, t), t);
    c->dc_pred += diff;
    if (j->scale == 8) {
        int dc = c->dc_pred;
        int v = clamp255((dc * j->q[c->tq][0] + 4) / 8 + 128);
        if (bx < c->bw && by < c->bh) {
            c->plane[by * c->bw + bx] = (uint8_t)v;
        }
        /* DC 之后仍需吃掉 AC 码，保持比特流同步。 */
        int k = 1;
        while (k < 64) {
            int rs = jhuff_decode(b, &j->ac[c->ta]);
            if (rs < 0) return 0;
            int r = rs >> 4;
            int s = rs & 15;
            if (s == 0) {
                if (r == 15) { k += 16; continue; }
                break;
            }
            k += r;
            if (k >= 64) break;
            jgetbits(b, s);
            k++;
        }
        return !b->err;
    }

    coef[0] = c->dc_pred * j->q[c->tq][0];
    int k = 1;
    while (k < 64) {
        int rs = jhuff_decode(b, &j->ac[c->ta]);
        if (rs < 0) return 0;
        int r = rs >> 4;
        int s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k >= 64) break;
        coef[zz[k]] = jextend(jgetbits(b, s), s) * j->q[c->tq][zz[k]];
        k++;
    }
    if (b->err) return 0;

    uint8_t block[64];
    jidct(coef, block, 8);

    for (int y = 0; y < 8; y++) {
        int py = by * 8 + y;
        if (py >= c->ch) break;
        for (int x = 0; x < 8; x++) {
            int px = bx * 8 + x;
            if (px >= c->cw) break;
            c->plane[py * c->cw + px] = block[y * 8 + x];
        }
    }
    return 1;
}

int img_jpeg_decode(const uint8_t *data, int size, ImgBitmap *out) {
    if (!data || size < 4 || !out) return 0;
    if (data[0] != 0xFF || data[1] != 0xD8) return 0;

    Jpeg j;
    for (uint32_t i = 0; i < sizeof(j); i++) ((uint8_t *)&j)[i] = 0;
    j.data = data;
    j.len = size;
    j.pos = 2;

    int sos_pos = -1;
    int sos_len = 0;

    while (j.pos + 4 <= size) {
        if (data[j.pos] != 0xFF) { j.pos++; continue; }
        uint8_t m = data[j.pos + 1];
        j.pos += 2;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;
        if (j.pos + 2 > size) return 0;
        int seglen = be16(data + j.pos);
        if (seglen < 2 || j.pos + seglen > size) return 0;
        const uint8_t *seg = data + j.pos + 2;
        int body = seglen - 2;

        if (m == 0xDB) {
            if (!jparse_dqt(&j, seg, body)) return 0;
        } else if (m == 0xC4) {
            if (!jparse_dht(&j, seg, body)) return 0;
        } else if (m == 0xC0) {
            if (body < 6) return 0;
            j.height = be16(seg + 1);
            j.width = be16(seg + 3);
            j.ncomp = seg[5];
            if (j.width <= 0 || j.height <= 0 || j.ncomp < 1 || j.ncomp > 3) return 0;
            if (body < 6 + j.ncomp * 3) return 0;
            j.hmax = 1;
            j.vmax = 1;
            for (int i = 0; i < j.ncomp; i++) {
                JComp *c = &j.comp[i];
                c->id = seg[6 + i * 3];
                c->h = seg[7 + i * 3] >> 4;
                c->v = seg[7 + i * 3] & 15;
                c->tq = seg[8 + i * 3];
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return 0;
                if (c->h > j.hmax) j.hmax = c->h;
                if (c->v > j.vmax) j.vmax = c->v;
            }
        } else if (m == 0xC2) {
            return 0;   /* 渐进式不支持 */
        } else if (m == 0xDD) {
            if (body >= 2) j.restart_interval = be16(seg);
        } else if (m == 0xDA) {
            if (body < 1) return 0;
            int ns = seg[0];
            if (body < 1 + ns * 2 + 3) return 0;
            for (int i = 0; i < ns; i++) {
                int cid = seg[1 + i * 2];
                int t = seg[2 + i * 2];
                for (int k = 0; k < j.ncomp; k++) {
                    if (j.comp[k].id == cid) {
                        j.comp[k].td = t >> 4;
                        j.comp[k].ta = t & 15;
                    }
                }
            }
            sos_pos = j.pos + seglen;
            sos_len = seglen;
            break;
        }

        j.pos += seglen;
    }

    if (sos_pos < 0 || j.width <= 0 || j.ncomp < 1) return 0;
    if (!j.q_set[0]) return 0;
    for (int i = 0; i < j.ncomp; i++) {
        if (!j.dc_set[j.comp[i].td] || !j.ac_set[j.comp[i].ta]) return 0;
    }

    j.scale = ((int64_t)j.width * j.height > 4000000) ? 8 : 1;
    j.mcu_cols = (j.width + 8 * j.hmax - 1) / (8 * j.hmax);
    j.mcu_rows = (j.height + 8 * j.vmax - 1) / (8 * j.vmax);

    int ow = (j.width + j.scale - 1) / j.scale;
    int oh = (j.height + j.scale - 1) / j.scale;
    if ((int64_t)ow * oh * 4 > IMG_MAX_BYTES) return 0;

    for (int i = 0; i < j.ncomp; i++) {
        JComp *c = &j.comp[i];
        c->cw = (j.width * c->h + j.hmax - 1) / j.hmax;
        c->ch = (j.height * c->v + j.vmax - 1) / j.vmax;
        c->bw = (c->cw + j.scale - 1) / j.scale;
        c->bh = (c->ch + j.scale - 1) / j.scale;
        c->plane = (uint8_t *)img_alloc((uint32_t)c->bw * (uint32_t)c->bh);
        c->dc_pred = 0;
        if (!c->plane) {
            for (int k = 0; k < i; k++) img_free(j.comp[k].plane);
            return 0;
        }
    }

    JBits bits;
    bits.data = data;
    bits.len = size;
    bits.pos = sos_pos;
    bits.buf = 0;
    bits.bits = 0;
    bits.err = 0;

    int mcu = 0;
    for (int my = 0; my < j.mcu_rows; my++) {
        for (int mx = 0; mx < j.mcu_cols; mx++) {
            if (j.restart_interval > 0 && mcu > 0 &&
                (mcu % j.restart_interval) == 0) {
                bits.bits = 0;
                while (bits.pos + 1 < size && data[bits.pos] == 0xFF &&
                       data[bits.pos + 1] == 0xFF) bits.pos++;
                if (bits.pos + 1 >= size || data[bits.pos] != 0xFF ||
                    data[bits.pos + 1] < 0xD0 || data[bits.pos + 1] > 0xD7) {
                    bits.err = 1;
                } else {
                    bits.pos += 2;
                }
                for (int i = 0; i < j.ncomp; i++) j.comp[i].dc_pred = 0;
            }

            for (int ci = 0; ci < j.ncomp; ci++) {
                JComp *c = &j.comp[ci];
                for (int by = 0; by < c->v; by++) {
                    for (int bx = 0; bx < c->h; bx++) {
                        int gx = mx * c->h + bx;
                        int gy = my * c->v + by;
                        if (!jdecode_block(&j, &bits, c, gx, gy)) {
                            bits.err = 1;
                            break;
                        }
                    }
                    if (bits.err) break;
                }
                if (bits.err) break;
            }
            if (bits.err) break;
            mcu++;
        }
        if (bits.err) break;
    }

    int ok = !bits.err;
    if (!ok) {
        for (int i = 0; i < j.ncomp; i++) img_free(j.comp[i].plane);
        return 0;
    }

    uint32_t *px = (uint32_t *)img_alloc((uint32_t)ow * (uint32_t)oh * 4);
    if (!px) {
        for (int i = 0; i < j.ncomp; i++) img_free(j.comp[i].plane);
        return 0;
    }

    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int Y = 0, Cb = 128, Cr = 128;
            if (j.ncomp == 1) {
                JComp *c = &j.comp[0];
                int cx = x * c->bw / ow;
                int cy = y * c->bh / oh;
                if (cx >= c->bw) cx = c->bw - 1;
                if (cy >= c->bh) cy = c->bh - 1;
                Y = c->plane[cy * c->bw + cx];
            } else {
                JComp *cy = &j.comp[0];
                JComp *cb = &j.comp[1];
                JComp *cr = &j.comp[2];

                int yx = x * cy->bw / ow;
                int yy = y * cy->bh / oh;
                if (yx >= cy->bw) yx = cy->bw - 1;
                if (yy >= cy->bh) yy = cy->bh - 1;
                Y = cy->plane[yy * cy->bw + yx];

                int cbx = x * cb->bw / ow;
                int cby = y * cb->bh / oh;
                if (cbx >= cb->bw) cbx = cb->bw - 1;
                if (cby >= cb->bh) cby = cb->bh - 1;
                Cb = cb->plane[cby * cb->bw + cbx];

                int crx = x * cr->bw / ow;
                int cry = y * cr->bh / oh;
                if (crx >= cr->bw) crx = cr->bw - 1;
                if (cry >= cr->bh) cry = cr->bh - 1;
                Cr = cr->plane[cry * cr->bw + crx];
            }

            int cb = Cb - 128;
            int cr = Cr - 128;
            int r = Y + ((91881 * cr) >> 16);
            int g = Y - ((22554 * cb + 46802 * cr) >> 16);
            int b = Y + ((116130 * cb) >> 16);
            px[(int64_t)y * ow + x] = 0xFF000000u |
                ((uint32_t)clamp255(r) << 16) |
                ((uint32_t)clamp255(g) << 8) |
                (uint32_t)clamp255(b);
        }
    }

    for (int i = 0; i < j.ncomp; i++) img_free(j.comp[i].plane);
    out->px = px;
    out->w = ow;
    out->h = oh;
    return 1;
}
