/* ============================================================
 * ahci.c - AHCI(SATA) 块设备驱动
 * ------------------------------------------------------------
 * 目标：让 SlyzFS 能直接跑在 SATA 硬盘上，不再依赖 PIIX IDE 的
 * PIO 通道。NVMe 需要完整 PCIe 配置空间与多队列，暂不实现；
 * SATA 是现代裸机的最低要求，因此这里完整实现 AHCI 1.3 的
 * 轮询式 DMA 读写路径。
 *
 * 实现范围：
 *   - PCI 枚举 class 0x01 / subclass 0x06 / progIF 0x01；
 *   - ABAR(BAR5) MMIO 映射与 HBA.GHC.AE 使能；
 *   - 端口停止/复位/启动，PxCLB/PxFB 命令列表与 FIS 缓冲；
 *   - H2D Register FIS + 单 PRDT 的 READ/WRITE DMA EXT；
 *   - IDENTIFY DEVICE 读取容量与逻辑扇区大小；
 *   - LBA28 / LBA48 自动选择，512 字节逻辑扇区。
 *
 * 边界：
 *   - 只使用命令槽 0、单 PRDT，适合一次一个扇区的块层调用；
 *   - 不走中断，全部轮询；
 *   - 只接受 SATA(AHCI) 控制器上的 512 字节逻辑扇区设备；
 *   - 初始化失败时 present() 返回 0，blk.c 继续回退 ATA PIO。
 * ============================================================ */

#include <stdint.h>
#include "ahci.h"
#include "io.h"
#include "tty.h"

/* ============================================================
 * 常量
 * ============================================================ */

#define AHCI_TIMEOUT_INIT  5000000000ull
#define AHCI_TIMEOUT_XFER  4000000000ull

/* HBA 全局寄存器 */
#define HBA_CAP   0x00u
#define HBA_GHC   0x04u
#define HBA_IS    0x08u
#define HBA_PI    0x0Cu
#define HBA_VS    0x10u

#define HBA_GHC_AE  (1u << 31)
#define HBA_GHC_IE  (1u << 1)
#define HBA_GHC_HR  (1u << 0)

/* 端口寄存器（相对端口基址） */
#define PX_CLB    0x00u
#define PX_CLBU   0x04u
#define PX_FB     0x08u
#define PX_FBU    0x0Cu
#define PX_IS     0x10u
#define PX_IE     0x14u
#define PX_CMD    0x18u
#define PX_TFD    0x20u
#define PX_SIG    0x24u
#define PX_SSTS   0x28u
#define PX_SCTL   0x2Cu
#define PX_SERR   0x30u
#define PX_SACT   0x34u
#define PX_CI     0x38u

/* PxCMD */
#define PX_CMD_ST   (1u << 0)
#define PX_CMD_SUD  (1u << 1)
#define PX_CMD_POD  (1u << 2)
#define PX_CMD_CLO  (1u << 3)
#define PX_CMD_FRE  (1u << 4)
#define PX_CMD_FR   (1u << 14)
#define PX_CMD_CR   (1u << 15)

/* PxTFD */
#define PX_TFD_ERR  (1u << 0)
#define PX_TFD_DRQ  (1u << 3)
#define PX_TFD_BSY  (1u << 7)

/* PxIS 错误位：OFS/INFS/IFS/HBDS/IF/TFES/HBFS */
#define PX_IS_ERROR 0x7F800000u
#define PX_IS_TFES  (1u << 29)

#define AHCI_SIG_ATA 0x00000101u

/* ATA 命令 */
#define ATA_CMD_READ_DMA      0xC8u
#define ATA_CMD_WRITE_DMA     0xCAu
#define ATA_CMD_READ_DMA_EXT  0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u
#define ATA_CMD_IDENTIFY      0xECu

/* ============================================================
 * 数据结构（全部按 AHCI 规范字节布局）
 * ============================================================ */

struct __attribute__((packed)) AhciCmdHdr {
    uint16_t cfl_flags;     /* CFL(bits0-4) | W(bit6) */
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
};

struct __attribute__((packed)) AhciPrdt {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;           /* 字节数-1，bit31 = 完成中断 */
};

/* 静态 DMA 内存：必须 1KB/256 对齐且物理连续。
 * 内核恒等映射，数组地址即物理地址，全部位于 32MB 以内。 */
static uint8_t g_cmd_list[1024] __attribute__((aligned(1024)));
static uint8_t g_fis[256]       __attribute__((aligned(256)));
static uint8_t g_cmd_tbl[256]   __attribute__((aligned(256)));
static uint8_t g_identify[512]  __attribute__((aligned(512)));

/* ============================================================
 * 状态
 * ============================================================ */

static uint32_t g_abar;
static uint32_t g_port_base;
static uint32_t g_port;
static uint32_t g_total_sectors;
static uint32_t g_reads;
static uint32_t g_writes;
static int      g_lba48;
static int      g_present;
static int      g_tried;

/* ============================================================
 * 基础工具
 * ============================================================ */

static void zero_mem(void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}

static uint64_t rdtsc_u64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint32_t rd32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void wr32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static void cache_flush_range(const void *addr, uint32_t bytes) {
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)addr + bytes;
    while (p < end) {
        __asm__ volatile ("clflush (%0)" :: "r"((void *)p) : "memory");
        p += 64u;
    }
    __asm__ volatile ("mfence" ::: "memory");
}

static void delay_loops(uint32_t n) {
    while (n--) {
        __asm__ volatile ("pause");
    }
}

/* 把 2MB 页的属性改成 UC，避免 MMIO 读写被缓存合并。
 * 与 EHCI/xHCI 驱动使用同一套页表布局（PDPT 在 0x72000）。 */
static void mark_mmio_uc(uint32_t phys, uint32_t bytes) {
    uint32_t start = phys & ~0x1FFFFFu;
    uint64_t end = ((uint64_t)phys + bytes + 0x1FFFFFull) & ~0x1FFFFFull;
    if (end > 0x100000000ull) end = 0x100000000ull;

    for (uint64_t p = start; p < end; p += 0x200000ull) {
        uint32_t pdpt = (uint32_t)((p >> 30) & 3u);
        uint32_t pdi = (uint32_t)((p >> 21) & 0x1FFu);
        uint64_t *pd = (uint64_t *)(uintptr_t)(0x72000u + pdpt * 4096u);
        uint64_t entry = pd[pdi];
        if ((entry & 0x81ull) != 0x81ull) continue;
        entry &= ~((1ull << 3) | (1ull << 4) | (1ull << 12));
        entry |= (1ull << 3) | (1ull << 4);
        pd[pdi] = entry;
        __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)p) : "memory");
    }
}

static int wait_bits_set(uint32_t addr, uint32_t mask, uint64_t timeout) {
    uint64_t start = rdtsc_u64();
    while ((rd32(addr) & mask) != mask) {
        if (rdtsc_u64() - start > timeout) return 0;
    }
    return 1;
}

static int wait_bits_clear(uint32_t addr, uint32_t mask, uint64_t timeout) {
    uint64_t start = rdtsc_u64();
    while ((rd32(addr) & mask) != 0) {
        if (rdtsc_u64() - start > timeout) return 0;
    }
    return 1;
}

static int wait_tfd_idle(uint32_t port, uint64_t timeout) {
    uint64_t start = rdtsc_u64();
    while (rd32(port + PX_TFD) & (PX_TFD_BSY | PX_TFD_DRQ)) {
        if (rdtsc_u64() - start > timeout) return 0;
    }
    return 1;
}

/* ============================================================
 * PCI
 * ============================================================ */

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) |
                 ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                 ((uint32_t)off & 0xFCu);
    outl(0xCF8, a);
    return inl(0xCFC);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint16_t value) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) |
                 ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                 ((uint32_t)off & 0xFCu);
    outl(0xCF8, a);
    outw((uint16_t)(0xCFC + (off & 2u)), value);
}

static int find_ahci(void) {
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0x00);
            if (id == 0xFFFFFFFFu) continue;

            uint8_t header = pci_read8(bus, slot, 0, 0x0E);
            uint8_t funcs = (header & 0x80u) ? 8u : 1u;

            for (uint8_t fn = 0; fn < funcs; fn++) {
                uint32_t cfg = pci_read32(bus, slot, fn, 0x00);
                if (cfg == 0xFFFFFFFFu) continue;

                uint8_t prog = pci_read8(bus, slot, fn, 0x09);
                uint8_t sub = pci_read8(bus, slot, fn, 0x0A);
                uint8_t cls = pci_read8(bus, slot, fn, 0x0B);
                if (cls != 0x01u || sub != 0x06u || prog != 0x01u) continue;

                /* AHCI 的 ABAR 固定是 BAR5（偏移 0x24） */
                uint32_t lo = pci_read32(bus, slot, fn, 0x24);
                uint32_t hi = pci_read32(bus, slot, fn, 0x28);
                uint32_t btype = (lo >> 1) & 3u;
                uint32_t bar = lo & ~0x0Fu;

                printk("[AHCI] bus=%u slot=%u fn=%u BAR5=0x%x type=%u\n",
                       (unsigned int)bus, (unsigned int)slot,
                       (unsigned int)fn, lo, btype);
                if (lo & 1u) continue;                 /* I/O BAR，不是 AHCI */
                if (btype == 2u && hi != 0) {
                    printk("[AHCI] ABAR above 4GB, unsupported\n");
                    continue;
                }
                if (bar == 0) continue;

                uint16_t cmd = pci_read16(bus, slot, fn, 0x04);
                pci_write16(bus, slot, fn, 0x04,
                            (uint16_t)(cmd | 0x0006u));   /* MEM | BUSMASTER */

                g_abar = bar;
                mark_mmio_uc(g_abar, 0x2000u);
                return 1;
            }
        }
    }
    return 0;
}

/* ============================================================
 * 端口与命令
 * ============================================================ */

static int port_start(uint32_t port) {
    uint32_t p = port;
    uint32_t cmd = rd32(p + PX_CMD);

    /* 1. 停命令列表 */
    if (cmd & PX_CMD_ST) {
        wr32(p + PX_CMD, cmd & ~PX_CMD_ST);
        if (!wait_bits_clear(p + PX_CMD, PX_CMD_CR, AHCI_TIMEOUT_INIT)) return 0;
    }
    /* 2. 停 FIS 接收 */
    cmd = rd32(p + PX_CMD);
    if (cmd & PX_CMD_FRE) {
        wr32(p + PX_CMD, cmd & ~PX_CMD_FRE);
        if (!wait_bits_clear(p + PX_CMD, PX_CMD_FR, AHCI_TIMEOUT_INIT)) return 0;
    }

    /* 3. 命令列表 / FIS 缓冲基址 */
    wr32(p + PX_CLB, (uint32_t)(uintptr_t)g_cmd_list);
    wr32(p + PX_CLBU, 0);
    wr32(p + PX_FB, (uint32_t)(uintptr_t)g_fis);
    wr32(p + PX_FBU, 0);

    /* 4. 清中断/错误状态并上电 */
    wr32(p + PX_IS, 0xFFFFFFFFu);
    wr32(p + PX_SERR, 0xFFFFFFFFu);
    cmd = rd32(p + PX_CMD);
    wr32(p + PX_CMD, cmd | PX_CMD_POD);
    delay_loops(2000000u);

    /* 5. 启动 FIS 接收，再启动命令列表 */
    cmd = rd32(p + PX_CMD);
    wr32(p + PX_CMD, cmd | PX_CMD_FRE);
    if (!wait_bits_set(p + PX_CMD, PX_CMD_FR, AHCI_TIMEOUT_INIT)) return 0;

    cmd = rd32(p + PX_CMD);
    wr32(p + PX_CMD, cmd | PX_CMD_ST);
    if (!wait_bits_set(p + PX_CMD, PX_CMD_CR, AHCI_TIMEOUT_INIT)) return 0;

    return wait_tfd_idle(p, AHCI_TIMEOUT_INIT);
}

/* 发起一条 ATA 命令。lba48 决定 FIS 的 LBA 布局与命令码。
 * count 以 512 字节扇区为单位；只使用一个 PRDT。 */
static int ahci_issue(uint8_t command, uint32_t lba, uint32_t count,
                      void *buf, int write, int lba48) {
    if (!g_present) return 0;
    if (count == 0) return 0;

    uint32_t p = g_port_base;
    uint32_t bytes = count * 512u;
    if (bytes == 0) return 0;
    if ((uintptr_t)buf > 0xFFFFFFFFull) return 0;

    if (!wait_tfd_idle(p, AHCI_TIMEOUT_XFER)) return 0;
    wr32(p + PX_IS, 0xFFFFFFFFu);
    wr32(p + PX_SERR, 0xFFFFFFFFu);

    zero_mem(g_cmd_list, sizeof(g_cmd_list));
    zero_mem(g_cmd_tbl, sizeof(g_cmd_tbl));

    struct AhciCmdHdr *h = (struct AhciCmdHdr *)(void *)g_cmd_list;
    h->cfl_flags = (uint16_t)(5u | (write ? (1u << 6) : 0u));
    h->prdtl = 1;
    h->prdbc = 0;
    h->ctba = (uint32_t)(uintptr_t)g_cmd_tbl;
    h->ctbau = 0;

    uint8_t *f = g_cmd_tbl;             /* H2D Register FIS */
    f[0] = 0x27;
    f[1] = 0x80;                        /* C=1 */
    f[2] = command;
    f[3] = 0;                           /* features */
    f[4] = (uint8_t)(lba);
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    if (command == ATA_CMD_IDENTIFY) {
        f[7] = 0;
    } else if (lba48) {
        f[7] = 0x40;
    } else {
        f[7] = 0xE0;
    }
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[11] = 0;
    f[12] = (uint8_t)(count);
    f[13] = (uint8_t)(count >> 8);
    f[14] = 0;
    f[15] = 0;

    struct AhciPrdt *pr = (struct AhciPrdt *)(void *)(g_cmd_tbl + 0x80);
    pr->dba = (uint32_t)(uintptr_t)buf;
    pr->dbau = 0;
    pr->rsv = 0;
    pr->dbc = (bytes - 1u) | (1u << 31);

    cache_flush_range(g_cmd_list, sizeof(g_cmd_list));
    cache_flush_range(g_cmd_tbl, sizeof(g_cmd_tbl));
    if (write) cache_flush_range(buf, bytes);

    wr32(p + PX_CI, 1u);

    uint64_t start = rdtsc_u64();
    for (;;) {
        uint32_t is = rd32(p + PX_IS);
        uint32_t ci = rd32(p + PX_CI);
        if (is & PX_IS_ERROR) break;
        if ((ci & 1u) == 0) break;
        if (rdtsc_u64() - start > AHCI_TIMEOUT_XFER) return 0;
    }

    uint32_t is = rd32(p + PX_IS);
    uint32_t tfd = rd32(p + PX_TFD);

    if (is & PX_IS_ERROR) return 0;
    if (is & PX_IS_TFES) return 0;
    if (tfd & PX_TFD_ERR) return 0;
    if (rd32(p + PX_CI) & 1u) return 0;

    return 1;
}

static int ahci_identify(void) {
    zero_mem(g_identify, sizeof(g_identify));
    if (!ahci_issue(ATA_CMD_IDENTIFY, 0, 1, g_identify, 0, 0)) return 0;

    uint16_t *w = (uint16_t *)(void *)g_identify;
    uint32_t cap28 = (uint32_t)w[60] | ((uint32_t)w[61] << 16);
    int lba48 = (w[83] & (1u << 10)) ? 1 : 0;

    uint64_t cap48 = (uint64_t)w[100] |
                     ((uint64_t)w[101] << 16) |
                     ((uint64_t)w[102] << 32) |
                     ((uint64_t)w[103] << 48);

    /* 逻辑扇区大小：words 117..118，单位字（2 字节）。 */
    uint32_t logical = 512u;
    if (w[106] & (1u << 12)) {
        uint32_t sz = (uint32_t)w[117] | ((uint32_t)w[118] << 16);
        if (sz != 0) logical = sz * 2u;
    }
    if (logical != 512u) {
        printk("[AHCI] logical sector %u bytes unsupported\n", logical);
        return 0;
    }

    uint64_t total = (lba48 && cap48 != 0) ? cap48 : (uint64_t)cap28;
    if (total == 0) return 0;
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;

    g_lba48 = lba48;
    g_total_sectors = (uint32_t)total;
    return 1;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int ahci_init(void) {
    if (g_tried) return g_present;
    g_tried = 1;

    if (!find_ahci()) return 0;

    uint32_t ghc = rd32(g_abar + HBA_GHC);
    ghc |= HBA_GHC_AE;
    ghc &= ~HBA_GHC_IE;
    wr32(g_abar + HBA_GHC, ghc);

    uint32_t cap = rd32(g_abar + HBA_CAP);
    uint32_t pi = rd32(g_abar + HBA_PI);
    printk("[AHCI] CAP=0x%x PI=0x%x VS=0x%x\n",
           cap, pi, rd32(g_abar + HBA_VS));

    for (uint32_t port = 0; port < 32; port++) {
        if ((pi & (1u << port)) == 0) continue;

        uint32_t pb = g_abar + 0x100u + port * 0x80u;
        uint32_t ssts = rd32(pb + PX_SSTS);
        uint32_t sig = rd32(pb + PX_SIG);
        if ((ssts & 0xFu) != 3u) continue;
        if (sig != AHCI_SIG_ATA) {
            printk("[AHCI] port %u signature=0x%x (not ATA)\n",
                   port, sig);
            continue;
        }

        g_port = port;
        g_port_base = pb;
        g_present = 1;

        if (!port_start(pb)) {
            g_present = 0;
            printk("[AHCI] port %u start failed cmd=0x%x tfd=0x%x\n",
                   port, rd32(pb + PX_CMD), rd32(pb + PX_TFD));
            continue;
        }
        if (!ahci_identify()) {
            g_present = 0;
            printk("[AHCI] port %u identify failed is=0x%x tfd=0x%x\n",
                   port, rd32(pb + PX_IS), rd32(pb + PX_TFD));
            continue;
        }

        printk("[AHCI] port %u ready, %u sectors%s\n",
               port, (unsigned int)g_total_sectors,
               g_lba48 ? " (LBA48)" : " (LBA28)");
        return 1;
    }

    g_present = 0;
    return 0;
}

int ahci_present(void) {
    return g_present;
}

int ahci_read(uint32_t lba, uint8_t *buf) {
    if (!g_present) return 0;
    uint8_t cmd = g_lba48 ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA;
    if (!ahci_issue(cmd, lba, 1, buf, 0, g_lba48)) return 0;
    g_reads++;
    return 1;
}

int ahci_write(uint32_t lba, const uint8_t *buf) {
    if (!g_present) return 0;
    uint8_t cmd = g_lba48 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA;
    if (!ahci_issue(cmd, lba, 1, (void *)buf, 1, g_lba48)) return 0;
    g_writes++;
    return 1;
}

uint32_t ahci_total_sectors(void) {
    return g_total_sectors;
}

/* FLUSH CACHE：没有数据阶段，PRDT 长度置 0。
 * 机械盘掉电前必须执行，否则控制器写缓存里的已提交事务会丢。 */
int ahci_flush(void) {
    if (!g_present) return 0;

    uint32_t p = g_port_base;
    if (!wait_tfd_idle(p, AHCI_TIMEOUT_XFER)) return 0;
    wr32(p + PX_IS, 0xFFFFFFFFu);
    wr32(p + PX_SERR, 0xFFFFFFFFu);

    zero_mem(g_cmd_list, sizeof(g_cmd_list));
    zero_mem(g_cmd_tbl, sizeof(g_cmd_tbl));

    struct AhciCmdHdr *h = (struct AhciCmdHdr *)(void *)g_cmd_list;
    h->cfl_flags = 5u;
    h->prdtl = 0;
    h->prdbc = 0;
    h->ctba = (uint32_t)(uintptr_t)g_cmd_tbl;
    h->ctbau = 0;

    uint8_t *f = g_cmd_tbl;
    f[0] = 0x27;                 /* H2D Register FIS */
    f[1] = 0x80;                 /* C=1 */
    f[2] = 0xE7;                 /* FLUSH CACHE */
    f[7] = 0x40;                 /* LBA 模式 */

    cache_flush_range(g_cmd_list, sizeof(g_cmd_list));
    cache_flush_range(g_cmd_tbl, sizeof(g_cmd_tbl));

    wr32(p + PX_CI, 1u);

    uint64_t start = rdtsc_u64();
    for (;;) {
        uint32_t is = rd32(p + PX_IS);
        uint32_t ci = rd32(p + PX_CI);
        if (is & PX_IS_ERROR) break;
        if ((ci & 1u) == 0) break;
        if (rdtsc_u64() - start > AHCI_TIMEOUT_XFER) return 0;
    }

    uint32_t is = rd32(p + PX_IS);
    uint32_t tfd = rd32(p + PX_TFD);
    if (is & PX_IS_ERROR) return 0;
    if (is & PX_IS_TFES) return 0;
    if (tfd & PX_TFD_ERR) return 0;
    if (rd32(p + PX_CI) & 1u) return 0;
    return 1;
}

void ahci_stats(uint32_t *reads, uint32_t *writes) {
    if (reads) *reads = g_reads;
    if (writes) *writes = g_writes;
}

const char *ahci_name(void) {
    return "SATA(AHCI)";
}
