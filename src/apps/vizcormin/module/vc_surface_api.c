/* ============================================================
 * vc_surface_api.c - 独立模块的渲染后端
 * ------------------------------------------------------------
 * 两种模式由内核 API 决定：
 *   - 窗口模式：win_width() > 0，绘制限制在应用窗口内容区；
 *   - 全屏模式：TTY 会话，退回 fb_* 直接写整个帧缓冲。
 * 上层 vc_render 只认 VcSurface，不区分这两条路径。
 * ============================================================ */

#include "vc_surface_api.h"

static const struct SlysKernelApi *g_api = 0;

void vc_surface_api_bind(const struct SlysKernelApi *api) {
    g_api = api;
}

static void api_fill(void *user, int x, int y, int w, int h, uint32_t color) {
    VcSurfaceApiConfig *cfg = (VcSurfaceApiConfig *)user;
    if (!g_api) return;

    if (cfg && cfg->use_win) {
        if (g_api->win_fill_rect) g_api->win_fill_rect(x, y, w, h, color);
    } else if (g_api->fb_fill_rect) {
        g_api->fb_fill_rect(x, y, w, h, color);
    }
}

static void api_glyph(void *user, int x, int y, uint32_t cp, uint32_t color) {
    VcSurfaceApiConfig *cfg = (VcSurfaceApiConfig *)user;
    if (!g_api || !cfg) return;

    if (cfg->use_win) {
        if (g_api->win_draw_cell) {
            g_api->win_draw_cell(x, y, (int)cfg->cell_w, (int)cfg->cell_h,
                                 cp, color);
        }
    } else if (g_api->fb_draw_cell) {
        g_api->fb_draw_cell(x, y, (int)cfg->cell_w, (int)cfg->cell_h,
                            cp, color);
    }
}

VcSurface vc_surface_api(VcSurfaceApiConfig *cfg) {
    VcSurface s;
    s.width = 0;
    s.height = 0;
    s.user = cfg;
    s.fill_rect = api_fill;
    s.draw_glyph = api_glyph;
    if (!g_api || !cfg) return s;

    uint32_t win_w = (g_api->win_width) ? g_api->win_width() : 0;
    uint32_t win_h = (g_api->win_height) ? g_api->win_height() : 0;
    if (win_w > 0 && win_h > 0 &&
        g_api->win_fill_rect && g_api->win_draw_cell) {
        cfg->use_win = 1;
        s.width = win_w;
        s.height = win_h;
        return s;
    }

    cfg->use_win = 0;
    s.width = (g_api->fb_width) ? g_api->fb_width() : 0;
    s.height = (g_api->fb_height) ? g_api->fb_height() : 0;
    return s;
}
