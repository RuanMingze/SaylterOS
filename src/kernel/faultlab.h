/* ============================================================
 * faultlab.h - 故障注入与崩溃转储内核服务
 * ------------------------------------------------------------
 * 供 SylFault（图形版 / 终端版）调用：
 *   - 按类型触发蓝屏（STOP 码与真实 Windows 语义对应，注明模拟）
 *   - 生成崩溃转储文本到 system\crash\dump-<uptime>.txt
 *   - 可配置的串口落盘（system\logs\serial.log）
 *   - 可控的内存泄漏模拟
 *
 * 串口落盘的反馈环约束：
 *   SlyzFS 的写入路径自身会向串口打印诊断。若串口输出再被落盘，
 *   就会形成 "写盘 -> 串口 -> 再写盘" 的自激循环。因此本模块在
 *   任何文件写入期间都把串口置为静默（连硬件串口也不输出），
 *   由 faultlab_serial_muted() 暴露给最底层的 kserial_putchar。
 * ============================================================ */

#ifndef FAULTLAB_H
#define FAULTLAB_H

#include <stdint.h>

/* 启动时调用一次（SlyzFS 挂载完成之后）：读取 etc\faultlab.cfg，
 * 按配置打开串口日志。重复调用会先释放上一份日志句柄。 */
void faultlab_init(void);

/* 故障类型（与 samples/sylfault.slyzcode 的按钮顺序一致）：
 *   0 手动触发      1 除零          2 空指针
 *   3 非法指令      4 双重释放      5 栈溢出
 *   6 内存耗尽      7 IRQL 违规     8 安全校验失败
 *   9 关键结构损坏 10 驱动 IRQL 违规
 * 成功注入不会返回（系统停机）；未知类型返回 -1。 */
int faultlab_trigger(int kind);

/* 立即写一份崩溃转储（不含 STOP 码）。返回 0 表示文件已落盘。 */
int faultlab_dump_now(void);

/* 泄漏模拟：再申请约 kb 千字节，返回当前累计泄漏的 KB 数。
 * 单块 64KB，上限 4MB，避免把整个内核堆吃干净。 */
int faultlab_leak(int kb);

/* 释放全部泄漏块，返回释放的 KB 数。 */
int faultlab_leak_free(void);

/* 当前累计泄漏的 KB 数。 */
int faultlab_leak_total(void);

/* 配置读写：dump、serial_to_disk、leak_kb（只读）。
 * 返回 0 成功；未知键或非法值返回 -1。 */
int faultlab_set(const char *key, const char *value);
int faultlab_get(const char *key, char *out, int cap);

/* 串口钩子：kserial_putchar 每输出一个字节调用一次。
 * 静默期间不应被调用；实现内部仍有重入保护。 */
void faultlab_serial_tap(char c);

/* 非 0 表示当前处于文件写入的静默窗口，串口应完全丢弃字节。 */
int faultlab_serial_muted(void);

/* 把串口缓冲刷到磁盘。可在崩溃路径反复调用。 */
void faultlab_serial_flush(void);

/* 崩溃路径入口：在 bsod() 冻结中断之前调用。
 * 按配置写转储、刷串口日志；全程不向串口输出任何字符。 */
void faultlab_on_bsod(uint32_t stop_code, const char *reason);

#endif /* FAULTLAB_H */
