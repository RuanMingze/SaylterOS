/* ============================================================
 * vm.c - 4KB 分页 / 按需分页 / swap 换入换出
 * ------------------------------------------------------------
 * 物理内存事实（与 pm.asm / mem.c / linker.ld 保持一致）：
 *   0x000000 - 0x0FFFFF  引导与 BIOS 区
 *   0x100000 - 0x3F0000  内核镜像
 *   0x3F0000 - 0x400000  内核初始栈
 *   0x400000 - 0xA00000  内核堆（mem.c）
 *   0xA00000 - 0x1200000 VBE backbuffer
 *   0x1200000 - 0x134E100 内核 BSS
 *   => 24MB（0x1800000）以上没有任何内核静态占用，作为 4KB 帧池起点。
 *
 * 页表事实：
 *   PML4 = 0x70000，PDPT = 0x71000，PD[0..3] = 0x72000..0x75000。
 *   引导阶段所有项都是 2MB 大页（PS=1），本文件在启用按需区时把
 *   0x20000000 起的 PD 项逐个替换成指向 4KB 页表的普通项。
 *   由于 4GB 恒等映射，页表本身的物理地址可以直接当指针使用。
 *
 * 按需分页区：0x20000000 起 16MB（4096 页）。
 *   该虚拟窗口在 QEMU 的 RAM 之外（RAM 只有 128MB），拆分前访问它
 *   只会读到 0xFF 垃圾；拆分后所有 PTE 置为不存在，第一次访问触发
 *   #PF，由 page_fault_handler 分配物理帧并填充 PTE。
 *
 * swap：
 *   数据存放在 SlyzFS 的受保护文件 etc\pagefile.os 中（8MB）。
 *   文件是"文件"而不是裸卷：句柄由 bridge 持有，读写绕过 shell 日志。
 *   保护位保证除显式提权（protect off ... --force）外连 root 也无法删除。
 *
 * 并发边界：
 *   当前内核只有 BSP 执行任务代码；AP 在 hlt 空闲循环中，不会访问
 *   按需分页区。因此这里只在本地执行 invlpg，不做跨核 TLB shootdown。
 *   后续若 AP 开始执行用户态任务，需要增加 IPI 广播刷 TLB。
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "vm.h"
#include "io.h"
#include "tty.h"
#include "bsod.h"

/* ---------- bridge 提供的 SlyzFS 接口 ---------- */
extern int slyzfs_swap_attach(void);
extern int slyzfs_swap_read(uint32_t offset, uint8_t *buf, int n);
extern int slyzfs_swap_write(uint32_t offset, const uint8_t *buf, int n);
extern uint32_t slyzfs_swap_bytes(void);
extern int slyzfs_is_protected(const char *path);

/* ---------- shell.c 提供的串口输出（启动期间不污染屏幕） ---------- */
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);
extern void kserial_hex32(uint32_t v);
extern void kserial_putchar(char c);

/* ============================================================
 * 常量
 * ============================================================ */

/* 页表物理地址（pm.asm build_page_tables 建立） */
#define VM_PML4_PHYS      0x70000u
#define VM_PDPT_PHYS      0x71000u
#define VM_PD_PHYS        0x72000u

/* #PF 处理函数指针槽：pm.asm 在实模式启动时清零，C 层在 vm_init 写入。
 * 选择 0x7DE0 是因为它在 VBE 结果结构（0x7DF0）之前、页表之后，
 * 不与 SCI 回调槽（0x7DE8）冲突。 */
#define VM_PF_HOOK_ADDR   0x7DE0u

/* 帧池 */
#define VM_FRAME_BASE     0x1800000u          /* 24MB */
#define VM_FRAME_RESERVE  0x400000u           /* 尾部保留 4MB 给栈/临时缓冲 */
#define VM_MAX_FRAMES     65536u              /* 最多管理 256MB 物理内存 */
#define VM_MAX_WORDS       (VM_MAX_FRAMES / 32u)

/* Slyz 模块固定映射区：驱动 0x02000000、SlyzFS 0x04000000、
 * VizCormin 0x08000000、Slyz libc 0x09000000。这些地址不是由
 * 帧分配器管理的普通内存——模块的代码/BSS 就住在里面。帧池如果
 * 从 24MB 一路向上分配，超过 32MB 后就会把正在运行的驱动 BSS
 * 覆盖掉（表现为音频任务跳到 0x0200xxxx 触发 #GP）。
 *
 * 因此把整个固定模块区在位图里预先标记为占用，帧池只使用
 * [24MB, 32MB) 与 [160MB, RAM) 两段。 */
#define VM_MODULE_ARENA_BASE 0x02000000u      /* 32MB */
#define VM_MODULE_ARENA_END  0x0A000000u      /* 160MB */

/* PTE 标志 */
#define VM_PTE_PRESENT    0x001ull
#define VM_PTE_WRITE      0x002ull
#define VM_PTE_ACCESSED   0x020ull
#define VM_PTE_DIRTY      0x040ull
#define VM_PTE_SWAP       0x200ull            /* 软件位：PTE 记录的是 swap 槽号 */
#define VM_PTE_ADDR_MASK  0x000FFFFFFFFFF000ull
#define VM_PTE_FRAME_MASK 0x00000000FFFFF000ull

/* swap 文件 */
#define VM_SWAP_PATH      "etc\\pagefile.os"
#define VM_SWAP_BYTES     (VM_SWAP_SLOTS * VM_PAGE_SIZE)

/* ============================================================
 * 状态
 * ============================================================ */

static uint32_t g_frame_bitmap[VM_MAX_WORDS];
static uint32_t g_frame_base;       /* 首帧物理地址 */
static uint32_t g_frame_count;      /* 可用帧数 */
static uint32_t g_frame_free;
static uint32_t g_frame_scan;       /* 下一次分配起点，避免每次都从头扫 */
static uint64_t g_ram_bytes;

static uint64_t *g_demand_tables[VM_DEMAND_PAGES / 512u];

static uint32_t g_resident;
static uint32_t g_resident_limit = VM_RESIDENT_LIMIT;
static uint32_t g_clock;

static uint8_t  g_swap_used[VM_SWAP_SLOTS / 8u];
static uint32_t g_swap_slots_used;
static int      g_swap_ready;
static int      g_vm_ready;

static uint32_t g_faults;
static uint32_t g_demand_faults;
static uint32_t g_prot_faults;
static uint32_t g_reserved_faults;
static uint32_t g_unknown_faults;
static uint32_t g_swap_out;
static uint32_t g_swap_in;
static uint32_t g_evictions;
static uint32_t g_oom;

/* ============================================================
 * 基础工具
 * ============================================================ */

static void vm_zero_page(uint32_t phys) {
    uint64_t *p = (uint64_t *)(uintptr_t)phys;
    for (uint32_t i = 0; i < (VM_PAGE_SIZE / 8u); i++) {
        p[i] = 0;
    }
}

static void vm_flush_page(uint64_t va) {
    __asm__ volatile ("invlpg (%0)" : : "r"((void *)(uintptr_t)va) : "memory");
}

static void vm_flush_all(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static uint8_t vm_cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

/* ============================================================
 * RAM 探测
 * ------------------------------------------------------------
 * CMOS 0x34/0x35 保存 16MB 以上内存总量，单位 64KB
 * （QEMU hw/i386/pc.c: (below_4g_mem_size - 0x1000000) / 0x10000）。
 * 注意基准是 16MB 而不是 1MB：用 1MB 作基准会在 64MB 机器上
 * 少算 15MB，帧池凭空缩水。
 * 读不到（< 32MB）时退回 32MB 的保守假设，只启用
 * 24MB..28MB 这一小段帧池。上限 256MB 是位图容量（65536 帧）决定的。
 * ============================================================ */
static uint64_t vm_detect_ram(void) {
    uint8_t lo = vm_cmos_read(0x34);
    uint8_t hi = vm_cmos_read(0x35);
    uint32_t units = ((uint32_t)hi << 8) | (uint32_t)lo;
    uint64_t ram = 0x1000000ull + (uint64_t)units * 65536ull;

    if (ram < 0x2000000ull) {
        return 0;
    }
    if (ram > 0x10000000ull) {
        ram = 0x10000000ull;
    }
    return ram;
}

/* ============================================================
 * 物理页帧分配器
 * ============================================================ */

static int vm_frame_test(uint32_t index) {
    return (g_frame_bitmap[index >> 5] & (1u << (index & 31u))) != 0;
}

static void vm_frame_mark(uint32_t index, int used) {
    uint32_t word = index >> 5;
    uint32_t bit = 1u << (index & 31u);
    if (used) {
        if (!(g_frame_bitmap[word] & bit)) {
            g_frame_bitmap[word] |= bit;
        }
    } else {
        g_frame_bitmap[word] &= ~bit;
    }
}

static uint32_t vm_alloc_frame_raw(void) {
    if (g_frame_free == 0) {
        return 0;
    }

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? g_frame_scan : 0;
        for (uint32_t i = start; i < g_frame_count; i++) {
            if (!vm_frame_test(i)) {
                vm_frame_mark(i, 1);
                g_frame_free--;
                g_frame_scan = i + 1u;
                if (g_frame_scan >= g_frame_count) {
                    g_frame_scan = 0;
                }
                return g_frame_base + i * VM_PAGE_SIZE;
            }
        }
    }

    return 0;
}

static void vm_free_frame_raw(uint32_t phys) {
    if (phys < g_frame_base) {
        return;
    }
    uint32_t index = (phys - g_frame_base) / VM_PAGE_SIZE;
    if (index >= g_frame_count) {
        return;
    }
    if (vm_frame_test(index)) {
        vm_frame_mark(index, 0);
        g_frame_free++;
        if (index < g_frame_scan) {
            g_frame_scan = index;
        }
    }
}

/* ============================================================
 * 连续大块分配（图片解码缓冲）
 * ------------------------------------------------------------
 * 帧池本身就是恒等映射的物理内存，只要找到一段连续空闲页就能
 * 直接把首地址交给调用方当指针用。这里按首次适配扫描；图片数量
 * 很少，扫描代价可以忽略。
 *
 * 注意：分配的页已被标记占用，但不会计入 g_resident（按需分页
 * 统计只覆盖用户态窗口），任务管理器的"驻留页"不会被图片撑大。
 * ============================================================ */
uint32_t vm_alloc_phys_bytes(uint32_t bytes) {
    if (bytes == 0 || g_frame_count == 0) return 0;

    uint32_t pages = (bytes + VM_PAGE_SIZE - 1u) / VM_PAGE_SIZE;
    if (pages == 0 || pages > g_frame_free) return 0;

    uint32_t run = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i < g_frame_count; i++) {
        if (!vm_frame_test(i)) {
            if (run == 0) start = i;
            run++;
            if (run == pages) {
                for (uint32_t k = start; k < start + pages; k++) {
                    vm_frame_mark(k, 1);
                }
                g_frame_free -= pages;
                if (start + pages < g_frame_scan || g_frame_scan < start) {
                    g_frame_scan = start + pages;
                    if (g_frame_scan >= g_frame_count) g_frame_scan = 0;
                }
                return g_frame_base + start * VM_PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void vm_free_phys_bytes(uint32_t phys, uint32_t bytes) {
    if (bytes == 0 || g_frame_count == 0) return;
    if (phys < g_frame_base) return;

    uint32_t start = (phys - g_frame_base) / VM_PAGE_SIZE;
    uint32_t pages = (bytes + VM_PAGE_SIZE - 1u) / VM_PAGE_SIZE;
    if (start >= g_frame_count) return;
    if (pages > g_frame_count - start) pages = g_frame_count - start;

    for (uint32_t i = start; i < start + pages; i++) {
        if (vm_frame_test(i)) {
            vm_frame_mark(i, 0);
            g_frame_free++;
        }
    }
    if (start < g_frame_scan) g_frame_scan = start;
}

/* ============================================================
 * 页表操作
 * ============================================================ */

static uint64_t *vm_pml4(void) {
    return (uint64_t *)(uintptr_t)VM_PML4_PHYS;
}

static uint64_t *vm_pdpt(void) {
    return (uint64_t *)(uintptr_t)VM_PDPT_PHYS;
}

/* 返回 va 所属的 4KB 页表指针。
 * create = 1 时若对应的 PD 项是 2MB 大页，则分配一个页帧作为页表，
 * 把整个 2MB 区域的 PTE 清零（即全部标记为"不存在"）。
 * 注意：拆分会让该 2MB 区域失去恒等映射，只允许对按需分页窗口调用。 */
static uint64_t *vm_pt_for(uint64_t va, int create) {
    uint32_t pml4_idx = (uint32_t)((va >> 39) & 0x1FFull);
    uint32_t pdpt_idx = (uint32_t)((va >> 30) & 0x1FFull);
    uint32_t pd_idx = (uint32_t)((va >> 21) & 0x1FFull);

    if (pml4_idx != 0 || pdpt_idx > 3) {
        return 0;
    }
    if ((vm_pml4()[pml4_idx] & VM_PTE_PRESENT) == 0) {
        return 0;
    }
    if ((vm_pdpt()[pdpt_idx] & VM_PTE_PRESENT) == 0) {
        return 0;
    }

    uint64_t *pd = (uint64_t *)(uintptr_t)VM_PD_PHYS + (size_t)pdpt_idx * 512u;
    uint64_t pde = pd[pd_idx];

    if (pde & 0x80ull) {
        if (!create) {
            return 0;
        }
        uint32_t frame = vm_alloc_frame_raw();
        if (frame == 0) {
            return 0;
        }
        uint64_t *pt = (uint64_t *)(uintptr_t)frame;
        for (uint32_t i = 0; i < 512u; i++) {
            pt[i] = 0;
        }
        pd[pd_idx] = (uint64_t)frame | VM_PTE_PRESENT | VM_PTE_WRITE;
        vm_flush_page(va & ~0x1FFFFFull);
        return pt;
    }

    if ((pde & VM_PTE_PRESENT) == 0) {
        return 0;
    }
    return (uint64_t *)(uintptr_t)(pde & VM_PTE_ADDR_MASK);
}

/* ============================================================
 * swap 槽位
 * ============================================================ */

static uint32_t vm_swap_alloc_slot(void) {
    for (uint32_t i = 0; i < VM_SWAP_SLOTS; i++) {
        uint32_t byte = i >> 3;
        uint32_t bit = 1u << (i & 7u);
        if ((g_swap_used[byte] & bit) == 0) {
            g_swap_used[byte] |= (uint8_t)bit;
            g_swap_slots_used++;
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static void vm_swap_free_slot(uint32_t slot) {
    if (slot >= VM_SWAP_SLOTS) {
        return;
    }
    uint32_t byte = slot >> 3;
    uint32_t bit = 1u << (slot & 7u);
    if (g_swap_used[byte] & bit) {
        g_swap_used[byte] &= (uint8_t)~bit;
        if (g_swap_slots_used > 0) {
            g_swap_slots_used--;
        }
    }
}

/* ============================================================
 * 换出 / 换入
 * ------------------------------------------------------------
 * 选择"总是写盘"而不是依赖 PTE.D：
 *   不同模拟器对 A/D 位的更新策略不一致，若把 D=0 当作 clean
 *   直接丢弃，在 A/D 位不更新的环境下会静默丢数据。这里对所有
 *   驻留页一律写 swap，以正确性优先；后续可用硬件 D 位做优化。
 * ============================================================ */

static int vm_swap_out_frame(uint32_t phys) {
    if (!g_swap_ready) {
        return -1;
    }
    uint32_t slot = vm_swap_alloc_slot();
    if (slot == 0xFFFFFFFFu) {
        return -1;
    }
    int n = slyzfs_swap_write(slot * VM_PAGE_SIZE,
                              (const uint8_t *)(uintptr_t)phys,
                              (int)VM_PAGE_SIZE);
    if (n != (int)VM_PAGE_SIZE) {
        vm_swap_free_slot(slot);
        return -1;
    }
    return (int)slot;
}

/* 时钟（二次机会）算法：扫描驻留页，A=1 的第一次只清 A，
 * 第二轮把 A=0 的页换出。g_clock 保证轮转公平。 */
static int vm_evict_one(void) {
    if (g_resident == 0) {
        return 0;
    }

    for (uint32_t pass = 0; pass < 3u; pass++) {
        for (uint32_t i = 0; i < VM_DEMAND_PAGES; i++) {
            uint32_t idx = (g_clock + i) & (VM_DEMAND_PAGES - 1u);
            uint64_t *pt = g_demand_tables[idx >> 9];
            uint32_t pti = idx & 511u;
            uint64_t pte = pt[pti];

            if ((pte & VM_PTE_PRESENT) == 0) {
                continue;
            }
            if (pte & VM_PTE_ACCESSED) {
                if (pass == 0) {
                    pt[pti] = pte & ~VM_PTE_ACCESSED;
                    continue;
                }
            }

            uint32_t frame = (uint32_t)(pte & VM_PTE_FRAME_MASK);
            int slot = vm_swap_out_frame(frame);
            if (slot < 0) {
                continue;
            }

            uint64_t va = (uint64_t)VM_DEMAND_BASE + (uint64_t)idx * VM_PAGE_SIZE;
            pt[pti] = ((uint64_t)(uint32_t)slot << 12) | VM_PTE_SWAP;
            vm_free_frame_raw(frame);
            if (g_resident > 0) {
                g_resident--;
            }
            g_swap_out++;
            g_evictions++;
            vm_flush_page(va);
            g_clock = idx + 1u;
            return 1;
        }
    }

    return 0;
}

static uint32_t vm_alloc_resident_phys(void) {
    while (g_resident >= g_resident_limit) {
        if (!vm_evict_one()) {
            g_oom++;
            return 0;
        }
    }
    uint32_t frame = vm_alloc_frame_raw();
    if (frame == 0) {
        g_oom++;
        return 0;
    }
    g_resident++;
    return frame;
}

static uint32_t vm_swap_in_page(uint32_t slot) {
    if (!g_swap_ready) {
        return 0;
    }
    uint32_t frame = vm_alloc_resident_phys();
    if (frame == 0) {
        return 0;
    }
    int n = slyzfs_swap_read(slot * VM_PAGE_SIZE,
                             (uint8_t *)(uintptr_t)frame,
                             (int)VM_PAGE_SIZE);
    if (n != (int)VM_PAGE_SIZE) {
        vm_free_frame_raw(frame);
        if (g_resident > 0) {
            g_resident--;
        }
        return 0;
    }
    vm_swap_free_slot(slot);
    return frame;
}

/* ============================================================
 * 缺页处理
 * ============================================================ */

/* 无法恢复的缺页：先保留完整串口现场，再进入蓝屏。
 * 蓝屏会冻结中断与全部任务，不会在崩溃后继续写盘。
 * RIP 必须一起打印：只有虚拟地址无法定位是哪条指令越界，
 * 有了 RIP 就能直接对照 nm 的符号表找到出错函数。 */
static void vm_panic_page_fault(uint64_t va, uint64_t error_code,
                                const char *why, uint64_t rip) {
    static char reason[128];
    int p = 0;

    const char *prefix = "unhandled page fault at 0x";
    while (*prefix && p < 40) reason[p++] = *prefix++;

    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nib = (uint32_t)((va >> (uint32_t)shift) & 0xFull);
        reason[p++] = (char)((nib < 10u) ? ('0' + nib) : ('A' + nib - 10u));
    }

    reason[p++] = ' ';
    reason[p++] = 'e';
    reason[p++] = 'r';
    reason[p++] = 'r';
    reason[p++] = '=';
    for (int shift = 4; shift >= 0; shift -= 4) {
        uint32_t nib = (uint32_t)((error_code >> (uint32_t)shift) & 0xFull);
        reason[p++] = (char)((nib < 10u) ? ('0' + nib) : ('A' + nib - 10u));
    }

    const char *rip_tag = " rip=0x";
    while (*rip_tag && p < 60) reason[p++] = *rip_tag++;
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nib = (uint32_t)((rip >> (uint32_t)shift) & 0xFull);
        reason[p++] = (char)((nib < 10u) ? ('0' + nib) : ('A' + nib - 10u));
    }

    reason[p++] = ' ';
    reason[p++] = '-';
    reason[p++] = ' ';
    while (*why && p < 120) reason[p++] = *why++;
    reason[p] = '\0';

    kserial_puts("[VM] fatal fault, rip=0x");
    kserial_hex32((uint32_t)rip);
    kserial_puts("\r\n");

    bsod(0x0000000Eu, reason);
}

int page_fault_handler(uint64_t fault_addr, uint64_t error_code, uint64_t rip) {
    (void)rip;

    g_faults++;

    if (!g_vm_ready) {
        g_unknown_faults++;
        kserial_puts("[VM] fault before init\r\n");
        return 0;
    }
    /* bit0 = 1：保护违规（页存在但权限不足），不可能由按需映射修复 */
    if (error_code & 0x1ull) {
        g_prot_faults++;
        kserial_puts("[VM] fault rejected: protection violation\r\n");
        vm_panic_page_fault(fault_addr, error_code, "protection violation", rip);
        return 0;
    }
    /* bit3 = 1：PTE 使用了保留位，属于页表损坏 */
    if (error_code & 0x8ull) {
        g_reserved_faults++;
        kserial_puts("[VM] fault rejected: reserved bit set\r\n");
        vm_panic_page_fault(fault_addr, error_code, "reserved PTE bit set", rip);
        return 0;
    }

    uint64_t va = fault_addr & ~0xFFFull;
    uint64_t limit = (uint64_t)VM_DEMAND_BASE +
                     (uint64_t)VM_DEMAND_PAGES * VM_PAGE_SIZE;
    if (va < (uint64_t)VM_DEMAND_BASE || va >= limit) {
        g_unknown_faults++;
        kserial_puts("[VM] fault outside demand window at 0x");
        kserial_hex32((uint32_t)va);
        kserial_puts("\r\n");
        vm_panic_page_fault(va, error_code, "outside demand window", rip);
        return 0;
    }

    uint32_t idx = (uint32_t)((va - (uint64_t)VM_DEMAND_BASE) >> 12);
    uint64_t *pt = g_demand_tables[idx >> 9];
    uint32_t pti = idx & 511u;
    uint64_t pte = pt[pti];

    if (pte & VM_PTE_PRESENT) {
        /* present 页不应触发缺页；走到这里说明 TLB 或页表被外部改坏 */
        g_unknown_faults++;
        kserial_puts("[VM] fault on present PTE at 0x");
        kserial_hex32((uint32_t)va);
        kserial_puts("\r\n");
        vm_panic_page_fault(va, error_code, "fault on present PTE", rip);
        return 0;
    }

    if (pte & VM_PTE_SWAP) {
        uint32_t slot = (uint32_t)((pte >> 12) & 0xFFFFFull);
        uint32_t frame = vm_swap_in_page(slot);
        if (frame == 0) {
            kserial_puts("[VM] swap-in failed slot=");
            kserial_dec32(slot);
            kserial_puts(" resident=");
            kserial_dec32(g_resident);
            kserial_puts(" slots_used=");
            kserial_dec32(g_swap_slots_used);
            kserial_puts("\r\n");
            vm_panic_page_fault(va, error_code, "swap-in failed", rip);
            return 0;
        }
        pt[pti] = (uint64_t)frame | VM_PTE_PRESENT | VM_PTE_WRITE | VM_PTE_ACCESSED;
        g_swap_in++;
        vm_flush_page(va);
        return 1;
    }

    /* 从未分配：分配物理帧并清空内容 */
    uint32_t frame = vm_alloc_resident_phys();
    if (frame == 0) {
        kserial_puts("[VM] out of memory at 0x");
        kserial_hex32((uint32_t)va);
        kserial_puts(" resident=");
        kserial_dec32(g_resident);
        kserial_puts(" limit=");
        kserial_dec32(g_resident_limit);
        kserial_puts(" slots_used=");
        kserial_dec32(g_swap_slots_used);
        kserial_puts(" free_frames=");
        kserial_dec32(g_frame_free);
        kserial_puts("\r\n");
        vm_panic_page_fault(va, error_code, "out of memory", rip);
        return 0;
    }
    vm_zero_page(frame);
    pt[pti] = (uint64_t)frame | VM_PTE_PRESENT | VM_PTE_WRITE | VM_PTE_ACCESSED;
    g_demand_faults++;
    vm_flush_page(va);
    return 1;
}

/* ============================================================
 * 初始化
 * ============================================================ */

static int vm_demand_setup(void) {
    uint32_t tables = VM_DEMAND_PAGES / 512u;

    for (uint32_t t = 0; t < tables; t++) {
        uint64_t va = (uint64_t)VM_DEMAND_BASE + (uint64_t)t * 512ull * VM_PAGE_SIZE;
        uint64_t *pt = vm_pt_for(va, 1);
        if (!pt) {
            return 0;
        }
        g_demand_tables[t] = pt;
    }

    /* 拆分产生的新页表可能让旧 TLB 中的 2MB 大页条目继续有效，
     * 一次性重载 CR3 清空整个 TLB。 */
    vm_flush_all();
    return 1;
}

void vm_init(void) {
    if (g_vm_ready) {
        return;
    }

    /* 先安装 #PF 回调，保证之后任何缺页都能进入 C 层而不是直接停机 */
    *(volatile uint64_t *)(uintptr_t)VM_PF_HOOK_ADDR = (uint64_t)(uintptr_t)page_fault_handler;

    g_ram_bytes = vm_detect_ram();
    if (g_ram_bytes == 0) {
        g_ram_bytes = 0x2000000ull;      /* 32MB 保守回退 */
        kserial_puts("[VM] RAM probe failed, assuming 32MB\r\n");
    }

    uint32_t end = (uint32_t)(g_ram_bytes - VM_FRAME_RESERVE);
    end &= ~(VM_PAGE_SIZE - 1u);

    g_frame_base = VM_FRAME_BASE;
    if (end <= g_frame_base) {
        g_frame_count = 0;
        g_frame_free = 0;
    } else {
        uint32_t count = (end - g_frame_base) / VM_PAGE_SIZE;
        if (count > VM_MAX_FRAMES) {
            count = VM_MAX_FRAMES;
        }
        g_frame_count = count;
        g_frame_free = count;
    }
    g_frame_scan = 0;

    for (uint32_t i = 0; i < VM_MAX_WORDS; i++) {
        g_frame_bitmap[i] = 0;
    }

    /* 预占 Slyz 模块固定区，避免大块图片缓冲覆盖驱动代码/BSS。 */
    {
        uint32_t pool_end = g_frame_base + g_frame_count * VM_PAGE_SIZE;
        uint32_t lo = VM_MODULE_ARENA_BASE;
        uint32_t hi = VM_MODULE_ARENA_END;
        if (lo < g_frame_base) lo = g_frame_base;
        if (hi > pool_end) hi = pool_end;

        if (hi > lo) {
            uint32_t first = (lo - g_frame_base) / VM_PAGE_SIZE;
            uint32_t last = (hi - g_frame_base) / VM_PAGE_SIZE;
            uint32_t reserved = 0;
            for (uint32_t i = first; i < last && i < g_frame_count; i++) {
                if (!vm_frame_test(i)) {
                    vm_frame_mark(i, 1);
                    reserved++;
                }
            }
            if (g_frame_free >= reserved) g_frame_free -= reserved;
            else g_frame_free = 0;
            kserial_puts("[VM] reserved module arena frames=");
            kserial_dec32(reserved);
            kserial_puts(" (0x");
            kserial_hex32(lo);
            kserial_puts("-0x");
            kserial_hex32(hi);
            kserial_puts(")\r\n");
        }
    }

    kserial_puts("[VM] frame pool base=0x");
    kserial_hex32(g_frame_base);
    kserial_puts(" frames=");
    kserial_dec32(g_frame_count);
    kserial_puts(" (");
    kserial_dec32((g_frame_count * VM_PAGE_SIZE) / (1024u * 1024u));
    kserial_puts(" MB)\r\n");

    if (g_frame_count < 256u) {
        kserial_puts("[VM] WARNING: frame pool too small, paging may fail\r\n");
    }

    /* 驻留配额不能超过实际帧池的一半：否则在低内存机器上会先耗尽
     * 物理帧而不是按配额换出。另一半留给页表页、换入过程与内核其它
     * 需要连续物理页的场景。 */
    if (g_resident_limit > g_frame_count / 2u) {
        g_resident_limit = g_frame_count / 2u;
        if (g_resident_limit < 16u) {
            g_resident_limit = 16u;
        }
        kserial_puts("[VM] resident limit clamped to ");
        kserial_dec32(g_resident_limit);
        kserial_puts(" pages\r\n");
    }

    if (!vm_demand_setup()) {
        kserial_puts("[VM] demand paging setup FAILED\r\n");
        return;
    }

    kserial_puts("[VM] demand window 0x");
    kserial_hex32(VM_DEMAND_BASE);
    kserial_puts(" pages=");
    kserial_dec32(VM_DEMAND_PAGES);
    kserial_puts(" tables=8\r\n");

    g_swap_ready = slyzfs_swap_attach() ? 1 : 0;
    if (g_swap_ready) {
        kserial_puts("[VM] pagefile ");
        kserial_puts(VM_SWAP_PATH);
        kserial_puts(" ready, slots=");
        kserial_dec32(VM_SWAP_SLOTS);
        kserial_puts(" protected=");
        kserial_dec32((uint32_t)slyzfs_is_protected(VM_SWAP_PATH));
        kserial_puts("\r\n");
    } else {
        kserial_puts("[VM] pagefile unavailable, demand paging without swap\r\n");
    }

    g_vm_ready = 1;
}

int vm_ready(void) {
    return g_vm_ready;
}

/* ============================================================
 * 统计输出
 * ============================================================ */

void vm_dump(void) {
    if (!g_vm_ready) {
        printk("vm: paging subsystem is not initialized\n");
        return;
    }

    printk("Virtual memory / paging\n");
    printk("  page size    : %u bytes\n", (unsigned int)VM_PAGE_SIZE);
    printk("  demand base  : 0x%x\n", (unsigned int)VM_DEMAND_BASE);
    printk("  demand size  : %u pages (%u MB)\n",
           (unsigned int)VM_DEMAND_PAGES,
           (unsigned int)((VM_DEMAND_PAGES * VM_PAGE_SIZE) / (1024u * 1024u)));
    printk("  phys frames  : %u total, %u free, %u used\n",
           (unsigned int)g_frame_count,
           (unsigned int)g_frame_free,
           (unsigned int)(g_frame_count - g_frame_free));
    printk("  frame pool   : 0x%x .. 0x%x\n",
           (unsigned int)g_frame_base,
           (unsigned int)(g_frame_base + g_frame_count * VM_PAGE_SIZE));
    printk("  resident     : %u / %u pages (%u KB)\n",
           (unsigned int)g_resident,
           (unsigned int)g_resident_limit,
           (unsigned int)(g_resident * 4u));
    printk("  page tables  : 8 x 4KB (PD split from 2MB pages)\n");
    printk("  faults       : %u total, %u demand, %u swap-in\n",
           (unsigned int)g_faults,
           (unsigned int)g_demand_faults,
           (unsigned int)g_swap_in);
    printk("  rejected     : %u protection, %u reserved-bit, %u outside\n",
           (unsigned int)g_prot_faults,
           (unsigned int)g_reserved_faults,
           (unsigned int)g_unknown_faults);
    printk("  swap-out     : %u pages, evictions %u, oom %u\n",
           (unsigned int)g_swap_out,
           (unsigned int)g_evictions,
           (unsigned int)g_oom);
}

void vm_dump_swap(void) {
    uint32_t bytes = slyzfs_swap_bytes();

    printk("Swap file\n");
    printk("  path         : %s\n", VM_SWAP_PATH);
    printk("  protected    : %s\n",
           slyzfs_is_protected(VM_SWAP_PATH) ? "yes (root cannot delete)" : "no");
    printk("  capacity     : %u slots x %u bytes = %u KB\n",
           (unsigned int)VM_SWAP_SLOTS,
           (unsigned int)VM_PAGE_SIZE,
           (unsigned int)(VM_SWAP_BYTES / 1024u));
    printk("  file size    : %u bytes (%u KB)\n",
           (unsigned int)bytes, (unsigned int)(bytes / 1024u));
    printk("  slots in use : %u / %u\n",
           (unsigned int)g_swap_slots_used,
           (unsigned int)VM_SWAP_SLOTS);
    printk("  swapped out  : %u pages (%u KB)\n",
           (unsigned int)g_swap_out,
           (unsigned int)((g_swap_out * VM_PAGE_SIZE) / 1024u));
    printk("  swapped in   : %u pages (%u KB)\n",
           (unsigned int)g_swap_in,
           (unsigned int)((g_swap_in * VM_PAGE_SIZE) / 1024u));
    if (!g_swap_ready) {
        printk("  state        : disabled (SlyzFS or pagefile unavailable)\n");
    }
}

int vm_set_resident_limit(uint32_t pages) {
    if (pages < 16u || pages > VM_DEMAND_PAGES) {
        return 0;
    }
    g_resident_limit = pages;
    return 1;
}

uint32_t vm_demand_pages(void) {
    return VM_DEMAND_PAGES;
}

uint32_t vm_resident_pages(void) {
    return g_resident;
}

uint32_t vm_fault_count(void) {
    return g_faults;
}

uint32_t vm_swapout_count(void) {
    return g_swap_out;
}

uint32_t vm_swapin_count(void) {
    return g_swap_in;
}

uint64_t vm_total_ram(void) {
    return g_ram_bytes;
}

void vm_frame_stats(uint32_t *total, uint32_t *free_frames, uint32_t *resident) {
    if (total) *total = g_frame_count;
    if (free_frames) *free_frames = g_frame_free;
    if (resident) *resident = g_resident;
}

uint32_t vm_swap_slots_used(void) {
    return g_swap_slots_used;
}

uint32_t vm_swap_slot_capacity(void) {
    return VM_SWAP_SLOTS;
}

/* ============================================================
 * 自测
 * ------------------------------------------------------------
 * 逐页写入一个带页号的特征值，再逐页读回校验：
 *   - 写阶段必然触发 VM_DEMAND_PAGES 次缺页；
 *   - 驻留页数超过配额后必然触发换出；
 *   - 读阶段访问已被换出的页必然触发换入。
 * 只有"数据全部正确 + 换出/换入计数都增长"才算通过。
 * ============================================================ */
int vm_selftest(void) {
    if (!g_vm_ready) {
        printk("pfa: paging is not initialized\n");
        return 0;
    }
    if (!g_swap_ready) {
        printk("pfa: swap is unavailable, cannot verify eviction\n");
        return 0;
    }

    volatile uint32_t *base = (volatile uint32_t *)(uintptr_t)VM_DEMAND_BASE;
    uint32_t pages = VM_DEMAND_PAGES;
    uint32_t out_before = g_swap_out;
    uint32_t in_before = g_swap_in;
    int ok = 1;

    printk("pfa: writing %u pages (quota %u), this exercises faults and swap\n",
           (unsigned int)pages, (unsigned int)g_resident_limit);

    kserial_puts("[VM] pfa start pages=");
    kserial_dec32(pages);
    kserial_puts(" quota=");
    kserial_dec32(g_resident_limit);
    kserial_puts("\r\n");

    for (uint32_t i = 0; i < pages; i++) {
        base[(size_t)i * (VM_PAGE_SIZE / 4u)] = 0xA5A50000u | (i & 0xFFFFu);
        if ((i & 511u) == 511u) {
            printk("pfa: write %u / %u\n", (unsigned int)(i + 1u), (unsigned int)pages);
        }
    }

    printk("pfa: verifying\n");
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t value = base[(size_t)i * (VM_PAGE_SIZE / 4u)];
        uint32_t expect = 0xA5A50000u | (i & 0xFFFFu);
        if (value != expect) {
            printk("pfa: MISMATCH page %u: got 0x%x expect 0x%x\n",
                   (unsigned int)i, (unsigned int)value, (unsigned int)expect);
            ok = 0;
            break;
        }
    }

    uint32_t out_done = g_swap_out - out_before;
    uint32_t in_done = g_swap_in - in_before;

    printk("pfa: faults=%u swap-out=%u swap-in=%u resident=%u\n",
           (unsigned int)g_faults, (unsigned int)out_done,
           (unsigned int)in_done, (unsigned int)g_resident);

    kserial_puts("[VM] pfa faults=");
    kserial_dec32(g_faults);
    kserial_puts(" out=");
    kserial_dec32(out_done);
    kserial_puts(" in=");
    kserial_dec32(in_done);
    kserial_puts(" resident=");
    kserial_dec32(g_resident);
    kserial_puts(" ok=");
    kserial_dec32((uint32_t)ok);
    kserial_puts("\r\n");

    if (!ok) {
        printk("pfa: FAIL (data corrupted)\n");
        return 0;
    }
    if (out_done == 0 || in_done == 0) {
        printk("pfa: FAIL (swap path was not exercised)\n");
        return 0;
    }

    printk("pfa: PASS\n");
    return 1;
}
