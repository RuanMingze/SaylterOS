/* ============================================================
 * gui_internal.h - 图形会话内部接口
 * ------------------------------------------------------------
 * gui.c 拆成四个编译单元后，共享状态与跨模块函数在这里声明：
 *
 *   gui_core.c      内核原语：文字/图标/光标/鼠标/键盘/IME/背景/时钟
 *   gui_dwm.c       窗口管理：z 序、焦点、拖动/缩放、窗口装饰、
 *                   损伤合并与最终呈现（compositor）
 *   gui_taskbar.c   任务栏：布局、时钟、搜索、开始/电源/网络菜单
 *   gui.c           会话流程（启动页/登录/桌面循环）与内置应用
 *
 * 依赖方向是单向的：core <- dwm <- taskbar <- gui.c，
 * gui.c 与 taskbar 两侧通过本头文件里的回调进入内置应用。
 * ============================================================ */

#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include <stdint.h>

#include "vbe.h"
#include "ui.h"
#include "tty.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "auth.h"
#include "system_cfg.h"
#include "acpi.h"
#include "acpica_glue.h"
#include "reboot.h"
#include "net.h"
#include "gui.h"
#include "debug.h"
#include "shell.h"
#include "html.h"
#include "mem.h"
#include "task.h"
#include "smp.h"
#include "vm.h"
#include "ata.h"
#include "ime.h"
#include "unicode.h"
#include "slyertype.h"
#include "lang.h"
#include "gui_slyz.h"
#include "image.h"
#include "audio.h"
#include "blk.h"
#include "gui_dialog.h"

/* ============================================================
 * 基础常量
 * ============================================================ */

#define GUI_TASKBAR_W 88
#define GUI_WIN_MAX   10
#define GUI_APP_COUNT 8
#define GUI_FM_MAX    128
#define GUI_ED_MAX    65536
#define GUI_CURSOR_W  16
#define GUI_CURSOR_H  24

#define GUI_DIRTY_MAX 12
#define GUI_SHADOW_PAD 20
#define GUI_HUNT_MAX  192

/* 界面文案统一走语言包：未收录的条目自动回退英文。 */
#define LTR(s) lang_tr(s)

enum {
    WIN_FILES = 0,
    WIN_EDITOR = 1,
    WIN_SETTINGS = 2,
    WIN_TERM = 3,
    WIN_BROWSER = 4,
    WIN_SLYZ = 5,
    WIN_TASKMGR = 6,
    WIN_CALC = 7,
    WIN_MEDIA = 8,
    /* 原生预装应用窗口：由 GuiExternalHost 驱动（如 VizCormin）。 */
    WIN_EXTERNAL = 9
};

enum {
    ACT_NONE = -1,
    ACT_LOGOUT = 0,
    ACT_REBOOT = 1,
    ACT_SHUTDOWN = 2,
    ACT_TTY = 3
};

/* ============================================================
 * 主题调色板
 * ------------------------------------------------------------
 * 所有界面颜色都从 g_pal 读取，切换主题只需重填这张表并重绘。
 * 提供两套出厂配色：
 *   GUI_THEME_DARK_ORANGE  科技橙（深色）
 *   GUI_THEME_LIGHT_GREEN  清晰绿（浅色）
 * 色彩定义在 gui_core.c 的 gui_theme_apply()。
 * ============================================================ */

#define GUI_THEME_DARK_ORANGE 0
#define GUI_THEME_LIGHT_GREEN 1

struct GuiPalette {
    uint32_t bg;
    uint32_t bg2;
    uint32_t panel;
    uint32_t panel2;
    uint32_t panel3;
    uint32_t border;
    uint32_t text;
    uint32_t dim;
    uint32_t accent;
    uint32_t accent2;
    uint32_t accent3;
    uint32_t ok;
    uint32_t err;
    uint32_t warn;

    uint32_t avatar_a;
    uint32_t avatar_b;
    uint32_t avatar_c;
    uint32_t avatar_d;

    /* 任务栏 / 窗口 / 交互状态 */
    uint32_t bar_top;
    uint32_t bar_bottom;
    uint32_t hover;
    uint32_t selected;
    uint32_t win_body;
    uint32_t title;
    uint32_t title_active;
    uint32_t shadow;
    uint32_t menu_bg;
    uint32_t tip_bg;
};

extern struct GuiPalette g_pal;

/* 重填调色板并同步 ui_theme。id 为 GUI_THEME_*；调用方负责标脏重绘。 */
void gui_theme_apply(int id);

/* 按 SLE_THEME_* 槽位返回当前主题颜色（0xRRGGBB）。 */
uint32_t gui_theme_color(int slot);

/* ============================================================
 * 界面动画（设置 → 动画，默认关闭）
 * ------------------------------------------------------------
 * 淡入淡出的做法：绘制前景之前把目标矩形下的像素存进一块复用
 * 缓冲，前景画完后按 alpha 把两份内容混合回去。只在动画开启且
 * 时间窗内才做，平时完全零开销。
 * ============================================================ */
int  gui_anim_enabled(void);
void gui_anim_touch(void);          /* 开始一段动画，主循环据此整帧重绘 */
int  gui_anim_active(void);         /* 是否仍在动画时间窗内 */
uint8_t gui_anim_alpha(uint32_t start_ms, uint32_t dur_ms);

/* begin 成功返回缓冲指针；end 把保存的底图与当前前景按 alpha 混合。
 * 同一时刻只支持一层淡入（菜单/对话框都是互斥的模态层）。 */
uint32_t *gui_anim_begin(ui_rect r);
void      gui_anim_end(uint8_t alpha);

#define C_BG       (g_pal.bg)
#define C_BG2      (g_pal.bg2)
#define C_PANEL    (g_pal.panel)
#define C_PANEL2   (g_pal.panel2)
#define C_PANEL3   (g_pal.panel3)
#define C_BORDER   (g_pal.border)
#define C_TEXT     (g_pal.text)
#define C_DIM      (g_pal.dim)
#define C_ACCENT   (g_pal.accent)
#define C_ACCENT2  (g_pal.accent2)
#define C_ACCENT3  (g_pal.accent3)
#define C_OK       (g_pal.ok)
#define C_ERR      (g_pal.err)
#define C_WARN     (g_pal.warn)

#define C_AVATAR_A (g_pal.avatar_a)
#define C_AVATAR_B (g_pal.avatar_b)
#define C_AVATAR_C (g_pal.avatar_c)
#define C_AVATAR_D (g_pal.avatar_d)

/* 常用交互色快捷别名 */
#define C_HOVER    (g_pal.hover)
#define C_SEL      (g_pal.selected)
#define C_WIN_BG   (g_pal.win_body)
#define C_TITLE    (g_pal.title)
#define C_MENU_BG  (g_pal.menu_bg)
#define C_TIP_BG   (g_pal.tip_bg)
#define C_BAR_TOP  (g_pal.bar_top)
#define C_BAR_BOT  (g_pal.bar_bottom)

/* ============================================================
 * 共享结构
 * ============================================================ */

struct GuiWindow {
    int     open;
    ui_rect r;
    int     z;
    int     maximized;
    int     minimized;
    ui_rect restore;      /* 最大化前的位置，用于还原 */
    uint32_t anim_t0;     /* 打开/还原时刻，用于淡入动画 */
};

struct GuiAppDef {
    const char *name;
    const char *desc;
    int         win;
    int         hidden;   /* 1 = 不在任务栏显示，只在搜索面板出现 */
};

/* ============================================================
 * core 状态（gui_core.c 定义）
 * ============================================================ */

extern int      gW;
extern int      gH;
extern ui_theme g_theme;

extern int g_mx;
extern int g_my;
extern int g_mdown;
extern int g_mdown_r;
extern int g_mclicked;
extern int g_rclicked;
extern int g_mwheel;
extern int g_mreleased;
extern int g_mmoved;

extern int g_cursor_x;
extern int g_cursor_y;

/* 上一帧登记的 hover 控件矩形（gui_dwm 的局部重绘据此擦除旧高亮） */
extern ui_rect g_hunt_prev[GUI_HUNT_MAX];
extern int    g_hunt_prev_count;

/* 输入法候选窗位置（编辑器/终端会读取） */
extern int g_ime_ax;
extern int g_ime_ay;

/* CMOS 实时时钟（设置页调整时区时使用） */
int rtc_get_utc(int *hour, int *minute, int *second);
int rtc_set_utc(int hour, int minute);
/* 读取 RTC 日期：year 为 2000+ 的年份，weekday 0=Sunday。 */
int rtc_get_date(int *year, int *month, int *day, int *weekday);

/* ============================================================
 * dwm 状态（gui_dwm.c 定义）
 * ============================================================ */

extern struct GuiWindow g_wins[GUI_WIN_MAX];
extern int g_z_counter;
extern int g_focus;
extern int g_dragging;
extern int g_drag_dx;
extern int g_drag_dy;
extern int g_resizing;
extern int g_resize_edge;
extern ui_rect g_resize_start;
extern int g_resize_mx0;
extern int g_resize_my0;

extern const GuiExternalHost *g_ext_host;
extern int    g_ext_space_w;
extern int    g_ext_space_h;
extern char   g_ext_title[32];
extern ui_rect g_ext_content;
extern int    g_ext_content_valid;

extern ui_rect g_dirty[GUI_DIRTY_MAX];
extern int    g_dirty_count;
extern int    g_dirty_full;
extern int    g_redraw_all_pending;

/* ============================================================
 * taskbar / menu 状态（gui_taskbar.c 定义）
 * ============================================================ */

extern const struct GuiAppDef g_apps[GUI_APP_COUNT];

extern int  g_search_open;
extern char g_search[64];
extern int  g_search_len;
extern int  g_search_sel;

extern int g_user_menu;
extern int g_power_menu;
extern int g_net_menu;
extern int g_pending_action;

/* ============================================================
 * 内置应用状态（gui.c 定义，dwm/taskbar 读取）
 * ============================================================ */

extern int g_fm_modal;
extern char g_fm_msg[96];
extern uint32_t g_fm_msg_until;
extern int g_ed_ask_save;
extern int g_ed_drag_sel;
extern char g_ed_msg[96];
extern uint32_t g_ed_msg_until;
extern char g_set_msg[96];
extern uint32_t g_set_msg_until;
extern int g_desk_menu;
extern int g_desk_prompt;
extern int g_desk_click_idx;
extern int g_desk_count;

/* ============================================================
 * core 原语（gui_core.c）
 * ============================================================ */

int  g_strlen(const char *s);
int  g_strcmp(const char *a, const char *b);
int  g_lower(int c);
int  g_strcasecmp(const char *a, const char *b);
void g_copy(char *dst, const char *src, int cap);
int  g_utoa(uint32_t v, char *out);
void g_itoa_pad(uint32_t v, char *out, int width);
int  g_contains_nocase(const char *haystack, const char *needle);
int  g_point_in(const ui_rect *r, int x, int y);
void gui_delay_ms(uint32_t ms);

void gui_cursor_draw(int x, int y);
void gui_cursor_erase(void);
void gui_mouse_update(void);
void gui_kb_begin(void);
void gui_kb_end(void);
char gui_kb_read(void);

void gui_text_center(const char *s, int cx, int y, uint32_t color);
void gui_text_n(int x, int y, const char *s, int max_chars, uint32_t color);
int  gui_text_wrap(int x, int y, int max_w, const char *s, uint32_t color, int max_lines);

int  gui_ime_active(void);
void gui_ime_layout(void);
void gui_ime_render(void);
ui_rect gui_ime_rect(void);

void gui_ring(int cx, int cy, int radius, int thickness, uint32_t color);
void gui_avatar(int cx, int cy, int radius, const char *name, uint32_t c1, uint32_t c2);
void gui_button(ui_rect r, const char *label, int hover, int primary);

void hunt_reset(void);
void hunt_add(ui_rect r);
void hunt_snapshot(void);

void icon_folder(int x, int y, int s, uint32_t color);
void icon_file(int x, int y, int s, uint32_t color);
void icon_editor(int x, int y, int s, uint32_t color);
void icon_search(int cx, int cy, int radius, uint32_t color);
void icon_settings(int x, int y, int s, uint32_t color);
void icon_power(int cx, int cy, int radius, uint32_t color);
void icon_terminal(int x, int y, int s, uint32_t color);
void icon_globe(int x, int y, int s, uint32_t color);
void icon_chart(int x, int y, int s, uint32_t color);
void icon_fault(int x, int y, int s, uint32_t color);
void icon_calc(int x, int y, int s, uint32_t color);
void icon_net(int x, int y, int s, uint32_t color);
void icon_close(int x, int y, int s, uint32_t color);
void icon_minimize(int x, int y, int s, uint32_t color);
void icon_maximize(int x, int y, int s, uint32_t color);
void icon_restore(int x, int y, int s, uint32_t color);

void gui_background(void);
void gui_clock_local(int *hour, int *minute);
void gui_clock_text(char out[6]);
void gui_tz_text(char *out, int cap);

/* ============================================================
 * dwm（gui_dwm.c）
 * ============================================================ */

ui_rect win_default_rect(int id);
void win_open(int id);
void win_focus(int id);
void win_close(int id);
void win_minimize(int id);
void win_restore_from_min(int id);
void win_toggle_max(int id);

ui_rect win_content_rect(const ui_rect *r);
ui_rect win_close_rect(const ui_rect *r);
ui_rect win_max_btn_rect(const ui_rect *r);
ui_rect win_min_btn_rect(const ui_rect *r);
ui_rect win_grip_rect(const ui_rect *r);
int  win_resize_edge_at(const ui_rect *r, int x, int y, int maximized);
int  win_topmost_at(int x, int y);

void gui_window_frame(int id, const ui_theme *t);
void gui_window_click(int id, int x, int y);
void gui_desktop_render(void);

void dirty_reset(void);
void dirty_all(void);
void dirty_add(int x, int y, int w, int h);
void dirty_add_rect(const ui_rect *r);
void dirty_add_padded(const ui_rect *r, int pad);
void dirty_compact(void);
void dirty_hover_scan(int old_x, int old_y, int x, int y);
void dirty_window(int id);
void dirty_surface_at(int x, int y);
int  dirty_is_full(void);
int  dirty_rect_count(void);
void dirty_rect_get(int index, ui_rect *out);
void dirty_swap_all(int old_cx, int old_cy);
void gui_render_dirty_surfaces(void);
uint32_t gui_state_sig(void);
void dirty_key_surface(void);
void dirty_blink_surface(void);

void gui_present_all(void);
void gui_present_cursor(void);
void gui_present_dirty(void);

/* 最近一次呈现的分段耗时（微秒）与脏区规模。
 * 只用于串口性能诊断：主循环限频读取，不影响正常绘制路径。 */
extern uint32_t g_perf_render_us;
extern uint32_t g_perf_blit_us;
extern uint32_t g_perf_dirty_rects;
extern uint32_t g_perf_dirty_px;
extern int      g_perf_full;

/* ============================================================
 * taskbar（gui_taskbar.c）
 * ============================================================ */

ui_rect tb_clock_rect(void);
ui_rect tb_net_rect(void);
ui_rect search_panel_rect(void);
ui_rect user_menu_rect(void);
ui_rect power_menu_rect(void);
ui_rect net_menu_rect(void);

int  gui_visible_app_count(void);
int  search_filter(int out[GUI_APP_COUNT]);
void launch_app(int win);
int  gui_hover_id(void);
int  gui_hover_rect(int id, ui_rect *out);

/* ---- 任务栏固定应用与新增面板 ---- */
void gui_taskbar_prefs_load(void);
int  gui_taskbar_pin_count(void);
int  gui_taskbar_pin_win(int slot);
int  gui_taskbar_is_pinned(int win);
void gui_taskbar_pin_add(int win);
void gui_taskbar_pin_remove(int win);
int  tb_pin_slots(void);
ui_rect tb_pin_rect(int slot);
ui_rect tb_more_rect(void);

extern int g_time_menu;
extern int g_tb_overflow;
extern int g_tb_menu;

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

void gui_taskbar_render(void);
void gui_search_panel_render(void);
void gui_user_menu_render(void);
void gui_power_menu_render(void);
void gui_net_menu_render(void);
void gui_clock_badge(void);

void gui_taskbar_click(int x, int y);
void gui_user_menu_click(int x, int y);
void gui_power_menu_click(int x, int y);
void gui_net_menu_click(int x, int y);
void gui_search_click(int x, int y);

/* ============================================================
 * 内置应用（gui.c 提供给 dwm / taskbar）
 * ============================================================ */

void gui_desk_render(void);
void gui_desk_refresh(void);
void gui_files_open(const char *path);
void gui_editor_open(const char *path);
int  gui_editor_is_open(void);
void gui_term_reset(void);
void gui_browser_reset(void);
int  gui_tm_sample(void);
void gui_slyz_req_size(int *w, int *h);

/* dwm 把内容区交给内置应用：渲染、点击、关闭确认都走这三个入口，
 * 窗口装饰与内置应用实现彻底解耦。 */
void gui_app_content_render(int id, const ui_theme *t, ui_rect c);
void gui_app_content_click(int id, ui_rect c, int x, int y);
void gui_window_request_close(int id);

#endif /* GUI_INTERNAL_H */
