/* ============================================================
 * smp.h - 多核（SMP）初始化与同步原语
 * ------------------------------------------------------------
 * 第一阶段目标：
 *   - 从 ACPI MADT 发现所有 CPU 与 IOAPIC
 *   - 初始化 BSP 的 Local APIC
 *   - 用 INIT-SIPI-SIPI 把每个 AP 拉进 64 位长模式
 *   - 每个 AP 用独立内核栈 + 共享 IDT 进入空闲循环
 *   - 提供自旋锁与原子计数，供后续并行任务使用
 *
 * 边界：中断仍由 8259 送到 BSP（LINT0 = ExtINT），IOAPIC 只做
 * 发现不做重定向。这样做是有意的——启动链路与中断链路分开改，
 * 任何一步出问题都能定位到具体模块。
 * ============================================================ */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define SMP_MAX_CPUS 16

/* 自旋锁：多核共享状态的唯一互斥手段（没有调度器阻塞语义） */
typedef volatile int smp_lock_t;

void smp_spin_lock(smp_lock_t *lock);
void smp_spin_unlock(smp_lock_t *lock);
int  smp_spin_trylock(smp_lock_t *lock);

/* 初始化多核。返回在线 CPU 数（含 BSP）。
 * 1 表示单核运行或 SMP 不可用（没有 MADT / 没有 APIC）。 */
int smp_init(void);

int      smp_cpu_count(void);        /* MADT 中可用的 CPU 总数 */
int      smp_online_count(void);     /* 当前在线数 */
int      smp_current_cpu(void);      /* 当前核索引，0..count-1 */
uint8_t  smp_cpu_apic_id(int index);
int      smp_cpu_is_online(int index);
int      smp_ioapic_count(void);
uint32_t smp_ioapic_addr(int index);
uint32_t smp_lapic_base(void);

/* 打印 CPU / IOAPIC 摘要（shell 的 smp 命令） */
void smp_dump(void);

/* ============================================================
 * 并行工作池
 * ------------------------------------------------------------
 * BSP 发布一个按索引分片的任务，所有在线 AP 通过 0xF0 IPI 被唤醒，
 * 与 BSP 一起用原子取号的方式领取分片。回调必须满足：
 *   1. 无锁、无阻塞、不调用 printk / 文件系统；
 *   2. 不同 index 之间不共享可写状态（例如只写各自的像素行区间）；
 *   3. 不访问按需分页区（AP 的 TLB 不做跨核 shootdown）。
 * 回调返回后 AP 回到 hlt，直到下一次 IPI。
 * ============================================================ */
typedef void (*smp_work_fn)(uint32_t index, uint32_t arg);

/* 同步执行 count 个分片；所有 AP 完成后才返回。
 * 单核时退化为 BSP 串行执行。 */
void smp_parallel_for(uint32_t count, smp_work_fn fn, uint32_t arg);

/* 每核累计执行的分片数（任务管理器据此显示逐核占用） */
uint32_t smp_cpu_work_units(int index);
int      smp_parallel_available(void);
uint32_t smp_parallel_timeouts(void);

/* 自检：并行执行 32 个分片并核对结果。返回 1 通过。 */
int smp_parallel_selftest(void);

#endif /* SMP_H */
