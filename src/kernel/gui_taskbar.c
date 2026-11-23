/* gui_taskbar.c - taskbar: layout, clock, search panel, start/power/network menus, running indicators. */

#include "gui_internal.h"

int g_search_open = 0;
char g_search[64];
int g_search_len = 0;
int g_search_sel = 0;

int g_user_menu = 0;
int g_power_menu = 0;
int g_net_menu = 0;
int g_time_menu = 0;
int g_tb_overflow = 0;
int g_tb_menu = 0;
int g_pending_action = ACT_NONE;

/* ---- 任务栏新增面板的原型（实现见文件后半） ---- */
ui_rect time_menu_rect(void);
ui_rect tb_overflow_rect(void);
ui_rect tb_menu_rect(void);
void gui_time_menu_render(void);
void gui_time_menu_click(int x, int y);
void gui_tb_overflow_render(void);
void gui_tb_overflow_click(int x, int y);
void gui_tb_menu_render(void);
int  gui_tb_menu_click(int x, int y);
void gui_tb_menu_close(void);
void gui_taskbar_rclick(int x, int y);
void gui_taskbar_drag_begin(int x, int y);
void gui_taskbar_drag_update(int x, int y);
void gui_taskbar_drag_end(void);

/* 拖动状态：按住固定图标后移动超过阈值才进入排序，
 * 否则松开时按普通点击启动应用。 */
static int g_drag_slot = -1;
static int g_drag_sy = 0;
static int g_drag_active = 0;

/* 右键菜单目标 */
static int g_tb_menu_kind = 0;     /* 1=固定应用，2=运行窗口 */
static int g_tb_menu_slot = 0;
static int g_tb_menu_win = -1;
struct GuiAppDef {
    const char *name;
    const char *desc;
    int         win;
    int         hidden;   /* 1 = 不在任务栏显示，只在搜索面板出现 */
};

const struct GuiAppDef g_apps[GUI_APP_COUNT] = {
    { "Files",        "Browse your home directory",      WIN_FILES,    0 },
    { "Text Editor",  "Create and edit text files",       WIN_EDITOR,   0 },
    { "Terminal",     "Run shell commands in a window",   WIN_TERM,     0 },
    { "Browser",      "Browse http pages (HTML + CSS)",   WIN_BROWSER,  0 },
    { "Settings",     "Boot mode, clock and account",     WIN_SETTINGS, 0 },
    { "Task Manager", "CPU, memory, disk and processes",  WIN_TASKMGR,  0 },
    { "Calculator",   "Arithmetic with divide-by-zero guard", WIN_CALC, 0 },
    { "SylFault",     "Crash, dump and memory leak lab",  WIN_SLYZ,     1 }
};

/* ============================================================
 * 固定应用（任务栏偏好）
 * ------------------------------------------------------------
 * 用户可右键固定/取消固定、拖动排序；结果写入 etc\desktop.cfg。
 * 默认只固定 4 个高频应用，其余放进"所有应用"，避免任务栏拥挤。
 * ============================================================ */
#define TB_MAX_PIN      8
#define TB_PIN_Y0       156
#define TB_PIN_STEP     54
#define TB_PIN_H        50
#define TB_DESKTOP_CFG  "etc\\desktop.cfg"

static int g_pin[TB_MAX_PIN];
static int g_pin_count;
static int g_pin_loaded;

/* SlyzFS 桥接层导出的接口（与 gui.c 使用同一套） */
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

static int tb_app_index(int win) {
    for (int i = 0; i < GUI_APP_COUNT; i++) {
        if (g_apps[i].win == win) return i;
    }
    return -1;
}

int gui_taskbar_pin_count(void) {
    return g_pin_count;
}

int gui_taskbar_pin_win(int slot) {
    if (slot < 0 || slot >= g_pin_count) return -1;
    return g_pin[slot];
}

void gui_taskbar_prefs_load(void) {
    if (g_pin_loaded) return;
    g_pin_loaded = 1;
    g_pin_count = 0;

    static char buf[256];
    int n = 0;
    void *fh = slyzfs_open(TB_DESKTOP_CFG, "r");
    if (fh) {
        n = slyzfs_read(fh, (uint8_t *)buf, (int)sizeof(buf) - 1);
        slyzfs_close(fh);
    }

    if (n > 0) {
        buf[n] = '\0';
        char *p = buf;
        while (*p && *p != '=') p++;
        if (*p == '=') p++;
        while (*p && g_pin_count < TB_MAX_PIN) {
            while (*p == ' ' || *p == '\t') p++;
            char *start = p;
            while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
            int len = (int)(p - start);
            if (len > 0) {
                for (int i = 0; i < GUI_APP_COUNT; i++) {
                    if (g_apps[i].hidden) continue;
                    const char *nm = g_apps[i].name;
                    int k = 0;
                    while (k < len && nm[k] &&
                           g_lower((uint8_t)nm[k]) == g_lower((uint8_t)start[k])) {
                        k++;
                    }
                    if (k == len && nm[k] == '\0') {
                        g_pin[g_pin_count++] = g_apps[i].win;
                        break;
                    }
                }
            }
            if (*p == ',') p++;
            else break;
        }
    }

    if (g_pin_count == 0) {
        g_pin[g_pin_count++] = WIN_FILES;
        g_pin[g_pin_count++] = WIN_TERM;
        g_pin[g_pin_count++] = WIN_SETTINGS;
        g_pin[g_pin_count++] = WIN_BROWSER;
    }
}

static void tb_pin_save(void) {
    char out[256];
    int p = 0;
    const char *head = "pin=";
    while (*head && p < 220) out[p++] = *head++;
    for (int i = 0; i < g_pin_count && p < 220; i++) {
        int ai = tb_app_index(g_pin[i]);
        if (ai < 0) continue;
        if (i > 0 && p < 220) out[p++] = ',';
        const char *nm = g_apps[ai].name;
        while (*nm && p < 220) out[p++] = *nm++;
    }
    out[p++] = '\n';

    void *fh = slyzfs_open(TB_DESKTOP_CFG, "w");
    if (!fh) return;
    slyzfs_write(fh, (const uint8_t *)out, p);
    slyzfs_close(fh);
}

int gui_taskbar_is_pinned(int win) {
    for (int i = 0; i < g_pin_count; i++) {
        if (g_pin[i] == win) return 1;
    }
    return 0;
}

void gui_taskbar_pin_add(int win) {
    if (gui_taskbar_is_pinned(win) || g_pin_count >= TB_MAX_PIN) return;
    if (tb_app_index(win) < 0) return;
    g_pin[g_pin_count++] = win;
    tb_pin_save();
}

void gui_taskbar_pin_remove(int win) {
    int k = 0;
    for (int i = 0; i < g_pin_count; i++) {
        if (g_pin[i] != win) g_pin[k++] = g_pin[i];
    }
    if (k != g_pin_count) {
        g_pin_count = k;
        tb_pin_save();
    }
}

/* 任务栏与启动器只排列可见应用；隐藏项不占槽位。 */
int gui_visible_app_count(void) {
    return g_pin_count;
}

int tb_pin_slots(void) {
    int bottom = tb_net_rect().y - 56;      /* 溢出按钮占一格 */
    int n = (bottom - TB_PIN_Y0) / TB_PIN_STEP;
    if (n < 1) n = 1;
    if (n > TB_MAX_PIN) n = TB_MAX_PIN;
    return n;
}

ui_rect tb_pin_rect(int slot) {
    return (ui_rect){ 16, TB_PIN_Y0 + slot * TB_PIN_STEP, 56, TB_PIN_H };
}

ui_rect tb_more_rect(void) {
    return (ui_rect){ 16, tb_net_rect().y - 56, 56, 48 };
}

/* 当前需要在任务栏"运行中窗口"区域列出的窗口（排除已固定的）。
 * 按 z 序从高到低排列：焦点窗口永远排在第一格。 */
static int tb_open_list(int *out, int max) {
    int n = 0;
    for (int i = 0; i < GUI_WIN_MAX && n < max; i++) {
        if (!g_wins[i].open || i == WIN_MEDIA) continue;
        out[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = out[i];
        int j = i - 1;
        while (j >= 0 && g_wins[out[j]].z < g_wins[v].z) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = v;
    }
    return n;
}

static int tb_running_list(int *out, int max) {
    int all[GUI_WIN_MAX];
    int n = tb_open_list(all, GUI_WIN_MAX);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (gui_taskbar_is_pinned(all[i])) continue;
        out[k++] = all[i];
    }
    return k;
}

/* 运行中窗口区域的起始 y：紧跟固定应用槽位之后。 */
static int tb_win_base(void) {
    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    return TB_PIN_Y0 + shown * TB_PIN_STEP + 16;
}
/* ---------- 任务栏几何 ----------
 * 竖直空间预算（1600x900）：头像 20..76、搜索 88..144、
 * 固定应用 156..、运行窗口列表、网络 gH-200、溢出按钮 gH-256、
 * 时钟 gH-140、电源 gH-64。屏幕变矮时固定槽位自动减少，
 * 多出的应用进入溢出侧拉菜单。 */

static ui_rect tb_avatar_rect(void) {
    return (ui_rect){ 16, 20, 56, 56 };
}

static ui_rect tb_search_rect(void) {
    return (ui_rect){ 16, 88, 56, 56 };
}

/* 参数是运行中窗口列表里的槽位序号（0..n-1），不是窗口 id。 */
static ui_rect tb_win_rect(int slot) {
    return (ui_rect){ 12 + (slot % 2) * 34, tb_win_base() + (slot / 2) * 30,
                      30, 26 };
}

ui_rect tb_net_rect(void) {
    return (ui_rect){ 16, gH - 200, 56, 48 };
}

ui_rect tb_clock_rect(void) {
    return (ui_rect){ 16, gH - 140, 56, 64 };
}

static ui_rect tb_power_rect(void) {
    return (ui_rect){ 16, gH - 64, 56, 48 };
}

ui_rect search_panel_rect(void) {
    /* 高度随应用数量增长：固定 500 在应用超过 5 个后会把最后一行
     * 画到面板外面。上限夹在屏幕内，保证任何分辨率都不越界。 */
    int h = 120 + GUI_APP_COUNT * 72 + 16;
    if (96 + h > gH - 16) h = gH - 16 - 96;
    if (h < 240) h = 240;
    return (ui_rect){ GUI_TASKBAR_W + 10, 96, 440, h };
}

ui_rect user_menu_rect(void) {
    return (ui_rect){ GUI_TASKBAR_W + 8, 20, 300, 176 };
}

ui_rect power_menu_rect(void) {
    return (ui_rect){ GUI_TASKBAR_W + 8, gH - 250, 300, 230 };
}

ui_rect net_menu_rect(void) {
    return (ui_rect){ GUI_TASKBAR_W + 8, gH - 560, 520, 520 };
}

static ui_rect volume_slider_rect(const ui_rect *p) {
    return (ui_rect){ p->x + 20, p->y + 128, p->w - 40, 14 };
}

/* ---------- 搜索 ---------- */
int search_filter(int out[GUI_APP_COUNT]) {
    int n = 0;
    for (int i = 0; i < GUI_APP_COUNT; i++) {
        if (g_search_len == 0 ||
            g_contains_nocase(g_apps[i].name, g_search) ||
            g_contains_nocase(g_apps[i].desc, g_search)) {
            out[n++] = i;
        }
    }
    return n;
}

void launch_app(int win) {
    /* 已打开的应用只恢复并聚焦，不能重新初始化（否则终端内容、
     * 编辑器缓冲会被清空）。 */
    if (win >= 0 && win < GUI_WIN_MAX && g_wins[win].open) {
        if (g_wins[win].minimized) win_restore_from_min(win);
        win_focus(win);
        g_search_open = 0;
        return;
    }

    if (win == WIN_FILES) {
        const struct AuthUser *u = auth_current();
        /* 文件管理器已迁移为独立 SLYZ 程序；镜像缺失时回退内置实现，
         * 保证系统盘被替换或损坏时桌面仍然可用。 */
        if (!gui_app_start(WIN_FILES, "system\\apps\\files.slyz")) {
            gui_files_open(u ? u->home : "");
        }
    } else if (win == WIN_EDITOR) {
        gui_editor_open("");
    } else if (win == WIN_TERM) {
        gui_term_reset();
    } else if (win == WIN_BROWSER) {
        gui_browser_reset();
    } else if (win == WIN_CALC) {
        /* 计算器已迁移为独立 SLYZ 程序。镜像缺失或损坏时保留内置实现，
         * 桌面不会因为一个应用文件丢失而无法使用。 */
        gui_app_start(WIN_CALC, "system\\apps\\calc.slyz");
    } else if (win == WIN_SETTINGS) {
        gui_app_start(WIN_SETTINGS, "system\\apps\\settings.slyz");
    } else if (win == WIN_TASKMGR) {
        gui_app_start(WIN_TASKMGR, "system\\apps\\taskmgr.slyz");
    } else if (win == WIN_SLYZ) {
        /* SylFault：开发工具，不在任务栏出现，只能从搜索启动。
         * 镜像缺失时窗口仍会打开并显示通用 Slyz 页，便于诊断系统盘。 */
        gui_app_start(WIN_SLYZ, "system\\apps\\sylfault.slyz");
    }
    win_open(win);
    g_search_open = 0;
}
int gui_hover_id(void) {
    int x = g_mx;
    int y = g_my;

    if (g_search_open) {
        ui_rect p = search_panel_rect();
        return g_point_in(&p, x, y) ? 60 : 0;
    }
    if (g_user_menu) {
        ui_rect p = user_menu_rect();
        return g_point_in(&p, x, y) ? 61 : 0;
    }
    if (g_power_menu) {
        ui_rect p = power_menu_rect();
        return g_point_in(&p, x, y) ? 62 : 0;
    }
    if (g_net_menu) {
        ui_rect p = net_menu_rect();
        return g_point_in(&p, x, y) ? 63 : 0;
    }

    if (g_time_menu) {
        ui_rect p = time_menu_rect();
        return g_point_in(&p, x, y) ? 66 : 0;
    }
    if (g_tb_menu) {
        ui_rect p = tb_menu_rect();
        return g_point_in(&p, x, y) ? 68 : 0;
    }
    if (g_tb_overflow) {
        ui_rect p = tb_overflow_rect();
        return g_point_in(&p, x, y) ? 67 : 0;
    }

    if (x < GUI_TASKBAR_W) {
        ui_rect av = tb_avatar_rect();
        if (g_point_in(&av, x, y)) return 1;
        ui_rect sr = tb_search_rect();
        if (g_point_in(&sr, x, y)) return 2;

        int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
        for (int s = 0; s < shown; s++) {
            ui_rect r = tb_pin_rect(s);
            if (g_point_in(&r, x, y)) return 10 + s;
        }
        if (g_pin_count > shown) {
            ui_rect mr = tb_more_rect();
            if (g_point_in(&mr, x, y)) return 64;
        }

        int list[GUI_WIN_MAX];
        int n = tb_running_list(list, GUI_WIN_MAX);
        for (int s = 0; s < n; s++) {
            ui_rect r = tb_win_rect(s);
            if (g_point_in(&r, x, y)) return 20 + list[s];
        }
        ui_rect cr = tb_clock_rect();
        if (g_point_in(&cr, x, y)) return 65;
        ui_rect nr = tb_net_rect();
        if (g_point_in(&nr, x, y)) return 5;
        ui_rect p = tb_power_rect();
        if (g_point_in(&p, x, y)) return 3;
        return 4;
    }

    int w = win_topmost_at(x, y);
    if (w >= 0) {
        ui_rect cr = win_close_rect(&g_wins[w].r);
        if (g_point_in(&cr, x, y)) return 40 + w;
        return 50 + w;
    }
    return 0;
}

/* hover id 反过来映射到"该控件占据的矩形"，局部重绘据此擦除旧高亮。
 * 返回 1 表示该 id 有明确矩形。 */
int gui_hover_rect(int id, ui_rect *out) {
    if (id == 1) {
        *out = tb_avatar_rect();
        return 1;
    }
    if (id == 2) {
        *out = tb_search_rect();
        return 1;
    }
    if (id == 3) {
        *out = tb_power_rect();
        return 1;
    }
    if (id == 5) {
        *out = tb_net_rect();
        return 1;
    }
    if (id == 4) {
        *out = (ui_rect){ 0, 0, GUI_TASKBAR_W, gH };
        return 1;
    }
    if (id >= 10 && id < 10 + TB_MAX_PIN) {
        *out = tb_pin_rect(id - 10);
        return 1;
    }
    if (id >= 20 && id < 20 + GUI_WIN_MAX) {
        int list[GUI_WIN_MAX];
        int n = tb_running_list(list, GUI_WIN_MAX);
        for (int s = 0; s < n; s++) {
            if (list[s] == id - 20) {
                *out = tb_win_rect(s);
                return 1;
            }
        }
        return 0;
    }
    if (id == 64) {
        *out = tb_more_rect();
        return 1;
    }
    if (id == 65) {
        *out = tb_clock_rect();
        return 1;
    }
    if (id == 66) {
        *out = time_menu_rect();
        return 1;
    }
    if (id == 67) {
        *out = tb_overflow_rect();
        return 1;
    }
    if (id == 68) {
        *out = tb_menu_rect();
        return 1;
    }
    if (id >= 40 && id < 40 + GUI_WIN_MAX) {
        *out = win_close_rect(&g_wins[id - 40].r);
        return 1;
    }
    if (id >= 50 && id < 50 + GUI_WIN_MAX) {
        *out = g_wins[id - 50].r;
        return 1;
    }
    if (id == 60) {
        *out = search_panel_rect();
        return 1;
    }
    if (id == 61) {
        *out = user_menu_rect();
        return 1;
    }
    if (id == 62) {
        *out = power_menu_rect();
        return 1;
    }
    if (id == 63) {
        *out = net_menu_rect();
        return 1;
    }
    return 0;
}

/* ---------- 任务栏渲染 ---------- */

/* 应用图标：任务栏固定区与运行列表共用 */
static void tb_draw_app_icon(int win, int x, int y, int s, uint32_t color) {
    if (win == WIN_FILES) {
        icon_folder(x, y, s, color);
    } else if (win == WIN_EDITOR) {
        icon_editor(x, y, s, color);
    } else if (win == WIN_TERM) {
        icon_terminal(x, y, s, color);
    } else if (win == WIN_BROWSER) {
        icon_globe(x, y, s, color);
    } else if (win == WIN_TASKMGR) {
        icon_chart(x, y, s, color);
    } else if (win == WIN_CALC) {
        icon_calc(x, y, s, color);
    } else if (win == WIN_EXTERNAL) {
        icon_editor(x, y, s, color);
    } else {
        icon_settings(x, y, s, color);
    }
}

void gui_taskbar_render(void) {
    ui_rect bar = { 0, 0, GUI_TASKBAR_W, gH };
    ui_gradient_v(bar, C_BAR_TOP, C_BAR_BOT);
    ui_vline(GUI_TASKBAR_W - 1, 0, gH, C_BORDER);

    const struct AuthUser *u = auth_current();
    const char *uname = u ? u->name : "?";
    ui_rect av = tb_avatar_rect();
    gui_avatar(av.x + av.w / 2, av.y + av.h / 2, 28, uname,
               C_AVATAR_A, C_AVATAR_B);

    ui_rect sr = tb_search_rect();
    int sr_hover = g_point_in(&sr, g_mx, g_my);
    hunt_add(sr);
    ui_round_panel(sr, 14, sr_hover ? C_ACCENT : C_BORDER,
                   g_search_open ? C_SEL : C_PANEL2);
    icon_search(sr.x + 28, sr.y + 26, 12, sr_hover ? C_ACCENT3 : C_DIM);

    /* 固定应用：只画前 N 个能放下的槽位 */
    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    for (int s = 0; s < shown; s++) {
        int win = g_pin[s];
        ui_rect r = tb_pin_rect(s);
        int hover = g_point_in(&r, g_mx, g_my);
        int open = (win >= 0 && win < GUI_WIN_MAX && g_wins[win].open);

        hunt_add(r);
        ui_round_panel(r, 14, hover ? C_ACCENT : (open ? C_ACCENT2 : C_BORDER),
                       hover ? C_HOVER : C_PANEL2);

        int ix = r.x + 14;
        int iy = r.y + 10;
        if (g_drag_active && g_drag_slot == s) {
            ix += 4;
            iy += 4;
        }
        tb_draw_app_icon(win, ix, iy, 28, open ? C_ACCENT3 : C_DIM);

        if (open) {
            vbe_fill_rect((uint16_t)(r.x + 8), (uint16_t)(r.y + r.h - 4),
                          40, 3, C_ACCENT);
        }
    }

    /* 固定应用放不下：在网络按钮上方显示"更多"按钮 */
    if (g_pin_count > shown) {
        ui_rect mr = tb_more_rect();
        int hover = g_point_in(&mr, g_mx, g_my);
        hunt_add(mr);
        ui_round_panel(mr, 14, hover ? C_ACCENT : C_BORDER,
                       g_tb_overflow ? C_SEL : C_PANEL2);
        vbe_fill_rect((uint16_t)(mr.x + 18), (uint16_t)(mr.y + 22), 20, 3,
                      hover ? C_ACCENT3 : C_DIM);
        vbe_fill_rect((uint16_t)(mr.x + 18), (uint16_t)(mr.y + 29), 12, 3,
                      hover ? C_ACCENT3 : C_DIM);
    }

    int list[GUI_WIN_MAX];
    int nwin = tb_running_list(list, GUI_WIN_MAX);

    if (nwin > 0) {
        ui_hline(16, tb_win_base() - 10, 56, C_BORDER);
    }

    for (int s = 0; s < nwin; s++) {
        int i = list[s];
        ui_rect r = tb_win_rect(s);
        int hover = g_point_in(&r, g_mx, g_my);
        int active = (g_focus == i);
        int minimized = g_wins[i].minimized;

        hunt_add(r);
        ui_round_panel(r, 9,
                       active ? C_ACCENT : (hover ? C_ACCENT2 : C_BORDER),
                       minimized ? C_PANEL : (active ? C_SEL : C_PANEL2));

        int ix = r.x + 7;
        int iy = r.y + 5;
        uint32_t ic = minimized ? C_DIM : (active ? C_TEXT : C_DIM);
        tb_draw_app_icon(i, ix, iy, 16, ic);
    }

    /* 网络状态：绿点 = 链路就绪且已配置，红点 = 不可用 */
    ui_rect nr = tb_net_rect();
    int nr_hover = g_point_in(&nr, g_mx, g_my);
    hunt_add(nr);
    int netok = net_ready();
    uint32_t net_color = netok ? C_OK : C_ERR;
    ui_round_panel(nr, 14, nr_hover ? C_ACCENT : C_BORDER,
                   nr_hover ? C_HOVER : C_PANEL2);
    icon_net(nr.x + 14, nr.y + 8, 28, nr_hover ? C_ACCENT3 : C_DIM);
    ui_round_fill((ui_rect){ nr.x + 36, nr.y + 34, 9, 9 }, 4, net_color);

    ui_rect pr = tb_power_rect();
    int pr_hover = g_point_in(&pr, g_mx, g_my);
    hunt_add(pr);
    ui_round_panel(pr, 14, pr_hover ? C_ERR : C_BORDER,
                   pr_hover ? C_HOVER : C_PANEL2);
    icon_power(pr.x + 28, pr.y + 24, 10, pr_hover ? C_ERR : C_DIM);

    /* 任务栏时钟：两行显示，56px 宽刚好容纳两个数字，
     * 不会像单行 "HH:MM"（80px）那样超出任务栏宽度。 */
    char clock[6];
    gui_clock_text(clock);
    ui_rect clock_box = tb_clock_rect();
    int ck_hover = g_point_in(&clock_box, g_mx, g_my);
    hunt_add(clock_box);
    ui_round_panel(clock_box, 12,
                   (g_time_menu || ck_hover) ? C_ACCENT : C_BORDER,
                   (g_time_menu || ck_hover) ? C_HOVER : C_PANEL2);

    char hh[3];
    char mm[3];
    hh[0] = clock[0]; hh[1] = clock[1]; hh[2] = '\0';
    mm[0] = clock[3]; mm[1] = clock[4]; mm[2] = '\0';
    ui_text_transparent(clock_box.x + 12, clock_box.y + 1, hh, C_TEXT);
    ui_text_transparent(clock_box.x + 12, clock_box.y + 33, mm, C_DIM);
}

/* ---------- 窗口渲染 ---------- */

/* ---------- 搜索面板 ---------- */

void gui_search_panel_render(void) {
    ui_rect p = search_panel_rect();
    ui_shadow(p, 16, 10);
    ui_round_panel(p, 16, C_BORDER, C_MENU_BG);

    ui_text_transparent(p.x + 20, p.y + 12, "SEARCH", C_DIM);

    ui_rect box = { p.x + 16, p.y + 50, p.w - 32, 52 };
    ui_round_panel(box, 10, C_ACCENT, C_PANEL);
    if (g_search_len > 0) {
        gui_text_n(box.x + 14, box.y + 10, g_search, 24, C_TEXT);
    } else {
        ui_text_transparent(box.x + 14, box.y + 10, "type an app name", C_DIM);
    }
    if (((net_now_ms() / 500u) & 1u) == 0) {
        vbe_fill_rect((uint16_t)(box.x + 14 + g_search_len * 16),
                      (uint16_t)(box.y + 12), 2, 28, C_ACCENT);
    }

    int matches[GUI_APP_COUNT];
    int n = search_filter(matches);

    int y = p.y + 120;
    for (int i = 0; i < n; i++) {
        const struct GuiAppDef *a = &g_apps[matches[i]];
        ui_rect row = { p.x + 12, y, p.w - 24, 64 };
        int hover = g_point_in(&row, g_mx, g_my);
        hunt_add(row);
        if (i == g_search_sel) {
            ui_round_fill(row, 10, C_SEL);
        } else if (hover) {
            ui_round_fill(row, 10, C_HOVER);
        }

        ui_rect icon = { row.x + 12, row.y + 16, 32, 32 };
        if (a->win == WIN_FILES) {
            icon_folder(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->win == WIN_EDITOR) {
            icon_editor(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->win == WIN_TASKMGR) {
            icon_chart(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->win == WIN_CALC) {
            icon_calc(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->hidden) {
            icon_fault(icon.x, icon.y, 32, C_WARN);
        } else {
            icon_settings(icon.x, icon.y, 32, C_ACCENT3);
        }

        ui_text_transparent(row.x + 60, row.y + 4, LTR(a->name), C_TEXT);
        gui_text_n(row.x + 60, row.y + 34, LTR(a->desc), 20, C_DIM);
        y += 72;
    }

    if (n == 0) {
        ui_text_transparent(p.x + 24, p.y + 140, "No applications found.", C_DIM);
    }
}

void gui_user_menu_render(void) {
    ui_rect p = user_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, C_MENU_BG);

    const struct AuthUser *u = auth_current();
    gui_avatar(p.x + 38, p.y + 40, 22, u ? u->name : "?", C_AVATAR_A, C_AVATAR_B);
    ui_text_transparent(p.x + 70, p.y + 14, u ? u->name : "?", C_TEXT);

    char uid_text[24];
    int n = g_utoa((uint32_t)(u ? u->uid : 0), uid_text);
    g_copy(uid_text + n, "  (uid)", 8);

    ui_text_transparent(p.x + 70, p.y + 48, uid_text, C_DIM);
    ui_hline(p.x + 16, p.y + 86, p.w - 32, C_BORDER);

    ui_rect out = { p.x + 12, p.y + 98, p.w - 24, 54 };
    int hover = g_point_in(&out, g_mx, g_my);
    hunt_add(out);
    if (hover) ui_round_fill(out, 9, C_HOVER);
    ui_text_transparent(out.x + 16, out.y + 11, "Log Out", C_TEXT);
}

void gui_power_menu_render(void) {
    ui_rect p = power_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, C_MENU_BG);

    ui_text_transparent(p.x + 20, p.y + 14, LTR("POWER"), C_DIM);
    ui_hline(p.x + 16, p.y + 52, p.w - 32, C_BORDER);

    const char *items[4] = { "Restart", "Shut Down", "Exit to Terminal", "Cancel" };
    for (int i = 0; i < 4; i++) {
        ui_rect row = { p.x + 12, p.y + 60 + i * 42, p.w - 24, 38 };
        int hover = g_point_in(&row, g_mx, g_my);
        hunt_add(row);
        if (hover) ui_round_fill(row, 8, C_HOVER);
        ui_text_transparent(row.x + 16, row.y + 3, LTR(items[i]),
                            i == 1 ? (hover ? 0xFFB4B4 : C_ERR) : C_TEXT);
    }
}

/* 网络状态面板：链路、地址、速率与收发计数 */
static ui_rect net_menu_close_rect(const ui_rect *p) {
    return (ui_rect){ p->x + p->w - 132, p->y + 10, 116, 34 };
}

/* 飞行模式开关：状态行右侧，与 Close 按钮错开一行 */
static ui_rect flight_toggle_rect(const ui_rect *p) {
    return (ui_rect){ p->x + p->w - 228, p->y + 48, 88, 38 };
}

void gui_net_menu_render(void) {
    ui_rect p = net_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, C_MENU_BG);

    struct NetInfo *ni = net_info();
    int ready = net_ready();

    ui_text_transparent(p.x + 20, p.y + 14, "CONTROL CENTER", C_DIM);

    ui_rect rf = net_menu_close_rect(&p);
    int rf_hover = g_point_in(&rf, g_mx, g_my);
    gui_button(rf, "Close", rf_hover, 0);

    ui_hline(p.x + 16, p.y + 52, p.w - 32, C_BORDER);

    /* 状态行 */
    int flight = net_flight();
    uint32_t st_color = flight ? C_WARN : (ready ? C_OK : C_ERR);
    ui_round_fill((ui_rect){ p.x + 20, p.y + 66, 10, 10 }, 5, st_color);

    /* 限宽避免状态文字压到右侧的 Flight 开关上：按 16px 字符网格
     * 计算可用列数，ui_text 的平滑字形不会再越界。 */
    ui_rect fr = flight_toggle_rect(&p);
    int st_cols = (fr.x - (p.x + 40) - 12) / 16;
    if (st_cols < 4) st_cols = 4;
    const char *st_text = flight ? "Airplane" : (ready ? "Online" : "Offline");
    gui_text_n(p.x + 40, p.y + 56, st_text, st_cols, st_color);

    int fr_hover = g_point_in(&fr, g_mx, g_my);
    hunt_add(fr);
    gui_button(fr, flight ? "ON" : "OFF", fr_hover, flight);

    /* 音量：软件增益，控制所有 MP3 播放 */
    int vol = audio_volume();
    ui_text_transparent(p.x + 20, p.y + 96, "VOLUME", C_DIM);

    char vol_text[8];
    int vn = g_utoa((uint32_t)vol, vol_text);
    vol_text[vn++] = '%';
    vol_text[vn] = '\0';
    ui_text_transparent(p.x + p.w - 40 - g_strlen(vol_text) * 16,
                        p.y + 96, vol_text, C_TEXT);

    ui_rect vs = volume_slider_rect(&p);
    hunt_add(vs);
    ui_round_fill(vs, 7, C_PANEL3);
    int fillw = (vs.w * vol) / 100;
    if (fillw > 0) {
        ui_round_fill((ui_rect){ vs.x, vs.y, fillw, vs.h }, 7, C_ACCENT);
    }
    int kx = vs.x + fillw;
    if (kx < vs.x + 7) kx = vs.x + 7;
    if (kx > vs.x + vs.w - 7) kx = vs.x + vs.w - 7;
    ui_round_fill((ui_rect){ kx - 7, vs.y - 3, 14, 20 }, 7, C_ACCENT3);

    ui_hline(p.x + 16, p.y + 160, p.w - 32, C_BORDER);

    char line[96];
    char ip_text[16];
    int max_chars = (p.w - 40) / 16;

    if (ni) {
        /* 单列布局：两列在窄面板里会让右侧文本越过边界。
         * 全部字段一行一个，统一按面板宽度限字符数。 */
        int y = p.y + 182;

        net_format_ipv4(ni->ip, ip_text);
        g_copy(line, "IP      ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        net_format_ipv4(ni->netmask, ip_text);
        g_copy(line, "Mask    ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        net_format_ipv4(ni->gateway, ip_text);
        g_copy(line, "Gateway ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        net_format_ipv4(ni->dns, ip_text);
        g_copy(line, "DNS     ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        char mac[18];
        net_format_mac(ni->mac, mac);
        g_copy(line, "MAC     ", sizeof(line));
        g_copy(line + 8, mac, 18);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 32;

        line[0] = '\0';
        int pos = 0;
        g_copy(line, "Speed   ", 10); pos = 8;
        pos += g_utoa(ni->speed_mbps, line + pos);
        g_copy(line + pos, " Mbps", 8);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 32;

        /* 收发统计 */
        pos = 0;
        g_copy(line, "RX ", 4); pos = 3;
        pos += g_utoa(ni->rx_packets, line + pos);
        g_copy(line + pos, " pkts / ", 10); pos += 8;
        pos += g_utoa(ni->rx_bytes, line + pos);
        g_copy(line + pos, " B", 3);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 32;

        pos = 0;
        g_copy(line, "TX ", 4); pos = 3;
        pos += g_utoa(ni->tx_packets, line + pos);
        g_copy(line + pos, " pkts / ", 10); pos += 8;
        pos += g_utoa(ni->tx_bytes, line + pos);
        g_copy(line + pos, " B", 3);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 36;

        if (!ready) {
            ui_text_wrap(p.x + 20, y, p.w - 40, 34,
                         "No link. Check that an e1000 adapter is attached.",
                         C_WARN, 2);
        }
    } else {
        ui_text_wrap(p.x + 20, p.y + 96, p.w - 40, 34,
                     "Network stack has not been initialized yet.", C_DIM, 2);
    }
}
void gui_clock_badge(void) {
    ui_rect badge = { gW - 116, 16, 100, 44 };
    ui_round_panel(badge, 12, C_BORDER, C_MENU_BG);
    char clock[6];
    gui_clock_text(clock);
    gui_text_center(clock, badge.x + badge.w / 2, badge.y + 6, C_TEXT);
}

void gui_taskbar_click(int x, int y) {
    ui_rect av = tb_avatar_rect();
    ui_rect sr = tb_search_rect();
    ui_rect pr = tb_power_rect();
    ui_rect nr = tb_net_rect();

    if (g_point_in(&av, x, y)) {
        g_user_menu = !g_user_menu;
        g_power_menu = 0;
        g_net_menu = 0;
        g_time_menu = 0;
        return;
    }
    if (g_point_in(&sr, x, y)) {
        g_search_open = !g_search_open;
        if (g_search_open) {
            g_search_len = 0;
            g_search[0] = '\0';
            g_search_sel = 0;
        }
        g_user_menu = 0;
        g_power_menu = 0;
        g_net_menu = 0;
        g_time_menu = 0;
        return;
    }
    if (g_point_in(&nr, x, y)) {
        g_net_menu = !g_net_menu;
        g_user_menu = 0;
        g_power_menu = 0;
        g_time_menu = 0;
        return;
    }
    if (g_point_in(&pr, x, y)) {
        g_power_menu = !g_power_menu;
        g_user_menu = 0;
        g_net_menu = 0;
        g_time_menu = 0;
        return;
    }

    /* 点击时间卡片：日期/星期/月历/带秒时间 */
    ui_rect ck = tb_clock_rect();
    if (g_point_in(&ck, x, y)) {
        g_time_menu = !g_time_menu;
        g_user_menu = 0;
        g_power_menu = 0;
        g_net_menu = 0;
        g_search_open = 0;
        return;
    }

    /* 固定应用：按下只记录候选，松开时才启动（区分点击与拖动排序） */
    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    for (int s = 0; s < shown; s++) {
        ui_rect r = tb_pin_rect(s);
        if (g_point_in(&r, x, y)) {
            gui_taskbar_drag_begin(x, y);
            return;
        }
    }

    if (g_pin_count > shown) {
        ui_rect mr = tb_more_rect();
        if (g_point_in(&mr, x, y)) {
            g_tb_overflow = !g_tb_overflow;
            g_time_menu = 0;
            return;
        }
    }

    int list[GUI_WIN_MAX];
    int nwin = tb_running_list(list, GUI_WIN_MAX);
    for (int s = 0; s < nwin; s++) {
        int i = list[s];
        ui_rect r = tb_win_rect(s);
        if (g_point_in(&r, x, y)) {
            if (g_wins[i].minimized) win_restore_from_min(i);
            win_focus(i);
            return;
        }
    }
}

void gui_user_menu_click(int x, int y) {
    ui_rect p = user_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_user_menu = 0;
        return;
    }

    ui_rect out = { p.x + 12, p.y + 98, p.w - 24, 54 };
    if (g_point_in(&out, x, y)) {
        g_pending_action = ACT_LOGOUT;
        g_user_menu = 0;
    }
}

void gui_power_menu_click(int x, int y) {
    ui_rect p = power_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_power_menu = 0;
        return;
    }

    for (int i = 0; i < 4; i++) {
        ui_rect row = { p.x + 12, p.y + 60 + i * 42, p.w - 24, 38 };
        if (!g_point_in(&row, x, y)) continue;

        if (i == 0) g_pending_action = ACT_REBOOT;
        else if (i == 1) g_pending_action = ACT_SHUTDOWN;
        else if (i == 2) g_pending_action = ACT_TTY;
        g_power_menu = 0;
        return;
    }
}

void gui_net_menu_click(int x, int y) {
    ui_rect p = net_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_net_menu = 0;
        return;
    }

    ui_rect rf = net_menu_close_rect(&p);
    if (g_point_in(&rf, x, y)) {
        g_net_menu = 0;
        return;
    }

    ui_rect fr = flight_toggle_rect(&p);
    if (g_point_in(&fr, x, y)) {
        net_set_flight(!net_flight());
        dirty_add_padded(&p, 18);
        return;
    }

    ui_rect vs = volume_slider_rect(&p);
    ui_rect hit = { vs.x, vs.y - 12, vs.w, vs.h + 24 };
    if (g_point_in(&hit, x, y)) {
        int vol = (x - vs.x) * 100 / vs.w;
        audio_set_volume(vol);
        dirty_add_padded(&p, 18);
        return;
    }
}

void gui_search_click(int x, int y) {
    ui_rect p = search_panel_rect();
    if (!g_point_in(&p, x, y)) {
        g_search_open = 0;
        return;
    }

    int matches[GUI_APP_COUNT];
    int n = search_filter(matches);

    int row_y = p.y + 120;
    for (int i = 0; i < n; i++) {
        ui_rect row = { p.x + 12, row_y + i * 72, p.w - 24, 64 };
        if (g_point_in(&row, x, y)) {
            launch_app(g_apps[matches[i]].win);
            return;
        }
    }
}

/* ============================================================
 * 时间菜单：日期、星期、月历与带秒时间
 * ============================================================ */
#define TIME_MENU_W 420
#define TIME_MENU_H 400

ui_rect time_menu_rect(void) {
    int y = gH - TIME_MENU_H - 24;
    if (y < 16) y = 16;
    return (ui_rect){ GUI_TASKBAR_W + 8, y, TIME_MENU_W, TIME_MENU_H };
}

/* 0=Sunday 的星期计算（Howard Hinnant 公式） */
static int tm_weekday(int y, int m, int d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    if (w < 0) w += 7;
    return w;
}

static int tm_days_in_month(int y, int m) {
    static const int dm[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

static void tm_put2(char *out, int v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

void gui_time_menu_render(void) {
    ui_rect p = time_menu_rect();
    ui_shadow(p, 16, 10);
    ui_round_panel(p, 16, C_BORDER, C_MENU_BG);

    ui_text_transparent(p.x + 20, p.y + 14, "DATE & TIME", C_DIM);

    int uh = 0, um = 0, us = 0;
    rtc_get_utc(&uh, &um, &us);

    int tz = system_cfg_timezone();
    int total = uh * 60 + um + tz * 60;
    int day_shift = 0;
    while (total < 0) { total += 1440; day_shift--; }
    while (total >= 1440) { total -= 1440; day_shift++; }
    int lh = total / 60;
    int lm = total % 60;

    int year = 2026, month = 1, day = 1, wd = 0;
    if (!rtc_get_date(&year, &month, &day, &wd)) {
        year = 2026; month = 1; day = 1;
    }
    if (day_shift != 0) {
        day += day_shift;
        if (day < 1) {
            month--;
            if (month < 1) { month = 12; year--; }
            day = tm_days_in_month(year, month);
        } else if (day > tm_days_in_month(year, month)) {
            day = 1;
            month++;
            if (month > 12) { month = 1; year++; }
        }
        wd = tm_weekday(year, month, day);
    }

    char tbuf[10];
    tm_put2(tbuf, lh); tbuf[2] = ':';
    tm_put2(tbuf + 3, lm); tbuf[5] = ':';
    tm_put2(tbuf + 6, us); tbuf[8] = '\0';
    ui_text_transparent(p.x + 20, p.y + 46, tbuf, C_TEXT);

    /* 日期：YYYY-MM-DD 与星期 */
    char yb[8], mb[4], db[4];
    int yn = g_utoa((uint32_t)year, yb);
    int mn = g_utoa((uint32_t)month, mb);
    int dn = g_utoa((uint32_t)day, db);
    char dbuf[24];
    int dp = 0;
    for (int i = 0; i < yn; i++) dbuf[dp++] = yb[i];
    dbuf[dp++] = '-';
    if (mn < 2) dbuf[dp++] = '0';
    for (int i = 0; i < mn; i++) dbuf[dp++] = mb[i];
    dbuf[dp++] = '-';
    if (dn < 2) dbuf[dp++] = '0';
    for (int i = 0; i < dn; i++) dbuf[dp++] = db[i];
    dbuf[dp] = '\0';

    static const char *wd_names[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    ui_text_transparent(p.x + 210, p.y + 50, dbuf, C_DIM);
    ui_text_transparent(p.x + 210, p.y + 82, wd_names[wd % 7], C_DIM);

    /* 月历：7 列，首行星期缩写 */
    static const char *wd_short[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    int cols = 7;
    int cw = (p.w - 40) / cols;
    int gy = p.y + 124;

    for (int c = 0; c < cols; c++) {
        int tx = p.x + 20 + c * cw + (cw - 32) / 2;
        ui_text_transparent(tx, gy, wd_short[c], C_DIM);
    }
    gy += 36;

    int first = tm_weekday(year, month, 1);
    int dim = tm_days_in_month(year, month);

    for (int i = 0; i < first + dim; i++) {
        int c = i % 7;
        int r = i / 7;
        int x = p.x + 20 + c * cw;
        int y = gy + r * 34;

        if (i < first) continue;
        int dnum = i - first + 1;

        char nb[4];
        int nn = g_utoa((uint32_t)dnum, nb);
        nb[nn] = '\0';

        int tw = nn * 16;
        if (dnum == day) {
            ui_round_fill((ui_rect){ x + (cw - 40) / 2, y - 4, 40, 32 }, 8, C_SEL);
        }
        ui_text_transparent(x + (cw - tw) / 2, y, nb,
                            dnum == day ? C_ACCENT3 : C_TEXT);
    }
}

void gui_time_menu_click(int x, int y) {
    ui_rect p = time_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_time_menu = 0;
        return;
    }
    /* 面板本身只读，不需要行交互 */
}

/* ============================================================
 * 溢出侧拉菜单：固定应用放不下、运行窗口放不下时使用
 * ============================================================ */
#define TB_OVF_W 320
#define TB_OVF_ROW 54

static int tb_overflow_count(void) {
    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    int hidden_pin = g_pin_count > shown ? g_pin_count - shown : 0;
    int run[GUI_WIN_MAX];
    int rn = tb_running_list(run, GUI_WIN_MAX);
    return hidden_pin + rn;
}

ui_rect tb_overflow_rect(void) {
    int rows = tb_overflow_count();
    if (rows < 1) rows = 1;
    int h = 56 + rows * TB_OVF_ROW + 12;
    if (h > 420) h = 420;
    int y = tb_more_rect().y + 48 - h;
    if (y < 16) y = 16;
    if (y + h > gH - 16) y = gH - 16 - h;
    return (ui_rect){ GUI_TASKBAR_W + 8, y, TB_OVF_W, h };
}

void gui_tb_overflow_render(void) {
    ui_rect p = tb_overflow_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, C_MENU_BG);

    ui_text_transparent(p.x + 18, p.y + 12, "MORE APPS", C_DIM);

    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    int run[GUI_WIN_MAX];
    int rn = tb_running_list(run, GUI_WIN_MAX);

    int row = 0;
    int y = p.y + 50;

    for (int i = shown; i < g_pin_count; i++) {
        ui_rect r = { p.x + 10, y, p.w - 20, TB_OVF_ROW - 6 };
        int hover = g_point_in(&r, g_mx, g_my);
        if (hover) ui_round_fill(r, 10, C_HOVER);
        tb_draw_app_icon(g_pin[i], r.x + 12, r.y + 10, 28,
                         g_wins[g_pin[i]].open ? C_ACCENT3 : C_DIM);
        int ai = tb_app_index(g_pin[i]);
        gui_text_n(r.x + 56, r.y + 12, ai >= 0 ? g_apps[ai].name : "App",
                   10, C_TEXT);
        y += TB_OVF_ROW;
        row++;
    }

    for (int s = 0; s < rn && row < 6; s++) {
        int win = run[s];
        ui_rect r = { p.x + 10, y, p.w - 20, TB_OVF_ROW - 6 };
        int hover = g_point_in(&r, g_mx, g_my);
        if (hover) ui_round_fill(r, 10, C_HOVER);
        tb_draw_app_icon(win, r.x + 12, r.y + 10, 28, C_DIM);
        int ai = tb_app_index(win);
        const char *nm = ai >= 0 ? g_apps[ai].name : "Window";
        gui_text_n(r.x + 56, r.y + 12, nm, 10, C_TEXT);
        y += TB_OVF_ROW;
        row++;
    }
}

void gui_tb_overflow_click(int x, int y) {
    ui_rect p = tb_overflow_rect();
    if (!g_point_in(&p, x, y)) {
        g_tb_overflow = 0;
        return;
    }

    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    int run[GUI_WIN_MAX];
    int rn = tb_running_list(run, GUI_WIN_MAX);

    int row = 0;
    int ry = p.y + 50;
    for (int i = shown; i < g_pin_count; i++, row++, ry += TB_OVF_ROW) {
        ui_rect r = { p.x + 10, ry, p.w - 20, TB_OVF_ROW - 6 };
        if (g_point_in(&r, x, y)) {
            g_tb_overflow = 0;
            launch_app(g_pin[i]);
            return;
        }
    }
    for (int s = 0; s < rn && row < 6; s++, row++, ry += TB_OVF_ROW) {
        ui_rect r = { p.x + 10, ry, p.w - 20, TB_OVF_ROW - 6 };
        if (g_point_in(&r, x, y)) {
            int w = run[s];
            g_tb_overflow = 0;
            if (g_wins[w].minimized) win_restore_from_min(w);
            win_focus(w);
            return;
        }
    }
}

/* ============================================================
 * 任务栏右键菜单
 * ============================================================ */
static int g_tb_menu_x = GUI_TASKBAR_W + 8;
static int g_tb_menu_y = 200;

ui_rect tb_menu_rect(void) {
    int h = (g_tb_menu_kind == 1) ? 108 : 152;
    int w = 400;
    int x = g_tb_menu_x;
    int y = g_tb_menu_y;
    if (x + w > gW - 8) x = gW - 8 - w;
    if (y + h > gH - 8) y = gH - 8 - h;
    if (x < GUI_TASKBAR_W + 4) x = GUI_TASKBAR_W + 4;
    if (y < 8) y = 8;
    return (ui_rect){ x, y, w, h };
}

void gui_tb_menu_render(void) {
    ui_rect p = tb_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, C_MENU_BG);

    if (g_tb_menu_kind == 1) {
        static const char *items[2] = { "Open", "Unpin from taskbar" };
        for (int i = 0; i < 2; i++) {
            ui_rect row = { p.x + 8, p.y + 8 + i * 46, p.w - 16, 42 };
            if (g_point_in(&row, g_mx, g_my)) ui_round_fill(row, 10, C_HOVER);
            gui_text_n(row.x + 16, row.y + 5, items[i], 16,
                       i == 1 ? C_WARN : C_TEXT);
        }
    } else {
        static const char *items[3] = { "Close window", "End task", "Pin to taskbar" };
        for (int i = 0; i < 3; i++) {
            ui_rect row = { p.x + 8, p.y + 8 + i * 46, p.w - 16, 42 };
            if (g_point_in(&row, g_mx, g_my)) ui_round_fill(row, 10, C_HOVER);
            gui_text_n(row.x + 16, row.y + 5, items[i], 16,
                       i == 0 ? C_ERR : C_TEXT);
        }
    }
}

int gui_tb_menu_click(int x, int y) {
    if (!g_tb_menu) return 0;

    ui_rect p = tb_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_tb_menu = 0;
        return 1;
    }

    if (g_tb_menu_kind == 1) {
        for (int i = 0; i < 2; i++) {
            ui_rect row = { p.x + 8, p.y + 8 + i * 46, p.w - 16, 42 };
            if (!g_point_in(&row, x, y)) continue;
            int win = g_pin[g_tb_menu_slot < g_pin_count ? g_tb_menu_slot : 0];
            g_tb_menu = 0;
            if (i == 0) {
                launch_app(win);
            } else {
                gui_taskbar_pin_remove(win);
            }
            return 1;
        }
    } else {
        for (int i = 0; i < 3; i++) {
            ui_rect row = { p.x + 8, p.y + 8 + i * 46, p.w - 16, 42 };
            if (!g_point_in(&row, x, y)) continue;
            int win = g_tb_menu_win;
            g_tb_menu = 0;
            if (i == 0 || i == 1) {
                if (win >= 0 && win < GUI_WIN_MAX) win_close(win);
            } else {
                gui_taskbar_pin_add(win);
            }
            return 1;
        }
    }
    return 1;
}

void gui_tb_menu_close(void) {
    g_tb_menu = 0;
}

void gui_taskbar_rclick(int x, int y) {
    if (x >= GUI_TASKBAR_W) return;
    if (g_time_menu || g_tb_overflow) {
        g_time_menu = 0;
        g_tb_overflow = 0;
        return;
    }

    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    for (int s = 0; s < shown; s++) {
        ui_rect r = tb_pin_rect(s);
        if (!g_point_in(&r, x, y)) continue;
        g_tb_menu = 1;
        g_tb_menu_kind = 1;
        g_tb_menu_slot = s;
        g_tb_menu_win = g_pin[s];
        g_tb_menu_x = GUI_TASKBAR_W + 8;
        g_tb_menu_y = y;
        return;
    }

    int run[GUI_WIN_MAX];
    int rn = tb_running_list(run, GUI_WIN_MAX);
    for (int s = 0; s < rn; s++) {
        ui_rect r = tb_win_rect(s);
        if (!g_point_in(&r, x, y)) continue;
        g_tb_menu = 1;
        g_tb_menu_kind = 2;
        g_tb_menu_slot = -1;
        g_tb_menu_win = run[s];
        g_tb_menu_x = GUI_TASKBAR_W + 8;
        g_tb_menu_y = y;
        return;
    }

    g_tb_menu = 0;
}

/* ============================================================
 * 固定图标拖动排序
 * ============================================================ */
void gui_taskbar_drag_begin(int x, int y) {
    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    for (int s = 0; s < shown; s++) {
        ui_rect r = tb_pin_rect(s);
        if (g_point_in(&r, x, y)) {
            g_drag_slot = s;
            g_drag_sy = y;
            g_drag_active = 0;
            return;
        }
    }
}

void gui_taskbar_drag_update(int x, int y) {
    (void)x;
    if (g_drag_slot < 0) return;

    if (!g_drag_active) {
        int dy = y - g_drag_sy;
        if (dy > 8 || dy < -8) g_drag_active = 1;
    }
    if (!g_drag_active) return;

    int shown = g_pin_count < tb_pin_slots() ? g_pin_count : tb_pin_slots();
    int target = (y - TB_PIN_Y0) / TB_PIN_STEP;
    if (target < 0) target = 0;
    if (target >= shown) target = shown - 1;
    if (target >= g_pin_count) target = g_pin_count - 1;
    if (target == g_drag_slot) return;

    int win = g_pin[g_drag_slot];
    if (target > g_drag_slot) {
        for (int i = g_drag_slot; i < target; i++) g_pin[i] = g_pin[i + 1];
    } else {
        for (int i = g_drag_slot; i > target; i--) g_pin[i] = g_pin[i - 1];
    }
    g_pin[target] = win;
    g_drag_slot = target;
    tb_pin_save();
}

void gui_taskbar_drag_end(void) {
    if (g_drag_slot >= 0 && !g_drag_active) {
        if (g_drag_slot < g_pin_count) launch_app(g_pin[g_drag_slot]);
    }
    g_drag_slot = -1;
    g_drag_active = 0;
}