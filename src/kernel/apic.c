/* ============================================================
 * apic.c - Local APIC（xAPIC MMIO）最小实现
 * ------------------------------------------------------------
 * 4GB 恒等映射由 pm.asm 建立，因此 LAPIC 的物理地址可以直接当
 * 指针访问，不需要额外的 MMIO 映射层。
 * 所有寄存器操作都加编译屏障，避免编译器把多次写相邻寄存器重排。
 * ============================================================ */

#include <stdint.h>
#include "apic.h"
#include "io.h"

/* 寄存器偏移（相对 LAPIC 基址） */
#define LAPIC_ID          0x020
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_ESR         0x280
#define LAPIC_ICR_LO      0x300
#define LAPIC_ICR_HI      0x310
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370

/* ICR 位域 */
#define ICR_DELIVERY_STATUS (1u << 12)
#define ICR_LEVEL_ASSERT    (1u << 14)
#define ICR_MODE_INIT       (5u << 8)
#define ICR_MODE_STARTUP    (6u << 8)

static volatile uint32_t *g_lapic = 0;
static uint32_t g_lapic_phys = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return g_lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    g_lapic[reg / 4] = value;
    __asm__ volatile ("" ::: "memory");
}

int apic_available(void) {
    return g_lapic != 0;
}

uint32_t apic_base(void) {
    return g_lapic_phys;
}

uint8_t apic_local_id(void) {
    if (!g_lapic) return 0;
    return (uint8_t)((lapic_read(LAPIC_ID) >> 24) & 0xFFu);
}

void apic_eoi(void) {
    if (!g_lapic) return;
    lapic_write(LAPIC_EOI, 0);
}

static int apic_wait_icr(void) {
    for (uint32_t i = 0; i < 1000000u; i++) {
        if ((lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_STATUS) == 0) return 1;
    }
    return 0;
}

static int apic_send_raw(uint8_t dest, uint32_t icr_lo) {
    if (!g_lapic) return 0;

    lapic_write(LAPIC_ICR_HI, (uint32_t)dest << 24);
    lapic_write(LAPIC_ICR_LO, icr_lo);
    return apic_wait_icr();
}

int apic_send_init(uint8_t dest_apic_id) {
    return apic_send_raw(dest_apic_id, ICR_LEVEL_ASSERT | ICR_MODE_INIT);
}

int apic_send_sipi(uint8_t dest_apic_id, uint8_t vector) {
    return apic_send_raw(dest_apic_id,
                         ICR_LEVEL_ASSERT | ICR_MODE_STARTUP | (uint32_t)vector);
}

int apic_send_ipi(uint8_t dest_apic_id, uint8_t vector) {
    return apic_send_raw(dest_apic_id,
                         ICR_LEVEL_ASSERT | (uint32_t)vector);
}

int apic_init(uint32_t lapic_phys) {
    if (!lapic_phys) lapic_phys = 0xFEE00000u;

    g_lapic_phys = lapic_phys;
    g_lapic = (volatile uint32_t *)(uintptr_t)lapic_phys;

    /* ID 寄存器全 1 说明这一段地址没有 APIC 响应 */
    uint32_t id = lapic_read(LAPIC_ID);
    if (id == 0xFFFFFFFFu) {
        g_lapic = 0;
        g_lapic_phys = 0;
        return 0;
    }

    /* 不写 IA32_APIC_BASE。
     * 实测在 QEMU TCG 下，对该 MSR 执行 wrmsr（即使写入的基址与读回
     * 值完全相同）会让 LAPIC 的 MMIO 区域立刻失效：ID/SVR 全部读回
     * 0xFFFFFFFF，SVR/ICR 的写入被丢弃，SIPI 永远发不出去。
     * 固件在把控制权交给内核之前已经使能了 APIC（首次读 ID 能读到
     * 真实值即为证据），因此这里不需要也不应该再碰这个寄存器。 */

    /* 软件使能 + spurious vector 0xFF（IDT 的默认门会处理它） */
    lapic_write(LAPIC_SVR, 0x1FF);

    /* 接收所有优先级 */
    lapic_write(LAPIC_TPR, 0);

    /* 清 ESR：先写再读，第二次写 0 才能真正清除 */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_ESR, 0);

    /* 定时器与错误中断全部屏蔽；LINT0 保持 ExtINT，让 8259 继续送达 */
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    lapic_write(LAPIC_LVT_ERROR, 0x10000);
    lapic_write(LAPIC_LVT_LINT0, 0x700);
    lapic_write(LAPIC_LVT_LINT1, 0x10000);

    return 1;
}
