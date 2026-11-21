/* ============================================================
 * vc_surface_vbe.h - vbe/SlyerType 渲染后端
 * ------------------------------------------------------------
 * 把 VcSurface 映射到内核 backbuffer：
 *   fill_rect -> vbe_fill_rect（遵守当前裁剪）
 *   draw_glyph -> slyertype_draw_cell_buf，失败时回退到
 *                 vbe_draw_utf8_transparent。
 * 调用方必须在绘制前 vbe_push_clip 到编辑器窗口。
 * ============================================================ */

#ifndef VC_SURFACE_VBE_H
#define VC_SURFACE_VBE_H

#include "../core/vc_render.h"

typedef struct {
    uint32_t cell_w;
    uint32_t cell_h;
} VcVbeConfig;

/* cfg 必须比返回的 VcSurface 活得久。 */
VcSurface vc_surface_vbe(VcVbeConfig *cfg);

#endif /* VC_SURFACE_VBE_H */
