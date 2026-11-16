/* ============================================================
 * ui.h - SaylterOS 图形界面组件库
 * ------------------------------------------------------------
 * 一套像素级、无浮点依赖的 immediate-mode 组件：
 *   面板 / 按钮 / 输入框 / 进度条 / 复选框 / 文本
 * 所有绘制走 VBE backbuffer，调用 ui_swap() 一次性上屏。
 *
 * 坐标全部是像素坐标；字形固定 16×32（font16x32）。
 * 不依赖堆分配，组件结构由调用方在栈上持有。
 * ============================================================ */

#ifndef UI_H
#define UI_H

#include <stdint.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} ui_rect;

typedef struct {
    uint32_t bg;
    uint32_t panel;
    uint32_t panel_alt;
    uint32_t border;
    uint32_t text;
    uint32_t text_dim;
    uint32_t accent;
    uint32_t accent_hover;
    uint32_t accent_dim;
    uint32_t ok;
    uint32_t warn;
    uint32_t err;
} ui_theme;

typedef struct {
    ui_rect r;
    const char *label;
    int hover;
    int pressed;
} ui_button;

typedef struct {
    ui_rect r;
    char text[64];
    int len;
    int focused;
} ui_textbox;

typedef struct {
    ui_rect r;
    int value;              /* 0..100 */
} ui_progress;

typedef struct {
    ui_rect r;
    const char *label;
    int checked;
    int hover;
} ui_checkbox;

/* 深色主题（与终端配色一致） */
void ui_theme_dark(ui_theme *t);

/* 基础绘制 */
void ui_rect_fill(ui_rect r, uint32_t color);
void ui_round_fill(ui_rect r, int radius, uint32_t color);
void ui_round_fill_alpha(ui_rect r, int radius, uint32_t color, uint8_t alpha);
void ui_round_gradient(ui_rect r, int radius, uint32_t top, uint32_t bottom);
void ui_gradient_v(ui_rect r, uint32_t top, uint32_t bottom);
void ui_shadow(ui_rect r, int radius, int spread);
void ui_round_panel(ui_rect r, int radius, uint32_t border, uint32_t fill);
void ui_hline(int x, int y, int w, uint32_t color);
void ui_vline(int x, int y, int h, uint32_t color);
void ui_line(int x0, int y0, int x1, int y1, uint32_t color);
void ui_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void ui_text_transparent(int x, int y, const char *s, uint32_t fg);
void ui_text_big(int x, int y, const char *s, uint32_t fg);
int  ui_text_width(const char *s);

/* ---- 自动换行 ----
 * 统一的文本排版入口，避免每个界面各自算字符宽度导致溢出。
 *   max_w    : 允许的最大像素宽度（按 16px 字符宽换算成列数）
 *   line_h   : 行高（像素），调用方按面板高度取 34 / 44 等
 *   max_lines: 最多绘制多少行，<= 0 表示不限制
 * '\n' 强制换行；超宽时优先在最近的空格处断行，没有空格则硬切。
 * 返回实际占用的行数。 */
int ui_text_wrap(int x, int y, int max_w, int line_h, const char *s,
                 uint32_t color, int max_lines);

/* 只计算换行后占用的行数，不绘制（布局预排用） */
int ui_text_wrap_lines(int max_w, const char *s);

/* 返回字符串按 max_w 换行后需要的总高度 */
int ui_text_wrap_height(int max_w, int line_h, const char *s, int max_lines);

/* 组件 */
void ui_panel(const ui_theme *t, ui_rect r, const char *title);
void ui_button_draw(const ui_theme *t, const ui_button *b);
void ui_textbox_draw(const ui_theme *t, const ui_textbox *tb, const char *label);
void ui_progress_draw(const ui_theme *t, const ui_progress *p, const char *label);
void ui_checkbox_draw(const ui_theme *t, const ui_checkbox *c);

/* 把 backbuffer 一次性提交到 LFB */
void ui_swap(void);

/* 交互示例桌面：返回时表示用户按 Esc 退出 */
void ui_demo_main(void);

#endif /* UI_H */
