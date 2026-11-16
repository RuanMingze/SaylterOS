/* ============================================================
 * ui.c - 像素级 GUI 组件库实现
 * ------------------------------------------------------------
 * 设计约束：
 *   1. 只用整数运算（构建参数禁用 SSE，不能碰浮点）；
 *   2. 圆角、渐变、阴影全部用扫描线 + alpha 混合实现，
 *      不依赖额外图片资源；
 *   3. 组件是 immediate-mode：每帧整屏重绘，状态由调用方持有。
 *
 * 颜色统一为 0x00RRGGBB（与 VBE 32bpp LFB 的 RGB 通道一致）。
 * ============================================================ */

#include <stdint.h>

#include "vbe.h"
#include "ui.h"
#include "keyboard.h"
#include "mouse.h"
#include "acpi.h"
#include "acpica_glue.h"

#define CURSOR_W 16
#define CURSOR_H 24

/* ============================================================
 * 颜色工具
 * ============================================================ */

static uint32_t ui_lerp_color(uint32_t c0, uint32_t c1, uint32_t t)
{
    /* t: 0..255 */
    uint32_t r0 = (c0 >> 16) & 0xFFu;
    uint32_t g0 = (c0 >> 8) & 0xFFu;
    uint32_t b0 = c0 & 0xFFu;
    uint32_t r1 = (c1 >> 16) & 0xFFu;
    uint32_t g1 = (c1 >> 8) & 0xFFu;
    uint32_t b1 = c1 & 0xFFu;

    uint32_t r = r0 + ((r1 - r0) * t) / 255u;
    uint32_t g = g0 + ((g1 - g0) * t) / 255u;
    uint32_t b = b0 + ((b1 - b0) * t) / 255u;

    return (r << 16) | (g << 8) | b;
}

void ui_theme_dark(ui_theme *t)
{
    t->bg           = 0x0B1016;
    t->panel        = 0x151C24;
    t->panel_alt    = 0x1D2731;
    t->border       = 0x2C3A47;
    t->text         = 0xE6EDF3;
    t->text_dim     = 0x8B9BA8;
    t->accent       = 0x3B82F6;
    t->accent_hover = 0x60A5FA;
    t->accent_dim   = 0x1D4ED8;
    t->ok           = 0x22C55E;
    t->warn         = 0xF59E0B;
    t->err          = 0xEF4444;
}

/* ============================================================
 * 基础图元
 * ============================================================ */

void ui_rect_fill(ui_rect r, uint32_t color)
{
    if (r.w <= 0 || r.h <= 0) return;
    vbe_fill_rect((uint16_t)r.x, (uint16_t)r.y, (uint16_t)r.w, (uint16_t)r.h, color);
}

/* 圆角半径 radius 在行 dy 处的内缩像素数（1/4 圆近似） */
static int ui_round_inset(int radius, int dy)
{
    int yy = radius - 1 - dy;
    int dx = radius;
    int r2 = radius * radius;

    while (dx > 0 && (dx - 1) * (dx - 1) + yy * yy >= r2) {
        dx--;
    }
    return radius - dx;
}

void ui_round_fill(ui_rect r, int radius, uint32_t color)
{
    if (r.w <= 0 || r.h <= 0) return;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;
    if (radius <= 0) {
        ui_rect_fill(r, color);
        return;
    }

    for (int dy = 0; dy < r.h; dy++) {
        int inset = 0;
        if (dy < radius) {
            inset = ui_round_inset(radius, dy);
        } else if (dy >= r.h - radius) {
            inset = ui_round_inset(radius, r.h - 1 - dy);
        }
        vbe_fill_rect((uint16_t)(r.x + inset), (uint16_t)(r.y + dy),
                      (uint16_t)(r.w - inset * 2), 1, color);
    }
}

void ui_round_fill_alpha(ui_rect r, int radius, uint32_t color, uint8_t alpha)
{
    if (r.w <= 0 || r.h <= 0 || alpha == 0) return;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;

    for (int dy = 0; dy < r.h; dy++) {
        int inset = 0;
        if (radius > 0 && dy < radius) {
            inset = ui_round_inset(radius, dy);
        } else if (radius > 0 && dy >= r.h - radius) {
            inset = ui_round_inset(radius, r.h - 1 - dy);
        }
        vbe_blend_rect((uint16_t)(r.x + inset), (uint16_t)(r.y + dy),
                       (uint16_t)(r.w - inset * 2), 1, color, alpha);
    }
}

void ui_gradient_v(ui_rect r, uint32_t top, uint32_t bottom)
{
    if (r.w <= 0 || r.h <= 0) return;
    int denom = (r.h > 1) ? (r.h - 1) : 1;

    for (int dy = 0; dy < r.h; dy++) {
        uint32_t c = ui_lerp_color(top, bottom, (uint32_t)dy * 255u / (uint32_t)denom);
        vbe_fill_rect((uint16_t)r.x, (uint16_t)(r.y + dy), (uint16_t)r.w, 1, c);
    }
}

void ui_round_gradient(ui_rect r, int radius, uint32_t top, uint32_t bottom)
{
    if (r.w <= 0 || r.h <= 0) return;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;

    int denom = (r.h > 1) ? (r.h - 1) : 1;
    for (int dy = 0; dy < r.h; dy++) {
        int inset = 0;
        if (radius > 0 && dy < radius) {
            inset = ui_round_inset(radius, dy);
        } else if (radius > 0 && dy >= r.h - radius) {
            inset = ui_round_inset(radius, r.h - 1 - dy);
        }
        uint32_t c = ui_lerp_color(top, bottom, (uint32_t)dy * 255u / (uint32_t)denom);
        vbe_fill_rect((uint16_t)(r.x + inset), (uint16_t)(r.y + dy),
                      (uint16_t)(r.w - inset * 2), 1, c);
    }
}

void ui_shadow(ui_rect r, int radius, int spread)
{
    if (r.w <= 0 || r.h <= 0 || spread <= 0) return;

    /* 由外向内叠加半透明圆角矩形，形成柔和投影 */
    for (int i = spread; i >= 1; i--) {
        ui_rect s;
        uint8_t alpha;

        s.x = r.x - i;
        s.y = r.y - i + 6;
        s.w = r.w + i * 2;
        s.h = r.h + i * 2;
        alpha = (uint8_t)(8 + (spread - i) * 10);

        ui_round_fill_alpha(s, radius + i, 0x000000, alpha);
    }
}

void ui_round_panel(ui_rect r, int radius, uint32_t border, uint32_t fill)
{
    ui_round_fill(r, radius, border);

    ui_rect in;
    in.x = r.x + 2;
    in.y = r.y + 2;
    in.w = r.w - 4;
    in.h = r.h - 4;
    ui_round_fill(in, radius > 2 ? radius - 2 : 0, fill);
}

void ui_hline(int x, int y, int w, uint32_t color)
{
    if (w <= 0) return;
    vbe_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, 1, color);
}

void ui_vline(int x, int y, int h, uint32_t color)
{
    if (h <= 0) return;
    vbe_fill_rect((uint16_t)x, (uint16_t)y, 1, (uint16_t)h, color);
}

void ui_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        vbe_put_pixel((uint16_t)x0, (uint16_t)y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int ui_text_width(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n * 16;
}

void ui_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    vbe_draw_text_px((uint16_t)x, (uint16_t)y, s, fg, bg);
}

/* 透明文本：只画前景像素，用于渐变/图片上，不会留下色块 */
static void ui_text_px_transparent(int x, int y, const char *s, uint32_t fg)
{
    extern const uint8_t font16x32[256][32][2];
    int px = x;

    while (*s) {
        const uint8_t (*glyph)[2] = font16x32[(uint8_t)(*s)];
        for (int row = 0; row < 32; row++) {
            uint8_t left = glyph[row][0];
            uint8_t right = glyph[row][1];
            for (int i = 0; i < 8; i++) {
                if (left & (0x80 >> i)) {
                    vbe_put_pixel((uint16_t)(px + i), (uint16_t)(y + row), fg);
                }
            }
            for (int i = 0; i < 8; i++) {
                if (right & (0x80 >> i)) {
                    vbe_put_pixel((uint16_t)(px + 8 + i), (uint16_t)(y + row), fg);
                }
            }
        }
        px += 16;
        s++;
    }
}

void ui_text_transparent(int x, int y, const char *s, uint32_t fg)
{
    ui_text_px_transparent(x, y, s, fg);
}

/* 2 倍放大标题：每个字形 32×64 像素 */
void ui_text_big(int x, int y, const char *s, uint32_t fg)
{
    extern const uint8_t font16x32[256][32][2];
    int px = x;

    while (*s) {
        const uint8_t (*glyph)[2] = font16x32[(uint8_t)(*s)];
        for (int row = 0; row < 32; row++) {
            uint8_t left = glyph[row][0];
            uint8_t right = glyph[row][1];
            for (int i = 0; i < 16; i++) {
                uint8_t bit = (i < 8) ? (uint8_t)((left >> (7 - i)) & 1u)
                                      : (uint8_t)((right >> (15 - i)) & 1u);
                if (bit) {
                    vbe_fill_rect((uint16_t)(px + i * 2), (uint16_t)(y + row * 2), 2, 2, fg);
                }
            }
        }
        px += 32;
        s++;
    }
}

/* ============================================================
 * 组件
 * ============================================================ */

void ui_panel(const ui_theme *t, ui_rect r, const char *title)
{
    ui_shadow(r, 14, 7);
    ui_round_fill(r, 14, t->border);

    ui_rect in;
    in.x = r.x + 1;
    in.y = r.y + 1;
    in.w = r.w - 2;
    in.h = r.h - 2;
    ui_round_gradient(in, 13, t->panel_alt, t->panel);

    if (title) {
        /* 左侧强调色竖条 + 2 倍标题 */
        vbe_fill_rect((uint16_t)(r.x + 24), (uint16_t)(r.y + 22), 4, 40, t->accent);
        ui_text_big(r.x + 40, r.y + 18, title, t->text);
        ui_hline(r.x + 24, r.y + 80, r.w - 48, t->border);
        /* 标题栏右上角三颗状态灯 */
        vbe_fill_rect((uint16_t)(r.x + r.w - 64), (uint16_t)(r.y + 26), 10, 10, t->err);
        vbe_fill_rect((uint16_t)(r.x + r.w - 48), (uint16_t)(r.y + 26), 10, 10, t->warn);
        vbe_fill_rect((uint16_t)(r.x + r.w - 32), (uint16_t)(r.y + 26), 10, 10, t->ok);
    }
}

void ui_button_draw(const ui_theme *t, const ui_button *b)
{
    uint32_t top;
    uint32_t bottom;

    if (b->pressed) {
        top = t->accent_dim;
        bottom = t->accent;
    } else if (b->hover) {
        top = t->accent_hover;
        bottom = t->accent;
    } else {
        top = t->accent;
        bottom = t->accent_dim;
    }

    ui_shadow(b->r, 12, b->pressed ? 2 : 4);
    ui_round_fill(b->r, 12, t->border);

    ui_rect in;
    in.x = b->r.x + 1;
    in.y = b->r.y + 1;
    in.w = b->r.w - 2;
    in.h = b->r.h - 2;
    ui_round_gradient(in, 11, top, bottom);

    if (!b->pressed) {
        /* 顶部 2px 高光，制造玻璃质感 */
        vbe_blend_rect((uint16_t)(in.x + 10), (uint16_t)(in.y + 2),
                       (uint16_t)(in.w - 20), 2, 0xFFFFFF, 80);
    }

    int tw = ui_text_width(b->label);
    int tx = b->r.x + (b->r.w - tw) / 2;
    int ty = b->r.y + (b->r.h - 32) / 2;
    ui_text_transparent(tx, ty, b->label, t->text);
}

void ui_textbox_draw(const ui_theme *t, const ui_textbox *tb, const char *label)
{
    if (label) {
        ui_text_transparent(tb->r.x, tb->r.y - 34, label, t->text_dim);
    }

    if (tb->focused) {
        ui_rect glow;
        glow.x = tb->r.x - 3;
        glow.y = tb->r.y - 3;
        glow.w = tb->r.w + 6;
        glow.h = tb->r.h + 6;
        ui_round_fill_alpha(glow, 12, t->accent, 70);
    }

    ui_round_fill(tb->r, 10, tb->focused ? t->accent : t->border);

    ui_rect in;
    in.x = tb->r.x + 1;
    in.y = tb->r.y + 1;
    in.w = tb->r.w - 2;
    in.h = tb->r.h - 2;
    ui_round_gradient(in, 9, t->panel_alt, t->panel);

    int tx = tb->r.x + 16;
    int ty = tb->r.y + (tb->r.h - 32) / 2;

    if (tb->len == 0 && !tb->focused) {
        ui_text_transparent(tx, ty, "type here...", t->text_dim);
    } else {
        ui_text_transparent(tx, ty, tb->text, t->text);
    }

    if (tb->focused) {
        int cx = tx + tb->len * 16;
        vbe_fill_rect((uint16_t)(cx + 1), (uint16_t)(ty + 2), 2, 28, t->accent);
    }
}

static void ui_itoa(int value, char *buf)
{
    char tmp[12];
    int n = 0;
    int i = 0;

    if (value < 0) {
        buf[i++] = '-';
        value = -value;
    }
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value > 0) {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (n > 0) {
        buf[i++] = tmp[--n];
    }
    buf[i] = '\0';
}

void ui_progress_draw(const ui_theme *t, const ui_progress *p, const char *label)
{
    if (label) {
        ui_text_transparent(p->r.x, p->r.y - 34, label, t->text_dim);
    }

    /* 轨道：外边框 + 内凹渐变 */
    ui_round_fill(p->r, p->r.h / 2, t->border);

    ui_rect track;
    track.x = p->r.x + 2;
    track.y = p->r.y + 2;
    track.w = p->r.w - 4;
    track.h = p->r.h - 4;
    ui_round_gradient(track, track.h / 2, 0x0A1218, 0x16212B);

    int inner = track.w - 4;
    int fill_w = inner * p->value / 100;
    if (fill_w > 4) {
        ui_rect fill;
        fill.x = track.x + 2;
        fill.y = track.y + 2;
        fill.w = fill_w;
        fill.h = track.h - 4;
        ui_round_gradient(fill, fill.h / 2, 0x34D399, 0x059669);
        vbe_blend_rect((uint16_t)(fill.x + 6), (uint16_t)(fill.y + 1),
                       (uint16_t)(fill.w > 12 ? fill.w - 12 : 1), 2, 0xFFFFFF, 90);
    }

    char buf[16];
    ui_itoa(p->value, buf);
    int i = 0;
    while (buf[i]) i++;
    buf[i++] = '%';
    buf[i] = '\0';
    ui_text_transparent(p->r.x + p->r.w + 16,
                        p->r.y + (p->r.h - 32) / 2, buf, t->text);
}

void ui_checkbox_draw(const ui_theme *t, const ui_checkbox *c)
{
    ui_rect box;
    box.x = c->r.x;
    box.y = c->r.y;
    box.w = 28;
    box.h = 28;

    ui_round_fill(box, 6, c->hover ? t->accent : t->border);

    ui_rect in;
    in.x = box.x + 2;
    in.y = box.y + 2;
    in.w = box.w - 4;
    in.h = box.h - 4;

    if (c->checked) {
        ui_round_gradient(in, 4, t->accent_hover, t->accent_dim);
        /* 对勾：两段 3px 粗线 */
        for (int k = 0; k < 3; k++) {
            ui_line(in.x + 5, in.y + 12 + k, in.x + 10, in.y + 18 + k, 0xFFFFFF);
            ui_line(in.x + 10, in.y + 18 + k, in.x + 19, in.y + 6 + k, 0xFFFFFF);
        }
    } else {
        ui_round_fill(in, 4, t->panel);
    }

    ui_text_transparent(c->r.x + 42, c->r.y - 2, c->label, t->text);
}

void ui_swap(void)
{
    vbe_swap();
}

/* ============================================================
 * 鼠标光标
 * ============================================================ */

/* 16×24 箭头：X = 黑色描边，W = 白色填充 */
static const char *ui_cursor_img[CURSOR_H] = {
    "X...............",
    "XX..............",
    "XWX.............",
    "XWWX............",
    "XWWWX...........",
    "XWWWWX..........",
    "XWWWWWX.........",
    "XWWWWWWX........",
    "XWWWWWWWX.......",
    "XWWWWWWWWX......",
    "XWWWWWWWWWX.....",
    "XWWWWWWWWWWX....",
    "XWWWWWWWWWWWX...",
    "XWWWWWWWWWWWWX..",
    "XWWWWWXXXXXXXX..",
    "XWWXWWX.........",
    "XWX.XWWX........",
    "XX..XWWX........",
    "X....XWWX.......",
    ".....XWWX.......",
    "......XWWX......",
    "......XWWX......",
    ".......XX.......",
    "................"
};

static uint32_t g_cursor_save[CURSOR_W * CURSOR_H];
static int g_cursor_x = -1;
static int g_cursor_y = -1;

static void ui_cursor_draw(int x, int y)
{
    vbe_save_rect((uint16_t)x, (uint16_t)y, CURSOR_W, CURSOR_H, g_cursor_save);

    for (int row = 0; row < CURSOR_H; row++) {
        const char *line = ui_cursor_img[row];
        for (int col = 0; col < CURSOR_W; col++) {
            if (line[col] == 'X') {
                vbe_put_pixel((uint16_t)(x + col), (uint16_t)(y + row), 0x000000);
            } else if (line[col] == 'W') {
                vbe_put_pixel((uint16_t)(x + col), (uint16_t)(y + row), 0xFFFFFF);
            }
        }
    }

    g_cursor_x = x;
    g_cursor_y = y;
}

static void ui_cursor_erase(void)
{
    if (g_cursor_x < 0) return;
    vbe_restore_rect((uint16_t)g_cursor_x, (uint16_t)g_cursor_y,
                     CURSOR_W, CURSOR_H, g_cursor_save);
    g_cursor_x = -1;
    g_cursor_y = -1;
}

/* ============================================================
 * 交互示例桌面
 * ============================================================ */

typedef struct {
    ui_button  inc;
    ui_button  reset;
    ui_checkbox anim;
    ui_progress bar;
    ui_textbox name;
    ui_textbox msg;
    int clicks;
    int progress;
    int quit;
    int left_x;
    int right_x;
    int panel_h;
} ui_demo_state;

/* 脏区域位：每帧只在有变化时重绘并提交，静止时完全不碰显存 */
#define R_BUTTONS  0x01u
#define R_CLICKS   0x02u
#define R_CHECK    0x04u
#define R_PROGRESS 0x08u
#define R_NAME     0x10u
#define R_MSG      0x20u

static void demo_layout(ui_demo_state *st, int W, int H)
{
    int left_x = W / 2 - 760;
    int right_x = W / 2 + 20;
    int panel_w = 720;
    int panel_h = H - 200;

    if (left_x < 24) left_x = 24;
    if (right_x + panel_w > W - 24) right_x = W - 24 - panel_w;

    st->inc.r.x = left_x + 40;
    st->inc.r.y = 250;
    st->inc.r.w = 300;
    st->inc.r.h = 76;
    st->inc.label = "Increment";

    st->reset.r.x = left_x + 360;
    st->reset.r.y = 250;
    st->reset.r.w = 300;
    st->reset.r.h = 76;
    st->reset.label = "Reset";

    st->anim.r.x = left_x + 40;
    st->anim.r.y = 400;
    st->anim.r.w = 300;
    st->anim.r.h = 36;
    st->anim.label = "Enable animation";

    st->bar.r.x = left_x + 40;
    st->bar.r.y = 520;
    st->bar.r.w = 520;
    st->bar.r.h = 36;

    st->name.r.x = right_x + 40;
    st->name.r.y = 250;
    st->name.r.w = panel_w - 80;
    st->name.r.h = 76;

    st->msg.r.x = right_x + 40;
    st->msg.r.y = 420;
    st->msg.r.w = panel_w - 80;
    st->msg.r.h = 76;

    st->left_x = left_x;
    st->right_x = right_x;
    st->panel_h = panel_h;

    (void)panel_h;
}

static void demo_render(const ui_theme *t, const ui_demo_state *st, int W, int H)
{
    /* 背景：整屏垂直渐变 + 顶部光晕 */
    ui_gradient_v((ui_rect){0, 0, W, H}, 0x0D141C, 0x070A0F);
    vbe_blend_rect((uint16_t)(W / 2 - 500), 0, 1000, 240, 0x3B82F6, 18);

    /* 顶栏 */
    ui_gradient_v((ui_rect){0, 0, W, 84}, 0x1B2633, 0x111A24);
    ui_hline(0, 83, W, t->accent);
    ui_text_big(40, 10, "SaylterOS", t->text);
    ui_text_transparent(40 + 10 * 32 + 24, 26, "Control Center", t->text_dim);
    ui_text_transparent(W - 260, 26, "UI Toolkit 0.1", t->text_dim);

    /* 两个主面板 */
    int left_x = W / 2 - 760;
    int right_x = W / 2 + 20;
    if (left_x < 24) left_x = 24;
    if (right_x + 720 > W - 24) right_x = W - 24 - 720;

    ui_panel(t, (ui_rect){left_x, 100, 720, H - 200}, "Components");
    ui_panel(t, (ui_rect){right_x, 100, 720, H - 200}, "Input & Status");

    /* 左侧组件 */
    ui_text_transparent(left_x + 40, 210, "Buttons", t->text_dim);
    ui_button_draw(t, &st->inc);
    ui_button_draw(t, &st->reset);

    char buf[32];
    int n = 0;
    const char *prefix = "Clicks: ";
    while (prefix[n]) {
        buf[n] = prefix[n];
        n++;
    }
    ui_itoa(st->clicks, buf + n);
    ui_text_transparent(left_x + 40, 350, buf, t->text);

    ui_checkbox_draw(t, &st->anim);

    ui_progress_draw(t, &st->bar, "Progress");

    /* 右侧输入区 */
    ui_textbox_draw(t, &st->name, "User name");
    ui_textbox_draw(t, &st->msg, "Message");

    ui_text_transparent(right_x + 40, 540, "Focus: click a field, type with keyboard.", t->text_dim);
    ui_text_transparent(right_x + 40, 584, "Esc returns to the TTY shell.", t->text_dim);

    /* 底部状态栏 */
    ui_gradient_v((ui_rect){0, H - 56, W, 56}, 0x111A24, 0x0B1016);
    ui_hline(0, H - 57, W, t->border);
    ui_text_transparent(40, H - 46, "Click to interact", t->text_dim);
    ui_text_transparent(W - 380, H - 46, "Esc: back to shell", t->text_dim);
}

/* ============================================================
 * 局部重绘
 * ------------------------------------------------------------
 * 全屏重绘（1600×900 + 多层阴影 alpha 混合）在 TCG 下要几百毫秒，
 * 鼠标会一顿一顿。这里把"面板背景渐变"和"单个组件"分开重绘，
 * 每次只提交变化的小矩形。
 * ============================================================ */

static ui_rect demo_panel_inner(ui_rect panel)
{
    ui_rect in;

    in.x = panel.x + 1;
    in.y = panel.y + 1;
    in.w = panel.w - 2;
    in.h = panel.h - 2;
    return in;
}

/* 把重绘区域裁到面板内部：越界会把相邻面板的背景刷成本面板的渐变，
 * 表现为面板之间出现一块突兀的矩形。 */
static ui_rect demo_clip(ui_rect a, ui_rect p)
{
    int x0 = (a.x > p.x) ? a.x : p.x;
    int y0 = (a.y > p.y) ? a.y : p.y;
    int x1 = (a.x + a.w < p.x + p.w) ? (a.x + a.w) : (p.x + p.w);
    int y1 = (a.y + a.h < p.y + p.h) ? (a.y + a.h) : (p.y + p.h);
    ui_rect r;

    r.x = x0;
    r.y = y0;
    r.w = (x1 > x0) ? (x1 - x0) : 0;
    r.h = (y1 > y0) ? (y1 - y0) : 0;
    return r;
}

/* 把面板内部的背景渐变重画到指定区域（局部擦除） */
static void demo_restore_bg(const ui_theme *t, ui_rect area, ui_rect panel_in)
{
    int denom = (panel_in.h > 1) ? (panel_in.h - 1) : 1;

    for (int dy = 0; dy < area.h; dy++) {
        int py = area.y + dy;
        if (py < panel_in.y || py >= panel_in.y + panel_in.h) continue;

        uint32_t c = ui_lerp_color(t->panel_alt, t->panel,
                                   (uint32_t)(py - panel_in.y) * 255u / (uint32_t)denom);
        vbe_fill_rect((uint16_t)area.x, (uint16_t)py, (uint16_t)area.w, 1, c);
    }
}

static void demo_redraw_buttons(const ui_theme *t, const ui_demo_state *st)
{
    ui_rect panel = { st->left_x, 100, 720, st->panel_h };
    ui_rect in = demo_panel_inner(panel);
    ui_rect area;

    area.x = st->inc.r.x - 16;
    area.y = st->inc.r.y - 46;
    area.w = (st->reset.r.x + st->reset.r.w) - st->inc.r.x + 32;
    area.h = st->inc.r.h + 58;
    area = demo_clip(area, in);

    demo_restore_bg(t, area, in);
    ui_text_transparent(st->left_x + 40, 210, "Buttons", t->text_dim);
    ui_button_draw(t, &st->inc);
    ui_button_draw(t, &st->reset);
    vbe_swap_rect((uint16_t)area.x, (uint16_t)area.y, (uint16_t)area.w, (uint16_t)area.h);
}

static void demo_redraw_clicks(const ui_theme *t, const ui_demo_state *st)
{
    ui_rect panel = { st->left_x, 100, 720, st->panel_h };
    ui_rect in = demo_panel_inner(panel);
    ui_rect area = { st->left_x + 32, 342, 520, 44 };
    char buf[32];
    int n = 0;
    const char *prefix = "Clicks: ";

    area = demo_clip(area, in);
    demo_restore_bg(t, area, in);
    while (prefix[n]) {
        buf[n] = prefix[n];
        n++;
    }
    ui_itoa(st->clicks, buf + n);
    ui_text_transparent(st->left_x + 40, 350, buf, t->text);
    vbe_swap_rect((uint16_t)area.x, (uint16_t)area.y, (uint16_t)area.w, (uint16_t)area.h);
}

static void demo_redraw_checkbox(const ui_theme *t, const ui_demo_state *st)
{
    ui_rect panel = { st->left_x, 100, 720, st->panel_h };
    ui_rect in = demo_panel_inner(panel);
    ui_rect area = { st->anim.r.x - 6, st->anim.r.y - 6,
                     st->anim.r.w + 60, st->anim.r.h + 12 };

    area = demo_clip(area, in);
    demo_restore_bg(t, area, in);
    ui_checkbox_draw(t, &st->anim);
    vbe_swap_rect((uint16_t)area.x, (uint16_t)area.y, (uint16_t)area.w, (uint16_t)area.h);
}

static void demo_redraw_progress(const ui_theme *t, const ui_demo_state *st)
{
    ui_rect panel = { st->left_x, 100, 720, st->panel_h };
    ui_rect in = demo_panel_inner(panel);
    ui_rect area = { st->bar.r.x - 8, st->bar.r.y - 44,
                     st->bar.r.w + 140, st->bar.r.h + 56 };

    area = demo_clip(area, in);
    demo_restore_bg(t, area, in);
    ui_progress_draw(t, &st->bar, "Progress");
    vbe_swap_rect((uint16_t)area.x, (uint16_t)area.y, (uint16_t)area.w, (uint16_t)area.h);
}

static void demo_redraw_textbox(const ui_theme *t, const ui_demo_state *st,
                                ui_textbox *tb, const char *label)
{
    ui_rect panel = { st->right_x, 100, 720, st->panel_h };
    ui_rect in = demo_panel_inner(panel);
    /* 覆盖范围：标签（上 34px）+ 输入框本体 + 聚焦发光的 3px 外扩。
     * 只写 r.h + 16 会漏掉下半部分，重绘后输入框下沿残留在旧像素上。 */
    ui_rect area = { tb->r.x - 10, tb->r.y - 44, tb->r.w + 20, tb->r.h + 60 };

    area = demo_clip(area, in);
    demo_restore_bg(t, area, in);
    ui_textbox_draw(t, tb, label);
    vbe_swap_rect((uint16_t)area.x, (uint16_t)area.y, (uint16_t)area.w, (uint16_t)area.h);
}

static void demo_present(const ui_theme *t, const ui_demo_state *st, int W, int H, int mx, int my)
{
    demo_render(t, st, W, H);
    ui_cursor_draw(mx, my);
    ui_swap();
}

void ui_demo_main(void)
{
    ui_theme t;
    ui_demo_state st;
    int W;
    int H;
    int mx;
    int my;
    int press_latch = 0;
    int tick = 0;

    ui_theme_dark(&t);

    /* GUI 也需要鼠标：进入桌面前初始化 8042 第二端口 */
    (void)ps2_mouse_init();

    W = vbe_current.width;
    H = vbe_current.height;
    if (W < 800 || H < 600) {
        return;
    }

    /* 状态初始化 */
    st.clicks = 0;
    st.progress = 0;
    st.quit = 0;
    st.bar.value = 0;
    st.anim.checked = 1;
    st.anim.hover = 0;
    st.inc.hover = 0;
    st.inc.pressed = 0;
    st.reset.hover = 0;
    st.reset.pressed = 0;
    st.name.len = 0;
    st.name.text[0] = '\0';
    st.name.focused = 1;
    st.msg.len = 0;
    st.msg.text[0] = '\0';
    st.msg.focused = 0;

    demo_layout(&st, W, H);

    mx = W / 2;
    my = H / 2;

    demo_present(&t, &st, W, H, mx, my);

    while (!st.quit) {
        int8_t dx = 0;
        int8_t dy = 0;
        uint8_t buttons = 0;
        int moved = 0;
        int clicked = 0;
        unsigned int dirty = 0;

        /* ---------- 电源键 ---------- */
        {
            int fixed = acpica_power_button_pending();
            int power = fixed;
            if (!power && acpi_poll_power_button()) {
                power = 1;
            }
            if (power) {
                if (acpica_ready()) {
                    acpica_request_s5();
                }
                acpi_power_off();
            }
        }
        acpica_drain_deferred();

        /* ---------- 鼠标 ---------- */
        int h_inc = st.inc.hover;
        int h_reset = st.reset.hover;
        int h_anim = st.anim.hover;

        while (ps2_mouse_poll(&dx, &dy, &buttons)) {
            int old_x = mx;
            int old_y = my;

            mx += dx;
            my += dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > W - CURSOR_W) mx = W - CURSOR_W;
            if (my > H - CURSOR_H) my = H - CURSOR_H;

            st.inc.hover = point_in_rect((int16_t)mx, (int16_t)my,
                                         (uint16_t)st.inc.r.x, (uint16_t)st.inc.r.y,
                                         (uint16_t)st.inc.r.w, (uint16_t)st.inc.r.h);
            st.reset.hover = point_in_rect((int16_t)mx, (int16_t)my,
                                           (uint16_t)st.reset.r.x, (uint16_t)st.reset.r.y,
                                           (uint16_t)st.reset.r.w, (uint16_t)st.reset.r.h);
            st.anim.hover = point_in_rect((int16_t)mx, (int16_t)my,
                                          (uint16_t)st.anim.r.x, (uint16_t)st.anim.r.y,
                                          (uint16_t)st.anim.r.w,
                                          (uint16_t)st.anim.r.h);

            if ((buttons & 1u) && !press_latch) {
                press_latch = 1;
                clicked = 1;
            }
            if (!(buttons & 1u)) {
                press_latch = 0;
            }

            if (old_x != mx || old_y != my) {
                moved = 1;
            }
        }

        int hover_changed = (h_inc != st.inc.hover) ||
                            (h_reset != st.reset.hover) ||
                            (h_anim != st.anim.hover);

        if (clicked) {
            if (st.inc.hover) {
                st.clicks++;
                dirty |= R_CLICKS;
            } else if (st.reset.hover) {
                st.clicks = 0;
                st.progress = 0;
                st.bar.value = 0;
                dirty |= R_CLICKS | R_PROGRESS;
            } else if (st.anim.hover) {
                st.anim.checked = !st.anim.checked;
                dirty |= R_CHECK;
            }

            int in_name = point_in_rect((int16_t)mx, (int16_t)my,
                                        (uint16_t)st.name.r.x, (uint16_t)st.name.r.y,
                                        (uint16_t)st.name.r.w, (uint16_t)st.name.r.h);
            int in_msg = point_in_rect((int16_t)mx, (int16_t)my,
                                       (uint16_t)st.msg.r.x, (uint16_t)st.msg.r.y,
                                       (uint16_t)st.msg.r.w, (uint16_t)st.msg.r.h);

            if (in_name && !st.name.focused) {
                st.name.focused = 1;
                st.msg.focused = 0;
                dirty |= R_NAME | R_MSG;
            } else if (in_msg && !st.msg.focused) {
                st.name.focused = 0;
                st.msg.focused = 1;
                dirty |= R_NAME | R_MSG;
            }
        }

        /* ---------- 键盘 ---------- */
        char c = kb_read_char();
        if (c) {
            ui_textbox *tf = st.name.focused ? &st.name : &st.msg;

            if (c == 27) {
                st.quit = 1;
                break;
            }
            if (c == '\n' || c == '\r') {
                if (tf->len > 0) {
                    tf->len = 0;
                    tf->text[0] = '\0';
                }
            } else if (c == 8 || c == 127) {
                if (tf->len > 0) {
                    tf->len--;
                    tf->text[tf->len] = '\0';
                }
            } else if (c >= 32 && c < 127 && tf->len < (int)sizeof(tf->text) - 1) {
                tf->text[tf->len++] = c;
                tf->text[tf->len] = '\0';
            }

            dirty |= (tf == &st.name) ? R_NAME : R_MSG;
        }

        /* ---------- 动画 ---------- */
        if (st.anim.checked) {
            tick++;
            if (tick >= 6) {
                tick = 0;
                st.progress = (st.progress + 1) % 101;
                st.bar.value = st.progress;
                dirty |= R_PROGRESS;
            }
        }

        /* ---------- hover 变化：只重绘按钮与复选框 ---------- */
        if (moved && hover_changed) {
            dirty |= R_BUTTONS | R_CHECK;
        }

        /* ---------- 统一提交 ----------
         * 只有"光标移动"或"有脏区域"时才碰显存：
         *   1. 先擦掉旧光标（backbuffer 与 LFB 都回到干净背景）
         *   2. 按位重绘脏区域（各自 swap_rect）
         *   3. 在新位置补画光标，并刷新旧/新两个小矩形
         * 静止时完全不写显存，避免 QEMU 窗口反复合成造成闪烁。 */
        if (moved || dirty) {
            int old_cx = g_cursor_x;
            int old_cy = g_cursor_y;

            ui_cursor_erase();

            if (dirty & R_BUTTONS)  demo_redraw_buttons(&t, &st);
            if (dirty & R_CLICKS)   demo_redraw_clicks(&t, &st);
            if (dirty & R_CHECK)    demo_redraw_checkbox(&t, &st);
            if (dirty & R_PROGRESS) demo_redraw_progress(&t, &st);
            if (dirty & R_NAME)     demo_redraw_textbox(&t, &st, &st.name, "User name");
            if (dirty & R_MSG)      demo_redraw_textbox(&t, &st, &st.msg, "Message");

            ui_cursor_draw(mx, my);
            if (old_cx >= 0) {
                vbe_swap_rect((uint16_t)old_cx, (uint16_t)old_cy, CURSOR_W, CURSOR_H);
            }
            vbe_swap_rect((uint16_t)mx, (uint16_t)my, CURSOR_W, CURSOR_H);
        }

        for (volatile int spin = 0; spin < 20000; spin++) {
            __asm__ volatile ("nop");
        }
    }

    ui_cursor_erase();
}
