/* ============================================================
 * vbe.c - 双缓冲 framebuffer 操作
 * ------------------------------------------------------------
 * VBE BIOS 调用已在 bootloader (pm.asm, Real Mode) 完成
 * 所有绘制写 off-screen backbuffer，vbe_swap() 一次 blit 到 LFB
 * ============================================================ */

#include "vbe.h"
#include <stddef.h>

vbe_info_t vbe_current;

/* ---- backbuffer ---- */
/* 分辨率上限 1600x1200x32 = 7.68MB，从 10MB 物理地址开始预留 8MB。
 * 内存布局：内核镜像 1MB..3MB，栈顶 0x3FF000，内核堆 4MB..10MB，
 * backbuffer 10MB..18MB，内核 BSS 固定在 18MB（0x1200000）。
 * backbuffer 绝不能和堆/BSS 重叠——逐帧像素写入会把分配块或文件系统
 * 对象清零。 */
#define BACKBUFFER_PHYS       0x00A00000
#define BACKBUFFER_MAX_BYTES  (8u * 1024u * 1024u)
static uint32_t *back = NULL;
static uint32_t back_pitch_bytes = 0;   /* 每行 stride (字节) */

static inline uint32_t *back_ptr(uint16_t x, uint16_t y) {
    return (uint32_t *)((uint8_t *)back + (uint32_t)y * back_pitch_bytes + (uint32_t)x * 4);
}

/* ---- 全局裁剪矩形 ----
 * GUI 的局部重绘依赖它：渲染整棵场景树，但只有落在裁剪矩形内的像素
 * 会真正写入 backbuffer，然后只把该矩形 blit 到 LFB。
 * TTY 走 vbe_fb_* 直写路径，不经过这里，不受影响。 */
static int g_clip_active = 0;
static int g_clip_x0 = 0;
static int g_clip_y0 = 0;
static int g_clip_x1 = 0;
static int g_clip_y1 = 0;

void vbe_set_clip(int x, int y, int w, int h) {
    int x1 = x + w;
    int y1 = y + h;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > (int)vbe_current.width) x1 = (int)vbe_current.width;
    if (y1 > (int)vbe_current.height) y1 = (int)vbe_current.height;
    if (x1 < x) x1 = x;
    if (y1 < y) y1 = y;

    g_clip_x0 = x;
    g_clip_y0 = y;
    g_clip_x1 = x1;
    g_clip_y1 = y1;
    g_clip_active = 1;
}

void vbe_clear_clip(void) {
    g_clip_active = 0;
}

int vbe_clip_active(void) {
    return g_clip_active;
}

void vbe_get_clip(int *x, int *y, int *w, int *h) {
    if (x) *x = g_clip_active ? g_clip_x0 : 0;
    if (y) *y = g_clip_active ? g_clip_y0 : 0;
    if (w) *w = g_clip_active ? (g_clip_x1 - g_clip_x0) : (int)vbe_current.width;
    if (h) *h = g_clip_active ? (g_clip_y1 - g_clip_y0) : (int)vbe_current.height;
}

/* ---- 裁剪栈 ----
 * 浏览器这类嵌套内容需要在既有脏矩形裁剪里再裁一层。直接调
 * vbe_set_clip 会覆盖外层矩形，clear 之后外层裁剪就丢了，后续表面
 * 会画到脏矩形外面。push/pop 让内层裁剪与外层求交，退出时恢复。 */

#define VBE_CLIP_STACK 6

struct VbeClipFrame {
    int active;
    int x0;
    int y0;
    int x1;
    int y1;
};

static struct VbeClipFrame g_clip_stack[VBE_CLIP_STACK];
static int g_clip_sp = 0;

void vbe_push_clip(int x, int y, int w, int h) {
    if (g_clip_sp < VBE_CLIP_STACK) {
        struct VbeClipFrame *f = &g_clip_stack[g_clip_sp++];
        f->active = g_clip_active;
        f->x0 = g_clip_x0;
        f->y0 = g_clip_y0;
        f->x1 = g_clip_x1;
        f->y1 = g_clip_y1;
    }

    vbe_set_clip(x, y, w, h);
}

void vbe_pop_clip(void) {
    if (g_clip_sp > 0) {
        struct VbeClipFrame *f = &g_clip_stack[--g_clip_sp];
        g_clip_active = f->active;
        g_clip_x0 = f->x0;
        g_clip_y0 = f->y0;
        g_clip_x1 = f->x1;
        g_clip_y1 = f->y1;
    } else {
        g_clip_active = 0;
    }
}

void vbe_init(void) {
    vbe_info_t *src = VBE_INFO_PTR;
    vbe_current.magic = 0;
    back = NULL;
    back_pitch_bytes = 0;

    if (src->magic != VBE_MAGIC_OK) return;

    /* 只接受 32bpp 线性帧缓冲：绘制路径按 4 字节像素写，
     * 其它色深会让整屏颜色错位。 */
    if (src->bpp != 32) return;
    if (src->width == 0 || src->height == 0) return;
    if (src->framebuffer == 0) return;
    if (src->pitch < (uint16_t)(src->width * 4u)) return;

    uint32_t back_bytes = (uint32_t)src->pitch * src->height;
    if (back_bytes == 0 || back_bytes > BACKBUFFER_MAX_BYTES) return;

    vbe_current = *src;
    back = (uint32_t *)BACKBUFFER_PHYS;
    back_pitch_bytes = vbe_current.pitch;
    g_clip_active = 0;

    /* 清掉上一轮（bootloader/BIOS）残留在物理内存里的像素，
     * 否则首次 swap 会把随机内容闪到屏幕上。 */
    vbe_fill_color(0x00000000u);
}

uint8_t vbe_is_enabled(void) {
    return vbe_current.magic == VBE_MAGIC_OK && back != NULL;
}

/* ---- 核心：把 backbuffer 全量 blit 到 LFB (DWORD 拷贝) ---- */
void vbe_swap(void) {
    if (!vbe_is_enabled()) return;
    volatile uint32_t *fb32 = (volatile uint32_t *)(uintptr_t)vbe_current.framebuffer;
    uint32_t width = vbe_current.width;
    uint32_t height = vbe_current.height;
    uint32_t lfb_stride = vbe_current.pitch / 4;
    uint32_t back_stride = back_pitch_bytes / 4;
    for (uint32_t y = 0; y < height; y++) {
        volatile uint32_t *dst = fb32 + y * lfb_stride;
        const uint32_t *src = back + y * back_stride;
        for (uint32_t x = 0; x < width; x++) {
            dst[x] = src[x];
        }
    }
}

/* ---- 脏矩形：只 blit 一个矩形区域 ---- */
void vbe_swap_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!vbe_is_enabled()) return;
    if ((uint32_t)x + w > vbe_current.width) w = (uint16_t)(vbe_current.width - x);
    if ((uint32_t)y + h > vbe_current.height) h = (uint16_t)(vbe_current.height - y);
    if (w == 0 || h == 0) return;
    volatile uint32_t *fb32 = (volatile uint32_t *)(uintptr_t)vbe_current.framebuffer;
    uint32_t lfb_stride = vbe_current.pitch / 4;
    uint32_t back_stride = back_pitch_bytes / 4;
    for (uint16_t row = 0; row < h; row++) {
        volatile uint32_t *dst = fb32 + (uint32_t)(y + row) * lfb_stride + x;
        const uint32_t *src = back + (uint32_t)(y + row) * back_stride + x;
        for (uint16_t col = 0; col < w; col++) {
            dst[col] = src[col];
        }
    }
}

/* ---- 所有绘制都写 backbuffer ---- */

void vbe_fill_color(uint32_t color) {
    if (!vbe_is_enabled()) return;
    /* 按行填充：pitch 可能大于 width*4（硬件对齐），
     * 线性填会错位并越过 backbuffer 尾部。 */
    uint32_t stride = back_pitch_bytes / 4;
    for (uint32_t y = 0; y < vbe_current.height; y++) {
        uint32_t *row = back + y * stride;
        for (uint32_t x = 0; x < vbe_current.width; x++) {
            row[x] = color;
        }
    }
}

void vbe_fill_sky_blue(void) {
    vbe_fill_color(0x00BFE8FF);
}

/* 多核并行填充的行区间入口。
 * 与 vbe_fill_rect 的区别：不接收 y/h 而是 [y0,y1)，让调用方按行切片；
 * 仍遵守全局裁剪矩形，否则局部重绘时并行填充会把窗外的桌面涂掉。 */
void vbe_fill_rect_rows(uint16_t x, uint16_t w, uint16_t y0, uint16_t y1,
                        uint32_t color) {
    if (!vbe_is_enabled()) return;
    if (w == 0 || y1 <= y0) return;

    int x0 = (int)x;
    int x1 = (int)x + (int)w;
    int yy0 = (int)y0;
    int yy1 = (int)y1;

    if (g_clip_active) {
        if (x0 < g_clip_x0) x0 = g_clip_x0;
        if (x1 > g_clip_x1) x1 = g_clip_x1;
        if (yy0 < g_clip_y0) yy0 = g_clip_y0;
        if (yy1 > g_clip_y1) yy1 = g_clip_y1;
    }
    if (x1 > (int)vbe_current.width) x1 = (int)vbe_current.width;
    if (yy1 > (int)vbe_current.height) yy1 = (int)vbe_current.height;
    if (x0 >= x1 || yy0 >= yy1) return;

    uint32_t stride = back_pitch_bytes / 4;
    for (int yy = yy0; yy < yy1; yy++) {
        uint32_t *row = back + (uint32_t)yy * stride;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void vbe_put_pixel(uint16_t x, uint16_t y, uint32_t color) {
    if (!vbe_is_enabled()) return;
    if (x >= vbe_current.width || y >= vbe_current.height) return;
    if (g_clip_active) {
        if ((int)x < g_clip_x0 || (int)x >= g_clip_x1 ||
            (int)y < g_clip_y0 || (int)y >= g_clip_y1) {
            return;
        }
    }
    *back_ptr(x, y) = color;
}

uint32_t vbe_get_pixel(uint16_t x, uint16_t y) {
    if (!vbe_is_enabled()) return 0;
    if (x >= vbe_current.width || y >= vbe_current.height) return 0;
    return *back_ptr(x, y);
}

void vbe_blend_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint32_t color, uint8_t alpha) {
    if (!vbe_is_enabled() || alpha == 0) return;
    if (alpha == 255) {
        vbe_fill_rect(x, y, w, h, color);
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = (int)x + w;
    int y1 = (int)y + h;

    if (g_clip_active) {
        if (x0 < g_clip_x0) x0 = g_clip_x0;
        if (y0 < g_clip_y0) y0 = g_clip_y0;
        if (x1 > g_clip_x1) x1 = g_clip_x1;
        if (y1 > g_clip_y1) y1 = g_clip_y1;
    }
    if (x0 >= x1 || y0 >= y1 || x0 >= (int)vbe_current.width ||
        y0 >= (int)vbe_current.height) {
        return;
    }

    uint32_t a = alpha;
    uint32_t ia = 255u - a;
    uint32_t sr = ((color >> 16) & 0xFFu) * a / 255u;
    uint32_t sg = ((color >> 8) & 0xFFu) * a / 255u;
    uint32_t sb = (color & 0xFFu) * a / 255u;

    for (int row = y0; row < y1; row++) {
        uint32_t *line = back_ptr((uint16_t)x0, (uint16_t)row);
        for (int col = x0; col < x1; col++) {
            uint32_t d = line[col - x0];
            uint32_t dr = ((d >> 16) & 0xFFu) * ia / 255u;
            uint32_t dg = ((d >> 8) & 0xFFu) * ia / 255u;
            uint32_t db = (d & 0xFFu) * ia / 255u;
            line[col - x0] = 0xFF000000u | ((dr + sr) << 16) | ((dg + sg) << 8) | (db + sb);
        }
    }
}

void vbe_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    if (!vbe_is_enabled()) return;

    int x0 = x;
    int y0 = y;
    int x1 = (int)x + (int)w;
    int y1 = (int)y + (int)h;

    if (x1 > (int)vbe_current.width) x1 = (int)vbe_current.width;
    if (y1 > (int)vbe_current.height) y1 = (int)vbe_current.height;
    if (g_clip_active) {
        if (x0 < g_clip_x0) x0 = g_clip_x0;
        if (y0 < g_clip_y0) y0 = g_clip_y0;
        if (x1 > g_clip_x1) x1 = g_clip_x1;
        if (y1 > g_clip_y1) y1 = g_clip_y1;
    }
    if (x0 >= x1 || y0 >= y1 || x0 >= (int)vbe_current.width ||
        y0 >= (int)vbe_current.height) {
        return;
    }

    for (int row = y0; row < y1; row++) {
        uint32_t *r = back_ptr((uint16_t)x0, (uint16_t)row);
        for (int col = 0; col < x1 - x0; col++) {
            r[col] = color;
        }
    }
}

/* save/restore 保留 —— 纯 RAM 操作，还能用 */
void vbe_save_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t *buf) {
    if (!vbe_is_enabled() || !buf) return;
    uint32_t idx = 0;
    for (uint16_t row = 0; row < h; row++) {
        uint32_t *src = back_ptr(x, y + row);
        for (uint16_t col = 0; col < w; col++) {
            buf[idx++] = src[col];
        }
    }
}

void vbe_restore_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint32_t *buf) {
    if (!vbe_is_enabled() || !buf) return;
    uint32_t idx = 0;
    for (uint16_t row = 0; row < h; row++) {
        uint32_t *dst = back_ptr(x, y + row);
        for (uint16_t col = 0; col < w; col++) {
            dst[col] = buf[idx++];
        }
    }
}

/* ============================================================
 * 16x32 bitmap font
 * ============================================================ */
extern const uint8_t font16x32[256][32][2];
#define FONT16_W  16
#define FONT16_H  32

/* 行位图 → 像素查表。
 * 终端底色/前景色长时间不变，逐像素判断"这一位是前景还是背景"
 * 在 TCG/无 GPU 环境下是主要开销；把 256 种字节值展开成 8 个 dword
 * 后，每行字模只需要 2 次查表 + 16 次顺序写。
 * 表只在颜色变化时重建（2KB dword × 8 = 8KB）。 */
static uint32_t g_lut_fg = 0xFFFFFFFFu;
static uint32_t g_lut_bg = 0xFFFFFFFFu;
static uint32_t g_row_lut[256][8];

static void build_row_lut(uint32_t fg, uint32_t bg) {
    if (fg == g_lut_fg && bg == g_lut_bg) return;
    for (int b = 0; b < 256; b++) {
        uint32_t *row = g_row_lut[b];
        for (int i = 0; i < 8; i++) {
            row[i] = (b & (0x80 >> i)) ? fg : bg;
        }
    }
    g_lut_fg = fg;
    g_lut_bg = bg;
}

void vbe_draw_char(uint16_t cx, uint16_t cy, char c, uint32_t fg, uint32_t bg) {
    if (!vbe_is_enabled()) return;
    uint16_t px = (uint16_t)(cx * FONT16_W);
    uint16_t py = (uint16_t)(cy * FONT16_H);
    if ((uint32_t)px + FONT16_W > vbe_current.width) return;
    if ((uint32_t)py + FONT16_H > vbe_current.height) return;

    build_row_lut(fg, bg);

    const uint8_t (*glyph)[2] = font16x32[(uint8_t)c];
    uint32_t back_stride = back_pitch_bytes / 4;
    for (int row = 0; row < FONT16_H; row++) {
        uint32_t *line = back + (uint32_t)(py + row) * back_stride + px;
        const uint32_t *left = g_row_lut[glyph[row][0]];
        const uint32_t *right = g_row_lut[glyph[row][1]];
        for (int i = 0; i < 8; i++) line[i] = left[i];
        for (int i = 0; i < 8; i++) line[8 + i] = right[i];
    }
}

void vbe_draw_string(const char *s, uint16_t cx, uint16_t cy, uint32_t fg, uint32_t bg) {
    if (!vbe_is_enabled()) return;
    while (*s) {
        vbe_draw_char(cx, cy, *s++, fg, bg);
        cx++;
    }
}

/* 像素坐标文本：GUI 组件需要 16px 网格之外的位置。
 * 不走 g_row_lut：GUI 的前景/背景色组合多，缓存会频繁重建；
 * 直接逐位写 dword，文本量小，开销可以接受。 */
void vbe_draw_text_px(uint16_t x, uint16_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!vbe_is_enabled()) return;

    uint32_t stride = back_pitch_bytes / 4;
    uint16_t px = x;

    while (*s) {
        if ((uint32_t)px + FONT16_W > vbe_current.width) return;
        if ((uint32_t)y + FONT16_H > vbe_current.height) return;

        const uint8_t (*glyph)[2] = font16x32[(uint8_t)(*s)];
        for (int row = 0; row < FONT16_H; row++) {
            int py = (int)y + row;
            if (g_clip_active && (py < g_clip_y0 || py >= g_clip_y1)) continue;

            uint32_t *line = back + (uint32_t)py * stride + px;
            uint8_t left = glyph[row][0];
            uint8_t right = glyph[row][1];
            for (int i = 0; i < 16; i++) {
                int pxx = (int)px + i;
                if (g_clip_active && (pxx < g_clip_x0 || pxx >= g_clip_x1)) continue;
                uint8_t bit = (i < 8) ? (uint8_t)((left >> (7 - i)) & 1u)
                                      : (uint8_t)((right >> (15 - i)) & 1u);
                line[i] = bit ? fg : bg;
            }
        }

        px = (uint16_t)(px + FONT16_W);
        s++;
    }
}

/* ============================================================
 * 文本终端直写帧缓冲
 * ============================================================ */

static inline volatile uint32_t *fb_ptr(uint16_t x, uint16_t y) {
    return (volatile uint32_t *)((uint8_t *)(uintptr_t)vbe_current.framebuffer +
                                 (uint32_t)y * vbe_current.pitch +
                                 (uint32_t)x * 4u);
}

void vbe_fb_clear(uint32_t color) {
    if (!vbe_is_enabled()) return;
    uint32_t stride = vbe_current.pitch / 4;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)vbe_current.framebuffer;
    for (uint32_t y = 0; y < vbe_current.height; y++) {
        volatile uint32_t *row = fb + y * stride;
        for (uint32_t x = 0; x < vbe_current.width; x++) {
            row[x] = color;
        }
    }
}

void vbe_fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    if (!vbe_is_enabled()) return;
    if ((uint32_t)x + w > vbe_current.width) return;
    if ((uint32_t)y + h > vbe_current.height) return;
    for (uint16_t row = 0; row < h; row++) {
        volatile uint32_t *line = fb_ptr(x, (uint16_t)(y + row));
        for (uint16_t col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

void vbe_fb_draw_char(uint16_t cx, uint16_t cy, char c, uint32_t fg, uint32_t bg) {
    if (!vbe_is_enabled()) return;
    uint16_t px = (uint16_t)(cx * FONT16_W);
    uint16_t py = (uint16_t)(cy * FONT16_H);
    if ((uint32_t)px + FONT16_W > vbe_current.width) return;
    if ((uint32_t)py + FONT16_H > vbe_current.height) return;

    build_row_lut(fg, bg);

    const uint8_t (*glyph)[2] = font16x32[(uint8_t)c];
    for (int row = 0; row < FONT16_H; row++) {
        volatile uint32_t *line = fb_ptr(px, (uint16_t)(py + row));
        const uint32_t *left = g_row_lut[glyph[row][0]];
        const uint32_t *right = g_row_lut[glyph[row][1]];
        for (int i = 0; i < 8; i++) line[i] = left[i];
        for (int i = 0; i < 8; i++) line[8 + i] = right[i];
    }
}

void vbe_fb_scroll_up(uint16_t pixel_rows, uint32_t fill_color) {
    if (!vbe_is_enabled() || pixel_rows == 0) return;
    if (pixel_rows >= vbe_current.height) {
        vbe_fb_clear(fill_color);
        return;
    }
    uint32_t stride = vbe_current.pitch / 4;
    uint32_t width = vbe_current.width;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)vbe_current.framebuffer;

    for (uint32_t y = 0; y < (uint32_t)(vbe_current.height - pixel_rows); y++) {
        volatile uint32_t *dst = fb + y * stride;
        volatile uint32_t *src = fb + (y + pixel_rows) * stride;
        for (uint32_t x = 0; x < width; x++) {
            dst[x] = src[x];
        }
    }
    for (uint32_t y = (uint32_t)(vbe_current.height - pixel_rows);
         y < vbe_current.height; y++) {
        volatile uint32_t *dst = fb + y * stride;
        for (uint32_t x = 0; x < width; x++) {
            dst[x] = fill_color;
        }
    }
}
