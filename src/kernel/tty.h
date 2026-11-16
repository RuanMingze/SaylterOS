#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* 终端初始化：检测 VBE 是否可用。
 *   - 可用：进入图形终端，字符单元 16x32（内置 VGA ROM 字体），
 *           行列为 framebuffer 尺寸 / 字形尺寸（如 1600x1200 → 100x37）。
 *   - 不可用：回退到 80x25 VGA 文本模式，行为与旧版一致。
 * 必须在 vbe_init() 之后调用。 */
void tty_init(void);

/* 当前字符终端尺寸（图形/文本模式都有效） */
uint16_t tty_get_cols(void);
uint16_t tty_get_rows(void);

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
/* 只写屏幕、不回显串口的字符输出，用于 cat 这类大块文本场景 */
void tty_putchar_vga(char c);
void printk(const char *fmt, ...);

/* 输出重定向：fn 非 NULL 时 printk 的字符全部交给它，不写屏幕/串口。
 * GUI 内嵌终端用它在窗口里显示 shell 命令输出。传 NULL 恢复默认。 */
void tty_set_output_hook(void (*fn)(const char *s, int len));

/* ---- 直接单元格绘制（编辑器 / 蓝屏 / shell 行编辑使用） ---- */

/* 整行用指定颜色属性填充为空格 */
void tty_clear_row(uint16_t row, uint8_t attr);
/* 从 (x, y) 清到该行行尾（不影响其它行） */
void tty_clear_from(uint16_t x, uint16_t y);
/* 在指定位置写一个带属性的字符（不移动光标） */
void tty_put_at(uint16_t x, uint16_t y, char c, uint8_t attr);
/* 批量绘制：成批写单元格时只做一次帧缓冲回写，降低全屏重绘开销 */
void tty_begin_batch(void);
void tty_end_batch(void);

/* 光标闪烁驱动：输入空闲时周期调用。
 * 图形终端用 PIT 通道 0 计数计时，约 0.5 秒切换一次"擦除 / 重绘"；
 * 文本终端由 VGA 硬件光标负责，本函数直接返回。 */
void tty_tick(void);

/* 光标闪烁驱动：空闲等待（无按键）时周期调用。
 * 图形终端用 PIT 计数器计时，约 0.5s 切换一次"擦除/重绘"；
 * 文本终端由 VGA 硬件光标负责，本函数直接返回。 */
void tty_tick(void);

#endif
