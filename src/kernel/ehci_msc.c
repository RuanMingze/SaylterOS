/* ============================================================
 * ehci_msc.c - EHCI(USB 2.0) + USB Mass Storage(BOT/SCSI)
 * ------------------------------------------------------------
 * 为什么单独实现 EHCI
 *   现代笔记本只有 xHCI，但老机器/工控机/部分扩展卡只有 EHCI
 *   （PCI class 0x0C, subclass 0x03, progIF 0x20）。EHCI 没有
 *   命令环和事件环，控制/批量传输都用同一套异步 QH/qTD 链表，
 *   因此在裸机下反而比 xHCI 更容易做对。
 *
 * 实现范围
 *   - EHCI 控制器复位、端口上电/复位、异步列表启动；
 *   - QH(48B) / qTD(32B) 构造，控制传输（SETUP/DATA/STATUS）
 *     与批量 IN/OUT；
 *   - SET_ADDRESS / GET_DESCRIPTOR / SET_CONFIGURATION /
 *     SET_INTERFACE 枚举；
 *   - BOT(CBW/CSW) + SCSI INQUIRY / TEST UNIT READY /
 *     READ CAPACITY(10|16) / READ(10) / WRITE(10)；
 *   - 非破坏性写自测（读 LBA0 原样写回再比对）。
 *
 * 边界
 *   - 只处理根端口直连设备，不做 Hub 级联；
 *   - 只支持 512 字节逻辑块；
 *   - 初始化失败时 present() 返回 0，blk.c 继续尝试 ATA。
 *
 * 并发
 *   所有传输串行化；QH/qTD 在提交前 clflush，完成后失效再读。
 * ============================================================ */

#include <stdint.h>
#include "ehci_msc.h"
#include "io.h"
#include "tty.h"

/* ============================================================
 * 常量
 * ============================================================ */

#define EHCI_TIMEOUT_INIT  2500000000ull
#define EHCI_TIMEOUT_XFER   800000000ull

#define EHCI_CAPLENGTH     0x00u
#define EHCI_HCSPARAMS     0x04u
#define EHCI_HCCPARAMS     0x08u

/* 操作寄存器相对 op base（BAR + CAPLENGTH）的偏移 */
#define EHCI_USBCMD        0x00u
#define EHCI_USBSTS        0x04u
#define EHCI_USBINTR       0x08u
#define EHCI_FRINDEX       0x0Cu
#define EHCI_CTRLDSSEG     0x10u
#define EHCI_PERIODICLIST  0x14u
#define EHCI_ASYNCLIST     0x18u
#define EHCI_CONFIGFLAG    0x40u
#define EHCI_PORTSC(p)     (0x44u + ((p) - 1u) * 4u)

#define EHCI_CMD_RS        (1u << 0)
#define EHCI_CMD_HCRESET   (1u << 1)
#define EHCI_CMD_ASE       (1u << 5)
#define EHCI_STS_HCH       (1u << 12)

#define EHCI_PORT_CCS      (1u << 0)
#define EHCI_PORT_CSC      (1u << 1)
#define EHCI_PORT_PE       (1u << 2)
#define EHCI_PORT_PEC      (1u << 3)
#define EHCI_PORT_OCC      (1u << 5)
#define EHCI_PORT_PR       (1u << 8)
#define EHCI_PORT_PP       (1u << 12)
#define EHCI_PORT_PO       (1u << 13)
#define EHCI_PORT_SPEED(v) (((v) >> 26) & 3u)

#define QH_ONLY            0u
#define QH_HEAD            0u
#define QH_EP0             0u
#define QH_BIN             0u
#define QH_BOUT            0u

#define QTD_SETUP          0u
#define QTD_DATA           1u
#define QTD_STATUS         2u
#define QTD_BIN            3u
#define QTD_BOUT           4u

#define QTD_NEXT_END       0xFFFFFFFFu

#define PID_OUT            0u
#define PID_IN             1u
#define PID_SETUP          2u

/* ============================================================
 * 数据结构
 * ============================================================ */

struct __attribute__((packed)) EhciQh {
    uint32_t horiz;       /* +0  horizontal link */
    uint32_t ep_char;     /* +4  endpoint characteristics */
    uint32_t ep_cap;      /* +8  endpoint capabilities */
    uint32_t cur_qtd;     /* +12 current qTD (hardware) */
    uint32_t next_qtd;    /* +16 overlay: next qTD */
    uint32_t alt_qtd;     /* +20 overlay: alternate qTD */
    uint32_t token;       /* +24 overlay: qTD token */
    uint32_t buf[5];      /* +28 buffer pointers */
};

struct __attribute__((packed)) EhciQtd {
    uint32_t next;        /* +0  next qTD */
    uint32_t alt;         /* +4  alternate qTD */
    uint32_t token;       /* +8  token */
    uint32_t buf[5];      /* +12 buffer pointers */
};

static uint8_t g_qh_mem[512]        __attribute__((aligned(4096)));
static uint8_t g_qtd_mem[512]       __attribute__((aligned(4096)));
static uint8_t g_xfer_buf[4096]     __attribute__((aligned(4096)));
static uint8_t g_verify_buf[4096]   __attribute__((aligned(4096)));
static uint8_t g_desc[4096]         __attribute__((aligned(4096)));
static uint8_t g_setup[8]           __attribute__((aligned(64)));
static uint8_t g_cbw[64]            __attribute__((aligned(64)));
static uint8_t g_csw[64]            __attribute__((aligned(64)));

/* ============================================================
 * 状态
 * ============================================================ */

static uint32_t g_base;
static uint32_t g_op;
static uint32_t g_ports;
static uint32_t g_port;
static uint32_t g_speed;        /* 0=full 1=low 2=high */
static uint32_t g_addr;
static uint32_t g_ep0_mps;
static uint32_t g_bin_num;
static uint32_t g_bout_num;
static uint32_t g_bin_mps;
static uint32_t g_bout_mps;
static uint8_t  g_in_dt;
static uint8_t  g_out_dt;
static uint32_t g_total_sectors;
static uint32_t g_reads;
static uint32_t g_writes;
static uint32_t g_tag;
static int      g_state;
static char     g_name[32];

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

static uint32_t r32u(const void *p) {
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void w32u(void *p, uint32_t v) {
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

static uint32_t pci_bar32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t lo = pci_read32(bus, slot, func, off);
    if (lo & 1u) return 0;
    return lo & ~0x0Fu;
}

/* ============================================================
 * QH / qTD
 * ============================================================ */

static struct EhciQh *qh_ptr(uint32_t idx) {
    return (struct EhciQh *)(void *)(g_qh_mem + idx * 64u);
}

static struct EhciQtd *qtd_ptr(uint32_t idx) {
    return (struct EhciQtd *)(void *)(g_qtd_mem + idx * 32u);
}

static void ehci_qh_configure(uint32_t addr, uint32_t ep_num, uint32_t speed,
                              uint32_t mps, uint32_t is_control,
                              uint32_t port) {
    struct EhciQh *q = qh_ptr(QH_ONLY);
    u_memset(q, 0, sizeof(*q));

    /* 单 QH 异步列表：H=1，horiz 终止。控制器每个微帧从
     * ASYNCLISTADDR 重新扫描，因此不需要多 QH 链接。 */
    q->horiz = 1u;

    uint32_t ec = ((mps & 0x7FFu) << 16) |
                  ((speed & 3u) << 12) |
                  ((ep_num & 0x0Fu) << 8) |
                  (addr & 0x7Fu) |
                  (1u << 15);
    if (is_control) ec |= (1u << 27);
    q->ep_char = ec;

    q->ep_cap = (port & 0x7Fu) << 23;
    q->cur_qtd = 0;
    q->next_qtd = 1;
    q->alt_qtd = 1;

    cache_flush_range(q, sizeof(*q));
}

static uint32_t g_cur_addr = 0xFFFFFFFFu;
static uint32_t g_cur_ep = 0xFFFFFFFFu;
static uint32_t g_cur_control = 0xFFFFFFFFu;

static void ehci_select_ep(uint32_t addr, uint32_t ep_num, uint32_t mps,
                           uint32_t is_control) {
    if (addr == g_cur_addr && ep_num == g_cur_ep &&
        is_control == g_cur_control) {
        return;
    }
    ehci_qh_configure(addr, ep_num, g_speed, mps, is_control, g_port);
    g_cur_addr = addr;
    g_cur_ep = ep_num;
    g_cur_control = is_control;
}

static void ehci_invalidate_ep_cache(void) {
    g_cur_addr = 0xFFFFFFFFu;
    g_cur_ep = 0xFFFFFFFFu;
    g_cur_control = 0xFFFFFFFFu;
}

static void ehci_build_qtd(uint32_t idx, uint32_t next_idx, uint32_t buf,
                           uint32_t len, uint32_t pid, uint32_t dt,
                           uint32_t ioc) {
    struct EhciQtd *q = qtd_ptr(idx);
    u_memset(q, 0, sizeof(*q));

    q->next = (next_idx == QTD_NEXT_END)
                  ? 1u
                  : (uint32_t)(uintptr_t)qtd_ptr(next_idx);
    q->alt = 1;

    uint32_t tok = ((len & 0x7FFFu) << 16) |
                   (3u << 10) |
                   ((pid & 3u) << 8) |
                   0x80u;                 /* Active */
    if (ioc) tok |= (1u << 15);
    if (dt) tok |= (1u << 31);
    q->token = tok;
    q->buf[0] = buf;
}

static int ehci_run_chain(uint32_t first_qtd, uint32_t last_qtd,
                          uint64_t timeout) {
    struct EhciQh *q = qh_ptr(QH_ONLY);

    cache_flush_range(qtd_ptr(first_qtd),
                      32u * (last_qtd - first_qtd + 1u));

    q->cur_qtd = 0;
    q->alt_qtd = 1;
    q->token = 0;
    for (uint32_t i = 0; i < 5u; i++) q->buf[i] = 0;
    q->next_qtd = (uint32_t)(uintptr_t)qtd_ptr(first_qtd);
    cache_flush_range(q, sizeof(*q));

    uint64_t start = u_rdtsc();
    for (;;) {
        struct EhciQtd *last = qtd_ptr(last_qtd);
        cache_flush_range(last, sizeof(*last));
        uint32_t tok = last->token;

        if ((tok & 0x80u) == 0) {
            uint32_t status = tok & 0x7Fu;
            q->next_qtd = 1;
            cache_flush_range(q, sizeof(*q));
            return status == 0u;
        }

        if (u_rdtsc() - start > timeout) {
            q->next_qtd = 1;
            cache_flush_range(q, sizeof(*q));
            return 0;
        }
    }
}

static int wait_bits_clear(uint32_t addr, uint32_t mask, uint64_t timeout) {
    uint64_t start = u_rdtsc();
    while (rd32(addr) & mask) {
        if (u_rdtsc() - start > timeout) return 0;
    }
    return 1;
}

static int wait_bits_set(uint32_t addr, uint32_t mask, uint64_t timeout) {
    uint64_t start = u_rdtsc();
    while ((rd32(addr) & mask) != mask) {
        if (u_rdtsc() - start > timeout) return 0;
    }
    return 1;
}

/* ============================================================
 * 控制传输
 * ============================================================ */

static int ehci_control(uint8_t bm, uint8_t br, uint16_t wvalue,
                        uint16_t windex, void *buf, uint16_t len, int in) {
    g_setup[0] = bm;
    g_setup[1] = br;
    g_setup[2] = (uint8_t)wvalue;
    g_setup[3] = (uint8_t)(wvalue >> 8);
    g_setup[4] = (uint8_t)windex;
    g_setup[5] = (uint8_t)(windex >> 8);
    g_setup[6] = (uint8_t)len;
    g_setup[7] = (uint8_t)(len >> 8);

    uint32_t last;

    if (len != 0) {
        ehci_build_qtd(QTD_SETUP, QTD_DATA,
                       (uint32_t)(uintptr_t)g_setup, 8, PID_SETUP, 0, 0);
        ehci_build_qtd(QTD_DATA, QTD_STATUS,
                       (uint32_t)(uintptr_t)buf, len, in ? PID_IN : PID_OUT, 1, 0);
        ehci_build_qtd(QTD_STATUS, QTD_NEXT_END, 0, 0, PID_IN, 1, 1);
        last = QTD_STATUS;
        cache_flush_range(g_setup, sizeof(g_setup));
        if (!in) cache_flush_range(buf, len);
    } else {
        ehci_build_qtd(QTD_SETUP, QTD_STATUS,
                       (uint32_t)(uintptr_t)g_setup, 8, PID_SETUP, 0, 0);
        ehci_build_qtd(QTD_STATUS, QTD_NEXT_END, 0, 0, PID_IN, 1, 1);
        last = QTD_STATUS;
        cache_flush_range(g_setup, sizeof(g_setup));
    }

    ehci_select_ep(g_addr, 0, g_ep0_mps, 1);

    if (!ehci_run_chain(QTD_SETUP, last, EHCI_TIMEOUT_XFER)) {
        return 0;
    }

    if (len != 0 && in) {
        cache_flush_range(buf, len);
    }
    return 1;
}

/* ============================================================
 * 批量传输
 * ============================================================ */

static int ehci_bulk(int in, void *buf, uint32_t len) {
    uint32_t qh_idx = in ? QH_BIN : QH_BOUT;
    uint32_t qtd_idx = in ? QTD_BIN : QTD_BOUT;
    uint8_t *dt = in ? &g_in_dt : &g_out_dt;

    ehci_build_qtd(qtd_idx, QTD_NEXT_END,
                   (uint32_t)(uintptr_t)buf, len,
                   in ? PID_IN : PID_OUT, *dt, 1);

    if (!in) cache_flush_range(buf, len);

    ehci_select_ep(g_addr, in ? g_bin_num : g_bout_num,
                   in ? g_bin_mps : g_bout_mps, 0);

    if (!ehci_run_chain(qtd_idx, qtd_idx, EHCI_TIMEOUT_XFER)) {
        printk("[EHCI] bulk timeout %s len=%u\n", in ? "IN" : "OUT", len);
        return 0;
    }

    if (in) cache_flush_range(buf, len);
    *dt ^= 1u;
    return 1;
}

/* ============================================================
 * BOT / SCSI
 * ============================================================ */

static int bot_command(const uint8_t *cb, uint32_t cb_len,
                       void *data, uint32_t data_len, int data_in) {
    u_memset(g_cbw, 0, 31);
    w32u(g_cbw + 0, 0x43425355u);
    g_tag++;
    w32u(g_cbw + 4, g_tag);
    w32u(g_cbw + 8, data_len);
    g_cbw[12] = (uint8_t)(data_in ? 0x80 : 0x00);
    g_cbw[13] = 0;
    g_cbw[14] = (uint8_t)cb_len;
    for (uint32_t i = 0; i < cb_len && i < 16u; i++) {
        g_cbw[15 + i] = cb[i];
    }
    cache_flush_range(g_cbw, 31);

    if (!ehci_bulk(0, g_cbw, 31)) {
        printk("[EHCI] BOT CBW failed\n");
        return 0;
    }

    if (data_len != 0) {
        if (!ehci_bulk(data_in, data, data_len)) {
            printk("[EHCI] BOT data failed (in=%u)\n", (unsigned int)data_in);
            return 0;
        }
    }

    if (!ehci_bulk(1, g_csw, 13)) {
        printk("[EHCI] BOT CSW failed\n");
        return 0;
    }

    if (r32u(g_csw + 0) != 0x53425355u) {
        printk("[EHCI] bad CSW sig=0x%x\n", r32u(g_csw + 0));
        return 0;
    }
    if (r32u(g_csw + 4) != g_tag) {
        printk("[EHCI] CSW tag mismatch\n");
        return 0;
    }
    if (g_csw[12] != 0) {
        printk("[EHCI] CSW status=%u\n", (unsigned int)g_csw[12]);
        return 0;
    }
    return 1;
}

static int scsi_probe(void) {
    uint8_t cb[16];

    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x12;
    cb[4] = 36;
    if (!bot_command(cb, 6, g_xfer_buf, 36, 1)) {
        printk("[EHCI] INQUIRY failed\n");
        return 0;
    }

    uint32_t n = 0;
    for (uint32_t i = 8; i < 16 && n < sizeof(g_name) - 1u; i++) {
        g_name[n++] = (char)g_xfer_buf[i];
    }
    g_name[n++] = ' ';
    for (uint32_t i = 16; i < 32 && n < sizeof(g_name) - 1u; i++) {
        g_name[n++] = (char)g_xfer_buf[i];
    }
    while (n > 0 && (g_name[n - 1] == ' ' || g_name[n - 1] == 0)) n--;
    g_name[n] = 0;

    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x00;
    if (!bot_command(cb, 6, 0, 0, 0)) {
        printk("[EHCI] TEST UNIT READY failed\n");
        return 0;
    }

    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x25;
    if (bot_command(cb, 10, g_xfer_buf, 8, 1)) {
        uint32_t last = ((uint32_t)g_xfer_buf[0] << 24) |
                        ((uint32_t)g_xfer_buf[1] << 16) |
                        ((uint32_t)g_xfer_buf[2] << 8) |
                        (uint32_t)g_xfer_buf[3];
        uint32_t bs = ((uint32_t)g_xfer_buf[4] << 24) |
                      ((uint32_t)g_xfer_buf[5] << 16) |
                      ((uint32_t)g_xfer_buf[6] << 8) |
                      (uint32_t)g_xfer_buf[7];
        if (bs != 512u) {
            printk("[EHCI] unsupported block size %u\n", bs);
            return 0;
        }
        if (last != 0xFFFFFFFFu) {
            g_total_sectors = last + 1u;
            return 1;
        }
    }

    /* READ CAPACITY(16) 兜底 */
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x9E;
    cb[1] = 0x10;
    if (!bot_command(cb, 16, g_xfer_buf, 32, 1)) {
        return 0;
    }
    uint32_t last = ((uint32_t)g_xfer_buf[4] << 24) |
                    ((uint32_t)g_xfer_buf[5] << 16) |
                    ((uint32_t)g_xfer_buf[6] << 8) |
                    (uint32_t)g_xfer_buf[7];
    uint32_t bs = ((uint32_t)g_xfer_buf[8] << 24) |
                  ((uint32_t)g_xfer_buf[9] << 16) |
                  ((uint32_t)g_xfer_buf[10] << 8) |
                  (uint32_t)g_xfer_buf[11];
    if (bs != 512u) {
        return 0;
    }
    g_total_sectors = last + 1u;
    return 1;
}

/* ============================================================
 * 控制器
 * ============================================================ */

static int find_ehci(void) {
    for (uint8_t bus = 0; bus < 4; bus++) {
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
                if (cls != 0x0Cu || sub != 0x03u || prog != 0x20u) continue;

                uint32_t lo = pci_read32(bus, slot, fn, 0x10);
                uint32_t hi = pci_read32(bus, slot, fn, 0x14);
                uint32_t btype = (lo >> 1) & 3u;
                uint32_t bar = lo & ~0x0Fu;
                printk("[EHCI] bar_lo=0x%x bar_hi=0x%x type=%u\n",
                       lo, hi, btype);
                if (lo & 1u) return 0;
                if (btype == 2u && hi != 0) {
                    printk("[EHCI] BAR above 4GB, unsupported\n");
                    return 0;
                }
                if (bar == 0) return 0;

                uint16_t cmd = pci_read16(bus, slot, fn, 0x04);
                pci_write16(bus, slot, fn, 0x04, (uint16_t)(cmd | 0x0006u));

                g_base = bar;
                mark_mmio_uc(g_base, 64u * 1024u);
                printk("[EHCI] bus=%u slot=%u fn=%u BAR0=0x%x\n",
                       (unsigned int)bus, (unsigned int)slot,
                       (unsigned int)fn, g_base);
                return 1;
            }
        }
    }
    return 0;
}

static int controller_init(void) {
    uint32_t cap = rd32(g_base + EHCI_CAPLENGTH);
    g_op = g_base + (cap & 0xFFu);

    uint32_t hcs = rd32(g_base + EHCI_HCSPARAMS);
    uint32_t hcc = rd32(g_base + EHCI_HCCPARAMS);
    g_ports = hcs & 0xFu;
    printk("[EHCI] caplen=0x%x ports=%u hcs=0x%x hcc=0x%x cmd=0x%x sts=0x%x\n",
           cap & 0xFFu, g_ports, hcs, hcc,
           rd32(g_op + EHCI_USBCMD), rd32(g_op + EHCI_USBSTS));
    if (g_ports == 0) return 0;

    wr32(g_op + EHCI_USBINTR, 0);

    /* 无条件先停控制器：清 RS 后 HCH 必须为 1 */
    wr32(g_op + EHCI_USBCMD, rd32(g_op + EHCI_USBCMD) & ~EHCI_CMD_RS);
    if (!wait_bits_set(g_op + EHCI_USBSTS, EHCI_STS_HCH,
                       EHCI_TIMEOUT_INIT)) {
        printk("[EHCI] halt timeout sts=0x%x\n", rd32(g_op + EHCI_USBSTS));
        return 0;
    }

    /* HCRESET：规范要求自清除；QEMU 的 usb-ehci 在部分状态下
     * 不会清这一位，因此超时后不直接失败，先把位清掉继续配置。 */
    wr32(g_op + EHCI_USBCMD, EHCI_CMD_HCRESET);
    delay_loops(2000000u);
    if (!wait_bits_clear(g_op + EHCI_USBCMD, EHCI_CMD_HCRESET,
                         200000000ull)) {
        printk("[EHCI] HCRESET not self-cleared (cmd=0x%x), forcing\n",
               rd32(g_op + EHCI_USBCMD));
        wr32(g_op + EHCI_USBCMD, 0);
    }
    if (!wait_bits_set(g_op + EHCI_USBSTS, EHCI_STS_HCH, EHCI_TIMEOUT_INIT)) {
        printk("[EHCI] post-reset halt timeout sts=0x%x\n",
               rd32(g_op + EHCI_USBSTS));
        return 0;
    }

    wr32(g_op + EHCI_CONFIGFLAG, 1u);
    wr32(g_op + EHCI_CTRLDSSEG, 0u);
    wr32(g_op + EHCI_PERIODICLIST, 0u);
    wr32(g_op + EHCI_FRINDEX, 0u);

    g_cur_addr = 0xFFFFFFFFu;
    g_cur_ep = 0xFFFFFFFFu;
    g_cur_control = 0xFFFFFFFFu;

    ehci_qh_configure(0, 0, 2, 64, 1, 1);
    ehci_invalidate_ep_cache();
    wr32(g_op + EHCI_ASYNCLIST, (uint32_t)(uintptr_t)qh_ptr(QH_ONLY));

    for (uint32_t p = 1; p <= g_ports; p++) {
        uint32_t reg = g_op + EHCI_PORTSC(p);
        uint32_t v = rd32(reg);
        v &= ~(1u << 13);          /* PO：归 EHCI 所有 */
        v &= ~(EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC);
        v |= EHCI_PORT_PP;
        wr32(reg, v);
    }
    delay_loops(2000000u);

    /* 保留 USBCMD 高 16 位（QEMU 复位后带 0x80000 保留位），
     * 只置 RS/ASE，避免被控制器当成非法状态重置。 */
    uint32_t run_cmd = rd32(g_op + EHCI_USBCMD);
    wr32(g_op + EHCI_USBCMD,
         (run_cmd & 0xFFFF0000u) | EHCI_CMD_RS | EHCI_CMD_ASE);
    if (!wait_bits_clear(g_op + EHCI_USBSTS, EHCI_STS_HCH,
                         EHCI_TIMEOUT_INIT)) {
        printk("[EHCI] run timeout cmd=0x%x sts=0x%x\n",
               rd32(g_op + EHCI_USBCMD), rd32(g_op + EHCI_USBSTS));
        return 0;
    }

    printk("[EHCI] async list running\n");
    return 1;
}

static int port_reset(uint32_t port) {
    uint32_t reg = g_op + EHCI_PORTSC(port);
    uint32_t v = rd32(reg);
    uint32_t before = v;

    if ((v & EHCI_PORT_CCS) == 0) {
        return 0;
    }

    if ((v & EHCI_PORT_PP) == 0) {
        wr32(reg, (v & ~(EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)) |
                  EHCI_PORT_PP);
        delay_loops(200000u);
        v = rd32(reg);
    }

    v &= ~(1u << 13);              /* PO = EHCI */
    v &= ~(EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC);
    v |= EHCI_PORT_PR;
    wr32(reg, v);

    delay_loops(6000000u);         /* >50ms */

    v = rd32(reg);
    v &= ~(1u << 13);
    v &= ~(EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC);
    v &= ~EHCI_PORT_PR;
    wr32(reg, v);

    uint64_t start = u_rdtsc();
    for (;;) {
        v = rd32(reg);
        if (v & EHCI_PORT_PE) {
            g_speed = EHCI_PORT_SPEED(v);
            printk("[EHCI] port%u enabled speed=%u portsc=0x%x\n",
                   port, g_speed, v);
            return 1;
        }
        if (u_rdtsc() - start > EHCI_TIMEOUT_INIT) {
            printk("[EHCI] port%u not enabled before=0x%x now=0x%x\n",
                   port, before, v);
            return 0;
        }
    }
}

/* ============================================================
 * 枚举
 * ============================================================ */

static int parse_config(uint32_t *mps_in, uint32_t *mps_out,
                        uint32_t *ep_in, uint32_t *ep_out) {
    if (!ehci_control(0x80, 6, 0x0200, 0, g_desc, 9, 1)) return 0;

    uint32_t total = (uint32_t)g_desc[2] | ((uint32_t)g_desc[3] << 8);
    if (total < 9u || total > sizeof(g_desc)) return 0;
    if (!ehci_control(0x80, 6, 0x0200, 0, g_desc, (uint16_t)total, 1)) return 0;

    int msc = 0;
    uint32_t in_num = 0, out_num = 0;
    uint32_t in_mps = 0, out_mps = 0;

    uint32_t i = 0;
    while (i + 2u <= total) {
        uint32_t len = g_desc[i];
        uint32_t type = g_desc[i + 1];
        if (len < 2u || i + len > total) break;

        if (type == 4u && len >= 9u) {
            uint8_t cls = g_desc[i + 5];
            uint8_t sub = g_desc[i + 6];
            uint8_t proto = g_desc[i + 7];
            msc = (cls == 8u && sub == 6u && proto == 0x50u);
        } else if (type == 5u && len >= 7u && msc) {
            uint8_t addr = g_desc[i + 2];
            uint8_t attr = g_desc[i + 3];
            uint32_t mps = (uint32_t)g_desc[i + 4] |
                           ((uint32_t)g_desc[i + 5] << 8);
            mps &= 0x7FFu;

            if ((attr & 3u) == 2u) {
                if (addr & 0x80u) {
                    in_num = addr & 0x0Fu;
                    in_mps = mps;
                } else {
                    out_num = addr & 0x0Fu;
                    out_mps = mps;
                }
            }
        }
        i += len;
    }

    if (!msc || in_num == 0 || out_num == 0) return 0;

    *mps_in = in_mps ? in_mps : 512u;
    *mps_out = out_mps ? out_mps : 512u;
    *ep_in = in_num;
    *ep_out = out_num;
    return 1;
}

static int enumerate_device(void) {
    uint32_t port = 0;
    for (uint32_t p = 1; p <= g_ports; p++) {
        uint32_t v = rd32(g_op + EHCI_PORTSC(p));
        if (v & EHCI_PORT_CCS) {
            port = p;
            break;
        }
    }
    if (port == 0) {
        printk("[EHCI] no device connected\n");
        return 0;
    }
    g_port = port;

    if (!port_reset(port)) {
        return 0;
    }

    g_ep0_mps = (g_speed == 2u) ? 64u : 8u;
    g_addr = 0;
    ehci_invalidate_ep_cache();

    /* 第一次 GET_DESCRIPTOR：读 18 字节 */
    if (!ehci_control(0x80, 6, 0x0100, 0, g_desc, 18, 1)) {
        printk("[EHCI] get device descriptor failed\n");
        return 0;
    }

    printk("[EHCI] device %x:%x\n",
           (unsigned int)(g_desc[8] | ((uint32_t)g_desc[9] << 8)),
           (unsigned int)(g_desc[10] | ((uint32_t)g_desc[11] << 8)));

    /* SET_ADDRESS(1) */
    if (!ehci_control(0x00, 5, 1, 0, 0, 0, 0)) {
        printk("[EHCI] set address failed\n");
        return 0;
    }
    g_addr = 1;
    ehci_invalidate_ep_cache();
    delay_loops(100000u);

    /* 用新地址重新读一次描述符 */
    if (!ehci_control(0x80, 6, 0x0100, 0, g_desc, 18, 1)) {
        printk("[EHCI] re-read device descriptor failed\n");
        return 0;
    }

    /* SET_CONFIGURATION(1) */
    if (!ehci_control(0x00, 9, 1, 0, 0, 0, 0)) {
        printk("[EHCI] set configuration failed\n");
        return 0;
    }

    /* SET_INTERFACE(0) */
    ehci_control(0x01, 11, 0, 0, 0, 0, 0);

    uint32_t mps_in, mps_out, ep_in, ep_out;
    if (!parse_config(&mps_in, &mps_out, &ep_in, &ep_out)) {
        printk("[EHCI] no BOT/SCSI interface\n");
        return 0;
    }

    g_bin_num = ep_in;
    g_bout_num = ep_out;
    g_bin_mps = mps_in;
    g_bout_mps = mps_out;
    g_in_dt = 0;
    g_out_dt = 0;

    printk("[EHCI] bulk IN ep%u mps=%u / OUT ep%u mps=%u\n",
           ep_in, mps_in, ep_out, mps_out);

    ehci_invalidate_ep_cache();

    if (!scsi_probe()) {
        return 0;
    }

    /* 非破坏性写自测：读 LBA0 -> 原样写回 -> 再读比对 */
    {
        uint8_t cb[10];

        u_memset(cb, 0, sizeof(cb));
        cb[0] = 0x28;
        cb[8] = 1;
        if (!bot_command(cb, 10, g_xfer_buf, 512, 1)) {
            printk("[EHCI] self-test read0 failed\n");
            return 0;
        }

        u_memset(cb, 0, sizeof(cb));
        cb[0] = 0x2A;
        cb[8] = 1;
        if (!bot_command(cb, 10, g_xfer_buf, 512, 0)) {
            printk("[EHCI] self-test write failed\n");
            return 0;
        }

        u_memset(cb, 0, sizeof(cb));
        cb[0] = 0x28;
        cb[8] = 1;
        if (!bot_command(cb, 10, g_verify_buf, 512, 1)) {
            printk("[EHCI] self-test read1 failed\n");
            return 0;
        }

        for (uint32_t i = 0; i < 512u; i++) {
            if (g_xfer_buf[i] != g_verify_buf[i]) {
                printk("[EHCI] self-test mismatch at %u\n", i);
                return 0;
            }
        }
        printk("[EHCI] write self-test passed\n");
    }

    return 1;
}

/* ============================================================
 * 对外
 * ============================================================ */

int ehci_msc_init(void) {
    if (g_state == 1) return 1;
    if (g_state == 2) return 0;
    g_state = 2;

    if (!find_ehci()) {
        printk("[EHCI] no EHCI controller\n");
        return 0;
    }
    if (!controller_init()) {
        printk("[EHCI] init failed\n");
        return 0;
    }
    if (!enumerate_device()) {
        printk("[EHCI] no usable mass-storage device\n");
        return 0;
    }

    g_state = 1;
    printk("[EHCI] %s, %u sectors (%u MB)\n",
           g_name, g_total_sectors,
           (unsigned int)(g_total_sectors / 2048u));
    return 1;
}

int ehci_msc_present(void) {
    return g_state == 1;
}

int ehci_msc_read(uint32_t lba, uint8_t *buf) {
    if (g_state != 1) return 0;

    uint8_t cb[10];
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x28;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[8] = 1;

    if (!bot_command(cb, 10, g_xfer_buf, 512, 1)) return 0;
    u_memcpy(buf, g_xfer_buf, 512);
    g_reads++;
    return 1;
}

int ehci_msc_write(uint32_t lba, const uint8_t *buf) {
    if (g_state != 1) return 0;

    u_memcpy(g_xfer_buf, buf, 512);

    uint8_t cb[10];
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x2A;
    cb[2] = (uint8_t)(lba >> 24);
    cb[3] = (uint8_t)(lba >> 16);
    cb[4] = (uint8_t)(lba >> 8);
    cb[5] = (uint8_t)lba;
    cb[8] = 1;

    if (!bot_command(cb, 10, g_xfer_buf, 512, 0)) return 0;
    g_writes++;
    return 1;
}

uint32_t ehci_msc_total_sectors(void) {
    return g_total_sectors;
}

/* SYNCHRONIZE CACHE(10)：无数据阶段。 */
int ehci_msc_flush(void) {
    if (g_state != 1) return 0;
    uint8_t cb[10];
    u_memset(cb, 0, sizeof(cb));
    cb[0] = 0x35;
    return bot_command(cb, 10, 0, 0, 0);
}

void ehci_msc_stats(uint32_t *reads, uint32_t *writes) {
    if (reads) *reads = g_reads;
    if (writes) *writes = g_writes;
}

const char *ehci_msc_name(void) {
    return g_name;
}
