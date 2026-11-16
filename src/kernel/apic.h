/* ============================================================
 * apic.h - Local APIC 驱动（xAPIC，MMIO）
 * ------------------------------------------------------------
 * 只承担多核必需的部分：
 *   - 读取本核 APIC ID（AP 用来确认自己是哪一颗）
 *   - 软件使能 APIC，配置 LVT 让 PIC 通道继续工作（LINT0 = ExtINT）
 *   - 发送 INIT / SIPI / 普通 IPI
 * 不启用 LAPIC 定时器，也不接管 IOAPIC 重定向，中断仍由 8259
 * 送到 BSP——多核第一阶段的目标是"所有核都能跑起来"，把中断
 * 路由迁移单独立项，避免一次改动同时动启动链路与中断链路。
 * ============================================================ */

#ifndef APIC_H
#define APIC_H

#include <stdint.h>

int      apic_init(uint32_t lapic_phys);   /* 0 表示使用默认 0xFEE00000 */
int      apic_available(void);
uint32_t apic_base(void);
uint8_t  apic_local_id(void);

/* 写 LAPIC EOI。ISR 里如果走 APIC 路径需要调用。 */
void apic_eoi(void);

/* 发送 INIT / STARTUP / fixed IPI 到指定 APIC ID。
 * 返回值：1 = 已发出，0 = ICR 一直忙（目标不存在或总线异常）。 */
int apic_send_init(uint8_t dest_apic_id);
int apic_send_sipi(uint8_t dest_apic_id, uint8_t vector);
int apic_send_ipi(uint8_t dest_apic_id, uint8_t vector);

#endif /* APIC_H */
