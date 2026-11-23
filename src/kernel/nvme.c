/* ============================================================
 * nvme.c - NVMe(PCIe) 块设备驱动
 * ------------------------------------------------------------
 * 实现范围：
 *   - PCI 枚举 class 0x01 / subclass 0x08 / progIF 0x02；
 *   - BAR0 64 位 MMIO（必须在 4GB 以下，内核只有 4GB 恒等映射）；
 *   - 控制器复位、Admin 队列、Identify Namespace/Controller；
 *   - 单 I/O 完成队列 + 提交队列，轮询完成，不开中断；
 *   - READ / WRITE / FLUSH 命令，512 字节扇区；
 *   - 统计读/写扇区数。
 *
 * 边界：
 *   - 只接受 512 字节逻辑块（LBADS=9）；4K-native 盘拒绝接管；
 *   - 只用命名空间 1、单 I/O 队列，一次一个未完成命令；
 *   - 数据经 512 字节对齐的 bounce buffer 中转，避免 PRP 跨页。
 * ============================================================ */

#include <stdint.h>
#include "nvme.h"
#include "io.h"
#include "tty.h"

/* ============================================================
 * 常量
 * ============================================================ */

#define NVME_TIMEOUT_INIT 5000000000ull
#define NVME_TIMEOUT_CMD  4000000000ull

#define NVME_QDEPTH       64

/* 控制器寄存器（相对 BAR0） */
#define REG_CAP    0x00u
#define REG_VS     0x08u
#define REG_INTMS  0x0Cu
#define REG_CC     0x14u
#define REG_CSTS   0x1Cu
#define REG_AQA    0x24u
#define REG_ASQ    0x28u
#define REG_ACQ    0x30u
#define DOORBELL   0x1000u

#define CC_EN      (1u << 0)
#define CC_IOSQES  (6u << 16)   /* 64 字节提交队列项 */
#define CC_IOCQES  (4u << 20)   /* 16 字节完成队列项 */

#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)

/* Admin 命令 */
#define ADM_CREATE_SQ 0x01u
#define ADM_CREATE_CQ 0x05u
#define ADM_IDENTIFY  0x06u

/* I/O 命令 */
#define IO_FLUSH 0x00u
#define IO_WRITE 0x01u
#define IO_READ  0x02u

#define NSID_1 1u

/* ============================================================
 * 数据结构（NVMe 规范字节布局）
 * ============================================================ */

struct __attribute__((packed)) NvmeCmd {
    uint8_t  opc;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct __attribute__((packed)) NvmeCqe {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sqhd;
    uint16_t sqid;
    uint32_t dw3;           /* CID(15:0) | P(16) | SF(24:17) | SC(27:25) */
};

struct NvmeQueue {
    uint8_t *sq;
    uint8_t *cq;
    uint16_t qsize;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t phase;
    uint32_t sq_db;
    uint32_t cq_db;
};

/* ============================================================
 * 静态 DMA 内存：4KB 对齐、物理连续、恒等映射
 * ============================================================ */

static uint8_t g_admin_sq[NVME_QDEPTH * 64] __attribute__((aligned(4096)));
static uint8_t g_admin_cq[NVME_QDEPTH * 16] __attribute__((aligned(4096)));
static uint8_t g_io_sq[NVME_QDEPTH * 64]    __attribute__((aligned(4096)));
static uint8_t g_io_cq[NVME_QDEPTH * 16]    __attribute__((aligned(4096)));
static uint8_t g_identify[4096]             __attribute__((aligned(4096)));
static uint8_t g_bounce[512]                __attribute__((aligned(512)));

/* ============================================================
 * 状态
 * ============================================================ */

static uint32_t g_mmio;
static uint32_t g_db_stride;
static uint32_t g_total_sectors;
static uint32_t g_reads;
static uint32_t g_writes;
static uint16_t g_cid;
static int      g_present;
static int      g_tried;
static char     g_name[44];

static struct NvmeQueue g_admin;
static struct NvmeQueue g_io;

/* 最近一次完成的原始状态，命令失败时打印出来便于定位 */
static uint32_t g_dbg_dw3;
static uint32_t g_dbg_sct;
static uint32_t g_dbg_sc;

/* ============================================================
 * 基础工具
 * ============================================================ */

static void zero_mem(void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}

static void copy_mem(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static uint64_t rdtsc_u64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint32_t nvme_rd32(uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(g_mmio + off);
}

static void nvme_wr32(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(g_mmio + off) = value;
}

static uint64_t nvme_rd64(uint32_t off) {
    uint32_t lo = nvme_rd32(off);
    uint32_t hi = nvme_rd32(off + 4);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void nvme_wr64(uint32_t off, uint64_t value) {
    nvme_wr32(off, (uint32_t)value);
    nvme_wr32(off + 4, (uint32_t)(value >> 32));
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

static uint32_t phys32(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

/* 把 2MB 页改成 UC，避免 MMIO 被缓存/合并。
 * 与 AHCI/EHCI/xHCI 使用同一页表布局（PDPT 在 0x71000 指向的 PD）。 */
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

/* ============================================================
 * PCI 配置空间
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

/* ============================================================
 * 控制器初始化
 * ============================================================ */

static int wait_csts(uint32_t mask, uint32_t value, uint64_t timeout) {
    uint64_t start = rdtsc_u64();
    while ((nvme_rd32(REG_CSTS) & mask) != value) {
        if (rdtsc_u64() - start > timeout) return 0;
    }
    return 1;
}

static void queue_init(struct NvmeQueue *q, uint8_t *sq, uint8_t *cq,
                       uint16_t qsize, uint16_t qid) {
    zero_mem(q, sizeof(*q));
    zero_mem(sq, (uint32_t)qsize * 64u);
    zero_mem(cq, (uint32_t)qsize * 16u);
    q->sq = sq;
    q->cq = cq;
    q->qsize = qsize;
    q->phase = 1;
    q->sq_db = DOORBELL + (2u * qid) * g_db_stride;
    q->cq_db = DOORBELL + (2u * qid + 1u) * g_db_stride;
}

static int queue_submit(struct NvmeQueue *q, struct NvmeCmd *cmd) {
    uint16_t cid = ++g_cid;
    cmd->cid = cid;

    struct NvmeCmd *slot =
        (struct NvmeCmd *)(void *)(q->sq + (uint32_t)q->sq_tail * 64u);
    copy_mem(slot, cmd, 64);
    cache_flush_range(slot, 64);

    q->sq_tail = (uint16_t)((q->sq_tail + 1u) % q->qsize);
    nvme_wr32(q->sq_db, q->sq_tail);

    uint64_t start = rdtsc_u64();
    struct NvmeCqe *cqe =
        (struct NvmeCqe *)(void *)(q->cq + (uint32_t)q->cq_head * 16u);

    for (;;) {
        uint32_t dw3 = *(volatile uint32_t *)((uintptr_t)cqe + 12u);
        uint16_t phase = (uint16_t)((dw3 >> 16) & 1u);
        if (phase == q->phase) {
            __asm__ volatile ("mfence" ::: "memory");
            uint16_t rid = (uint16_t)(dw3 & 0xFFFFu);
            uint32_t status = dw3 >> 17;
            uint32_t sc = status & 0xFFu;
            uint32_t sct = (status >> 8) & 0x07u;

            g_dbg_dw3 = dw3;
            g_dbg_sct = sct;
            g_dbg_sc = sc;

            q->cq_head = (uint16_t)((q->cq_head + 1u) % q->qsize);
            if (q->cq_head == 0) q->phase ^= 1u;
            nvme_wr32(q->cq_db, q->cq_head);

            if (rid != cid) return 0;
            return (sc == 0 && sct == 0) ? 1 : 0;
        }
        if (rdtsc_u64() - start > NVME_TIMEOUT_CMD) return 0;
    }
}

static int nvme_identify(uint32_t nsid, uint32_t cns) {
    struct NvmeCmd cmd;
    zero_mem(&cmd, sizeof(cmd));
    cmd.opc = ADM_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = phys32(g_identify);
    cmd.cdw10 = cns;
    return queue_submit(&g_admin, &cmd);
}

static int nvme_create_io_queues(uint16_t qsize) {
    struct NvmeCmd cmd;

    /* 完成队列：CDW10 = QID(15:0) | QSIZE(31:16)；
     * CDW11 = PC(bit0) | IEN(bit1) | IV(31:16)。
     * 轮询模式 IEN=0；Create 命令的 QID/QSIZE 位置与 I/O 命令的
     * CDW10 不同，这里必须按 16 位字段拼。 */
    zero_mem(&cmd, sizeof(cmd));
    cmd.opc = ADM_CREATE_CQ;
    cmd.prp1 = phys32(g_io_cq);
    cmd.cdw10 = 1u | ((uint32_t)(qsize - 1u) << 16);
    cmd.cdw11 = 1u;
    printk("[NVMe] create CQ: qsize=%u prp=0x%x\n",
           (unsigned int)qsize, (unsigned int)phys32(g_io_cq));
    if (!queue_submit(&g_admin, &cmd)) {
        printk("[NVMe] create I/O CQ failed (dw3=%x sct=%u sc=%u)\n",
               (unsigned int)g_dbg_dw3, (unsigned int)g_dbg_sct,
               (unsigned int)g_dbg_sc);
        return 0;
    }

    /* 提交队列：CDW10 = QID(15:0) | QSIZE(31:16)；
     * CDW11 = PC(bit0) | QPRIO(2:1) | CQID(31:16)。 */
    zero_mem(&cmd, sizeof(cmd));
    cmd.opc = ADM_CREATE_SQ;
    cmd.prp1 = phys32(g_io_sq);
    cmd.cdw10 = 1u | ((uint32_t)(qsize - 1u) << 16);
    cmd.cdw11 = 1u | (1u << 16);
    printk("[NVMe] create SQ: qsize=%u prp=0x%x\n",
           (unsigned int)qsize, (unsigned int)phys32(g_io_sq));
    if (!queue_submit(&g_admin, &cmd)) {
        printk("[NVMe] create I/O SQ failed (dw3=%x sct=%u sc=%u)\n",
               (unsigned int)g_dbg_dw3, (unsigned int)g_dbg_sct,
               (unsigned int)g_dbg_sc);
        return 0;
    }
    return 1;
}

static int nvme_find(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func,
                     uint32_t *out_bar) {
    for (uint8_t bus = 0; bus < 16; bus++) {
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
                if (cls != 0x01u || sub != 0x08u || prog != 0x02u) continue;

                uint32_t lo = pci_read32(bus, slot, fn, 0x10);
                uint32_t hi = pci_read32(bus, slot, fn, 0x14);
                if (lo & 1u) continue;                  /* I/O BAR，不是 NVMe */

                uint32_t type = (lo >> 1) & 3u;
                uint64_t bar;
                if (type == 2u) {
                    bar = ((uint64_t)hi << 32) | (lo & ~0x0Full);
                } else if (type == 0u) {
                    bar = lo & ~0x0Full;
                } else {
                    continue;                           /* 64 位 BAR 的 hi */
                }
                if (bar == 0) continue;
                if ((bar >> 32) != 0) {
                    printk("[NVMe] BAR0 above 4GB (0x%x%08x), unsupported\n",
                           (unsigned int)(bar >> 32), (unsigned int)bar);
                    continue;
                }

                *out_bus = bus;
                *out_slot = slot;
                *out_func = fn;
                *out_bar = (uint32_t)bar;
                return 1;
            }
        }
    }
    return 0;
}

int nvme_init(void) {
    if (g_tried) return g_present;
    g_tried = 1;

    uint8_t bus, slot, fn;
    uint32_t bar;
    if (!nvme_find(&bus, &slot, &fn, &bar)) {
        return 0;
    }

    g_mmio = bar;
    mark_mmio_uc(g_mmio, 0x2000u);

    /* 打开 Memory Space + Bus Master */
    uint16_t cmdreg = pci_read16(bus, slot, fn, 0x04);
    pci_write16(bus, slot, fn, 0x04, (uint16_t)(cmdreg | 0x0006u));

    /* 轮询模式：屏蔽所有中断，避免未安装的 IRQ 线上报 */
    nvme_wr32(REG_INTMS, 0xFFFFFFFFu);

    uint64_t cap = nvme_rd64(REG_CAP);
    uint32_t vs = nvme_rd32(REG_VS);
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0x0Fu);
    g_db_stride = 4u << dstrd;

    uint32_t maxq = (uint32_t)(cap & 0xFFFFu) + 1u;
    if (maxq < 2u) {
        printk("[NVMe] invalid MQES\n");
        return 0;
    }
    uint16_t qsize = NVME_QDEPTH;
    while ((uint32_t)qsize > maxq) qsize >>= 1;
    if (qsize < 2u) {
        printk("[NVMe] queue depth too small\n");
        return 0;
    }

    printk("[NVMe] bus=%u slot=%u fn=%u BAR0=0x%x ver=%u.%u q=%u\n",
           (unsigned int)bus, (unsigned int)slot, (unsigned int)fn,
           (unsigned int)g_mmio, (unsigned int)(vs >> 16),
           (unsigned int)((vs >> 8) & 0xFFu), (unsigned int)qsize);

    /* 复位控制器 */
    nvme_wr32(REG_CC, 0);
    if (!wait_csts(CSTS_RDY, 0, NVME_TIMEOUT_INIT)) {
        printk("[NVMe] controller disable timeout\n");
        return 0;
    }

    queue_init(&g_admin, g_admin_sq, g_admin_cq, qsize, 0);
    nvme_wr32(REG_AQA, ((uint32_t)(qsize - 1u) << 16) | (uint32_t)(qsize - 1u));
    nvme_wr64(REG_ASQ, (uint64_t)phys32(g_admin_sq));
    nvme_wr64(REG_ACQ, (uint64_t)phys32(g_admin_cq));

    nvme_wr32(REG_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    if (!wait_csts(CSTS_RDY, CSTS_RDY, NVME_TIMEOUT_INIT)) {
        printk("[NVMe] controller enable timeout\n");
        return 0;
    }
    if (nvme_rd32(REG_CSTS) & CSTS_CFS) {
        printk("[NVMe] controller fatal status\n");
        return 0;
    }

    /* Identify Namespace 1：CNS=0x00 是命名空间，0x01 才是控制器 */
    zero_mem(g_identify, sizeof(g_identify));
    if (!nvme_identify(NSID_1, 0x00u)) {
        printk("[NVMe] identify namespace failed\n");
        return 0;
    }

    uint64_t nsze = *(uint64_t *)(void *)(g_identify + 0);
    uint8_t nlbaf = g_identify[25];
    uint8_t flbas = (uint8_t)(g_identify[26] & 0x0Fu);
    uint32_t lbaf = *(uint32_t *)(void *)(g_identify + 128 + (uint32_t)flbas * 4u);
    uint32_t lbads = (lbaf >> 16) & 0xFFu;

    if (nsze == 0 || nlbaf == 0) {
        printk("[NVMe] namespace 1 is empty (nsze=0, nlbaf=%u)\n",
               (unsigned int)nlbaf);
        return 0;
    }
    if (lbads != 9u) {
        printk("[NVMe] unsupported LBA size 2^%u (need 512)\n",
               (unsigned int)lbads);
        return 0;
    }
    if (nsze > 0xFFFFFFFFull) {
        printk("[NVMe] namespace > 2TB, truncating to 32-bit LBA\n");
        nsze = 0xFFFFFFFFull;
    }

    /* Identify Controller：CNS=0x01 */
    zero_mem(g_identify, sizeof(g_identify));
    if (nvme_identify(0, 0x01u)) {
        int n = 0;
        for (int i = 0; i < 40 && n < (int)sizeof(g_name) - 1; i++) {
            char c = (char)g_identify[24 + i];
            if (c == '\0') break;
            if (c == ' ' && n == 0) continue;
            g_name[n++] = c;
        }
        while (n > 0 && g_name[n - 1] == ' ') n--;
        g_name[n] = '\0';
    }
    if (!g_name[0]) {
        const char *def = "NVMe SSD";
        int n = 0;
        while (def[n] && n < (int)sizeof(g_name) - 1) {
            g_name[n] = def[n];
            n++;
        }
        g_name[n] = '\0';
    }

    if (!nvme_create_io_queues(qsize)) {
        return 0;
    }
    queue_init(&g_io, g_io_sq, g_io_cq, qsize, 1);

    g_total_sectors = (uint32_t)nsze;
    g_present = 1;
    printk("[NVMe] %s, %u sectors (%u MB)\n",
           g_name, g_total_sectors, (unsigned int)(g_total_sectors / 2048u));
    return 1;
}

/* ============================================================
 * 块读写
 * ============================================================ */

static int nvme_rw(uint32_t lba, void *buf, int write) {
    if (!g_present) return 0;
    if (lba >= g_total_sectors) return 0;

    if (write) {
        copy_mem(g_bounce, buf, 512);
        cache_flush_range(g_bounce, 512);
    }

    struct NvmeCmd cmd;
    zero_mem(&cmd, sizeof(cmd));
    cmd.opc = write ? IO_WRITE : IO_READ;
    cmd.nsid = NSID_1;
    cmd.prp1 = phys32(g_bounce);
    cmd.cdw10 = lba;
    cmd.cdw11 = 0;
    cmd.cdw12 = 0;              /* NLB = 0：一个逻辑块 */

    if (!queue_submit(&g_io, &cmd)) {
        return 0;
    }

    if (!write) {
        __asm__ volatile ("mfence" ::: "memory");
        copy_mem(buf, g_bounce, 512);
        g_reads++;
    } else {
        g_writes++;
    }
    return 1;
}

int nvme_read(uint32_t lba, uint8_t *buf) {
    return nvme_rw(lba, buf, 0);
}

int nvme_write(uint32_t lba, const uint8_t *buf) {
    return nvme_rw(lba, (void *)buf, 1);
}

int nvme_flush(void) {
    if (!g_present) return 0;

    struct NvmeCmd cmd;
    zero_mem(&cmd, sizeof(cmd));
    cmd.opc = IO_FLUSH;
    cmd.nsid = NSID_1;
    return queue_submit(&g_io, &cmd);
}

int nvme_present(void) {
    return g_present;
}

uint32_t nvme_total_sectors(void) {
    return g_total_sectors;
}

void nvme_stats(uint32_t *reads, uint32_t *writes) {
    if (reads) *reads = g_reads;
    if (writes) *writes = g_writes;
}

const char *nvme_name(void) {
    return g_name[0] ? g_name : "NVMe SSD";
}
