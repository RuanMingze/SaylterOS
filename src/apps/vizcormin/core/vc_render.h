/* ============================================================
 * vc_render.h - VizCormin 渲染层（与后端解耦）
 * ------------------------------------------------------------
 * 渲染器只通过 VcSurface 回调画矩形与字形：
 *   - 宿主测试用内存后端记录绘制调用；
 *   - SaylterOS 用 SlyzUI/vbe 后端实现同样的接口。
 *
 * 一帧的内容：背景、当前行、行号栏、选区、可视行文本、光标。
 * 水平滚动以“列”为单位，超出的字符不提交给后端。
 * ============================================================ */

#ifndef VC_RENDER_H
#define VC_RENDER_H

#include <stdint.h>
#include "vc_view.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    void    *user;
    void   (*fill_rect)(void *user, int x, int y, int w, int h, uint32_t color);
    void   (*draw_glyph)(void *user, int x, int y, uint32_t cp, uint32_t color);
} VcSurface;

typedef struct {
    uint32_t bg;
    uint32_t fg;
    uint32_t dim;
    uint32_t cursor;
    uint32_t selection;
    uint32_t current_line;
    uint32_t gutter_bg;
    uint32_t gutter_fg;

    uint32_t cell_w;
    uint32_t cell_h;
    uint32_t padding_x;
    uint32_t padding_y;
    uint32_t gutter_cols;
} VcTheme;

typedef struct {
    uint32_t glyphs;
    uint32_t rects;
} VcRenderStats;

typedef struct VcRenderer VcRenderer;

VcRenderer *vc_render_create(const VcSurface *surface, const VcTheme *theme);
void        vc_render_destroy(VcRenderer *r);

/* 窗口尺寸/偏移变化时更新后端，不需要重建渲染器。 */
void        vc_render_set_surface(VcRenderer *r, const VcSurface *surface);

/* x_scroll 以列为单位；focused=0 时不画光标。 */
VcRenderStats vc_render_frame(VcRenderer *r, const VcView *v,
                              uint32_t x_scroll,
                              uint32_t cursor_pos,
                              uint32_t sel_start, uint32_t sel_end,
                              int focused);

#endif /* VC_RENDER_H */
