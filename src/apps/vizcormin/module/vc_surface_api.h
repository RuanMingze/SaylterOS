/* ============================================================
 * vc_surface_api.h - 独立模块的 vbe 渲染后端（经 KernelApi）
 * ============================================================ */

#ifndef VC_SURFACE_API_H
#define VC_SURFACE_API_H

#include "slys.h"
#include "../core/vc_render.h"

typedef struct {
    uint32_t cell_w;
    uint32_t cell_h;
    /* 1 = 绘制到宿主窗口内容区（win_*），0 = 全屏 TTY（fb_*）。 */
    int      use_win;
} VcSurfaceApiConfig;

void      vc_surface_api_bind(const struct SlysKernelApi *api);
VcSurface vc_surface_api(VcSurfaceApiConfig *cfg);

#endif /* VC_SURFACE_API_H */
