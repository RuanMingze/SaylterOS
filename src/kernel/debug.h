/* ============================================================
 * debug.h - SaylterOS 调试模式
 * ------------------------------------------------------------
 * 编译期宏 SAYLTER_DEBUG=1 时启用：
 *   - SlyzFS 挂载之前的早期调试控制台；
 *   - 串口输出落盘（SlyzFS 就绪后写入当前启动盘）；
 *   - 常驻 HUD（内存 / CPU / 磁盘）；
 *   - 关闭"挂载失败即格式化"的危险行为。
 *
 * 释放构建里所有入口都是空操作，不增加运行时开销。
 * ============================================================ */

#ifndef SAYLTER_DEBUG_H
#define SAYLTER_DEBUG_H

#include <stdint.h>

#ifndef SAYLTER_DEBUG
#define SAYLTER_DEBUG 0
#endif

#if SAYLTER_DEBUG

/* 早期调试控制台：在 VBE/TTY 就绪后、正常内核启动之前运行。
 * 用户输入 next 后返回，kernel_main 继续原有启动流程。 */
void debug_early_console(void);

/* 串口字节镜像：tty_putchar 对每个输出字符调用。 */
void debug_serial_byte(char c);
/* SlyzFS 挂载成功后调用，开始把缓冲写入当前启动盘。 */
void debug_serial_fs_ready(void);
/* 手动刷新一次日志文件。 */
void debug_serial_flush(void);

/* 常驻 HUD。debug_tick 由 tty_tick 周期调用；
 * debug_hud_gui_tick 由 GUI 桌面循环每帧调用。 */
void debug_tick(void);
void debug_hud_gui_tick(void);
void debug_storage_ready(void);

/* CPU 空闲采样：TTY 空闲循环调用。 */
void debug_cpu_idle_mark(void);

/* 设置查询 */
int debug_hud_enabled(void);
int debug_serial_enabled(void);
int debug_auto_format_allowed(void);

#else

static inline void debug_early_console(void) {}
static inline void debug_serial_byte(char c) { (void)c; }
static inline void debug_serial_fs_ready(void) {}
static inline void debug_serial_flush(void) {}
static inline void debug_tick(void) {}
static inline void debug_hud_gui_tick(void) {}
static inline void debug_storage_ready(void) {}
static inline void debug_cpu_idle_mark(void) {}
static inline int debug_hud_enabled(void) { return 0; }
static inline int debug_serial_enabled(void) { return 0; }
static inline int debug_auto_format_allowed(void) { return 1; }

#endif

#endif /* SAYLTER_DEBUG_H */
