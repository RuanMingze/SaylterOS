/* ============================================================
 * vc_surface_vbe.c - vbe/SlyerType 渲染后端实现
 * ============================================================ */

#include "vc_surface_vbe.h"
#include "vbe.h"
#include "slyertype.h"

static void vbe_surface_fill(void *user, int x, int y, int w, int h, uint32_t color) {
    (void)user;
    if (w <= 0 || h <= 0) return;
    vbe_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static uint32_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static void vbe_surface_glyph(void *user, int x, int y, uint32_t cp, uint32_t color) {
    VcVbeConfig *cfg = (VcVbeConfig *)user;
    if (!cfg) return;

    uint32_t cw = cfg->cell_w;
    uint32_t ch = cfg->cell_h;
    if (slyertype_draw_cell_buf(x, y, (int)cw, (int)ch, cp, color)) return;

    char utf8[5];
    uint32_t n = utf8_encode(cp, utf8);
    utf8[n] = '\0';
    vbe_draw_utf8_transparent((uint16_t)x, (uint16_t)y, utf8, color);
}

VcSurface vc_surface_vbe(VcVbeConfig *cfg) {
    VcSurface s;
    s.width = vbe_current.width;
    s.height = vbe_current.height;
    s.user = cfg;
    s.fill_rect = vbe_surface_fill;
    s.draw_glyph = vbe_surface_glyph;

    /* SlyerType 的 CJK 单元格是 32x32，ASCII 按 16 宽排版；
     * 单元格必须至少 16x32，否则字形会重叠、越界。 */
    if (cfg && (cfg->cell_w == 0 || cfg->cell_h == 0)) {
        cfg->cell_w = 16;
        cfg->cell_h = 32;
    }
    return s;
}
