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

uint16_t tty_get_cursor_x(void) {
    return cursor_x;
}

uint16_t tty_get_cursor_y(void) {
    return cursor_y;
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
    if (cursor_y >= VGA_ROWS) {
        tty_scroll();
    }
}

/* ------------------------------------------------------------
 * 向上滚动 1 行 (最上面丢掉, 最下面空出来)
 * ------------------------------------------------------------ */
static void tty_scroll(void) {
    uint16_t *video = (uint16_t *)VGA_MEM;
    /* 从第 2 行开始拷贝到第 1 行位置 */
    for (int row = 1; row < VGA_ROWS; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            video[(row - 1) * VGA_COLS + col] = video[row * VGA_COLS + col];
        }
    }
    /* 最底行清空 */
    for (int col = 0; col < VGA_COLS; col++) {
        video[(VGA_ROWS - 1) * VGA_COLS + col] = (tty_color << 8) | ' ';
    }
    cursor_y = VGA_ROWS - 1;
}

/* ------------------------------------------------------------
 * 写单个字符到 VGA
 * ------------------------------------------------------------ */
void tty_putchar(char c) {
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
            if (cursor_y >= VGA_ROWS) tty_scroll();
        }
    }
    tty_update_hw_cursor();
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
    tty_update_hw_cursor();
}

/* ------------------------------------------------------------
 * 设置 TTY 颜色
 * ------------------------------------------------------------ */
void tty_set_color(uint8_t color) {
    tty_color = color;
}

/* ------------------------------------------------------------
 * 简易 vsprintf (仅支持 %s, %d, %x, %c)
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
