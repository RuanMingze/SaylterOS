/* ============================================================
 * usb_msc.c - xHCI + USB Mass Storage (Bulk-Only Transport)
 * ------------------------------------------------------------
 * 目标：让 SaylterOS 直接从 U 盘读写 SlyzFS，不再依赖 0x1F0 的
 * ATA PIO。
 *
 * 实现范围
 *   - xHCI（USB 3.0 主机控制器）PCI 探测与 MMIO 初始化；
 *   - 根端口复位、Enable Slot、Address Device、Configure Endpoint；
 *   - 设备/配置描述符解析，只接受 Mass Storage / SCSI / BOT
 *     （bInterfaceClass=8, SubClass=6, Protocol=0x50）；
 *   - Bulk-Only Transport 的 CBW/CSW 与 SCSI 命令：
 *     INQUIRY / TEST UNIT READY / READ CAPACITY(10|16) /
 *     READ(10) / WRITE(10)；
 *   - 轮询事件环，不依赖中断，避免和现有 IRQ 路由耦合。
 *
 * 边界
 *   - 只处理直接挂在根端口上的设备（route string = 0），
 *     不做 USB Hub 级联；
 *   - 只支持 512 字节逻辑块；
 *   - 初始化失败时 present() 返回 0，blk.c 自动回退 ATA。
 *
 * 并发
 *   所有传输串行化，调用方（文件系统）本身也是单线程的顺序访问。
 * ============================================================ */

#include <stdint.h>
#include "usb_msc.h"
#include "io.h"
#include "tty.h"

/* ============================================================
 * 常量
 * ============================================================ */

#define USB_TIMEOUT_INIT   2500000000ull   /* ~1s @2.5GHz */
#define USB_TIMEOUT_XFER    800000000ull   /* ~0.3s */

#define TRB_COUNT          256u
#define TRB_BYTES          (TRB_COUNT * 16u)

/* xHCI 寄存器偏移 */
#define XHCI_CAPLENGTH     0x00u
#define XHCI_HCSPARAMS1    0x04u
#define XHCI_HCSPARAMS2    0x08u
#define XHCI_DBOFF         0x14u
#define XHCI_RTSOFF        0x18u

#define OP_USBCMD          0x00u
#define OP_USBSTS          0x04u
#define OP_PAGESIZE        0x08u
#define OP_CRCR            0x18u
#define OP_DCBAAP          0x30u
#define OP_CONFIG          0x38u
#define OP_PORTS           0x400u

#define RT_IMAN            0x20u
#define RT_IMOD            0x24u
#define RT_ERSTSZ          0x28u
#define RT_ERSTBA          0x30u
#define RT_ERDP            0x38u

/* TRB 标志 */
#define TRB_C              (1u << 0)
#define TRB_TC             (1u << 1)
#define TRB_IOC            (1u << 5)
#define TRB_IDT            (1u << 6)
#define TRB_DIR_IN         (1u << 16)
#define TRB_TYPE(t)        ((uint32_t)(t) << 10)

#define TRB_NORMAL         1u
#define TRB_SETUP          2u
#define TRB_DATA           3u
#define TRB_STATUS         4u
#define TRB_LINK           6u
#define TRB_ENABLE_SLOT    9u
#define TRB_ADDRESS_DEV    11u
#define TRB_CONFIGURE_EP   12u
#define TRB_EVT_TRANSFER   32u
#define TRB_EVT_CMD        33u

/* USB 描述符类型 */
#define DESC_DEVICE        1u
#define DESC_CONFIG        2u
#define DESC_INTERFACE     4u
#define DESC_ENDPOINT      5u

/* ============================================================
 * 静态 DMA 内存（恒等映射，虚拟地址 == 物理地址）
 * ============================================================ */

struct __attribute__((packed)) XhciTrb {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
};

struct __attribute__((packed)) XhciErstEntry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
};

static uint8_t  g_cmd_mem[TRB_BYTES]    __attribute__((aligned(4096)));
static uint8_t  g_evt_mem[TRB_BYTES]    __attribute__((aligned(4096)));
static uint8_t  g_ep0_mem[TRB_BYTES]    __attribute__((aligned(4096)));
static uint8_t  g_bin_mem[TRB_BYTES]    __attribute__((aligned(4096)));
static uint8_t  g_bout_mem[TRB_BYTES]   __attribute__((aligned(4096)));
static uint8_t  g_dcbaa[4096]           __attribute__((aligned(4096)));
static uint8_t  g_dev_ctx[4096]         __attribute__((aligned(4096)));
static uint8_t  g_in_ctx[4096]          __attribute__((aligned(4096)));
static uint8_t  g_xfer_buf[4096]        __attribute__((aligned(4096)));
static uint8_t  g_desc[1024]            __attribute__((aligned(64)));
static uint8_t  g_cbw[64]               __attribute__((aligned(64)));
static uint8_t  g_csw[64]               __attribute__((aligned(64)));
static uint8_t  g_verify_buf[512]       __attribute__((aligned(64)));
static struct XhciErstEntry g_erst      __attribute__((aligned(64)));

/* ============================================================
 * 运行状态
 * ============================================================ */

struct XhciRing {
    struct XhciTrb *trbs;
    uint32_t        enq;
    uint8_t         cycle;
    uint32_t        base_lo;
    uint32_t        base_hi;
};

static struct XhciRing g_cmd;
static struct XhciRing g_ep0;
static struct XhciRing g_bin;
static struct XhciRing g_bout;

static uint32_t g_mmio;
static uint32_t g_caplen;
static uint32_t g_op;
static uint32_t g_db;
static uint32_t g_rt;
static uint32_t g_max_slots;
static uint32_t g_max_ports;

static uint32_t g_evt_deq;
static uint8_t  g_evt_cycle;

static uint32_t g_slot;
static uint32_t g_dci_in;
static uint32_t g_dci_out;
static uint32_t g_mps_in;
static uint32_t g_mps_out;

static uint32_t g_total_sectors;
static uint32_t g_tag;
static uint32_t g_read_sectors;
static uint32_t g_write_sectors;
static int      g_state;          /* 0 未试，1 可用，2 已失败 */
static char     g_name[32];

/* ---- 控制器共享状态 ---- */
static int      g_ctrl_ready;     /* xHCI 已初始化（MSC 或键盘） */
static uint32_t g_msc_port;       /* MSC 占用的根端口（0 = 无） */

/* ---- USB HID 启动键盘（独立槽位） ---- */
static uint8_t  g_kbd_ep0_mem[TRB_BYTES]  __attribute__((aligned(4096)));
static uint8_t  g_kbd_in_mem[TRB_BYTES]   __attribute__((aligned(4096)));
static uint8_t  g_kbd_dev_ctx[4096]       __attribute__((aligned(4096)));
static uint8_t  g_kbd_report[64]          __attribute__((aligned(64)));
static struct XhciRing g_kbd_ep0;
static struct XhciRing g_kbd_in;
static uint32_t g_kbd_slot;
static uint32_t g_kbd_dci;
static uint32_t g_kbd_mps;
static uint32_t g_kbd_interval;
static int      g_kbd_ready;
static int      g_kbd_pending;
static int      g_kbd_tried;
static uint64_t g_kbd_last_try;
static uint32_t g_kbd_port;       /* 键盘占用的根端口 */
static uint32_t g_kbd_fail_mask;  /* 已确认不是键盘的端口 */

/* ============================================================
 * 基础工具
 * ============================================================ */

static void u_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

static void u_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static uint64_t u_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint32_t u_rd32(const void *p) {
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void u_wr32(void *p, uint32_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void wr32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static uint64_t rd64(uint32_t addr) {
    return *(volatile uint64_t *)(uintptr_t)addr;
}

static void wr64(uint32_t addr, uint64_t value) {
    *(volatile uint64_t *)(uintptr_t)addr = value;
}
/* ============================================================
 * MMIO 缓存类型：标记为 UC
 * ------------------------------------------------------------
 * xHCI 的 BAR 必须按 uncached 访问，否则写合并/乱序会让寄存器
 * 读写失效。使用 2MB 大页的 PWT|PCD（PAT 索引 3 = UC），不依赖
 * PAT MSR 的当前配置。
 * ============================================================ */

/* ============================================================
 * DMA 缓存维护
 * ------------------------------------------------------------
 * xHCI 读 ring / 输入上下文 / 数据缓冲时不会 snoop CPU 缓存。
 * 所有交给控制器的 WB 内存必须先 clflush 写回；IN 传输前也要
 * 先失效，避免旧的脏行在 DMA 之后被写回覆盖设备数据。
 * ============================================================ */

static void cache_flush_range(const void *addr, uint32_t bytes) {
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)addr + (uintptr_t)bytes;
    while (p < end) {
        __asm__ volatile ("clflush (%0)" :: "r"((void *)p) : "memory");
        p += 64u;
    }
    __asm__ volatile ("mfence" ::: "memory");
}

static void flush_ring(struct XhciRing *ring) {
    cache_flush_range(ring->trbs, TRB_BYTES);
}

static void mark_mmio_uc(uint32_t phys, uint32_t bytes) {    uint32_t start = phys & ~0x1FFFFFu;
    uint64_t end = ((uint64_t)phys + bytes + 0x1FFFFFull) & ~0x1FFFFFull;
    if (end > 0x100000000ull) {
        end = 0x100000000ull;
    }

    for (uint64_t p = start; p < end; p += 0x200000ull) {
        uint32_t pdpt = (uint32_t)((p >> 30) & 3u);
        uint32_t pdi = (uint32_t)((p >> 21) & 0x1FFu);
        uint64_t *pd = (uint64_t *)(uintptr_t)(0x72000u + pdpt * 4096u);
        uint64_t entry = pd[pdi];
        if ((entry & 0x81ull) != 0x81ull) {
            continue;
        }
        entry &= ~((1ull << 3) | (1ull << 4) | (1ull << 12));
        entry |= (1ull << 3) | (1ull << 4);   /* PWT | PCD -> UC */
        pd[pdi] = entry;
        __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)p) : "memory");
    }
}

/* ============================================================
 * PCI
 * ============================================================ */

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint16_t value) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outl(0xCF8, addr);
    outw((uint16_t)(0xCFC + (off & 2u)), value);
}

static uint64_t pci_bar64(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t lo = pci_read32(bus, slot, func, off);
    if (lo & 1u) {
        return 0;
    }
    uint64_t bar = (uint64_t)(lo & ~0x0Fu);
    if (((lo >> 1) & 3u) == 2u) {
        uint32_t hi = pci_read32(bus, slot, func, (uint8_t)(off + 4u));
        bar |= ((uint64_t)hi) << 32;
    }
    return bar;
}

/* ============================================================
 * TRB 环
 * ============================================================ */

static void ring_init(struct XhciRing *ring, void *mem) {
    ring->trbs = (struct XhciTrb *)mem;
    ring->enq = 0;
    ring->cycle = 1;
    ring->base_lo = (uint32_t)(uintptr_t)mem;
    ring->base_hi = (uint32_t)(((uint64_t)(uintptr_t)mem) >> 32);
    u_memset(mem, 0, TRB_BYTES);
}

static void ring_push(struct XhciRing *ring, uint32_t dw0, uint32_t dw1,
                      uint32_t dw2, uint32_t type_flags) {
    if (ring->enq >= TRB_COUNT - 1u) {
        struct XhciTrb *link = &ring->trbs[TRB_COUNT - 1u];
        link->dw0 = ring->base_lo;
        link->dw1 = ring->base_hi;
        link->dw2 = 0;
        link->dw3 = TRB_TYPE(TRB_LINK) | TRB_TC | (uint32_t)(ring->cycle & 1u);
        ring->enq = 0;
        ring->cycle ^= 1u;
    }

    struct XhciTrb *t = &ring->trbs[ring->enq];
    t->dw0 = dw0;
    t->dw1 = dw1;
    t->dw2 = dw2;
    t->dw3 = type_flags | (uint32_t)(ring->cycle & 1u);
    ring->enq++;
}

static void doorbell(uint32_t slot, uint32_t target) {
    wr32(g_db + slot * 4u, target);
}

/* ============================================================
 * 事件环
 * ============================================================ */

static int event_wait(uint32_t want_type, uint32_t want_ep,
                      uint32_t *out_dw2, uint32_t *out_dw3,
                      uint64_t timeout) {
    uint64_t start = u_rdtsc();

    for (;;) {
        struct XhciTrb *t = &((struct XhciTrb *)(void *)g_evt_mem)[g_evt_deq];
        uint32_t dw3 = t->dw3;

        if ((dw3 & 1u) == (uint32_t)(g_evt_cycle & 1u)) {
            uint32_t type = (dw3 >> 10) & 0x3Fu;
            uint32_t ep = (dw3 >> 24) & 0xFFu;
            uint32_t ep16 = (dw3 >> 16) & 0xFFu;
            uint32_t dw2 = t->dw2;

            g_evt_deq++;
            if (g_evt_deq >= TRB_COUNT) {
                g_evt_deq = 0;
                g_evt_cycle ^= 1u;
            }
            wr64(g_rt + RT_ERDP,
                 ((uint64_t)(uintptr_t)g_evt_mem) +
                 (uint64_t)g_evt_deq * 16ull + 8ull);

            /* 规范把 Endpoint ID 放在 bits 31:24；部分实现（QEMU）
             * 放在 bits 23:16。两个位置都接受，避免漏掉传输完成。 */
            if (type == want_type &&
                (want_ep == 0xFFFFFFFFu || ep == want_ep || ep16 == want_ep)) {
                if (out_dw2) *out_dw2 = dw2;
                if (out_dw3) *out_dw3 = dw3;
                return 1;
            }
            continue;
        }

        if (u_rdtsc() - start > timeout) {
            struct XhciTrb *e =
                &((struct XhciTrb *)(void *)g_evt_mem)[g_evt_deq];
            printk("[USB] evt timeout type=%u ep=%u deq=%u cyc=%u raw=%x %x %x %x\n",
                   want_type, want_ep, g_evt_deq,
                   (unsigned int)g_evt_cycle,
                   e->dw0, e->dw1, e->dw2, e->dw3);
            for (uint32_t i = 0; i < 12u; i++) {
                struct XhciTrb *q =
                    &((struct XhciTrb *)(void *)g_evt_mem)[i];
                printk("  E%u=%x,%x", i, q->dw2, q->dw3);
            }
            printk("\n");
            return 0;
        }
    }
}

/* 非阻塞取出一个事件（供 USB 键盘轮询使用）。同时会推进 ERDP。 */
static int event_poll_one(uint32_t *out_type, uint32_t *out_ep,
                          uint32_t *out_dw2, uint32_t *out_dw3) {
    struct XhciTrb *t = &((struct XhciTrb *)(void *)g_evt_mem)[g_evt_deq];

    if ((t->dw3 & 1u) != (uint32_t)(g_evt_cycle & 1u)) return 0;

    g_evt_deq++;
    if (g_evt_deq >= TRB_COUNT) {
        g_evt_deq = 0;
        g_evt_cycle ^= 1u;
    }
    wr64(g_rt + RT_ERDP,
         ((uint64_t)(uintptr_t)g_evt_mem) +
         (uint64_t)g_evt_deq * 16ull + 8ull);

    if (out_type) *out_type = (t->dw3 >> 10) & 0x3Fu;
    if (out_ep)   *out_ep   = (t->dw3 >> 24) & 0xFFu;
    if (out_dw2)  *out_dw2  = t->dw2;
    if (out_dw3)  *out_dw3  = t->dw3;
    return 1;
}

static int wait_bits_clear(uint32_t addr, uint32_t mask, uint64_t timeout) {    uint64_t start = u_rdtsc();
    while (rd32(addr) & mask) {
        if (u_rdtsc() - start > timeout) {
            return 0;
        }
    }
    return 1;
}

static int wait_bits_set(uint32_t addr, uint32_t mask, uint64_t timeout) {
    uint64_t start = u_rdtsc();
    while ((rd32(addr) & mask) != mask) {
        if (u_rdtsc() - start > timeout) {
            return 0;
        }
    }
    return 1;
}

/* ============================================================
 * 控制传输
 * ============================================================ */

static int control_transfer(uint8_t bm, uint8_t br, uint16_t wvalue,
                            uint16_t windex, void *buf, uint16_t len, int in) {
    ring_push(&g_ep0,
              (uint32_t)bm | ((uint32_t)br << 8) | ((uint32_t)wvalue << 16),
              (uint32_t)windex | ((uint32_t)len << 16),
              8u,
              TRB_TYPE(TRB_SETUP) | TRB_IDT);

    if (len != 0) {
        uint32_t addr = (uint32_t)(uintptr_t)buf;
        ring_push(&g_ep0, addr,
                  (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32),
                  (uint32_t)len,
                  TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0u));
    }

    uint32_t status_dir = (len != 0 && in) ? 0u : TRB_DIR_IN;
    ring_push(&g_ep0, 0, 0, 0,
              TRB_TYPE(TRB_STATUS) | status_dir | TRB_IOC);

    flush_ring(&g_ep0);
    if (len != 0) {
        cache_flush_range(buf, len);
    }

    doorbell(g_slot, 1);

    uint32_t dw2 = 0, dw3 = 0;
    if (!event_wait(TRB_EVT_TRANSFER, 1u, &dw2, &dw3, USB_TIMEOUT_XFER)) {
        return 0;
    }
    if (len != 0) {
        cache_flush_range(buf, len);
    }
    uint32_t cc = (dw2 >> 24) & 0xFFu;
    return (cc == 1u || cc == 13u);
}

/* ============================================================
 * Bulk 传输
 * ============================================================ */

static int bulk_transfer(uint32_t dci, void *buf, uint32_t len, int in) {
    struct XhciRing *ring = in ? &g_bin : &g_bout;

    ring_push(ring,
              (uint32_t)(uintptr_t)buf,
              (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32),
              len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);

    flush_ring(ring);
    cache_flush_range(buf, len);

    doorbell(g_slot, dci);

    uint32_t dw2 = 0, dw3 = 0;
    if (!event_wait(TRB_EVT_TRANSFER, dci, &dw2, &dw3, USB_TIMEOUT_XFER)) {
        cache_flush_range(g_dev_ctx, sizeof(g_dev_ctx));
        uint32_t *dc = (uint32_t *)(void *)g_dev_ctx;
        printk("[USB] bulk timeout dci=%u len=%u in=%u sts=0x%x epdeq=%x:%x\n",
               dci, len, in, (unsigned int)rd32(g_op + OP_USBSTS),
               dc[dci * 8u + 3u], dc[dci * 8u + 2u]);
        return 0;
    }
    cache_flush_range(buf, len);
    uint32_t cc = (dw2 >> 24) & 0xFFu;
    if (cc != 1u && cc != 13u) {
        printk("[USB] bulk cc=%u dci=%u len=%u in=%u\n", cc, dci, len, in);
    }
    return (cc == 1u || cc == 13u);
}

/* ============================================================
 * BOT（Bulk-Only Transport）
 * ============================================================ */

static int bot_command(const uint8_t *cb, uint32_t cb_len,
                       void *data, uint32_t data_len, int data_in) {
    u_memset(g_cbw, 0, 31);
    u_wr32(g_cbw + 0,  0x43425355u);          /* "USBC" */
    g_tag++;
    u_wr32(g_cbw + 4,  g_tag);
    u_wr32(g_cbw + 8,  data_len);
    g_cbw[12] = (uint8_t)(data_in ? 0x80 : 0x00);
    g_cbw[13] = 0;
    g_cbw[14] = (uint8_t)cb_len;
    for (uint32_t i = 0; i < cb_len && i < 16u; i++) {
        g_cbw[15 + i] = cb[i];
    }

    if (!bulk_transfer(g_dci_out, g_cbw, 31u, 0)) {
        printk("[USB] bot: CBW stage failed\n");
        return 0;
    }

    if (data_len != 0) {
        if (!bulk_transfer(data_in ? g_dci_in : g_dci_out,
                           data, data_len, data_in)) {
            printk("[USB] bot: data stage failed (in=%u len=%u)\n",
                   (unsigned int)data_in, data_len);
            return 0;
        }
    }

    if (!bulk_transfer(g_dci_in, g_csw, 13u, 1)) {
        printk("[USB] bot: CSW stage failed\n");
        return 0;
    }

    if (u_rd32(g_csw + 0) != 0x53425355u) {   /* "USBS" */
        printk("[USB] bad CSW sig=0x%x tag=%u\n",
               u_rd32(g_csw + 0), u_rd32(g_csw + 4));
        return 0;
    }
    if (u_rd32(g_csw + 4) != g_tag) {
        printk("[USB] CSW tag mismatch %u != %u\n",
               u_rd32(g_csw + 4), g_tag);
        return 0;
    }
    if (g_csw[12] != 0u) {
        printk("[USB] CSW status=%u\n", (unsigned int)g_csw[12]);
        return 0;
    }
    return 1;
}

/* ============================================================
 * 控制器初始化
 * ============================================================ */

static int find_xhci(void) {
    for (uint8_t bus = 0; bus < 4; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0x00);
            if (id == 0xFFFFFFFFu) {
                continue;
            }
            uint8_t header = pci_read8(bus, slot, 0, 0x0E);
            uint8_t funcs = (header & 0x80u) ? 8u : 1u;

            for (uint8_t fn = 0; fn < funcs; fn++) {
                uint32_t cfg = pci_read32(bus, slot, fn, 0x00);
                if (cfg == 0xFFFFFFFFu) {
                    continue;
                }
                uint8_t prog = pci_read8(bus, slot, fn, 0x09);
                uint8_t sub  = pci_read8(bus, slot, fn, 0x0A);
                uint8_t cls  = pci_read8(bus, slot, fn, 0x0B);

                if (cls != 0x0Cu || sub != 0x03u || prog != 0x30u) {
                    continue;
                }

                uint64_t bar = pci_bar64(bus, slot, fn, 0x10);
                if (bar == 0 || bar >= 0x100000000ull) {
                    return 0;
                }

                uint16_t cmd = pci_read16(bus, slot, fn, 0x04);
                pci_write16(bus, slot, fn, 0x04,
                            (uint16_t)(cmd | 0x0006u));   /* MEM | MASTER */

                g_mmio = (uint32_t)bar;
                mark_mmio_uc(g_mmio, 64u * 1024u);
                printk("[USB] xHCI bus=%u slot=%u fn=%u BAR0=0x%x\n",
                       (unsigned int)bus, (unsigned int)slot,
                       (unsigned int)fn, g_mmio);
                return 1;
            }
        }
    }
    return 0;
}

static int controller_init(void) {
    uint32_t cap = rd32(g_mmio + XHCI_CAPLENGTH);
    g_caplen = cap & 0xFFu;
    g_op = g_mmio + g_caplen;

    uint32_t hcs1 = rd32(g_mmio + XHCI_HCSPARAMS1);
    g_max_slots = hcs1 & 0xFFu;
    g_max_ports = (hcs1 >> 24) & 0xFFu;
    if (g_max_slots == 0) {
        printk("[USB] no slots (hcs1=0x%x)\n", hcs1);
        return 0;
    }
    if (g_max_slots > 64u) {
        g_max_slots = 64u;
    }

    printk("[USB] cap=0x%x slots=%u ports=%u\n",
           cap, g_max_slots, g_max_ports);
    if (g_max_ports == 0) {
        return 0;
    }

    uint32_t dboff = rd32(g_mmio + XHCI_DBOFF) & ~0x3u;
    uint32_t rtsoff = rd32(g_mmio + XHCI_RTSOFF) & ~0x1Fu;
    g_db = g_mmio + dboff;
    g_rt = g_mmio + rtsoff;

    /* 先停控制器：HCH（USBSTS.bit0）置位才表示已停止。
     * 固件可能已经停在 halted 状态，此时不需要再等。 */
    if ((rd32(g_op + OP_USBSTS) & 1u) == 0) {
        wr32(g_op + OP_USBCMD, rd32(g_op + OP_USBCMD) & ~1u);
        if (!wait_bits_set(g_op + OP_USBSTS, 1u, USB_TIMEOUT_INIT)) {
            printk("[USB] halt timeout sts=0x%x\n", rd32(g_op + OP_USBSTS));
            return 0;
        }
    }

    /* HCRST */
    wr32(g_op + OP_USBCMD, rd32(g_op + OP_USBCMD) | 2u);
    if (!wait_bits_clear(g_op + OP_USBCMD, 2u, USB_TIMEOUT_INIT)) {
        printk("[USB] HCRST timeout cmd=0x%x\n", rd32(g_op + OP_USBCMD));
        return 0;
    }
    if (!wait_bits_clear(g_op + OP_USBSTS, 1u << 11, USB_TIMEOUT_INIT)) {
        printk("[USB] CNR timeout sts=0x%x\n", rd32(g_op + OP_USBSTS));
        return 0;
    }

    /* 4KB 页 */
    wr32(g_op + OP_PAGESIZE, 1u);

    /* 槽位数量 */
    wr32(g_op + OP_CONFIG, g_max_slots);

    /* 设备上下文基址数组 */
    u_memset(g_dcbaa, 0, sizeof(g_dcbaa));
    wr64(g_op + OP_DCBAAP, (uint64_t)(uintptr_t)g_dcbaa);

    /* 命令环 */
    ring_init(&g_cmd, g_cmd_mem);
    wr64(g_op + OP_CRCR, ((uint64_t)(uintptr_t)g_cmd_mem) | 1ull);

    /* 事件环 */
    u_memset(g_evt_mem, 0, sizeof(g_evt_mem));
    g_evt_deq = 0;
    g_evt_cycle = 1;
    u_memset(&g_erst, 0, sizeof(g_erst));
    g_erst.base = (uint64_t)(uintptr_t)g_evt_mem;
    g_erst.size = TRB_COUNT;
    wr32(g_rt + RT_ERSTSZ, 1u);
    wr64(g_rt + RT_ERSTBA, (uint64_t)(uintptr_t)&g_erst);
    wr64(g_rt + RT_ERDP, (uint64_t)(uintptr_t)g_evt_mem);

    /* 启动 */
    wr32(g_op + OP_USBCMD, rd32(g_op + OP_USBCMD) | 0x1u | 0x4u);
    if (!wait_bits_clear(g_op + OP_USBSTS, 1u, USB_TIMEOUT_INIT)) {
        printk("[USB] run timeout sts=0x%x\n", rd32(g_op + OP_USBSTS));
        return 0;
    }

    /* 给所有根端口上电 */
    for (uint32_t p = 1; p <= g_max_ports; p++) {
        uint32_t addr = g_op + OP_PORTS + (p - 1u) * 0x10u;
        uint32_t v = rd32(addr);
        v &= ~0x00FE0000u;   /* 清 RW1C，避免误清状态 */
        v |= (1u << 9);      /* PP */
        wr32(addr, v);
    }

    printk("[USB] ring=0x%x dcbaap=0x%x erdp=0x%x\n",
           (unsigned int)rd32(g_op + OP_CRCR),
           (unsigned int)rd32(g_op + OP_DCBAAP),
           (unsigned int)rd32(g_rt + RT_ERDP));

    return 1;
}

static int port_reset(uint32_t port) {
    uint32_t addr = g_op + OP_PORTS + (port - 1u) * 0x10u;
    uint32_t v = rd32(addr);
    uint32_t before = v;

    v &= ~0x00FE0000u;
    v |= (1u << 3);          /* PR */
    wr32(addr, v);

    uint64_t start = u_rdtsc();
    for (;;) {
        v = rd32(addr);
        if (v & (1u << 21)) {    /* PRC */
            v &= ~0x00FE0000u;
            v |= (1u << 21);     /* 写 1 清除 */
            wr32(addr, v);
            break;
        }
        if (((v & (1u << 3)) == 0) && (v & 2u)) {   /* PR 已清且 PED */
            break;
        }
        if (u_rdtsc() - start > USB_TIMEOUT_INIT) {
            printk("[USB] port%u reset timeout before=0x%x now=0x%x\n",
                   port, before, v);
            return 0;
        }
    }

    /* 复位完成后等端口进入 Enabled（PED） */
    start = u_rdtsc();
    for (;;) {
        v = rd32(addr);
        if ((v & 1u) && (v & 2u)) {   /* CCS + PED */
            printk("[USB] port%u reset ok portsc=0x%x\n", port, v);
            return 1;
        }
        if (u_rdtsc() - start > USB_TIMEOUT_INIT) {
            printk("[USB] port%u not enabled portsc=0x%x\n", port, v);
            return 0;
        }
    }
}

/* ============================================================
 * 枚举
 * ============================================================ */

static int scsi_probe(void);

static int enable_slot(uint32_t *slot_out) {
    ring_push(&g_cmd, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    flush_ring(&g_cmd);
    doorbell(0, 0);

    uint32_t dw2 = 0, dw3 = 0;
    if (!event_wait(TRB_EVT_CMD, 0xFFFFFFFFu, &dw2, &dw3, USB_TIMEOUT_INIT)) {
        struct XhciTrb *e =
            &((struct XhciTrb *)(void *)g_evt_mem)[g_evt_deq];
        printk("[USB] enable slot timeout evt=%x %x %x %x deq=%u cyc=%u sts=%x\n",
               e->dw0, e->dw1, e->dw2, e->dw3,
               g_evt_deq, g_evt_cycle,
               (unsigned int)rd32(g_op + OP_USBSTS));
        return 0;
    }
    if (((dw2 >> 24) & 0xFFu) != 1u) {
        printk("[USB] enable slot cc=%u\n", (unsigned int)((dw2 >> 24) & 0xFFu));
        return 0;
    }
    uint32_t slot = (dw3 >> 24) & 0xFFu;
    if (slot == 0 || slot > g_max_slots) {
        return 0;
    }
    *slot_out = slot;
    return 1;
}

static int address_device(uint32_t slot, uint32_t port, uint32_t speed) {
    u_memset(g_dev_ctx, 0, sizeof(g_dev_ctx));
    ((uint64_t *)(void *)g_dcbaa)[slot] = (uint64_t)(uintptr_t)g_dev_ctx;

    u_memset(g_in_ctx, 0, sizeof(g_in_ctx));
    uint32_t *ictx = (uint32_t *)(void *)g_in_ctx;

    ictx[1] = (1u << 0) | (1u << 1);    /* slot + EP0 */

    uint32_t *slotc = ictx + 8;          /* offset 32 */
    slotc[0] = (speed << 20) | (1u << 27);
    slotc[1] = (port << 16);

    uint32_t mps = (speed == 4u) ? 512u : (speed == 3u ? 64u : 8u);
    uint32_t *ep0 = ictx + 16;           /* offset 64 */
    ep0[1] = (mps << 16) | (3u << 1) | (4u << 3);
    ep0[2] = ((uint32_t)(uintptr_t)g_ep0_mem) | 1u;
    ep0[3] = (uint32_t)(((uint64_t)(uintptr_t)g_ep0_mem) >> 32);
    ep0[4] = 8u << 16;

    ring_init(&g_ep0, g_ep0_mem);

    ring_push(&g_cmd,
              (uint32_t)(uintptr_t)g_in_ctx,
              (uint32_t)(((uint64_t)(uintptr_t)g_in_ctx) >> 32),
              0,
              TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24));
    cache_flush_range(g_in_ctx, sizeof(g_in_ctx));
    cache_flush_range(g_dcbaa, sizeof(g_dcbaa));
    flush_ring(&g_cmd);
    doorbell(0, 0);

    uint32_t dw2 = 0, dw3 = 0;
    if (!event_wait(TRB_EVT_CMD, 0xFFFFFFFFu, &dw2, &dw3, USB_TIMEOUT_INIT)) {
        return 0;
    }
    return ((dw2 >> 24) & 0xFFu) == 1u;
}

static int configure_endpoints(uint32_t slot, uint32_t port, uint32_t speed) {
    u_memset(g_in_ctx, 0, sizeof(g_in_ctx));
    uint32_t *ictx = (uint32_t *)(void *)g_in_ctx;

    ictx[1] = (1u << 0) |
              (1u << g_dci_in) |
              (1u << g_dci_out);

    uint32_t entries = (g_dci_in > g_dci_out) ? g_dci_in : g_dci_out;
    uint32_t *slotc = ictx + 8;
    slotc[0] = (speed << 20) | (entries << 27);
    slotc[1] = (port << 16);

    uint32_t *ep_out = ictx + (g_dci_out + 1u) * 8u;
    ep_out[0] = 0;
    ep_out[1] = (g_mps_out << 16) | (3u << 1) | (2u << 3);
    ep_out[2] = ((uint32_t)(uintptr_t)g_bout_mem) | 1u;
    ep_out[3] = (uint32_t)(((uint64_t)(uintptr_t)g_bout_mem) >> 32);
    ep_out[4] = g_mps_out << 16;

    uint32_t *ep_in = ictx + (g_dci_in + 1u) * 8u;
    ep_in[0] = 0;
    ep_in[1] = (g_mps_in << 16) | (3u << 1) | (6u << 3);
    ep_in[2] = ((uint32_t)(uintptr_t)g_bin_mem) | 1u;
    ep_in[3] = (uint32_t)(((uint64_t)(uintptr_t)g_bin_mem) >> 32);
    ep_in[4] = g_mps_in << 16;

    ring_init(&g_bin, g_bin_mem);
    ring_init(&g_bout, g_bout_mem);

    ring_push(&g_cmd,
              (uint32_t)(uintptr_t)g_in_ctx,
              (uint32_t)(((uint64_t)(uintptr_t)g_in_ctx) >> 32),
              0,
              TRB_TYPE(TRB_CONFIGURE_EP) | (slot << 24));
    cache_flush_range(g_in_ctx, sizeof(g_in_ctx));
    flush_ring(&g_cmd);
    doorbell(0, 0);

    uint32_t dw2 = 0, dw3 = 0;
    if (!event_wait(TRB_EVT_CMD, 0xFFFFFFFFu, &dw2, &dw3, USB_TIMEOUT_INIT)) {
        return 0;
    }
    uint32_t cc = (dw2 >> 24) & 0xFFu;
    if (cc != 1u) {
        printk("[USB] configure ep cc=%u\n", cc);
        return 0;
    }

    /* 读回设备上下文，确认端点状态（0=Disabled, 1=Running） */
    cache_flush_range(g_dev_ctx, sizeof(g_dev_ctx));
    uint32_t *dc = (uint32_t *)(void *)g_dev_ctx;
    printk("[USB] slot=%u configured, ep IN=%u OUT=%u state=%u/%u\n",
           g_slot, g_dci_in, g_dci_out,
           dc[g_dci_in * 8u] & 7u, dc[g_dci_out * 8u] & 7u);
    return 1;
}

static int parse_config(uint32_t *mps_in, uint32_t *mps_out,
                        uint32_t *dci_in, uint32_t *dci_out) {
    uint32_t total = 0;
    uint32_t got = 0;

    if (!control_transfer(0x80, 6, (DESC_CONFIG << 8), 0,
                          g_desc, 9, 1)) {
        return 0;
    }
    total = (uint32_t)g_desc[2] | ((uint32_t)g_desc[3] << 8);
    if (total < 9u || total > sizeof(g_desc)) {
        return 0;
    }
    if (!control_transfer(0x80, 6, (DESC_CONFIG << 8), 0,
                          g_desc, (uint16_t)total, 1)) {
        return 0;
    }
    got = total;

    int msc = 0;
    uint32_t in_addr = 0, out_addr = 0;
    uint32_t in_mps = 0, out_mps = 0;

    uint32_t i = 0;
    while (i + 2u <= got) {
        uint32_t len = g_desc[i];
        uint32_t type = g_desc[i + 1];
        if (len < 2u || i + len > got) {
            break;
        }
        if (type == DESC_INTERFACE && len >= 9u) {
            uint8_t cls = g_desc[i + 5];
            uint8_t sub = g_desc[i + 6];
            uint8_t proto = g_desc[i + 7];
            msc = (cls == 8u && sub == 6u && proto == 0x50u);
        } else if (type == DESC_ENDPOINT && len >= 7u && msc) {
            uint8_t addr = g_desc[i + 2];
            uint8_t attr = g_desc[i + 3];
            uint32_t mps = (uint32_t)g_desc[i + 4] |
                           ((uint32_t)g_desc[i + 5] << 8);
            mps &= 0x7FFu;

            if ((attr & 3u) == 2u) {
                if (addr & 0x80u) {
                    in_addr = addr;
                    in_mps = mps;
                } else {
                    out_addr = addr;
                    out_mps = mps;
                }
            }
        }
        i += len;
    }

    if (!msc || in_addr == 0 || out_addr == 0) {
        return 0;
    }

    printk("[USB] ep addr in=0x%x out=0x%x\n", in_addr, out_addr);

    *dci_in = ((in_addr & 0x0Fu) << 1) | 1u;
    *dci_out = (out_addr & 0x0Fu) << 1;
    *mps_in = in_mps ? in_mps : 512u;
    *mps_out = out_mps ? out_mps : 512u;
    return 1;
}

/* ============================================================
 * SCSI
 * ============================================================ */

static int scsi_probe(void) {
    uint8_t cb[16];

    /* INQUIRY */
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x12;
    cb[4] = 36;

    uint8_t *inq = g_xfer_buf;
    if (!bot_command(cb, 6, inq, 36, 1)) {
        printk("[USB] INQUIRY failed\n");
        return 0;
    }

    /* vendor[8] + product[16] */
    uint32_t n = 0;
    for (uint32_t i = 8; i < 16 && n < sizeof(g_name) - 1u; i++) {
        g_name[n++] = (char)inq[i];
    }
    g_name[n++] = ' ';
    for (uint32_t i = 16; i < 32 && n < sizeof(g_name) - 1u; i++) {
        g_name[n++] = (char)inq[i];
    }
    while (n > 0 && (g_name[n - 1] == ' ' || g_name[n - 1] == 0)) {
        n--;
    }
    g_name[n] = 0;

    /* TEST UNIT READY */
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x00;
    if (!bot_command(cb, 6, 0, 0, 0)) {
        return 0;
    }

    /* READ CAPACITY(10) */
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x25;

    uint8_t *cap = g_xfer_buf;
    if (bot_command(cb, 10, cap, 8, 1)) {
        uint32_t last = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                        ((uint32_t)cap[2] << 8) | (uint32_t)cap[3];
        uint32_t bs = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                      ((uint32_t)cap[6] << 8) | (uint32_t)cap[7];
        if (bs != 512u) {
            return 0;
        }
        if (last != 0xFFFFFFFFu) {
            g_total_sectors = last + 1u;
            return 1;
        }
    }

    /* READ CAPACITY(16) */
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x9E;
    cb[1] = 0x10;

    uint8_t *cap16 = g_xfer_buf;
    if (!bot_command(cb, 16, cap16, 32, 1)) {
        return 0;
    }
    uint32_t last = ((uint32_t)cap16[4] << 24) | ((uint32_t)cap16[5] << 16) |
                    ((uint32_t)cap16[6] << 8) | (uint32_t)cap16[7];
    uint32_t bs = ((uint32_t)cap16[8] << 24) | ((uint32_t)cap16[9] << 16) |
                  ((uint32_t)cap16[10] << 8) | (uint32_t)cap16[11];
    if (bs != 512u) {
        return 0;
    }
    g_total_sectors = last + 1u;
    return 1;
}

/* ============================================================
 * 对外初始化
 * ============================================================ */

static int usb_enumerate(void) {
    uint32_t port = 0;
    for (uint32_t p = 1; p <= g_max_ports; p++) {
        uint32_t v;
        if (p == g_kbd_port) continue;      /* 已被 USB 键盘占用 */
        v = rd32(g_op + OP_PORTS + (p - 1u) * 0x10u);
        if (v & 1u) {
            port = p;
            break;
        }
    }
    if (port == 0) {
        printk("[USB] no device connected\n");
        return 0;
    }

    if (!port_reset(port)) {
        printk("[USB] port %u reset failed\n", port);
        return 0;
    }

    uint32_t portsc = rd32(g_op + OP_PORTS + (port - 1u) * 0x10u);
    uint32_t speed = (portsc >> 10) & 0xFu;
    printk("[USB] device on port %u speed=%u\n", port, speed);

    uint32_t slot = 0;
    if (!enable_slot(&slot)) {
        printk("[USB] enable slot failed\n");
        return 0;
    }
    g_slot = slot;

    if (!address_device(slot, port, speed)) {
        printk("[USB] address device failed\n");
        return 0;
    }

    /* GET_DESCRIPTOR(DEVICE) */
    if (!control_transfer(0x80, 6, (DESC_DEVICE << 8), 0,
                          g_desc, 18, 1)) {
        printk("[USB] get device descriptor failed\n");
        return 0;
    }
    if (g_desc[7] < 8u) {
        printk("[USB] bad bMaxPacketSize0=%u\n", (unsigned int)g_desc[7]);
        return 0;
    }
    printk("[USB] device %x:%x\n",
           (unsigned int)(g_desc[8] | ((uint32_t)g_desc[9] << 8)),
           (unsigned int)(g_desc[10] | ((uint32_t)g_desc[11] << 8)));

    /* SET_CONFIGURATION(1) */
    if (!control_transfer(0x00, 9, 1, 0, 0, 0, 0)) {
        printk("[USB] set configuration failed\n");
        return 0;
    }

    /* SET_INTERFACE(0, alt 0)：部分设备要求显式选择备用设置 */
    control_transfer(0x01, 11, 0, 0, 0, 0, 0);

    uint32_t mps_in = 0, mps_out = 0, dci_in = 0, dci_out = 0;
    if (!parse_config(&mps_in, &mps_out, &dci_in, &dci_out)) {
        printk("[USB] no BOT/SCSI interface\n");
        return 0;
    }

    g_mps_in = mps_in;
    g_mps_out = mps_out;
    g_dci_in = dci_in;
    g_dci_out = dci_out;
    printk("[USB] bulk IN dci=%u mps=%u / OUT dci=%u mps=%u\n",
           dci_in, mps_in, dci_out, mps_out);

    if (!configure_endpoints(slot, port, speed)) {
        printk("[USB] configure endpoints failed\n");
        return 0;
    }

    if (!scsi_probe()) {
        printk("[USB] SCSI probe failed\n");
        return 0;
    }

    /* 非破坏性写自测：读 LBA0，原样写回，再读比对。
     * 此时 g_state 还是 2（未对外可用），所以直接用 bot_command，
     * 不走 usb_msc_read/write 的状态检查。 */
    {
        uint8_t cb[10];

        u_memset(cb, 0, sizeof(cb));
        cb[0] = 0x28;
        cb[8] = 1;
        if (!bot_command(cb, 10, g_xfer_buf, 512, 1)) {
            printk("[USB] write self-test: read0 failed\n");
            return 0;
        }

        u_memset(cb, 0, sizeof(cb));
        cb[0] = 0x2A;
        cb[8] = 1;
        if (!bot_command(cb, 10, g_xfer_buf, 512, 0)) {
            printk("[USB] write self-test: write failed\n");
            return 0;
        }

        u_memset(cb, 0, sizeof(cb));
        cb[0] = 0x28;
        cb[8] = 1;
        if (!bot_command(cb, 10, g_verify_buf, 512, 1)) {
            printk("[USB] write self-test: read1 failed\n");
            return 0;
        }

        for (uint32_t i = 0; i < 512u; i++) {
            if (g_xfer_buf[i] != g_verify_buf[i]) {
                printk("[USB] write self-test mismatch at %u\n", i);
                return 0;
            }
        }
        printk("[USB] write self-test passed\n");
    }

    g_msc_port = port;
    return 1;
}

/* ============================================================
 * USB HID 启动键盘（2.4G 无线键盘接收器 / 有线 USB 键盘）
 * ------------------------------------------------------------
 * 复用已初始化的 xHCI 控制器：独立槽位、独立 EP0 环、一条中断 IN
 * 端点。只使用启动协议（8 字节报告：修饰键 + 保留 + 6 个键码），
 * 不解析 HID 报告描述符。
 * 边界：只处理直接挂在根端口上的设备，不做 Hub 级联。
 * ============================================================ */

static int kbd_control(uint8_t bm, uint8_t br, uint16_t wvalue,
                       uint16_t windex, void *buf, uint16_t len, int in) {
    uint32_t dw2 = 0, dw3 = 0;
    uint32_t cc;

    ring_push(&g_kbd_ep0,
              (uint32_t)bm | ((uint32_t)br << 8) | ((uint32_t)wvalue << 16),
              (uint32_t)windex | ((uint32_t)len << 16),
              8u, TRB_TYPE(TRB_SETUP) | TRB_IDT);

    if (len != 0) {
        uint32_t addr = (uint32_t)(uintptr_t)buf;
        ring_push(&g_kbd_ep0, addr,
                  (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32),
                  (uint32_t)len,
                  TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0u));
    }

    ring_push(&g_kbd_ep0, 0, 0, 0,
              TRB_TYPE(TRB_STATUS) | ((len != 0 && in) ? 0u : TRB_DIR_IN) |
              TRB_IOC);

    flush_ring(&g_kbd_ep0);
    if (len != 0) cache_flush_range(buf, len);

    doorbell(g_kbd_slot, 1);

    if (!event_wait(TRB_EVT_TRANSFER, 1u, &dw2, &dw3, USB_TIMEOUT_XFER)) {
        return 0;
    }
    if (len != 0) cache_flush_range(buf, len);

    cc = (dw2 >> 24) & 0xFFu;
    return (cc == 1u || cc == 13u);
}

static int kbd_address_device(uint32_t slot, uint32_t port, uint32_t speed) {
    uint32_t *ictx;
    uint32_t *slotc;
    uint32_t *ep0;
    uint32_t mps;
    uint32_t dw2 = 0, dw3 = 0;

    u_memset(g_kbd_dev_ctx, 0, sizeof(g_kbd_dev_ctx));
    ((uint64_t *)(void *)g_dcbaa)[slot] = (uint64_t)(uintptr_t)g_kbd_dev_ctx;

    u_memset(g_in_ctx, 0, sizeof(g_in_ctx));
    ictx = (uint32_t *)(void *)g_in_ctx;
    ictx[1] = (1u << 0) | (1u << 1);

    slotc = ictx + 8;
    slotc[0] = (speed << 20) | (1u << 27);
    slotc[1] = (port << 16);

    mps = (speed == 4u) ? 512u : (speed == 3u ? 64u : 8u);
    ep0 = ictx + 16;
    ep0[1] = (mps << 16) | (3u << 1) | (4u << 3);
    ep0[2] = ((uint32_t)(uintptr_t)g_kbd_ep0_mem) | 1u;
    ep0[3] = (uint32_t)(((uint64_t)(uintptr_t)g_kbd_ep0_mem) >> 32);
    ep0[4] = 8u << 16;

    ring_init(&g_kbd_ep0, g_kbd_ep0_mem);

    ring_push(&g_cmd,
              (uint32_t)(uintptr_t)g_in_ctx,
              (uint32_t)(((uint64_t)(uintptr_t)g_in_ctx) >> 32),
              0,
              TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24));
    cache_flush_range(g_in_ctx, sizeof(g_in_ctx));
    cache_flush_range(g_dcbaa, sizeof(g_dcbaa));
    flush_ring(&g_cmd);
    doorbell(0, 0);

    if (!event_wait(TRB_EVT_CMD, 0xFFFFFFFFu, &dw2, &dw3, USB_TIMEOUT_INIT)) {
        return 0;
    }
    return ((dw2 >> 24) & 0xFFu) == 1u;
}

/* 在配置描述符里找 HID 启动键盘接口及其中断 IN 端点 */
static int kbd_parse_config(uint8_t *iface_out, uint32_t *dci,
                            uint32_t *mps, uint32_t *interval) {
    uint32_t total;
    uint32_t i;
    int in_kbd = 0;
    uint8_t iface_no = 0;

    if (!kbd_control(0x80, 6, (DESC_CONFIG << 8), 0, g_desc, 9, 1)) return 0;
    total = (uint32_t)g_desc[2] | ((uint32_t)g_desc[3] << 8);
    if (total < 9u || total > sizeof(g_desc)) return 0;
    if (!kbd_control(0x80, 6, (DESC_CONFIG << 8), 0, g_desc,
                     (uint16_t)total, 1)) return 0;

    i = 0;
    while (i + 2u <= total) {
        uint32_t len = g_desc[i];
        uint32_t type = g_desc[i + 1];

        if (len < 2u || i + len > total) break;

        if (type == DESC_INTERFACE && len >= 9u) {
            uint8_t cls = g_desc[i + 5];
            uint8_t sub = g_desc[i + 6];
            uint8_t proto = g_desc[i + 7];

            iface_no = g_desc[i + 2];
            in_kbd = (cls == 3u && sub == 1u && (proto == 1u || proto == 0u));
        } else if (type == DESC_ENDPOINT && len >= 7u && in_kbd) {
            uint8_t addr = g_desc[i + 2];
            uint8_t attr = g_desc[i + 3];
            uint32_t ep_mps = (uint32_t)g_desc[i + 4] |
                              ((uint32_t)g_desc[i + 5] << 8);
            uint8_t ep_interval = g_desc[i + 6];

            if ((addr & 0x80u) && (attr & 3u) == 3u) {
                *iface_out = iface_no;
                *dci = ((uint32_t)(addr & 0x0Fu) << 1) | 1u;
                *mps = ep_mps & 0x7FFu;
                if (*mps == 0) *mps = 8u;
                *interval = ep_interval;
                return 1;
            }
        }
        i += len;
    }
    return 0;
}

static int kbd_configure(uint32_t slot, uint32_t port, uint32_t speed,
                         uint32_t dci, uint32_t mps, uint32_t interval) {
    uint32_t *ictx;
    uint32_t *slotc;
    uint32_t *ep;
    uint32_t dw2 = 0, dw3 = 0;

    u_memset(g_in_ctx, 0, sizeof(g_in_ctx));
    ictx = (uint32_t *)(void *)g_in_ctx;
    ictx[1] = (1u << 0) | (1u << dci);

    slotc = ictx + 8;
    slotc[0] = (speed << 20) | (dci << 27);
    slotc[1] = (port << 16);

    ep = ictx + (dci + 1u) * 8u;
    ep[0] = ((interval ? interval : 8u) & 0xFFu) << 16;
    ep[1] = (mps << 16) | (3u << 1) | (6u << 3);   /* 中断 IN */
    ep[2] = ((uint32_t)(uintptr_t)g_kbd_in_mem) | 1u;
    ep[3] = (uint32_t)(((uint64_t)(uintptr_t)g_kbd_in_mem) >> 32);
    ep[4] = mps << 16;

    ring_init(&g_kbd_in, g_kbd_in_mem);

    ring_push(&g_cmd,
              (uint32_t)(uintptr_t)g_in_ctx,
              (uint32_t)(((uint64_t)(uintptr_t)g_in_ctx) >> 32),
              0,
              TRB_TYPE(TRB_CONFIGURE_EP) | (slot << 24));
    cache_flush_range(g_in_ctx, sizeof(g_in_ctx));
    flush_ring(&g_cmd);
    doorbell(0, 0);

    if (!event_wait(TRB_EVT_CMD, 0xFFFFFFFFu, &dw2, &dw3, USB_TIMEOUT_INIT)) {
        return 0;
    }
    return ((dw2 >> 24) & 0xFFu) == 1u;
}

/* 预先挂一个 8 字节中断 IN 传输；收到报告后由轮询重新挂上 */
static void kbd_arm(void) {
    uint64_t a = (uint64_t)(uintptr_t)g_kbd_report;

    u_memset(g_kbd_report, 0, 8);   /* 短包时避免残留上一帧按键 */
    ring_push(&g_kbd_in, (uint32_t)a, (uint32_t)(a >> 32), 8u,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    flush_ring(&g_kbd_in);
    cache_flush_range(g_kbd_report, 8);
    doorbell(g_kbd_slot, g_kbd_dci);
    g_kbd_pending = 1;
}

static int kbd_try_port(uint32_t port) {
    uint32_t portsc;
    uint32_t speed;
    uint32_t slot = 0;
    uint8_t iface = 0;
    uint32_t dci = 0, mps = 8, interval = 8;

    if (!port_reset(port)) return 0;

    portsc = rd32(g_op + OP_PORTS + (port - 1u) * 0x10u);
    speed = (portsc >> 10) & 0xFu;

    if (!enable_slot(&slot)) return 0;
    g_kbd_slot = slot;

    if (!kbd_address_device(slot, port, speed)) return 0;

    if (!kbd_control(0x80, 6, (DESC_DEVICE << 8), 0, g_desc, 18, 1)) {
        return 0;
    }
    printk("[KBD] usb device %x:%x port=%u speed=%u\n",
           (unsigned int)(g_desc[8] | ((uint32_t)g_desc[9] << 8)),
           (unsigned int)(g_desc[10] | ((uint32_t)g_desc[11] << 8)),
           port, speed);

    if (!kbd_control(0x00, 9, 1, 0, 0, 0, 0)) return 0;

    if (!kbd_parse_config(&iface, &dci, &mps, &interval)) {
        printk("[KBD] port %u is not a boot keyboard\n", port);
        return 0;
    }

    g_kbd_dci = dci;
    g_kbd_mps = mps;
    g_kbd_interval = interval;

    if (!kbd_configure(slot, port, speed, dci, mps, interval)) return 0;

    /* 切到启动协议；SET_IDLE(0) 让设备只在状态变化时上报 */
    kbd_control(0x21, 0x0B, 0, iface, 0, 0, 0);
    kbd_control(0x21, 0x0A, 0, iface, 0, 0, 0);

    g_kbd_pending = 0;
    kbd_arm();

    g_kbd_port = port;
    printk("[KBD] boot keyboard ready: port=%u dci=%u mps=%u\n",
           port, dci, mps);
    return 1;
}

int usb_kbd_init(void) {
    uint32_t p;

    if (g_kbd_ready) return 1;

    if (!g_ctrl_ready) {
        if (!find_xhci()) return 0;
        if (!controller_init()) return 0;
        g_ctrl_ready = 1;
    }

    if (g_kbd_tried &&
        (u_rdtsc() - g_kbd_last_try) < 5000000000ull) {
        return 0;
    }
    g_kbd_tried = 1;
    g_kbd_last_try = u_rdtsc();

    for (p = 1; p <= g_max_ports; p++) {
        uint32_t v;

        if (p == g_msc_port) continue;              /* 已被 U 盘占用 */
        if (p < 32u && (g_kbd_fail_mask & (1u << p))) continue;  /* 非键盘口 */
        v = rd32(g_op + OP_PORTS + (p - 1u) * 0x10u);
        if (!(v & 1u)) continue;

        if (kbd_try_port(p)) {
            g_kbd_ready = 1;
            return 1;
        }
        if (p < 32u) g_kbd_fail_mask |= (1u << p);
    }
    return 0;
}

int usb_kbd_poll_report(uint8_t *out8) {
    int got = 0;
    int i;
    int k;

    if (!g_kbd_ready) {
        if (!usb_kbd_init()) return 0;
    }

    for (i = 0; i < 16; i++) {
        uint32_t type = 0, ep = 0, dw2 = 0, dw3 = 0;
        uint32_t epid, ep16, cc;

        if (!event_poll_one(&type, &ep, &dw2, &dw3)) break;
        if (type != TRB_EVT_TRANSFER) continue;

        epid = (dw3 >> 24) & 0xFFu;
        ep16 = (dw3 >> 16) & 0xFFu;
        if (epid != g_kbd_dci && ep16 != g_kbd_dci) continue;

        g_kbd_pending = 0;
        cache_flush_range(g_kbd_report, 8);
        cc = (dw2 >> 24) & 0xFFu;
        if ((cc == 1u || cc == 13u) && out8 && !got) {
            for (k = 0; k < 8; k++) out8[k] = g_kbd_report[k];
            got = 1;
        }
    }

    if (!g_kbd_pending) kbd_arm();
    return got;
}

int usb_kbd_present(void) {
    return g_kbd_ready;
}

int usb_msc_init(void) {
    if (g_state == 1) {
        return 1;
    }
    if (g_state == 2) {
        return 0;
    }
    g_state = 2;

    if (!g_ctrl_ready) {
        if (!find_xhci()) {
            printk("[USB] no xHCI controller\n");
            return 0;
        }
        if (!controller_init()) {
            printk("[USB] xHCI init failed\n");
            return 0;
        }
        g_ctrl_ready = 1;
    }
    if (!usb_enumerate()) {
        printk("[USB] no usable mass-storage device\n");
        return 0;
    }

    g_state = 1;
    printk("[USB] %s, %u sectors (%u MB)\n",
           g_name, g_total_sectors,
           (unsigned int)(g_total_sectors / 2048u));
    return 1;
}

int usb_msc_present(void) {
    return g_state == 1;
}

int usb_msc_read(uint32_t lba, uint8_t *buf) {
    if (g_state != 1) {
        return 0;
    }

    uint8_t cb[10];
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x28;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[7] = 0;
    cb[8] = 1;

    if (!bot_command(cb, 10, g_xfer_buf, 512, 1)) {
        return 0;
    }
    u_memcpy(buf, g_xfer_buf, 512);
    g_read_sectors++;
    return 1;
}

int usb_msc_write(uint32_t lba, const uint8_t *buf) {
    if (g_state != 1) {
        return 0;
    }

    u_memcpy(g_xfer_buf, buf, 512);

    uint8_t cb[10];
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x2A;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[7] = 0;
    cb[8] = 1;

    if (!bot_command(cb, 10, g_xfer_buf, 512, 0)) {
        return 0;
    }
    g_write_sectors++;
    return 1;
}

uint32_t usb_msc_total_sectors(void) {
    return g_total_sectors;
}

/* SYNCHRONIZE CACHE(10)：无数据阶段，只发 CDB 并等 CSW。
 * 机械 U 盘的写缓存同样需要显式刷盘，否则拔出时会丢已提交数据。 */
int usb_msc_flush(void) {
    if (g_state != 1) return 0;
    uint8_t cb[10];
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x35;
    return bot_command(cb, 10, 0, 0, 0);
}

void usb_msc_stats(uint32_t *reads, uint32_t *writes) {
    if (reads) *reads = g_read_sectors;
    if (writes) *writes = g_write_sectors;
}

const char *usb_msc_name(void) {
    return g_name;
}
