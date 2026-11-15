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

/* 已处理的 SCI 中断次数（诊断用：为 0 说明硬件中断没送到）。 */
uint32_t acpica_sci_count(void);

/* 打印 ACPICA 运行期状态（shell 的 acpi 命令使用）。 */
void acpica_dump(void);

/* RSDP 物理地址（由 kernel/acpi.c 实现）。 */
uint32_t acpi_find_rsdp_phys(void);

#endif /* SAYLTER_ACPICA_GLUE_H */
