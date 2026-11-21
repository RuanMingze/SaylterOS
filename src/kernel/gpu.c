/* ============================================================
 * gpu.c - Intel Gen9 (Kaby Lake) 集成显卡 2D 后端
 * ------------------------------------------------------------
 * 目标与边界
 *   SaylterOS 的 GUI 目前由 CPU 逐像素画进 backbuffer，再用
 *   rep movsq 整块搬到 VBE 线性帧缓冲。CPU 在 1600x900 下已经
 *   吃满一个核，所以这里把"backbuffer -> 扫描面"的搬运交给
 *   GPU 的 BCS（Blitter Command Streamer，2D 引擎）。
 *
 * 只用公开资料实现
 *   - GGTT PTE 布局：Intel 公开的 i915 源码（gen8_pte_encode）。
 *     bit0=present, bit1=RW, bit3=PWT, bit4=PCD, bit7=PAT,
 *     物理地址位于 bit39:12。
 *   - ring 寄存器：i915 的 intel_engine_regs.h。BCS0 基址
 *     0x22000，TAIL/HEAD/START/CTL 偏移 0x30/0x34/0x38/0x3C。
 *   - 2D 命令 XY_COLOR_BLT(0x54) / XY_SRC_COPY_BLT(0x53)：
 *     Intel 公开的 PRM 与 Mesa i915 驱动的 blit 序列。
 *     命令低 3 位是"DWord 长度 - 2"：6 dword 填 4，8 dword 填 6。
 *   - 扫描面 GPU 地址：PIPEA/B/C_SURF 寄存器（0x7019C/0x7119C/
 *     0x7219C），并用 GGTT 反查物理地址校验。
 *   没有内嵌任何闭源二进制，也没有复制厂商驱动代码。
 *
 * 失败即回退
 *   初始化分三段自测：GGTT 填充、GGTT 拷贝、屏幕上 4x4 像素。
 *   任何一段失败、任何一次 ring 超时，都会把 g_ready 清零并
 *   不再访问 GPU；vbe.c 会自然回到原来的 CPU 路径。
 *
 * 并发边界
 *   GUI 的 swap 只在 BSP 上调用；这里另外用 g_busy 防止重入。
 *   AP 不接触 GPU 寄存器。
 * ============================================================ */

#include <stdint.h>
#include "gpu.h"
#include "vbe.h"
#include "io.h"
#include "mem.h"
#include "tty.h"

/* 启动早期 tty 还没初始化，探测日志走串口 */
extern void kserial_puts(const char *s);
extern void kserial_hex32(uint32_t v);
extern void kserial_dec32(uint32_t v);

/* ============================================================
 * 常量
 * ============================================================ */

#define PCI_CFG_ADDR        0xCF8u
#define PCI_CFG_DATA        0xCFCu

#define INTEL_VENDOR        0x8086u
#define PCI_CLASS_DISPLAY   0x03u

/* Gen8+ 的 GGTT 镜像位于 GTTMMADR + 8MB，共 1M 项 × 8 字节 = 8MB */
#define GGTT_OFFSET         0x00800000u
#define GGTT_ENTRIES        0x00100000u

/* BCS0（blitter）ring 寄存器 */
#define BCS_RING_BASE       0x00022000u
#define RING_TAIL           0x30u
#define RING_HEAD           0x34u
#define RING_START          0x38u
#define RING_CTL            0x3Cu

/* Gen9 的 blitter forcewake 域。
 * GT 进入 RC6 后 ring 寄存器访问会被丢弃，必须先请求 forcewake。
 * 写 set 寄存器时用 _MASKED_BIT_ENABLE 语义：请求位 + 高 16 位掩码。 */
#define FW_BLITTER_SET      0x000A280u
#define FW_BLITTER_ACK      0x00130044u
#define FW_KERNEL           0x00000001u
#define FW_TIMEOUT_CYCLES   125000000ull   /* 约 50ms @2.5GHz */

#define RING_CTL_SIZE(sz)   ((sz) - 4096u)
#define RING_NR_PAGES       0x001FF000u
#define RING_VALID          0x00000001u
#define RING_ADDR_MASK      0x001FFFFCu

/* 扫描面寄存器（pipe A/B/C） */
#define PIPE_A_SURF         0x0007019Cu
#define PIPE_B_SURF         0x0007119Cu
#define PIPE_C_SURF         0x0007219Cu

/* GGTT PTE 标志（与 Intel i915 的 gen8_pte_encode 一致） */
#define PTE_PRESENT         0x0000000000000001ull
#define PTE_RW              0x0000000000000002ull
#define PTE_UC              0x0000000000000018ull   /* PWT|PCD */
#define PTE_PHYS_MASK       0x000000FFFFFFF000ull

/* 2D 命令长度字段 = dword 数 - 2 */
#define XY_COLOR_BLT_LEN    4u
#define XY_SRC_COPY_BLT_LEN 6u

#define GPU_RING_BYTES      65536u
#define GPU_RING_PAGES      (GPU_RING_BYTES / 4096u)

/* GGTT 自 2GB 起分配，避开 BIOS 映射的低位 aperture */
#define GGTT_ALLOC_START    0x00080000u

/* ring 等待上限：约 0.3 秒（2.5GHz 下 8 亿周期） */
#define GPU_TIMEOUT_CYCLES  800000000ull

#define IA32_PAT            0x00000277u
#define IA32_MTRRCAP        0x000000FEu
#define IA32_MTRR_DEF_TYPE  0x000002FFu
#define IA32_MTRR_PHYSBASE0 0x00000200u

#define PAGE_INVLPG(v)      __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)(v)) : "memory")

/* ============================================================
 * 状态
 * ============================================================ */

static gpu_info_t g_info;
static int        g_ready;
static int        g_busy;
static volatile int g_disabled;

static uint32_t   g_mmio;          /* BAR0 物理/虚拟（恒等映射） */
static uint32_t   g_ggtt_cursor;

static uint32_t  *g_ring_mem;      /* CPU 侧 ring 指针（恒等映射） */
static uint32_t   g_ring_tail;     /* 下一次写入偏移 */

static uint32_t   g_scratch_a;     /* 自测页 A 物理地址 */
static uint32_t   g_scratch_b;
static uint32_t   g_scratch_va_a;
static uint32_t   g_scratch_va_b;
static int        g_fw_held;

/* ============================================================
 * 基础工具
 * ============================================================ */

static uint64_t gpu_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void gpu_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t gpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void gpu_mfence(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

static void gpu_wbinvd(void) {
    __asm__ volatile ("wbinvd" ::: "memory");
}

static int gpu_cpu_has_pat(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1u), "c"(0u));
    (void)a; (void)b; (void)c;
    return (d & (1u << 16)) != 0;
}

/* 把 [addr, addr+bytes) 涉及的每个 64 字节缓存行写回并失效。
 * 不在这里 mfence：批量刷新后由调用方统一加屏障。 */
static void gpu_clflush_range(const void *addr, uint32_t bytes) {
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)addr + (uintptr_t)bytes;
    while (p < end) {
        __asm__ volatile ("clflush (%0)" :: "r"((void *)p) : "memory");
        p += 64u;
    }
}

/* ============================================================
 * MMIO / GGTT 访问
 * ============================================================ */

static uint32_t mmio_read(uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(g_mmio + off);
}

static void mmio_write(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(g_mmio + off) = value;
}

static uint64_t ggtt_read(uint32_t index) {
    volatile uint64_t *p =
        (volatile uint64_t *)(uintptr_t)(g_mmio + GGTT_OFFSET + index * 8u);
    return *p;
}

static void ggtt_write(uint32_t index, uint64_t value) {
    volatile uint64_t *p =
        (volatile uint64_t *)(uintptr_t)(g_mmio + GGTT_OFFSET + index * 8u);
    *p = value;
}

/* ============================================================
 * forcewake
 * ------------------------------------------------------------
 * 请求 blitter 域保持上电。成功返回 1 并一直持有，直到
 * gpu_shutdown；失败返回 0，仍然继续自测（自测失败会回退）。
 * ============================================================ */

static int gpu_forcewake_get(void) {
    mmio_write(FW_BLITTER_SET, FW_KERNEL | (FW_KERNEL << 16));

    uint64_t start = gpu_rdtsc();
    while ((mmio_read(FW_BLITTER_ACK) & FW_KERNEL) != FW_KERNEL) {
        if (gpu_rdtsc() - start > FW_TIMEOUT_CYCLES) {
            return 0;
        }
    }
    g_fw_held = 1;
    return 1;
}

static void gpu_forcewake_put(void) {
    if (!g_fw_held) {
        return;
    }
    /* _MASKED_BIT_DISABLE(FW_KERNEL) */
    mmio_write(FW_BLITTER_SET, FW_KERNEL << 16);
    g_fw_held = 0;
}

/* ============================================================
 * PCI 配置空间
 * ============================================================ */

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outl(PCI_CFG_ADDR, addr);
    return inl(PCI_CFG_DATA);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

/* 读取一个 64 位内存 BAR；I/O BAR 或越界返回 0 */
static uint64_t pci_bar64(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t off) {
    uint32_t lo = pci_read32(bus, slot, func, off);
    if (lo & 1u) {
        return 0;
    }
    uint64_t bar = (uint64_t)(lo & ~0x0Fu);
    uint32_t type = (lo >> 1) & 3u;
    if (type == 2u) {
        uint32_t hi = pci_read32(bus, slot, func, (uint8_t)(off + 4u));
        bar |= ((uint64_t)hi) << 32;
    }
    return bar;
}

/* ============================================================
 * 设备名称表
 * ------------------------------------------------------------
 * 只列 Kaby Lake / Skylake 的 Gen9 核显。不在表里的 Intel 显示
 * 设备一律不启用 blitter：Gen12 的 ring 与 2D 命令行为不同，
 * 盲发命令有把引擎弄挂的风险。
 * ============================================================ */

struct GpuNameEntry {
    uint16_t id;
    const char *name;
};

static const struct GpuNameEntry g_gpu_names[] = {
    { 0x5916, "Intel HD Graphics 620" },
    { 0x5906, "Intel HD Graphics 620" },
    { 0x591E, "Intel HD Graphics 615" },
    { 0x5912, "Intel HD Graphics 630" },
    { 0x5917, "Intel UHD Graphics 620" },
    { 0x5926, "Intel Iris Plus Graphics 640" },
    { 0x5927, "Intel Iris Plus Graphics 650" },
    { 0x591D, "Intel HD Graphics 620" },
    { 0x591B, "Intel HD Graphics 630" },
    { 0x0000, 0 }
};

static const char *gpu_name_for(uint16_t id) {
    for (int i = 0; g_gpu_names[i].name; i++) {
        if (g_gpu_names[i].id == id) {
            return g_gpu_names[i].name;
        }
    }
    return 0;
}

/* ============================================================
 * MTRR 诊断（只读）
 * ------------------------------------------------------------
 * 不做任何 MTRR 修改。只把 LFB 区间当前的有效内存类型算出来，
 * 用于解释"为什么 PAT 的 WC 可能不生效"。类型编码：
 * 0=UC, 1=WC, 4=WT, 6=WB。
 * ============================================================ */

static uint32_t mtrr_decode_size(uint64_t mask) {
    uint64_t m = mask & 0x0000000FFFFFF000ull;
    if (m == 0) {
        return 0;
    }
    uint32_t low = (uint32_t)(m & 0xFFFFFFFFu);
    if (low == 0) {
        return 0;   /* 覆盖 4GB 以上，本机不会用到 */
    }
    uint32_t lsb = (uint32_t)__builtin_ctz(low);
    if (lsb < 12u || lsb >= 32u) {
        return 0;
    }
    return 1u << lsb;
}

static int mtrr_type_for(uint32_t phys) {
    uint64_t cap = gpu_rdmsr(IA32_MTRRCAP);
    uint32_t count = (uint32_t)(cap & 0xFFu);
    if (count > 16u) {
        count = 16u;
    }

    int result = -1;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t base_msr = gpu_rdmsr(IA32_MTRR_PHYSBASE0 + i * 2u);
        uint64_t mask_msr = gpu_rdmsr(IA32_MTRR_PHYSBASE0 + i * 2u + 1u);

        if ((mask_msr & (1ull << 11)) == 0) {
            continue;   /* 该项未启用 */
        }

        uint32_t type = (uint32_t)(base_msr & 0xFFu);
        uint32_t base = (uint32_t)(base_msr & 0xFFFFF000u);
        uint32_t size = mtrr_decode_size(mask_msr);
        if (size == 0) {
            continue;
        }
        if (phys < base || phys - base >= size) {
            continue;
        }

        if (type == 0u) {           /* UC 优先级最高 */
            return 0;
        }
        if (type == 4u && result != 0) {
            result = 4;
        } else if (type == 1u && result != 0 && result != 4) {
            result = 1;
        } else if (type == 6u && result < 0) {
            result = 6;
        }
    }

    if (result < 0) {
        uint64_t def = gpu_rdmsr(IA32_MTRR_DEF_TYPE);
        result = (int)(def & 0x7u);
    }
    return result;
}

/* ============================================================
 * 页表缓存属性
 * ------------------------------------------------------------
 * 4GB 恒等映射使用 2MB 大页，PD 基址 0x72000，每个 PDPT 项
 * 一张 4KB 的 PD。这里只改覆盖目标物理区间的 PD 项：
 *   MMIO：PAT=0, PCD=1, PWT=1 -> 索引 3 -> UC
 *   LFB ：PAT=1, PCD=0, PWT=0 -> 索引 4 -> WC（需要先改 PAT MSR）
 * 修改后对每个 2MB 区间 invlpg，最后 wbinvd 一次。
 * ============================================================ */

static void gpu_set_range_cache(uint32_t phys, uint32_t bytes,
                                uint32_t pat_bit, uint32_t pcd, uint32_t pwt) {
    uint32_t start = phys & ~0x1FFFFFu;

    /* 用 64 位算尾部，避免 BAR 贴近 4GB 时 32 位回绕成 0 导致整段漏设 */
    uint64_t end64 = ((uint64_t)phys + (uint64_t)bytes + 0x1FFFFFull) &
                     ~0x1FFFFFull;
    if (end64 > 0x100000000ull) {
        end64 = 0x100000000ull;
    }

    for (uint64_t p = (uint64_t)start; p < end64; p += 0x200000ull) {
        uint32_t pdpt = (uint32_t)((p >> 30) & 3u);
        uint32_t pdi = (uint32_t)((p >> 21) & 0x1FFu);
        uint64_t *pd = (uint64_t *)(uintptr_t)(0x72000u + pdpt * 4096u);
        uint64_t entry = pd[pdi];

        /* 只处理 present 且 PS=1 的 2MB 大页 */
        if ((entry & 0x81ull) != 0x81ull) {
            continue;
        }

        entry &= ~((1ull << 3) | (1ull << 4) | (1ull << 12));
        entry |= ((uint64_t)pwt << 3);
        entry |= ((uint64_t)pcd << 4);
        entry |= ((uint64_t)pat_bit << 12);
        pd[pdi] = entry;
        PAGE_INVLPG((uint32_t)p);
    }
}

/* ============================================================
 * 扫描面 GPU 地址
 * ------------------------------------------------------------
 * PIPEx_SURF 里存的是 GGTT 地址。读出后反查 GGTT[va>>12] 的
 * 物理地址，必须和 VBE 报告的 framebuffer 一致才采信。
 * 三步：三个 pipe 的 SURF -> 按 aperture 线性偏移猜 -> 扫 GGTT。
 * ============================================================ */

static int ggtt_matches(uint32_t gpu_va, uint32_t phys) {
    if (gpu_va == 0) {
        return 0;
    }
    uint64_t pte = ggtt_read((gpu_va >> 12) & (GGTT_ENTRIES - 1u));
    if ((pte & PTE_PRESENT) == 0) {
        return 0;
    }
    return (uint32_t)(pte & PTE_PHYS_MASK) == (phys & 0xFFFFF000u);
}

static uint32_t gpu_find_lfb_va(uint32_t phys) {
    static const uint32_t regs[3] = { PIPE_A_SURF, PIPE_B_SURF, PIPE_C_SURF };

    for (int i = 0; i < 3; i++) {
        uint32_t surf = mmio_read(regs[i]) & 0xFFFFF000u;
        if (ggtt_matches(surf, phys)) {
            return surf;
        }
    }

    uint32_t guess = phys & 0xFFFFF000u;
    if (ggtt_matches(guess, phys)) {
        return guess;
    }

    /* 扫描低 512MB GGTT；只做一次，失败就回退 */
    for (uint32_t i = 0; i < 0x00020000u; i++) {
        uint64_t pte = ggtt_read(i);
        if ((pte & PTE_PRESENT) == 0) {
            continue;
        }
        if ((uint32_t)(pte & PTE_PHYS_MASK) == (phys & 0xFFFFF000u)) {
            return i << 12;
        }
    }
    return 0;
}

/* ============================================================
 * GGTT 分配
 * ============================================================ */

static int ggtt_alloc(uint32_t pages, uint32_t *out_index) {
    uint32_t i = g_ggtt_cursor;

    while (i + pages <= GGTT_ENTRIES) {
        uint32_t k = 0;
        while (k < pages && (ggtt_read(i + k) & PTE_PRESENT) == 0) {
            k++;
        }
        if (k == pages) {
            g_ggtt_cursor = i + pages;
            *out_index = i;
            return 1;
        }
        i += k + 1u;
    }
    return 0;
}

static void ggtt_map(uint32_t index, uint32_t phys, uint32_t pages) {
    for (uint32_t k = 0; k < pages; k++) {
        uint64_t pte = ((uint64_t)(phys + k * 4096u) & PTE_PHYS_MASK) |
                       PTE_PRESENT | PTE_RW | PTE_UC;
        ggtt_write(index + k, pte);
    }
}

static void ggtt_unmap(uint32_t index, uint32_t pages) {
    for (uint32_t k = 0; k < pages; k++) {
        ggtt_write(index + k, 0);
    }
}

/* ============================================================
 * BCS ring
 * ============================================================ */

static uint32_t ring_head(void) {
    return mmio_read(BCS_RING_BASE + RING_HEAD) & RING_ADDR_MASK;
}

static int ring_wait_idle(uint64_t timeout) {
    uint64_t start = gpu_rdtsc();
    for (;;) {
        if (ring_head() == (g_ring_tail & RING_ADDR_MASK)) {
            return 1;
        }
        if (gpu_rdtsc() - start > timeout) {
            return 0;
        }
    }
}

static int ring_setup(uint32_t ring_va) {
    /* 先停 ring，再写地址与 CTL；HEAD 归零后再使能 */
    mmio_write(BCS_RING_BASE + RING_CTL, 0);
    mmio_write(BCS_RING_BASE + RING_START, ring_va);
    mmio_write(BCS_RING_BASE + RING_HEAD, 0);
    mmio_write(BCS_RING_BASE + RING_TAIL, 0);

    uint32_t ctl = (RING_CTL_SIZE(GPU_RING_BYTES) & RING_NR_PAGES) | RING_VALID;
    mmio_write(BCS_RING_BASE + RING_CTL, ctl);

    /* 有些固件会让 HEAD 停在非零位置，读回来对齐一次 */
    uint32_t head = ring_head();
    g_ring_tail = head & RING_ADDR_MASK;
    mmio_write(BCS_RING_BASE + RING_TAIL, g_ring_tail);
    return 1;
}

static void ring_stop(void) {
    if (g_mmio != 0) {
        mmio_write(BCS_RING_BASE + RING_CTL, 0);
    }
}

/* 提交一批 dword：处理回绕、写回缓存、推进 TAIL、等待 RETIRE */
static int ring_submit(const uint32_t *dw, uint32_t count) {
    if (g_ring_mem == 0 || count == 0 || count > 64u) {
        return 0;
    }

    uint32_t bytes = count * 4u;
    uint32_t head = ring_head();
    uint32_t tail = g_ring_tail & RING_ADDR_MASK;
    uint32_t used = (tail >= head) ? (tail - head)
                                   : (GPU_RING_BYTES - head + tail);

    if (GPU_RING_BYTES - used - 8u < bytes) {
        if (!ring_wait_idle(GPU_TIMEOUT_CYCLES)) {
            return 0;
        }
    }

    /* 命令不能跨过 ring 尾部：用 MI_NOOP 填满剩余空间再回绕 */
    if (tail + bytes > GPU_RING_BYTES) {
        uint32_t pad_dwords = (GPU_RING_BYTES - tail) / 4u;
        uint32_t *pad = g_ring_mem + (tail >> 2);
        for (uint32_t i = 0; i < pad_dwords; i++) {
            pad[i] = 0;
        }
        gpu_clflush_range(pad, pad_dwords * 4u);
        tail = 0;
    }

    uint32_t *dst = g_ring_mem + (tail >> 2);
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = dw[i];
    }

    gpu_clflush_range(dst, bytes);
    gpu_mfence();

    g_ring_tail = tail + bytes;
    if (g_ring_tail >= GPU_RING_BYTES) {
        g_ring_tail = 0;
    }

    mmio_write(BCS_RING_BASE + RING_TAIL, g_ring_tail);
    gpu_mfence();

    if (!ring_wait_idle(GPU_TIMEOUT_CYCLES)) {
        return 0;
    }
    return 1;
}

/* ============================================================
 * 2D 命令构造与下发
 * ============================================================ */

static uint32_t build_color_blt(uint32_t *out, uint32_t dst_va,
                                uint32_t pitch, int x, int y,
                                int w, int h, uint32_t color) {
    /* XY_COLOR_BLT：6 dword，长度字段 4，ROP=0xF0(PATCOPY) */
    uint32_t cmd = (2u << 29) | (0x54u << 22) | XY_COLOR_BLT_LEN |
                   (1u << 20) | (1u << 21);
    uint32_t br13 = (pitch & 0xFFFFu) | (0xF0u << 16) |
                    (1u << 24) | (1u << 25);

    out[0] = cmd;
    out[1] = br13;
    out[2] = ((uint32_t)y << 16) | (uint32_t)x;
    out[3] = ((uint32_t)(y + h) << 16) | (uint32_t)(x + w);
    out[4] = dst_va;
    out[5] = color;
    return 6u;
}

static uint32_t build_copy_blt(uint32_t *out,
                               uint32_t dst_va, uint32_t dst_pitch,
                               int dx, int dy,
                               uint32_t src_va, uint32_t src_pitch,
                               int sx, int sy, int w, int h) {
    /* XY_SRC_COPY_BLT：8 dword，长度字段 6，ROP=0xCC(SRCCOPY) */
    uint32_t cmd = (2u << 29) | (0x53u << 22) | XY_SRC_COPY_BLT_LEN |
                   (1u << 20) | (1u << 21);
    uint32_t br13 = (dst_pitch & 0xFFFFu) | (0xCCu << 16) |
                    (1u << 24) | (1u << 25);

    out[0] = cmd;
    out[1] = br13;
    out[2] = ((uint32_t)dy << 16) | (uint32_t)dx;
    out[3] = ((uint32_t)(dy + h) << 16) | (uint32_t)(dx + w);
    out[4] = dst_va;
    out[5] = ((uint32_t)sy << 16) | (uint32_t)sx;
    out[6] = src_pitch & 0xFFFFu;
    out[7] = src_va;
    return 8u;
}

/* ============================================================
 * 自测
 * ============================================================ */

/* 离屏填充 + 拷贝：不碰屏幕，验证 GGTT 映射、ring、两条命令 */
static int gpu_selftest_offscreen(void) {
    volatile uint32_t *a = (volatile uint32_t *)(uintptr_t)g_scratch_a;
    volatile uint32_t *b = (volatile uint32_t *)(uintptr_t)g_scratch_b;
    uint32_t cmd[8];

    for (uint32_t i = 0; i < 1024u; i++) {
        a[i] = 0;
        b[i] = 0;
    }
    gpu_clflush_range((void *)(uintptr_t)g_scratch_a, 4096);
    gpu_clflush_range((void *)(uintptr_t)g_scratch_b, 4096);
    gpu_mfence();

    /* 1) 用 GPU 往 A 里填 32x32 品红，pitch=128 */
    uint32_t n = build_color_blt(cmd, g_scratch_va_a, 128u,
                                 0, 0, 32, 32, 0x00FF00FFu);
    if (!ring_submit(cmd, n)) {
        return 0;
    }
    gpu_mfence();

    if (a[0] != 0x00FF00FFu) return 0;
    if (a[31] != 0x00FF00FFu) return 0;
    if (a[32] != 0x00000000u) return 0;

    /* 2) A -> B 拷贝，验证 XY_SRC_COPY_BLT */
    gpu_clflush_range((void *)(uintptr_t)g_scratch_a, 4096);
    gpu_clflush_range((void *)(uintptr_t)g_scratch_b, 4096);
    gpu_mfence();

    n = build_copy_blt(cmd, g_scratch_va_b, 128u, 0, 0,
                       g_scratch_va_a, 128u, 0, 0, 32, 32);
    if (!ring_submit(cmd, n)) {
        return 0;
    }
    gpu_mfence();

    if (b[0] != 0x00FF00FFu) return 0;
    if (b[31] != 0x00FF00FFu) return 0;
    if (b[32] != 0x00000000u) return 0;

    return 1;
}

/* 屏幕上 4x4：验证 PIPEx_SURF 推导出的 LFB GPU 地址确实指向
 * VBE 扫描面，并且扫描面是线性布局。保存/恢复原像素，闪烁不可见。 */
static int gpu_selftest_display(void) {
    if (!vbe_is_enabled() || g_info.lfb_gpu_va == 0) {
        return 0;
    }

    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)g_info.lfb_phys;
    uint32_t stride = vbe_current.pitch / 4u;
    uint32_t save[4][4];

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            save[r][c] = fb[(uint32_t)r * stride + (uint32_t)c];
        }
    }
    gpu_clflush_range((void *)fb, 4u * vbe_current.pitch);
    gpu_mfence();

    uint32_t cmd[8];
    uint32_t n = build_color_blt(cmd, g_info.lfb_gpu_va, vbe_current.pitch,
                                 0, 0, 4, 4, 0x00FF00FFu);
    if (!ring_submit(cmd, n)) {
        return 0;
    }
    gpu_mfence();

    uint32_t got = fb[0];

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            fb[(uint32_t)r * stride + (uint32_t)c] = save[r][c];
        }
    }
    gpu_clflush_range((void *)fb, 4u * vbe_current.pitch);
    gpu_mfence();

    return (got == 0x00FF00FFu) ? 1 : 0;
}

/* ============================================================
 * 初始化：硬件探测 + 缓存属性 + 扫描面地址
 * ============================================================ */

int gpu_init(void) {
    g_ready = 0;
    g_busy = 0;
    g_disabled = 0;
    g_mmio = 0;

    g_info.present = 0;
    g_info.ready = 0;
    g_info.lfb_gpu_va = 0;
    g_info.bb_gpu_va = 0;
    g_info.blits = 0;
    g_info.bytes_copied = 0;
    g_info.timeouts = 0;
    g_info.mtrr_type = -1;
    g_info.pat_ok = 0;
    g_info.wc_enabled = 0;

    if (!vbe_is_enabled()) {
        return 0;
    }

    /* ---- PCI 扫描：bus0 上找 Intel 显示控制器 ---- */
    uint8_t found_slot = 0xFF, found_func = 0xFF;
    uint16_t found_dev = 0;
    uint16_t found_vendor = 0;

    for (uint8_t slot = 0; slot < 32 && found_slot == 0xFF; slot++) {
        uint32_t id = pci_read32(0, slot, 0, 0x00);
        if (id == 0xFFFFFFFFu) {
            continue;
        }
        uint8_t header = pci_read8(0, slot, 0, 0x0E);
        uint8_t funcs = (header & 0x80u) ? 8u : 1u;
        for (uint8_t fn = 0; fn < funcs; fn++) {
            uint32_t cfg = pci_read32(0, slot, fn, 0x00);
            if (cfg == 0xFFFFFFFFu) {
                continue;
            }
            uint16_t vendor = (uint16_t)(cfg & 0xFFFFu);
            uint16_t device = (uint16_t)(cfg >> 16);
            uint8_t cls = pci_read8(0, slot, fn, 0x0B);
            if (vendor != INTEL_VENDOR || cls != PCI_CLASS_DISPLAY) {
                continue;
            }
            if (gpu_name_for(device) == 0) {
                kserial_puts("[gpu] unsupported Intel display device ");
                kserial_hex32(device);
                kserial_puts("\n");
                return 0;
            }
            found_slot = slot;
            found_func = fn;
            found_dev = device;
            found_vendor = vendor;
            break;
        }
    }

    if (found_slot == 0xFF) {
        kserial_puts("[gpu] no Intel display controller on bus 0\n");
        return 0;
    }

    uint64_t bar0 = pci_bar64(0, found_slot, found_func, 0x10);
    uint64_t bar2 = pci_bar64(0, found_slot, found_func, 0x18);

    g_info.bus = 0;
    g_info.slot = found_slot;
    g_info.func = found_func;
    g_info.vendor = found_vendor;
    g_info.device = found_dev;
    g_info.mmio_phys = (uint32_t)bar0;
    g_info.lfb_phys = vbe_current.framebuffer;

    kserial_puts("[gpu] found ");
    kserial_puts(gpu_name_for(found_dev));
    kserial_puts(" mmio=");
    kserial_hex32((uint32_t)bar0);
    kserial_puts(" bar2=");
    kserial_hex32((uint32_t)bar2);
    kserial_puts(" vbe_fb=");
    kserial_hex32(vbe_current.framebuffer);
    kserial_puts("\n");

    /* 4GB 恒等映射之外无法访问：两段 BAR 必须都在 4GB 以下 */
    if (bar0 == 0 || bar0 >= 0x100000000ull ||
        vbe_current.framebuffer == 0 ||
        vbe_current.framebuffer >= 0x100000000u) {
        kserial_puts("[gpu] BAR above 4GB, unsupported\n");
        return 0;
    }

    g_mmio = (uint32_t)bar0;
    g_info.present = 1;

    /* ---- 缓存类型：MMIO -> UC，LFB -> WC（PAT 支持时） ---- */
    g_info.pat_ok = gpu_cpu_has_pat();
    gpu_wbinvd();

    if (g_info.pat_ok) {
        uint64_t pat = gpu_rdmsr(IA32_PAT);
        /* PAT4（bit39:32）设为 WC(01)，其余索引保持固件默认 */
        pat = (pat & ~(0xFFull << 32)) | (0x01ull << 32);
        gpu_wrmsr(IA32_PAT, pat);

        uint32_t lfb_bytes = (uint32_t)vbe_current.pitch * vbe_current.height;
        gpu_set_range_cache(vbe_current.framebuffer, lfb_bytes, 1u, 0u, 0u);
        g_info.wc_enabled = 1;
    }

    gpu_set_range_cache((uint32_t)bar0, 16u * 1024u * 1024u, 0u, 1u, 1u);

    gpu_wbinvd();

    g_info.mtrr_type = mtrr_type_for(vbe_current.framebuffer);

    kserial_puts("[gpu] mtrr=");
    kserial_dec32((uint32_t)g_info.mtrr_type);
    kserial_puts(" pat=");
    kserial_dec32((uint32_t)g_info.pat_ok);
    kserial_puts("\n");

    /* ---- 扫描面 GPU 地址 ---- */
    g_info.lfb_gpu_va = gpu_find_lfb_va(vbe_current.framebuffer);
    if (g_info.lfb_gpu_va == 0) {
        kserial_puts("[gpu] cannot resolve scanout GGTT address\n");
        g_info.present = 0;
        g_mmio = 0;
        return 0;
    }

    kserial_puts("[gpu] lfb_gpu_va=");
    kserial_hex32(g_info.lfb_gpu_va);
    kserial_puts("\n");

    return 1;
}

/* ============================================================
 * gpu_start：GGTT 映射 / ring / 自测
 * ============================================================ */

int gpu_start(void) {
    if (!g_info.present || g_mmio == 0) {
        return 0;
    }
    if (g_ready) {
        return 1;
    }

    g_ggtt_cursor = GGTT_ALLOC_START;

    /* ---- ring 内存：64KB，页对齐（恒等映射下物理=虚拟） ---- */
    void *ring_raw = kmalloc(GPU_RING_BYTES + 8192u);
    if (ring_raw == 0) {
        printk("[GPU] ring allocation failed, fallback to CPU\n");
        return 0;
    }
    uintptr_t ring_ptr = ((uintptr_t)ring_raw + 4095u) & ~(uintptr_t)4095u;
    g_ring_mem = (uint32_t *)ring_ptr;
    for (uint32_t i = 0; i < GPU_RING_BYTES / 4u; i++) {
        g_ring_mem[i] = 0;
    }
    gpu_clflush_range(g_ring_mem, GPU_RING_BYTES);

    /* ---- 自测页：两页，pitch=128 的 32x32 表面 ----
     * kmalloc 只保证 8 字节对齐，页对齐最多浪费 4088 字节，
     * 因此按 8192 + 4096 申请，保证对齐后仍有两整页可用。 */
    void *scratch_raw = kmalloc(8192u + 4096u);
    if (scratch_raw == 0) {
        printk("[GPU] scratch allocation failed, fallback to CPU\n");
        return 0;
    }
    uintptr_t scratch_ptr = ((uintptr_t)scratch_raw + 4095u) & ~(uintptr_t)4095u;
    g_scratch_a = (uint32_t)scratch_ptr;
    g_scratch_b = (uint32_t)(scratch_ptr + 4096u);

    /* ---- 分配 GGTT 项 ---- */
    uint32_t ring_index = 0;
    uint32_t scratch_index = 0;

    if (!ggtt_alloc(GPU_RING_PAGES, &ring_index)) {
        printk("[GPU] no GGTT run for ring, fallback to CPU\n");
        return 0;
    }
    if (!ggtt_alloc(2u, &scratch_index)) {
        printk("[GPU] no GGTT run for scratch, fallback to CPU\n");
        return 0;
    }

    uint32_t bb_pages = 0;
    uint32_t bb_phys = vbe_backbuffer_phys();
    uint32_t bb_bytes = vbe_backbuffer_bytes();
    if (bb_phys != 0 && bb_bytes != 0) {
        bb_pages = (bb_bytes + 4095u) / 4096u;
        if (bb_pages > 4096u) {
            bb_pages = 0;
        }
    }

    uint32_t bb_index = 0;
    if (bb_pages != 0) {
        if (!ggtt_alloc(bb_pages, &bb_index)) {
            printk("[GPU] no GGTT run for backbuffer (%u pages), fallback to CPU\n",
                   bb_pages);
            return 0;
        }
    }

    ggtt_map(ring_index, (uint32_t)ring_ptr, GPU_RING_PAGES);
    ggtt_map(scratch_index, g_scratch_a, 2u);
    if (bb_pages != 0) {
        ggtt_map(bb_index, bb_phys, bb_pages);
    }

    g_scratch_va_a = scratch_index << 12;
    g_scratch_va_b = (scratch_index + 1u) << 12;
    g_info.ring_gpu_va = ring_index << 12;
    g_info.ring_size = GPU_RING_BYTES;
    g_info.bb_gpu_va = bb_pages ? (bb_index << 12) : 0;
    g_info.bb_pages = bb_pages;

    /* ---- 请求 blitter forcewake ----
     * 失败不代表硬件一定不可用（有些固件已经保持上电），继续跑自测，
     * 由自测结果决定是否启用。 */
    int fw_ok = gpu_forcewake_get();
    printk("[GPU] blitter forcewake: %s\n", fw_ok ? "ack ok" : "no ack");

    /* ---- 启动 ring ---- */
    ring_setup(g_info.ring_gpu_va);

    /* ---- 自测 ---- */
    if (!gpu_selftest_offscreen()) {
        g_info.timeouts = 1;
        printk("[GPU] offscreen blitter self-test FAILED, fallback to CPU\n");
        ring_stop();
        gpu_forcewake_put();
        ggtt_unmap(ring_index, GPU_RING_PAGES);
        ggtt_unmap(scratch_index, 2u);
        if (bb_pages) {
            ggtt_unmap(bb_index, bb_pages);
        }
        g_info.bb_gpu_va = 0;
        g_info.bb_pages = 0;
        return 0;
    }
    printk("[GPU] offscreen self-test passed (fill + copy)\n");

    if (!gpu_selftest_display()) {
        g_info.timeouts = 1;
        printk("[GPU] display self-test FAILED, fallback to CPU\n");
        ring_stop();
        gpu_forcewake_put();
        ggtt_unmap(ring_index, GPU_RING_PAGES);
        ggtt_unmap(scratch_index, 2u);
        if (bb_pages) {
            ggtt_unmap(bb_index, bb_pages);
        }
        g_info.bb_gpu_va = 0;
        g_info.bb_pages = 0;
        return 0;
    }
    printk("[GPU] display self-test passed (LFB %x linear)\n",
           g_info.lfb_gpu_va);

    g_ready = 1;
    g_info.ready = 1;

    printk("[GPU] %s ready: %ux%u %ubpp, ring %u KB @ %x, backbuffer %u pages @ %x\n",
           gpu_type_name(),
           vbe_current.width, vbe_current.height, vbe_current.bpp,
           GPU_RING_BYTES / 1024u, g_info.ring_gpu_va,
           g_info.bb_pages, g_info.bb_gpu_va);
    printk("[GPU] blitter acceleration ENABLED (CPU fallback armed)\n");

    return 1;
}

/* ============================================================
 * 对外查询
 * ============================================================ */

int gpu_ready(void) {
    return g_ready && !g_disabled;
}

void gpu_get_info(gpu_info_t *out) {
    if (out) {
        *out = g_info;
    }
}

const char *gpu_type_name(void) {
    const char *name = gpu_name_for(g_info.device);
    return name ? name : "Intel Graphics";
}

/* ============================================================
 * 搬运
 * ============================================================ */

int gpu_copy_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!gpu_ready() || g_busy) {
        return -1;
    }
    if (g_info.bb_gpu_va == 0 || g_info.lfb_gpu_va == 0) {
        return -1;
    }
    if (w == 0 || h == 0) {
        return -1;
    }
    if ((uint32_t)x + w > vbe_current.width ||
        (uint32_t)y + h > vbe_current.height) {
        return -1;
    }

    g_busy = 1;

    /* GPU 通过 GGTT 读 backbuffer，读之前必须把 CPU 的 WB 行刷回内存 */
    uint32_t stride = vbe_backbuffer_stride();
    const uint8_t *base = (const uint8_t *)(uintptr_t)vbe_backbuffer_phys();

    if (x == 0 && w * 4u >= stride) {
        gpu_clflush_range(base + (uint32_t)y * stride, h * stride);
    } else {
        for (uint32_t row = 0; row < h; row++) {
            gpu_clflush_range(base + ((uint32_t)y + row) * stride + (uint32_t)x * 4u,
                              (uint32_t)w * 4u);
        }
    }
    gpu_mfence();

    uint32_t cmd[8];
    uint32_t n = build_copy_blt(cmd,
                                g_info.lfb_gpu_va, vbe_current.pitch,
                                x, y,
                                g_info.bb_gpu_va, stride,
                                x, y, w, h);

    int ok = ring_submit(cmd, n);
    if (!ok) {
        g_info.timeouts++;
        g_ready = 0;
        g_disabled = 1;
        g_info.ready = 0;
        ring_stop();
        g_busy = 0;
        return -1;
    }

    g_info.blits++;
    g_info.bytes_copied += (uint32_t)w * (uint32_t)h * 4u;
    g_busy = 0;
    return 0;
}

int gpu_copy_all(void) {
    if (!gpu_ready()) {
        return -1;
    }
    return gpu_copy_rect(0, 0, vbe_current.width, vbe_current.height);
}

void gpu_shutdown(void) {
    if (g_mmio != 0) {
        ring_stop();
        gpu_forcewake_put();
    }
    g_ready = 0;
    g_disabled = 1;
    g_info.ready = 0;
}
