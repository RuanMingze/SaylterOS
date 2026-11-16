/* ============================================================
 * smp.c - 多核启动与同步
 * ------------------------------------------------------------
 * 启动序列（每个 AP 一次）：
 *   INIT IPI  -> 等 10ms
 *   SIPI(0x07)-> 等 200us
 *   SIPI(0x07)-> 轮询在线标志
 * AP 在 0x7000 的 trampoline 里完成 16->32->64 位切换，之后调用
 * smp_ap_entry：装载共享 IDT、初始化自己的 LAPIC、置在线标志、
 * 进入 hlt 空闲循环。
 *
 * 0x7000..0x7FFF 是约定的 trampoline 页；0x7DF0 起是 bootloader
 * 留下的 VBE 信息，trampoline 只有几百字节，不会覆盖它。
 * ============================================================ */

#include <stdint.h>
#include "smp.h"
#include "apic.h"
#include "acpi.h"
#include "tty.h"
#include "io.h"
#include "net.h"

/* 串口诊断（shell.c 实现）：并行池自检结果必须能在 headless 启动时观测 */
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

#define SMP_TRAMP_BASE  0x7000u
#define SMP_STACK_SIZE  16384
#define SMP_MAX_IOAPIC  4

struct SmpCpu {
    uint8_t apic_id;
    volatile int online;
};

struct SmpIoApic {
    uint8_t  id;
    uint32_t addr;
    uint32_t gsi_base;
};

static struct SmpCpu g_cpus[SMP_MAX_CPUS];
static int g_cpu_count = 0;

static struct SmpIoApic g_ioapics[SMP_MAX_IOAPIC];
static int g_ioapic_count = 0;

static uint32_t g_lapic_phys = 0;
static volatile int g_online_count = 0;

/* 每核独立栈：AP 从 trampoline 进来时 RSP 就指向这里 */
static uint8_t g_ap_stacks[SMP_MAX_CPUS][SMP_STACK_SIZE]
    __attribute__((aligned(16)));

/* trampoline 符号（ap_tramp.asm） */
extern uint8_t  ap_tramp_start[];
extern uint8_t  ap_tramp_end[];
extern uint32_t ap_tramp_cr3_off;
extern uint64_t ap_tramp_entry_off;
extern uint64_t ap_tramp_stack_off;

/* ============================================================
 * 自旋锁
 * ============================================================ */

void smp_spin_lock(smp_lock_t *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            __asm__ volatile ("pause");
        }
    }
}

void smp_spin_unlock(smp_lock_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

int smp_spin_trylock(smp_lock_t *lock) {
    return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0;
}

/* ============================================================
 * ACPI 表遍历
 * ============================================================ */

static uint32_t smp_read_u32(volatile uint8_t *p) {
    return *(volatile uint32_t *)p;
}

static uint64_t smp_read_u64(volatile uint8_t *p) {
    return *(volatile uint64_t *)p;
}

static int smp_sig4(volatile uint8_t *p, const char *sig) {
    return p[0] == (uint8_t)sig[0] && p[1] == (uint8_t)sig[1] &&
           p[2] == (uint8_t)sig[2] && p[3] == (uint8_t)sig[3];
}

/* RSDP -> RSDT/XSDT -> 找签名 "APIC" 的 MADT，返回物理地址 */
static uint32_t smp_find_madt(uint32_t rsdp_phys) {
    if (!rsdp_phys) return 0;

    volatile uint8_t *rsdp = (volatile uint8_t *)(uintptr_t)rsdp_phys;
    if (!smp_sig4(rsdp, "RSD ")) return 0;

    uint8_t revision = rsdp[15];
    uint32_t root = 0;
    int is64 = 0;

    if (revision >= 2) {
        uint64_t xsdt = smp_read_u64(rsdp + 24);
        root = (uint32_t)xsdt;
        is64 = 1;
    } else {
        root = smp_read_u32(rsdp + 16);
    }
    if (!root) return 0;

    volatile uint8_t *tbl = (volatile uint8_t *)(uintptr_t)root;
    uint32_t length = smp_read_u32(tbl + 4);
    if (length < 36 || length > 4u * 1024u * 1024u) return 0;

    uint32_t stride = is64 ? 8u : 4u;
    uint32_t count = (length - 36u) / stride;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = 0;
        if (is64) {
            entry = (uint32_t)smp_read_u64(tbl + 36 + i * 8);
        } else {
            entry = smp_read_u32(tbl + 36 + i * 4);
        }
        if (!entry) continue;

        volatile uint8_t *e = (volatile uint8_t *)(uintptr_t)entry;
        if (smp_sig4(e, "APIC")) return entry;
    }

    return 0;
}

/* MADT：头部 44 字节（标准 36 + LAPIC 地址 + flags），其后是变长记录 */
static int smp_parse_madt(uint32_t madt) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)madt;

    uint32_t length = smp_read_u32(p + 4);
    if (length < 44) return 0;

    g_lapic_phys = smp_read_u32(p + 36);

    uint32_t off = 44;
    while (off + 2 <= length) {
        uint8_t type = p[off];
        uint8_t rlen = p[off + 1];
        if (rlen < 2 || off + rlen > length) break;

        if (type == 0 && rlen >= 8) {
            /* Processor Local APIC：flags bit0 = 可用 */
            uint8_t apic_id = p[off + 3];
            uint32_t flags = smp_read_u32(p + off + 4);
            if ((flags & 1u) && g_cpu_count < SMP_MAX_CPUS) {
                g_cpus[g_cpu_count].apic_id = apic_id;
                g_cpus[g_cpu_count].online = 0;
                g_cpu_count++;
            }
        } else if (type == 1 && rlen >= 12) {
            /* IOAPIC：只记录，不改路由 */
            if (g_ioapic_count < SMP_MAX_IOAPIC) {
                g_ioapics[g_ioapic_count].id = p[off + 2];
                g_ioapics[g_ioapic_count].addr = smp_read_u32(p + off + 4);
                g_ioapics[g_ioapic_count].gsi_base = smp_read_u32(p + off + 8);
                g_ioapic_count++;
            }
        }

        off += rlen;
    }

    return g_cpu_count > 0 ? 1 : 0;
}

/* ============================================================
 * trampoline
 * ============================================================ */

static void smp_copy_trampoline(void) {
    uintptr_t src = (uintptr_t)ap_tramp_start;
    uintptr_t end = (uintptr_t)ap_tramp_end;
    uintptr_t dst = (uintptr_t)SMP_TRAMP_BASE;

    for (uintptr_t p = src; p < end; p++) {
        *(volatile uint8_t *)(dst + (p - src)) = *(volatile uint8_t *)p;
    }
}

static uint32_t smp_tramp_off(uintptr_t sym) {
    return (uint32_t)(sym - (uintptr_t)ap_tramp_start);
}

static void smp_fill_trampoline(uint64_t entry, uint64_t stack_top) {
    uint64_t cr3 = 0;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint32_t off_cr3 = smp_tramp_off((uintptr_t)&ap_tramp_cr3_off);
    uint32_t off_entry = smp_tramp_off((uintptr_t)&ap_tramp_entry_off);
    uint32_t off_stack = smp_tramp_off((uintptr_t)&ap_tramp_stack_off);

    *(volatile uint32_t *)(uintptr_t)(SMP_TRAMP_BASE + off_cr3) = (uint32_t)cr3;
    *(volatile uint64_t *)(uintptr_t)(SMP_TRAMP_BASE + off_entry) = entry;
    *(volatile uint64_t *)(uintptr_t)(SMP_TRAMP_BASE + off_stack) = stack_top;
}

/* ============================================================
 * 延时
 * ============================================================ */

static void smp_delay_ms(uint32_t ms) {
    uint32_t t0 = net_now_ms();
    uint64_t guard = 0;

    while ((net_now_ms() - t0) < ms) {
        __asm__ volatile ("pause");
        if (++guard > 300000000ull) break;
    }
}

/* ============================================================
 * 并行工作池
 * ------------------------------------------------------------
 * 这是"发现多核之后真正让多核干活"的部分：BSP 通过 0xF0 IPI
 * 唤醒所有 AP，AP 与 BSP 用原子取号领取分片，全部完成后 BSP 返回。
 *
 * 等待采用 cli 检查 + sti;hlt：x86 保证 STI 后的下一条指令执行前
 * 不响应中断，因此 "sti; hlt" 是原子睡眠，随后到达的 IPI 一定能
 * 把 AP 唤醒，不会出现"检查时 epoch 没变、刚 hlt 就变"的丢唤醒。
 *
 * 超时保护：某个 AP 若因虚拟化时序没收到 IPI，BSP 最多自旋
 * SMP_SPIN_LIMIT 次后放弃并累加 timeouts，绝不永久卡住 GUI。
 * ============================================================ */

#define SMP_IPI_VECTOR   0xF0u
/* 等待上限刻意压到约 1-5ms（QEMU 下 100 万次 pause 约 1-3ms）。
 * 这颗 AP 若因虚拟化时序丢 IPI，BSP 不能为它无限等待；
 * 超时后由 BSP 补做未完成分片，帧时间仍然可控。 */
#define SMP_SPIN_LIMIT   1000000ull
#define SMP_MAX_SLICES   64u

static volatile uint32_t g_work_epoch = 0;
static volatile uint32_t g_work_next = 0;
static volatile uint32_t g_work_count = 0;
static volatile uint32_t g_work_done = 0;
static volatile uint32_t g_work_arg = 0;
static volatile uint32_t g_work_workers = 1;
static volatile uint32_t g_cpu_rank[SMP_MAX_CPUS];
static volatile uint8_t  g_slice_done[SMP_MAX_SLICES];
static volatile uint32_t g_cpu_units[SMP_MAX_CPUS];
static volatile uint32_t g_parallel_timeouts = 0;
static smp_work_fn g_work_fn = 0;
static int g_bsp_index = 0;
static volatile int g_parallel_enabled = 0;

int smp_parallel_available(void) {
    return g_parallel_enabled != 0;
}

uint32_t smp_cpu_work_units(int index) {
    if (index < 0 || index >= SMP_MAX_CPUS) return 0;
    return __atomic_load_n(&g_cpu_units[index], __ATOMIC_RELAXED);
}

uint32_t smp_parallel_timeouts(void) {
    return __atomic_load_n(&g_parallel_timeouts, __ATOMIC_RELAXED);
}

void smp_parallel_for(uint32_t count, smp_work_fn fn, uint32_t arg) {
    if (!fn || count == 0) return;
    if (count > SMP_MAX_SLICES) count = SMP_MAX_SLICES;

    /* 硬件不满足（单核 / 无 LAPIC）时退化为 BSP 串行，
     * 保证调用方永远能拿到完整结果，不依赖多核可用性。 */
    if (!(g_online_count > 1) || !apic_available()) {
        for (uint32_t i = 0; i < count; i++) {
            fn(i, arg);
        }
        __atomic_add_fetch(&g_cpu_units[g_bsp_index], count, __ATOMIC_RELAXED);
        return;
    }

    /* 静态轮转分片：按 rank 取 index = rank, rank+workers, ...
     * 不能用"原子抢号"——BSP 不经过 IPI 唤醒，永远比 AP 快，
     * 会把几乎所有分片抢走，任务管理器看到的仍然是单核满载。
     * 静态分配保证每个在线的核拿到数量几乎相等的份额。 */
    for (uint32_t i = 0; i < count; i++) {
        __atomic_store_n(&g_slice_done[i], 0, __ATOMIC_RELAXED);
    }

    int workers = 1;
    __atomic_store_n(&g_cpu_rank[g_bsp_index], 0, __ATOMIC_RELAXED);
    for (int i = 0; i < g_cpu_count; i++) {
        if (i == g_bsp_index) continue;
        if (!__atomic_load_n(&g_cpus[i].online, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_cpu_rank[i], 0xFFFFFFFFu, __ATOMIC_RELAXED);
            continue;
        }
        __atomic_store_n(&g_cpu_rank[i], (uint32_t)workers, __ATOMIC_RELAXED);
        workers++;
    }
    __atomic_store_n(&g_work_workers, (uint32_t)workers, __ATOMIC_RELEASE);

    g_work_fn = fn;
    g_work_arg = arg;
    __atomic_store_n(&g_work_count, count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_work_done, 0, __ATOMIC_RELEASE);

    /* 先推进 epoch，再发 IPI：AP 被唤醒后读到的必然是新的工作代次 */
    __atomic_add_fetch(&g_work_epoch, 1, __ATOMIC_SEQ_CST);

    int aps = 0;
    for (int i = 0; i < g_cpu_count; i++) {
        if (i == g_bsp_index) continue;
        if (!__atomic_load_n(&g_cpus[i].online, __ATOMIC_ACQUIRE)) continue;
        if (apic_send_ipi(g_cpus[i].apic_id, SMP_IPI_VECTOR)) {
            aps++;
        }
    }

    /* BSP 自己的一份 */
    for (uint32_t i = 0; i < count; i += (uint32_t)workers) {
        fn(i, arg);
        __atomic_store_n(&g_slice_done[i], 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_cpu_units[g_bsp_index], 1, __ATOMIC_RELAXED);
    }

    uint64_t guard = 0;
    while (__atomic_load_n(&g_work_done, __ATOMIC_ACQUIRE) < (uint32_t)aps) {
        __asm__ volatile ("pause");
        if (++guard > SMP_SPIN_LIMIT) {
            __atomic_add_fetch(&g_parallel_timeouts, 1, __ATOMIC_RELAXED);
            break;
        }
    }

    /* 超时兜底：任何没被 AP 领走的分片由 BSP 补做。
     * 这样即使某个 AP 偶发丢 IPI，画面/数据仍然完整，只是少了并行度。 */
    for (uint32_t i = 0; i < count; i++) {
        if (__atomic_load_n(&g_slice_done[i], __ATOMIC_ACQUIRE)) continue;
        fn(i, arg);
        __atomic_store_n(&g_slice_done[i], 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_cpu_units[g_bsp_index], 1, __ATOMIC_RELAXED);
    }

    g_work_fn = 0;
}

/* 自检：32 个分片各自写一个唯一值，全部核对 */
static volatile uint32_t g_selftest_result[32];

static void smp_selftest_slice(uint32_t index, uint32_t arg) {
    (void)arg;
    if (index < 32) {
        g_selftest_result[index] = index * 3u + 7u;
    }
}

int smp_parallel_selftest(void) {
    for (uint32_t i = 0; i < 32; i++) {
        g_selftest_result[i] = 0;
    }
    smp_parallel_for(32, smp_selftest_slice, 0);
    for (uint32_t i = 0; i < 32; i++) {
        if (g_selftest_result[i] != i * 3u + 7u) return 0;
    }
    /* 只有自检成功才打开并行入口；失败时调用方继续走串行路径，
     * 不会因为某颗 AP 的 IPI 偶发丢失而让 GUI 每帧等超时。 */
    __atomic_store_n(&g_parallel_enabled, 1, __ATOMIC_RELEASE);
    return 1;
}

/* ============================================================
 * AP 入口
 * ============================================================ */

static void smp_load_idt(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;

    /* 与 pm.asm 的 build_idt64 一致：256 项 × 16 字节，位于 0x60000 */
    idtr.limit = (uint16_t)(256 * 16 - 1);
    idtr.base = 0x60000;

    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

static void smp_ap_entry(void) {
    smp_load_idt();

    if (!apic_init(g_lapic_phys)) {
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    uint8_t id = apic_local_id();
    int my_index = 0;
    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].apic_id != id) continue;

        my_index = i;
        __atomic_store_n(&g_cpus[i].online, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_online_count, 1, __ATOMIC_SEQ_CST);
        break;
    }

    /* 并行工作循环：睡眠等待 epoch 变化，醒来后领分片执行。
     * cli 期间检查 epoch，再 sti;hlt 原子睡眠，避免丢唤醒。 */
    uint32_t seen = __atomic_load_n(&g_work_epoch, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t cur = 0;

        __asm__ volatile ("cli" ::: "memory");
        cur = __atomic_load_n(&g_work_epoch, __ATOMIC_ACQUIRE);
        if (cur == seen) {
            __asm__ volatile ("sti; hlt" ::: "memory");
            continue;
        }
        __asm__ volatile ("sti" ::: "memory");
        seen = cur;

        smp_work_fn fn = g_work_fn;
        uint32_t arg = __atomic_load_n(&g_work_arg, __ATOMIC_RELAXED);
        uint32_t count = __atomic_load_n(&g_work_count, __ATOMIC_ACQUIRE);
        uint32_t workers = __atomic_load_n(&g_work_workers, __ATOMIC_ACQUIRE);
        uint32_t rank = __atomic_load_n(&g_cpu_rank[my_index], __ATOMIC_RELAXED);

        if (!fn || workers == 0 || rank >= workers) {
            __atomic_add_fetch(&g_work_done, 1, __ATOMIC_SEQ_CST);
            continue;
        }

        /* 与 BSP 使用同一套轮转规则，份额固定、互不重叠 */
        for (uint32_t i = rank; i < count; i += workers) {
            fn(i, arg);
            __atomic_store_n(&g_slice_done[i], 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_cpu_units[my_index], 1, __ATOMIC_RELAXED);
        }

        __atomic_add_fetch(&g_work_done, 1, __ATOMIC_SEQ_CST);
    }
}

/* ============================================================
 * 启动单个 AP
 * ============================================================ */

static int smp_boot_ap(int idx) {
    uint8_t id = g_cpus[idx].apic_id;
    uint8_t vector = (uint8_t)(SMP_TRAMP_BASE >> 12);

    smp_fill_trampoline((uint64_t)(uintptr_t)smp_ap_entry,
                        (uint64_t)(uintptr_t)&g_ap_stacks[idx][SMP_STACK_SIZE]);

    /* INIT-SIPI-SIPI 序列在虚拟机上偶发丢失（尤其是核数多、
     * 首次 SIPI 与 AP 上电时序重叠时）。标准做法是重试整条序列，
     * 这里最多 3 轮，每轮之间留足 20ms。 */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!apic_send_init(id)) continue;
        smp_delay_ms(50);

        if (!apic_send_sipi(id, vector)) continue;
        smp_delay_ms(10);

        if (!apic_send_sipi(id, vector)) continue;

        for (int t = 0; t < 600; t++) {
            if (__atomic_load_n(&g_cpus[idx].online, __ATOMIC_ACQUIRE)) return 1;
            smp_delay_ms(1);
        }

        smp_delay_ms(40);
    }

    return 0;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int smp_cpu_count(void) {
    return g_cpu_count;
}

int smp_online_count(void) {
    return g_online_count;
}

int smp_current_cpu(void) {
    uint8_t id = apic_local_id();

    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].apic_id == id) return i;
    }
    return 0;
}

uint8_t smp_cpu_apic_id(int index) {
    if (index < 0 || index >= g_cpu_count) return 0;
    return g_cpus[index].apic_id;
}

int smp_cpu_is_online(int index) {
    if (index < 0 || index >= g_cpu_count) return 0;
    return g_cpus[index].online != 0;
}

int smp_ioapic_count(void) {
    return g_ioapic_count;
}

uint32_t smp_ioapic_addr(int index) {
    if (index < 0 || index >= g_ioapic_count) return 0;
    return g_ioapics[index].addr;
}

uint32_t smp_lapic_base(void) {
    return g_lapic_phys;
}

void smp_dump(void) {
    if (g_cpu_count == 0) {
        printk("SMP: MADT not found, running single-core\n");
        return;
    }

    printk("SMP: %d CPU(s) reported, %d online\n",
           g_cpu_count, g_online_count);

    int self = smp_current_cpu();
    for (int i = 0; i < g_cpu_count; i++) {
        printk("  cpu%d  apic_id=%u  %s%s\n",
               i, (unsigned)g_cpus[i].apic_id,
               g_cpus[i].online ? "online" : "offline",
               (i == self) ? "  <- current" : "");
    }

    printk("  local APIC: 0x%x\n", (unsigned)g_lapic_phys);

    for (int i = 0; i < g_ioapic_count; i++) {
        printk("  ioapic%d  id=%u  addr=0x%x  gsi_base=%u\n",
               i, (unsigned)g_ioapics[i].id,
               (unsigned)g_ioapics[i].addr,
               (unsigned)g_ioapics[i].gsi_base);
    }
}

int smp_init(void) {
    uint32_t rsdp = acpi_find_rsdp_phys();
    uint32_t madt = smp_find_madt(rsdp);

    if (!rsdp) {
        printk("[SMP] RSDP not found, single-core\n");
        return 1;
    }
    if (!madt) {
        printk("[SMP] MADT not found (rsdp=0x%x)\n", rsdp);
        return 1;
    }
    if (!smp_parse_madt(madt)) {
        printk("[SMP] MADT parse failed (madt=0x%x)\n", madt);
        return 1;
    }

    /* 注意：不要改页表把 LAPIC 段标成 PCD/PWT。
     * 在 QEMU TCG 下，LAPIC 由专门的 MMIO 内存区域模拟；一旦该页
     * 的内存类型被改写，TCG 会把它当成普通内存访问，读回全 1，
     * SVR/ICR 写入全部丢失。真机的 LAPIC 区域通常已由固件 MTRR
     * 声明为 UC，PTE 不需要也不应该再动。 */

    if (!apic_init(g_lapic_phys)) {
        printk("[SMP] Local APIC init failed (base=0x%x)\n", g_lapic_phys);
        g_cpu_count = 0;
        return 1;
    }

    /* BSP 标记在线 */
    int bsp = smp_current_cpu();
    if (bsp >= 0 && bsp < g_cpu_count) {
        g_bsp_index = bsp;
        g_cpus[bsp].online = 1;
    }
    g_online_count = 1;

    if (g_cpu_count <= 1) {
        printk("[SMP] MADT lists %d usable CPU(s)\n", g_cpu_count);
        return 1;
    }

    smp_copy_trampoline();

    for (int i = 0; i < g_cpu_count; i++) {
        if (i == bsp) continue;
        int ok = smp_boot_ap(i);
        printk("[SMP] ap%d apic_id=%u %s\n",
               i, (unsigned)g_cpus[i].apic_id, ok ? "online" : "timeout");
    }

    /* 工作池自检：真的把分片分给 AP 执行并核对结果。
     * 失败时只记录，系统仍以单核串行方式继续工作。 */
    if (g_online_count > 1) {
        int ok = smp_parallel_selftest();
        printk("[SMP] parallel pool: %s (timeouts=%u)\n",
               ok ? "ready" : "FAILED",
               (unsigned)g_parallel_timeouts);

        kserial_puts("[SMP] parallel pool ");
        kserial_puts(ok ? "READY" : "FAILED");
        kserial_puts(" timeouts=");
        kserial_dec32(g_parallel_timeouts);
        kserial_puts(" units0=");
        kserial_dec32(smp_cpu_work_units(g_bsp_index));
        kserial_puts(" units1=");
        kserial_dec32(smp_cpu_work_units(g_bsp_index == 0 ? 1 : 0));
        kserial_puts("\r\n");
    }

    return g_online_count;
}
