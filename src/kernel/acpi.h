/* ============================================================
 * acpi.h - SaylterOS 最小 ACPI 支持（S5 关机 + 电源键事件）
 * ------------------------------------------------------------
 * 本阶段只实现"读取固定表结构"这一层：
 *   RSDP → RSDT/XSDT → FADT → PM1 寄存器块
 * 不解析 DSDT/SSDT 的 AML，因此 \_S5 包里的 SLP_TYP 取常见默认值，
 * 并保留由 shell 命令覆盖的入口（poweroff <slp_typ>）。
 * 后续引入 uACPI/ACPICA 时，只需替换本模块的初始化与取值来源。
 * ============================================================ */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* 默认 S5 SLP_TYP。准确值来自 DSDT 的 \_S5 包，本阶段不解析 AML。
 * 传统 BIOS 多为 5，QEMU / 部分 UEFI 用 0；acpi_power_off() 会在
 * 默认值无效时自动回退到另一个 soft-off 候选值，两者都不进 S1/S3/S4。 */
#define ACPI_S5_SLP_TYP_DEFAULT 5

/* 定位并解析 ACPI 表、使能 PM1、打开电源按钮事件。
 * 返回 1 表示可用，0 表示未找到有效 FADT。 */
int acpi_init(void);

/* ACPI 是否已成功初始化 */
int acpi_available(void);

/* FADT 中的 PM1 寄存器端口（不可用时为 0） */
uint16_t acpi_pm1a_evt_port(void);
uint16_t acpi_pm1a_cnt_port(void);
uint16_t acpi_pm1b_cnt_port(void);

/* FADT 的 SCI 中断号（通常是 IRQ9；0 表示不可用）。
 * 中断层用它把硬件 IRQ 转发给 ACPICA 的 SCI 分发器。 */
uint16_t acpi_sci_irq(void);

/* 轮询 PM1_STS.PWRBTN_STS。
 * 命中时写 1 清除该状态位并返回 1，否则返回 0。
 * 不依赖 SCI 中断路由，可在忙等循环里周期调用。 */
int acpi_poll_power_button(void);

/* 用默认 SLP_TYP 执行 S5 软关机；成功进入 S5 后不会返回。
 * 硬件不支持时返回 0。 */
int acpi_power_off(void);

/* 用指定 SLP_TYP（0..7）执行 S5 软关机 */
int acpi_power_off_with(uint8_t slp_typ);

/* 覆盖默认 S5 SLP_TYP */
void acpi_set_s5_slp_typ(uint8_t slp_typ);

/* 当前使用的 S5 SLP_TYP */
uint8_t acpi_get_s5_slp_typ(void);

/* 打印 ACPI 摘要（shell 的 acpi 命令使用） */
void acpi_dump(void);

/* 在 EBDA 与 0xE0000..0xFFFFF 中查找 RSDP，返回其物理地址（0 = 未找到）。
 * ACPICA 的 AcpiOsGetRootPointer 直接复用这个结果。 */
uint32_t acpi_find_rsdp_phys(void);

#endif /* ACPI_H */
