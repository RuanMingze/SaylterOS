/* ============================================================
 * image.c - BMP 解码与图片绘制
 * ------------------------------------------------------------
 * 解码器只依赖 SlyzFS 的顺序读接口：BMP 的头部、调色板、位图
 * 数据在文件里本来就是顺序排列的，用一次打开 + 顺序读即可完成，
 * 不需要 seek 支持。
 *
 * 缓存策略：
 *   桌面每一帧都会请求同一张壁纸，如果每次都解码，800×450 的
 *   图片会吃掉整帧预算。因此按路径缓存解码结果，命中时只做一次
 *   线性比较。缓存总预算 3.5MB，超出时淘汰最久未使用的图片；
 *   单张图片超过预算直接拒绝，避免把内核堆打满。
 *
 * 像素格式：
 *   统一为 0xAARRGGBB。BMP 的 32 位图经常把 alpha 全写成 0，
 *   这种文件按不透明处理；真正带 alpha 的图仍然按 alpha 混合。
 * ============================================================ */

#include <stdint.h>

#include "image.h"
#include "vbe.h"
#include "mem.h"

/* ============================================================
 * SlyzFS 接口
 * ------------------------------------------------------------
 * 目录项结构必须与 slyzfs_bridge.cpp 的 SlyzDirEntry 逐字节一致。
 * 这里只用到 type 与 size，但结构体大小必须相同，否则读取会越界。
 * ============================================================ */

#pragma pack(push, 1)
struct ImageDirEntry {
    uint8_t  type;
    uint8_t  flags;
    uint16_t name_len;
    char     name[256];
    uint32_t size;
    uint32_t start_lba;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)

extern int   slyzfs_stat(const char *path, struct ImageDirEntry *out);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

/* ============================================================
 * 缓存
 * ============================================================ */

#define IMAGE_CACHE_MAX     6
#define IMAGE_CACHE_BUDGET  (3u * 1024u * 1024u + 512u * 1024u)
#define IMAGE_PATH_MAX      160
#define IMAGE_MAX_DIM       4096

struct ImageEntry {
    int       used;
    char      path[IMAGE_PATH_MAX];
    int       w;
    int       h;
    uint32_t *px;        /* 0xAARRGGBB */
    uint32_t  bytes;
    uint32_t  stamp;
};

static struct ImageEntry g_images[IMAGE_CACHE_MAX];
static uint32_t g_image_stamp = 1;
static uint32_t g_image_total = 0;
static uint32_t g_palette[256];

/* ============================================================
 * 小工具
 * ============================================================ */

static int img_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int img_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void img_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int rd32s(const uint8_t *p) {
    return (int)rd32(p);
}

/* 顺序读满 n 字节；SlyzFS 的 read 允许短读，必须循环。 */
static int img_read_full(void *fh, uint8_t *buf, int n) {
    int total = 0;
    while (total < n) {
        int got = slyzfs_read(fh, buf + total, n - total);
        if (got <= 0) break;
        total += got;
    }
    return total;
}

static int img_skip(void *fh, int n) {
    uint8_t scratch[64];
    while (n > 0) {
        int chunk = n > (int)sizeof(scratch) ? (int)sizeof(scratch) : n;
        int got = img_read_full(fh, scratch, chunk);
        if (got != chunk) return 0;
        n -= chunk;
    }
    return 1;
}

static int mask_shift(uint32_t mask) {
    int shift = 0;
    if (!mask) return 0;
    while ((mask & 1u) == 0 && shift < 32) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static int mask_bits(uint32_t mask) {
    int bits = 0;
    while (mask && bits < 32) {
        bits += (int)(mask & 1u);
        mask >>= 1;
    }
    return bits;
}

/* 按通道掩码把原始值展开为 8 位。5/6 位通道用整数比例放大，
 * 不做浮点运算，保持与裸机编译选项一致。 */
static uint32_t mask_value(uint32_t value, uint32_t mask) {
    if (!mask) return 0;

    int shift = mask_shift(mask);
    int bits = mask_bits(mask);
    if (bits <= 0) return 0;
    if (bits >= 32) return value;

    uint32_t raw = (value >> shift) & ((1u << bits) - 1u);
    if (bits >= 8) return raw >> (bits - 8);
    return (raw * 255u) / ((1u << bits) - 1u);
}

static int img_isqrt(int v) {
    if (v <= 0) return 0;
    int x = v;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

/* ============================================================
 * BMP 解码
 * ============================================================ */

static void img_free_pixels(struct ImageEntry *e) {
    if (e->px) {
        kfree(e->px);
        e->px = 0;
    }
    if (e->bytes <= g_image_total) {
        g_image_total -= e->bytes;
    } else {
        g_image_total = 0;
    }
    e->bytes = 0;
    e->w = 0;
    e->h = 0;
}

/* 解码成功时填好 out->w/h/px/bytes（out 的其他字段由调用方维护）。
 * 失败返回 0，且不留下任何分配。 */
static int bmp_decode(const char *path, struct ImageEntry *out) {
    struct ImageDirEntry ent;
    if (!slyzfs_stat(path, &ent)) return 0;
    if (ent.type == 0x02) return 0;
    if (ent.size < 54) return 0;

    void *fh = slyzfs_open(path, "r");
    if (!fh) return 0;

    uint8_t hdr[54];
    if (img_read_full(fh, hdr, 54) != 54) {
        slyzfs_close(fh);
        return 0;
    }

    if (hdr[0] != 'B' || hdr[1] != 'M') {
        slyzfs_close(fh);
        return 0;
    }

    uint32_t data_off = rd32(hdr + 10);
    uint32_t dib_size = rd32(hdr + 14);
    if (dib_size < 40) {
        /* OS/2 BITMAPCOREHEADER：布局不同，不支持 */
        slyzfs_close(fh);
        return 0;
    }

    int w = rd32s(hdr + 18);
    int raw_h = rd32s(hdr + 22);
    int planes = (int)rd16(hdr + 26);
    int bpp = (int)rd16(hdr + 28);
    uint32_t compression = rd32(hdr + 30);

    if (planes != 1 || w <= 0 || raw_h == 0) {
        slyzfs_close(fh);
        return 0;
    }
    if (w > IMAGE_MAX_DIM) {
        slyzfs_close(fh);
        return 0;
    }

    int top_down = raw_h < 0;
    int h = top_down ? -raw_h : raw_h;
    if (h <= 0 || h > IMAGE_MAX_DIM) {
        slyzfs_close(fh);
        return 0;
    }

    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        slyzfs_close(fh);
        return 0;
    }
    if (bpp == 8 && compression != 0) {
        slyzfs_close(fh);
        return 0;
    }
    if (compression != 0 && compression != 3) {
        /* 不支持 RLE4 / RLE8 / JPEG / PNG 压缩位图 */
        slyzfs_close(fh);
        return 0;
    }
    if (compression == 3 && bpp != 16 && bpp != 32) {
        slyzfs_close(fh);
        return 0;
    }

    uint32_t row_bytes = ((uint32_t)bpp * (uint32_t)w + 31u) / 32u * 4u;
    if (row_bytes == 0 || row_bytes > 1024u * 1024u) {
        slyzfs_close(fh);
        return 0;
    }

    uint32_t pixels = (uint32_t)w * (uint32_t)h;
    if (pixels == 0 || pixels > (IMAGE_CACHE_BUDGET / 4u)) {
        slyzfs_close(fh);
        return 0;
    }

    /* ---- 头部之后的扩展字段 / 掩码 ---- */
    uint8_t ext[16];
    int ext_len = 0;
    uint32_t consumed = 54u;

    if (dib_size > 40) {
        uint32_t extra = dib_size - 40u;
        if (extra > 1024u * 1024u) {
            slyzfs_close(fh);
            return 0;
        }
        uint32_t remaining = extra;
        while (remaining > 0) {
            uint8_t chunk[16];
            int n = remaining > (uint32_t)sizeof(chunk)
                        ? (int)sizeof(chunk) : (int)remaining;
            if (img_read_full(fh, chunk, n) != n) {
                slyzfs_close(fh);
                return 0;
            }
            for (int i = 0; i < n && ext_len < (int)sizeof(ext); i++) {
                ext[ext_len++] = chunk[i];
            }
            remaining -= (uint32_t)n;
        }
        consumed += extra;
    } else if (compression == 3) {
        /* 40 字节头 + BI_BITFIELDS：3 个颜色掩码紧跟在头部之后 */
        if (img_read_full(fh, ext, 12) != 12) {
            slyzfs_close(fh);
            return 0;
        }
        ext_len = 12;
        consumed += 12u;
    }

    uint32_t mask_r = 0;
    uint32_t mask_g = 0;
    uint32_t mask_b = 0;
    uint32_t mask_a = 0;

    if (compression == 3) {
        if (ext_len < 12) {
            slyzfs_close(fh);
            return 0;
        }
        mask_r = rd32(ext + 0);
        mask_g = rd32(ext + 4);
        mask_b = rd32(ext + 8);
        if (ext_len >= 16) mask_a = rd32(ext + 12);
    } else if (bpp == 16) {
        mask_r = 0x7C00u;
        mask_g = 0x03E0u;
        mask_b = 0x001Fu;
    }

    /* ---- 调色板 ---- */
    if (bpp == 8) {
        uint32_t palette_off = 14u + dib_size;
        if (consumed > palette_off) {
            slyzfs_close(fh);
            return 0;
        }
        if (!img_skip(fh, (int)(palette_off - consumed))) {
            slyzfs_close(fh);
            return 0;
        }
        consumed = palette_off;

        int count = 256;
        if (data_off > palette_off) {
            uint32_t avail = (data_off - palette_off) / 4u;
            if (avail < (uint32_t)count) count = (int)avail;
        } else {
            count = 0;
        }
        if (count <= 0) {
            slyzfs_close(fh);
            return 0;
        }

        for (int i = 0; i < count; i++) {
            uint8_t pal[4];
            if (img_read_full(fh, pal, 4) != 4) {
                slyzfs_close(fh);
                return 0;
            }
            g_palette[i] = 0xFF000000u |
                           ((uint32_t)pal[2] << 16) |
                           ((uint32_t)pal[1] << 8) |
                           (uint32_t)pal[0];
        }
        consumed += (uint32_t)count * 4u;
    }

    /* ---- 跳到像素数据 ---- */
    if (data_off > consumed) {
        if (!img_skip(fh, (int)(data_off - consumed))) {
            slyzfs_close(fh);
            return 0;
        }
    } else if (data_off < consumed) {
        slyzfs_close(fh);
        return 0;
    }

    uint32_t need = pixels * 4u;
    uint32_t *px = (uint32_t *)kmalloc(need);
    if (!px) {
        slyzfs_close(fh);
        return 0;
    }

    uint8_t *row = (uint8_t *)kmalloc(row_bytes);
    if (!row) {
        kfree(px);
        slyzfs_close(fh);
        return 0;
    }

    int alpha_seen = 0;
    int ok = 1;

    for (int r = 0; r < h && ok; r++) {
        if (img_read_full(fh, row, (int)row_bytes) != (int)row_bytes) {
            ok = 0;
            break;
        }

        int y = top_down ? r : (h - 1 - r);
        uint32_t *dst = px + (uint32_t)y * (uint32_t)w;

        for (int x = 0; x < w; x++) {
            uint32_t color = 0xFF000000u;

            if (bpp == 8) {
                color = g_palette[row[x]];
            } else if (bpp == 24) {
                const uint8_t *s = row + x * 3;
                color = 0xFF000000u |
                        ((uint32_t)s[2] << 16) |
                        ((uint32_t)s[1] << 8) |
                        (uint32_t)s[0];
            } else if (bpp == 16) {
                const uint8_t *s = row + x * 2;
                uint32_t v = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
                uint32_t rr = mask_value(v, mask_r);
                uint32_t gg = mask_value(v, mask_g);
                uint32_t bb = mask_value(v, mask_b);
                color = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            } else {
                const uint8_t *s = row + x * 4;
                if (compression == 3) {
                    uint32_t v = rd32(s);
                    uint32_t rr = mask_value(v, mask_r);
                    uint32_t gg = mask_value(v, mask_g);
                    uint32_t bb = mask_value(v, mask_b);
                    uint32_t aa = mask_a ? mask_value(v, mask_a) : 255u;
                    if (aa != 0) alpha_seen = 1;
                    color = (aa << 24) | (rr << 16) | (gg << 8) | bb;
                } else {
                    uint32_t aa = s[3];
                    if (aa != 0) alpha_seen = 1;
                    color = (aa << 24) |
                            ((uint32_t)s[2] << 16) |
                            ((uint32_t)s[1] << 8) |
                            (uint32_t)s[0];
                }
            }

            dst[x] = color;
        }
    }

    kfree(row);
    slyzfs_close(fh);

    if (!ok) {
        kfree(px);
        return 0;
    }

    /* 绝大多数 32 位 BMP 的 alpha 字节是全 0（BI_RGB 视为未使用），
     * 这种文件必须按不透明处理，否则图片会整张透明。 */
    if (bpp == 32 && !alpha_seen) {
        for (uint32_t i = 0; i < pixels; i++) px[i] |= 0xFF000000u;
    }

    out->w = w;
    out->h = h;
    out->px = px;
    out->bytes = need;
    return 1;
}

/* ============================================================
 * 缓存查找 / 淘汰
 * ============================================================ */

static int image_slot_for_path(const char *path) {
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (g_images[i].used && img_strcmp(g_images[i].path, path) == 0) {
            return i;
        }
    }
    return IMAGE_NONE;
}

static int image_evict_one(uint32_t need_bytes) {
    int oldest = -1;
    uint32_t oldest_stamp = 0;

    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (!g_images[i].used) continue;
        if (oldest < 0 || g_images[i].stamp < oldest_stamp) {
            oldest = i;
            oldest_stamp = g_images[i].stamp;
        }
    }

    if (oldest < 0) return 0;

    img_free_pixels(&g_images[oldest]);
    g_images[oldest].used = 0;
    g_images[oldest].path[0] = '\0';

    if (g_image_total > IMAGE_CACHE_BUDGET) {
        return image_evict_one(need_bytes);
    }
    return 1;
}

int image_load(const char *path) {
    if (!path || !path[0]) return IMAGE_NONE;

    int found = image_slot_for_path(path);
    if (found != IMAGE_NONE) {
        g_images[found].stamp = g_image_stamp++;
        return found;
    }

    struct ImageEntry tmp;
    tmp.used = 0;
    tmp.path[0] = '\0';
    tmp.w = 0;
    tmp.h = 0;
    tmp.px = 0;
    tmp.bytes = 0;
    tmp.stamp = 0;

    if (!bmp_decode(path, &tmp)) return IMAGE_NONE;

    /* 缓存预算不够就按最久未使用顺序淘汰，直到放得下 */
    while (g_image_total + tmp.bytes > IMAGE_CACHE_BUDGET) {
        if (!image_evict_one(tmp.bytes)) {
            kfree(tmp.px);
            return IMAGE_NONE;
        }
    }

    int slot = -1;
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (!g_images[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (!image_evict_one(tmp.bytes)) {
            kfree(tmp.px);
            return IMAGE_NONE;
        }
        for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
            if (!g_images[i].used) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        kfree(tmp.px);
        return IMAGE_NONE;
    }

    g_images[slot].used = 1;
    img_copy(g_images[slot].path, path, IMAGE_PATH_MAX);
    g_images[slot].w = tmp.w;
    g_images[slot].h = tmp.h;
    g_images[slot].px = tmp.px;
    g_images[slot].bytes = tmp.bytes;
    g_images[slot].stamp = g_image_stamp++;
    g_image_total += tmp.bytes;
    return slot;
}

int image_width(int handle) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX) return 0;
    if (!g_images[handle].used) return 0;
    return g_images[handle].w;
}

int image_height(int handle) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX) return 0;
    if (!g_images[handle].used) return 0;
    return g_images[handle].h;
}

void image_free(int handle) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX) return;
    if (!g_images[handle].used) return;

    img_free_pixels(&g_images[handle]);
    g_images[handle].used = 0;
    g_images[handle].path[0] = '\0';
    g_images[handle].stamp = 0;
}

void image_cache_clear(void) {
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (!g_images[i].used) continue;
        img_free_pixels(&g_images[i]);
        g_images[i].used = 0;
        g_images[i].path[0] = '\0';
        g_images[i].stamp = 0;
    }
    g_image_total = 0;
}

uint32_t image_cache_bytes(void) {
    return g_image_total;
}

/* ============================================================
 * 绘制
 * ============================================================ */

static void image_put_px(int x, int y, uint32_t argb) {
    uint32_t a = argb >> 24;

    if (a == 0) return;
    if (a == 255) {
        vbe_put_pixel((uint16_t)x, (uint16_t)y, argb & 0x00FFFFFFu);
        return;
    }

    uint32_t dst = vbe_get_pixel((uint16_t)x, (uint16_t)y);
    uint32_t ia = 255u - a;

    uint32_t r = (((argb >> 16) & 0xFFu) * a + ((dst >> 16) & 0xFFu) * ia) / 255u;
    uint32_t g = (((argb >> 8) & 0xFFu) * a + ((dst >> 8) & 0xFFu) * ia) / 255u;
    uint32_t b = ((argb & 0xFFu) * a + (dst & 0xFFu) * ia) / 255u;

    vbe_put_pixel((uint16_t)x, (uint16_t)y, (r << 16) | (g << 8) | b);
}

/* 取当前裁剪矩形；没有裁剪时就是整个屏幕。 */
static void image_clip(int *cx, int *cy, int *cw, int *ch) {
    vbe_get_clip(cx, cy, cw, ch);
}

void image_draw(int handle, int x, int y) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX) return;
    struct ImageEntry *e = &g_images[handle];
    if (!e->used || !e->px) return;

    int cx = 0, cy = 0, cw = 0, ch = 0;
    image_clip(&cx, &cy, &cw, &ch);

    int dx0 = 0;
    int dy0 = 0;
    int dx1 = e->w;
    int dy1 = e->h;

    if (x + dx1 <= cx || x >= cx + cw || y + dy1 <= cy || y >= cy + ch) return;

    if (x + dx0 < cx) dx0 = cx - x;
    if (y + dy0 < cy) dy0 = cy - y;
    if (x + dx1 > cx + cw) dx1 = cx + cw - x;
    if (y + dy1 > cy + ch) dy1 = cy + ch - y;

    for (int sy = dy0; sy < dy1; sy++) {
        const uint32_t *src = e->px + (uint32_t)sy * (uint32_t)e->w;
        int py = y + sy;
        for (int sx = dx0; sx < dx1; sx++) {
            image_put_px(x + sx, py, src[sx]);
        }
    }
}

void image_draw_scaled(int handle, int x, int y, int w, int h) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX) return;
    struct ImageEntry *e = &g_images[handle];
    if (!e->used || !e->px) return;
    if (w <= 0 || h <= 0) return;

    int cx = 0, cy = 0, cw = 0, ch = 0;
    image_clip(&cx, &cy, &cw, &ch);

    int dx0 = 0;
    int dy0 = 0;
    int dx1 = w;
    int dy1 = h;

    if (x >= cx + cw || x + w <= cx || y >= cy + ch || y + h <= cy) return;

    if (x + dx0 < cx) dx0 = cx - x;
    if (y + dy0 < cy) dy0 = cy - y;
    if (x + dx1 > cx + cw) dx1 = cx + cw - x;
    if (y + dy1 > cy + ch) dy1 = cy + ch - y;
    if (dx0 >= dx1 || dy0 >= dy1) return;

    int iw = e->w;
    int ih = e->h;

    for (int dy = dy0; dy < dy1; dy++) {
        int sy = (int)(((int64_t)dy * ih) / h);
        if (sy >= ih) sy = ih - 1;

        const uint32_t *src = e->px + (uint32_t)sy * (uint32_t)iw;
        int py = y + dy;

        int sx = (int)(((int64_t)dx0 * iw) / w);
        int acc = (int)(((int64_t)dx0 * iw) % w);

        for (int dx = dx0; dx < dx1; dx++) {
            if (sx >= iw) sx = iw - 1;
            image_put_px(x + dx, py, src[sx]);

            acc += iw;
            while (acc >= w) {
                acc -= w;
                sx++;
            }
        }
    }
}

void image_draw_circle(int handle, int cx, int cy, int radius) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX) return;
    struct ImageEntry *e = &g_images[handle];
    if (!e->used || !e->px) return;
    if (radius <= 0) return;
    if (radius > 1024) radius = 1024;

    int scx = 0, scy = 0, scw = 0, sch = 0;
    image_clip(&scx, &scy, &scw, &sch);

    /* 非正方形图片先居中裁成正方形，再映射到圆内，
     * 避免宽图被压扁、头像比例失真。 */
    int side = e->w < e->h ? e->w : e->h;
    if (side <= 0) return;

    int ox = (e->w - side) / 2;
    int oy = (e->h - side) / 2;
    int diameter = radius * 2;

    int r2 = radius * radius;

    for (int dy = -radius; dy < radius; dy++) {
        int py = cy + dy;
        if (py < scy || py >= scy + sch) continue;

        int span = img_isqrt(r2 - dy * dy);
        if (span <= 0) continue;

        int dx0 = -span;
        int dx1 = span + 1;
        if (cx + dx0 < scx) dx0 = scx - cx;
        if (cx + dx1 > scx + scw) dx1 = scx + scw - cx;
        if (dx0 >= dx1) continue;

        /* 目的坐标 [-r, r) -> 源图正方形 [0, side) */
        int mapped = dy + radius;
        int sy = oy + (int)(((int64_t)mapped * side) / diameter);
        if (sy >= e->h) sy = e->h - 1;

        const uint32_t *src = e->px + (uint32_t)sy * (uint32_t)e->w;

        int sx = ox + (int)(((int64_t)(dx0 + radius) * side) / diameter);
        int acc = (int)(((int64_t)(dx0 + radius) * side) % diameter);

        for (int dx = dx0; dx < dx1; dx++) {
            if (sx >= ox + side) sx = ox + side - 1;
            image_put_px(cx + dx, py, src[sx]);

            acc += side;
            while (acc >= diameter) {
                acc -= diameter;
                sx++;
            }
        }
    }
}
