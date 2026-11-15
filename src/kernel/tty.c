/* ============================================================
 * tty.c - SaylterOS VGA 文本模式驱动 (80x25)
 * ------------------------------------------------------------
 * 1. 软件 TTY 状态 (cursor x/y, color)
 * 2. 硬件光标同步 (每次写字符都更新 VGA CRTC)
 * 3. printk 用 vsprintf + putchar
 * 4. 所有写函数禁用中断保护 race condition
 * ============================================================ */

#include <stdint.h>
#include <stdarg.h>
#include "io.h"

/* VGA 常量 */
#define VGA_MEM   0xB8000
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_PORT  0x3D4    /* CRTC 地址寄存器 */
#define VGA_DATA  0x3D5    /* CRTC 数据寄存器 */

/* CRTC 寄存器号 */
#define CRT_CURSOR_START 0x0A
#define CRT_CURSOR_END   0x0B
#define CRT_CURSOR_HIGH  0x0E
#define CRT_CURSOR_LOW   0x0F

/* 默认颜色：浅灰底黑字 */
#define COLOR_DEFAULT 0x0F   /* 黑底白字 */
#define COLOR_PROMPT  0x1F   /* 蓝底白字 */

/* forward declarations */
static void tty_scroll(void);
static void tty_update_hw_cursor(void);
static void tty_newline(void);

/* 全局状态 */
static uint16_t cursor_x = 0;
static uint16_t cursor_y = 0;
static uint8_t  tty_color = COLOR_DEFAULT;
/* 滚动上限行：只有 0..scroll_limit 参与滚动，其下的行是保留区（安全区），
 * 用于固定显示 shell 提示符，避免大量输出把 prompt 顶出屏幕。
 * shell 会根据当前 prompt 的实际显示宽度动态设置该值。 */
static uint16_t scroll_limit = VGA_ROWS - 1;

uint16_t tty_get_cursor_x(void) {
    return cursor_x;
}

uint16_t tty_get_cursor_y(void) {
    return cursor_y;
}

/* 设置滚动区底部（含）。row 为保留区上方最后一行。
 * 例：保留底部 2 行时传 VGA_ROWS - 1 - 2 = 22。 */
void tty_set_scroll_limit(uint16_t row) {
    if (row >= VGA_ROWS) row = VGA_ROWS - 1;
    scroll_limit = row;
    if (cursor_y > scroll_limit) cursor_y = scroll_limit;
}

uint16_t tty_get_scroll_limit(void) {
    return scroll_limit;
}

/* 提交保留区：把 scroll_limit 下方的每一行依次搬到滚动区末行并滚动一次。
 * 效果等同于终端里"这一行输入已经敲定了"：
 *   - 历史 prompt + 命令进入可滚动区域，不再被下一次 prompt 覆盖；
 *   - 保留区清空，避免上一轮输入残留字符；
 *   - 光标停在滚动区最后一行，命令输出从这里开始写。 */
void tty_commit_reserved(void) {
    uint16_t *video = (uint16_t *)VGA_MEM;
    uint16_t rows = (uint16_t)(VGA_ROWS - 1 - scroll_limit);

    for (uint16_t r = 0; r < rows; r++) {
        for (int col = 0; col < VGA_COLS; col++) {
            video[scroll_limit * VGA_COLS + col] =
                video[(scroll_limit + 1 + r) * VGA_COLS + col];
        }
        tty_scroll();
    }

    for (uint16_t row = (uint16_t)(scroll_limit + 1); row < VGA_ROWS; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            video[row * VGA_COLS + col] = (uint16_t)((tty_color << 8) | ' ');
        }
    }

    cursor_x = 0;
    cursor_y = scroll_limit;
    tty_update_hw_cursor();
}

void tty_set_cursor(uint16_t x, uint16_t y) {
    if (x >= VGA_COLS) x = VGA_COLS - 1;
    if (y >= VGA_ROWS) y = VGA_ROWS - 1;
    cursor_x = x;
    cursor_y = y;
    tty_update_hw_cursor();
}

void tty_set_cursor_shape(uint8_t start_scanline, uint8_t end_scanline) {
    outb(VGA_PORT, CRT_CURSOR_START);
    outb(VGA_DATA, start_scanline);
    outb(VGA_PORT, CRT_CURSOR_END);
    outb(VGA_DATA, end_scanline);
}

/* ------------------------------------------------------------
 * 硬件光标同步 —— 每次输出后必须调!
 * ------------------------------------------------------------ */
static void tty_update_hw_cursor(void) {
    uint16_t pos = cursor_y * VGA_COLS + cursor_x;
    /* Set the CRTC cursor index before writing the high/low bytes. */
    outb(VGA_PORT, CRT_CURSOR_HIGH);
    outb(VGA_DATA, (pos >> 8) & 0xFF);
    outb(VGA_PORT, CRT_CURSOR_LOW);
    outb(VGA_DATA, pos & 0xFF);
}

/* ------------------------------------------------------------
 * 把 CR/LF 转成 VGA 位置
 * ------------------------------------------------------------ */
static void tty_newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y > scroll_limit) {
        tty_scroll();
    }
}

/* ------------------------------------------------------------
 * 在滚动区内向上滚动 1 行（保留区不受影响）
 * ------------------------------------------------------------ */
static void tty_scroll(void) {
    uint16_t *video = (uint16_t *)VGA_MEM;
    for (int row = 1; row <= (int)scroll_limit; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            video[(row - 1) * VGA_COLS + col] = video[row * VGA_COLS + col];
        }
    }
    for (int col = 0; col < VGA_COLS; col++) {
        video[scroll_limit * VGA_COLS + col] = (tty_color << 8) | ' ';
    }
    cursor_y = scroll_limit;
}

/* ------------------------------------------------------------
 * 只写 VGA 的字符输出（不回显串口）
 * ------------------------------------------------------------
 * 供 cat 这类大块文本输出使用：9600 波特率下把十几 KB 内容同时
 * 回显到 COM1 需要十几秒，屏幕会被拖成幻灯片。串口那边由调用方
 * 在结束时补一行摘要即可。
 * ------------------------------------------------------------ */
static void tty_vga_putc(char c) {
    uint16_t *video = (uint16_t *)VGA_MEM;

    if (c == '\n' || c == '\r') {
        tty_newline();
    } else if (c == '\b') {
        /* 退格 */
        if (cursor_x > 0) cursor_x--;
        video[cursor_y * VGA_COLS + cursor_x] = (tty_color << 8) | ' ';
    } else {
        video[cursor_y * VGA_COLS + cursor_x] = (tty_color << 8) | c;
        cursor_x++;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > scroll_limit) tty_scroll();
        }
    }
    tty_update_hw_cursor();
}

void tty_putchar_vga(char c) {
    tty_vga_putc(c);
}

/* ------------------------------------------------------------
 * 写单个字符到 VGA + 串口
 * ------------------------------------------------------------ */
void tty_putchar(char c) {
    /* COM1 echo: 等 TX buffer 空再写, 否则 printk 快速输出时丢字节 */
    while ((inb(0x3FD) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);

    tty_vga_putc(c);
}

/* ------------------------------------------------------------
 * 写字符串
 * ------------------------------------------------------------ */
void tty_write(const char *s, int len) {
    for (int i = 0; i < len; i++) tty_putchar(s[i]);
}

/* ------------------------------------------------------------
 * 清屏
 * ------------------------------------------------------------ */
void tty_clear(void) {
    uint16_t *video = (uint16_t *)VGA_MEM;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        video[i] = (tty_color << 8) | ' ';
    cursor_x = 0;
    cursor_y = 0;
    scroll_limit = VGA_ROWS - 1;
    tty_update_hw_cursor();
}

/* ------------------------------------------------------------
 * 设置 TTY 颜色
 * ------------------------------------------------------------ */
void tty_set_color(uint8_t color) {
    tty_color = color;
}

/* ------------------------------------------------------------
 * 简易 vsprintf (支持 %s, %d, %u, %x, %c)
 * ------------------------------------------------------------ */
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
 * printk —— 格式化输出到 VGA
 * ------------------------------------------------------------ */
void printk(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = tty_vsprintf(buf, fmt, args);
    va_end(args);
    tty_write(buf, len);
}
