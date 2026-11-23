#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* ------------------------------------------------------------
 * mouse.h —— PS/2 指针设备（8042 第二端口）
 * ------------------------------------------------------------
 * 启动时自动识别三类设备：
 *   - Synaptics 触控板：切到绝对模式，驱动内实现单指移动、
 *     轻点左键、双指点按右键、三指点按中键、双指滚动、
 *     轻点拖拽、双击、手掌误触抑制与指针加速；
 *   - IntelliMouse 滚轮鼠标（4 字节包）；
 *   - 标准 3 字节 PS/2 鼠标。
 *
 * 轮询模式（非阻塞）：调用者反复调用 ps2_mouse_poll_ex() 取事件。
 * 每次返回一个事件，没数据返回 0。触控板手势产生的合成点击
 * 同样以按键事件返回，上层 GUI 不需要区分鼠标和触控板。
 * ------------------------------------------------------------ */

/* 统一光标尺寸，两个命令都用 */
#define MOUSE_W  8
#define MOUSE_HEIGHT  16

uint8_t ps2_mouse_init(void);   /* 返回 1 = 设备 OK, 0 = 失败 */
void    ps2_mouse_disable(void);

/* 非阻塞 poll：返回 1 表示拿到一个事件（相对位移 / 按键 / 滚轮），
 * 0 表示当前没有新数据。buttons：bit0 左 / bit1 右 / bit2 中。 */
uint8_t ps2_mouse_poll(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons);

/* 带滚轮版本：out_wheel 返回滚轮格数（正 = 向上滚，负 = 向下滚）。
 * 触控板双指滚动同样通过它上报。out_wheel 可为 NULL。 */
uint8_t ps2_mouse_poll_ex(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons,
                          int8_t *out_wheel);

/* ---- 触控板共享入口（PS/2 Synaptics 与 I2C HID 共用同一套手势引擎） ----
 * x/y：触控板坐标（I2C 侧归一到 0..6143）
 * z  ：压力，<=0 表示无接触
 * w  ：2=单指 / 0=双指 / 1=三指 / >=10 手掌
 * phys：物理按键位图（bit0 左 / bit1 右 / bit2 中）
 * now ：毫秒时间戳 */
void tp_input_packet(int x, int y, int z, int w, int phys, uint32_t now);

/* 向统一鼠标事件队列投递一个事件（供 I2C HID 驱动使用） */
void ps2_mouse_post_event(int dx, int dy, int wheel, uint8_t buttons);

#endif /* MOUSE_H */
