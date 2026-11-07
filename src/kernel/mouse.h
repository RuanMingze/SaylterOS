#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* ------------------------------------------------------------
 * mouse.h —— PS/2 鼠标（8042 第二端口）驱动
 * ------------------------------------------------------------
 * 轮询模式（非阻塞）：调用者主动 ps2_mouse_poll() 尝试取 3 字节
 * 有数据返回 1 并填 dx/dy/buttons，无数据返回 0
 * ------------------------------------------------------------ */

/* 统一光标尺寸，两个命令都用 */
#define MOUSE_W  8
#define MOUSE_HEIGHT  16

uint8_t ps2_mouse_init(void);   /* 返回 1 = 鼠标 OK, 0 = 失败 */
void    ps2_mouse_disable(void);

/* 非阻塞 poll：返回 1 表示有新数据包（3 字节完整），0 表示现在没数据 */
uint8_t ps2_mouse_poll(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons);

#endif /* MOUSE_H */