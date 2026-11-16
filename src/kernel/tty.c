/* ============================================================
 * tty.c - SaylterOS 终端驱动
 * ------------------------------------------------------------
 * 双后端：
 *   1. 图形后端（默认）：VBE 线性帧缓冲 + 16x32 VGA ROM 字体。
 *      字符矩阵保存在影子缓冲 cells[][] 里，绘制时按单元格渲染到
 *      backbuffer 再回写屏幕；分辨率由 bootloader 选出的 VBE 模式决定，
 *      因此可以比 80x25 文本模式大得多。
 *   2. 文本后端（回退）：VBE 不可用时沿用 0xB8000 + CRTC 硬件光标，
 *      保证老机器/无 VBE 环境仍能进 shell。
 *
 * 光标在图形模式下没有硬件支持，按单元格重绘实现：
 *   - 插入模式：整格反色（相当于方块光标）
 *   - 覆盖模式：底部 3 像素下划线
 *
 * 所有绘制都经过 cells[][]，滚动/批刷新/光标恢复互不干扰。
 * ============================================================ */

#include <stdint.h>
#include <stdarg.h>
#include "io.h"
#include "tty.h"
#include "vbe.h"

/* VGA 文本模式常量 */
#define VGA_MEM   0xB8000
#define VGA_PORT  0x3D4    /* CRTC 地址寄存器 */
#define VGA_DATA  0x3D5    /* CRTC 数据寄存器 */

#define CRT_CURSOR_START 0x0A
#define CRT_CURSOR_END   0x0B
#define CRT_CURSOR_HIGH  0x0E
#define CRT_CURSOR_LOW   0x0F

/* 图形终端上限：128 列 x 56 行（2048x1792 字符栅格）。
 * 1600x1200 模式下实际为 100x37；上限只用于静态数组定尺寸。
 * 14336 字节 BSS，仍在 0xB8000 红线以内。 */
#define TTY_MAX_COLS  128
#define TTY_MAX_ROWS  56

#define GFX_FONT_W    16
#define GFX_FONT_H    32

/* 默认颜色：黑底白字 */
#define COLOR_DEFAULT 0x0F

/* 16 色 VGA 调色板（0x00RRGGBB） */
static const uint32_t vga_palette[16] = {
    0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,
    0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,
    0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,
    0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu
};

/* ---- 全局状态 ---- */
static uint16_t tty_cols = 80;
static uint16_t tty_rows = 25;
static uint8_t  tty_gfx = 0;
static uint16_t cursor_x = 0;
static uint16_t cursor_y = 0;
static uint8_t  tty_color = COLOR_DEFAULT;
static uint16_t scroll_limit = 24;

/* 图形模式光标形态：1 = 方块（插入），0 = 下划线（覆盖） */
static uint8_t gfx_cursor_block = 1;

/* 影子字符矩阵：图形模式下的唯一可信状态 */
static uint16_t cells[TTY_MAX_ROWS][TTY_MAX_COLS];

/* 批量绘制深度（保留 API；直写帧缓冲后不再需要脏矩形合并） */
static int batch_depth = 0;

/* forward declarations */
static void tty_newline(void);
static void tty_update_hw_cursor(void);
static void tty_text_scroll(void);
static void gfx_scroll_up(void);
static void gfx_draw_glyph(int x, int y);
static void gfx_draw_cursor_overlay(void);
static void gfx_present(void);
static void gfx_paint_row(int y);

/* ============================================================
 * 基础工具
 * ============================================================ */

static inline uint32_t attr_fg(uint8_t attr) {
    return vga_palette[attr & 0x0Fu];
}

static inline uint32_t attr_bg(uint8_t attr) {
    return vga_palette[(attr >> 4) & 0x0Fu];
}

static inline uint16_t cell_value(char c, uint8_t attr) {
    return (uint16_t)(((uint16_t)attr << 8) | (uint8_t)c);
}

static inline uint16_t blank_cell(uint8_t attr) {
    return cell_value(' ', attr);
}

static inline uint16_t *text_mem(void) {
    return (uint16_t *)VGA_MEM;
}

uint16_t tty_get_cols(void) { return tty_cols; }
uint16_t tty_get_rows(void) { return tty_rows; }
uint16_t tty_get_cursor_x(void) { return cursor_x; }
uint16_t tty_get_cursor_y(void) { return cursor_y; }

/* ============================================================
 * 图形后端：脏矩形与单元格绘制
 * ============================================================ */

/* ============================================================
 * 图形终端：直写帧缓冲
 * ------------------------------------------------------------
 * 单缓冲策略：字形直接渲染到 VBE LFB，不再维护 backbuffer→LFB 的
 * 逐字符复制。字符写入的显存流量因此减半，这是无 GPU 加速环境下
 * 最直接的优化；backbuffer 只保留给 GUI 使用。
 * ============================================================ */

/* 只画字符像素，不含光标 */
static void gfx_draw_glyph(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)tty_cols || y >= (int)tty_rows) return;
    uint16_t cell = cells[y][x];
    char ch = (char)(cell & 0x00FFu);
    uint8_t attr = (uint8_t)(cell >> 8);
    vbe_fb_draw_char((uint16_t)x, (uint16_t)y, ch, attr_fg(attr), attr_bg(attr));
}

/* 光标闪烁计时：PIT 通道 0 的 16 位递减计数器（默认 1.193182MHz）。
 * 纯软件实现，不依赖任何图形硬件光标。 */
#define PIT_INPUT_HZ   1193182u
#define PIT_BLINK_HALF (PIT_INPUT_HZ / 2u)   /* 0.5 秒 */

static uint32_t pit_last_count = 0;
static uint32_t pit_accum = 0;
static int cursor_on = 1;

static uint16_t pit_read_count(void) {
    outb(0x43, 0x00);                 /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static void gfx_draw_cursor_overlay(void) {
    if (cursor_x >= tty_cols || cursor_y >= tty_rows) return;
    uint16_t cell = cells[cursor_y][cursor_x];
    uint8_t attr = (uint8_t)(cell >> 8);
    uint32_t fg = attr_fg(attr);

    /* 任何一次显式重绘都视为"用户又在操作"，光标立即恢复可见并重新计时 */
    cursor_on = 1;
    pit_accum = 0;

    if (gfx_cursor_block) {
        /* Ins 插入模式：底部半个字符高度的实心块 */
        vbe_fb_fill_rect((uint16_t)(cursor_x * GFX_FONT_W),
                         (uint16_t)(cursor_y * GFX_FONT_H + GFX_FONT_H / 2),
                         GFX_FONT_W, GFX_FONT_H / 2, fg);
    } else {
        /* 默认（覆盖）模式：底部粗横线，类似加粗的下划线 */
        vbe_fb_fill_rect((uint16_t)(cursor_x * GFX_FONT_W),
                         (uint16_t)(cursor_y * GFX_FONT_H + GFX_FONT_H - 5),
                         GFX_FONT_W, 5, fg);
    }
}

/* 保留 API：直写模式下没有待回写的脏区 */
static void gfx_present(void) {
}

/* ============================================================
 * 光标闪烁驱动（PIT 计数，软件实现）
 * ============================================================ */

void tty_tick(void) {
    if (!tty_gfx) return;

    uint16_t now = pit_read_count();
    int32_t delta = (int32_t)pit_last_count - (int32_t)now;
    if (delta < 0) delta += 65536;    /* 16 位计数器回绕 */
    pit_last_count = now;
    if (delta <= 0) return;

    pit_accum += (uint32_t)delta;
    if (pit_accum < PIT_BLINK_HALF) return;
    pit_accum -= PIT_BLINK_HALF;

    /* 擦除 → 等待 → 重绘，形成闪烁 */
    gfx_draw_glyph(cursor_x, cursor_y);
    cursor_on = !cursor_on;
    if (cursor_on) gfx_draw_cursor_overlay();
}

static void gfx_paint_row(int y) {
    for (int x = 0; x < (int)tty_cols; x++) {
        gfx_draw_glyph(x, y);
    }
}

/* 整行重绘（含光标叠加），用于清行/行编辑 */
static void gfx_paint_row_with_cursor(int y) {
    gfx_paint_row(y);
    if ((int)cursor_y == y) gfx_draw_cursor_overlay();
    gfx_present();
}

/* 全屏重绘：先整屏填背景，再只画非空字符。
 * 终端绝大多数格子是空格，跳过它们能省掉大量字模展开与显存写。 */
static void gfx_paint_all(void) {
    uint8_t clear_attr = tty_color;
    uint8_t clear_bg = (uint8_t)((clear_attr >> 4) & 0x0F);

    vbe_fb_clear(attr_bg(clear_attr));

    for (int y = 0; y < (int)tty_rows; y++) {
        for (int x = 0; x < (int)tty_cols; x++) {
            uint16_t cell = cells[y][x];
            char ch = (char)(cell & 0x00FFu);
            uint8_t attr = (uint8_t)(cell >> 8);
            if (ch == ' ' && (uint8_t)((attr >> 4) & 0x0F) == clear_bg) continue;
            vbe_fb_draw_char((uint16_t)x, (uint16_t)y, ch,
                             attr_fg(attr), attr_bg(attr));
        }
    }
    gfx_draw_cursor_overlay();
}

/* ============================================================
 * 文本后端（无 VBE 回退）
 * ============================================================ */

static void tty_update_hw_cursor(void) {
    if (tty_gfx) return;
    uint16_t pos = (uint16_t)(cursor_y * tty_cols + cursor_x);
    outb(VGA_PORT, CRT_CURSOR_HIGH);
    outb(VGA_DATA, (pos >> 8) & 0xFF);
    outb(VGA_PORT, CRT_CURSOR_LOW);
    outb(VGA_DATA, pos & 0xFF);
}

static void tty_text_scroll(void) {
    uint16_t *video = text_mem();
    for (int row = 1; row <= (int)scroll_limit; row++) {
        for (int col = 0; col < (int)tty_cols; col++) {
            video[(row - 1) * tty_cols + col] = video[row * tty_cols + col];
        }
    }
    for (int col = 0; col < (int)tty_cols; col++) {
        video[scroll_limit * tty_cols + col] = blank_cell(tty_color);
    }
    cursor_y = scroll_limit;
}

/* ============================================================
 * 图形后端滚动 / 提交保留区
 * ============================================================ */

static void gfx_scroll_up(void) {
    int oy = cursor_y;

    /* 1. 字符矩阵上移一行 */
    for (int y = 0; y < (int)scroll_limit; y++) {
        for (int x = 0; x < (int)tty_cols; x++) {
            cells[y][x] = cells[y + 1][x];
        }
    }
    for (int x = 0; x < (int)tty_cols; x++) {
        cells[scroll_limit][x] = blank_cell(tty_color);
    }

    /* 2. 像素直接在 LFB 内按行上移 32 像素，底部填当前背景色。
     * 单缓冲下无需再回写整屏，滚动只产生一次显存内搬运。 */
    vbe_fb_scroll_up(GFX_FONT_H, attr_bg(tty_color));

    cursor_y = scroll_limit;

    /* 3. 旧光标若落在保留区，那一行像素没有被搬移，需要按字符矩阵重绘 */
    if (oy > (int)scroll_limit) {
        gfx_paint_row(oy);
    }
    gfx_draw_cursor_overlay();
}

/* ============================================================
 * 初始化
 * ============================================================ */

void tty_init(void) {
    /* 先关闭 VGA 文本硬件光标：图形模式下它没有意义 */
    outb(VGA_PORT, CRT_CURSOR_START);
    outb(VGA_DATA, 0x20);

    if (vbe_is_enabled()) {
        uint16_t c = (uint16_t)(vbe_current.width / GFX_FONT_W);
        uint16_t r = (uint16_t)(vbe_current.height / GFX_FONT_H);
        if (c < 1) c = 1;
        if (r < 1) r = 1;
        if (c > TTY_MAX_COLS) c = TTY_MAX_COLS;
        if (r > TTY_MAX_ROWS) r = TTY_MAX_ROWS;
        tty_cols = c;
        tty_rows = r;
        tty_gfx = 1;
        /* 默认覆盖模式：底部粗线光标 */
        gfx_cursor_block = 0;
    } else {
        tty_cols = 80;
        tty_rows = 25;
        tty_gfx = 0;
        tty_set_cursor_shape(0x0E, 0x0F);
    }

    tty_color = COLOR_DEFAULT;
    cursor_x = 0;
    cursor_y = 0;
    scroll_limit = (uint16_t)(tty_rows - 1);
    pit_last_count = pit_read_count();
    pit_accum = 0;
    cursor_on = 1;
    tty_clear();
}

/* ============================================================
 * 清屏 / 光标 / 滚动区
 * ============================================================ */

void tty_clear(void) {
    if (tty_gfx) {
        for (int y = 0; y < (int)tty_rows; y++) {
            for (int x = 0; x < (int)tty_cols; x++) {
                cells[y][x] = blank_cell(tty_color);
            }
        }
        cursor_x = 0;
        cursor_y = 0;
        scroll_limit = (uint16_t)(tty_rows - 1);
        gfx_paint_all();
        return;
    }

    uint16_t *video = text_mem();
    for (int i = 0; i < (int)tty_cols * (int)tty_rows; i++) {
        video[i] = blank_cell(tty_color);
    }
    cursor_x = 0;
    cursor_y = 0;
    scroll_limit = (uint16_t)(tty_rows - 1);
    tty_update_hw_cursor();
}

void tty_set_cursor(uint16_t x, uint16_t y) {
    if (x >= tty_cols) x = (uint16_t)(tty_cols - 1);
    if (y >= tty_rows) y = (uint16_t)(tty_rows - 1);

    if (!tty_gfx) {
        cursor_x = x;
        cursor_y = y;
        tty_update_hw_cursor();
        return;
    }

    if (x == cursor_x && y == cursor_y) return;

    int ox = cursor_x;
    int oy = cursor_y;
    cursor_x = x;
    cursor_y = y;
    /* 擦掉旧光标：重画旧格字符，再叠加新光标 */
    gfx_draw_glyph(ox, oy);
    gfx_draw_cursor_overlay();
    gfx_present();
}

void tty_set_cursor_shape(uint8_t start_scanline, uint8_t end_scanline) {
    if (!tty_gfx) {
        outb(VGA_PORT, CRT_CURSOR_START);
        outb(VGA_DATA, start_scanline);
        outb(VGA_PORT, CRT_CURSOR_END);
        outb(VGA_DATA, end_scanline);
        return;
    }

    /* 图形模式：扫描线起点在 0x08 及以上视为粗光标（方块），否则下划线 */
    uint8_t block = (start_scanline >= 0x08) ? 1 : 0;
    if (block == gfx_cursor_block) return;
    gfx_cursor_block = block;
    gfx_draw_glyph(cursor_x, cursor_y);
    gfx_draw_cursor_overlay();
    gfx_present();
}

void tty_set_scroll_limit(uint16_t row) {
    if (row >= tty_rows) row = (uint16_t)(tty_rows - 1);
    if (row == scroll_limit) return;

    int ox = cursor_x;
    int oy = cursor_y;
    scroll_limit = row;
    if (cursor_y > scroll_limit) cursor_y = scroll_limit;

    if (!tty_gfx) {
        tty_update_hw_cursor();
        return;
    }

    if (oy != (int)cursor_y) {
        gfx_draw_glyph(ox, oy);
        gfx_draw_cursor_overlay();
        gfx_present();
    }
}

uint16_t tty_get_scroll_limit(void) {
    return scroll_limit;
}

void tty_commit_reserved(void) {
    uint16_t reserved = (uint16_t)(tty_rows - 1 - scroll_limit);

    if (tty_gfx) {
        for (uint16_t r = 0; r < reserved; r++) {
            /* 把保留区第 r 行搬到滚动区末行，再整体上滚一行 */
            for (int x = 0; x < (int)tty_cols; x++) {
                cells[scroll_limit][x] = cells[scroll_limit + 1 + r][x];
            }
            for (int y = 0; y < (int)scroll_limit; y++) {
                for (int x = 0; x < (int)tty_cols; x++) {
                    cells[y][x] = cells[y + 1][x];
                }
            }
            for (int x = 0; x < (int)tty_cols; x++) {
                cells[scroll_limit][x] = blank_cell(tty_color);
            }
        }
        for (int row = (int)scroll_limit + 1; row < (int)tty_rows; row++) {
            for (int x = 0; x < (int)tty_cols; x++) {
                cells[row][x] = blank_cell(tty_color);
            }
        }
        cursor_x = 0;
        cursor_y = scroll_limit;
        gfx_paint_all();
        return;
    }

    uint16_t *video = text_mem();
    for (uint16_t r = 0; r < reserved; r++) {
        for (int col = 0; col < (int)tty_cols; col++) {
            video[scroll_limit * tty_cols + col] =
                video[(scroll_limit + 1 + r) * tty_cols + col];
        }
        tty_text_scroll();
    }
    for (int row = (int)scroll_limit + 1; row < (int)tty_rows; row++) {
        for (int col = 0; col < (int)tty_cols; col++) {
            video[row * tty_cols + col] = blank_cell(tty_color);
        }
    }
    cursor_x = 0;
    cursor_y = scroll_limit;
    tty_update_hw_cursor();
}

/* ============================================================
 * 单元格直接绘制
 * ============================================================ */

void tty_clear_row(uint16_t row, uint8_t attr) {
    if (row >= tty_rows) return;
    if (!tty_gfx) {
        uint16_t *video = text_mem();
        for (int col = 0; col < (int)tty_cols; col++) {
            video[row * tty_cols + col] = blank_cell(attr);
        }
        tty_update_hw_cursor();
        return;
    }
    for (int col = 0; col < (int)tty_cols; col++) {
        cells[row][col] = blank_cell(attr);
    }
    gfx_paint_row_with_cursor(row);
}

void tty_clear_from(uint16_t x, uint16_t y) {
    if (y >= tty_rows) return;
    if (x >= tty_cols) return;

    if (!tty_gfx) {
        uint16_t *video = text_mem();
        for (int col = x; col < (int)tty_cols; col++) {
            video[y * tty_cols + col] = blank_cell(tty_color);
        }
        tty_update_hw_cursor();
        return;
    }

    for (int col = x; col < (int)tty_cols; col++) {
        cells[y][col] = blank_cell(tty_color);
        gfx_draw_glyph(col, y);
    }
    if ((int)cursor_y == y && cursor_x >= x) gfx_draw_cursor_overlay();
    gfx_present();
}

void tty_put_at(uint16_t x, uint16_t y, char c, uint8_t attr) {
    if (x >= tty_cols || y >= tty_rows) return;
    if (!tty_gfx) {
        text_mem()[y * tty_cols + x] = cell_value(c, attr);
        tty_update_hw_cursor();
        return;
    }
    cells[y][x] = cell_value(c, attr);
    gfx_draw_glyph(x, y);
    if (cursor_x == x && cursor_y == y) gfx_draw_cursor_overlay();
    gfx_present();
}

void tty_begin_batch(void) {
    batch_depth++;
}

void tty_end_batch(void) {
    if (batch_depth <= 0) return;
    batch_depth--;
}

/* ============================================================
 * 字符输出
 * ============================================================ */

static void tty_newline(void) {
    if (tty_gfx) {
        int ox = cursor_x;
        int oy = cursor_y;
        /* 先擦掉旧光标：否则滚动时反色方块会被一起搬到上一行留下残影 */
        gfx_draw_glyph(ox, oy);
        cursor_x = 0;
        cursor_y++;
        if (cursor_y > scroll_limit) {
            gfx_scroll_up();
        } else {
            gfx_draw_cursor_overlay();
            gfx_present();
        }
        return;
    }

    cursor_x = 0;
    cursor_y++;
    if (cursor_y > scroll_limit) {
        tty_text_scroll();
    }
    tty_update_hw_cursor();
}

/* 只写屏幕（图形或文本），不回显串口 */
static void tty_screen_putc(char c) {
    if (c == '\n' || c == '\r') {
        tty_newline();
        return;
    }

    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            if (!tty_gfx) {
                text_mem()[cursor_y * tty_cols + cursor_x] = blank_cell(tty_color);
                tty_update_hw_cursor();
                return;
            }
            int nx = cursor_x;
            cells[cursor_y][nx] = blank_cell(tty_color);
            gfx_draw_glyph(nx, cursor_y);
            /* 旧光标在 nx+1 处，必须单独擦掉，否则退格后残留一个反色方块 */
            gfx_draw_glyph(nx + 1, cursor_y);
            gfx_draw_cursor_overlay();
            gfx_present();
        }
        return;
    }

    /* 行尾自动换行 */
    if (cursor_x >= tty_cols) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y > scroll_limit) {
            if (tty_gfx) {
                gfx_scroll_up();
            } else {
                tty_text_scroll();
            }
        }
    }
    if (cursor_y >= tty_rows) cursor_y = (uint16_t)(tty_rows - 1);

    if (!tty_gfx) {
        text_mem()[cursor_y * tty_cols + cursor_x] = cell_value(c, tty_color);
        cursor_x++;
        tty_update_hw_cursor();
        return;
    }

    int px = cursor_x;
    int py = cursor_y;
    cells[py][px] = cell_value(c, tty_color);
    gfx_draw_glyph(px, py);
    cursor_x++;
    /* 旧格去掉光标，新格补上光标 */
    gfx_draw_cursor_overlay();
    gfx_present();
}

void tty_putchar_vga(char c) {
    tty_screen_putc(c);
}

void tty_putchar(char c) {
    /* COM1 echo: 等 TX buffer 空再写, 否则 printk 快速输出时丢字节 */
    while ((inb(0x3FD) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);

    tty_screen_putc(c);
}

void tty_write(const char *s, int len) {
    for (int i = 0; i < len; i++) tty_putchar(s[i]);
}

void tty_set_color(uint8_t color) {
    tty_color = color;
}

/* ============================================================
 * 简易 vsprintf (支持 %s, %d, %u, %x, %c)
 * ============================================================ */
static int tty_vsprintf(char *buf, const char *fmt, va_list args) {
    int len = 0;
    while (*fmt) {
        if (*fmt == '%' && fmt[1]) {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    while (*s) buf[len++] = *s++;
                    break;
                }
                case 'd': {
                    int32_t v = va_arg(args, int32_t);
                    char tmp[12]; int i = 0;
                    if (v < 0) { buf[len++] = '-'; v = -v; }
                    if (v == 0) tmp[i++] = '0';
                    while (v) { tmp[i++] = '0' + v % 10; v /= 10; }
                    while (i--) buf[len++] = tmp[i];
                    break;
                }
                case 'u': {
                    uint32_t v = va_arg(args, uint32_t);
                    char tmp[12]; int i = 0;
                    if (v == 0) tmp[i++] = '0';
                    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
                    while (i--) buf[len++] = tmp[i];
                    break;
                }
                case 'x': {
                    uint32_t v = va_arg(args, uint32_t);
                    char tmp[12]; int i = 0;
                    if (v == 0) tmp[i++] = '0';
                    while (v) { tmp[i++] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
                    while (i--) buf[len++] = tmp[i];
                    break;
                }
                case 'c': buf[len++] = (char)va_arg(args, int); break;
                default:  buf[len++] = *fmt; break;
            }
        } else {
            buf[len++] = *fmt;
        }
        fmt++;
    }
    buf[len] = '\0';
    return len;
}

/* ------------------------------------------------------------
 * 输出重定向
 * ------------------------------------------------------------
 * GUI 内嵌终端需要一个"和 TTY 同源"的命令输出通道：命令本身仍然
 * 调用 printk，只是字符流向从 LFB/串口改成终端的字符网格。
 * hook 非 NULL 时 printk 不写屏幕也不写串口，避免把 GUI 画面打花。 */
static void (*g_out_hook)(const char *s, int len) = 0;

void tty_set_output_hook(void (*fn)(const char *s, int len)) {
    g_out_hook = fn;
}

/* ------------------------------------------------------------
 * printk —— 格式化输出到屏幕（并回显串口）
 * ------------------------------------------------------------ */
void printk(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = tty_vsprintf(buf, fmt, args);
    va_end(args);

    if (g_out_hook) {
        g_out_hook(buf, len);
        return;
    }

    /* 一次 printk 的所有字符只做一次帧缓冲回写：
     * 逐字符 swap 在 1600x1200 下会产生大量 MMIO 写，是卡顿的主要来源。 */
    tty_begin_batch();
    tty_write(buf, len);
    tty_end_batch();
}
