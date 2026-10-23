/* ============================================================
 * vbe.c - minimal VBE graphics backend
 * ------------------------------------------------------------
 * 目的：
 *  1. 设定一个固定的 GUI 模式：1024x768x32
 *  2. 填充线性帧缓冲
 *  3. 作为后续真正 BIOS VBE 0x4F00 / 0x4F01 / 0x4F02 扩展的基础
 * ============================================================ */

#include <stdint.h>
#include "vbe.h"

static vbe_mode_info_t vbe_current = {
    .width = VBE_WIDTH,
    .height = VBE_HEIGHT,
    .bpp = VBE_BPP,
    .pitch = VBE_WIDTH * 4,
    .framebuffer = VBE_FB,
    .enabled = 0
};

void vbe_init_basic(void) {
    vbe_current.width = VBE_WIDTH;
    vbe_current.height = VBE_HEIGHT;
    vbe_current.bpp = VBE_BPP;
    vbe_current.pitch = VBE_WIDTH * 4;
    vbe_current.framebuffer = VBE_FB;
    vbe_current.enabled = 1;
}

uint8_t vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp) {
    if (width != VBE_WIDTH || height != VBE_HEIGHT || bpp != VBE_BPP) {
        return 0;
    }

    if (!vbe_bios_switch_mode()) {
        return 0;
    }

    vbe_current.width = width;
    vbe_current.height = height;
    vbe_current.bpp = bpp;
    vbe_current.pitch = width * (bpp / 8);
    vbe_current.framebuffer = VBE_FB;
    vbe_current.enabled = 1;
    return 1;
}

void vbe_fill_color(uint32_t color) {
    if (!vbe_current.enabled) {
        return;
    }

    volatile uint32_t *fb = (volatile uint32_t *)vbe_current.framebuffer;
    const uint32_t pitch = vbe_current.pitch / 4;

    for (uint32_t y = 0; y < vbe_current.height; y++) {
        for (uint32_t x = 0; x < vbe_current.width; x++) {
            fb[y * pitch + x] = color;
        }
    }
}

void vbe_fill_sky_blue(void) {
    vbe_fill_color(0x00BFE8FF);
}
