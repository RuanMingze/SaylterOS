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
/* 1024*768*4 = 3MB, 放在 2MB 物理地址 (QEMU 256MB RAM 下绝对安全) */
#define BACKBUFFER_PHYS  0x00200000
static uint32_t *back = NULL;
static uint32_t back_pitch_bytes = 0;   /* 每行 stride (字节) */

static inline uint32_t *back_ptr(uint16_t x, uint16_t y) {
    return (uint32_t *)((uint8_t *)back + (uint32_t)y * back_pitch_bytes + (uint32_t)x * 4);
}

void vbe_init(void) {
    vbe_info_t *src = VBE_INFO_PTR;
    if (src->magic == VBE_MAGIC_OK) {
        vbe_current = *src;
        /* 初始化 backbuffer */
        back = (uint32_t *)BACKBUFFER_PHYS;
        back_pitch_bytes = vbe_current.pitch;
    } else {
        vbe_current.magic = 0;
        back = NULL;
    }
}

uint8_t vbe_is_enabled(void) {
    return vbe_current.magic == VBE_MAGIC_OK && back != NULL;
}

/* ---- 核心：把 backbuffer 全量 blit 到 LFB (DWORD 拷贝) ---- */
void vbe_swap(void) {
    if (!vbe_is_enabled()) return;
    volatile uint32_t *fb32 = (volatile uint32_t *)vbe_current.framebuffer;
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
    volatile uint32_t *fb32 = (volatile uint32_t *)vbe_current.framebuffer;
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
    /* 注意：rep stosl 需要 ES 寄存器指向正确段，GCC inline asm 里不一定安全
       换回标准 C 循环 —— RAM 写本来就快，瓶颈是 MMIO swap */
    uint32_t total = vbe_current.width * vbe_current.height;
    uint32_t *p = back;
    for (uint32_t i = 0; i < total; i++)
        p[i] = color;
}

void vbe_fill_sky_blue(void) {
    vbe_fill_color(0x00BFE8FF);
}

void vbe_put_pixel(uint16_t x, uint16_t y, uint32_t color) {
    if (!vbe_is_enabled()) return;
    if (x >= vbe_current.width || y >= vbe_current.height) return;
    *back_ptr(x, y) = color;
}

void vbe_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    if (!vbe_is_enabled()) return;
    if ((uint32_t)x + w > vbe_current.width) return;
    if ((uint32_t)y + h > vbe_current.height) return;
    for (uint16_t row = 0; row < h; row++) {
        uint32_t *r = back_ptr(x, y + row);
        for (uint16_t col = 0; col < w; col++) {
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

void vbe_draw_char(uint16_t cx, uint16_t cy, char c, uint32_t fg, uint32_t bg) {
    if (!vbe_is_enabled()) return;
    uint16_t px = (uint16_t)(cx * FONT16_W);
    uint16_t py = (uint16_t)(cy * FONT16_H);
    const uint8_t (*glyph)[2] = font16x32[(uint8_t)c];
    uint32_t back_stride = back_pitch_bytes / 4;
    for (int row = 0; row < FONT16_H; row++) {
        uint32_t *line = back + (uint32_t)(py + row) * back_stride + px;
        uint8_t b0 = glyph[row][0];
        uint8_t b1 = glyph[row][1];
        for (int col = 0; col < 16; col++) {
            uint8_t bit;
            if (col < 8) {
                bit = (b0 >> (7 - col)) & 1;
            } else {
                bit = (b1 >> (15 - col)) & 1;
            }
            line[col] = bit ? fg : bg;
        }
    }
}

void vbe_draw_string(const char *s, uint16_t cx, uint16_t cy, uint32_t fg, uint32_t bg) {
    if (!vbe_is_enabled()) return;
    while (*s) {
        vbe_draw_char(cx, cy, *s++, fg, bg);
        cx++;
    }
}
