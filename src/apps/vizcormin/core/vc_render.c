/* ============================================================
 * vc_render.c - VizCormin 渲染层实现
 * ============================================================ */

#include "vc_render.h"
#include "vc_alloc.h"
#include <string.h>

struct VcRenderer {
    VcSurface surface;
    VcTheme   theme;
    VcRenderStats stats;
};

VcRenderer *vc_render_create(const VcSurface *surface, const VcTheme *theme) {
    if (!surface || !theme || !surface->fill_rect || !surface->draw_glyph) return 0;
    if (theme->cell_w == 0 || theme->cell_h == 0) return 0;
    VcRenderer *r = (VcRenderer *)vc_zalloc(sizeof(VcRenderer));
    if (!r) return 0;
    r->surface = *surface;
    r->theme = *theme;
    return r;
}

void vc_render_destroy(VcRenderer *r) {
    vc_free(r);
}

void vc_render_set_surface(VcRenderer *r, const VcSurface *surface) {
    if (r && surface) r->surface = *surface;
}

static void r_fill(VcRenderer *r, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    r->surface.fill_rect(r->surface.user, x, y, w, h, color);
    r->stats.rects++;
}

static void r_glyph(VcRenderer *r, int x, int y, uint32_t cp, uint32_t color) {
    r->surface.draw_glyph(r->surface.user, x, y, cp, color);
    r->stats.glyphs++;
}

static uint32_t r_read_cp(const VcText *t, uint32_t pos, uint32_t n) {
    char b[4];
    if (vc_copy(t, pos, n, b, 4) != n) return 0;
    const uint8_t *p = (const uint8_t *)b;
    if (n == 1) return p[0];
    if (n == 2) return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (n == 3) return ((uint32_t)(p[0] & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return ((uint32_t)(p[0] & 0x07) << 18) |
           ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

static uint32_t r_cp_cols(uint32_t cp) {
    if (cp >= 0x2E80u) return 2;
    return 1;
}

static uint32_t r_tab_advance(uint32_t col) {
    return VC_TAB_SIZE - (col % VC_TAB_SIZE);
}

static int format_u32(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

VcRenderStats vc_render_frame(VcRenderer *r, const VcView *v,
                              uint32_t x_scroll,
                              uint32_t cursor_pos,
                              uint32_t sel_start, uint32_t sel_end,
                              int focused) {
    VcRenderStats zero = { 0, 0 };
    if (!r) return zero;
    r->stats = zero;

    const VcTheme *th = &r->theme;
    uint32_t width = r->surface.width;
    uint32_t height = r->surface.height;
    uint32_t rows = vc_view_rows(v);
    uint32_t first = vc_view_first_visual(v);

    r_fill(r, 0, 0, (int)width, (int)height, th->bg);

    uint32_t gutter_w = th->gutter_cols * th->cell_w;
    uint32_t text_x = th->padding_x + gutter_w;
    int text_w = (int)width - (int)text_x;
    if (text_w < 0) text_w = 0;
    uint32_t visible_cols = th->cell_w ? (uint32_t)text_w / th->cell_w : 0;

    /* 行号栏底色一次铺满 */
    r_fill(r, 0, 0, (int)(th->padding_x + gutter_w), (int)height, th->gutter_bg);

    uint32_t cursor_visual = 0;
    uint32_t cursor_col = 0;
    if (vc_view_offset_to_pos(v, cursor_pos, &cursor_visual, &cursor_col)) {
        /* 光标可能不在可视区，下面逐行判断 */
    }

    for (uint32_t row = 0; row < rows; row++) {
        uint32_t vi = first + row;
        VcVisualLine vl;
        if (!vc_view_line_info(v, vi, &vl)) break;

        int y = (int)(th->padding_y + row * th->cell_h);

        /* 当前行底色：只覆盖文本区，避免盖住行号栏 */
        if (vi == cursor_visual) {
            r_fill(r, (int)text_x, y, text_w, (int)th->cell_h, th->current_line);
        }

        /* 行号：只在逻辑行的第一个可视行绘制 */
        uint32_t line_start = vc_line_start(vc_view_text(v), vl.line);
        if (vl.offset == line_start) {
            char num[12];
            int n = format_u32(vl.line + 1, num);
            int gx = (int)(th->padding_x + gutter_w) - 6 - n * (int)th->cell_w;
            if (gx < (int)th->padding_x) gx = (int)th->padding_x;
            for (int i = 0; i < n; i++) {
                int px = gx + i * (int)th->cell_w;
                if (px + (int)th->cell_w <= (int)(th->padding_x + gutter_w)) {
                    r_glyph(r, px, y, (uint32_t)(uint8_t)num[i], th->gutter_fg);
                }
            }
        }

        /* 选区 */
        VcSpan span;
        if (vc_view_selection_spans(v, vi, sel_start, sel_end, &span, 1) == 1) {
            uint32_t s = span.start_col;
            uint32_t e = span.end_col;
            if (e > x_scroll && s < x_scroll + visible_cols) {
                if (s < x_scroll) s = x_scroll;
                if (e > x_scroll + visible_cols) e = x_scroll + visible_cols;
                int sx = (int)text_x + (int)((s - x_scroll) * th->cell_w);
                int sw = (int)((e - s) * th->cell_w);
                r_fill(r, sx, y, sw, (int)th->cell_h, th->selection);
            }
        }

        /* 文本 */
        const VcText *t = vc_view_text(v);
        uint32_t i = 0;
        uint32_t col = vl.col_start;
        while (i < vl.length) {
            uint32_t n = vc_char_size(t, vl.offset + i);
            if (n == 0) break;
            uint32_t cp = r_read_cp(t, vl.offset + i, n);
            uint32_t adv = (cp == '\t') ? r_tab_advance(col) : r_cp_cols(cp);

            if (col >= x_scroll && col + adv <= x_scroll + visible_cols) {
                int gx = (int)text_x + (int)((col - x_scroll) * th->cell_w);
                if (cp != '\t') {
                    r_glyph(r, gx, y, cp, th->fg);
                }
            }

            col += adv;
            i += n;
        }

        /* 光标 */
        if (focused && vi == cursor_visual &&
            cursor_col >= x_scroll && cursor_col < x_scroll + visible_cols) {
            int cx = (int)text_x + (int)((cursor_col - x_scroll) * th->cell_w);
            r_fill(r, cx, y + 2, 2, (int)th->cell_h - 4, th->cursor);
        }
    }

    return r->stats;
}
