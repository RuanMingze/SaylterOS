/* ============================================================
 * png_dec.c - PNG 解码（含 Alpha）
 * ------------------------------------------------------------
 * 支持：
 *   - 位深 1/2/4/8/16（16 位取高字节）；
 *   - 颜色类型 0/2/3/4/6（灰度、RGB、调色板、灰度+A、RGBA）；
 *   - tRNS（调色板 Alpha / RGB 透明键 / 灰度透明键）；
 *   - 非隔行扫描；五种行过滤器。
 * 不支持 Adam7 隔行（返回失败）。
 * ============================================================ */

#include "imgcodec.h"

static int png_strcmp4(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t png_crc(const uint8_t *p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            if (c & 1u) c = (c >> 1) ^ 0xEDB88320u;
            else c >>= 1;
        }
    }
    return c ^ 0xFFFFFFFFu;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int img_png_decode(const uint8_t *data, int size, ImgBitmap *out) {
    if (!data || size < 8 || !out) return 0;
    static const uint8_t sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    for (int i = 0; i < 8; i++) if (data[i] != sig[i]) return 0;

    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t palette[256][4];
    for (int i = 0; i < 256; i++) {
        palette[i][0] = palette[i][1] = palette[i][2] = 0;
        palette[i][3] = 255;
    }
    int has_trns = 0;
    uint16_t trns_gray = 0;
    uint16_t trns_r = 0, trns_g = 0, trns_b = 0;

    int idat_total = 0;
    int pos = 8;
    while (pos + 8 <= size) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        if (len > (uint32_t)(size - pos - 12)) return 0;
        const uint8_t *body = data + pos + 8;
        uint32_t crc = be32(body + len);
        if (png_crc(type, (int)len + 4) != crc) return 0;

        if (png_strcmp4(type, "IHDR")) {
            if (len < 13) return 0;
            w = (int)be32(body);
            h = (int)be32(body + 4);
            depth = body[8];
            ctype = body[9];
            if (body[10] != 0 || body[11] != 0) return 0;
            interlace = body[12];
        } else if (png_strcmp4(type, "PLTE")) {
            int n = (int)len / 3;
            if (n > 256) n = 256;
            for (int i = 0; i < n; i++) {
                palette[i][0] = body[i * 3 + 0];
                palette[i][1] = body[i * 3 + 1];
                palette[i][2] = body[i * 3 + 2];
            }
        } else if (png_strcmp4(type, "tRNS")) {
            has_trns = 1;
            if (ctype == 3) {
                int n = (int)len;
                if (n > 256) n = 256;
                for (int i = 0; i < n; i++) palette[i][3] = body[i];
            } else if (ctype == 0 && len >= 2) {
                trns_gray = be16(body);
            } else if (ctype == 2 && len >= 6) {
                trns_r = be16(body);
                trns_g = be16(body + 2);
                trns_b = be16(body + 4);
            }
        } else if (png_strcmp4(type, "IDAT")) {
            idat_total += (int)len;
        } else if (png_strcmp4(type, "IEND")) {
            break;
        }
        pos += 12 + (int)len;
    }

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    if (interlace != 0) return 0;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return 0;

    int channels = 0;
    switch (ctype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return 0;
    }
    if (ctype == 3 && depth > 8) return 0;

    int bits_per_px = channels * depth;
    int rowbytes = (w * bits_per_px + 7) / 8;
    int64_t raw_size = (int64_t)h * (rowbytes + 1);
    int64_t px_size = (int64_t)w * h * 4;
    if (raw_size > IMG_MAX_BYTES || px_size > IMG_MAX_BYTES) return 0;
    if (idat_total <= 0) return 0;

    uint8_t *zdata = (uint8_t *)img_alloc((uint32_t)idat_total);
    uint8_t *raw = (uint8_t *)img_alloc((uint32_t)raw_size);
    uint32_t *px = (uint32_t *)img_alloc((uint32_t)px_size);
    if (!zdata || !raw || !px) {
        if (zdata) img_free(zdata);
        if (raw) img_free(raw);
        if (px) img_free(px);
        return 0;
    }

    int zpos = 0;
    pos = 8;
    while (pos + 8 <= size && zpos < idat_total) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        if (png_strcmp4(type, "IDAT")) {
            const uint8_t *body = data + pos + 8;
            for (uint32_t i = 0; i < len && zpos < idat_total; i++) {
                zdata[zpos++] = body[i];
            }
        }
        pos += 12 + (int)len;
    }

    int raw_len = 0;
    if (!img_inflate(zdata, zpos, raw, (int)raw_size, &raw_len)) {
        img_free(zdata);
        img_free(raw);
        img_free(px);
        return 0;
    }
    if (raw_len < (int)raw_size) {
        img_free(zdata);
        img_free(raw);
        img_free(px);
        return 0;
    }
    img_free(zdata);

    /* 还原五种行过滤器 */
    int bpp = (bits_per_px + 7) / 8;
    if (bpp < 1) bpp = 1;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (int64_t)y * (rowbytes + 1);
        int filter = row[0];
        uint8_t *cur = row + 1;
        uint8_t *prev = (y > 0) ? raw + (int64_t)(y - 1) * (rowbytes + 1) + 1 : 0;

        for (int x = 0; x < rowbytes; x++) {
            int a = (x >= bpp) ? cur[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            cur[x] = (uint8_t)v;
        }
    }

    /* 扫描线 -> ARGB */
    for (int y = 0; y < h; y++) {
        const uint8_t *row = raw + (int64_t)y * (rowbytes + 1) + 1;
        for (int x = 0; x < w; x++) {
            uint32_t r = 255, g = 255, b = 255, a = 255;

            if (depth == 8 || depth == 16) {
                int step = depth / 8;
                int base = x * channels * step;
                if (ctype == 0) {
                    int gray = row[base];
                    r = g = b = (uint32_t)gray;
                    if (has_trns && (uint16_t)gray == trns_gray) a = 0;
                } else if (ctype == 2) {
                    r = row[base];
                    g = row[base + step];
                    b = row[base + 2 * step];
                    if (has_trns && r == (trns_r >> 8) &&
                        g == (trns_g >> 8) && b == (trns_b >> 8)) a = 0;
                } else if (ctype == 3) {
                    int idx = row[base];
                    r = palette[idx][0];
                    g = palette[idx][1];
                    b = palette[idx][2];
                    a = palette[idx][3];
                } else if (ctype == 4) {
                    r = g = b = row[base];
                    a = row[base + step];
                } else {
                    r = row[base];
                    g = row[base + step];
                    b = row[base + 2 * step];
                    a = row[base + 3 * step];
                }
            } else {
                int bit = x * depth;
                int byte = bit >> 3;
                int shift = 8 - depth - (bit & 7);
                int val = (row[byte] >> shift) & ((1 << depth) - 1);
                if (ctype == 3) {
                    r = palette[val][0];
                    g = palette[val][1];
                    b = palette[val][2];
                    a = palette[val][3];
                } else {
                    int gray = val * 255 / ((1 << depth) - 1);
                    r = g = b = (uint32_t)gray;
                    if (has_trns && (uint16_t)val == trns_gray) a = 0;
                }
            }

            px[(int64_t)y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    img_free(raw);
    out->px = px;
    out->w = w;
    out->h = h;
    return 1;
}
