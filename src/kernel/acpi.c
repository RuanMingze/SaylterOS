/* ============================================================
 * acpi.c - SaylterOS 最小 ACPI 支持（S5 关机 + 电源键事件）
 * ------------------------------------------------------------
 * 实现范围（刻意保持很小）：
 *   1. 在 EBDA 与 0xE0000..0xFFFFF 中查找 RSDP（校验 20 字节和）
 *   2. 通过 RSDT（优先）或 XSDT 找到 FADT（签名 "FACP"）
 *   3. 取出 PM1a/PM1b 的 EVT/CNT 端口、SCI IRQ、SMI_CMD 等字段
 *   4. 若 SCI_EN 未置位则按 FADT 的 ACPI_ENABLE 字节使能 ACPI
 *   5. 打开 PM1_EN.PWRBTN_EN，并轮询 PM1_STS.PWRBTN_STS 识别电源键
 *   6. 写 SLP_TYP|SLP_EN 到 PM1_CNT 进入 S5
 *
 * 明确不做的事：
 *   - 不解析 DSDT/SSDT 的 AML，因此不读取 \_S5 包
 *   - 不注册 SCI 中断处理（IRQ 路由需要 _PRT，属于 AML 范畴）
 *   - 不实现 S3/S4、不复位寄存器、不接管 GPE
 * 这些留给后续引入 uACPI/ACPICA 时补齐。
 * ============================================================ */

#include <stdint.h>
#include "acpi.h"
#include "io.h"
#include "tty.h"

/* ---- FADT 字段偏移（ACPI 6.x 规范，单位：字节） ----
 * 注意表头固定 36 字节（0x00..0x23），第一个非表头字段从 0x24 开始：
 *   FIRMWARE_CTRL 0x24, DSDT 0x28, Reserved 0x2C, PM_Profile 0x2D,
 *   SCI_INT 0x2E, SMI_CMD 0x30, ACPI_ENABLE 0x34, ACPI_DISABLE 0x35,
 *   S4BIOS_REQ 0x36, PSTATE_CNT 0x37, PM1a_EVT_BLK 0x38 ...
 * 少算表头尾部 1 字节会让所有字段整体错位，读出的 SCI IRQ / PM1 端口
 * 看似"有值"但全是相邻字段的拼接结果（曾把 IRQ9 读成 0x0900）。 */
#define FADT_LENGTH          0x04
#define FADT_SCI_INT         0x2E   /* 2 字节 */
#define FADT_SMI_CMD         0x30   /* 4 字节 */
#define FADT_ACPI_ENABLE     0x34   /* 1 字节 */
#define FADT_PM1A_EVT_BLK    0x38   /* 4 字节（低 16 位为端口） */
#define FADT_PM1B_EVT_BLK    0x3C
#define FADT_PM1A_CNT_BLK    0x40
#define FADT_PM1B_CNT_BLK    0x44
#define FADT_PM1_EVT_LEN     0x58
#define FADT_PM1_CNT_LEN     0x59

/* FADT 最小可用长度：至少覆盖 PM1_CNT_LEN 字段 */
#define FADT_MIN_LENGTH      0x5A

/* ---- PM1 寄存器位 ---- */
#define PM1_STS_PWRBTN       (uint16_t)(1u << 8)
#define PM1_CNT_SCI_EN       (uint16_t)(1u << 0)
#define PM1_CNT_SLP_TYP_SHIFT 10
#define PM1_CNT_SLP_EN       (uint16_t)(1u << 13)

/* 默认 S5 SLP_TYP 定义在 acpi.h 中（ACPI_S5_SLP_TYP_DEFAULT），
 * 这里只保存运行期的可覆盖值。 */

/* ---- 内部状态 ---- */
static int      acpi_ok = 0;
static uint32_t acpi_fadt_addr = 0;
static uint16_t acpi_sci_int = 0;
static uint32_t acpi_smi_cmd = 0;
static uint8_t  acpi_enable_val = 0;
static uint16_t acpi_pm1a_evt = 0;
static uint16_t acpi_pm1b_evt = 0;
static uint16_t acpi_pm1a_cnt = 0;
static uint16_t acpi_pm1b_cnt = 0;
static uint8_t  acpi_pm1_evt_len = 0;
static uint8_t  acpi_pm1_cnt_len = 0;
static uint8_t  acpi_s5_slp_typ = ACPI_S5_SLP_TYP_DEFAULT;

/* ============================================================
 * 表内存访问 —— 全部按字节拼装，避免非对齐指针解引用
 * ============================================================ */

static uint8_t acpi_rd8(uint32_t addr) {
    return *(volatile uint8_t *)(uintptr_t)addr;
}

static uint16_t acpi_rd16(uint32_t addr) {
    return (uint16_t)((uint16_t)acpi_rd8(addr) |
                      ((uint16_t)acpi_rd8(addr + 1) << 8));
}

static uint32_t acpi_rd32(uint32_t addr) {
    return (uint32_t)acpi_rd8(addr) |
           ((uint32_t)acpi_rd8(addr + 1) << 8) |
           ((uint32_t)acpi_rd8(addr + 2) << 16) |
           ((uint32_t)acpi_rd8(addr + 3) << 24);
}

static int acpi_sig_eq(uint32_t addr, const char *sig, int len) {
    for (int i = 0; i < len; i++) {
        if (acpi_rd8(addr + (uint32_t)i) != (uint8_t)sig[i]) return 0;
    }
    return 1;
}

/* 整张表的 8 位和必须为 0 */
static int acpi_checksum_ok(uint32_t addr, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + acpi_rd8(addr + i));
    }
    return sum == 0;
}

/* ============================================================
 * RSDP 查找
 * ============================================================ */

static int acpi_rsdp_valid(uint32_t addr) {
    /* 签名 "RSD PTR " 共 8 字节 */
    if (!acpi_sig_eq(addr, "RSD PTR ", 8)) return 0;
    /* ACPI 1.0 的 20 字节和校验；后续版本前 20 字节仍保持该规则 */
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        sum = (uint8_t)(sum + acpi_rd8(addr + (uint32_t)i));
    }
    return sum == 0;
}

uint32_t acpi_find_rsdp_phys(void) {
    /* 1) EBDA：BIOS 数据区 0x040E 存 EBDA 段地址，其前 1KB 内可能有 RSDP */
    uint32_t ebda = (uint32_t)acpi_rd16(0x040E) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (uint32_t addr = ebda; addr < ebda + 1024; addr += 16) {
            if (acpi_rsdp_valid(addr)) return addr;
        }
    }

    /* 2) 0xE0000..0xFFFFF 按 16 字节步进扫描 */
    for (uint32_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        if (acpi_rsdp_valid(addr)) return addr;
    }
    return 0;
}

/* ============================================================
 * RSDT / XSDT → FADT
 * ============================================================ */

static uint32_t acpi_scan_table_list(uint32_t table_addr) {
    if (!table_addr) return 0;

    uint32_t len = acpi_rd32(table_addr + 4);
    /* 表头 36 字节，剩余部分才是指针数组 */
    if (len < 36 || len > 0x10000) return 0;
    if (!acpi_checksum_ok(table_addr, len)) return 0;

    uint32_t count = (len - 36) / 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = acpi_rd32(table_addr + 36 + i * 4);
        if (!entry) continue;
        if (acpi_sig_eq(entry, "FACP", 4)) return entry;
    }
    return 0;
}

static uint32_t acpi_find_fadt(uint32_t rsdp) {
    /* ACPI 1.0：RSDT 地址在 RSDP+16 */
    uint32_t rsdt = acpi_rd32(rsdp + 16);
    if (rsdt) {
        uint32_t fadt = acpi_scan_table_list(rsdt);
        if (fadt) return fadt;
    }

    /* ACPI 2.0+：RSDT 可能为 0，改用 XSDT（RSDP+24，8 字节地址）。
     * 32 位内核只取低 32 位；ACPI 表位于 4GB 以下的平台上足够了。 */
    uint32_t xsdt_lo = acpi_rd32(rsdp + 24);
    if (xsdt_lo) {
        uint32_t fadt = acpi_scan_table_list(xsdt_lo);
        if (fadt) return fadt;
    }
    return 0;
}

/* ============================================================
 * PM1 使能
 * ============================================================ */

/* ACPI 规范：若 PM1_CNT.SCI_EN 为 0，表示固件尚未把 ACPI 交给 OS，
 * 需要向 SMI_CMD 写入 ACPI_ENABLE 并等待 SCI_EN 置位。 */
static void acpi_enable_pm(void) {
    if (!acpi_pm1a_cnt) return;
    if (inw(acpi_pm1a_cnt) & PM1_CNT_SCI_EN) return;

    /* SMI_CMD 为 0 或 0xFFFF 表示平台不支持该切换方式 */
    uint16_t smi_port = (uint16_t)(acpi_smi_cmd & 0xFFFF);
    if (smi_port == 0 || smi_port == 0xFFFF || acpi_enable_val == 0) return;

    outb(smi_port, acpi_enable_val);

    for (uint32_t i = 0; i < 1000000u; i++) {
        if (inw(acpi_pm1a_cnt) & PM1_CNT_SCI_EN) return;
        __asm__ volatile ("nop");
    }
}

static void acpi_enable_power_button(void) {
    if (!acpi_pm1a_evt) return;

    /* PM1_EVT_BLK 布局：+0 PM1_STS，+2 PM1_EN */
    uint16_t pm1_en = inw((uint16_t)(acpi_pm1a_evt + 2));
    pm1_en = (uint16_t)(pm1_en | PM1_STS_PWRBTN);
    outw((uint16_t)(acpi_pm1a_evt + 2), pm1_en);

    /* 清掉固件遗留的事件状态，避免刚进 shell 就误判为按下 */
    if (inw(acpi_pm1a_evt) & PM1_STS_PWRBTN) {
        outw(acpi_pm1a_evt, PM1_STS_PWRBTN);
    }
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int acpi_init(void) {
    acpi_ok = 0;

    uint32_t rsdp = acpi_find_rsdp_phys();
    if (!rsdp) return 0;

    uint32_t fadt = acpi_find_fadt(rsdp);
    if (!fadt) return 0;

    uint32_t length = acpi_rd32(fadt + FADT_LENGTH);
    if (length < FADT_MIN_LENGTH) return 0;
    if (!acpi_checksum_ok(fadt, length)) return 0;

    acpi_fadt_addr = fadt;
    acpi_sci_int = acpi_rd16(fadt + FADT_SCI_INT);
    acpi_smi_cmd = acpi_rd32(fadt + FADT_SMI_CMD);
    acpi_enable_val = acpi_rd8(fadt + FADT_ACPI_ENABLE);
    acpi_pm1a_evt = (uint16_t)(acpi_rd32(fadt + FADT_PM1A_EVT_BLK) & 0xFFFF);
    acpi_pm1b_evt = (uint16_t)(acpi_rd32(fadt + FADT_PM1B_EVT_BLK) & 0xFFFF);
    acpi_pm1a_cnt = (uint16_t)(acpi_rd32(fadt + FADT_PM1A_CNT_BLK) & 0xFFFF);
    acpi_pm1b_cnt = (uint16_t)(acpi_rd32(fadt + FADT_PM1B_CNT_BLK) & 0xFFFF);
    acpi_pm1_evt_len = acpi_rd8(fadt + FADT_PM1_EVT_LEN);
    acpi_pm1_cnt_len = acpi_rd8(fadt + FADT_PM1_CNT_LEN);

    /* 没有 PM1a 控制块就无法进入任何睡眠状态 */
    if (!acpi_pm1a_cnt) return 0;

    acpi_ok = 1;
    acpi_enable_pm();
    acpi_enable_power_button();
    return 1;
}

int acpi_available(void) {
    return acpi_ok;
}

uint16_t acpi_pm1a_evt_port(void) {
    return acpi_pm1a_evt;
}

uint16_t acpi_pm1a_cnt_port(void) {
    return acpi_pm1a_cnt;
}

uint16_t acpi_pm1b_cnt_port(void) {
    return acpi_pm1b_cnt;
}

uint16_t acpi_sci_irq(void) {
    return acpi_sci_int;
}

int acpi_poll_power_button(void) {
    if (!acpi_ok || !acpi_pm1a_evt) return 0;

    uint16_t status = inw(acpi_pm1a_evt);
    if (!(status & PM1_STS_PWRBTN)) return 0;

    /* PM1_STS 是写 1 清除 */
    outw(acpi_pm1a_evt, PM1_STS_PWRBTN);
    return 1;
}

void acpi_set_s5_slp_typ(uint8_t slp_typ) {
    if (slp_typ > 7) slp_typ = 7;
    acpi_s5_slp_typ = slp_typ;
}

uint8_t acpi_get_s5_slp_typ(void) {
    return acpi_s5_slp_typ;
}

int acpi_power_off_with(uint8_t slp_typ) {
    if (!acpi_ok || !acpi_pm1a_cnt) return 0;

    /* 部分平台在进入 shell 前仍由固件持有 ACPI，这里再确认一次 */
    acpi_enable_pm();

    uint16_t value = (uint16_t)(((uint16_t)(slp_typ & 0x07) << PM1_CNT_SLP_TYP_SHIFT) |
                                PM1_CNT_SLP_EN);

    /* 先写 PM1b（若存在）再写 PM1a，符合规范建议的顺序 */
    if (acpi_pm1b_cnt) {
        outw(acpi_pm1b_cnt, value);
    }
    outw(acpi_pm1a_cnt, value);

    /* 正常情况下芯片组已经切断电源，这里不会返回。
     * 给一段空转时间以覆盖较慢的电源管理单元。 */
    for (volatile uint32_t i = 0; i < 2000000u; i++) {
        __asm__ volatile ("nop");
    }
    return 0;
}

int acpi_power_off(void) {
    if (!acpi_ok || !acpi_pm1a_cnt) return 0;

    uint8_t primary = acpi_s5_slp_typ;
    uint8_t fallback = (primary == 0) ? 5 : 0;

    acpi_power_off_with(primary);

    /* 走到这里说明主值没有被硬件接受。0 和 5 是固件里最常见的两个
     * S5 SLP_TYP：QEMU 与部分 UEFI 用 0，传统 BIOS 多用 5；
     * 两者都表示 soft-off，不会把机器带进 S1/S3/S4，因此可以安全回退。 */
    acpi_power_off_with(fallback);
    return 0;
}

void acpi_dump(void) {
    if (!acpi_ok) {
        printk("ACPI: no usable FADT found\n");
        printk("ACPI: S5 shutdown and power button are unavailable\n");
        return;
    }

    printk("ACPI: FADT @ 0x%x\n", acpi_fadt_addr);
    printk("  SCI IRQ        : %u\n", (unsigned int)acpi_sci_int);
    printk("  PM1a_EVT       : 0x%x (len %u)\n",
           (unsigned int)acpi_pm1a_evt, (unsigned int)acpi_pm1_evt_len);
    printk("  PM1a_CNT       : 0x%x (len %u)\n",
           (unsigned int)acpi_pm1a_cnt, (unsigned int)acpi_pm1_cnt_len);
    printk("  PM1b_EVT/CNT   : 0x%x / 0x%x\n",
           (unsigned int)acpi_pm1b_evt, (unsigned int)acpi_pm1b_cnt);
    printk("  SMI_CMD        : 0x%x  ACPI_ENABLE 0x%x\n",
           (unsigned int)acpi_smi_cmd, (unsigned int)acpi_enable_val);
    printk("  SCI_EN         : %u\n",
           (unsigned int)((inw(acpi_pm1a_cnt) & PM1_CNT_SCI_EN) ? 1u : 0u));
    printk("  PWRBTN_EN      : %u\n",
           (unsigned int)((inw((uint16_t)(acpi_pm1a_evt + 2)) & PM1_STS_PWRBTN) ? 1u : 0u));
    printk("  PWRBTN_STS     : %u\n",
           (unsigned int)((inw(acpi_pm1a_evt) & PM1_STS_PWRBTN) ? 1u : 0u));
    printk("  S5 SLP_TYP     : %u (fallback %u)\n",
           (unsigned int)acpi_s5_slp_typ,
           (unsigned int)((acpi_s5_slp_typ == 0) ? 5 : 0));
}
