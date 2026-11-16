/* ============================================================
 * net.c - Intel E1000 驱动与 TCP/IP 协议栈
 * ------------------------------------------------------------
 * 为什么用轮询而不是中断：
 *   1. 内核的 PIC 初始化把 IRQ9 给了 SCI（ACPICA），IRQ 接线与
 *      EOI 语义已经比较敏感，再插一路 PCI 中断会增加互相干扰面；
 *   2. 网络在本系统中只有同步客户端（curl/ping/nslookup），
 *      调用者本来就要阻塞等待结果，轮询不会浪费可调度时间；
 *   3. 轮询实现没有"中断里不能睡眠/不能长时间持锁"的约束，
 *      协议栈状态机可以写得更直白。
 *   因此 net_init 只做设备初始化，并把 IMC 写成全 1 关闭全部中断。
 *
 * 缓冲区布局（全部静态 BSS，上界远小于 linker.ld 的 20MB 限制）：
 *   RX 环 32 x 2048 = 64KB，TX 环 32 x 2048 = 64KB，
 *   TCP 接收环 64KB，报文拼装缓冲约 8KB。
 *   绝不使用 0xA00000 的 VBE backbuffer，也不占内核堆。
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "net.h"

/* ============================================================
 * 基础工具
 * ============================================================ */

static void net_memzero(void *dst, uint32_t len) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++) p[i] = 0;
}

static void net_memcpy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static inline uint16_t net_bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t net_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

#define net_htons16 net_bswap16
#define net_ntohs16 net_bswap16
#define net_htonl32 net_bswap32
#define net_ntohl32 net_bswap32

/* ============================================================
 * 时间基准：PIT 校准 TSC
 * ------------------------------------------------------------
 * PIT 通道 0 只有 16 位递减计数器（默认分频下约 55ms 回绕一次），
 * 直接用它记时会在长时间不采样时丢圈。这里用 PIT 做一次性校准，
 * 之后全部依赖 TSC 的单调递增。校准窗口 20ms，QEMU 与真机的
 * TSC 在长模式下都可用；-mno-sse 不影响 rdtsc。
 * ============================================================ */

#define NET_PIT_HZ 1193182u

static uint64_t g_tsc_base = 0;
static uint64_t g_tsc_per_ms = 0;
static int      g_time_ready = 0;

static inline uint64_t net_rdtsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint16_t net_pit_latch(void) {
    outb(0x43, 0x00);          /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}

static void net_time_init(void) {
    if (g_time_ready) return;

    uint16_t last = net_pit_latch();
    uint32_t ticks = 0;
    uint64_t start = net_rdtsc();

    /* 累计 20ms 的 PIT 滴答。循环体本身很快（每次几微秒），
     * 远小于 16 位计数器 55ms 的回绕周期，不会漏圈。 */
    while (ticks < (NET_PIT_HZ / 50u)) {
        uint16_t cur = net_pit_latch();
        ticks += (uint16_t)(last - cur);
        last = cur;
    }

    uint64_t delta = net_rdtsc() - start;
    g_tsc_per_ms = delta / 20u;
    if (g_tsc_per_ms == 0) g_tsc_per_ms = 1;
    g_tsc_base = net_rdtsc();
    g_time_ready = 1;
}

uint32_t net_now_ms(void) {
    if (!g_time_ready) net_time_init();
    return (uint32_t)((net_rdtsc() - g_tsc_base) / g_tsc_per_ms);
}

/* ============================================================
 * IPv4 / MAC 文本转换
 * ============================================================ */

int net_parse_ipv4(const char *text, uint32_t *out) {
    uint32_t part[4];
    int count = 0;
    uint32_t value = 0;
    int digits = 0;

    if (!text || *text == '\0') return 0;

    while (*text) {
        char c = *text;
        if (c >= '0' && c <= '9') {
            value = value * 10u + (uint32_t)(c - '0');
            if (++digits > 3) return 0;
        } else if (c == '.') {
            if (digits == 0 || count >= 3) return 0;
            part[count++] = value;
            value = 0;
            digits = 0;
        } else {
            return 0;
        }
        text++;
    }

    if (count != 3 || digits == 0) return 0;
    part[3] = value;

    for (int i = 0; i < 4; i++) {
        if (part[i] > 255u) return 0;
    }

    *out = (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3];
    return 1;
}

void net_format_ipv4(uint32_t ip, char out[16]) {
    uint32_t a = (ip >> 24) & 0xFFu;
    uint32_t b = (ip >> 16) & 0xFFu;
    uint32_t c = (ip >> 8) & 0xFFu;
    uint32_t d = ip & 0xFFu;

    int pos = 0;
    uint32_t fields[4];
    fields[0] = a; fields[1] = b; fields[2] = c; fields[3] = d;

    for (int i = 0; i < 4; i++) {
        uint32_t v = fields[i];
        char tmp[4];
        int n = 0;
        if (v == 0) {
            tmp[n++] = '0';
        }
        while (v > 0) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (n > 0) out[pos++] = tmp[--n];
        if (i != 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

void net_format_mac(const uint8_t mac[6], char out[18]) {
    static const char hex[] = "0123456789ABCDEF";
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        out[pos++] = hex[(mac[i] >> 4) & 0x0F];
        out[pos++] = hex[mac[i] & 0x0F];
        if (i != 5) out[pos++] = ':';
    }
    out[pos] = '\0';
}

/* ============================================================
 * PCI 配置空间（CF8/CFC 机制）
 * ============================================================ */

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           ((uint32_t)off & 0xFCu);
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, off));
    return inw((uint16_t)(PCI_CONFIG_DATA + (off & 2)));
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, off));
    outw((uint16_t)(PCI_CONFIG_DATA + (off & 2)), value);
}

/* ============================================================
 * E1000 寄存器与描述符
 * ============================================================ */

#define E1000_CTRL        0x0000
#define E1000_STATUS      0x0008
#define E1000_EERD        0x0014
#define E1000_ICR         0x00C0
#define E1000_ITR         0x00C4
#define E1000_IMS         0x00D0
#define E1000_IMC         0x00D8
#define E1000_RCTL        0x0100
#define E1000_TCTL        0x0400
#define E1000_TIPG        0x0410
#define E1000_RDBAL       0x2800
#define E1000_RDBAH       0x2804
#define E1000_RDLEN       0x2808
#define E1000_RDH         0x2810
#define E1000_RDT         0x2818
#define E1000_TDBAL       0x3800
#define E1000_TDBAH       0x3804
#define E1000_TDLEN       0x3808
#define E1000_TDH         0x3810
#define E1000_TDT         0x3818
#define E1000_MTA         0x5200
#define E1000_RAL         0x5400
#define E1000_RAH         0x5404

/* CTRL */
#define E1000_CTRL_RST    0x04000000u
#define E1000_CTRL_SLU    0x00000040u
#define E1000_CTRL_ASDE   0x00000020u

/* STATUS */
#define E1000_STATUS_LU   0x00000002u

/* RCTL */
#define E1000_RCTL_EN     0x00000002u
#define E1000_RCTL_SBP    0x00000004u
#define E1000_RCTL_UPE    0x00000008u
#define E1000_RCTL_BAM    0x00008000u
#define E1000_RCTL_SECRC  0x04000000u

/* TCTL */
#define E1000_TCTL_EN     0x00000002u
#define E1000_TCTL_PSP    0x00000008u
#define E1000_TCTL_CT     0x00000100u
#define E1000_TCTL_COLD   0x00040000u

/* 描述符状态与命令 */
#define E1000_RXD_STAT_DD 0x01u
#define E1000_RXD_STAT_EOP 0x02u
#define E1000_TXD_STAT_DD 0x01u
#define E1000_TXD_CMD_EOP 0x01u
#define E1000_TXD_CMD_IFCS 0x02u
#define E1000_TXD_CMD_RS  0x08u

#define NET_RX_COUNT 32u
#define NET_TX_COUNT 32u
#define NET_BUF_SIZE  2048u

struct E1000RxDesc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct E1000TxDesc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* ============================================================
 * 协议头结构（全部 packed，直接覆盖在收发缓冲区上）
 * ============================================================ */

struct EthHeader {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed));

struct ArpPacket {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct IpHeader {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct UdpHeader {
    uint16_t sport;
    uint16_t dport;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct TcpHeader {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define ETH_TYPE_IPV4 0x0800u
#define ETH_TYPE_ARP  0x0806u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

/* ============================================================
 * 全局状态
 * ============================================================ */

static struct NetInfo g_info;

static volatile uint32_t *g_mmio = 0;

static struct E1000RxDesc g_rx_ring[NET_RX_COUNT] __attribute__((aligned(16)));
static struct E1000TxDesc g_tx_ring[NET_TX_COUNT] __attribute__((aligned(16)));
static uint8_t g_rx_buffers[NET_RX_COUNT][NET_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_tx_buffers[NET_TX_COUNT][NET_BUF_SIZE] __attribute__((aligned(16)));

static uint32_t g_rx_cur = 0;
static uint32_t g_tx_cur = 0;
static uint32_t g_ip_id = 1;

/* 报文拼装缓冲区：每次发送前组装、发送后立即失配，
 * 单线程顺序执行，不存在并发覆盖。 */
static uint8_t g_frame[NET_BUF_SIZE];
static uint8_t g_icmp_pkt[NET_BUF_SIZE];
static uint8_t g_udp_pkt[1600];
static uint8_t g_tcp_pkt[1600];
static uint8_t g_dns_query[512];
static uint8_t g_dns_reply[512];

static inline uint32_t e1000_read(uint32_t off) {
    return g_mmio[off >> 2];
}

static inline void e1000_write(uint32_t off, uint32_t value) {
    g_mmio[off >> 2] = value;
}

/* ============================================================
 * 校验和
 * ============================================================ */

static uint32_t checksum_accum(const uint8_t *data, uint32_t len, uint32_t sum) {
    while (len > 1) {
        sum += (uint32_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
        data += 2;
        len -= 2;
    }
    if (len != 0) {
        sum += (uint32_t)((uint16_t)data[0] << 8);
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

static uint16_t transport_checksum(uint32_t src, uint32_t dst,
                                   uint8_t proto, const uint8_t *seg, uint16_t len) {
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFFu;
    sum += src & 0xFFFFu;
    sum += (dst >> 16) & 0xFFFFu;
    sum += dst & 0xFFFFu;
    sum += (uint32_t)proto;
    sum += (uint32_t)len;
    sum = checksum_accum(seg, len, sum);
    return checksum_finish(sum);
}

/* ============================================================
 * E1000 初始化
 * ============================================================ */

static int e1000_read_eeprom(uint8_t addr, uint16_t *out) {
    /* EERD: bit0=Start, bit1=Done, bit8..15=Address, bit16..31=Data */
    e1000_write(E1000_EERD, 1u | ((uint32_t)addr << 8));
    for (uint32_t i = 0; i < 100000u; i++) {
        uint32_t value = e1000_read(E1000_EERD);
        if (value & 0x2u) {
            *out = (uint16_t)(value >> 16);
            return 1;
        }
    }
    return 0;
}

static int e1000_reset(void) {
    uint32_t ctrl = e1000_read(E1000_CTRL);
    e1000_write(E1000_CTRL, ctrl | E1000_CTRL_RST);

    uint32_t start = net_now_ms();
    while (net_now_ms() - start < 100u) {
        if ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) == 0) break;
    }
    /* 复位后 EEPROM 自动加载需要约 10ms，期间访问 RAL/RAH 会读到 0 */
    start = net_now_ms();
    while (net_now_ms() - start < 10u) {
        __asm__ volatile ("pause");
    }
    return 1;
}

static void e1000_read_mac(uint8_t mac[6]) {
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);

    mac[0] = (uint8_t)(ral & 0xFFu);
    mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
    mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
    mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
    mac[4] = (uint8_t)(rah & 0xFFu);
    mac[5] = (uint8_t)((rah >> 8) & 0xFFu);

    int all_zero = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) { all_zero = 0; break; }
    }
    if (!all_zero) return;

    uint16_t w0 = 0;
    uint16_t w1 = 0;
    uint16_t w2 = 0;
    if (e1000_read_eeprom(0, &w0) && e1000_read_eeprom(1, &w1) &&
        e1000_read_eeprom(2, &w2)) {
        mac[0] = (uint8_t)(w0 & 0xFFu);
        mac[1] = (uint8_t)(w0 >> 8);
        mac[2] = (uint8_t)(w1 & 0xFFu);
        mac[3] = (uint8_t)(w1 >> 8);
        mac[4] = (uint8_t)(w2 & 0xFFu);
        mac[5] = (uint8_t)(w2 >> 8);
    }
}

static void e1000_setup_rx(void) {
    for (uint32_t i = 0; i < NET_RX_COUNT; i++) {
        g_rx_ring[i].addr = (uint64_t)(uintptr_t)g_rx_buffers[i];
        g_rx_ring[i].status = 0;
        g_rx_ring[i].length = 0;
    }
    g_rx_cur = 0;

    e1000_write(E1000_RDBAL, (uint32_t)(uintptr_t)g_rx_ring);
    e1000_write(E1000_RDBAH, (uint32_t)((uint64_t)(uintptr_t)g_rx_ring >> 32));
    e1000_write(E1000_RDLEN, NET_RX_COUNT * (uint32_t)sizeof(struct E1000RxDesc));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, NET_RX_COUNT - 1u);

    e1000_write(E1000_RCTL,
                E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_BAM |
                E1000_RCTL_SECRC);
}

static void e1000_setup_tx(void) {
    for (uint32_t i = 0; i < NET_TX_COUNT; i++) {
        g_tx_ring[i].addr = (uint64_t)(uintptr_t)g_tx_buffers[i];
        g_tx_ring[i].length = 0;
        g_tx_ring[i].cso = 0;
        g_tx_ring[i].cmd = 0;
        g_tx_ring[i].status = 0;
        g_tx_ring[i].css = 0;
        g_tx_ring[i].special = 0;
    }
    g_tx_cur = 0;

    e1000_write(E1000_TDBAL, (uint32_t)(uintptr_t)g_tx_ring);
    e1000_write(E1000_TDBAH, (uint32_t)((uint64_t)(uintptr_t)g_tx_ring >> 32));
    e1000_write(E1000_TDLEN, NET_TX_COUNT * (uint32_t)sizeof(struct E1000TxDesc));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    e1000_write(E1000_TCTL,
                E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);
    e1000_write(E1000_TIPG, 10u | (8u << 10) | (6u << 20));
}

static int e1000_find_pci(void) {
    for (uint32_t bus = 0; bus < 256u; bus++) {
        for (uint32_t slot = 0; slot < 32u; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((id & 0xFFFFu) == 0xFFFFu) continue;

            uint8_t funcs = 1;
            uint32_t header = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            if ((header >> 16) & 0x80u) funcs = 8;

            for (uint8_t func = 0; func < funcs; func++) {
                uint32_t cfg = pci_read32((uint8_t)bus, (uint8_t)slot, func, 0x00);
                uint16_t vendor = (uint16_t)(cfg & 0xFFFFu);
                uint16_t device = (uint16_t)(cfg >> 16);
                if (vendor != 0x8086u) continue;

                /* 82540EM / 82545EM / 82574L / I219 家族 */
                if (device == 0x100Eu || device == 0x100Fu ||
                    device == 0x10D3u || device == 0x153Au ||
                    device == 0x15B8u || device == 0x1004u) {
                    g_info.bus = (uint8_t)bus;
                    g_info.slot = (uint8_t)slot;
                    g_info.func = func;
                    g_info.vendor_id = vendor;
                    g_info.device_id = device;
                    return 1;
                }
            }
        }
    }
    return 0;
}

int net_init(void) {
    if (g_info.initialized) return g_info.present ? 0 : -1;

    net_memzero(&g_info, (uint32_t)sizeof(g_info));
    net_time_init();

    /* QEMU 用户网络默认网段，静态配置避免依赖 DHCP 客户端 */
    g_info.ip = 0x0A00020Fu;        /* 10.0.2.15 */
    g_info.netmask = 0xFFFFFF00u;   /* 255.255.255.0 */
    g_info.gateway = 0x0A000202u;   /* 10.0.2.2 */
    g_info.dns = 0x0A000203u;       /* 10.0.2.3 */

    g_info.initialized = 1;

    if (!e1000_find_pci()) {
        g_info.present = 0;
        return -1;
    }

    /* 打开内存空间(Bit1)与总线主控(Bit2)。命令寄存器是低 16 位，
     * 用 16 位写避免把高 16 位的状态位（RW1C）误清零。 */
    uint16_t command = pci_read16(g_info.bus, g_info.slot, g_info.func, 0x04);
    pci_write16(g_info.bus, g_info.slot, g_info.func, 0x04,
                (uint16_t)(command | 0x0006u));

    uint32_t bar0 = pci_read32(g_info.bus, g_info.slot, g_info.func, 0x10);
    if ((bar0 & 0x1u) != 0) {
        g_info.present = 0;
        return -2;
    }
    if ((bar0 & 0x6u) == 0x4u) {
        uint32_t bar1 = pci_read32(g_info.bus, g_info.slot, g_info.func, 0x14);
        g_info.mmio_base = ((uint64_t)bar1 << 32) | (uint64_t)(bar0 & 0xFFFFFFF0u);
    } else {
        g_info.mmio_base = (uint64_t)(bar0 & 0xFFFFFFF0u);
    }
    if (g_info.mmio_base == 0) {
        g_info.present = 0;
        return -2;
    }

    g_mmio = (volatile uint32_t *)(uintptr_t)g_info.mmio_base;

    /* 关闭全部中断，清掉可能的残留中断状态 */
    e1000_write(E1000_IMC, 0xFFFFFFFFu);
    (void)e1000_read(E1000_ICR);
    e1000_write(E1000_ITR, 0);

    e1000_reset();

    e1000_write(E1000_IMC, 0xFFFFFFFFu);
    (void)e1000_read(E1000_ICR);

    /* 清多播表：128 个 32 位寄存器 */
    for (uint32_t i = 0; i < 128u; i++) {
        e1000_write(E1000_MTA + i * 4u, 0);
    }

    e1000_read_mac(g_info.mac);

    /* 写回 MAC 并置 AV(Address Valid) 位 */
    e1000_write(E1000_RAL,
                (uint32_t)g_info.mac[0] |
                ((uint32_t)g_info.mac[1] << 8) |
                ((uint32_t)g_info.mac[2] << 16) |
                ((uint32_t)g_info.mac[3] << 24));
    e1000_write(E1000_RAH,
                (uint32_t)g_info.mac[4] |
                ((uint32_t)g_info.mac[5] << 8) |
                0x80000000u);

    e1000_setup_rx();
    e1000_setup_tx();

    /* 置链路 UP */
    e1000_write(E1000_CTRL,
                e1000_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    uint32_t status = 0;
    uint32_t start = net_now_ms();
    while (net_now_ms() - start < 2000u) {
        status = e1000_read(E1000_STATUS);
        if (status & E1000_STATUS_LU) break;
    }

    g_info.link_up = (status & E1000_STATUS_LU) ? 1 : 0;
    uint32_t speed = (status >> 6) & 0x3u;
    if (speed == 0) g_info.speed_mbps = 10;
    else if (speed == 1) g_info.speed_mbps = 100;
    else g_info.speed_mbps = 1000;

    g_info.present = 1;
    return 0;
}

int net_ready(void) {
    return g_info.initialized && g_info.present && g_info.link_up;
}

struct NetInfo *net_info(void) {
    return &g_info;
}

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns) {
    if (ip != 0) g_info.ip = ip;
    if (netmask != 0) g_info.netmask = netmask;
    if (gateway != 0) g_info.gateway = gateway;
    if (dns != 0) g_info.dns = dns;
}

/* ============================================================
 * 以太网收发
 * ============================================================ */

static int e1000_send_frame(const uint8_t *data, uint16_t len) {
    if (!g_info.present || len == 0 || len > NET_BUF_SIZE) return 0;

    uint32_t idx = g_tx_cur;
    uint32_t start = net_now_ms();

    /* 等待上一轮占用该描述符的报文被硬件回收。
     * status 由 DMA 写回，普通内存读可能被编译器缓存，加编译器屏障强制重读。 */
    for (;;) {
        __asm__ volatile ("" ::: "memory");
        if (g_tx_ring[idx].cmd == 0 ||
            (g_tx_ring[idx].status & E1000_TXD_STAT_DD)) {
            break;
        }
        if (net_now_ms() - start >= 100u) {
            g_info.tx_errors++;
            return 0;
        }
    }

    net_memcpy(g_tx_buffers[idx], data, len);

    g_tx_ring[idx].length = len;
    g_tx_ring[idx].cso = 0;
    g_tx_ring[idx].cmd = (uint8_t)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS |
                                   E1000_TXD_CMD_RS);
    g_tx_ring[idx].status = 0;
    g_tx_ring[idx].css = 0;
    g_tx_ring[idx].special = 0;

    io_memory_barrier();
    e1000_write(E1000_TDT, (idx + 1u) % NET_TX_COUNT);
    g_tx_cur = (idx + 1u) % NET_TX_COUNT;

    /* 等硬件置 DD，确保缓冲区下次可被安全覆盖 */
    start = net_now_ms();
    for (;;) {
        __asm__ volatile ("" ::: "memory");
        if (g_tx_ring[idx].status & E1000_TXD_STAT_DD) break;
        if (net_now_ms() - start >= 100u) {
            g_info.tx_errors++;
            break;
        }
    }

    g_info.tx_packets++;
    g_info.tx_bytes += len;
    return 1;
}

/* ARP 缓存：8 项，按最近使用时间淘汰 */
#define NET_ARP_CACHE_SIZE 8
#define NET_ARP_TTL_MS     300000u

struct ArpEntry {
    int      valid;
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t stamp;
};

static struct ArpEntry g_arp[NET_ARP_CACHE_SIZE];

static void arp_cache_update(uint32_t ip, const uint8_t mac[6]) {
    int slot = -1;
    for (int i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < NET_ARP_CACHE_SIZE; i++) {
            if (!g_arp[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        slot = 0;
        for (int i = 0; i < NET_ARP_CACHE_SIZE; i++) {
            if (g_arp[i].stamp < oldest) {
                oldest = g_arp[i].stamp;
                slot = i;
            }
        }
    }

    g_arp[slot].valid = 1;
    g_arp[slot].ip = ip;
    net_memcpy(g_arp[slot].mac, mac, 6);
    g_arp[slot].stamp = net_now_ms();
}

static int arp_cache_lookup(uint32_t ip, uint8_t mac[6]) {
    uint32_t now = net_now_ms();
    for (int i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            if (now - g_arp[i].stamp > NET_ARP_TTL_MS) {
                g_arp[i].valid = 0;
                return 0;
            }
            net_memcpy(mac, g_arp[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static int arp_send_request(uint32_t target_ip) {
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    struct EthHeader *eth = (struct EthHeader *)g_frame;
    struct ArpPacket *arp = (struct ArpPacket *)(g_frame + sizeof(struct EthHeader));

    net_memcpy(eth->dst, broadcast, 6);
    net_memcpy(eth->src, g_info.mac, 6);
    eth->type = net_htons16(ETH_TYPE_ARP);

    arp->htype = net_htons16(1);          /* Ethernet */
    arp->ptype = net_htons16(ETH_TYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = net_htons16(1);           /* request */
    net_memcpy(arp->sha, g_info.mac, 6);
    arp->spa = net_htonl32(g_info.ip);
    net_memzero(arp->tha, 6);
    arp->tpa = net_htonl32(target_ip);

    g_info.arp_requests++;
    return e1000_send_frame(g_frame, (uint16_t)(sizeof(struct EthHeader) +
                                                sizeof(struct ArpPacket)));
}

static void arp_send_reply(uint32_t target_ip, const uint8_t target_mac[6]) {
    struct EthHeader *eth = (struct EthHeader *)g_frame;
    struct ArpPacket *arp = (struct ArpPacket *)(g_frame + sizeof(struct EthHeader));

    net_memcpy(eth->dst, target_mac, 6);
    net_memcpy(eth->src, g_info.mac, 6);
    eth->type = net_htons16(ETH_TYPE_ARP);

    arp->htype = net_htons16(1);
    arp->ptype = net_htons16(ETH_TYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = net_htons16(2);           /* reply */
    net_memcpy(arp->sha, g_info.mac, 6);
    arp->spa = net_htonl32(g_info.ip);
    net_memcpy(arp->tha, target_mac, 6);
    arp->tpa = net_htonl32(target_ip);

    e1000_send_frame(g_frame, (uint16_t)(sizeof(struct EthHeader) +
                                         sizeof(struct ArpPacket)));
}

int net_arp_resolve(uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms) {
    if (!g_info.present) return 0;
    if (arp_cache_lookup(ip, mac_out)) return 1;

    uint32_t start = net_now_ms();
    uint32_t last_send = start - 1000u;

    while (net_now_ms() - start < timeout_ms) {
        if (net_now_ms() - last_send >= 500u) {
            arp_send_request(ip);
            last_send = net_now_ms();
        }
        net_poll();
        if (arp_cache_lookup(ip, mac_out)) return 1;
    }

    g_info.arp_misses++;
    return 0;
}

static void arp_handle(const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct ArpPacket)) return;

    const struct ArpPacket *arp = (const struct ArpPacket *)pkt;
    if (net_ntohs16(arp->htype) != 1) return;
    if (net_ntohs16(arp->ptype) != ETH_TYPE_IPV4) return;
    if (arp->hlen != 6 || arp->plen != 4) return;

    uint32_t spa = net_ntohl32(arp->spa);
    uint16_t oper = net_ntohs16(arp->oper);

    /* 任何 ARP 报文都带发送方的映射，先缓存再判断是否需要回应 */
    if (spa != 0) {
        arp_cache_update(spa, arp->sha);
    }

    if (oper == 1 && net_ntohl32(arp->tpa) == g_info.ip) {
        arp_send_reply(spa, arp->sha);
        g_info.arp_replies++;
    }
}

/* ============================================================
 * IP 发送
 * ============================================================ */

static int ip_send(uint32_t dst_ip, uint8_t proto,
                   const uint8_t *payload, uint16_t payload_len) {
    if (!g_info.present) return 0;

    uint32_t next_hop = g_info.gateway;
    if ((dst_ip & g_info.netmask) == (g_info.ip & g_info.netmask)) {
        next_hop = dst_ip;
    }
    if (next_hop == 0) next_hop = dst_ip;

    uint8_t dst_mac[6];
    if (!arp_cache_lookup(next_hop, dst_mac)) {
        if (!net_arp_resolve(next_hop, dst_mac, 3000u)) return 0;
    }

    struct EthHeader *eth = (struct EthHeader *)g_frame;
    struct IpHeader *ip = (struct IpHeader *)(g_frame + sizeof(struct EthHeader));

    net_memcpy(eth->dst, dst_mac, 6);
    net_memcpy(eth->src, g_info.mac, 6);
    eth->type = net_htons16(ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = net_htons16((uint16_t)(20u + payload_len));
    ip->id = net_htons16((uint16_t)(g_ip_id++ & 0xFFFFu));
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = proto;
    ip->checksum = 0;
    ip->src = net_htonl32(g_info.ip);
    ip->dst = net_htonl32(dst_ip);

    net_memcpy(g_frame + sizeof(struct EthHeader) + 20u, payload, payload_len);

    uint32_t sum = checksum_accum((const uint8_t *)ip, 20, 0);
    ip->checksum = net_htons16(checksum_finish(sum));

    g_info.ip_tx++;

    uint16_t frame_len = (uint16_t)(sizeof(struct EthHeader) + 20u + payload_len);
    return e1000_send_frame(g_frame, frame_len);
}

/* ============================================================
 * ICMP
 * ============================================================ */

static int      g_ping_waiting = 0;
static uint16_t g_ping_id = 0;
static uint16_t g_ping_seq = 0;
static uint32_t g_ping_start = 0;

static void icmp_send_echo_reply(uint32_t dst_ip, const uint8_t *request, uint16_t len) {
    if (len > (uint16_t)(NET_BUF_SIZE - 40u)) return;

    net_memcpy(g_icmp_pkt, request, len);
    g_icmp_pkt[0] = 0;                 /* Echo Reply */
    g_icmp_pkt[1] = 0;
    g_icmp_pkt[2] = 0;
    g_icmp_pkt[3] = 0;

    uint16_t check = checksum_finish(checksum_accum(g_icmp_pkt, len, 0));
    g_icmp_pkt[2] = (uint8_t)(check >> 8);
    g_icmp_pkt[3] = (uint8_t)(check & 0xFFu);

    ip_send(dst_ip, IP_PROTO_ICMP, g_icmp_pkt, len);
    g_info.icmp_tx++;
}

static void icmp_handle(const uint8_t *pkt, uint16_t len, uint32_t src_ip) {
    if (len < 8) return;
    g_info.icmp_rx++;

    uint8_t type = pkt[0];
    if (type == 8) {
        icmp_send_echo_reply(src_ip, pkt, len);
        return;
    }

    if (type == 0 && g_ping_waiting) {
        uint16_t id = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
        uint16_t seq = (uint16_t)(((uint16_t)pkt[6] << 8) | pkt[7]);
        if (id == g_ping_id && seq == g_ping_seq) {
            g_ping_waiting = 0;
        }
    }
}

int net_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms) {
    if (net_init() != 0) return 0;

    /* 先解析 ARP，把耗时排除在 RTT 之外 */
    uint8_t mac[6];
    if (!net_arp_resolve(ip, mac, 3000u)) return 0;

    static uint16_t payload_seq = 1;
    int payload_len = 32;
    int total = 8 + payload_len;

    net_memzero(g_icmp_pkt, (uint32_t)total);
    g_icmp_pkt[0] = 8;                 /* Echo Request */
    g_icmp_pkt[1] = 0;
    g_ping_id = 0x5341u;               /* "SA" */
    g_ping_seq = payload_seq++;

    g_icmp_pkt[4] = (uint8_t)(g_ping_id >> 8);
    g_icmp_pkt[5] = (uint8_t)(g_ping_id & 0xFFu);
    g_icmp_pkt[6] = (uint8_t)(g_ping_seq >> 8);
    g_icmp_pkt[7] = (uint8_t)(g_ping_seq & 0xFFu);

    for (int i = 0; i < payload_len; i++) {
        g_icmp_pkt[8 + i] = (uint8_t)('a' + (i % 26));
    }

    uint16_t check = checksum_finish(checksum_accum(g_icmp_pkt, (uint32_t)total, 0));
    g_icmp_pkt[2] = (uint8_t)(check >> 8);
    g_icmp_pkt[3] = (uint8_t)(check & 0xFFu);

    g_ping_waiting = 1;
    g_ping_start = net_now_ms();

    if (!ip_send(ip, IP_PROTO_ICMP, g_icmp_pkt, (uint16_t)total)) {
        g_ping_waiting = 0;
        return 0;
    }
    g_info.icmp_tx++;

    while (net_now_ms() - g_ping_start < timeout_ms) {
        net_poll();
        if (!g_ping_waiting) {
            if (rtt_ms) *rtt_ms = net_now_ms() - g_ping_start;
            return 1;
        }
    }

    g_ping_waiting = 0;
    return 0;
}

/* ============================================================
 * UDP 与 DNS
 * ============================================================ */

static uint16_t g_dns_local_port = 53001;
static uint16_t g_dns_txid = 0;
static int      g_dns_waiting = 0;
static int      g_dns_ok = 0;
static uint32_t g_dns_result = 0;

static int udp_send(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                    const uint8_t *data, uint16_t len) {
    if ((uint32_t)(sizeof(struct UdpHeader) + len) > sizeof(g_udp_pkt)) return 0;

    struct UdpHeader *udp = (struct UdpHeader *)g_udp_pkt;
    udp->sport = net_htons16(sport);
    udp->dport = net_htons16(dport);
    udp->length = net_htons16((uint16_t)(sizeof(struct UdpHeader) + len));
    udp->checksum = 0;

    net_memcpy(g_udp_pkt + sizeof(struct UdpHeader), data, len);

    uint16_t check = transport_checksum(g_info.ip, dst_ip, IP_PROTO_UDP,
                                        g_udp_pkt,
                                        (uint16_t)(sizeof(struct UdpHeader) + len));
    if (check == 0) check = 0xFFFFu;   /* UDP 校验和 0 表示"未计算"，必须避免 */
    udp->checksum = net_htons16(check);

    g_info.udp_tx++;
    return ip_send(dst_ip, IP_PROTO_UDP, g_udp_pkt,
                   (uint16_t)(sizeof(struct UdpHeader) + len));
}

static int dns_build_query(const char *host, uint8_t *buf, uint16_t cap,
                           uint16_t *out_len, uint16_t id) {
    if (cap < 17 || host == 0 || *host == '\0') return 0;

    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)(id & 0xFFu);
    buf[2] = 0x01;      /* RD = 1 */
    buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01;   /* QDCOUNT = 1 */
    buf[6] = 0x00; buf[7] = 0x00;   /* ANCOUNT = 0 */
    buf[8] = 0x00; buf[9] = 0x00;   /* NSCOUNT = 0 */
    buf[10] = 0x00; buf[11] = 0x00; /* ARCOUNT = 0 */

    uint32_t off = 12;
    const char *p = host;

    while (*p) {
        if (*p == '.') {
            p++;
            continue;
        }
        const char *label = p;
        uint32_t label_len = 0;
        while (*p && *p != '.') {
            p++;
            label_len++;
        }
        if (label_len == 0 || label_len > 63u) return 0;
        if (off + 1u + label_len + 5u > cap) return 0;

        buf[off++] = (uint8_t)label_len;
        for (uint32_t i = 0; i < label_len; i++) {
            buf[off++] = (uint8_t)label[i];
        }
    }

    if (off + 5u > cap) return 0;
    buf[off++] = 0x00;              /* 名字结束 */
    buf[off++] = 0x00; buf[off++] = 0x01;   /* QTYPE = A */
    buf[off++] = 0x00; buf[off++] = 0x01;   /* QCLASS = IN */

    *out_len = (uint16_t)off;
    return 1;
}

static int dns_skip_name(const uint8_t *buf, uint32_t len, uint32_t *off) {
    while (*off < len) {
        uint8_t c = buf[*off];
        if (c == 0) {
            (*off)++;
            return 1;
        }
        if ((c & 0xC0u) == 0xC0u) {
            if (*off + 2u > len) return 0;
            *off += 2u;
            return 1;
        }
        if ((c & 0xC0u) != 0) return 0;
        *off += (uint32_t)c + 1u;
    }
    return 0;
}

static int dns_parse_reply(const uint8_t *buf, uint32_t len, uint16_t expect_id,
                           uint32_t *ip_out) {
    if (len < 12) return 0;
    uint16_t id = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    if (id != expect_id) return 0;

    uint16_t flags = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    if ((flags & 0x8000u) == 0) return 0;          /* 不是响应 */
    if ((flags & 0x000Fu) != 0) return 0;          /* RCODE != NoError */

    uint16_t qdcount = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    uint16_t ancount = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);

    uint32_t off = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        if (!dns_skip_name(buf, len, &off)) return 0;
        if (off + 4u > len) return 0;
        off += 4u;   /* QTYPE + QCLASS */
    }

    for (uint16_t i = 0; i < ancount; i++) {
        if (!dns_skip_name(buf, len, &off)) return 0;
        if (off + 10u > len) return 0;

        uint16_t type = (uint16_t)(((uint16_t)buf[off] << 8) | buf[off + 1]);
        uint16_t klass = (uint16_t)(((uint16_t)buf[off + 2] << 8) | buf[off + 3]);
        uint16_t rdlen = (uint16_t)(((uint16_t)buf[off + 8] << 8) | buf[off + 9]);
        off += 10u;

        if (off + rdlen > len) return 0;

        if (type == 1u && klass == 1u && rdlen == 4u) {
            *ip_out = ((uint32_t)buf[off] << 24) |
                      ((uint32_t)buf[off + 1] << 16) |
                      ((uint32_t)buf[off + 2] << 8) |
                      (uint32_t)buf[off + 3];
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

static void udp_handle(const uint8_t *pkt, uint16_t len, uint32_t src_ip) {
    if (len < (uint16_t)sizeof(struct UdpHeader)) return;

    const struct UdpHeader *udp = (const struct UdpHeader *)pkt;
    uint16_t sport = net_ntohs16(udp->sport);
    uint16_t dport = net_ntohs16(udp->dport);
    uint16_t ulen = net_ntohs16(udp->length);

    if (ulen < (uint16_t)sizeof(struct UdpHeader) || ulen > len) return;

    /* UDP 校验和可选：0 表示发送方未计算。非 0 时必须校验通过，
     * 否则 DNS 响应可能被中间链路破坏而解析出错误地址。 */
    if (net_ntohs16(udp->checksum) != 0) {
        if (transport_checksum(src_ip, g_info.ip, IP_PROTO_UDP, pkt, ulen) != 0) {
            g_info.rx_errors++;
            return;
        }
    }

    g_info.udp_rx++;

    if (dport == g_dns_local_port && sport == 53u && src_ip == g_info.dns) {
        uint16_t copy = ulen - (uint16_t)sizeof(struct UdpHeader);
        if (copy > sizeof(g_dns_reply)) copy = sizeof(g_dns_reply);
        net_memcpy(g_dns_reply, pkt + sizeof(struct UdpHeader), copy);

        uint32_t resolved = 0;
        if (dns_parse_reply(g_dns_reply, copy, g_dns_txid, &resolved)) {
            g_dns_result = resolved;
            g_dns_ok = 1;
            g_dns_waiting = 0;
        }
    }
}

int net_dns_resolve(const char *host, uint32_t *ip_out, uint32_t timeout_ms) {
    uint32_t literal = 0;
    if (net_parse_ipv4(host, &literal)) {
        *ip_out = literal;
        return 1;
    }

    if (net_init() != 0) return 0;
    if (g_info.dns == 0) return 0;

    /* timeout_ms 是整个解析的总预算，拆成 3 次重试，
     * 避免一次 NXDOMAIN 让调用者等满 15 秒。 */
    uint32_t per_try = timeout_ms / 3u;
    if (per_try < 1000u) per_try = 1000u;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint16_t id = (uint16_t)(net_rdtsc() ^ (uint64_t)(attempt * 7919));
        uint16_t qlen = 0;
        if (!dns_build_query(host, g_dns_query, (uint16_t)sizeof(g_dns_query),
                             &qlen, id)) {
            return 0;
        }

        g_dns_local_port = (uint16_t)(53000u + (net_rdtsc() & 0x3FFu));
        g_dns_txid = id;
        g_dns_ok = 0;
        g_dns_waiting = 1;

        if (!udp_send(g_info.dns, g_dns_local_port, 53, g_dns_query, qlen)) {
            g_dns_waiting = 0;
            continue;
        }

        uint32_t start = net_now_ms();
        while (net_now_ms() - start < per_try) {
            net_poll();
            if (!g_dns_waiting) break;
        }
        g_dns_waiting = 0;

        if (g_dns_ok) {
            *ip_out = g_dns_result;
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 * TCP 客户端
 * ============================================================ */

#define NET_TCP_RX_RING   (64u * 1024u)
#define NET_TCP_TX_MAX    8192u
#define NET_TCP_MSS       1460u
#define NET_TCP_WINDOW    32768u
#define NET_POLL_BUDGET   16u

enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT,
    TCP_STATE_CLOSE_WAIT
};

struct TcpConn {
    int      state;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    int      peer_closed;
    int      failed;
};

static struct TcpConn g_tcp;
static uint8_t  g_tcp_rx_ring[NET_TCP_RX_RING];
static uint32_t g_tcp_rx_head = 0;
static uint32_t g_tcp_rx_tail = 0;
static uint32_t g_tcp_rx_count = 0;
static uint8_t  g_tcp_tx_buf[NET_TCP_TX_MAX];
static uint16_t g_tcp_local_port_next = 49152;

static uint32_t tcp_ring_free(void) {
    return NET_TCP_RX_RING - g_tcp_rx_count;
}

static void tcp_ring_push(const uint8_t *data, uint32_t len) {
    uint32_t first = NET_TCP_RX_RING - g_tcp_rx_tail;
    if (first > len) first = len;
    net_memcpy(g_tcp_rx_ring + g_tcp_rx_tail, data, first);
    if (len > first) {
        net_memcpy(g_tcp_rx_ring, data + first, len - first);
    }
    g_tcp_rx_tail = (g_tcp_rx_tail + len) % NET_TCP_RX_RING;
    g_tcp_rx_count += len;
}

static uint32_t tcp_ring_pop(void *dst, uint32_t cap) {
    uint32_t take = (g_tcp_rx_count < cap) ? g_tcp_rx_count : cap;
    if (take == 0) return 0;

    uint32_t first = NET_TCP_RX_RING - g_tcp_rx_head;
    if (first > take) first = take;
    net_memcpy(dst, g_tcp_rx_ring + g_tcp_rx_head, first);
    if (take > first) {
        net_memcpy((uint8_t *)dst + first, g_tcp_rx_ring, take - first);
    }
    g_tcp_rx_head = (g_tcp_rx_head + take) % NET_TCP_RX_RING;
    g_tcp_rx_count -= take;
    return take;
}

static uint16_t tcp_advertised_window(void) {
    uint32_t free_bytes = tcp_ring_free();
    if (free_bytes > NET_TCP_WINDOW) free_bytes = NET_TCP_WINDOW;
    return (uint16_t)free_bytes;
}

static int tcp_emit(uint32_t seq, uint8_t flags, const uint8_t *data, uint16_t len) {
    if (len > NET_TCP_MSS) return 0;

    struct TcpHeader *tcp = (struct TcpHeader *)g_tcp_pkt;
    net_memzero(tcp, sizeof(struct TcpHeader));

    tcp->sport = net_htons16(g_tcp.local_port);
    tcp->dport = net_htons16(g_tcp.remote_port);
    tcp->seq = net_htonl32(seq);
    tcp->ack = (flags & TCP_ACK) ? net_htonl32(g_tcp.rcv_nxt) : 0;
    tcp->data_off = (uint8_t)(5u << 4);
    tcp->flags = flags;
    tcp->window = net_htons16(tcp_advertised_window());
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (len != 0) {
        net_memcpy(g_tcp_pkt + sizeof(struct TcpHeader), data, len);
    }

    uint16_t check = transport_checksum(g_info.ip, g_tcp.remote_ip, IP_PROTO_TCP,
                                        g_tcp_pkt,
                                        (uint16_t)(sizeof(struct TcpHeader) + len));
    tcp->checksum = net_htons16(check);

    g_info.tcp_tx++;
    return ip_send(g_tcp.remote_ip, IP_PROTO_TCP, g_tcp_pkt,
                   (uint16_t)(sizeof(struct TcpHeader) + len));
}

static uint16_t tcp_alloc_port(void) {
    uint16_t port = g_tcp_local_port_next++;
    if (g_tcp_local_port_next < 49152u) g_tcp_local_port_next = 49152u;
    return port;
}

int net_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    if (net_init() != 0) return -1;

    net_memzero(&g_tcp, (uint32_t)sizeof(g_tcp));
    g_tcp_rx_head = 0;
    g_tcp_rx_tail = 0;
    g_tcp_rx_count = 0;

    g_tcp.remote_ip = ip;
    g_tcp.remote_port = port;
    g_tcp.local_port = tcp_alloc_port();
    g_tcp.iss = ((uint32_t)net_rdtsc() & 0x7FFFFFFFu) | 1u;
    g_tcp.snd_una = g_tcp.iss;
    g_tcp.snd_nxt = g_tcp.iss + 1u;
    g_tcp.rcv_nxt = 0;
    g_tcp.state = TCP_STATE_SYN_SENT;

    uint32_t per_try = timeout_ms / 3u;
    if (per_try < 1000u) per_try = 1000u;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!tcp_emit(g_tcp.iss, TCP_SYN, 0, 0)) {
            g_tcp.state = TCP_STATE_CLOSED;
            return -1;
        }

        uint32_t start = net_now_ms();
        while (net_now_ms() - start < per_try) {
            net_poll();
            if (g_tcp.state == TCP_STATE_ESTABLISHED) return 0;
            if (g_tcp.state == TCP_STATE_CLOSED) return -1;
        }
    }

    g_tcp.state = TCP_STATE_CLOSED;
    return -1;
}

int net_tcp_send(const void *data, uint32_t len, uint32_t timeout_ms) {
    if (g_tcp.state != TCP_STATE_ESTABLISHED && g_tcp.state != TCP_STATE_CLOSE_WAIT) {
        return -1;
    }
    if (len == 0) return 0;
    if (len > NET_TCP_TX_MAX) return -1;

    net_memcpy(g_tcp_tx_buf, data, len);

    uint32_t base = g_tcp.snd_nxt;
    uint32_t off = 0;

    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > NET_TCP_MSS) chunk = NET_TCP_MSS;
        if (!tcp_emit(base + off, TCP_ACK | TCP_PSH, g_tcp_tx_buf + off,
                      (uint16_t)chunk)) {
            return -1;
        }
        off += chunk;
        g_tcp.snd_nxt = base + off;
    }

    uint32_t start = net_now_ms();
    int retries = 0;

    while (g_tcp.snd_una < g_tcp.snd_nxt) {
        net_poll();

        if (g_tcp.state == TCP_STATE_CLOSED || g_tcp.failed) return -1;

        if (g_tcp.snd_una >= g_tcp.snd_nxt) break;

        if (net_now_ms() - start >= timeout_ms) {
            if (++retries > 4) return -1;

            uint32_t acked = g_tcp.snd_una - base;
            if (acked > len) acked = len;
            off = acked;
            g_tcp.snd_nxt = g_tcp.snd_una;

            while (off < len) {
                uint32_t chunk = len - off;
                if (chunk > NET_TCP_MSS) chunk = NET_TCP_MSS;
                if (!tcp_emit(base + off, TCP_ACK | TCP_PSH, g_tcp_tx_buf + off,
                              (uint16_t)chunk)) {
                    return -1;
                }
                off += chunk;
                g_tcp.snd_nxt = base + off;
            }

            start = net_now_ms();
        }
    }

    return (int)len;
}

int net_tcp_recv(void *buf, uint32_t buflen, uint32_t timeout_ms) {
    if (buflen == 0) return 0;

    uint32_t start = net_now_ms();

    for (;;) {
        if (g_tcp_rx_count > 0) {
            return (int)tcp_ring_pop(buf, buflen);
        }
        if (g_tcp.peer_closed || g_tcp.state == TCP_STATE_CLOSED) return 0;
        if (net_now_ms() - start >= timeout_ms) return -1;

        net_poll();
    }
}

void net_tcp_close(void) {
    if (g_tcp.state == TCP_STATE_ESTABLISHED ||
        g_tcp.state == TCP_STATE_CLOSE_WAIT) {
        if (tcp_emit(g_tcp.snd_nxt, (uint8_t)(TCP_FIN | TCP_ACK), 0, 0)) {
            g_tcp.snd_nxt++;
            g_tcp.state = TCP_STATE_FIN_WAIT;

            uint32_t start = net_now_ms();
            while (net_now_ms() - start < 300u) {
                net_poll();
                if (g_tcp.snd_una >= g_tcp.snd_nxt) break;
            }
        }
    } else if (g_tcp.state == TCP_STATE_SYN_SENT) {
        tcp_emit(g_tcp.snd_nxt, (uint8_t)(TCP_RST | TCP_ACK), 0, 0);
    }

    g_tcp.state = TCP_STATE_CLOSED;
}

static void tcp_handle(const uint8_t *pkt, uint16_t len, uint32_t src_ip) {
    if (len < (uint16_t)sizeof(struct TcpHeader)) return;
    if (g_tcp.state == TCP_STATE_CLOSED) return;

    const struct TcpHeader *tcp = (const struct TcpHeader *)pkt;

    uint16_t sport = net_ntohs16(tcp->sport);
    uint16_t dport = net_ntohs16(tcp->dport);

    if (src_ip != g_tcp.remote_ip) return;
    if (sport != g_tcp.remote_port) return;
    if (dport != g_tcp.local_port) return;

    uint32_t hdr_len = (uint32_t)(tcp->data_off >> 4) * 4u;
    if (hdr_len < 20u || hdr_len > len) return;

    uint16_t check = transport_checksum(src_ip, g_info.ip, IP_PROTO_TCP, pkt, len);
    if (check != 0) return;

    uint8_t flags = tcp->flags;
    uint32_t seq = net_ntohl32(tcp->seq);
    uint32_t ack = net_ntohl32(tcp->ack);
    uint32_t dlen = (uint32_t)len - hdr_len;

    g_info.tcp_rx++;

    if (flags & TCP_RST) {
        g_tcp.failed = 1;
        g_tcp.state = TCP_STATE_CLOSED;
        return;
    }

    if (g_tcp.state == TCP_STATE_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ack == g_tcp.iss + 1u) {
            g_tcp.rcv_nxt = seq + 1u;
            g_tcp.snd_una = ack;
            g_tcp.state = TCP_STATE_ESTABLISHED;
            tcp_emit(g_tcp.snd_nxt, TCP_ACK, 0, 0);
        }
        return;
    }

    if (flags & TCP_SYN) {
        /* 已建立连接上出现 SYN 属于异常，回 ACK 让对端收敛 */
        tcp_emit(g_tcp.snd_nxt, TCP_ACK, 0, 0);
        return;
    }

    if (flags & TCP_ACK) {
        if (ack > g_tcp.snd_una && ack <= g_tcp.snd_nxt) {
            g_tcp.snd_una = ack;
        }
    }

    int need_ack = 0;

    if (dlen > 0) {
        if (seq == g_tcp.rcv_nxt) {
            if (tcp_ring_free() >= dlen) {
                tcp_ring_push(pkt + hdr_len, dlen);
                g_tcp.rcv_nxt += dlen;
            }
            /* 窗口不足时丢弃该段，不推进 rcv_nxt，靠对端超时重传恢复 */
        }
        need_ack = 1;
    }

    if (flags & TCP_FIN) {
        if (seq + dlen == g_tcp.rcv_nxt) {
            g_tcp.rcv_nxt += 1u;
            g_tcp.peer_closed = 1;
            if (g_tcp.state == TCP_STATE_ESTABLISHED) {
                g_tcp.state = TCP_STATE_CLOSE_WAIT;
            }
        }
        need_ack = 1;
    }

    if (need_ack) {
        tcp_emit(g_tcp.snd_nxt, TCP_ACK, 0, 0);
    }
}

/* ============================================================
 * IPv4 接收与以太网接收
 * ============================================================ */

static void ip_handle(const uint8_t *pkt, uint32_t len) {
    if (len < 20u) return;

    const struct IpHeader *ip = (const struct IpHeader *)pkt;

    if ((ip->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0Fu) * 4u;
    if (ihl < 20u || ihl > len) return;

    uint16_t total = net_ntohs16(ip->total_len);
    if (total < ihl || total > len) return;

    /* 不实现分片重组：带偏移的分片直接丢弃。TCP 的 MSS 协商保证
     * 正常 HTTP 流量不会被 IP 分片。 */
    if (net_ntohs16(ip->frag_off) & 0x3FFFu) return;

    if (checksum_finish(checksum_accum(pkt, ihl, 0)) != 0) {
        g_info.rx_errors++;
        return;
    }

    uint32_t src = net_ntohl32(ip->src);
    uint32_t dst = net_ntohl32(ip->dst);
    if (dst != g_info.ip) return;

    const uint8_t *payload = pkt + ihl;
    uint16_t plen = (uint16_t)(total - ihl);

    g_info.ip_rx++;

    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            icmp_handle(payload, plen, src);
            break;
        case IP_PROTO_UDP:
            udp_handle(payload, plen, src);
            break;
        case IP_PROTO_TCP:
            tcp_handle(payload, plen, src);
            break;
        default:
            break;
    }
}

static void net_handle_frame(const uint8_t *frame, uint32_t len) {
    if (len < sizeof(struct EthHeader)) return;

    const struct EthHeader *eth = (const struct EthHeader *)frame;
    uint16_t type = net_ntohs16(eth->type);
    const uint8_t *payload = frame + sizeof(struct EthHeader);
    uint32_t plen = len - (uint32_t)sizeof(struct EthHeader);

    if (type == ETH_TYPE_ARP) {
        arp_handle(payload, plen);
    } else if (type == ETH_TYPE_IPV4) {
        ip_handle(payload, plen);
    }
}

void net_poll(void) {
    if (!g_info.present) return;

    for (uint32_t budget = 0; budget < NET_POLL_BUDGET; budget++) {
        struct E1000RxDesc *desc = &g_rx_ring[g_rx_cur];
        __asm__ volatile ("" ::: "memory");
        if (!(desc->status & E1000_RXD_STAT_DD)) break;

        if (desc->status & E1000_RXD_STAT_EOP) {
            if (!(desc->errors)) {
                uint32_t len = desc->length;
                if (len > NET_BUF_SIZE) len = NET_BUF_SIZE;
                g_info.rx_packets++;
                g_info.rx_bytes += len;
                net_handle_frame(g_rx_buffers[g_rx_cur], len);
            } else {
                g_info.rx_errors++;
            }
        }

        desc->status = 0;
        e1000_write(E1000_RDT, g_rx_cur);
        g_rx_cur = (g_rx_cur + 1u) % NET_RX_COUNT;
    }
}
