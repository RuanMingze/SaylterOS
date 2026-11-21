/* ============================================================
 * vizcormin_entry.c - VizCormin 独立模块入口
 * ------------------------------------------------------------
 * 模块不再自己接管屏幕或死循环，而是向宿主返回一张能力表：
 *   - GUI 会话：宿主窗口每帧调用 render，键鼠事件转发到 key/mouse；
 *   - TTY 会话：shell 自己驱动 render/key，并负责 fb_swap。
 * 窗口尺寸变化时宿主调用 resize，模块据此重建布局。
 * 模块内不引用任何内核符号，全部经 SlysKernelApi。
 * ============================================================ */

#include "slys.h"
#include "../core/vc_alloc.h"
#include "../app/vc_app.h"
#include "vc_fs_api.h"
#include "vc_surface_api.h"

static const struct SlysKernelApi *g_api = 0;
static VcApp   *g_app = 0;
static VcSurface g_surface;
static VcSurfaceApiConfig g_cfg;
static int      g_quit = 0;

static uint32_t map_key(uint8_t c, uint32_t *mods) {
    *mods = 0;

    /* 内核把 Ctrl+字母统一编码为 1..26，这与 ASCII 控制字符重叠：
     * 退格=8(Ctrl+H)、Tab=9(Ctrl+I)、换行=10(Ctrl+J)、回车=13(Ctrl+M)。
     * 这几个必须先按编辑键处理，否则退格会被当成 Ctrl+H 吞掉。 */
    if (c >= 1 && c <= 26 && c != 8 && c != 9 && c != 10 && c != 13) {
        *mods = VC_MOD_CTRL;
        return (uint32_t)('a' + c - 1);
    }

    switch (c) {
        case 0xF1: return VC_KEY_LEFT;
        case 0xF2: return VC_KEY_RIGHT;
        case 0xF6: return VC_KEY_UP;
        case 0xF7: return VC_KEY_DOWN;
        case 0xF3: return VC_KEY_HOME;
        case 0xF4: return VC_KEY_END;
        case 0xF5: return VC_KEY_DELETE;
        case 0xF8: return VC_KEY_PGUP;
        case 0xF9: return VC_KEY_PGDN;
        case 27:   return VC_KEY_ESC;
        default:   return (uint32_t)c;
    }
}

static int host_is_windowed(void) {
    return g_api && g_api->win_width && g_api->win_width() > 0;
}

static int host_init(const struct SlysKernelApi *api, const char *path) {
    if (!api || !api->alloc || !api->free) return -1;

    g_api = api;
    g_quit = 0;
    vc_alloc_bind(api->alloc, api->free);
    vc_fs_api_bind(api);
    vc_surface_api_bind(api);

    g_cfg.cell_w = 16;
    g_cfg.cell_h = 32;
    g_cfg.use_win = 0;
    g_surface = vc_surface_api(&g_cfg);
    if (g_surface.width == 0 || g_surface.height == 0) return -1;

    VcFileOps ops = vc_fs_api();
    g_app = vc_app_create(&ops, g_surface.width, g_surface.height,
                          g_cfg.cell_w, g_cfg.cell_h);
    if (!g_app) return -1;

    if (path && path[0]) {
        if (vc_app_open(g_app, path) < 0 && api->log) {
            api->log("[VIZCORMIN] open failed\r\n");
        }
    }

    /* 丢弃启动命令残留的按键（回车、文件名等），避免进入文档。 */
    if (api->kbd_read) {
        for (int i = 0; i < 64; i++) {
            if (api->kbd_read() == 0) break;
        }
    }
    return 0;
}

static void host_render(void) {
    if (g_app) vc_app_render(g_app, &g_surface);
}

static void host_key(uint32_t raw) {
    if (!g_app) return;

    uint8_t u = (uint8_t)raw;
    if (u == 17) {                     /* Ctrl+Q */
        g_quit = 1;
        if (host_is_windowed() && g_api->win_close) g_api->win_close();
        return;
    }

    uint32_t mods = 0;
    uint32_t key = map_key(u, &mods);
    vc_app_key(g_app, key, mods);
    if (g_api->win_invalidate) g_api->win_invalidate();
}

static void host_mouse(int x, int y, int button) {
    if (!g_app) return;
    vc_app_mouse(g_app, x, y, button);
    if (g_api->win_invalidate) g_api->win_invalidate();
}

static void host_resize(void) {
    if (!g_app) return;

    uint32_t w = g_api->win_width ? g_api->win_width() : g_surface.width;
    uint32_t h = g_api->win_height ? g_api->win_height() : g_surface.height;
    if (w == 0 || h == 0) return;

    g_surface.width = w;
    g_surface.height = h;
    vc_app_set_size(g_app, w, h);
}

static void host_shutdown(void) {
    if (g_app) {
        vc_app_destroy(g_app);
        g_app = 0;
    }
    g_quit = 0;
}

static int host_finished(void) {
    return g_quit;
}

static const struct SlysVizCorminHost g_host = {
    SLYS_VIZCORMIN_ABI,
    host_init,
    host_render,
    host_key,
    host_mouse,
    host_resize,
    host_shutdown,
    host_finished
};

const struct SlysVizCorminHost *vizcormin_entry(
        const struct SlysKernelApi *api) {
    g_api = api;
    return &g_host;
}
