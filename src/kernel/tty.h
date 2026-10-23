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
void printk(const char *fmt, ...);

#endif
