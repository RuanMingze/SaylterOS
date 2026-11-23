/* ============================================================
 * image.c - 图片解码（BMP / PNG / JPEG / SVG）与绘制
 * ------------------------------------------------------------
 * BMP 走 SlyzFS 顺序读：头部、调色板、位图数据在文件里本来就是
 * 顺序排列的，一次打开 + 顺序读即可，不需要 seek。
 *
 * PNG / JPEG / SVG 需要整块缓冲才能解码（zlib 回溯、DCT 行缓存、
 * 路径扫描），因此整文件读入内存后交给 imgcodec 的解码器。
 * 解码器实现分别在 png_dec.c / jpeg_dec.c / svg_dec.c / inflate.c。
 *
 * 内存：
 *   图片像素动辄几 MB，远超 6MB 内核堆，因此统一走 img_alloc：
 *   优先向 vm 的物理帧池申请连续页（恒等映射，可直接当指针），
 *   帧池不可用时退回内核堆。每块缓冲前面带 16 字节头，用于记录
 *   长度与来源，img_free 据此决定归还帧池还是堆。
 *
 * 缓存策略：
 *   桌面每一帧都会请求同一张壁纸，如果每次都解码，整帧预算会被
 *   吃光。因此按路径缓存解码结果，命中时只做一次线性比较。总预算
 *   24MB，超出时淘汰最久未使用的图片。
 *
 * 像素格式：
 *   统一为 0xAARRGGBB。BMP 的 32 位图经常把 alpha 全写成 0，
 *   这种文件按不透明处理；PNG/SVG 的 alpha 正常保留。
 * ============================================================ */

#include <stdint.h>

#include "image.h"
#include "imgcodec.h"
#include "vbe.h"
#include "mem.h"
#include "vm.h"
#include "net.h"

/* 串口诊断（仅首次加载时输出） */
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

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

#define IMAGE_CACHE_MAX     8
#define IMAGE_CACHE_BUDGET  (24u * 1024u * 1024u)
#define IMAGE_PATH_MAX      160
#define IMAGE_MAX_DIM       8192
#define IMAGE_FILE_MAX      IMG_MAX_BYTES

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
 * 图片缓冲分配器（imgcodec.h 的实现）
 * ------------------------------------------------------------
 * 帧池优先：像素缓冲通常几 MB，内核堆只有 6MB，直接 kmalloc
 * 会把堆打满导致后续 ACPICA/GUI 分配失败。帧池是恒等映射的
 * 物理内存，返回地址可直接解引用。
 *
 * 每块缓冲前有 16 字节头，记录长度与来源。img_free 只看头部，
 * 不依赖调用方重复传长度。
 * ============================================================ */

struct ImgHdr {
    uint32_t magic;
    uint32_t bytes;
    uint32_t raw;
    uint32_t from_pool;
};

#define IMG_HDR_MAGIC 0x494D4753u   /* "IMGS" */
#define IMG_HDR_SIZE  16u

void *img_alloc(uint32_t bytes) {
    if (bytes == 0 || bytes > 0xFFFFFFF0u - IMG_HDR_SIZE) return 0;

    uint32_t raw = bytes + IMG_HDR_SIZE;
    uint32_t phys = vm_alloc_phys_bytes(raw);
    if (phys) {
        struct ImgHdr *h = (struct ImgHdr *)(uintptr_t)phys;
        h->magic = IMG_HDR_MAGIC;
        h->bytes = bytes;
        h->raw = raw;
        h->from_pool = 1;
        return (void *)(uintptr_t)(phys + IMG_HDR_SIZE);
    }

    struct ImgHdr *h = (struct ImgHdr *)kmalloc(raw);
    if (!h) return 0;
    h->magic = IMG_HDR_MAGIC;
    h->bytes = bytes;
    h->raw = raw;
    h->from_pool = 0;
    return (void *)((uint8_t *)h + IMG_HDR_SIZE);
}

void img_free(void *ptr) {
    if (!ptr) return;
    struct ImgHdr *h = (struct ImgHdr *)((uint8_t *)ptr - IMG_HDR_SIZE);
    if (h->magic != IMG_HDR_MAGIC) return;

    h->magic = 0;
    if (h->from_pool) {
        vm_free_phys_bytes((uint32_t)(uintptr_t)h, h->raw);
    } else {
        kfree(h);
    }
}

/* ============================================================
 * BMP 解码
 * ============================================================ */

static void img_free_pixels(struct ImageEntry *e) {
    if (e->px) {
        img_free(e->px);
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
    uint32_t *px = (uint32_t *)img_alloc(need);
    if (!px) {
        slyzfs_close(fh);
        return 0;
    }

    /* 一次性读入全部像素数据，而不是逐行/逐块调用 SlyzFS。
     * 每次 slyzfs_read 都会重新读文件的 MFT 记录：分块读取会让
     * "MFT 区"和"像素数据区"交替访问，预读缓存反复失效，读取量
     * 放大数倍。整块读只有一次模块调用，顺序 I/O 一次完成。 */
    uint32_t data_bytes = row_bytes * (uint32_t)h;
    if ((uint64_t)data_off + data_bytes > (uint64_t)ent.size) {
        img_free(px);
        slyzfs_close(fh);
        return 0;
    }

    uint8_t *raw = (uint8_t *)img_alloc(data_bytes);
    if (!raw) {
        img_free(px);
        slyzfs_close(fh);
        return 0;
    }

    int alpha_seen = 0;
    int ok = 1;
    uint32_t read_us = 0;
    uint32_t conv_us = 0;

    uint32_t rt0 = net_now_us();
    if (img_read_full(fh, raw, (int)data_bytes) != (int)data_bytes) {
        ok = 0;
    }
    uint32_t rt1 = net_now_us();
    read_us = rt1 - rt0;

    for (int r = 0; r < h && ok; r++) {
        const uint8_t *row = raw + (uint32_t)r * row_bytes;
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
    conv_us = net_now_us() - rt1;

    img_free(raw);
    slyzfs_close(fh);

    kserial_puts("[IMG] bmp read=");
    kserial_dec32(read_us);
    kserial_puts("us convert=");
    kserial_dec32(conv_us);
    kserial_puts("us rows=");
    kserial_dec32((uint32_t)h);
    {
        extern void slyzfs_cache_stats(uint32_t *hits, uint32_t *misses);
        uint32_t hits = 0;
        uint32_t misses = 0;
        slyzfs_cache_stats(&hits, &misses);
        kserial_puts(" rc_hit=");
        kserial_dec32(hits);
        kserial_puts(" rc_miss=");
        kserial_dec32(misses);
    }
    kserial_puts("\r\n");

    if (!ok) {
        img_free(px);
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

/* ============================================================
 * 整文件解码（PNG / JPEG / SVG）
 * ------------------------------------------------------------
 * 这三种格式都必须先拿到完整字节流。文件最大 24MB，走帧池而不是
 * 内核堆；读入后按魔数分派，解码器输出统一的 0xAARRGGBB。
 * ============================================================ */

#define IMG_KIND_BMP   1
#define IMG_KIND_PNG   2
#define IMG_KIND_JPEG  3
#define IMG_KIND_SVG   4

static int image_kind(const uint8_t *p, int n) {
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) {
        return IMG_KIND_PNG;
    }
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
        return IMG_KIND_JPEG;
    }

    /* SVG：允许 BOM / XML 声明 / 注释，在前 1KB 内找 <svg */
    int limit = n < 1024 ? n : 1024;
    for (int i = 0; i + 3 < limit; i++) {
        if (p[i] == '<' && p[i + 1] == 's' && p[i + 2] == 'v' && p[i + 3] == 'g') {
            return IMG_KIND_SVG;
        }
    }

    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return IMG_KIND_BMP;
    return 0;
}

static int image_decode_memory(const char *path, int kind, struct ImageEntry *out) {
    struct ImageDirEntry ent;
    if (!slyzfs_stat(path, &ent)) return 0;
    if (ent.type == 0x02) return 0;
    if (ent.size < 8 || ent.size > IMAGE_FILE_MAX) return 0;

    int size = (int)ent.size;
    uint8_t *data = (uint8_t *)img_alloc((uint32_t)size);
    if (!data) return 0;

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        img_free(data);
        return 0;
    }
    int got = img_read_full(fh, data, size);
    slyzfs_close(fh);
    if (got != size) {
        img_free(data);
        return 0;
    }

    /* 魔数可能与扩展名不一致：以实际字节为准重新判定。 */
    int real = image_kind(data, size);
    if (real != kind) kind = real;

    ImgBitmap bmp;
    bmp.px = 0;
    bmp.w = 0;
    bmp.h = 0;

    int ok = 0;
    if (kind == IMG_KIND_PNG) {
        ok = img_png_decode(data, size, &bmp);
    } else if (kind == IMG_KIND_JPEG) {
        ok = img_jpeg_decode(data, size, &bmp);
    } else if (kind == IMG_KIND_SVG) {
        /* 头像/图标类 SVG 用白色 currentColor 渲染；
         * 512 足够桌面壁纸缩放，又不会让掩码缓冲过大。 */
        ok = img_svg_decode(data, size, 512, 0xFFFFFFFFu, &bmp);
    }

    img_free(data);
    if (!ok || !bmp.px) return 0;

    if (bmp.w <= 0 || bmp.h <= 0 || bmp.w > IMAGE_MAX_DIM || bmp.h > IMAGE_MAX_DIM) {
        img_free(bmp.px);
        return 0;
    }

    uint32_t bytes = (uint32_t)bmp.w * (uint32_t)bmp.h * 4u;
    if (bytes == 0 || bytes > IMAGE_CACHE_BUDGET) {
        img_free(bmp.px);
        return 0;
    }

    out->w = bmp.w;
    out->h = bmp.h;
    out->px = bmp.px;
    out->bytes = bytes;
    return 1;
}

int image_load(const char *path) {
    if (!path || !path[0]) return IMAGE_NONE;

    int found = image_slot_for_path(path);
    if (found != IMAGE_NONE) {
        g_images[found].stamp = g_image_stamp++;
        return found;
    }

    /* 串口诊断：首次加载每张图时分段输出探测/解码耗时，
     * 用来区分"磁盘 I/O 慢"和"解码慢"。 */
    extern void kserial_puts(const char *s);
    extern void kserial_dec32(uint32_t v);
    uint32_t t_probe0 = net_now_us();

    struct ImageEntry tmp;
    tmp.used = 0;
    tmp.path[0] = '\0';
    tmp.w = 0;
    tmp.h = 0;
    tmp.px = 0;
    tmp.bytes = 0;
    tmp.stamp = 0;

    /* 先读文件头判定格式：BMP 走顺序读，其余整文件解码。 */
    static uint8_t probe[1024];
    int probe_len = 0;
    void *pfh = slyzfs_open(path, "r");
    if (!pfh) return IMAGE_NONE;
    probe_len = img_read_full(pfh, probe, (int)sizeof(probe));
    slyzfs_close(pfh);
    if (probe_len < 8) return IMAGE_NONE;

    int kind = image_kind(probe, probe_len);
    if (kind == 0) return IMAGE_NONE;

    uint32_t t_dec0 = net_now_us();
    if (kind == IMG_KIND_BMP) {
        if (!bmp_decode(path, &tmp)) return IMAGE_NONE;
    } else {
        if (!image_decode_memory(path, kind, &tmp)) return IMAGE_NONE;
    }
    uint32_t t_dec1 = net_now_us();

    kserial_puts("[IMG] ");
    kserial_puts(path);
    kserial_puts(" probe=");
    kserial_dec32(t_dec0 - t_probe0);
    kserial_puts("us decode=");
    kserial_dec32(t_dec1 - t_dec0);
    kserial_puts("us bytes=");
    kserial_dec32(tmp.bytes);
    kserial_puts("\r\n");

    /* 缓存预算不够就按最久未使用顺序淘汰，直到放得下 */
    while (g_image_total + tmp.bytes > IMAGE_CACHE_BUDGET) {
        if (!image_evict_one(tmp.bytes)) {
            img_free(tmp.px);
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
            img_free(tmp.px);
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
        img_free(tmp.px);
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

int image_scale_to_buffer(int handle, uint32_t *dst, int dw, int dh, uint32_t bg) {
    if (handle < 0 || handle >= IMAGE_CACHE_MAX || !dst || dw <= 0 || dh <= 0) return 0;

    struct ImageEntry *e = &g_images[handle];
    if (!e->used || !e->px) return 0;

    int iw = e->w;
    int ih = e->h;
    if (iw <= 0 || ih <= 0) return 0;

    uint32_t br = (bg >> 16) & 0xFFu;
    uint32_t bgc = (bg >> 8) & 0xFFu;
    uint32_t bb = bg & 0xFFu;

    for (int dy = 0; dy < dh; dy++) {
        int sy = (int)(((int64_t)dy * ih) / dh);
        if (sy >= ih) sy = ih - 1;

        const uint32_t *src = e->px + (uint32_t)sy * (uint32_t)iw;
        uint32_t *line = dst + (uint32_t)dy * (uint32_t)dw;

        int sx = 0;
        int acc = 0;
        for (int dx = 0; dx < dw; dx++) {
            if (sx >= iw) sx = iw - 1;
            uint32_t s = src[sx];
            uint32_t a = s >> 24;

            if (a >= 255) {
                line[dx] = s & 0x00FFFFFFu;
            } else if (a == 0) {
                line[dx] = bg & 0x00FFFFFFu;
            } else {
                uint32_t ia = 255u - a;
                uint32_t r = (((s >> 16) & 0xFFu) * a + br * ia) / 255u;
                uint32_t g = (((s >> 8) & 0xFFu) * a + bgc * ia) / 255u;
                uint32_t b = ((s & 0xFFu) * a + bb * ia) / 255u;
                line[dx] = (r << 16) | (g << 8) | b;
            }

            acc += iw;
            while (acc >= dw) {
                acc -= dw;
                sx++;
            }
        }
    }
    return 1;
}

void *image_alloc_buffer(uint32_t bytes) {
    return img_alloc(bytes);
}

void image_free_buffer(void *ptr) {
    if (ptr) img_free(ptr);
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
