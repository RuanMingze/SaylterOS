/* ============================================================
 * acpica_glue.h - ACPICA 与 SaylterOS 内核的衔接层
 * ============================================================ */

#ifndef SAYLTER_ACPICA_GLUE_H
#define SAYLTER_ACPICA_GLUE_H

#include <stdint.h>

/* 初始化 ACPICA 子系统并加载 ACPI 表。
 * 必须在 tty_init() 之后调用（ACPICA 的错误输出会写到终端与串口）。
 * 返回 1 表示 AML 命名空间可用，0 表示失败（失败原因走串口日志）。 */
int acpica_init(void);

/* 排空 ACPICA 的延迟调用队列（AcpiOsExecute 入队的回调）。
 * 在 shell 空闲循环里周期调用。 */
void acpica_drain_deferred(void);

/* 把硬件 IRQ 转发给 ACPICA 注册的处理程序。
 * 返回 1 表示已被 ACPICA 处理，0 表示没有注册处理程序。 */
int acpica_dispatch_irq(uint32_t irq);

/* SCI 中断入口（由 pm.asm 的 isr_sci 间接调用）。
 * 内部把中断转发给 ACPICA 注册的 SCI handler，后者负责清 GPE /
 * 固定事件状态；不能在中断上下文里执行 AML，ACPICA 会入队延迟处理。 */
void acpica_sci_handler(void);

/* ACPICA 是否已完成初始化（AML、SCI、GPE、固定事件都可用了）。 */
int acpica_ready(void);

/* 电源键事件（ACI fixed event）是否已触发。读取时清除标志。
 * 由 shell 空闲循环轮询，命中后再执行 S5 关机。 */
int acpica_power_button_pending(void);

/* 通过 ACPICA 执行 S5：读取 \_S5 的 SLP_TYP，执行 _PTS 后写 PM1_CNT。
 * 成功时机器断电、不会返回；返回 0 表示失败（调用方可回退到
 * kernel/acpi.c 的 acpi_power_off()）。 */
int acpica_request_s5(void);

/* 固件定义的睡眠状态位图（bit0=S0 ... bit5=S5）。
 * 只表示 DSDT 里有对应的 \_Sx 包，不代表 OS 已实现该状态的恢复。 */
uint32_t acpica_firmware_sleep_mask(void);

/* OS 已实现的睡眠状态位图（S0/S1/S2/S3/S5；S4 缺少休眠镜像）。 */
uint32_t acpica_os_sleep_mask(void);

/* 进入指定睡眠状态（1..5）。
 * 返回 0 = 已成功进入并返回（S1/S2，或 QEMU 下被立即唤醒）；
 *      -1 = ACPICA 未就绪；-2 = S4 无休眠镜像；-3 = 固件未定义该状态；
 *      -4 = _PTS 准备失败；-5 = 写入 PM1_CNT 失败。
 * S3 在真机上不会返回（CPU 掉电），唤醒后由固件跳回；QEMU 下
 * system_wakeup 会从同一处继续执行并走唤醒恢复流程。 */
int acpica_enter_sleep(uint8_t state);

/* 最近一次唤醒源：0=未知，1=电源键，2=其他（GPE/定时器）。 */
const char *acpica_last_wake_source(void);

/* 已进入睡眠的次数（诊断用）。 */
uint32_t acpica_sleep_count(void);

/* 已处理的 SCI 中断次数（诊断用：为 0 说明硬件中断没送到）。 */
uint32_t acpica_sci_count(void);

/* 打印 ACPICA 运行期状态（shell 的 acpi 命令使用）。 */
void acpica_dump(void);

/* RSDP 物理地址（由 kernel/acpi.c 实现）。 */
uint32_t acpi_find_rsdp_phys(void);

#endif /* SAYLTER_ACPICA_GLUE_H */
