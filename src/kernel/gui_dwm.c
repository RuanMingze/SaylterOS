/* gui_dwm.c - window manager / compositor: z-order, focus, drag, resize, decorations, damage merge, present. */

#include "gui_internal.h"
#include "net.h"

/* ---- 呈现性能计数（串口诊断用，见 gui.c 主循环） ---- */
uint32_t g_perf_render_us = 0;
uint32_t g_perf_blit_us = 0;
uint32_t g_perf_dirty_rects = 0;
uint32_t g_perf_dirty_px = 0;
int      g_perf_full = 0;

struct GuiWindow {
    int     open;
    ui_rect r;
    int     z;
    int     maximized;
    int     minimized;
    ui_rect restore;      /* 最大化前的位置，用于还原 */
    uint32_t anim_t0;     /* 打开/还原时刻，用于淡入动画 */
};

struct GuiWindow g_wins[GUI_WIN_MAX];
int g_z_counter = 0;

/* ---- 原生应用窗口状态 ---- */
const GuiExternalHost *g_ext_host = 0;
int    g_ext_space_w = 960;
int    g_ext_space_h = 620;
char   g_ext_title[32] = "App";
ui_rect g_ext_content;
int    g_ext_content_valid = 0;

void dirty_all(void);
void dirty_add(int x, int y, int w, int h);
int g_focus = -1;
int g_dragging = -1;
int g_drag_dx = 0;
int g_drag_dy = 0;

/* 调整窗口大小：edge 用位标记，bit0 = 右边缘，bit1 = 下边缘，
 * 两者都置位就是右下角手柄。 */
int g_resizing = -1;
int g_resize_edge = 0;
ui_rect g_resize_start;
int g_resize_mx0 = 0;
int g_resize_my0 = 0;
ui_rect win_default_rect(int id) {
    ui_rect r;

    if (id == WIN_FILES) {
        r.w = 900;
        r.h = 600;
    } else if (id == WIN_EDITOR) {
        r.w = 960;
        r.h = 620;
    } else if (id == WIN_TERM) {
        r.w = 900;
        r.h = 600;
    } else if (id == WIN_BROWSER) {
        r.w = 1000;
        r.h = 660;
    } else if (id == WIN_SLYZ) {
        int w = 660;
        int h = 620;
        gui_slyz_req_size(&w, &h);
        r.w = w + 24;
        r.h = h + 90;
    } else if (id == WIN_TASKMGR) {
        r.w = 1040;
        r.h = 700;
    } else if (id == WIN_CALC) {
        r.w = 420;
        r.h = 560;
    } else if (id == WIN_MEDIA) {
        r.w = 600;
        r.h = 540;
    } else if (id == WIN_EXTERNAL) {
        r.w = g_ext_space_w + 20;
        r.h = g_ext_space_h + 66;
    } else {
        r.w = 640;
        r.h = 640;
    }

    int avail_w = gW - GUI_TASKBAR_W - 80;
    int avail_h = gH - 120;
    if (r.w > avail_w) r.w = avail_w;
    if (r.h > avail_h) r.h = avail_h;

    r.x = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W - r.w) / 2;
    r.y = (gH - r.h) / 2;
    return r;
}

void win_open(int id) {
    if (!g_wins[id].open) {
        g_wins[id].open = 1;
        g_wins[id].r = win_default_rect(id);
        g_wins[id].maximized = 0;
        g_wins[id].minimized = 0;
    }
    g_wins[id].minimized = 0;
    g_wins[id].z = ++g_z_counter;
    g_wins[id].anim_t0 = net_now_ms();
    g_focus = id;
    gui_anim_touch();
}

/* 按内容区尺寸调整窗口。SLYZ 应用在 win.open 里声明自己需要多大，
 * 不同应用的布局差异很大（计算器 420x560、任务管理器 1040x700），
 * 统一用 640x640 会让文字越界。外框 = 内容 + 左右 10px、上下 56+10px。 */
void gui_slyz_resize_window(int win, int w, int h) {
    if (win < 0 || win >= GUI_WIN_MAX) return;

    int avail_w = gW - GUI_TASKBAR_W - 24;
    int avail_h = gH - 24;
    if (w < 240) w = 240;
    if (h < 160) h = 160;
    if (w + 20 > avail_w) w = avail_w - 20;
    if (h + 66 > avail_h) h = avail_h - 66;

    ui_rect r;
    r.w = w + 20;
    r.h = h + 66;
    r.x = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W - r.w) / 2;
    r.y = (gH - r.h) / 2;
    if (r.x < GUI_TASKBAR_W + 4) r.x = GUI_TASKBAR_W + 4;
    if (r.y < 4) r.y = 4;

    if (!g_wins[win].open) {
        g_wins[win].open = 1;
        g_wins[win].maximized = 0;
        g_wins[win].minimized = 0;
        g_wins[win].z = ++g_z_counter;
        g_wins[win].anim_t0 = net_now_ms();
        g_focus = win;
        gui_anim_touch();
    }
    g_wins[win].r = r;
    dirty_all();
}

void win_focus(int id) {
    g_wins[id].z = ++g_z_counter;
    g_focus = id;
}

void win_close(int id) {
    if (gui_app_active(id)) {
        gui_app_stop(id);
    }
    /* 原生应用窗口：先摘掉宿主再通知，避免 close 回调里再次
     * 请求关闭造成递归。 */
    if (id == WIN_EXTERNAL && g_ext_host) {
        const GuiExternalHost *h = g_ext_host;
        g_ext_host = 0;
        g_ext_content_valid = 0;
        if (h->close) h->close();
    }
    /* 媒体播放器没有独立进程：窗口关闭就停止音频，避免后台
     * 继续播放却找不到控制入口。 */
    if (id == WIN_MEDIA) {
        audio_stop();
    }
    g_wins[id].open = 0;
    g_wins[id].z = 0;
    g_wins[id].maximized = 0;
    g_wins[id].minimized = 0;
    if (g_focus == id) g_focus = -1;
    if (g_dragging == id) g_dragging = -1;
    if (g_resizing == id) g_resizing = -1;
    if (id == WIN_FILES || id == WIN_EDITOR) gui_desk_refresh();
}

/* ============================================================
 * 原生应用窗口（GuiExternalHost）
 * ============================================================ */

int gui_external_open(const char *title, int w, int h,
                      const GuiExternalHost *host) {
    if (!host) return 0;
    g_ext_host = host;
    g_ext_space_w = w > 240 ? w : 240;
    g_ext_space_h = h > 160 ? h : 160;
    int n = 0;
    if (title) {
        while (title[n] && n < (int)sizeof(g_ext_title) - 1) {
            g_ext_title[n] = title[n];
            n++;
        }
    }
    g_ext_title[n] = '\0';
    g_ext_content_valid = 0;
    win_open(WIN_EXTERNAL);
    dirty_all();
    return 1;
}

void gui_external_close(void) {
    if (g_wins[WIN_EXTERNAL].open) {
        win_close(WIN_EXTERNAL);
    } else if (g_ext_host) {
        const GuiExternalHost *h = g_ext_host;
        g_ext_host = 0;
        g_ext_content_valid = 0;
        if (h->close) h->close();
    }
    dirty_all();
}

int gui_external_is_open(void) {
    return g_wins[WIN_EXTERNAL].open && g_ext_host != 0;
}

uint32_t gui_ext_width(void) {
    /* 窗口没打开时必须返回 0：TTY 会话里应用靠这个判断该走
     * 全屏 fb_* 后端，而不是画到不存在的窗口内容区。 */
    if (!g_wins[WIN_EXTERNAL].open) return 0;
    if (g_ext_content_valid && g_ext_content.w > 0) {
        return (uint32_t)g_ext_content.w;
    }
    return (uint32_t)g_ext_space_w;
}

uint32_t gui_ext_height(void) {
    if (!g_wins[WIN_EXTERNAL].open) return 0;
    if (g_ext_content_valid && g_ext_content.h > 0) {
        return (uint32_t)g_ext_content.h;
    }
    return (uint32_t)g_ext_space_h;
}

void gui_ext_invalidate(void) {
    /* 只标脏内容区：打字时不必重绘标题栏与阴影，逐个按键整窗重绘
     * 在 QEMU 的软件渲染下会明显掉帧。 */
    if (g_wins[WIN_EXTERNAL].open && g_ext_content_valid) {
        dirty_add(g_ext_content.x, g_ext_content.y,
                  g_ext_content.w, g_ext_content.h);
    }
}

void gui_ext_request_close(void) {
    gui_external_close();
}

void gui_ext_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_ext_content_valid || w <= 0 || h <= 0) return;
    int x0 = g_ext_content.x + x;
    int y0 = g_ext_content.y + y;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < g_ext_content.x) x0 = g_ext_content.x;
    if (y0 < g_ext_content.y) y0 = g_ext_content.y;
    if (x1 > g_ext_content.x + g_ext_content.w) {
        x1 = g_ext_content.x + g_ext_content.w;
    }
    if (y1 > g_ext_content.y + g_ext_content.h) {
        y1 = g_ext_content.y + g_ext_content.h;
    }
    if (x1 <= x0 || y1 <= y0) return;
    vbe_fill_rect((uint16_t)x0, (uint16_t)y0,
                  (uint16_t)(x1 - x0), (uint16_t)(y1 - y0), color);
}

int gui_ext_draw_cell(int x, int y, int cw, int ch,
                      uint32_t cp, uint32_t fg) {
    if (!g_ext_content_valid) return 0;
    int x0 = g_ext_content.x + x;
    int y0 = g_ext_content.y + y;
    if (x0 + cw <= g_ext_content.x || y0 + ch <= g_ext_content.y) return 0;
    if (x0 >= g_ext_content.x + g_ext_content.w) return 0;
    if (y0 >= g_ext_content.y + g_ext_content.h) return 0;
    vbe_push_clip(g_ext_content.x, g_ext_content.y,
                  g_ext_content.w, g_ext_content.h);
    int r = slyertype_draw_cell_buf(x0, y0, cw, ch, cp, fg);
    vbe_pop_clip();
    return r;
}

/* 最小化：保留 open 与内容，只从桌面与命中测试里移除；
 * 任务栏的窗口按钮仍然可点，点一下还原。 */
void win_minimize(int id) {
    g_wins[id].minimized = 1;
    if (g_focus == id) g_focus = -1;
    if (g_dragging == id) g_dragging = -1;
    if (g_resizing == id) g_resizing = -1;
}

void win_restore_from_min(int id) {
    g_wins[id].minimized = 0;
    g_wins[id].anim_t0 = net_now_ms();
    gui_anim_touch();
}

static ui_rect win_max_rect(void) {
    ui_rect r;
    r.x = GUI_TASKBAR_W + 8;
    r.y = 8;
    r.w = gW - GUI_TASKBAR_W - 16;
    r.h = gH - 16;
    return r;
}

void win_toggle_max(int id) {
    struct GuiWindow *w = &g_wins[id];

    if (w->maximized) {
        w->r = w->restore;
        w->maximized = 0;
    } else {
        w->restore = w->r;
        w->r = win_max_rect();
        w->maximized = 1;
    }

    w->minimized = 0;
    win_focus(id);
}

ui_rect win_content_rect(const ui_rect *r) {
    ui_rect c;
    c.x = r->x + 10;
    c.y = r->y + 56;
    c.w = r->w - 20;
    c.h = r->h - 66;
    return c;
}

ui_rect win_close_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 36;
    c.h = 34;
    c.x = r->x + r->w - 50;
    c.y = r->y + 9;
    return c;
}

ui_rect win_max_btn_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 36;
    c.h = 34;
    c.x = r->x + r->w - 50 - 42;
    c.y = r->y + 9;
    return c;
}

ui_rect win_min_btn_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 36;
    c.h = 34;
    c.x = r->x + r->w - 50 - 84;
    c.y = r->y + 9;
    return c;
}

/* 右下角调整手柄：18x18，画三条斜线作为视觉提示 */
ui_rect win_grip_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 18;
    c.h = 18;
    c.x = r->x + r->w - 18;
    c.y = r->y + r->h - 18;
    return c;
}

/* 判定鼠标落在窗口的哪个调整边上；0 表示不在调整区 */
int win_resize_edge_at(const ui_rect *r, int x, int y, int maximized) {
    if (maximized) return 0;

    int edge = 0;

    if (x >= r->x + r->w - 8 && x < r->x + r->w &&
        y >= r->y + 52 && y < r->y + r->h) {
        edge |= 1;
    }
    if (y >= r->y + r->h - 8 && y < r->y + r->h &&
        x >= r->x + 8 && x < r->x + r->w) {
        edge |= 2;
    }

    return edge;
}

int win_topmost_at(int x, int y) {
    int best = -1;
    int best_z = -1;

    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (!g_wins[i].open || g_wins[i].minimized) continue;
        if (g_point_in(&g_wins[i].r, x, y) && g_wins[i].z > best_z) {
            best_z = g_wins[i].z;
            best = i;
        }
    }
    return best;
}
void gui_window_frame(int id, const ui_theme *t) {
    ui_rect r = g_wins[id].r;

    ui_shadow(r, 16, 10);
    ui_round_gradient(r, 16, C_PANEL2, C_PANEL);
    ui_round_panel(r, 16, (g_focus == id) ? C_ACCENT2 : C_BORDER, C_WIN_BG);

    /* 标题栏 */
    ui_rect title = { r.x + 2, r.y + 2, r.w - 4, 50 };
    ui_round_fill(title, 14, (g_focus == id) ? g_pal.title_active : C_TITLE);
    ui_hline(r.x + 16, r.y + 52, r.w - 32, C_BORDER);

    int ix = r.x + 18;
    int iy = r.y + 15;
    if (id == WIN_FILES) {
        icon_folder(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_EDITOR) {
        icon_file(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_TERM) {
        icon_terminal(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_BROWSER) {
        icon_globe(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_SLYZ) {
        icon_editor(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_TASKMGR) {
        icon_chart(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_CALC) {
        icon_calc(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_MEDIA) {
        icon_chart(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_EXTERNAL) {
        icon_editor(ix, iy, 24, C_ACCENT3);
    } else {
        icon_settings(ix, iy, 24, C_ACCENT3);
    }

    const char *custom_title = (id == WIN_SLYZ) ? gui_app_title(WIN_SLYZ) :
                               (id == WIN_EXTERNAL) ? g_ext_title : 0;
    const char *title_text = custom_title ? custom_title :
                             id == WIN_FILES ? "Files" :
                             id == WIN_EDITOR ? "Text Editor" :
                             id == WIN_TERM ? "Terminal" :
                             id == WIN_BROWSER ? "Browser" :
                             id == WIN_SLYZ ? "Slyz App" :
                             id == WIN_TASKMGR ? "Task Manager" :
                             id == WIN_CALC ? "Calculator" :
                             id == WIN_MEDIA ? "Media Player" : "Settings";
    ui_text_transparent(r.x + 54, r.y + 10, title_text, C_TEXT);

    ui_rect cr = win_close_rect(&r);
    int cr_hover = g_point_in(&cr, g_mx, g_my);
    hunt_add(cr);
    ui_round_fill(cr, 8, cr_hover ? C_ERR : C_PANEL2);
    icon_close(cr.x + 8, cr.y + 7, 20, cr_hover ? 0xFFFFFF : C_DIM);

    ui_rect mr = win_max_btn_rect(&r);
    int mr_hover = g_point_in(&mr, g_mx, g_my);
    hunt_add(mr);
    ui_round_fill(mr, 8, mr_hover ? C_ACCENT2 : C_PANEL2);
    if (g_wins[id].maximized) {
        icon_restore(mr.x + 8, mr.y + 7, 20, mr_hover ? 0xFFFFFF : C_DIM);
    } else {
        icon_maximize(mr.x + 8, mr.y + 7, 20, mr_hover ? 0xFFFFFF : C_DIM);
    }

    ui_rect nr = win_min_btn_rect(&r);
    int nr_hover = g_point_in(&nr, g_mx, g_my);
    hunt_add(nr);
    ui_round_fill(nr, 8, nr_hover ? C_ACCENT2 : C_PANEL2);
    icon_minimize(nr.x + 8, nr.y + 7, 20, nr_hover ? 0xFFFFFF : C_DIM);

    /* 内容区完全交给内置应用：dwm 只负责边框、标题栏与层级。
     * 以后把内置应用换成独立进程时，这里只需要改成一个 surface 提交。 */
    ui_rect c = win_content_rect(&r);
    gui_app_content_render(id, t, c);

    /* 右下角调整手柄：三条斜线，最大化的窗口不显示 */
    if (!g_wins[id].maximized) {
        ui_rect grip = win_grip_rect(&r);
        int grip_hover = g_point_in(&grip, g_mx, g_my);
        hunt_add(grip);

        uint32_t gc = grip_hover ? C_ACCENT3 : C_BORDER;
        for (int k = 0; k < 3; k++) {
            int off = 5 + k * 4;
            ui_line(grip.x + off, grip.y + grip.h - 4,
                    grip.x + grip.w - 4, grip.y + off, gc);
        }
    }

}
/* ---------- 桌面整体渲染 ---------- */

static int gui_window_zorder(int *order) {
    int n = 0;
    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (g_wins[i].open && !g_wins[i].minimized) order[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && g_wins[order[j]].z > g_wins[v].z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    return n;
}

void gui_desktop_render(void) {
    gui_background();
    gui_clock_badge();

    /* 桌面图标位于窗口之下：窗口是前景应用，绝不能被子图标盖住。
     * 顺序必须与 gui_render_dirty_surfaces 完全一致，否则全屏刷新与
     * 局部刷新会得到不同的层级结果。 */
    gui_desk_render();

    /* 窗口按 z 值从低到高绘制 */
    int order[GUI_WIN_MAX];
    int n = gui_window_zorder(order);
    for (int i = 0; i < n; i++) {
        int id = order[i];
        uint32_t t0 = g_wins[id].anim_t0;
        int animating = gui_anim_enabled() && t0 != 0 &&
                        (net_now_ms() - t0) < 170u;

        ui_rect pad = { g_wins[id].r.x - GUI_SHADOW_PAD,
                        g_wins[id].r.y - GUI_SHADOW_PAD,
                        g_wins[id].r.w + GUI_SHADOW_PAD * 2,
                        g_wins[id].r.h + GUI_SHADOW_PAD * 2 };
        uint32_t *under = animating ? gui_anim_begin(pad) : 0;

        gui_window_frame(id, &g_theme);

        if (under) gui_anim_end(gui_anim_alpha(t0, 170));
    }

    gui_taskbar_render();

    if (g_search_open) gui_search_panel_render();
    if (g_user_menu) gui_user_menu_render();
    if (g_power_menu) gui_power_menu_render();
    if (g_net_menu) gui_net_menu_render();
    if (g_time_menu) gui_time_menu_render();
    if (g_tb_overflow) gui_tb_overflow_render();
    if (g_tb_menu) gui_tb_menu_render();

    /* 模态对话框与底部提示永远在最上层 */
    gui_dialog_render();
    gui_toast_render();
}

/* ============================================================
 * 动态脏矩形
 * ------------------------------------------------------------
 * 全屏 blit（1024x768 约 78 万像素）是旧版卡顿的主因。现在每帧只
 * 收集真正变化的矩形，重绘时用 vbe_set_clip 把整棵场景树的绘制限
 * 制在单个矩形内，表面包围盒与当前裁剪不相交就整块跳过，最后逐
 * 矩形 vbe_swap_rect 上屏。
 *
 * 画面不残留脏点由三层保证：
 *   1. 每个改变外观的事件都显式 dirty_add 出它影响的区域；
 *   2. 状态指纹兜底：结构性状态变了但没人标脏时退化成全屏刷新；
 *   3. 光标单独走"擦-重绘-画"路径，并与脏矩形一起上屏。
 * ============================================================ */

#define GUI_DIRTY_MAX 12
#define GUI_SHADOW_PAD 20

ui_rect g_dirty[GUI_DIRTY_MAX];
int g_dirty_count = 0;
int g_dirty_full = 0;
int g_redraw_all_pending = 0;

void dirty_reset(void) {
    g_dirty_count = 0;
    g_dirty_full = 0;
}

/* 应用通过 cfg.set 改了壁纸/头像后调用：下一帧丢弃局部脏矩形，
 * 整屏重绘，避免只刷新设置窗口而桌面背景保持旧图。 */
void gui_request_redraw_all(void) {
    g_redraw_all_pending = 1;
}

void dirty_all(void) {
    g_dirty_full = 1;
    g_dirty_count = 0;
}

int dirty_is_full(void) {
    return g_dirty_full;
}

static int g_rect_touch(const ui_rect *a, const ui_rect *b) {
    return !(a->x + a->w < b->x || b->x + b->w < a->x ||
             a->y + a->h < b->y || b->y + b->h < a->y);
}

void dirty_add(int x, int y, int w, int h) {
    if (g_dirty_full || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gW) w = gW - x;
    if (y + h > gH) h = gH - y;
    if (w <= 0 || h <= 0) return;

    ui_rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    for (int i = 0; i < g_dirty_count; i++) {
        ui_rect *d = &g_dirty[i];
        if (!g_rect_touch(d, &r)) continue;

        int x0 = (d->x < r.x) ? d->x : r.x;
        int y0 = (d->y < r.y) ? d->y : r.y;
        int x1 = (d->x + d->w > r.x + r.w) ? d->x + d->w : r.x + r.w;
        int y1 = (d->y + d->h > r.y + r.h) ? d->y + d->h : r.y + r.h;

        int merged = (x1 - x0) * (y1 - y0);
        int apart = d->w * d->h + r.w * r.h;

        /* 对角合并会把面积放大好几倍，宁可多留一块独立矩形 */
        if (merged > apart + apart / 2 + 24576) continue;

        d->x = x0;
        d->y = y0;
        d->w = x1 - x0;
        d->h = y1 - y0;
        return;
    }

    if (g_dirty_count >= GUI_DIRTY_MAX) {
        dirty_all();
        return;
    }
    g_dirty[g_dirty_count++] = r;
}

void dirty_add_rect(const ui_rect *r) {
    dirty_add(r->x, r->y, r->w, r->h);
}

void dirty_add_padded(const ui_rect *r, int pad) {
    dirty_add(r->x - pad, r->y - pad, r->w + pad * 2, r->h + pad * 2);
}

/* 多个矩形互相重叠时反复合并，减少渲染与上屏次数 */
void dirty_compact(void) {
    int i = 0;
    while (i < g_dirty_count) {
        int merged = 0;
        for (int j = i + 1; j < g_dirty_count; j++) {
            ui_rect *a = &g_dirty[i];
            ui_rect *b = &g_dirty[j];
            if (!g_rect_touch(a, b)) continue;

            int x0 = (a->x < b->x) ? a->x : b->x;
            int y0 = (a->y < b->y) ? a->y : b->y;
            int x1 = (a->x + a->w > b->x + b->w) ? a->x + a->w : b->x + b->w;
            int y1 = (a->y + a->h > b->y + b->h) ? a->y + a->h : b->y + b->h;

            int merged_area = (x1 - x0) * (y1 - y0);
            int apart = a->w * a->h + b->w * b->h;
            if (merged_area > apart + apart / 2 + 24576) continue;

            a->x = x0;
            a->y = y0;
            a->w = x1 - x0;
            a->h = y1 - y0;
            g_dirty[j] = g_dirty[g_dirty_count - 1];
            g_dirty_count--;
            merged = 1;
            break;
        }
        if (!merged) i++;
    }
}

/* 鼠标移动后：只重绘"旧指针命中的 hover 控件"与"新指针命中的"。
 * g_hunt_prev 是上一帧渲染时登记的控件矩形，正好对应屏幕上当前的
 * 高亮状态，所以用它判断需要擦除的旧高亮。 */
void dirty_hover_scan(int old_x, int old_y, int x, int y) {
    for (int i = 0; i < g_hunt_prev_count; i++) {
        const ui_rect *r = &g_hunt_prev[i];
        if (g_point_in(r, old_x, old_y) || g_point_in(r, x, y)) {
            dirty_add_rect(r);
        }
    }
}

void dirty_window(int id) {
    if (id < 0 || id >= GUI_WIN_MAX) return;
    if (!g_wins[id].open) return;
    dirty_add_padded(&g_wins[id].r, GUI_SHADOW_PAD);
}

/* 鼠标所在表面（点击后没有结构性变化时只重绘这一块） */
void dirty_surface_at(int x, int y) {
    if (g_search_open) {
        ui_rect p = search_panel_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (g_user_menu) {
        ui_rect p = user_menu_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (g_power_menu) {
        ui_rect p = power_menu_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (g_net_menu) {
        ui_rect p = net_menu_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (x < GUI_TASKBAR_W) {
        dirty_add(0, 0, GUI_TASKBAR_W, gH);
        return;
    }

    int id = win_topmost_at(x, y);
    if (id >= 0) {
        dirty_window(id);
        return;
    }

    /* 桌面空白：中央那块空桌面提示可能被点亮 */
    dirty_add(GUI_TASKBAR_W, gH / 2 - 100, gW - GUI_TASKBAR_W, 160);
}

/* 当前裁剪矩形与给定包围盒是否相交 */
static int gui_clip_hit(const ui_rect *r) {
    if (!vbe_clip_active()) return 1;

    int cx = 0;
    int cy = 0;
    int cw = 0;
    int ch = 0;
    vbe_get_clip(&cx, &cy, &cw, &ch);

    return !(r->x + r->w <= cx || cx + cw <= r->x ||
             r->y + r->h <= cy || cy + ch <= r->y);
}

int dirty_rect_count(void) {
    return g_dirty_full ? 0 : g_dirty_count;
}

void dirty_rect_get(int index, ui_rect *out) {
    if (!out || index < 0 || index >= g_dirty_count) return;
    *out = g_dirty[index];
}

/* 把"旧光标 + 全部脏矩形 + 新光标"依次上屏。
 * 旧光标位置已经被 gui_cursor_erase 恢复成背景，必须单独刷一次。 */
void dirty_swap_all(int old_cx, int old_cy) {
    if (old_cx >= 0) {
        vbe_swap_rect((uint16_t)old_cx, (uint16_t)old_cy,
                      GUI_CURSOR_W, GUI_CURSOR_H);
    }

    int n = dirty_rect_count();
    for (int i = 0; i < n; i++) {
        ui_rect r;
        dirty_rect_get(i, &r);
        vbe_swap_rect((uint16_t)r.x, (uint16_t)r.y,
                      (uint16_t)r.w, (uint16_t)r.h);
    }

    vbe_swap_rect((uint16_t)g_mx, (uint16_t)g_my,
                  GUI_CURSOR_W, GUI_CURSOR_H);
}

/* 对每个脏矩形重绘一次场景树，绘制被裁剪在矩形内 */
void gui_render_dirty_surfaces(void) {
    int order[GUI_WIN_MAX];
    int n = gui_window_zorder(order);

    for (int i = 0; i < g_dirty_count; i++) {
        ui_rect d = g_dirty[i];
        vbe_set_clip(d.x, d.y, d.w, d.h);

        gui_background();
        gui_desk_render();

        ui_rect badge = { gW - 116, 16, 100, 44 };
        if (gui_clip_hit(&badge)) gui_clock_badge();

        for (int k = 0; k < n; k++) {
            ui_rect box = g_wins[order[k]].r;
            box.x -= GUI_SHADOW_PAD;
            box.y -= GUI_SHADOW_PAD;
            box.w += GUI_SHADOW_PAD * 2;
            box.h += GUI_SHADOW_PAD * 2;
            if (!gui_clip_hit(&box)) continue;

            int id = order[k];
            uint32_t t0 = g_wins[id].anim_t0;
            int animating = gui_anim_enabled() && t0 != 0 &&
                            (net_now_ms() - t0) < 170u;
            uint32_t *under = animating ? gui_anim_begin(box) : 0;

            gui_window_frame(id, &g_theme);

            if (under) gui_anim_end(gui_anim_alpha(t0, 170));
        }

        ui_rect tb = { 0, 0, GUI_TASKBAR_W, gH };
        if (gui_clip_hit(&tb)) gui_taskbar_render();

        if (g_search_open) {
            ui_rect p = search_panel_rect();
            if (gui_clip_hit(&p)) gui_search_panel_render();
        }
        if (g_user_menu) {
            ui_rect p = user_menu_rect();
            if (gui_clip_hit(&p)) gui_user_menu_render();
        }
        if (g_power_menu) {
            ui_rect p = power_menu_rect();
            if (gui_clip_hit(&p)) gui_power_menu_render();
        }
        if (g_net_menu) {
            ui_rect p = net_menu_rect();
            if (gui_clip_hit(&p)) gui_net_menu_render();
        }
        if (g_time_menu) {
            ui_rect p = time_menu_rect();
            if (gui_clip_hit(&p)) gui_time_menu_render();
        }
        if (g_tb_overflow) {
            ui_rect p = tb_overflow_rect();
            if (gui_clip_hit(&p)) gui_tb_overflow_render();
        }
        if (g_tb_menu) {
            ui_rect p = tb_menu_rect();
            if (gui_clip_hit(&p)) gui_tb_menu_render();
        }

        /* 输入法候选窗永远画在最上层 */
        if (gui_ime_active()) {
            ui_rect ip = gui_ime_rect();
            ui_rect hit = { ip.x - 4, ip.y - 4, ip.w + 8, ip.h + 8 };
            if (gui_clip_hit(&hit)) gui_ime_render();
        }

        /* 模态对话框/提示覆盖包括候选窗在内的一切 */
        gui_dialog_render();
        gui_toast_render();

        vbe_clear_clip();
    }
}

void gui_window_click(int id, int x, int y) {
    ui_rect r = g_wins[id].r;

    ui_rect close_r = win_close_rect(&r);
    if (g_point_in(&close_r, x, y)) {
        gui_window_request_close(id);
        return;
    }

    ui_rect max_r = win_max_btn_rect(&r);
    if (g_point_in(&max_r, x, y)) {
        win_toggle_max(id);
        return;
    }

    ui_rect min_r = win_min_btn_rect(&r);
    if (g_point_in(&min_r, x, y)) {
        win_minimize(id);
        return;
    }

    /* 边缘/角落调整大小优先于拖动与内容点击 */
    int edge = win_resize_edge_at(&r, x, y, g_wins[id].maximized);
    if (edge) {
        win_focus(id);
        g_resizing = id;
        g_resize_edge = edge;
        g_resize_start = r;
        g_resize_mx0 = x;
        g_resize_my0 = y;
        return;
    }

    if (y >= r.y && y < r.y + 52) {
        win_focus(id);
        /* 最大化时标题栏不拖动，避免把窗口拖成"半出屏"的怪状态 */
        if (!g_wins[id].maximized) {
            g_dragging = id;
            g_drag_dx = x - r.x;
            g_drag_dy = y - r.y;
        }
        return;
    }

    win_focus(id);

    ui_rect c = win_content_rect(&r);
    gui_app_content_click(id, c, x, y);
}
static uint32_t g_sig_mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

uint32_t gui_state_sig(void) {
    uint32_t h = 2166136261u;

    h = g_sig_mix(h, (uint32_t)(g_focus + 1));
    h = g_sig_mix(h, (uint32_t)g_search_open);
    h = g_sig_mix(h, (uint32_t)g_user_menu);
    h = g_sig_mix(h, (uint32_t)g_power_menu);
    h = g_sig_mix(h, (uint32_t)g_net_menu);
    h = g_sig_mix(h, (uint32_t)g_time_menu);
    h = g_sig_mix(h, (uint32_t)g_tb_overflow);
    h = g_sig_mix(h, (uint32_t)g_tb_menu);
    h = g_sig_mix(h, (uint32_t)g_fm_modal);
    h = g_sig_mix(h, (uint32_t)g_ed_ask_save);
    h = g_sig_mix(h, (uint32_t)g_desk_menu);
    h = g_sig_mix(h, (uint32_t)g_desk_prompt);
    h = g_sig_mix(h, (uint32_t)(g_desk_click_idx + 1));
    h = g_sig_mix(h, (uint32_t)g_desk_count);

    for (int i = 0; i < GUI_WIN_MAX; i++) {
        h = g_sig_mix(h, (uint32_t)(g_wins[i].open ? (g_wins[i].z + 1) : 0));
        h = g_sig_mix(h, (uint32_t)g_wins[i].maximized);
        h = g_sig_mix(h, (uint32_t)g_wins[i].minimized);
    }

    return h;
}

/* 键盘输入影响的表面：搜索面板 / 焦点窗口 */
void dirty_key_surface(void) {
    if (g_search_open) {
        ui_rect p = search_panel_rect();
        dirty_add_padded(&p, 18);
        return;
    }
    if (g_focus >= 0 && g_wins[g_focus].open) {
        dirty_window(g_focus);
        return;
    }
    dirty_all();
}

/* 光标闪烁只影响"带输入框"的表面 */
void dirty_blink_surface(void) {
    if (g_search_open) {
        ui_rect p = search_panel_rect();
        dirty_add_padded(&p, 18);
        return;
    }
    if (g_fm_modal) {
        dirty_window(WIN_FILES);
        return;
    }
    if (g_ed_ask_save) {
        dirty_window(WIN_EDITOR);
        return;
    }
    if (gui_editor_is_open()) {
        dirty_window(WIN_EDITOR);
    }
}

static void pf_account(int full, uint32_t pixels);

void gui_present_all(void) {
    gui_cursor_erase();
    hunt_reset();
    gui_desktop_render();
    hunt_snapshot();
    gui_cursor_draw(g_mx, g_my);
    ui_swap();
}

void gui_present_cursor(void) {
    int old_x = g_cursor_x;
    int old_y = g_cursor_y;

    gui_cursor_erase();
    gui_cursor_draw(g_mx, g_my);

    if (old_x >= 0) {
        vbe_swap_rect((uint16_t)old_x, (uint16_t)old_y,
                      GUI_CURSOR_W, GUI_CURSOR_H);
    }
    vbe_swap_rect((uint16_t)g_mx, (uint16_t)g_my,
                  GUI_CURSOR_W, GUI_CURSOR_H);

    pf_account(0, (uint32_t)GUI_CURSOR_W * GUI_CURSOR_H * 2u);
}

/* ---- 临时呈现统计（验证稳态没有全屏 blit） ---- */
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

static uint32_t g_pf_frames = 0;
static uint32_t g_pf_full = 0;
static uint32_t g_pf_pixels = 0;

static void pf_account(int full, uint32_t pixels) {
    g_pf_frames++;
    if (full) g_pf_full++;
    g_pf_pixels += pixels;

    if (g_pf_frames >= 600) {
        kserial_puts("[GUI] frames=");
        kserial_dec32(g_pf_frames);
        kserial_puts(" full=");
        kserial_dec32(g_pf_full);
        kserial_puts(" avg_px=");
        kserial_dec32(g_pf_pixels / g_pf_frames);
        kserial_puts("\r\n");
        g_pf_frames = 0;
        g_pf_full = 0;
        g_pf_pixels = 0;
    }
}

void gui_present_dirty(void) {
    int old_x = g_cursor_x;
    int old_y = g_cursor_y;

    uint32_t t0 = net_now_us();
    gui_cursor_erase();

    hunt_reset();
    if (dirty_is_full()) {
        gui_desktop_render();
    } else {
        gui_render_dirty_surfaces();
    }
    hunt_snapshot();

    gui_cursor_draw(g_mx, g_my);
    uint32_t t1 = net_now_us();

    g_perf_render_us = t1 - t0;
    g_perf_full = dirty_is_full();
    g_perf_dirty_rects = (uint32_t)dirty_rect_count();

    if (dirty_is_full()) {
        ui_swap();
        uint32_t t2 = net_now_us();
        g_perf_blit_us = t2 - t1;
        g_perf_dirty_px = (uint32_t)gW * (uint32_t)gH;
        pf_account(1, (uint32_t)gW * (uint32_t)gH);
        return;
    }

    uint32_t px = (uint32_t)GUI_CURSOR_W * GUI_CURSOR_H * 2u;
    int n_rects = dirty_rect_count();
    for (int i = 0; i < n_rects; i++) {
        ui_rect rr;
        dirty_rect_get(i, &rr);
        px += (uint32_t)rr.w * (uint32_t)rr.h;
    }

    dirty_swap_all(old_x, old_y);
    uint32_t t2 = net_now_us();
    g_perf_blit_us = t2 - t1;
    g_perf_dirty_px = px;
    pf_account(0, px);
}
