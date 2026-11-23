/* ============================================================
 * shell.c - SaylterOS Shell
 * ------------------------------------------------------------
 * 打印 banner + prompt + readline + 命令解析 + Levenshtein 纠错
 * ============================================================ */

#include <stdint.h>
#include "tty.h"
#include "keyboard.h"
#include "io.h"
#include "reboot.h"
#include "editor.h"
#include "acpi.h"
#include "acpica_glue.h"
#include "auth.h"
#include "slyzvm.h"
#include "slyz3.h"
#include "slyz3_tty.h"
#include "mem.h"
#include "task.h"
#include "bsod.h"
#include "ui.h"
#include "gui.h"
#include "system_cfg.h"
#include "lang.h"
#include "audio.h"
#include "crypto.h"
#include "net.h"
#include "smp.h"
#include "vm.h"
#include "ime.h"
#include "ime_pinyin.h"
#include "unicode.h"
#include "faultlab.h"
#include "gpu.h"
#include "blk.h"
#include "slyertype.h"
#include "debug.h"
#include "slys.h"

/* ---- SlyzFS 目录项镜像结构 ----
 * 必须与 slyzfs.h 的 DirEntry 以及 bridge 层的 SlyzDirEntry 逐字节一致：
 * 71 = 1(type) + 52(name) + 2(start_cluster) + 4(size) + 4(created) + 4(modified) + 4(reserved)。
 * 不加 pack(1) 时编译器会按 4 字节对齐把结构撑到 72，数组下标访问会整体错位。 */
#pragma pack(push, 1)
struct SlyzDirEntry {
    uint8_t  type;
    uint8_t  flags;
    uint16_t name_len;
    char     name[256];
    uint32_t size;
    uint32_t start_lba;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)

#define SLYZFS_DIR_FILE 0x01
#define SLYZFS_DIR_DIR  0x02

extern int slyzfs_try_mount(void);
extern int slyzfs_format_and_mount(void);
extern int slyzfs_mkdir(const char* path);
extern int slyzfs_rmdir(const char* path);
extern int slyzfs_list_dir(const char* path, struct SlyzDirEntry* out, int out_max);
extern int slyzfs_stat(const char* path, struct SlyzDirEntry* out);
extern int slyzfs_create(const char* path);
extern int slyzfs_remove(const char* path);
extern int slyzfs_usage(uint32_t* total_clusters, uint32_t* used_clusters,
                        uint32_t* cluster_bytes);
extern int slyzfs_mklazy(void);
/* 静默开关：启动页打印期间完成挂载，避免 [SlyzFS] 日志打乱启动画面 */
extern void slyzfs_set_quiet(int quiet);
/* 权限接口：登录层通过这些调用切换操作者身份 */
extern void slyzfs_set_uid(int uid);
extern int  slyzfs_get_uid(void);
extern int  slyzfs_chmod(const char* path, int mode);
extern int  slyzfs_chown(const char* path, int uid);
extern int  slyzfs_set_protected(const char* path, int on);
extern int  slyzfs_is_protected(const char* path);
extern void* slyzfs_open(const char* path, const char* mode);
extern int slyzfs_read(void* fh, uint8_t* buf, int n);
extern int slyzfs_write(void* fh, const uint8_t* buf, int n);
extern void slyzfs_close(void* fh);
#ifdef SAYLTER_VBE
#include "vbe.h"
#include "mouse.h"
#endif

/* MinGW 栈探针 stub —— 裸内核没有 libgcc 的 ___chkstk_ms */
void ___chkstk_ms(void) {}
void __chkstk_ms(void)  {}

/* 简易 delay —— 空循环 */
static void delay(uint32_t count) {
    while (count--) __asm__ volatile ("nop");
}

/* ---------- 简易磁盘测试（ATA PIO + 内存回退） ---------- */
#define ATA_DATA_PORT    0x1F0
#define ATA_ERR_PORT     0x1F1
#define ATA_SECCNT_PORT  0x1F2
#define ATA_LBA_LOW_PORT 0x1F3
#define ATA_LBA_MID_PORT 0x1F4
#define ATA_LBA_HIGH_PORT 0x1F5
#define ATA_DRIVE_PORT   0x1F6
#define ATA_CMD_PORT     0x1F7
#define ATA_CTRL_PORT    0x3F6
#define DISK_TEST_LBA    8192u
#define DISK_MAX_SECTORS 65536u

static uint8_t disk_test_shadow = 0;

static void serial_putc(char c) {
    while ((inb(0x3FD) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
}

static void serial_print(const char *s) {
    while (*s) serial_putc(*s++);
}

void kserial_putchar(char c) {
    /* 故障实验室写 dump / 串口日志落盘期间完全静默：
     * 连硬件串口都不输出，避免 "写盘 -> 串口 -> 再写盘" 的自激循环。 */
    if (faultlab_serial_muted()) return;
    serial_putc((unsigned char)c);
    faultlab_serial_tap((char)c);
}
void kserial_puts(const char *s) { serial_print(s); }
void kserial_dec32(uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}
void kserial_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) {
        serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

static void serial_hex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    serial_putc(hex[(v >> 4) & 0xF]);
    serial_putc(hex[v & 0xF]);
}

static int ata_wait_busy_clear(uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint8_t status = inb(ATA_CMD_PORT);
        if ((status & 0x80) == 0) return 1;
        delay(1000);
    }
    serial_print("[ATA] busy timeout\n");
    return 0;
}

static int ata_wait_drq(uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint8_t status = inb(ATA_CMD_PORT);
        if (status & 0x08) return 1;
        if (status & 0x01) {
            serial_print("[ATA] error status=");
            serial_hex8(status);
            serial_putc('\n');
            return 0;
        }
        delay(1000);
    }
    serial_print("[ATA] drq timeout\n");
    return 0;
}

int ata_disk_present(void) {
    /* 探测结果缓存：该函数位于每次扇区读写的关键路径上，
     * 若每次都在这里做 1000 次空转的 delay，扫描数万扇区会慢到不可用。
     * 硬盘在位状态在系统生命周期内不会变化，只探测一次即可。 */
    static int cached = -1;
    if (cached >= 0) return cached;
    outb(ATA_DRIVE_PORT, 0xA0);
    delay(1000);
    uint8_t status = inb(ATA_CMD_PORT);
    (void)inb(ATA_LBA_MID_PORT);
    (void)inb(ATA_LBA_HIGH_PORT);
    cached = (status != 0x00 && status != 0xFF);
    return cached;
}

/* 注意：ATA 读写是高频路径（扫描可执行文件时单条命令可达 6 万次扇区访问），
 * 绝不能在成功路径上打印串口日志 —— 9600 波特率下会把 3 秒的操作拖成 77 分钟，
 * 表现为"任意命令都卡死"。仅在失败时输出诊断信息。 */
/* ATA 累计读写扇区数：任务管理器的磁盘页面与设备页都依赖它。
 * 只统计成功完成的扇区，失败重试不计入，避免把超时算成写入量。 */
static uint32_t g_ata_read_sectors = 0;
static uint32_t g_ata_write_sectors = 0;

void ata_stats(uint32_t *reads, uint32_t *writes) {
    if (reads) *reads = g_ata_read_sectors;
    if (writes) *writes = g_ata_write_sectors;
}

/* ATA 容量与 LBA48 能力：IDENTIFY DEVICE 只做一次并缓存。
 * 机械盘与 >128GB 的盘必须走 LBA48（0x24/0x34）：LBA28 只能寻址
 * 前 128GB，越界写会绕回磁盘头部，可能直接毁掉分区表。 */
static uint32_t g_ata_total_sectors = 0;
static int g_ata_lba48 = 0;
static int g_ata_identified = 0;
static int g_ata_write_cache = 0;

static void ata_identify_capacity(void) {
    if (g_ata_identified) return;
    g_ata_identified = 1;
    g_ata_total_sectors = 0;
    g_ata_lba48 = 0;
    if (!ata_disk_present()) return;
    if (!ata_wait_busy_clear(20000)) return;

    outb(ATA_DRIVE_PORT, 0xA0);
    outb(ATA_ERR_PORT, 0x00);
    outb(ATA_SECCNT_PORT, 0x00);
    outb(ATA_LBA_LOW_PORT, 0x00);
    outb(ATA_LBA_MID_PORT, 0x00);
    outb(ATA_LBA_HIGH_PORT, 0x00);
    outb(ATA_CMD_PORT, 0xEC);

    uint8_t st = inb(ATA_CMD_PORT);
    if (st == 0x00 || st == 0xFF) return;
    if (!ata_wait_drq(20000)) return;

    uint16_t w[256];
    for (int i = 0; i < 256; i++) w[i] = inw(ATA_DATA_PORT);

    uint32_t cap28 = (uint32_t)w[60] | ((uint32_t)w[61] << 16);
    int lba48 = (w[83] & (1u << 10)) ? 1 : 0;
    uint64_t cap48 = (uint64_t)w[100] | ((uint64_t)w[101] << 16) |
                     ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
    uint64_t total = (lba48 && cap48 != 0) ? cap48 : (uint64_t)cap28;
    if (total == 0) total = 32u * 1024u * 1024u / 512u;
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;

    g_ata_total_sectors = (uint32_t)total;
    g_ata_lba48 = lba48;
    g_ata_write_cache = (w[85] & (1u << 5)) ? 1 : 0;
}

/* 打开设备写缓存。配合 blk_flush() 在日志提交点显式落盘：
 * 既保留机械盘的顺序写性能，又能在掉电前保证已提交事务持久。
 * ACPI 已完成设备枚举与 D0 上电，这里只做 ATA 层的 SET FEATURES。 */
void ata_enable_write_cache(void) {
    ata_identify_capacity();
    if (!ata_disk_present()) return;
    if (!g_ata_write_cache) return;
    if (!ata_wait_busy_clear(20000)) return;

    outb(ATA_DRIVE_PORT, g_ata_lba48 ? 0x40 : 0xA0);
    outb(ATA_ERR_PORT, 0x02);      /* features: enable write cache */
    outb(ATA_SECCNT_PORT, 0x00);
    outb(ATA_LBA_LOW_PORT, 0x00);
    outb(ATA_LBA_MID_PORT, 0x00);
    outb(ATA_LBA_HIGH_PORT, 0x00);
    outb(ATA_CMD_PORT, 0xEF);      /* SET FEATURES */
    (void)ata_wait_busy_clear(20000);
}

/* 把 LBA 写进命令寄存器。lba48 时先写高 24 位，再写低 24 位。 */
static void ata_program_lba(uint32_t lba, int lba48) {
    if (lba48) {
        outb(ATA_DRIVE_PORT, 0x40);            /* LBA 模式，不选设备 */
        outb(ATA_ERR_PORT, 0x00);
        outb(ATA_SECCNT_PORT, 0x00);
        outb(ATA_LBA_LOW_PORT, (uint8_t)(lba >> 24));
        outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 32));
        outb(ATA_LBA_HIGH_PORT, (uint8_t)(lba >> 40));
        outb(ATA_SECCNT_PORT, 0x01);
        outb(ATA_LBA_LOW_PORT, (uint8_t)lba);
        outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
        outb(ATA_LBA_HIGH_PORT, (uint8_t)(lba >> 16));
    } else {
        outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_ERR_PORT, 0x00);
        outb(ATA_SECCNT_PORT, 0x01);
        outb(ATA_LBA_LOW_PORT, (uint8_t)lba);
        outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
        outb(ATA_LBA_HIGH_PORT, (uint8_t)(lba >> 16));
    }
}

/* ATA PIO 的整扇区搬运必须用 rep ins/outs。
 * 逐 word 的 256 次 inw/outw 在 QEMU 下每次都是一次 VM exit，
 * 读 4MB 壁纸要 215 万次端口访问，实测把一次登录拖到十几秒；
 * rep 版本让 CPU 在一条指令内完成整个串，次数降到每扇区一次。 */
static void ata_read_words(uint16_t *dst, int words) {
    uint32_t port = ATA_DATA_PORT;
    __asm__ volatile (
        "cld\n\t"
        "rep insw"
        : "+D"(dst), "+c"((uint32_t)words)
        : "d"(port)
        : "memory");
}

static void ata_write_words(const uint16_t *src, int words) {
    uint32_t port = ATA_DATA_PORT;
    __asm__ volatile (
        "cld\n\t"
        "rep outsw"
        : "+S"(src), "+c"((uint32_t)words)
        : "d"(port)
        : "memory");
}

int ata_read_sector(uint32_t lba, uint8_t *buf) {
    if (!ata_disk_present()) return 0;
    ata_identify_capacity();
    if (!ata_wait_busy_clear(20000)) return 0;

    int use48 = g_ata_lba48 && (lba > 0x0FFFFFFFu);
    ata_program_lba(lba, use48);
    outb(ATA_CMD_PORT, use48 ? 0x24 : 0x20);

    if (!ata_wait_drq(20000)) {
        serial_print("[ATA] read drq timeout, lba=");
        kserial_hex32(lba);
        serial_putc('\n');
        return 0;
    }
    ata_read_words((uint16_t *)(void *)buf, 256);
    g_ata_read_sectors++;
    return 1;
}

/* ============================================================
 * IDE 总线主控 DMA（只用于读）
 * ------------------------------------------------------------
 * PIO 模式下每个 word 都是一次端口访问，QEMU 里读 4MB 壁纸要
 * 两百多万次 I/O，CPU 全部陷在这里；顺序读文件（字体、壁纸、
 * 驱动镜像）因此被拖到秒级。总线主控 DMA 让 IDE 控制器直接
 * 把数据写进内存，读取吞吐提升一个数量级。
 *
 * 写路径保持 PIO：写入还要经过日志与 flush，DMA 出错会直接
 * 破坏盘上数据，风险与收益不成比例。
 * ============================================================ */

#define BMIDE_CMD       0
#define BMIDE_STATUS    2
#define BMIDE_PRDT      4
#define BMIDE_EOT       0x8000u

struct IdePrd {
    uint32_t phys;
    uint16_t count;      /* 字节数；0 表示 64KB */
    uint16_t flags;
};

static uint16_t g_bm_base = 0;
static int g_bm_checked = 0;
static int g_bm_ok = 0;
static int g_dma_failed = 0;   /* 首次失败后永久回退 PIO，避免反复超时 */

static uint32_t ide_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void ide_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                            uint32_t value) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8, addr);
    outl(0xCFC, value);
}

/* 在 PCI 总线上找 IDE 控制器（class 0x01 / subclass 0x01），
 * 读 BAR4 得到总线主控寄存器组的 IO 基址。找不到就直接放弃，
 * 所有调用方回退到 PIO，行为与没有 DMA 时一致。 */
static void ide_dma_probe(void) {
    if (g_bm_checked) return;
    g_bm_checked = 1;

    for (int bus = 0; bus < 4 && !g_bm_ok; bus++) {
        for (int slot = 0; slot < 32 && !g_bm_ok; slot++) {
            for (int fn = 0; fn < 2; fn++) {
                uint32_t id = ide_pci_read32((uint8_t)bus, (uint8_t)slot,
                                             (uint8_t)fn, 0x00);
                if ((id & 0xFFFFu) == 0xFFFFu) break;
                uint32_t cls = ide_pci_read32((uint8_t)bus, (uint8_t)slot,
                                              (uint8_t)fn, 0x08);
                if (((cls >> 24) & 0xFFu) != 0x01u) continue;
                if (((cls >> 16) & 0xFFu) != 0x01u) continue;

                uint32_t bar4 = ide_pci_read32((uint8_t)bus, (uint8_t)slot,
                                               (uint8_t)fn, 0x20);
                if ((bar4 & 0x1u) == 0) continue;
                uint16_t base = (uint16_t)(bar4 & 0xFFFCu);
                if (base == 0) continue;

                /* DMA 之前必须打开 PCI 命令寄存器的 I/O 空间与总线主控
                 * 位，否则 BMIDE 命令不会真正发起传输，状态位永远停在
                 * active——表现出来就是读取挂死。 */
                uint32_t cmd = ide_pci_read32((uint8_t)bus, (uint8_t)slot,
                                              (uint8_t)fn, 0x04);
                cmd |= 0x0005u;              /* I/O space + bus master */
                ide_pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)fn,
                                0x04, cmd);

                g_bm_base = base;
                g_bm_ok = 1;
                break;
            }
        }
    }

    kserial_puts("[ATA] bus-master DMA ");
    if (g_bm_ok) {
        kserial_puts("enabled base=");
        kserial_hex32(g_bm_base);
        kserial_puts("\r\n");
    } else {
        kserial_puts("unavailable, using PIO\r\n");
    }
}

static void bmide_stop(void) {
    if (!g_bm_ok) return;
    outb((uint16_t)(g_bm_base + BMIDE_CMD), 0);
    for (int i = 0; i < 200000; i++) {
        if ((inb((uint16_t)(g_bm_base + BMIDE_STATUS)) & 0x01) == 0) return;
        __asm__ volatile ("pause");
    }
}

/* DMA 读 count 个扇区。返回 0 表示不可用/失败，调用方回退 PIO。
 * 缓冲区只需 4KB 对齐；跨 64KB 边界时拆成两条 PRDT 条目。 */
static int ata_dma_read(uint32_t lba, uint32_t count, uint8_t *buf) {
    ide_dma_probe();
    if (!g_bm_ok || g_dma_failed || count == 0 || count > 256) return 0;
    if (((uintptr_t)buf & 0x1FFu) != 0) return 0;
    if (g_ata_lba48 && lba > 0x0FFFFFFFu) return 0;
    if (!ata_disk_present()) return 0;

    ata_identify_capacity();
    if (!ata_wait_busy_clear(20000)) return 0;

    static struct IdePrd prd[2] __attribute__((aligned(16)));
    uint32_t bytes = count * 512u;
    uintptr_t addr = (uintptr_t)buf;
    uint32_t left = bytes;
    int np = 0;

    while (left > 0 && np < 2) {
        uint32_t chunk = 0x10000u - (uint32_t)(addr & 0xFFFFu);
        if (chunk > left) chunk = left;
        prd[np].phys = (uint32_t)addr;
        prd[np].count = (uint16_t)(chunk >= 65536u ? 0u : chunk);
        prd[np].flags = 0;
        np++;
        addr += chunk;
        left -= chunk;
    }
    if (left != 0) return 0;        /* 超过 128KB 的请求不由 DMA 处理 */
    prd[np - 1].flags = BMIDE_EOT;

    /* 关闭设备中断：本内核没有注册 IRQ14 处理函数，
     * 传输完成由 BMIDE 状态位轮询判断。 */
    outb(ATA_CTRL_PORT, 0x02);

    bmide_stop();
    outb((uint16_t)(g_bm_base + BMIDE_STATUS), 0x06);
    outl((uint16_t)(g_bm_base + BMIDE_PRDT), (uint32_t)(uintptr_t)&prd);

    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERR_PORT, 0x00);
    outb(ATA_SECCNT_PORT, (uint8_t)(count == 256u ? 0u : count));
    outb(ATA_LBA_LOW_PORT, (uint8_t)lba);
    outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH_PORT, (uint8_t)(lba >> 16));

    outb((uint16_t)(g_bm_base + BMIDE_CMD), 0x09);   /* start + 读方向 */
    outb(ATA_CMD_PORT, 0xC8);                        /* READ DMA */

    int ok = 0;
    for (uint32_t i = 0; i < 400000u; i++) {
        uint8_t st = inb((uint16_t)(g_bm_base + BMIDE_STATUS));
        if (st & 0x02) break;                        /* 控制器错误 */
        if ((st & 0x01) == 0) {
            ok = 1;                                  /* 传输结束 */
            break;
        }
        __asm__ volatile ("pause");
    }

    (void)ata_wait_busy_clear(20000);
    bmide_stop();
    outb((uint16_t)(g_bm_base + BMIDE_STATUS), 0x06);
    outb(ATA_CTRL_PORT, 0x00);

    if (!ok) {
        g_dma_failed = 1;
        kserial_puts("[ATA] DMA read failed, falling back to PIO\r\n");
        return 0;
    }
    g_ata_read_sectors += count;
    return 1;
}

/* 多扇区 PIO：一条 READ SECTORS 命令后连续搬 count×256 个 word。
 * DMA 不可用（或缓冲区未对齐）时仍比逐扇区省掉命令握手。 */
static int ata_read_pio_multi(uint32_t lba, uint32_t count, uint8_t *buf) {
    if (count == 0) return 1;
    if (g_ata_lba48 && lba > 0x0FFFFFFFu) {
        for (uint32_t i = 0; i < count; i++) {
            if (!ata_read_sector(lba + i, buf + i * 512u)) return 0;
        }
        return 1;
    }
    if (!ata_wait_busy_clear(20000)) return 0;

    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERR_PORT, 0x00);
    outb(ATA_SECCNT_PORT, (uint8_t)(count == 256u ? 0u : count));
    outb(ATA_LBA_LOW_PORT, (uint8_t)lba);
    outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH_PORT, (uint8_t)(lba >> 16));
    outb(ATA_CMD_PORT, 0x20);

    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq(200000)) return 0;
        ata_read_words((uint16_t *)(void *)(buf + s * 512u), 256);
    }
    g_ata_read_sectors += count;
    return 1;
}

int ata_read_multi(uint32_t lba, uint32_t count, uint8_t *buf) {
    if (count == 0) return 1;
    if (count > 256) {
        uint32_t done = 0;
        while (done < count) {
            uint32_t n = count - done;
            if (n > 256u) n = 256u;
            if (!ata_read_multi(lba + done, n, buf + done * 512u)) return 0;
            done += n;
        }
        return 1;
    }
    if (ata_dma_read(lba, count, buf)) return 1;
    return ata_read_pio_multi(lba, count, buf);
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    if (!ata_disk_present()) return 0;
    ata_identify_capacity();
    if (!ata_wait_busy_clear(20000)) return 0;

    int use48 = g_ata_lba48 && (lba > 0x0FFFFFFFu);
    ata_program_lba(lba, use48);
    outb(ATA_CMD_PORT, use48 ? 0x34 : 0x30);

    if (!ata_wait_drq(20000)) {
        serial_print("[ATA] write drq timeout, lba=");
        kserial_hex32(lba);
        serial_putc('\n');
        return 0;
    }
    ata_write_words((const uint16_t *)(const void *)buf, 256);
    /* 不在这里逐扇区 FLUSH：那会把机械盘拖成龟速。
     * 事务落盘时机由文件系统在日志提交点调用 blk_flush() 决定。 */
    g_ata_write_sectors++;
    return 1;
}/* 显式刷盘：FLUSH CACHE。必须在日志提交、目录元数据更新、
 * 文件 close 与卸载后调用，机械盘掉电时才不会丢已提交事务。 */
int ata_flush(void) {
    if (!ata_disk_present()) return 0;
    if (!ata_wait_busy_clear(20000)) return 0;
    outb(ATA_DRIVE_PORT, g_ata_lba48 ? 0x40 : 0xA0);
    outb(ATA_CMD_PORT, 0xE7);
    return ata_wait_busy_clear(40000);
}

uint32_t ata_total_sectors(void) {
    ata_identify_capacity();
    return g_ata_total_sectors;
}

static int disk_write_test_value(uint32_t sector, uint8_t value) {
    if (sector >= DISK_MAX_SECTORS) return 0;
    uint8_t block[512];
    for (int i = 0; i < 512; i++) block[i] = 0;
    block[0] = value;
    if (!blk_write(sector, block)) return 0;
    disk_test_shadow = value;
    return 1;
}

static int disk_read_test_value(uint32_t sector, uint8_t *out_value) {
    if (sector >= DISK_MAX_SECTORS) return 0;
    uint8_t block[512];
    for (int i = 0; i < 512; i++) block[i] = 0;
    if (!blk_read(sector, block)) return 0;
    *out_value = block[0];
    disk_test_shadow = block[0];
    return 1;
}

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s || *s == '\0') return 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < '0' || c > '9') return 0;
        uint32_t digit = (uint32_t)(c - '0');
        if (v > (UINT32_MAX / 10u)) return 0;
        v = v * 10u + digit;
        s++;
    }
    *out = v;
    return 1;
}

static int parse_u8(const char *s, uint8_t *out) {
    uint32_t v;
    if (!parse_u32(s, &v) || v > 255u) return 0;
    *out = (uint8_t)v;
    return 1;
}

static void split_first_arg(char *args, char **first, char **rest) {
    char *p = args;
    while (*p == ' ' || *p == '\t') p++;
    *first = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p == '\0') {
        *rest = p;
        return;
    }
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;
    *rest = p;
}

/* Banner */
static const char banner[] =
    "  _____              _ _          _____ _____ ______ \n"
    " / ____|            | | |        / ____| ____|  ____|\n"
    "| (___   __ _  ___  | | |_ ___  | (___ | |__ | |__   \n"
    " \\___ \\ / _` |/ _ \\ | | __/ _ \\  \\___ \\|  __||  __|  \n"
    " ____) | (_| |  __/ | | || (_) | ____) | |___| |     \n"
    "|_____/ \\__,_|\\___| |_|\\__\\___/ |_____/|_____|_|     \n"
    "\n"
    "  [ SAYLTER OPERATING SYSTEM ] v0.3 Beta2 - TTY Shell\n"
    "\n";

static const char *help_text[] = {
    "  help                            Show this help",
    "  min                             Show minimal builtin commands",
    "  clear                           Clear screen",
    "  echo <text>                     Print text",
    "  dir / ls [path]                 List a directory (default: current)",
    "  pwd                             Print working directory",
    "  cd <path>                       Change directory (supports .. and /)",
    "  mkdir <path>                    Create a SlyzFS directory",
    "  rmdir <path>                    Remove an empty SlyzFS directory",
    "  rm [-f] <file>...               Delete files (-f skips the confirmation)",
    "  df                              Show filesystem used / free space",
    "  run <file>                      Execute a SLYZ image or text script",
    "  bg <file>                       Run a SLYZ image as a background task",
    "  kill <pid>                      Terminate a task (pid 0 triggers BSOD)",
    "  nano <file> / edit <file>       Full-screen editor (Ctrl+S save, Ctrl+X exit)",
    "  vizcormin [file]                VizCormin app module (Ctrl+S save, Ctrl+Q quit)",
    "  cat <file>                      Print file contents",
    "  ask <prompt>                    Prompt for yes/no input",
    "  exit / quit                     Exit the shell",
    "  dd if=<src> of=<dst> [bs=] [count=] [seek=] [skip=]  Raw sector / text copy (see 'dd' alone for help)",
    "  remove <start> <end>            Clear sectors",
    "  gui                             Enter the graphical desktop session",
    "  version                         Show OS version",
    "  uname                           Show kernel info",
    "  cjk                             Run the UTF-8 / Chinese font self-test",
    "  whoami                          Show current user",
    "  date                            Show fake system date",
    "  mem                             Show kernel heap statistics",
    "  gpu                             Show Intel GPU probe, GGTT and blitter status",
    "  ps                              Show tasks, quotas and memory usage",
    "  smp / cpu                       Show CPUs, APIC IDs and Local APIC base",
    "  reboot                          Reset the machine via 8042",
    "  poweroff / shutdown [n]         ACPI S5 shutdown (optional SLP_TYP 0..7)",
    "  acpi                            Show ACPI / PM1 status",
    "  powercfg /a                     List S0-S5 sleep states and OS support",
    "  powercfg /sleep <1..5>          Enter a sleep state (S2 is deprecated)",
    "  powercfg /lastwake              Show last wake source",
    "  powercfg /devices               Show power button / SCI wiring",
    "  bootmode [gui|tty|status]       Show or set the startup mode (etc\\system.cfg)",
    "  useradd <name>                  Create a user (root only)",
    "  passwd [name]                   Change password (self or root)",
    "  su [name]                       Switch user (default root)",
    "  logout                          End session and show login prompt",
    "  chmod <octal> <path>            Change permissions, e.g. chmod 600 secret.txt",
    "  chown <uid> <path>              Change owner (root only)",
    "  vm                              Show paging, frame pool and fault statistics",
    "  vm swap                         Show swap file, slots and page traffic",
    "  vm limit <pages>                Set resident page quota (16..4096)",
    "  swap                            Alias of 'vm swap'",
    "  pfa                             Run the demand paging + swap self-test",
    "  protect <path>                  Show the protected-file flag",
    "  protect on <path>               Mark a file protected (root only)",
    "  protect off <path> --force      Remove protection (root + explicit override)",
    "  ip / ifconfig                   Show NIC, MAC, IPv4 and packet counters",
    "  ip set <ip> <mask> [gw] [dns]   Configure static IPv4 (default 10.0.2.15)",
    "  curl [-iIvs] [-A ua] [-m sec] <url>   HTTP/1.1 GET with a custom User-Agent",
    "  ping <host> [count]             ICMP echo test (default 4 packets)",
    "  nslookup <host>                 Resolve an A record through DNS",
    0
};

#define SHELL_PATH_MAX 256
#define SHELL_HISTORY_CAP 16

static void cmd_strcpy(char *dst, const char *src);
static void cmd_strncpy(char *dst, const char *src, int n);
static int cmd_strcmp(const char *a, const char *b);
static int cmd_strlen(const char *s);
static int try_run_slyz_by_name(const char *name);
static int exec_cmd(const char *cmd, char *args);
static void format_mode_string(char out[11], uint16_t mode, uint8_t is_dir);
static void cmd_clear(void);
static void cmd_help(void);
static int read_yes_no(void);

static char shell_history[SHELL_HISTORY_CAP][256];
static int shell_history_count = 0;
static int shell_history_index = -1;

/* ---------- 当前工作目录与动态提示符 ----------
 * shell_cwd 使用 SlyzFS 路径约定：反斜杠分隔，空串代表根目录。
 * shell_prompt 由 shell_refresh_prompt() 按 cwd 重新生成，
 * 同时据此动态计算 TTY 底部需要保留几行（安全区），防止大量输出把
 * 提示符顶出屏幕。 */
static char shell_cwd[SHELL_PATH_MAX] = "";
static char shell_prompt[SHELL_PATH_MAX + 32] = "saylter@os:~$";

/* 非交互模式：GUI 内嵌终端设置。此时没有独立的 TTY 键盘，
 * 任何"等用户敲 y/N"或"独占全屏"的命令都必须改写行为，
 * 否则会卡死整个图形循环。 */
static int shell_noninteractive_flag = 0;

void shell_set_noninteractive(int on) {
    shell_noninteractive_flag = on ? 1 : 0;
}

int shell_noninteractive(void) {
    return shell_noninteractive_flag;
}

static void shell_refresh_prompt(void);
static void shell_sync_scroll_zone(void);

static void shell_add_history(const char *text) {
    if (text == 0 || *text == '\0') return;

    for (int i = 0; i < shell_history_count; i++) {
        if (cmd_strcmp(shell_history[i], text) == 0) {
            shell_history_index = shell_history_count;
            return;
        }
    }

    if (shell_history_count < SHELL_HISTORY_CAP) {
        shell_history_count++;
    } else {
        for (int i = 1; i < SHELL_HISTORY_CAP; i++) {
            cmd_strcpy(shell_history[i - 1], shell_history[i]);
        }
    }

    cmd_strcpy(shell_history[shell_history_count - 1], text);
    shell_history_index = shell_history_count;
}

static void shell_load_history(char *buf, int maxlen, int direction) {
    if (shell_history_count == 0) {
        buf[0] = '\0';
        return;
    }

    if (direction > 0) {
        if (shell_history_index >= shell_history_count || shell_history_index < 0) {
            shell_history_index = shell_history_count - 1;
        } else if (shell_history_index > 0) {
            shell_history_index--;
        }
    } else {
        if (shell_history_index < 0) {
            shell_history_index = 0;
        }
        if (shell_history_index + 1 < shell_history_count) {
            shell_history_index++;
        } else {
            shell_history_index = shell_history_count;
            buf[0] = '\0';
            return;
        }
    }

    if (shell_history_index >= shell_history_count || shell_history_index < 0) {
        buf[0] = '\0';
        return;
    }

    cmd_strncpy(buf, shell_history[shell_history_index], maxlen);
}

/* 计算 UTF-8 串在 avail 列内能完整显示的字节数。
 * 多字节字符不能被劈开：宁可少显示一个字，也不能让解码器
 * 收到半个序列后在下一行拼出一个乱码。 */
static int line_fit_bytes(const char *s, int len, int avail) {
    int i = 0;
    int cols = 0;
    while (i < len) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, len - i, &cp);
        if (used <= 0) break;
        int w = utf32_width(cp);
        if (cols + w > avail) break;
        cols += w;
        i += used;
    }
    return i;
}

/* ---- 输入法候选栏 ----
 * 优先画在输入行右侧；右侧放不下就借用上一行。无论画在哪里，
 * 下一次重绘都会先按记录的位置擦掉，屏幕上不会留下残影。 */
static uint16_t g_ime_row;
static uint16_t g_ime_col;
static int      g_ime_w;
static int      g_ime_shown;

static void shell_ime_bar_clear(void) {
    if (!g_ime_shown) return;
    for (int i = 0; i < g_ime_w; i++) {
        tty_put_at((uint16_t)(g_ime_col + i), g_ime_row, ' ', 0x07);
    }
    g_ime_shown = 0;
    g_ime_w = 0;
}

static void shell_ime_bar_draw(uint16_t prompt_x, int line_cols) {
    shell_ime_bar_clear();
    if (!ime_chinese_enabled()) return;
    if (ime_composing()[0] == '\0') return;

    char text[200];
    int p = 0;

    const char *comp = ime_composing();
    for (int i = 0; comp[i] && p < 20; i++) text[p++] = comp[i];
    text[p++] = ' ';

    for (int i = 0; i < IME_PAGE_SIZE; i++) {
        char cand[16];
        int n = ime_candidate_text(i, cand, sizeof(cand));
        if (n <= 0) break;
        if (p + n + 3 >= (int)sizeof(text)) break;
        text[p++] = (char)('1' + i);
        for (int k = 0; k < n; k++) text[p++] = cand[k];
        text[p++] = ' ';
    }
    text[p] = '\0';

    int width = utf8_width(text);
    uint16_t cols = tty_get_cols();
    uint16_t row = tty_get_cursor_y();
    uint16_t col = (uint16_t)(prompt_x + line_cols + 2);

    if ((int)col + width >= (int)cols) {
        if (row == 0) return;
        row = (uint16_t)(row - 1);
        col = 1;
        int blank = width + 1;
        if (col + blank > cols) blank = cols - col;
        for (int i = 0; i < blank; i++) {
            tty_put_at((uint16_t)(col + i), row, ' ', 0x07);
        }
    }

    int drawn = tty_put_text_at(col, row, text, 0x0E);
    g_ime_row = row;
    g_ime_col = col;
    g_ime_w = drawn;
    g_ime_shown = 1;
}

/* 重绘当前输入行：先清掉提示符右侧内容，再逐字符重画。
 * 行宽来自 tty（图形终端通常 > 80 列），不再假设 80x25。
 * 中文按显示列排版，光标位置也用显示列而不是字节数。 */
static void redraw_line(char *buf, int len, int pos, uint16_t prompt_x) {
    uint16_t row = tty_get_cursor_y();
    uint16_t cols = tty_get_cols();

    int avail = (int)cols - (int)prompt_x;
    if (avail < 0) avail = 0;
    int shown = line_fit_bytes(buf, len, avail);

    int cpos = utf8_width_n(buf, pos);
    if (cpos > avail) cpos = avail;

    tty_begin_batch();
    shell_ime_bar_clear();
    tty_clear_from(prompt_x, row);
    tty_set_cursor(prompt_x, row);
    for (int i = 0; i < shown; i++) {
        /* 重绘不是真实输出：只刷新屏幕，不回显串口。
         * 否则每编辑一个字符，串口日志里就会多一整行重复内容。 */
        tty_putchar_vga(buf[i]);
    }
    tty_set_cursor((uint16_t)(prompt_x + cpos), row);
    tty_end_batch();

    shell_ime_bar_draw(prompt_x, utf8_width_n(buf, len));
}

/* 提交当前输入行：
 * readline 把 prompt 和输入画在保留区，这里把它上提到滚动区，
 * 同时向串口补一个换行，保证日志与屏幕历史一致。
 * 不能直接用 tty_putchar('\n')：那会在保留区里触发一次滚动，
 * 把光标拉到滚动区末行却不搬运输入行，结果是旧命令行既不上屏也不被清空。 */
static void shell_commit_input_line(void) {
    /* 提交时清掉输入法状态与候选栏，避免残留到历史输出里 */
    ime_reset();
    shell_ime_bar_clear();
    kserial_putchar('\n');
    tty_commit_reserved();
}

/* 把一段 UTF-8 文本插入到缓冲区的 pos 处（输入法上屏用） */
static int shell_insert_utf8(char *buf, int *len, int *pos, int maxlen,
                             const char *text) {
    int n = 0;
    while (text[n]) n++;
    if (n <= 0) return 0;
    if (*len + n > maxlen - 1) return 0;

    for (int i = *len; i >= *pos; i--) {
        buf[i + n] = buf[i];
    }
    for (int i = 0; i < n; i++) {
        buf[*pos + i] = text[i];
    }
    *len += n;
    *pos += n;
    return 1;
}

static void readline(char *buf, int maxlen, uint16_t prompt_x) {
    int pos = 0;
    int len = 0;
    /* 默认覆盖模式：光标为底部粗线；按 Ins 切到插入模式（半高块） */
    uint8_t insert_mode = 0;
    tty_set_cursor_shape(0x06, 0x06);

    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            /* 空闲等待时驱动光标闪烁 */
            tty_tick();
            /* 排空 ACPICA 的延迟调用（notify / GPE 后续处理） */
            acpica_drain_deferred();
            /* 电源键：优先走 ACPI fixed event（由 SCI 送达），
             * 没有 fixed event 时才回退到 PM1_STS 轮询。 */
            {
                int fixed = acpica_power_button_pending();
                int power = fixed;
                if (!power && acpi_poll_power_button()) {
                    power = 1;
                }
                if (power) {
                    printk("\n[ACPI] power button (%s): shutting down (S5), SCI irq seen=%u\n",
                           fixed ? "SCI fixed event" : "PM1_STS poll",
                           (unsigned int)acpica_sci_count());
                    if (acpica_ready()) {
                        acpica_request_s5();
                    }
                    acpi_power_off();
                    /* 走到这里说明硬件忽略了 S5，恢复提示符与已输入内容 */
                    printk("[ACPI] S5 request ignored by hardware\n");
                    tty_set_cursor(0, (uint16_t)(tty_get_scroll_limit() + 1));
                    printk(shell_prompt);
                    printk(" ");
                    redraw_line(buf, len, pos, prompt_x);
                }
            }
            /* 空闲时让出 CPU：后台脚本任务在这里获得执行机会 */
            task_yield();
            delay(50000);
            continue;
        }

        /* 输入法优先处理：只有它放行，按键才进入命令行的编辑逻辑。
         * 上屏文本按 UTF-8 插入到光标处，之后整行重绘。 */
        {
            char commit[64];
            int ir = ime_key((int)(unsigned char)c, commit, sizeof(commit));
            if (ir == IME_CONSUMED) {
                redraw_line(buf, len, pos, prompt_x);
                continue;
            }
            if (ir == IME_COMMIT) {
                if (shell_insert_utf8(buf, &len, &pos, maxlen, commit)) {
                    redraw_line(buf, len, pos, prompt_x);
                }
                continue;
            }
        }

        if (c == 3) {
            printk("^C");
            buf[0] = '\0';
            shell_commit_input_line();
            return;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            shell_add_history(buf);
            shell_commit_input_line();
            return;
        }

        if (c == (char)0xF0) {
            insert_mode = !insert_mode;
            tty_set_cursor_shape(insert_mode ? 0x0F : 0x06, insert_mode ? 0x0F : 0x06);
            tty_set_cursor((uint16_t)(prompt_x + utf8_width_n(buf, pos)),
                           tty_get_cursor_y());
            continue;
        }

        if (c == (char)0xF6) {
            shell_load_history(buf, maxlen, 1);
            len = cmd_strlen(buf);
            pos = len;
            redraw_line(buf, len, pos, prompt_x);
            continue;
        }

        if (c == (char)0xF7) {
            shell_load_history(buf, maxlen, -1);
            len = cmd_strlen(buf);
            pos = len;
            redraw_line(buf, len, pos, prompt_x);
            continue;
        }

        if (c == (char)0xF1) {
            if (pos > 0) {
                pos = utf8_prev_char(buf, pos);
                tty_set_cursor((uint16_t)(prompt_x + utf8_width_n(buf, pos)),
                               tty_get_cursor_y());
            }
            continue;
        }

        if (c == (char)0xF2) {
            if (pos < len) {
                pos = utf8_next_char(buf, pos);
                tty_set_cursor((uint16_t)(prompt_x + utf8_width_n(buf, pos)),
                               tty_get_cursor_y());
            }
            continue;
        }

        if (c == '\b') {
            if (pos > 0) {
                int prev = utf8_prev_char(buf, pos);
                int del = pos - prev;
                for (int i = prev; i + del <= len; i++) {
                    buf[i] = buf[i + del];
                }
                len -= del;
                pos = prev;
                buf[len] = '\0';
                redraw_line(buf, len, pos, prompt_x);
            }
            continue;
        }

        if (c == (char)0xF5) {
            if (pos < len) {
                int next = utf8_next_char(buf, pos);
                int del = next - pos;
                for (int i = pos; i + del <= len; i++) {
                    buf[i] = buf[i + del];
                }
                len -= del;
                buf[len] = '\0';
                redraw_line(buf, len, pos, prompt_x);
            }
            continue;
        }

        if ((unsigned char)c < 0x20) {
            continue;
        }

        if ((unsigned char)c >= 0x80) {
            continue;
        }

        if (len < maxlen - 1) {
            if (insert_mode && pos < len) {
                for (int i = len; i > pos; i--) {
                    buf[i] = buf[i - 1];
                }
                buf[pos] = c;
                len++;
                pos++;
                redraw_line(buf, len, pos, prompt_x);
            } else {
                buf[pos] = c;
                pos++;
                if (pos > len) len = pos;
                buf[len] = '\0';
                tty_putchar(c);
            }
        }
    }
}

/* ---------- 字符串工具 ---------- */

static void cmd_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++)) {}
}

static void cmd_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int cmd_strcmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if ((ca | 32) != (cb | 32)) return (ca | 32) - (cb | 32);
        a++; b++;
    }
    return ((*a | 32) - (*b | 32));
}

static int cmd_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ---------- 动态提示符与 TTY 安全区 ---------- */

/* 根据当前登录用户与 shell_cwd 重建提示符。
 * 根目录显示 "~"，其余显示完整路径；root 用 '#'，普通用户用 '$'。 */
static void shell_refresh_prompt(void) {
    const struct AuthUser *user = auth_current();
    const char *who = (user && user->name[0]) ? user->name : "root";
    const char *suffix = (user && user->uid != 0) ? "$" : "#";
    const char *shown = (shell_cwd[0] == '\0') ? "~" : shell_cwd;

    int pos = 0;
    int limit = (int)sizeof(shell_prompt) - 1;

    while (*who && pos < limit) shell_prompt[pos++] = *who++;
    const char *at = "@os:";
    while (*at && pos < limit) shell_prompt[pos++] = *at++;
    while (*shown && pos < limit) shell_prompt[pos++] = *shown++;
    while (*suffix && pos < limit) shell_prompt[pos++] = *suffix++;

    shell_prompt[pos] = '\0';
    shell_sync_scroll_zone();
}

/* 按提示符的实际显示宽度（含其后一个空格）动态计算需要保留的行数，
 * 把 TTY 滚动区压到保留区之上。提示符变长（cd 进入深层目录）时
 * 保留行数随之增加，保证输入行永远不会被上方的输出滚掉。
 * 行宽/行高都取自 tty，图形终端下不再是固定的 80x25。 */
static void shell_sync_scroll_zone(void) {
    int cols = (int)tty_get_cols();
    int rows = (int)tty_get_rows();
    int width = cmd_strlen(shell_prompt) + 1;
    int needed = (width + cols - 1) / cols;
    if (needed < 1) needed = 1;
    if (needed > rows - 2) needed = rows - 2;
    if (needed < 1) needed = 1;
    tty_set_scroll_limit((uint16_t)(rows - 1 - needed));
}

static int cmd_strncmp(const char *a, const char *b, int n);

static int leading_indent(const char *s) {
    int n = 0;
    while (s && (s[n] == ' ' || s[n] == '\t')) n++;
    return n;
}

static char *strip_leading_ws(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int find_matching_endif(char **lines, int count, int start_idx, int current_indent) {
    int depth = 1;
    for (int i = start_idx + 1; i < count; i++) {
        char *trim = strip_leading_ws(lines[i]);
        if (*trim == '\0') continue;

        int indent = leading_indent(lines[i]);
        if (cmd_strncmp(trim, "if ", 3) == 0 && indent > current_indent) {
            depth++;
        } else if (cmd_strcmp(trim, "endif") == 0 && indent <= current_indent) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static int cmd_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '\0' && cb == '\0') return 0;
        if (ca == '\0') return -1;
        if (cb == '\0') return 1;
        if ((ca | 32) != (cb | 32)) return ((ca | 32) - (cb | 32));
    }
    return 0;
}

/* ---------- Levenshtein 距离 ---------- */

static int levenshtein(const char *a, const char *b) {
    int la = cmd_strlen(a);
    int lb = cmd_strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    int prev[32], curr[32];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int x = prev[j] + 1;
            int y = curr[j-1] + 1;
            int z = prev[j-1] + cost;
            int m = (x < y) ? x : y;
            if (z < m) m = z;
            curr[j] = m;
        }
        for (int k = 0; k <= lb; k++) prev[k] = curr[k];
    }
    return prev[lb];
}

/* ---------- 内置命令表 ---------- */

static const char *builtin_cmds[] = {
    "help","min","clear","cls","echo","dir","ls","pwd","cd","run","ask","dd","remove",
    "mkdir","rmdir","rm","del","df","nano","edit","vizcormin","app","cat","run","bg","kill","gui","ime","version","uname","whoami","date","mem","linktest",
    "reboot","poweroff","shutdown","halt","acpi","powercfg","bootmode","ps","smp","cpu",
    "useradd","passwd","su","logout","chmod","chown",
    "ip","ifconfig","curl","ping","nslookup", 0
};

/* 找最接近的内置命令，仅返回编辑距离 <= 2 的 */
static const char *find_closest(const char *cmd) {
    const char *best = 0;
    int best_d = 3;  /* 只接受距离 <= 2 */
    for (int i = 0; builtin_cmds[i]; i++) {
        int d = levenshtein(cmd, builtin_cmds[i]);
        if (d < best_d) { best_d = d; best = builtin_cmds[i]; }
    }
    return best;
}

/* ---------- CMOS RTC 读真实时间 ---------- */

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}
/* BCD → 十进制 */
static int bcd2dec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }
/* 读 RTC，返回时等更新完成、处理 BCD/二进制、推算 century */
static void rtc_get(int *out_year, int *out_mon, int *out_day,
                    int *out_hour, int *out_min, int *out_sec) {
    /* 等待 RTC 更新周期结束 (bit 7 of status A = update in progress) */
    while (cmos_read(0x0A) & 0x80) {}

    uint8_t reg_b = cmos_read(0x0B);
    int bcd = !(reg_b & 0x04);  /* bit2=1 是二进制模式, 0 是 BCD (默认 BCD) */

    int s  = cmos_read(0x00);
    int mi = cmos_read(0x02);
    int h  = cmos_read(0x04);
    int d  = cmos_read(0x07);
    int mo = cmos_read(0x08);
    int y  = cmos_read(0x09);

    if (bcd) { s=bcd2dec(s); mi=bcd2dec(mi); h=bcd2dec(h);
               d=bcd2dec(d); mo=bcd2dec(mo); y=bcd2dec(y); }

    /* Century: 先试 0x32（有 BIOS 写入），否则按年份推算 */
    int century = (y <= 70) ? 2000 : 1900;
    uint8_t c = cmos_read(0x32);
    if (c != 0xFF) {
        int ci = bcd ? bcd2dec(c) : c;
        if (ci >= 19 && ci <= 20) century = ci * 100;
    }

    *out_year = century + y;
    *out_mon  = mo;
    *out_day  = d;
    *out_hour = h;
    *out_min  = mi;
    *out_sec  = s;
}
/* 把 1 位/2 位数字补零写成 2 字符 + \0，打印 %s 即可 */
static void pad2(char out[3], int v) {
    out[0] = '0' + (v / 10);
    out[1] = '0' + (v % 10);
    out[2] = '\0';
}

/* ---------- 命令实现 ---------- */

static int slyz_last_answer = 0;

/* ---------- 路径解析 ---------- */

/* 把用户输入的路径规范化为 SlyzFS 绝对路径：
 * 反斜杠分隔、无前导/尾随分隔符、空串表示根目录。
 * 支持绝对路径（以 \ 或 / 开头）、相对路径、"." 与 ".."。 */
static void shell_resolve_path(char *out, const char *cwd, const char *input) {
    char buf[SHELL_PATH_MAX];
    int pos = 0;

    buf[0] = '\0';
    if (!input || !*input) {
        cmd_strncpy(out, cwd, SHELL_PATH_MAX);
        return;
    }

    int absolute = (input[0] == '\\' || input[0] == '/');
    if (!absolute) {
        for (int i = 0; cwd[i] && pos < SHELL_PATH_MAX - 1; i++) buf[pos++] = cwd[i];
    }

    const char *p = input;
    while (*p) {
        while (*p == '\\' || *p == '/' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char seg[64];
        int sl = 0;
        while (*p && *p != '\\' && *p != '/' && *p != ' ' && *p != '\t' && sl < 63) {
            seg[sl++] = *p++;
        }
        seg[sl] = '\0';

        if (sl == 1 && seg[0] == '.') continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            while (pos > 0 && buf[pos - 1] != '\\') pos--;
            if (pos > 0) pos--;
            buf[pos] = '\0';
            continue;
        }

        if (pos > 0 && pos < SHELL_PATH_MAX - 1) buf[pos++] = '\\';
        for (int i = 0; i < sl && pos < SHELL_PATH_MAX - 1; i++) buf[pos++] = seg[i];
        buf[pos] = '\0';
    }

    buf[pos] = '\0';
    cmd_strncpy(out, buf, SHELL_PATH_MAX);
}

/* ---------- 目录浏览与导航 ---------- */

/* 单条目录项的格式化输出。dir 列表与单文件查询共用这一份，
 * 避免两处格式漂移。 */
static void print_dir_entry(const struct SlyzDirEntry *ent) {
    uint16_t mode = ent->mode;
    if (mode == 0) {
        /* 旧卷条目没有权限信息，按保守默认值显示 */
        mode = (ent->type == SLYZFS_DIR_DIR) ? 0700 : 0600;
    }
    char mode_str[11];
    format_mode_string(mode_str, mode,
                       ent->type == SLYZFS_DIR_DIR ? 1 : 0);
    /* 注意：printk 的 vsprintf 不支持 %6u 这类宽度修饰符，
     * 写成 %6u 会让它不消费对应的可变参数，导致后续 %s 参数错位、名字乱码。 */
    printk("  %s  %u  %u  %s\n",
           mode_str,
           (unsigned int)ent->uid,
           (unsigned int)ent->size,
           ent->name);
}

/* dir / ls —— 列出指定目录（缺省为当前工作目录）。
 * 参数可以是相对路径、绝对路径，也可以指向单个文件：
 * 指向文件时只输出该条目，而不是把它当目录去 list_dir。 */

#define SHELL_MODE_PROTECTED 0x8000u
#define SLYZ_LOGO_UTF8 "\xF3\xB0\x80\x81"

/* 隐藏细节：点开头与受保护条目不在普通列表中展开，
 * 但会在计数行留下 SlyzOS 私有字形作为线索。
 * v2 目录项把保护位放在 flags bit0（不再占用 mode 高位）。 */
static int dir_entry_hidden(const struct SlyzDirEntry *e) {
    if (e->name[0] == '.') return 1;
    if (e->flags & 1u) return 1;
    return 0;
}

static void print_dir_footer(int count, int hidden_any) {
    if (count == 1 && !hidden_any) {
        printk("1 item\n");
        return;
    }
    if (count == 1) {
        printk("1 item(s)  " SLYZ_LOGO_UTF8 "\n");
        return;
    }
    if (hidden_any) {
        printk("%d items  " SLYZ_LOGO_UTF8 "\n", count);
    } else {
        printk("%d items\n", count);
    }
}

static void cmd_dir(const char *args) {
    static struct SlyzDirEntry entries[128];
    char path[SHELL_PATH_MAX];

    if (args && *args) {
        shell_resolve_path(path, shell_cwd, args);
    } else {
        cmd_strncpy(path, shell_cwd, SHELL_PATH_MAX);
    }

    const char *shown = path[0] ? path : "\\";

    /* 目标是普通文件：SlyzFS 的 list_dir 只接受目录，这里提前分流 */
    struct SlyzDirEntry single;
    if (path[0] != '\0' && slyzfs_stat(path, &single)) {
        if (single.type != SLYZFS_DIR_DIR) {
            printk("File: %s\n", shown);
            printk("  MODE        UID  SIZE  NAME\n");
            print_dir_entry(&single);
            print_dir_footer(1, dir_entry_hidden(&single));
            return;
        }
    }

    int n = slyzfs_list_dir(path, entries, 128);
    if (n < 0) {
        printk("dir: cannot read '%s' (permission denied or missing)\n", shown);
        return;
    }
    if (n == 0) {
        printk("Directory of %s\n", shown);
        printk("(empty directory)\n");
        return;
    }
    printk("Directory of %s\n", shown);
    printk("  MODE        UID  SIZE  NAME\n");
    int visible = 0;
    int hidden_any = 0;
    for (int i = 0; i < n; i++) {
        if (dir_entry_hidden(&entries[i])) {
            hidden_any = 1;
            continue;
        }
        print_dir_entry(&entries[i]);
        visible++;
    }
    if (visible == 0 && !hidden_any) {
        printk("(empty directory)\n");
        return;
    }
    print_dir_footer(visible, hidden_any);
}

static void cmd_pwd(void) {
    printk("%s\n", shell_cwd[0] ? shell_cwd : "\\");
}

/* ---------- df：显示卷容量与剩余空间 ----------
 * printk 的 vsprintf 不支持 %10u 之类的宽度修饰符，表格对齐只能自己拼字符串。 */

static int df_append_text(char *buf, int pos, int cap, const char *s) {
    while (*s && pos < cap - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

static int df_append_spaces(char *buf, int pos, int cap, int count) {
    while (count-- > 0 && pos < cap - 1) buf[pos++] = ' ';
    buf[pos] = '\0';
    return pos;
}

/* 右对齐写入一个十进制数，width 为所在列宽 */
static int df_append_u32(char *buf, int pos, int cap, uint32_t v, int width) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    if (width > n) pos = df_append_spaces(buf, pos, cap, width - n);
    while (n > 0 && pos < cap - 1) buf[pos++] = tmp[--n];
    buf[pos] = '\0';
    return pos;
}

/* 右对齐写入一段文本 */
static int df_append_text_right(char *buf, int pos, int cap, const char *s, int width) {
    int n = 0;
    while (s[n]) n++;
    if (width > n) pos = df_append_spaces(buf, pos, cap, width - n);
    return df_append_text(buf, pos, cap, s);
}

static void cmd_df(void) {
    uint32_t total_clusters = 0;
    uint32_t used_clusters = 0;
    uint32_t cluster_bytes = 0;

    if (!slyzfs_usage(&total_clusters, &used_clusters, &cluster_bytes) ||
        cluster_bytes == 0) {
        printk("df: SlyzFS is not mounted\n");
        return;
    }

    uint32_t free_clusters = (used_clusters <= total_clusters)
                                 ? (total_clusters - used_clusters) : 0;
    /* 卷上限约 32MB（65536 扇区），簇数 * 簇字节数不会溢出 32 位 */
    uint32_t total_kb = (total_clusters * cluster_bytes) / 1024u;
    uint32_t used_kb = (used_clusters * cluster_bytes) / 1024u;
    uint32_t free_kb = (free_clusters * cluster_bytes) / 1024u;
    uint32_t percent = (total_clusters > 0)
                           ? (uint32_t)((used_clusters * 100u) / total_clusters) : 0;

    const int col_kb = 12;
    const int col_pct = 6;

    /* 表头与数据行必须用同一套列宽构造，否则头尾不对齐 */
    char head[128];
    int hp = 0;
    hp = df_append_text(head, hp, sizeof(head), "Filesystem");
    hp = df_append_spaces(head, hp, sizeof(head), 4);
    hp = df_append_text_right(head, hp, sizeof(head), "1K-blocks", col_kb);
    hp = df_append_text_right(head, hp, sizeof(head), "Used", col_kb);
    hp = df_append_text_right(head, hp, sizeof(head), "Available", col_kb);
    hp = df_append_text_right(head, hp, sizeof(head), "Use%", col_pct);
    hp = df_append_text(head, hp, sizeof(head), "  Mounted on");
    printk("%s\n", head);

    char row[128];
    int rp = 0;
    rp = df_append_text(row, rp, sizeof(row), "SlyzFS");
    rp = df_append_spaces(row, rp, sizeof(row), 8);
    rp = df_append_u32(row, rp, sizeof(row), total_kb, col_kb);
    rp = df_append_u32(row, rp, sizeof(row), used_kb, col_kb);
    rp = df_append_u32(row, rp, sizeof(row), free_kb, col_kb);
    rp = df_append_u32(row, rp, sizeof(row), percent, col_pct - 1);
    rp = df_append_text(row, rp, sizeof(row), "%");
    rp = df_append_text(row, rp, sizeof(row), "  /");
    printk("%s\n", row);

    printk("\n  clusters : %u used / %u total (%u bytes each)\n",
           (unsigned int)used_clusters, (unsigned int)total_clusters,
           (unsigned int)cluster_bytes);
    printk("  raw disk : %u sectors (%u KB) via %s\n",
           (unsigned int)blk_total_sectors(),
           (unsigned int)(blk_total_sectors() / 2u),
           blk_backend());
}

static void cmd_cd(const char *args) {
    char target[SHELL_PATH_MAX];
    shell_resolve_path(target, shell_cwd, args);

    if (target[0] == '\0') {
        shell_cwd[0] = '\0';
        shell_refresh_prompt();
        return;
    }

    struct SlyzDirEntry ent;
    if (!slyzfs_stat(target, &ent)) {
        printk("cd: '%s' does not exist\n", target);
        return;
    }
    if (ent.type != SLYZFS_DIR_DIR) {
        printk("cd: '%s' is not a directory\n", target);
        return;
    }

    cmd_strncpy(shell_cwd, target, SHELL_PATH_MAX);
    shell_refresh_prompt();
}

static void cmd_mkdir(const char *args) {
    if (!args || !*args) {
        printk("usage: mkdir <path>\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (slyzfs_mkdir(path)) {
        printk("mkdir: '%s' created\n", path);
    } else {
        printk("mkdir: cannot create '%s'\n", path);
    }
}

static void cmd_rmdir(const char *args) {
    if (!args || !*args) {
        printk("usage: rmdir <path>\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (slyzfs_rmdir(path)) {
        printk("rmdir: '%s' removed\n", path);
    } else {
        printk("rmdir: cannot remove '%s'\n", path);
    }
}

/* ---------- rm / del：删除文件 ----------
 * 与 rmdir 互补：这里只删普通文件，目录必须走 rmdir（保持"删除前先清空"的语义）。
 * 默认逐个确认，避免误删；-f 关闭确认，供脚本与批量删除使用。
 * 支持一次传多个路径。 */

/* 删除单个目标，返回 1 表示确实删掉了 */
static int rm_remove_target(const char *target, int force) {
    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, target);

    if (path[0] == '\0') {
        printk("rm: refusing to remove the root directory\n");
        return 0;
    }

    struct SlyzDirEntry ent;
    if (!slyzfs_stat(path, &ent)) {
        /* 路径不存在：-f 下保持静默，和 Unix rm 的行为一致 */
        if (!force) {
            printk("rm: cannot remove '%s': no such file or directory\n", path);
        }
        return 0;
    }

    if (ent.type == SLYZFS_DIR_DIR) {
        printk("rm: '%s' is a directory (use rmdir)\n", path);
        return 0;
    }

    if (!force) {
        if (shell_noninteractive()) {
            /* GUI 终端里没法回应 y/N 提示，要求显式 -f，避免误删 */
            printk("rm: '%s' skipped (use 'rm -f' in the GUI terminal)\n", path);
            return 0;
        }
        printk("rm: remove file '%s'? [y/N] ", path);
        if (!read_yes_no()) {
            printk("rm: '%s' not removed\n", path);
            return 0;
        }
    }

    if (!slyzfs_remove(path)) {
        /* stat 已确认条目存在且是文件，remove 失败只可能是父目录写权限不足 */
        printk("rm: '%s': permission denied\n", path);
        return 0;
    }

    printk("rm: '%s' removed\n", path);
    return 1;
}

static void cmd_rm(char *args) {
    if (!args || !*args) {
        printk("usage: rm [-f] <file> [file...]\n");
        return;
    }

    int force = 0;
    int handled = 0;
    char *p = args;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';

        if (cmd_strcmp(tok, "-f") == 0) {
            force = 1;
            continue;
        }
        if (tok[0] == '-') {
            printk("rm: unknown option '%s' (supported: -f)\n", tok);
            continue;
        }

        handled = 1;
        rm_remove_target(tok, force);
    }

    if (!handled) {
        printk("usage: rm [-f] <file> [file...]\n");
    }
}

static void cmd_run(const char *args) {
    if (!args || !*args) {
        printk("usage: run <file>\n");
        return;
    }

    int rc = try_run_slyz_by_name(args);
    if (rc == 1) return;
    if (rc == 2) {
        printk("run: %s terminated abnormally\n", args);
        return;
    }
    printk("run: %s not found\n", args);
}

/* ---- app：加载并执行 .slys 预装模块 ----
 * 模块按头部声明的固定地址加载，入口签名为
 *   void *entry(const struct SlysKernelApi *api)
 * 入口返回后控制权回到 shell；musl 应用通过 longjmp 把 exit
 * 转成"入口返回"，因此这里能拿到退出码。 */
static void cmd_app(const char *args) {
    char rel[SHELL_PATH_MAX];
    while (args && (*args == ' ' || *args == '\t')) args++;
    if (!args || *args == '\0') {
        printk("usage: app <program.slys>\n");
        return;
    }

    shell_resolve_path(rel, shell_cwd, args);
    if (rel[0] == '\0') {
        printk("app: invalid path\n");
        return;
    }

    char full[SHELL_PATH_MAX + 4];
    int n = 0;
    full[n++] = 'C';
    full[n++] = ':';
    full[n++] = '\\';
    for (int i = 0; rel[i] && n < (int)sizeof(full) - 1; i++) {
        full[n++] = rel[i];
    }
    full[n] = '\0';

    void *entry = 0;
    char name[24];
    if (!slys_load(full, &entry, name, (int)sizeof(name))) {
        printk("app: cannot load %s\n", full);
        return;
    }

    SlysModuleEntryFn fn = (SlysModuleEntryFn)entry;
    void *rc = fn(slys_kernel_api());
    printk("\napp: '%s' exited with %d\n", name, (int)(intptr_t)rc);
}

/* ---- nano / edit：全屏文本编辑器 ----
 * 路径先按当前工作目录解析成 SlyzFS 绝对路径，再交给编辑器。
 * 编辑期间编辑器独占整个屏幕，退出时自行清屏并把 TTY 交还，
 * 这里只需重新计算一次安全区。 */
static void cmd_edit(const char *args) {
    if (!args || *args == '\0') {
        printk("usage: nano <file>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (path[0] == '\0') {
        printk("nano: invalid file name\n");
        return;
    }

    /* 已存在的文件先试读打开：这样能在进入全屏界面之前就拒绝无权限的文件，
     * 而不是让用户编辑半天后在保存时才失败。 */
    struct SlyzDirEntry probe_ent;
    if (slyzfs_stat(path, &probe_ent)) {
        void *probe = slyzfs_open(path, "r");
        if (!probe) {
            printk("nano: %s: permission denied\n", path);
            return;
        }
        slyzfs_close(probe);
    }

    int rc = 0;
    if (shell_noninteractive()) {
        /* GUI 终端会自己接管 nano/edit，在终端窗口里跑字符编辑器。
         * 走到这里说明是从脚本等路径调用的，没有可用的终端界面。 */
        printk("nano: run this from the GUI terminal window\n");
        return;
    }

    rc = editor_run(path);
    shell_refresh_prompt();

    if (rc == 0) {
        printk("nano: closed %s\n", path);
    } else {
        printk("nano: cannot edit %s\n", path);
    }
}

/* ---- VizCormin 窗口宿主桥接 ----
 * shell 只做三件事：解析路径、加载 .slys 模块、把模块导出的能力表
 * 接到 GUI 的窗口系统上。编辑器代码完全不在内核镜像里。
 * 图形会话中命令立即返回，窗口由 GUI 每帧驱动；TTY 会话没有窗口
 * 系统，shell 自己驱动模块全屏渲染。 */
static const struct SlysVizCorminHost *g_vc_host = 0;

static void vc_bridge_render(int x, int y, int w, int h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    if (g_vc_host && g_vc_host->render) g_vc_host->render();
}

static void vc_bridge_key(int key) {
    if (g_vc_host && g_vc_host->key) {
        g_vc_host->key((uint32_t)(uint8_t)key);
    }
}

static void vc_bridge_mouse(int x, int y, int button) {
    if (g_vc_host && g_vc_host->mouse) g_vc_host->mouse(x, y, button);
}

static void vc_bridge_close(void) {
    if (g_vc_host && g_vc_host->shutdown) g_vc_host->shutdown();
    g_vc_host = 0;
}

static void vc_bridge_resize(int w, int h) {
    (void)w;
    (void)h;
    if (g_vc_host && g_vc_host->resize) g_vc_host->resize();
}

static const GuiExternalHost g_vc_bridge = {
    vc_bridge_render,
    vc_bridge_key,
    vc_bridge_mouse,
    vc_bridge_close,
    vc_bridge_resize
};

static void cmd_vizcormin(const char *args) {
    char path[SHELL_PATH_MAX];
    path[0] = '\0';

    if (args && *args) {
        shell_resolve_path(path, shell_cwd, args);
        if (path[0] == '\0') {
            printk("vizcormin: invalid file name\n");
            return;
        }
        struct SlyzDirEntry probe;
        for (uint32_t i = 0; i < sizeof(probe); i++) ((uint8_t *)&probe)[i] = 0;
        if (!slyzfs_stat(path, &probe)) {
            printk("vizcormin: %s: no such file\n", path);
            return;
        }
    }

    if (g_vc_host) {
        printk("vizcormin: already running\n");
        return;
    }

    void *entry = 0;
    char name[24];
    if (!slys_load("C:\\Programs\\VizCormin\\vizcormin.slys",
                   &entry, name, (int)sizeof(name))) {
        printk("vizcormin: cannot load C:\\Programs\\VizCormin\\vizcormin.slys\n");
        return;
    }

    SlysVizCorminEntryFn fn = (SlysVizCorminEntryFn)entry;
    const struct SlysVizCorminHost *host = fn(slys_kernel_api());
    if (!host || host->version != SLYS_VIZCORMIN_ABI || !host->init) {
        printk("vizcormin: module ABI mismatch\n");
        return;
    }
    g_vc_host = host;

    if (shell_noninteractive()) {
        /* GUI：先开窗口，应用再按内容区尺寸初始化。 */
        if (!gui_external_open("VizCormin", 960, 620, &g_vc_bridge)) {
            g_vc_host = 0;
            printk("vizcormin: cannot open window\n");
            return;
        }
        if (host->init(slys_kernel_api(), path) < 0) {
            gui_external_close();
            g_vc_host = 0;
            printk("vizcormin: init failed\n");
            return;
        }
        return;
    }

    /* TTY：模块全屏绘制，shell 负责交换与读键。 */
    if (host->init(slys_kernel_api(), path) < 0) {
        g_vc_host = 0;
        printk("vizcormin: init failed\n");
        return;
    }
    const struct SlysKernelApi *api = slys_kernel_api();
    for (;;) {
        host->render();
        if (api->fb_swap) api->fb_swap();

        char ch = api->kbd_read ? api->kbd_read() : 0;
        if (ch == 0) {
            for (volatile int i = 0; i < 30000; i++) {
                __asm__ volatile ("nop");
            }
            continue;
        }
        host->key((uint32_t)(uint8_t)ch);
        if (host->finished && host->finished()) break;
    }
    host->shutdown();
    g_vc_host = 0;
    tty_repaint();
}

/* ---- cat：输出文件内容 ----
 * 通过 SlyzFS 打开并分块读取，正文直接写 VGA 而不经过 tty_putchar：
 * 后者每个字符都要等一次 9600 波特率的串口发送，十几 KB 的文件会把
 * 屏幕拖成十几秒的幻灯片。串口侧只在结束时补一行统计，保留可追溯性。 */
static void cmd_cat(const char *args) {
    if (!args || *args == '\0') {
        printk("usage: cat <file>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (path[0] == '\0') {
        printk("cat: invalid path\n");
        return;
    }

    struct SlyzDirEntry ent;
    if (!slyzfs_stat(path, &ent)) {
        printk("cat: %s: no such file\n", path);
        return;
    }
    if (ent.type == SLYZFS_DIR_DIR) {
        printk("cat: %s: is a directory\n", path);
        return;
    }

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        /* 前面已用 stat 确认条目存在且不是目录，open 失败只可能是权限 */
        printk("cat: %s: permission denied\n", path);
        return;
    }

    uint8_t chunk[512];
    int total = 0;
    int ended_with_newline = 1;
    int n;
    while ((n = slyzfs_read(fh, chunk, (int)sizeof(chunk))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            /* CRLF 文本只保留 '\n'，否则屏幕上每行之间会多出空行 */
            if (c == '\r') continue;
            tty_putchar_vga(c);
            total++;
            ended_with_newline = (c == '\n');
        }
    }
    slyzfs_close(fh);

    /* 文件末尾没有换行时补一个，避免下一条输出粘在最后一行行尾 */
    if (!ended_with_newline) {
        tty_putchar_vga('\n');
    }

    kserial_puts("[cat] ");
    kserial_puts(path);
    kserial_puts(" ");
    kserial_dec32((uint32_t)total);
    kserial_puts(" bytes\r\n");
}

static void cmd_ask(const char *args) {
    char prompt[96];
    if (!args || !*args) {
        printk("usage: ask <prompt>\n");
        return;
    }

    cmd_strncpy(prompt, args, sizeof(prompt));
    char *p = prompt;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    char *end = p + cmd_strlen(p) - 1;
    while (end >= p && (*end == ' ' || *end == '\t' || *end == '"')) {
        *end-- = '\0';
    }

    printk("%s [Y/N] ", p);
    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            tty_tick();
            delay(50000);
            continue;
        }
        if (c == 3) {
            printk("^C\n");
            slyz_last_answer = 0;
            return;
        }
        if (c == 'y' || c == 'Y') {
            printk("y\n");
            slyz_last_answer = 1;
            return;
        }
        if (c == 'n' || c == 'N') {
            printk("n\n");
            slyz_last_answer = 0;
            return;
        }
    }
}

/* SLYZ v2 镜像按文件实际大小从内核堆分配，嵌套层数由 slyz_depth 限制，
 * 每层在执行期间各自持有一份独立副本。 */
static int slyz_depth = 0;

static int slyz_op_run(const char *line) {    static char buffer[256];
    cmd_strncpy(buffer, line, sizeof(buffer));

    char *cmd = buffer;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return 1;

    char *args = cmd;
    while (*args && *args != ' ' && *args != '\t') args++;
    if (*args) {
        *args++ = '\0';
        while (*args == ' ' || *args == '\t') args++;
    }

    if (cmd_strcmp(cmd, "ask") == 0) {
        cmd_ask(args);
        return 1;
    }
    return exec_cmd(cmd, args) != 0;
}


/* 脚本的内存申请走任务配额：超出配额时 OOM 直接杀掉当前脚本任务。
 * 内核任务（id 0）没有配额，失败只返回 0，由 VM 终止脚本。 */
static void *slyz_op_alloc(uint32_t bytes) {
    struct Task *t = task_current();
    void *ptr = task_alloc(bytes);
    if (ptr) return ptr;

    if (t && t->id != 0) {
        task_oom_kill(bytes);   /* 不返回 */
    }
    return 0;
}

/* ---------- SLYZ v2 宿主：系统调用实现 ----------
 * 虚拟机只搬运参数，具体行为全部在这里落地：屏幕输出、SlyzFS、
 * system.cfg、RTC 与 shell 命令。图形调用在 TTY 会话里没有窗口
 * 承载，统一返回 -1 并提示一次，程序应自行降级。 */

static uint8_t slyz_text_buf[4096];
static int slyz_gui_warned = 0;

static const char *slyz_arg_text(struct SlyzVal *args, uint16_t argc, int index) {
    if (index < 0 || index >= (int)argc) return "";
    int n = 0;
    const char *s = slyz_val_str(&args[index], &n);
    return s ? s : "";
}

static int64_t slyz_arg_int(struct SlyzVal *args, uint16_t argc, int index) {
    if (index < 0 || index >= (int)argc) return 0;

    struct SlyzVal *v = &args[index];
    if (v->type == SLYZ_T_INT) return v->u.i;

    int n = 0;
    const char *s = slyz_val_str(v, &n);
    if (!s) return 0;

    int i = 0;
    int neg = 0;
    int64_t out = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        out = out * 10 + (s[i] - '0');
        i++;
    }
    return neg ? -out : out;
}

static void slyz_host_log(const char *text) {
    printk("slyz: %s\n", text);
}

static void slyz_print_value(struct SlyzVm *vm, struct SlyzVal *v, int newline) {
    char buf[512];
    int n = slyz_val_to_text(vm, v, buf, (int)sizeof(buf));
    if (n > 0) printk("%s", buf);
    if (newline) printk("\n");
}

static void slyz_path_of(const char *input, char *out) {
    shell_resolve_path(out, shell_cwd, input);
}

static int slyz_fs_read(struct SlyzVm *vm, const char *path, struct SlyzVal *ret) {
    char full[SHELL_PATH_MAX];
    slyz_path_of(path, full);

    void *fh = slyzfs_open(full, "r");
    if (!fh) {
        slyz_ret_nil(ret);
        return 0;
    }

    int total = 0;
    while (total < (int)sizeof(slyz_text_buf) - 1) {
        int n = slyzfs_read(fh, slyz_text_buf + total,
                            (int)sizeof(slyz_text_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);

    slyz_text_buf[total] = '\0';
    if (!slyz_ret_str(vm, ret, (const char *)slyz_text_buf, total)) {
        return -1;
    }
    return 0;
}

static int slyz_fs_write(const char *path, const char *text, int len) {
    char full[SHELL_PATH_MAX];
    slyz_path_of(path, full);

    void *fh = slyzfs_open(full, "w");
    if (!fh) return -1;

    int written = 0;
    while (written < len) {
        int n = slyzfs_write(fh, (const uint8_t *)text + written, len - written);
        if (n <= 0) break;
        written += n;
    }
    slyzfs_close(fh);

    return written == len ? written : -1;
}

static int slyz_fs_list(struct SlyzVm *vm, const char *path, struct SlyzVal *ret) {
    char full[SHELL_PATH_MAX];
    slyz_path_of(path, full);

    static struct SlyzDirEntry entries[64];
    int n = slyzfs_list_dir(full, entries, 64);
    if (n < 0) n = 0;

    if (!slyz_ret_arr(vm, ret, n)) return -1;

    for (int i = 0; i < n; i++) {
        char name[53];
        int k = 0;
        while (k < 52 && entries[i].name[k]) {
            name[k] = (char)entries[i].name[k];
            k++;
        }
        name[k] = '\0';

        struct SlyzVal item;
        if (!slyz_ret_str(vm, &item, name, k)) return -1;
        slyz_arr_set(vm, ret, i, item);
    }

    return 0;
}

static int slyz_cfg_get(const char *key, char *out, int cap) {
    out[0] = '\0';

    void *fh = slyzfs_open(SYSTEM_CFG_PATH, "r");
    if (!fh) return 0;

    int n = slyzfs_read(fh, slyz_text_buf, (int)sizeof(slyz_text_buf) - 1);
    slyzfs_close(fh);
    if (n <= 0) return 0;
    slyz_text_buf[n] = '\0';

    int key_len = 0;
    while (key[key_len]) key_len++;

    int i = 0;
    while (i < n) {
        int start = i;
        while (i < n && slyz_text_buf[i] != '\n') i++;
        int end = i;
        if (i < n) i++;

        if (end > start && slyz_text_buf[start] == '#') continue;

        int eq = -1;
        for (int k = start; k < end; k++) {
            if (slyz_text_buf[k] == '=') { eq = k; break; }
        }
        if (eq < 0) continue;
        if (eq - start != key_len) continue;

        int same = 1;
        for (int k = 0; k < key_len; k++) {
            if (slyz_text_buf[start + k] != key[k]) { same = 0; break; }
        }
        if (!same) continue;

        int v = eq + 1;
        while (v < end && (slyz_text_buf[v] == ' ' || slyz_text_buf[v] == '\t')) v++;
        while (end > v && (slyz_text_buf[end - 1] == '\r' ||
                           slyz_text_buf[end - 1] == ' ' ||
                           slyz_text_buf[end - 1] == '\t')) {
            end--;
        }

        int len = end - v;
        if (len > cap - 1) len = cap - 1;
        for (int k = 0; k < len; k++) out[k] = (char)slyz_text_buf[v + k];
        out[len] = '\0';
        return 1;
    }

    return 0;
}

static int slyz_cfg_set(const char *key, const char *value) {
    if (cmd_strcmp(key, "boot.mode") == 0) {
        int gui = (cmd_strcmp(value, "gui") == 0) ? 1 : 0;
        return system_cfg_set_boot_gui(gui) ? 0 : -1;
    }
    if (cmd_strcmp(key, "lang") == 0) {
        int id = (cmd_strcmp(value, "zh") == 0) ? 1 : 0;
        return system_cfg_set_lang(id) ? 0 : -1;
    }
    if (cmd_strcmp(key, "timezone") == 0) {
        int hours = 0;
        int neg = 0;
        int i = 0;
        if (value[0] == '-') { neg = 1; i = 1; }
        while (value[i] >= '0' && value[i] <= '9') {
            hours = hours * 10 + (value[i] - '0');
            i++;
        }
        if (neg) hours = -hours;
        return system_cfg_set_timezone(hours) ? 0 : -1;
    }
    return -1;
}

static int slyz_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int64_t slyz_unix_seconds(void) {
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    rtc_get(&y, &mo, &d, &h, &mi, &s);

    int64_t days = 0;
    for (int yy = 1970; yy < y; yy++) {
        days += slyz_is_leap(yy) ? 366 : 365;
    }

    static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    for (int mm = 1; mm < mo; mm++) {
        days += month_days[mm - 1];
        if (mm == 2 && slyz_is_leap(y)) days++;
    }
    days += d - 1;

    return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

static void slyz_put2(char *out, int v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

static int slyz_time_text(char *out, int cap, int with_date) {
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    rtc_get(&y, &mo, &d, &h, &mi, &s);

    int tz = system_cfg_timezone();
    h += tz;
    while (h >= 24) { h -= 24; d++; }
    while (h < 0) { h += 24; d--; }

    if (with_date) {
        if (cap < 11) return 0;
        char tmp[6];
        int yy = y;
        tmp[0] = (char)('0' + (yy / 1000) % 10);
        tmp[1] = (char)('0' + (yy / 100) % 10);
        tmp[2] = (char)('0' + (yy / 10) % 10);
        tmp[3] = (char)('0' + yy % 10);
        out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2]; out[3] = tmp[3];
        out[4] = '-';
        slyz_put2(out + 5, mo);
        out[7] = '-';
        slyz_put2(out + 8, d);
        out[10] = '\0';
        return 10;
    }

    if (cap < 9) return 0;
    slyz_put2(out + 0, h);
    out[2] = ':';
    slyz_put2(out + 3, mi);
    out[5] = ':';
    slyz_put2(out + 6, s);
    out[8] = '\0';
    return 8;
}

/* SLE_SYS_INPUT 的终端实现：打印提示词，读一行可编辑文本。
 * 仅 TTY 会话可用；图形应用走 gui_slyz.c 的空字符串实现。 */
static int slyz_console_read_line(const char *prompt, char *out, int cap) {
    int len = 0;

    if (!out || cap <= 0) return 0;
    if (prompt && *prompt) printk("%s", prompt);

    /* SLYZ 任务第一次被 task_switch 恢复时 IF=0，键盘 IRQ 不会进入。
     * 这里让出一次：切回本任务后 task_yield 会执行 sti。
     * 之后必须忙等轮询，不能持续 yield —— shell 的 task_wait 有
     * 100 万次让出上限，长时间等待会把它提前放回提示符抢键盘。 */
    task_yield();

    for (;;) {
        char c = kb_read_char();
        if (c == 0) {
            tty_tick();
            delay(20000);
            continue;
        }
        if (c == '\r' || c == '\n') {
            printk("\n");
            break;
        }
        if (c == 3) {              /* Ctrl+C：放弃本行输入 */
            printk("^C\n");
            len = 0;
            break;
        }
        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                printk("\b \b");
            }
            continue;
        }
        if ((unsigned char)c >= 32 && (unsigned char)c < 127 && len < cap - 1) {
            out[len++] = c;
            printk("%c", c);
        }
    }

    out[len] = '\0';
    return len;
}

static int slyz_host_sys(struct SlyzVm *vm, uint16_t id,
                         struct SlyzVal *args, uint16_t argc,
                         struct SlyzVal *ret) {
    switch (id) {
        case SLE_SYS_PRINT:
            if (argc >= 1) slyz_print_value(vm, &args[0], 0);
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_PRINTLN:
            if (argc >= 1) slyz_print_value(vm, &args[0], 1);
            else printk("\n");
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EPRINT:
            if (argc >= 1) slyz_print_value(vm, &args[0], 1);
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_FS_READ:
            return slyz_fs_read(vm, slyz_arg_text(args, argc, 0), ret);

        case SLE_SYS_FS_WRITE: {
            const char *path = slyz_arg_text(args, argc, 0);
            int len = 0;
            const char *text = slyz_arg_text(args, argc, 1);
            if (argc >= 2) text = slyz_val_str(&args[1], &len);
            if (!text) { text = ""; len = 0; }
            slyz_ret_int(ret, (int64_t)slyz_fs_write(path, text, len));
            return 0;
        }

        case SLE_SYS_FS_EXISTS: {
            char full[SHELL_PATH_MAX];
            slyz_path_of(slyz_arg_text(args, argc, 0), full);
            struct SlyzDirEntry ent;
            slyz_ret_int(ret, slyzfs_stat(full, &ent) ? 1 : 0);
            return 0;
        }

        case SLE_SYS_FS_LIST:
            return slyz_fs_list(vm, slyz_arg_text(args, argc, 0), ret);

        case SLE_SYS_FS_REMOVE: {
            char full[SHELL_PATH_MAX];
            slyz_path_of(slyz_arg_text(args, argc, 0), full);
            slyz_ret_int(ret, slyzfs_remove(full) ? 0 : -1);
            return 0;
        }

        case SLE_SYS_FS_MKDIR: {
            char full[SHELL_PATH_MAX];
            slyz_path_of(slyz_arg_text(args, argc, 0), full);
            slyz_ret_int(ret, slyzfs_mkdir(full) ? 0 : -1);
            return 0;
        }

        case SLE_SYS_CFG_GET: {
            char value[128];
            if (!slyz_cfg_get(slyz_arg_text(args, argc, 0), value, (int)sizeof(value))) {
                if (!slyz_ret_str(vm, ret, "", 0)) return -1;
            } else {
                int n = 0;
                while (value[n]) n++;
                if (!slyz_ret_str(vm, ret, value, n)) return -1;
            }
            return 0;
        }

        case SLE_SYS_CFG_SET:
            slyz_ret_int(ret, (int64_t)slyz_cfg_set(slyz_arg_text(args, argc, 0),
                                                    slyz_arg_text(args, argc, 1)));
            return 0;

        case SLE_SYS_CFG_KEYS: {
            static const char *keys[3] = { "boot.mode", "lang", "timezone" };
            if (!slyz_ret_arr(vm, ret, 3)) return -1;
            for (int i = 0; i < 3; i++) {
                struct SlyzVal item;
                int n = 0;
                while (keys[i][n]) n++;
                if (!slyz_ret_str(vm, &item, keys[i], n)) return -1;
                slyz_arr_set(vm, ret, i, item);
            }
            return 0;
        }

        case SLE_SYS_TIME_NOW:
            slyz_ret_int(ret, slyz_unix_seconds());
            return 0;

        case SLE_SYS_TIME_MS:
            slyz_ret_int(ret, (int64_t)net_now_ms());
            return 0;

        case SLE_SYS_TIME_TEXT: {
            char buf[16];
            int n = slyz_time_text(buf, (int)sizeof(buf), 0);
            if (!slyz_ret_str(vm, ret, buf, n)) return -1;
            return 0;
        }

        case SLE_SYS_DATE_TEXT: {
            char buf[16];
            int n = slyz_time_text(buf, (int)sizeof(buf), 1);
            if (!slyz_ret_str(vm, ret, buf, n)) return -1;
            return 0;
        }

        case SLE_SYS_FAULT_TRIGGER:
            /* 成功注入不会返回；未知类型返回 -1。 */
            slyz_ret_int(ret, (int64_t)faultlab_trigger((int)slyz_arg_int(args, argc, 0)));
            return 0;

        case SLE_SYS_FAULT_DUMP:
            slyz_ret_int(ret, (int64_t)faultlab_dump_now());
            return 0;

        case SLE_SYS_FAULT_LEAK:
            slyz_ret_int(ret, (int64_t)faultlab_leak((int)slyz_arg_int(args, argc, 0)));
            return 0;

        case SLE_SYS_FAULT_LEAK_FREE:
            slyz_ret_int(ret, (int64_t)faultlab_leak_free());
            return 0;

        case SLE_SYS_FAULT_SET:
            slyz_ret_int(ret, (int64_t)faultlab_set(slyz_arg_text(args, argc, 0),
                                                    slyz_arg_text(args, argc, 1)));
            return 0;

        case SLE_SYS_FAULT_GET: {
            char value[32];
            if (faultlab_get(slyz_arg_text(args, argc, 0), value, (int)sizeof(value)) != 0) {
                if (!slyz_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            int n = 0;
            while (value[n]) n++;
            if (!slyz_ret_str(vm, ret, value, n)) return -1;
            return 0;
        }

        case SLE_SYS_INPUT: {
            char line[256];
            int n = slyz_console_read_line(slyz_arg_text(args, argc, 0),
                                           line, (int)sizeof(line));
            if (!slyz_ret_str(vm, ret, line, n)) return -1;
            return 0;
        }

        case SLE_SYS_SLEEP: {
            int64_t ms = slyz_arg_int(args, argc, 0);
            if (ms < 0) ms = 0;
            uint32_t start = net_now_ms();
            while ((uint32_t)(net_now_ms() - start) < (uint32_t)ms) {
                task_yield();
            }
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_BEEP:
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EXEC: {
            const char *cmd = slyz_arg_text(args, argc, 0);
            int ok = slyz_op_run(cmd);
            slyz_ret_int(ret, ok ? 0 : -1);
            return 0;
        }

        case SLE_SYS_EXIT: {
            int64_t code = slyz_arg_int(args, argc, 0);
            slyzvm_request_exit(vm, (int)code);
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_WIN_OPEN:
        case SLE_SYS_WIN_CLOSE:
        case SLE_SYS_WIN_SIZE:
        case SLE_SYS_WIN_HEIGHT:
        case SLE_SYS_WIN_TITLE:
        case SLE_SYS_GFX_CLEAR:
        case SLE_SYS_GFX_RECT:
        case SLE_SYS_GFX_FRAME:
        case SLE_SYS_GFX_TEXT:
        case SLE_SYS_GFX_TWIDTH:
        case SLE_SYS_GFX_LINE:
        case SLE_SYS_GFX_CIRCLE:
        case SLE_SYS_GFX_FLUSH:
        case SLE_SYS_EVT_POLL:
        case SLE_SYS_EVT_WAIT:
            if (!slyz_gui_warned) {
                slyz_gui_warned = 1;
                printk("slyz: graphics calls need the GUI session (tty has no window)\n");
            }
            slyz_ret_int(ret, -1);
            return 0;

        default:
            slyz_ret_int(ret, -1);
            return 0;
    }
}

static void slyz_op_free(void *ptr) {
    task_free(ptr);
}

static void slyz_op_tick(void) {
    task_account_step();
}

static const struct SlyzVmOps slyz_vm_ops = {
    slyz_host_sys,
    slyz_op_alloc,
    slyz_op_free,
    slyz_op_tick,
    slyz_host_log
};

/* 脚本作为独立任务执行时的镜像。v2 镜像最大约 320KB，静态数组会
 * 撑爆 BSS，因此由任务自行持有并在退出时释放；同一时间只允许
 * 一个 SLYZ 任务运行，避免镜像引用互相覆盖。 */
static uint8_t *slyz_task_image = 0;
static int slyz_task_size = 0;
static char slyz_task_name[SHELL_PATH_MAX];
static int slyz_run_background = 0;

static void slyz_task_entry(void) {
    struct Task *t = task_current();
    if (!t) {
        if (slyz_task_image) { kfree(slyz_task_image); slyz_task_image = 0; }
        task_exit(-1);
        return;
    }

    slyz_depth++;
    int rc = slyzvm_run(slyz_task_image, slyz_task_size, &slyz_vm_ops, slyz_task_name);
    slyz_depth--;

    /* 镜像所有权归任务：无论成功失败都在这里归还堆，
     * 前台 task_wait 之后不再触碰 slyz_task_image。 */
    if (slyz_task_image) {
        kfree(slyz_task_image);
        slyz_task_image = 0;
    }
    slyz_task_size = 0;

    task_exit(rc ? 0 : 1);
}

static int try_run_slyz_by_name(const char *name) {
    if (!name || *name == '\0') return 0;
    if (slyz_depth >= 2) {
        printk("run: script nesting too deep\n");
        return 0;
    }
    if (slyz_task_image) {
        printk("run: another SLYZ program is still running\n");
        return 2;
    }

    /* 先按用户给的名字找，找不到再补 .slyz 后缀。 */
    char path[SHELL_PATH_MAX];
    uint8_t *image = 0;
    int got = 0;

    for (int attempt = 0; attempt < 4 && !got; attempt++) {
        if (attempt == 0) {
            shell_resolve_path(path, shell_cwd, name);
        } else if (attempt == 1) {
            char with_ext[SHELL_PATH_MAX];
            int pos = 0;
            while (name[pos] && pos < SHELL_PATH_MAX - 8) {
                with_ext[pos] = name[pos];
                pos++;
            }
            with_ext[pos++] = '.';
            with_ext[pos++] = 's';
            with_ext[pos++] = 'l';
            with_ext[pos++] = 'y';
            with_ext[pos++] = 'z';
            with_ext[pos] = '\0';
            shell_resolve_path(path, shell_cwd, with_ext);
        } else {
            /* PATH 语义：当前目录找不到时再到 system\apps 下找。
             * 这样 `run slyzfs` 与 `run system\apps\slyzfs.slyz` 等价。 */
            static const char *prefix = "system\\apps\\";
            int pos = 0;
            for (int i = 0; prefix[i] && pos < SHELL_PATH_MAX - 1; i++) {
                path[pos++] = prefix[i];
            }
            for (int i = 0; name[i] && pos < SHELL_PATH_MAX - 1; i++) {
                path[pos++] = name[i];
            }
            if (attempt == 3) {
                const char *ext = ".slyz";
                for (int i = 0; ext[i] && pos < SHELL_PATH_MAX - 1; i++) {
                    path[pos++] = ext[i];
                }
            }
            path[pos] = '\0';
        }

        struct SlyzDirEntry ent;
        if (!slyzfs_stat(path, &ent)) continue;
        if (ent.type == SLYZFS_DIR_DIR || ent.size == 0) continue;
        if (ent.size > SLYZ_MAX_FILE) {
            printk("run: image is too large (max %u bytes)\n", (unsigned)SLYZ_MAX_FILE);
            return 2;
        }

        uint8_t *buf = (uint8_t *)kmalloc(ent.size + 1);
        if (!buf) {
            printk("run: out of memory for image\n");
            return 2;
        }

        void *fh = slyzfs_open(path, "r");
        if (!fh) {
            kfree(buf);
            continue;
        }

        int total = 0;
        while (total < (int)ent.size) {
            int n = slyzfs_read(fh, buf + total, (int)ent.size - total);
            if (n <= 0) break;
            total += n;
        }
        slyzfs_close(fh);

        if (total <= 0) {
            kfree(buf);
            continue;
        }

        buf[total] = 0;
        image = buf;
        got = total;
    }

    if (!got || !image) {
        return 0;
    }

    /* SLYZ v3（SLYZ3BIN）：声明式 GUI 程序必须由图形会话创建窗口。
     * 旧路径只认 v2 魔数，v3 镜像会被当成文本脚本执行，结果既没有
     * 窗口也没有可读输出；这里显式分派到 GUI 宿主。 */
    if (slyz3_is_binary(image, got)) {
        if (gui_keyboard_owned()) {
            int win = gui_open_file_path(path);
            kfree(image);
            if (win < 0) {
                printk("run: cannot open SLYZ v3 window\n");
            }
            /* 窗口已交给 GUI 宿主，不再走 TTY 任务/异常提示路径。 */
            return 1;
        }
        /* 终端会话：没有声明式 GUI 的程序（文件系统工具、批处理）
         * 交给 v3 终端宿主同步执行；需要窗口的镜像会被明确拒绝。 */
        int rc = slyz3_tty_run(image, got, path);
        kfree(image);
        return rc ? 1 : 2;
    }

    /* 二进制镜像：交给字节码虚拟机执行 */
    if (slyzvm_is_binary(image, got)) {
        struct Task *cur = task_current();

        /* 已经在脚本任务里：直接在当前任务中执行。
         * 此时内存申请仍走该任务的配额。 */
        if (cur && cur->id != 0) {
            slyz_depth++;
            int rc = slyzvm_run(image, got, &slyz_vm_ops, path);
            slyz_depth--;
            kfree(image);
            return rc ? 1 : 2;
        }

        /* shell 任务：把脚本放进独立任务，便于配额记账与 OOM 回收。
         * 镜像所有权交给任务，任务退出时释放。 */
        for (int i = 1; i < TASK_MAX; i++) {
            struct Task *other = task_by_id(i);
            if (other && (other->state == TASK_READY || other->state == TASK_RUNNING)) {
                printk("run: task slot %d busy (%s)\n", i, other->name);
                kfree(image);
                return 2;
            }
        }

        slyz_task_image = image;
        slyz_task_size = got;
        cmd_strncpy(slyz_task_name, path, sizeof(slyz_task_name));

        int tid = task_spawn(path, slyz_task_entry, TASK_QUOTA_DEFAULT);
        if (tid < 0) {
            printk("run: no free task slot\n");
            slyz_task_image = 0;
            slyz_task_size = 0;
            kfree(image);
            return 2;
        }

        if (slyz_run_background) {
            printk("[bg] pid %d: %s\n", tid, path);
            return 1;
        }

        task_wait(tid);

        struct Task *t = task_by_id(tid);
        int rc = 2;
        if (t) {
            rc = (t->exit_code == 0) ? 1 : 2;
            if (t->state == TASK_DEAD) t->state = TASK_FREE;
        }
        return rc;
    }

    /* 文本脚本：保留对旧 .slyz 的兼容 */
    char *script = (char *)image;
    script[got] = '\0';

    char *lines[64];
    int line_count = 0;
    char *p = script;
    while (*p && line_count < 64) {
        while (*p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;

        lines[line_count++] = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p == '\r') *p++ = '\0';
        if (*p == '\n') *p++ = '\0';
    }

    int parse_block(int start, int end) {
        for (int i = start; i < end; i++) {
            char *trim = strip_leading_ws(lines[i]);
            if (*trim == '\0') continue;
            if (cmd_strcmp(trim, "endif") == 0) continue;

            if (cmd_strncmp(trim, "if ", 3) == 0) {
                char *cond = trim + 3;
                while (*cond == ' ' || *cond == '\t') cond++;
                int expect = (cmd_strcmp(cond, "yes") == 0);
                if (cmd_strcmp(cond, "no") == 0) expect = 0;

                int current_indent = leading_indent(lines[i]);
                int end_idx = find_matching_endif(lines, line_count, i, current_indent);
                if (end_idx < 0) {
                    printk("%s: unterminated if block\n", name);
                    return 0;
                }

                int active = (slyz_last_answer && expect) || (!slyz_last_answer && !expect);
                if (active && !parse_block(i + 1, end_idx)) {
                    return 0;
                }
                i = end_idx;
                continue;
            }

            char *cmd = trim;
            while (*cmd && *cmd != ' ' && *cmd != '\t') cmd++;
            if (*cmd) {
                *cmd++ = '\0';
                while (*cmd == ' ' || *cmd == '\t') cmd++;
            }

            if (cmd_strcmp(trim, "ask") == 0) {
                cmd_ask(cmd);
                continue;
            }

            if (!exec_cmd(trim, cmd)) {
                printk("%s: invalid script command: %s\n", name, trim);
                return 0;
            }
        }
        return 1;
    }

    int parsed = parse_block(0, line_count) ? 1 : 2;
    kfree(image);
    return parsed;
}

static void cmd_help(void)  { for (int i = 0; help_text[i]; i++) printk("%s\n", help_text[i]); }
static void cmd_min(void)  { printk("help clear echo dir ls pwd cd dd mkdir rmdir rm del df nano vizcormin cat run bg kill gui ps smp version uname whoami date mem gpu vm swap pfa protect reboot poweroff acpi powercfg bootmode useradd passwd su logout chmod chown ip curl ping nslookup\n"); }
static void cmd_clear(void){ tty_clear(); }
static void cmd_echo(const char *args)  { printk("%s\n", args); }

/* ============================================================
 * dd —— 统一的裸扇区 / 文本读写命令
 *
 * 原 read / write / writet / readt 四条命令合并于此，避免多个
 * 语义重叠的裸扇区出入口。语法对齐 Unix dd：
 *   dd if=<src> of=<dst> [bs=<n>] [count=<n>] [seek=<n>] [skip=<n>] [quiet]
 *
 *   <src> / <dst> 取值：
 *     sector:<N>   第 N 个扇区（LBA 从 0 起）
 *     file:<path>  SlyzFS 里的普通文件
 *     text:<str>   字面文本（仅 if=）
 *     zero         全零填充（仅 if=）
 *     stdout       屏幕 hexdump（仅 of=）
 *
 *   bs     每块字节数，默认 512，必须为 512 的倍数，上限 4096
 *   count  块数，默认 1
 *   skip   if=sector: 时的起始块偏移
 *   seek   of=sector: 时的起始块偏移
 *
 * 示例：
 *   dd if=sector:2048 of=stdout               查看超级块
 *   dd if=text:hello of=sector:4096           把文本写入扇区
 *   dd if=zero of=sector:4096 count=8         清零 8 个扇区
 *   dd if=sector:4096 of=sector:8192 count=2  扇区拷贝
 * ============================================================ */

#define DD_BUF_SIZE 4096u

enum {
    DD_SPEC_SECTOR = 0,
    DD_SPEC_TEXT,
    DD_SPEC_ZERO,
    DD_SPEC_STDOUT,
    DD_SPEC_FILE
};

struct dd_spec {
    int         kind;
    uint32_t    sector;
    const char *text;
};

static int dd_parse_spec(const char *s, struct dd_spec *out) {
    if (!s || !*s) return 0;
    if (cmd_strncmp(s, "sector:", 7) == 0) {
        out->kind = DD_SPEC_SECTOR;
        out->text = 0;
        return parse_u32(s + 7, &out->sector) ? 1 : 0;
    }
    if (cmd_strncmp(s, "text:", 5) == 0) {
        out->kind = DD_SPEC_TEXT;
        out->sector = 0;
        out->text = s + 5;
        return 1;
    }
    if (cmd_strncmp(s, "file:", 5) == 0) {
        out->kind = DD_SPEC_FILE;
        out->sector = 0;
        out->text = s + 5;
        return out->text[0] != '\0';
    }
    if (cmd_strcmp(s, "zero") == 0) {
        out->kind = DD_SPEC_ZERO;
        out->sector = 0;
        out->text = 0;
        return 1;
    }
    if (cmd_strcmp(s, "stdout") == 0) {
        out->kind = DD_SPEC_STDOUT;
        out->sector = 0;
        out->text = 0;
        return 1;
    }
    return 0;
}

static void dd_hexdump(const uint8_t *buf, uint32_t n, uint32_t base) {
    static const char hexc[] = "0123456789abcdef";
    char line[96];
    for (uint32_t off = 0; off < n; off += 16u) {
        int pos = 0;
        uint32_t addr = base + off;
        for (int sh = 28; sh >= 0; sh -= 4) line[pos++] = hexc[(addr >> sh) & 0xFu];
        line[pos++] = ' ';
        line[pos++] = ' ';
        for (int i = 0; i < 16; i++) {
            if (off + (uint32_t)i < n) {
                uint8_t v = buf[off + (uint32_t)i];
                line[pos++] = hexc[v >> 4];
                line[pos++] = hexc[v & 0xFu];
            } else {
                line[pos++] = ' ';
                line[pos++] = ' ';
            }
            line[pos++] = ' ';
            if (i == 7) line[pos++] = ' ';
        }
        line[pos++] = '|';
        for (int i = 0; i < 16 && off + (uint32_t)i < n; i++) {
            uint8_t v = buf[off + (uint32_t)i];
            line[pos++] = (v >= 0x20u && v <= 0x7Eu) ? (char)v : '.';
        }
        line[pos++] = '|';
        line[pos] = '\0';
        printk("%s\n", line);
    }
}

static void dd_usage(void) {
    printk("usage: dd if=<src> of=<dst> [bs=<n>] [count=<n>] [seek=<n>] [skip=<n>] [quiet]\n");
    printk("  src/dst: sector:<N> | text:<str> | file:<path> | zero | stdout\n");
    printk("  example: dd if=sector:2048 of=stdout\n");
    printk("  example: dd if=sector:16384 of=file:demo.bin count=16\n");
    printk("  example: dd if=file:demo.bin of=sector:16384\n");
    printk("  example: dd if=zero of=sector:4096 count=8\n");
}

static void cmd_dd(char *args) {
    char *src_s = 0;
    char *dst_s = 0;
    uint32_t bs = 512u;
    uint32_t count = 1u;
    uint32_t seek = 0u;
    uint32_t skip = 0u;
    int quiet = 0;

    if (!args || !*args) { dd_usage(); return; }

    char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';

        if (cmd_strncmp(tok, "if=", 3) == 0) {
            src_s = tok + 3;
        } else if (cmd_strncmp(tok, "of=", 3) == 0) {
            dst_s = tok + 3;
        } else if (cmd_strncmp(tok, "bs=", 3) == 0) {
            if (!parse_u32(tok + 3, &bs)) { printk("dd: invalid bs\n"); return; }
        } else if (cmd_strncmp(tok, "count=", 6) == 0) {
            if (!parse_u32(tok + 6, &count)) { printk("dd: invalid count\n"); return; }
        } else if (cmd_strncmp(tok, "seek=", 5) == 0) {
            if (!parse_u32(tok + 5, &seek)) { printk("dd: invalid seek\n"); return; }
        } else if (cmd_strncmp(tok, "skip=", 5) == 0) {
            if (!parse_u32(tok + 5, &skip)) { printk("dd: invalid skip\n"); return; }
        } else if (cmd_strcmp(tok, "quiet") == 0) {
            quiet = 1;
        } else {
            printk("dd: unknown option '%s'\n", tok);
            return;
        }
    }

    if (!src_s || !*src_s || !dst_s || !*dst_s) { dd_usage(); return; }

    struct dd_spec src;
    struct dd_spec dst;
    if (!dd_parse_spec(src_s, &src)) { printk("dd: invalid if= spec '%s'\n", src_s); return; }
    if (!dd_parse_spec(dst_s, &dst)) { printk("dd: invalid of= spec '%s'\n", dst_s); return; }

    if (src.kind == DD_SPEC_STDOUT) { printk("dd: if= cannot be stdout\n"); return; }
    if (dst.kind == DD_SPEC_TEXT || dst.kind == DD_SPEC_ZERO) {
        printk("dd: of= must be sector:<N>, file:<path> or stdout\n");
        return;
    }
    if (bs == 0u || (bs % 512u) != 0u) {
        printk("dd: bs must be a non-zero multiple of 512\n");
        return;
    }
    if (bs > DD_BUF_SIZE) {
        printk("dd: bs too large (max %u)\n", (unsigned)DD_BUF_SIZE);
        return;
    }
    if (count == 0u) count = 1u;

    uint32_t sectors_per_block = bs / 512u;
    uint32_t blocks_per_chunk = DD_BUF_SIZE / bs;
    if (blocks_per_chunk == 0u) blocks_per_chunk = 1u;

    uint8_t buf[DD_BUF_SIZE];
    uint32_t done = 0;
    uint32_t total = 0;
    int text_consumed = 0;

    /* 文件源/目标是 SlyzFS 里的普通文件，句柄要在整个分块循环期间保持打开 */
    void *src_fh = 0;
    void *dst_fh = 0;
    char src_path[SHELL_PATH_MAX];
    char dst_path[SHELL_PATH_MAX];

    if (src.kind == DD_SPEC_FILE) {
        shell_resolve_path(src_path, shell_cwd, src.text);
        src_fh = slyzfs_open(src_path, "r");
        if (!src_fh) {
            printk("dd: cannot open %s\n", src_path);
            return;
        }
    }
    if (dst.kind == DD_SPEC_FILE) {
        shell_resolve_path(dst_path, shell_cwd, dst.text);
        if (dst_path[0] == '\0') {
            printk("dd: invalid output path\n");
            if (src_fh) slyzfs_close(src_fh);
            return;
        }
        dst_fh = slyzfs_open(dst_path, "w");
        if (!dst_fh) {
            printk("dd: cannot create %s\n", dst_path);
            if (src_fh) slyzfs_close(src_fh);
            return;
        }
    }

    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > blocks_per_chunk) chunk = blocks_per_chunk;
        uint32_t bytes = chunk * bs;
        uint32_t actual = bytes;

        /* ---- 填充缓冲区 ---- */
        if (src.kind == DD_SPEC_FILE) {
            int got = slyzfs_read(src_fh, buf, (int)bytes);
            if (got <= 0) break;               /* 读到文件尾 */
            actual = (uint32_t)got;
            for (uint32_t i = actual; i < bytes; i++) buf[i] = 0;
        } else if (src.kind == DD_SPEC_ZERO) {
            for (uint32_t i = 0; i < bytes; i++) buf[i] = 0;
        } else if (src.kind == DD_SPEC_TEXT) {
            uint32_t len = 0;
            if (!text_consumed) {
                while (src.text[len]) len++;
            }
            if (len > bytes) len = bytes;
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)src.text[i];
            for (uint32_t i = len; i < bytes; i++) buf[i] = 0;
            text_consumed = 1;
        } else {
            for (uint32_t b = 0; b < chunk; b++) {
                for (uint32_t s = 0; s < sectors_per_block; s++) {
                    uint32_t lba = src.sector + (skip + done + b) * sectors_per_block + s;
                    if (lba >= DISK_MAX_SECTORS) {
                        printk("dd: if sector %u out of range\n", (unsigned)lba);
                        return;
                    }
                    if (!blk_read(lba, buf + b * bs + s * 512u)) {
                        printk("dd: read failed at sector %u\n", (unsigned)lba);
                        return;
                    }
                }
            }
        }

        /* ---- 写出 ---- */
        if (dst.kind == DD_SPEC_STDOUT) {
            uint32_t base_addr = (src.kind == DD_SPEC_SECTOR)
                ? (src.sector + skip + done) * bs
                : done * bs;
            dd_hexdump(buf, actual, base_addr);
        } else if (dst.kind == DD_SPEC_FILE) {
            int n = slyzfs_write(dst_fh, buf, (int)actual);
            if (n != (int)actual) {
                printk("dd: file write failed\n");
                break;
            }
        } else {
            for (uint32_t b = 0; b < chunk; b++) {
                for (uint32_t s = 0; s < sectors_per_block; s++) {
                    uint32_t lba = dst.sector + (seek + done + b) * sectors_per_block + s;
                    if (lba >= DISK_MAX_SECTORS) {
                        printk("dd: of sector %u out of range\n", (unsigned)lba);
                        if (src_fh) slyzfs_close(src_fh);
                        if (dst_fh) slyzfs_close(dst_fh);
                        return;
                    }
                    if (!blk_write(lba, buf + b * bs + s * 512u)) {
                        printk("dd: write failed at sector %u\n", (unsigned)lba);
                        if (src_fh) slyzfs_close(src_fh);
                        if (dst_fh) slyzfs_close(dst_fh);
                        return;
                    }
                }
            }
        }

        total += actual;
        done += chunk;
    }

    if (src_fh) slyzfs_close(src_fh);
    if (dst_fh) slyzfs_close(dst_fh);

    if (!quiet) {
        printk("dd: %u bytes (%u block(s) x %u) done\n",
               (unsigned)total, (unsigned)count, (unsigned)bs);
    }
}

static void cmd_remove(char *args) {
    char *start_s = 0;
    char *end_s = 0;
    if (!args || !*args) { printk("usage: remove <start> <end>\n"); return; }

    split_first_arg(args, &start_s, &end_s);
    if (!*start_s || !*end_s) { printk("usage: remove <start> <end>\n"); return; }

    uint32_t start = 0, end = 0;
    if (!parse_u32(start_s, &start) || !parse_u32(end_s, &end)) {
        printk("remove: invalid sector range\n");
        return;
    }
    if (start >= DISK_MAX_SECTORS || end >= DISK_MAX_SECTORS) {
        printk("remove: sector out of range [0..%u]\n", DISK_MAX_SECTORS - 1u);
        return;
    }
    if (end < start) {
        printk("remove: end must be >= start\n");
        return;
    }

    for (uint32_t i = start; i <= end; i++) {
        uint8_t block[512];
        for (int j = 0; j < 512; j++) block[j] = 0;
        if (!blk_write(i, block)) {
            printk("remove: write failed at sector %u\n", (unsigned int)i);
            return;
        }
    }

    printk("remove: cleared sectors %u..%u\n", (unsigned int)start, (unsigned int)end);
}

#ifdef SAYLTER_VBE
static void shell_enter_gui(void) {
    uint32_t t0 = net_now_ms();
    vbe_init();
    kserial_puts("[PERF] vbe_init +");
    kserial_dec32(net_now_ms() - t0);
    kserial_puts("ms\r\n");
    if (!vbe_is_enabled()) {
        printk("[GUI] VBE not available.\n");
        return;
    }

    gui_main();

    /* 返回 TTY：图形像素不能残留在终端区域，重新初始化后清屏 */
    vbe_init();
    tty_clear();
    printk("[GUI] returned to shell\n");
}
#endif
static void cmd_version(void) { printk("SaylterOS v0.3 Beta2 (TTY shell)\n"); }
static void cmd_uname(void)   { printk("SaylterOS x86_64 64-bit long mode\n"); }

/* 中文与输入法自检：一次跑完 UTF-8 输出、字库覆盖与候选数据三个环节，
 * 装机现场不需要输入法也能确认中文链路是否正常。 */
static void cmd_cjk(void) {
    printk("SaylterOS 中文自检\n");
    printk("----------------------------------------\n");
    printk("常用汉字：你好世界，中文输入法，操作系统内核\n");
    printk("中文标点：，。？！：；、“”《》【】\n");
    printk("全角字符：ＡＢＣ　１２３（全角）\n");
    printk("中英混排：SaylterOS 是一个 64 位操作系统，原生支持 UTF-8。\n");
    printk("输入法：Ctrl+Space 切换，空格或 1-9 选字，- 与 = 翻页。\n");
    printk("音节表：%d 个音节，%d 个候选字。\n",
           (int)IME_SYLLABLE_COUNT, (int)IME_POOL_COUNT);
    printk("当前模式：%s\n", ime_chinese_enabled() ? "中文" : "英文");
}
static void cmd_ime(const char *args) {
    while (args && (*args == ' ' || *args == '\t')) args++;

    if (!args || *args == '\0') {
        printk("ime: %s\n", ime_chinese_enabled() ? "chinese" : "english");
        return;
    }
    if (cmd_strcmp(args, "on") == 0 || cmd_strcmp(args, "zh") == 0 ||
        cmd_strcmp(args, "1") == 0) {
        ime_set_chinese(1);
    } else if (cmd_strcmp(args, "off") == 0 || cmd_strcmp(args, "en") == 0 ||
               cmd_strcmp(args, "0") == 0) {
        ime_set_chinese(0);
    } else if (cmd_strcmp(args, "toggle") == 0) {
        ime_toggle_chinese();
    } else {
        printk("usage: ime [on|off|toggle]\n");
        return;
    }
    printk("ime: %s\n", ime_chinese_enabled() ? "chinese" : "english");
}

static void cmd_whoami(void) {
    const struct AuthUser *u = auth_current();
    if (u) {
        printk("%s (uid %u)\n", u->name, (unsigned int)u->uid);
    } else {
        printk("root (uid 0)\n");
    }
}
static void cmd_date(void) {
    int y, mo, d, h, mi, s;
    rtc_get(&y, &mo, &d, &h, &mi, &s);
    char Y[5], MO[3], D[3], H[3], MI[3], S[3];
    Y[0]='0'+(y/1000); Y[1]='0'+((y/100)%10);
    Y[2]='0'+((y/10)%10); Y[3]='0'+(y%10); Y[4]='\0';
    pad2(MO, mo); pad2(D, d); pad2(H, h); pad2(MI, mi); pad2(S, s);
    printk("%s-%s-%s %s:%s:%s UTC\n", Y, MO, D, H, MI, S);
}
static void cmd_mem(void) {
    uint32_t total = 0, used = 0, peak = 0, allocs = 0, frees = 0, failed = 0;
    mem_stats(&total, &used, &peak, &allocs, &frees, &failed);

    printk("kernel heap @ 0x%x  total %u bytes\n",
           mem_heap_base(), (unsigned int)total);
    printk("  used   : %u\n", (unsigned int)used);
    printk("  free   : %u\n", (unsigned int)mem_free_bytes());
    printk("  peak   : %u\n", (unsigned int)peak);
    printk("  allocs : %u   frees: %u   failed: %u\n",
           (unsigned int)allocs, (unsigned int)frees, (unsigned int)failed);
    printk("  tasks  : %u alive, %u bytes accounted\n",
           (unsigned int)task_count_alive(), (unsigned int)task_total_used());
}

/* ---- GPU 状态 ----
 * 真机调试用：确认探测到的设备、GGTT 地址、自测结果和累计搬运量。
 * 状态为 fallback 时全部渲染走 CPU，功能不受影响。 */
static void cmd_gpu(void) {
    gpu_info_t gi;
    gpu_get_info(&gi);

    if (!gi.present) {
        printk("GPU: no supported Intel display device (CPU rendering)\n");
        return;
    }

    printk("GPU: %s  %02x:%02x.%x  %04x:%04x\n",
           gpu_type_name(), (unsigned int)gi.bus, (unsigned int)gi.slot,
           (unsigned int)gi.func, (unsigned int)gi.vendor,
           (unsigned int)gi.device);
    printk("  state     : %s\n",
           gi.ready ? "blitter acceleration" : "CPU fallback");
    printk("  mmio      : 0x%x   lfb: 0x%x (gpu va 0x%x)\n",
           gi.mmio_phys, gi.lfb_phys, gi.lfb_gpu_va);
    printk("  backbuffer: %u pages @ gpu va 0x%x\n",
           gi.bb_pages, gi.bb_gpu_va);
    printk("  ring      : %u KB @ gpu va 0x%x\n",
           gi.ring_size / 1024u, gi.ring_gpu_va);
    printk("  cache     : mtrr=%d pat=%d wc=%d\n",
           gi.mtrr_type, gi.pat_ok, gi.wc_enabled);
    printk("  traffic   : %u blits, %u bytes, %u timeouts\n",
           gi.blits, gi.bytes_copied, gi.timeouts);
}

/* ---- 虚拟内存 / 分页 / swap ----
 * vm        概览：帧池、按需区、驻留配额、缺页与换页计数
 * vm swap   只看 swap 文件与槽位
 * vm limit  调整驻留页配额（页数），用于观察不同内存压力下的换出行为
 * pfa       自测：逐页写入并校验，强制走完缺页 + 换出 + 换入全链路
 */
static void cmd_vm(const char *args) {
    if (!args || !*args) {
        vm_dump();
        return;
    }

    char work[64];
    cmd_strncpy(work, args, sizeof(work));

    char *p = work;
    while (*p == ' ' || *p == '\t') p++;
    char *tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;

    if (cmd_strcmp(tok, "swap") == 0) {
        vm_dump_swap();
        return;
    }

    if (cmd_strcmp(tok, "limit") == 0) {
        uint32_t pages = 0;
        if (!parse_u32(p, &pages)) {
            printk("vm: usage: vm limit <16..4096>\n");
            return;
        }
        if (vm_set_resident_limit(pages)) {
            printk("vm: resident limit = %u pages (%u KB)\n",
                   (unsigned int)pages, (unsigned int)(pages * 4u));
        } else {
            printk("vm: limit must be 16..4096 pages\n");
        }
        return;
    }

    printk("vm: usage: vm | vm swap | vm limit <pages>\n");
}

static void cmd_swap(void) {
    vm_dump_swap();
}

static void cmd_pfa(const char *args) {
    (void)args;
    printk("pfa: starting demand paging self-test\n");
    if (vm_selftest()) {
        printk("pfa: self-test finished successfully\n");
    } else {
        printk("pfa: self-test FAILED\n");
    }
}

/* ---- 受保护文件 ----
 * protect <path>                 查询
 * protect on <path>              设置（root）
 * protect off <path> --force     解除（root + 显式 --force）
 * 保护位由文件系统层强制执行，shell 只负责解析参数与提示。 */
static void cmd_protect(const char *args) {
    char work[256];
    cmd_strncpy(work, (args && *args) ? args : "", sizeof(work));

    char *p = work;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) {
        printk("usage: protect <path> | protect on <path> | protect off <path> --force\n");
        return;
    }

    char *tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;

    int set_on = -1;
    if (cmd_strcmp(tok, "on") == 0) {
        set_on = 1;
    } else if (cmd_strcmp(tok, "off") == 0) {
        set_on = 0;
    }

    char path[SHELL_PATH_MAX];

    if (set_on >= 0) {
        if (!*p) {
            printk("protect: missing path\n");
            return;
        }

        int force = 0;
        if (set_on == 0) {
            /* 在路径里查找 "--force" 标记，找到后截断并置位 */
            char *q = p;
            while (*q) {
                if (q[0] == ' ' && q[1] == '-' && q[2] == '-' &&
                    q[3] == 'f' && q[4] == 'o' && q[5] == 'r' &&
                    q[6] == 'c' && q[7] == 'e' &&
                    (q[8] == '\0' || q[8] == ' ' || q[8] == '\t')) {
                    *q = '\0';
                    force = 1;
                    break;
                }
                q++;
            }
        }

        if (slyzfs_get_uid() != 0) {
            printk("protect: root only\n");
            return;
        }

        shell_resolve_path(path, shell_cwd, p);
        if (path[0] == '\0') {
            printk("protect: invalid path\n");
            return;
        }

        if (set_on) {
            if (slyzfs_set_protected(path, 1)) {
                printk("protect: %s is now protected (root cannot delete)\n", path);
            } else {
                printk("protect: cannot protect '%s' (not found or not a file)\n", path);
            }
            return;
        }

        if (!force) {
            printk("protect: removing protection requires an explicit override\n");
            printk("         protect off <path> --force\n");
            return;
        }
        if (slyzfs_set_protected(path, 0)) {
            printk("protect: %s is no longer protected\n", path);
        } else {
            printk("protect: cannot unprotect '%s'\n", path);
        }
        return;
    }

    /* 单参数：查询状态 */
    shell_resolve_path(path, shell_cwd, tok);
    if (path[0] == '\0') {
        printk("protect: invalid path\n");
        return;
    }

    struct SlyzDirEntry ent;
    if (!slyzfs_stat(path, &ent)) {
        printk("protect: '%s' not found\n", path);
        return;
    }
    if (ent.type != SLYZFS_DIR_FILE) {
        printk("protect: '%s' is a directory\n", path);
        return;
    }
    printk("%s: %s\n", path,
           slyzfs_is_protected(path) ? "protected (root cannot delete)" : "not protected");
}
/* 真实硬件复位：走 8042 键盘控制器的 0xFE 命令拉 CPU RESET#。
 * 旧实现只是 cli;hlt，等于关机，必须手动断电才能恢复。 */
static void cmd_reboot(void)  {
    printk("Rebooting (8042 reset line)...\n");
    reboot();
}

/* ---- ACPI S5 关机 ----
 * 只写 SLP_TYP|SLP_EN 到 PM1_CNT，不解析 DSDT 的 \_S5 包，
 * 因此默认 SLP_TYP=5（QEMU/Bochs 与多数 x86 平台）；
 * 特殊主板可用 `poweroff <0..7>` 现场试出正确值。 */
static void cmd_poweroff(const char *args) {
    uint8_t slp_typ = acpi_get_s5_slp_typ();

    if (!acpi_available() && !acpica_ready()) {
        printk("poweroff: ACPI not available\n");
        printk("Halting - power off manually\n");
        while (1) __asm__ volatile ("cli; hlt");
    }

    if (args && *args) {
        uint32_t v;
        if (!parse_u32(args, &v) || v > 7) {
            printk("poweroff: slp_typ must be 0..7\n");
            return;
        }
        slp_typ = (uint8_t)v;
        acpi_set_s5_slp_typ(slp_typ);
        printk("Shutting down (ACPI S5, SLP_TYP=%u)...\n", (unsigned int)slp_typ);
        acpi_power_off_with(slp_typ);
    } else {
        /* 首选 ACPICA 路径：\_S5 + _PTS，固件期望的顺序 */
        if (acpica_ready()) {
            printk("Shutting down (ACPI S5 via \\_S5, SLP_TYP=%u)...\n",
                   (unsigned int)slp_typ);
            acpica_request_s5();
            printk("poweroff: ACPICA S5 did not take effect, falling back to PM1 write\n");
        }
        printk("Shutting down (ACPI S5, SLP_TYP=%u then fallback %u)...\n",
               (unsigned int)slp_typ,
               (unsigned int)((slp_typ == 0) ? 5 : 0));
        acpi_power_off();
    }

    printk("poweroff: S5 request did not take effect\n");
    printk("Halting - power off manually\n");
    while (1) __asm__ volatile ("cli; hlt");
}

static void cmd_acpi(void) {
    acpi_dump();
    acpica_dump();
}

/* ---- powercfg：仿 Windows 的电源状态管理 ----
 *   powercfg /a              列出 S0..S5 的固件 / OS 支持情况
 *   powercfg /sleep <1..5>   进入 S<n>（/s 为简写）
 *   powercfg /lastwake       上次唤醒源与进入次数
 *   powercfg /devices        电源按钮与 SCI 接线状态
 *   powercfg /h on|off       休眠（S4）开关：当前无休眠镜像
 * S2 按用户要求保留，但会明确提示 ACPI 规范已废弃该状态。 */
static const char *powercfg_state_name(uint8_t s) {
    switch (s) {
        case 0: return "S0  Working";
        case 1: return "S1  Standby (CPU clock stopped)";
        case 2: return "S2  Standby (deprecated)";
        case 3: return "S3  Suspend to RAM";
        case 4: return "S4  Hibernate (disk image)";
        case 5: return "S5  Soft off";
        default: return "?";
    }
}

static void powercfg_usage(void) {
    printk("usage: powercfg /a | /sleep <1..5> | /lastwake | /devices | /h on|off\n");
}

static void cmd_powercfg(const char *args) {
    char opt[16];
    const char *p = args;
    int i = 0;

    while (p && *p == ' ') p++;
    if (!p || !*p) {
        powercfg_usage();
        return;
    }

    while (*p && *p != ' ' && i < (int)sizeof(opt) - 1) {
        opt[i++] = *p++;
    }
    opt[i] = '\0';
    while (*p == ' ') p++;

    if (cmd_strcmp(opt, "/a") == 0 || cmd_strcmp(opt, "/query") == 0 ||
        cmd_strcmp(opt, "/avail") == 0) {
        if (!acpica_ready()) {
            printk("powercfg: ACPICA not initialized\n");
            return;
        }

        uint32_t fw = acpica_firmware_sleep_mask();
        uint32_t os = acpica_os_sleep_mask();

        printk("Sleep states (S0-S5)\n\n");
        for (uint8_t s = 0; s <= 5; s++) {
            uint8_t f = (uint8_t)((fw >> s) & 1u);
            uint8_t o = (uint8_t)((os >> s) & 1u);
            printk("  %s  firmware:%s  os:%s%s\n",
                   powercfg_state_name(s),
                   f ? "yes" : "no ",
                   o ? "yes" : "no ",
                   (s == 2) ? "  [deprecated]" : "");
        }

        printk("\nUnavailable sleep states:\n");
        if (!(fw & (1u << 1))) printk("  S1: firmware does not define \\_S1\n");
        if (!(fw & (1u << 3))) printk("  S3: firmware does not define \\_S3\n");
        if (!(os & (1u << 4))) printk("  S4: no hibernate image support\n");
        printk("  S2: available here, but deprecated by the ACPI specification\n");
        return;
    }

    if (cmd_strcmp(opt, "/sleep") == 0 || cmd_strcmp(opt, "/s") == 0 ||
        cmd_strcmp(opt, "sleep") == 0) {
        uint32_t v;

        if (!acpica_ready()) {
            printk("powercfg: ACPICA not initialized\n");
            return;
        }
        if (!parse_u32(p, &v) || v > 5) {
            printk("powercfg: sleep state must be 1..5\n");
            return;
        }
        if (v == 0) {
            printk("powercfg: S0 is the working state\n");
            return;
        }
        if (v == 2) {
            printk("powercfg: warning: S2 is deprecated by ACPI, entering anyway\n");
        }

        printk("Entering S%u...\n", (unsigned int)v);
        int rc = acpica_enter_sleep((uint8_t)v);
        if (rc != 0) {
            printk("powercfg: cannot enter S%u (code %d)\n", (unsigned int)v, rc);
            if (rc == -2) {
                printk("  reason: S4 requires a hibernate image, not implemented\n");
            } else if (rc == -3) {
                printk("  reason: firmware does not define this state\n");
            } else if (rc == -4) {
                printk("  reason: _PTS preparation failed\n");
            } else if (rc == -5) {
                printk("  reason: PM1_CNT write failed\n");
            }
        }
        return;
    }

    if (cmd_strcmp(opt, "/lastwake") == 0 || cmd_strcmp(opt, "/wake") == 0) {
        if (!acpica_ready()) {
            printk("powercfg: ACPICA not initialized\n");
            return;
        }
        printk("Last wake source: %s\n", acpica_last_wake_source());
        printk("Sleep entries   : %u\n", (unsigned int)acpica_sleep_count());
        return;
    }

    if (cmd_strcmp(opt, "/devices") == 0) {
        if (!acpica_ready()) {
            printk("powercfg: ACPICA not initialized\n");
            return;
        }
        acpica_dump();
        return;
    }

    if (cmd_strcmp(opt, "/h") == 0 || cmd_strcmp(opt, "/hibernate") == 0) {
        if (cmd_strcmp(p, "on") == 0) {
            printk("powercfg: hibernate (S4) is not available: no hibernate image\n");
        } else if (cmd_strcmp(p, "off") == 0) {
            printk("powercfg: hibernate is already disabled\n");
        } else {
            printk("usage: powercfg /h on|off\n");
        }
        return;
    }

    printk("powercfg: unknown option '%s'\n", opt);
    powercfg_usage();
}

/* ---- 任务管理：后台运行与 kill ---- */

static void cmd_bg(const char *args) {
    if (!args || !*args) {
        printk("usage: bg <file>\n");
        return;
    }

    slyz_run_background = 1;
    int rc = try_run_slyz_by_name(args);
    slyz_run_background = 0;

    if (rc == 0) {
        printk("bg: %s not found\n", args);
    } else if (rc == 2) {
        printk("bg: cannot start %s\n", args);
    }
}

static void cmd_kill(const char *args) {
    if (!args || !*args) {
        printk("usage: kill <pid>    (pid 0 = kernel, triggers BSOD)\n");
        return;
    }

    const struct AuthUser *u = auth_current();
    if (u && u->uid != 0) {
        printk("kill: permission denied (root only)\n");
        return;
    }

    uint32_t pid;
    if (!parse_u32(args, &pid) || pid >= TASK_MAX) {
        printk("kill: invalid pid\n");
        return;
    }

    if (pid == 0) {
        printk("kill: the kernel task cannot be terminated normally.\n");
        printk("Trigger the crash handler and show BSOD? [y/N] ");
        if (!read_yes_no()) {
            printk("kill: cancelled\n");
            return;
        }
        bsod(0x00000000u, "kernel task terminated by user request");
        return;   /* 不会到这里 */
    }

    struct Task *t = task_by_id((int)pid);
    if (!t || t->state == TASK_FREE || t->state == TASK_DEAD) {
        printk("kill: no such process: %u\n", (unsigned)pid);
        return;
    }

    char name[TASK_NAME_MAX];
    cmd_strncpy(name, t->name, sizeof(name));

    if (task_kill((int)pid)) {
        printk("kill: pid %u (%s) terminated, memory released\n",
               (unsigned)pid, name);
    } else {
        printk("kill: cannot kill pid %u\n", (unsigned)pid);
    }
}

/* ---- 用户与权限命令 ---- */

static int parse_octal_mode(const char *s, uint16_t *out) {
    uint16_t v = 0;
    if (!s || !*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '7') return 0;
        if (v > 0777u) return 0;
        v = (uint16_t)(v * 8u + (uint16_t)(*s - '0'));
        s++;
    }
    *out = v;
    return 1;
}

/* 把 12 位权限位转成 ls 风格的 10 字符串 */
static void format_mode_string(char out[11], uint16_t mode, uint8_t is_dir) {
    out[0] = is_dir ? 'd' : '-';
    const char *rwx = "rwx";
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & (uint16_t)(1u << (8 - i))) ? rwx[i % 3] : '-';
    }
    out[10] = '\0';
}

static void cmd_useradd(const char *args) {
    if (!args || !*args) {
        printk("usage: useradd <name>\n");
        return;
    }
    auth_useradd(args);
}

static void cmd_passwd(const char *args) {
    auth_passwd(args);
}

static void cmd_su(const char *args) {
    const char *target = (args && *args) ? args : "root";
    if (!auth_switch_to(target)) return;

    const struct AuthUser *u = auth_current();
    if (u && u->home[0]) {
        cmd_strncpy(shell_cwd, u->home, SHELL_PATH_MAX);
    } else {
        shell_cwd[0] = '\0';
    }
    shell_refresh_prompt();
}

static void cmd_logout(void) {
    printk("logging out\n");
    auth_logout();

    if (!auth_login()) return;

    const struct AuthUser *u = auth_current();
    if (u && u->home[0]) {
        cmd_strncpy(shell_cwd, u->home, SHELL_PATH_MAX);
    } else {
        shell_cwd[0] = '\0';
    }

    tty_clear();
    printk("SaylterOS v0.3 Beta2 - type 'help' for commands\n\n");
    shell_refresh_prompt();
}

static void cmd_chmod(const char *args) {
    char work[160];
    cmd_strncpy(work, (args && *args) ? args : "", sizeof(work));

    char *mode_s = work;
    while (*mode_s == ' ' || *mode_s == '\t') mode_s++;
    char *path_s = mode_s;
    while (*path_s && *path_s != ' ' && *path_s != '\t') path_s++;
    if (*path_s) *path_s++ = '\0';
    while (*path_s == ' ' || *path_s == '\t') path_s++;

    if (!*mode_s || !*path_s) {
        printk("usage: chmod <octal-mode> <path>\n");
        return;
    }

    uint16_t mode;
    if (!parse_octal_mode(mode_s, &mode)) {
        printk("chmod: mode must be octal, e.g. chmod 600 secret.txt\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, path_s);
    if (slyzfs_chmod(path, (int)mode)) {
        printk("chmod: %s updated\n", path);
    } else {
        printk("chmod: permission denied or not found\n");
    }
}

static void cmd_chown(const char *args) {
    char work[160];
    cmd_strncpy(work, (args && *args) ? args : "", sizeof(work));

    char *uid_s = work;
    while (*uid_s == ' ' || *uid_s == '\t') uid_s++;
    char *path_s = uid_s;
    while (*path_s && *path_s != ' ' && *path_s != '\t') path_s++;
    if (*path_s) *path_s++ = '\0';
    while (*path_s == ' ' || *path_s == '\t') path_s++;

    if (!*uid_s || !*path_s) {
        printk("usage: chown <uid> <path>\n");
        return;
    }

    uint32_t uid;
    if (!parse_u32(uid_s, &uid) || uid > 255) {
        printk("chown: uid must be 0..255\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, path_s);
    if (slyzfs_chown(path, (int)uid)) {
        printk("chown: %s -> uid %u\n", path, (unsigned int)uid);
    } else {
        printk("chown: permission denied (root only) or not found\n");
    }
}
static void cmd_ps(void)      { task_dump(); }

/* 启动模式：读写 etc\system.cfg 的 boot.mode。
 * 与 `nano etc\system.cfg` 完全等价，只是提供一个不用进编辑器的快捷入口。 */
static int find_home_mp3(char *out, int cap) {
    static struct SlyzDirEntry entries[32];
    int n = slyzfs_list_dir("home\\root", entries, 32);
    if (n < 0) n = 0;

    for (int i = 0; i < n; i++) {
        const char *name = (const char *)entries[i].name;
        int len = 0;
        while (len < 52 && name[len]) len++;

        if (len < 5) continue;
        if (name[len - 4] != '.' ||
            (name[len - 3] != 'm' && name[len - 3] != 'M') ||
            (name[len - 2] != 'p' && name[len - 2] != 'P') ||
            (name[len - 1] != '3')) {
            continue;
        }

        int o = 0;
        const char *prefix = "home\\root\\";
        while (prefix[o] && o < cap - 1) {
            out[o] = prefix[o];
            o++;
        }
        for (int k = 0; k < len && o < cap - 1; k++) out[o++] = name[k];
        out[o] = '\0';
        return 1;
    }
    return 0;
}

static void cmd_audiotest(const char *args) {
    char path_buf[160];
    const char *path = args;
    while (path && (*path == ' ' || *path == '\t')) path++;

    if (!path || !*path) {
        if (!find_home_mp3(path_buf, (int)sizeof(path_buf))) {
            printk("audiotest: no .mp3 found in home\\root\n");
            return;
        }
        path = path_buf;
    }

    audio_init();
    printk("audiotest: AC'97 %s\n", audio_available() ? "ready" : "not found");

    /* 先发一个 PC 扬声器音，用来区分"QEMU 音频后端没声音"和
     * "AC'97 PCM 通路没声音"。 */
    printk("audiotest: beep 880 Hz\n");
    audio_beep(880, 600);

    printk("audiotest: source %s\n", path);

    if (audio_play(path) != 0) {
        printk("audiotest: cannot open the MP3 file\n");
        return;
    }

    printk("audiotest: duration %u ms, volume %d%%\n",
           audio_duration_ms(), audio_volume());

    uint32_t start = net_now_ms();
    while (net_now_ms() - start < 20000u) {
        /* GUI 模式下解码由 audio.slys 后台任务完成，这里必须让出
         * CPU，否则终端窗口里的命令会把整帧占满、音频任务饿死。
         * TTY 模式没有后台任务，audio_pump 同步推进 DMA。 */
        audio_pump();
        task_yield();
        __asm__ volatile ("pause");
    }

    printk("audiotest: done at %u ms, state=%d\n",
           audio_position_ms(), audio_state());
    audio_stop();
}

static void cmd_cryptotest(const char *args) {
    (void)args;
    int failed = crypto_selftest();
    if (failed == 0) {
        printk("cryptotest: all known-answer vectors passed\n");
    } else {
        printk("cryptotest: %d vector(s) FAILED\n", failed);
    }
}

static void cmd_bootmode(const char *args) {
    char work[32];
    const char *p = args;
    int n = 0;

    while (p && (*p == ' ' || *p == '\t')) p++;
    while (p && *p && n < (int)sizeof(work) - 1) work[n++] = *p++;
    work[n] = '\0';
    while (n > 0 && (work[n - 1] == ' ' || work[n - 1] == '\t')) {
        work[--n] = '\0';
    }

    if (work[0] == '\0' || cmd_strcmp(work, "status") == 0) {
        printk("boot.mode = %s\n", system_cfg_boot_gui() ? "gui" : "tty");
        printk("config    = %s\n", SYSTEM_CFG_PATH);
        printk("edit with : nano %s\n", SYSTEM_CFG_PATH);
        return;
    }

    if (cmd_strcmp(work, "gui") == 0 || cmd_strcmp(work, "graphical") == 0) {
        if (system_cfg_set_boot_gui(1)) {
            printk("boot.mode = gui (reboot to apply)\n");
        } else {
            printk("bootmode: cannot write %s\n", SYSTEM_CFG_PATH);
        }
        return;
    }

    if (cmd_strcmp(work, "tty") == 0 || cmd_strcmp(work, "text") == 0) {
        if (system_cfg_set_boot_gui(0)) {
            printk("boot.mode = tty (reboot to apply)\n");
        } else {
            printk("bootmode: cannot write %s\n", SYSTEM_CFG_PATH);
        }
        return;
    }

    printk("usage: bootmode [gui|tty|status]\n");
}

/* 执行命令，返回 1 表示执行了，0 表示未找到 */
static int exec_cmd(const char *cmd, char *args) {
    /* GUI 终端：这些命令要么需要重新登录、要么需要密码输入，
     * 在非交互环境下会永久阻塞，直接拒绝并指路。 */
    if (shell_noninteractive()) {
        if (cmd_strcmp(cmd, "logout") == 0 ||
            cmd_strcmp(cmd, "useradd") == 0 ||
            cmd_strcmp(cmd, "adduser") == 0 ||
            cmd_strcmp(cmd, "passwd") == 0 ||
            cmd_strcmp(cmd, "ask") == 0) {
            printk("%s: needs an interactive TTY session\n", cmd);
            printk("Use Power menu -> Exit to Terminal.\n");
            return 1;
        }
    }

    if (cmd_strcmp(cmd, "help") == 0)      cmd_help();
    else if (cmd_strcmp(cmd, "min") == 0)   cmd_min();
    else if (cmd_strcmp(cmd, "clear") == 0) cmd_clear();
    else if (cmd_strcmp(cmd, "cls") == 0)   cmd_clear();
    else if (cmd_strcmp(cmd, "echo") == 0)  cmd_echo(args);
    else if (cmd_strcmp(cmd, "dir") == 0 || cmd_strcmp(cmd, "ls") == 0) cmd_dir(args);
    else if (cmd_strcmp(cmd, "pwd") == 0) cmd_pwd();
    else if (cmd_strcmp(cmd, "cd") == 0)  cmd_cd(args);
    else if (cmd_strcmp(cmd, "mkdir") == 0 || cmd_strcmp(cmd, "md") == 0) cmd_mkdir(args);
    else if (cmd_strcmp(cmd, "rmdir") == 0 || cmd_strcmp(cmd, "rd") == 0) cmd_rmdir(args);
    else if (cmd_strcmp(cmd, "rm") == 0 || cmd_strcmp(cmd, "del") == 0) cmd_rm(args);
    else if (cmd_strcmp(cmd, "df") == 0) cmd_df();
    else if (cmd_strcmp(cmd, "run") == 0)   cmd_run(args);
    else if (cmd_strcmp(cmd, "app") == 0)   cmd_app(args);
    else if (cmd_strcmp(cmd, "bg") == 0)    cmd_bg(args);
    else if (cmd_strcmp(cmd, "kill") == 0)  cmd_kill(args);
    else if (cmd_strcmp(cmd, "nano") == 0 || cmd_strcmp(cmd, "edit") == 0) cmd_edit(args);
    else if (cmd_strcmp(cmd, "vizcormin") == 0) cmd_vizcormin(args);
    else if (cmd_strcmp(cmd, "cat") == 0) cmd_cat(args);
    else if (cmd_strcmp(cmd, "ask") == 0)  cmd_ask(args);
    else if (cmd_strcmp(cmd, "exit") == 0 || cmd_strcmp(cmd, "quit") == 0) {
        printk("Bye.\n");
        return 2;
    }
    else if (cmd_strcmp(cmd, "dd") == 0)     cmd_dd(args);
    else if (cmd_strcmp(cmd, "remove") == 0) cmd_remove(args);
#ifdef SAYLTER_VBE
    else if (cmd_strcmp(cmd, "gui") == 0)   shell_enter_gui();
#endif
    else if (cmd_strcmp(cmd, "version") == 0) cmd_version();
    else if (cmd_strcmp(cmd, "uname") == 0) cmd_uname();
    else if (cmd_strcmp(cmd, "cjk") == 0)   cmd_cjk();
    else if (cmd_strcmp(cmd, "ime") == 0)   cmd_ime(args);
    else if (cmd_strcmp(cmd, "whoami") == 0) cmd_whoami();
    else if (cmd_strcmp(cmd, "date") == 0)  cmd_date();
    else if (cmd_strcmp(cmd, "mem") == 0)   cmd_mem();
    else if (cmd_strcmp(cmd, "gpu") == 0)   cmd_gpu();
    else if (cmd_strcmp(cmd, "linktest") == 0) {
        printk("[LINK] dynamic linking self-test\n");
        if (slys_link_selftest()) {
            printk("[LINK] OK\n");
        } else {
            printk("[LINK] FAILED\n");
        }
    }

    else if (cmd_strcmp(cmd, "reboot") == 0) cmd_reboot();
    else if (cmd_strcmp(cmd, "poweroff") == 0 || cmd_strcmp(cmd, "shutdown") == 0 ||
             cmd_strcmp(cmd, "halt") == 0) cmd_poweroff(args);
    else if (cmd_strcmp(cmd, "acpi") == 0) cmd_acpi();
    else if (cmd_strcmp(cmd, "powercfg") == 0) cmd_powercfg(args);
    else if (cmd_strcmp(cmd, "bootmode") == 0) cmd_bootmode(args);
    else if (cmd_strcmp(cmd, "audiotest") == 0) cmd_audiotest(args);
    else if (cmd_strcmp(cmd, "cryptotest") == 0) cmd_cryptotest(args);
    else if (cmd_strcmp(cmd, "useradd") == 0 || cmd_strcmp(cmd, "adduser") == 0) cmd_useradd(args);
    else if (cmd_strcmp(cmd, "passwd") == 0) cmd_passwd(args);
    else if (cmd_strcmp(cmd, "su") == 0)     cmd_su(args);
    else if (cmd_strcmp(cmd, "logout") == 0) cmd_logout();
    else if (cmd_strcmp(cmd, "chmod") == 0)  cmd_chmod(args);
    else if (cmd_strcmp(cmd, "chown") == 0)  cmd_chown(args);
    else if (cmd_strcmp(cmd, "vm") == 0)     cmd_vm(args);
    else if (cmd_strcmp(cmd, "swap") == 0)   cmd_swap();
    else if (cmd_strcmp(cmd, "pfa") == 0)    cmd_pfa(args);
    else if (cmd_strcmp(cmd, "protect") == 0) cmd_protect(args);
    else if (cmd_strcmp(cmd, "ps") == 0)    cmd_ps();
    else if (cmd_strcmp(cmd, "smp") == 0 || cmd_strcmp(cmd, "cpu") == 0) smp_dump();
    else if (cmd_strcmp(cmd, "ip") == 0 || cmd_strcmp(cmd, "ifconfig") == 0) netcmd_ip(args);
    else if (cmd_strcmp(cmd, "curl") == 0)  netcmd_curl(args);
    else if (cmd_strcmp(cmd, "ping") == 0)  netcmd_ping(args);
    else if (cmd_strcmp(cmd, "nslookup") == 0) netcmd_nslookup(args);
    else return 0;
    return 1;
}

/* ---------- 是/否确认 ---------- */
/* 读取单个字符，y 返回 1，其他返回 0。
 * GUI 终端里没有可阻塞的键盘来源，直接按"否"处理，绝不进入轮询。 */
static int read_yes_no(void) {
    if (shell_noninteractive()) return 0;

    while (1) {
        char c = kb_read_char();
        if (c == 0) { tty_tick(); delay(50000); continue; }
        if (c == '\n' || c == '\r' || c == 'n' || c == 'N') {
            printk("n\n");
            return 0;
        }
        if (c == 'y' || c == 'Y') {
            printk("y\n");
            return 1;
        }
    }
}

/* ---------- Shell 主循环 ---------- */

/* ---------- 供 GUI 内嵌终端调用的入口 ---------- */

const char *shell_prompt_text(void) {
    return shell_prompt;
}

int shell_resolve_input(const char *in, char *out, int cap) {
    if (!in || !out || cap <= 0) return 0;

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, in);
    if (path[0] == '\0') return 0;

    cmd_strncpy(out, path, cap);
    return 1;
}

int shell_exec_line(char *line) {
    if (!line) return 0;

    char *cmd = line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return 1;

    char *args = cmd;
    while (*args && *args != ' ' && *args != '\t') args++;
    if (*args) {
        *args++ = '\0';
        while (*args == ' ' || *args == '\t') args++;
    }

    int rc = exec_cmd(cmd, args);
    if (rc == 2) return 2;
    if (rc == 1) return 1;

    if (try_run_slyz_by_name(cmd)) return 1;

    printk("saylter: command not found: %s\n", cmd);
    return 0;
}

void shell_main(void) {
    /* 内核堆与任务系统必须最先就绪：之后的文件系统、账户库、
     * 脚本执行都可能申请内存或依赖任务上下文。 */
    mem_init();
    task_init();
    ime_init();

    /* 启动页静默挂载 SlyzFS：
     * 先把 bridge 的画面日志关掉，挂载/格式化完成后再打印 banner，
     * 启动画面因此不会被 [SlyzFS] 探测信息占满，进入 shell 时文件系统已就绪。 */
    slyzfs_set_quiet(1);
    int mount_ok = slyzfs_try_mount();
    int fresh_volume = 0;
    if (!mount_ok) {
        if (!debug_auto_format_allowed()) {
            /* 调试模式的安全闸：挂载失败只报告，不碰磁盘。
             * 工业 U 盘测试时避免把主硬盘上的旧分区格式化。 */
            printk("[SlyzFS] mount failed, auto-format disabled (debug safe mode)\n");
        } else if (slyzfs_format_and_mount()) {
            /* 新格式化的卷建立一套默认目录骨架，避免根目录空空如也 */
            slyzfs_mklazy();
            fresh_volume = 1;
        }
    }
    slyzfs_set_quiet(0);

    /* SlyzFS 就绪：调试串口日志开始写入当前启动盘。 */
    if (mount_ok || fresh_volume) {
        debug_serial_fs_ready();
    }

    /* ---- 平滑字体引擎 ----
     * 必须在 SlyzFS 挂载之后：字体是系统盘里的普通文件。
     * 未启用 VBE（80x25 文本模式）时 slyertype_init 直接返回 0，
     * 不分配内存，终端继续保持点阵与两个 '?' 的回退行为。 */
    uint32_t st_t0 = net_now_ms();
    slyertype_init();
    printk("[SlyerType] %s\n", slyertype_status());
    kserial_puts("[PERF] tty_fonts +");
    kserial_dec32(net_now_ms() - st_t0);
    kserial_puts("ms\r\n");

    /* ---- 分页 / 虚拟内存 / swap ----
     * 必须在 SlyzFS 挂载之后：pagefile.os 是文件系统里的普通文件。
     * 必须在登录之前：此时 uid 还是 0，pagefile 的创建与保护位设置
     * 都以 root 身份完成，后续普通用户既不能删除也不能改写保护位。 */
    vm_init();

    /* ---- 启动模式分流 ----
     * etc\system.cfg 的 boot.mode=gui 时，TTY banner 与 TTY 登录全部跳过，
     * 直接进入图形启动画面 -> 图形登录 -> 桌面。
     * 配置文件缺失时自动生成默认的 tty 配置，行为与旧版本一致。 */
    slyzfs_set_quiet(1);
    system_cfg_init();
    lang_init();
    /* 故障实验室必须在系统盘挂载后初始化：它要读 etc\faultlab.cfg，
     * 并在串口落盘开启时创建 system\logs\serial.log。 */
    faultlab_init();
    slyzfs_set_quiet(0);

    if (system_cfg_boot_gui() && vbe_is_enabled()) {
        tty_clear();
        gui_main();
        tty_clear();
    }

    /* 内核启动画面到此结束：先清屏，再从头输出 shell banner 与登录提示。
     * 不能反过来先输出再清屏，否则 banner 会被一起抹掉。 */
    tty_clear();

    printk(banner);
    printk("SaylterOS TTY Shell - type 'help' for commands\n");
    printk("Insert toggles edit mode, arrows move cursor, Backspace/Delete work\n");

    if (mount_ok || fresh_volume) {
        static struct SlyzDirEntry root_entries[32];
        int root_count = slyzfs_list_dir("", root_entries, 32);
        if (root_count < 0) root_count = 0;
        if (fresh_volume) {
            printk("[OK] SlyzFS formatted: new volume, %d root entries\n", root_count);
        } else {
            printk("[OK] SlyzFS mounted: %d root entries\n", root_count);
        }
    } else {
        printk("[!!] SlyzFS unavailable - file commands are disabled\n");
    }
    printk("\n");

    /* ---- 登录 ----
     * 账户库不存在时 auth_init 会创建 root 账户（无密码）；
     * 文件系统不可用时登录必然失败，此时降级为无会话的 root shell，
     * 让用户仍能使用 reboot/poweroff 等不依赖磁盘的命令。
     * 账户库的底层文件操作日志静默掉，避免把刚输出的 banner 顶走。 */
    slyzfs_set_quiet(1);
    auth_init();
    slyzfs_set_quiet(0);
    if (!auth_login()) {
        printk("[!] login unavailable - continuing as root without a session\n");
    }
    /* 按登录用户加载输入法词频（home\user\ime.freq）。 */
    ime_reload_user();

    /* 登录成功后从干净屏幕开始 */
    tty_clear();

    const struct AuthUser *session = auth_current();
    if (session && session->home[0]) {
        cmd_strncpy(shell_cwd, session->home, SHELL_PATH_MAX);
    } else {
        shell_cwd[0] = '\0';
    }

    printk("SaylterOS v0.3 Beta2 - type 'help' for commands\n");
    printk("User: %s (uid %u)   Home: %s\n\n",
           session ? session->name : "root",
           (unsigned int)(session ? session->uid : 0),
           shell_cwd[0] ? shell_cwd : "\\");

    shell_refresh_prompt();

    char line[256];
    while (1) {
        /* 每轮重新同步一次安全区：clear 命令或编辑器退出都会把 scroll_limit
         * 复位成全屏，若不在提示符绘制前重算，prompt 会被画到第 25 行并被 clamp，
         * 提交后没有保留区可搬运，历史行就会错位。 */
        shell_sync_scroll_zone();
        /* 提示符固定在保留区首行，上方输出再长也不会把它滚走 */
        tty_set_cursor(0, (uint16_t)(tty_get_scroll_limit() + 1));
        printk(shell_prompt);
        printk(" ");

        uint16_t prompt_x = tty_get_cursor_x();
        readline(line, sizeof(line), prompt_x);

        /* 空命令跳过 */
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (*cmd == '\0') continue;

        /* 切分 cmd + args */
        char *args = cmd;
        while (*args && *args != ' ' && *args != '\t') args++;
        if (*args) {
            *args++ = '\0';
            while (*args == ' ' || *args == '\t') args++;
        }

        /* 先查内置命令：内置命令必须优先响应。
         * 若先扫描磁盘查找同名 .slyz 可执行文件，每条命令都要遍历数万个扇区，
         * 在 9600 波特率串口下会表现为"任意命令都卡死"。 */
        int rc = exec_cmd(cmd, args);
        if (rc == 2) break;
        if (rc == 1) continue;

        /* 内置没有 → 再尝试作为 .slyz 可执行文件执行 */
        if (try_run_slyz_by_name(cmd)) continue;

        /* 未找到 → 试试纠错 */
        const char *guess = find_closest(cmd);
        if (guess) {
            printk("saylter: '%s' is not a command. Did you mean '%s'? [y/N] ", cmd, guess);
            if (read_yes_no()) {
                exec_cmd(guess, args);
            }
        } else {
            printk("saylter: command not found: %s\n", cmd);
        }
    }
}
