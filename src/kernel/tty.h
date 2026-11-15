#ifndef TTY_H
#define TTY_H

#include <stdint.h>

void tty_clear(void);
void tty_putchar(char c);
void tty_write(const char *s, int len);
void tty_set_color(uint8_t color);
void tty_set_cursor(uint16_t x, uint16_t y);
void tty_set_cursor_shape(uint8_t start_scanline, uint8_t end_scanline);
uint16_t tty_get_cursor_x(void);
uint16_t tty_get_cursor_y(void);
/* 滚动区底部行（含）。其下方为保留区（安全区），不参与滚动，
 * 用于固定显示 shell 提示符。shell 按 prompt 实际行数动态设置。 */
void tty_set_scroll_limit(uint16_t row);
uint16_t tty_get_scroll_limit(void);
/* 把整个保留区（scroll_limit 下方那些行）逐行上提到滚动区，
 * 相当于"提交"当前输入行：历史 prompt 进入可滚动区域，
 * 保留区被清空，光标落到滚动区最后一行等待命令输出。
 * shell 在用户按下回车后调用一次。 */
void tty_commit_reserved(void);
/* 只写 VGA、不回显串口的字符输出，用于 cat 这类大块文本场景 */
void tty_putchar_vga(char c);
void printk(const char *fmt, ...);

#endif
