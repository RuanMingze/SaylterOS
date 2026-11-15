/* ============================================================
 * acpica_glue.c - ACPICA 初始化序列与诊断输出
 * ------------------------------------------------------------
 * ACPICA 的标准启动顺序（缺一不可）：
 *   AcpiInitializeSubsystem   -> 建立全局状态，调用 AcpiOsInitialize
 *   AcpiInitializeTables      -> 通过 RSDP 找到 RSDT/XSDT 并安装表
 *   AcpiLoadTables            -> 加载 DSDT 与全部 SSDT（AML）
 *   AcpiEnableSubsystem       -> 使能 ACPI 硬件（FADT/PM1/固定事件）
 *   AcpiInitializeObjects     -> 遍历命名空间执行 _INI/_STA，建立设备对象
 *
 * P2 在 P1 的基础上补齐运行期路径：
 *   - 从 \_S5 读取 SLP_TYP，交给 kernel/acpi.c 作为关机参数
 *   - 安装电源键 fixed event，事件触发后由 shell 空闲循环执行 S5
 *   - AcpiUpdateAllGpes 使能全部 GPE
 *   - 注册 SCI 中断回调并开放硬件 IRQ（pm.asm 的 isr_sci -> 本文件）
 *
 * 本文件只负责这条序列与运行期状态；OSL 实现在 acpica_osl.c。
 * ============================================================ */

#include <acpi.h>
#include <stdint.h>

#include "acpi.h"
#include "io.h"
#include "acpica_glue.h"
#include "tty.h"

/* SCI 回调地址：与 pm.asm 的 SCI_CALLBACK_ADDR 必须一致。
 * 放在 VBE 结果结构（0x7DF0）之前的 8 字节空隙里。 */
#define SCI_CALLBACK_ADDR 0x7DE8

/* 命名空间里被 AcpiGetDevices 枚举到的设备数量 */
static uint32_t g_device_count;

/* 运行期状态（shell 的 acpi 命令会读取） */
static int      g_acpica_ready;
static int      g_fixed_power_installed;
static int      g_sci_enabled;
static uint32_t g_sci_irq;
static volatile uint32_t g_sci_count;
static volatile int g_power_pending;

static ACPI_STATUS acpica_device_callback(ACPI_HANDLE Object,
                                          UINT32 NestingLevel,
                                          void *Context,
                                          void **ReturnValue)
{
    (void)Object;
    (void)NestingLevel;
    (void)Context;
    (void)ReturnValue;

    g_device_count++;
    return AE_OK;
}

/* 注意：全部诊断输出走 AcpiOsPrintf（只写 COM1），不经过 tty。
 * ACPI 初始化发生在启动画面播放期间，屏幕上不应出现这些日志；
 * 需要排查时看串口即可。 */
static void acpica_print_status(const char *stage, ACPI_STATUS status)
{
    if (ACPI_SUCCESS(status)) {
        AcpiOsPrintf("[ACPICA] %s OK\n", stage);
        return;
    }
    AcpiOsPrintf("[ACPICA] %s FAILED: %s (0x%x)\n",
                 stage, AcpiFormatException(status), (unsigned int)status);
}

static void acpica_print_table_info(void)
{
    ACPI_TABLE_HEADER *dsdt = NULL;
    ACPI_STATUS status;

    status = AcpiGetTable(ACPI_SIG_DSDT, 0, &dsdt);
    if (ACPI_FAILURE(status) || !dsdt) {
        AcpiOsPrintf("[ACPICA] DSDT not found\n");
        return;
    }

    AcpiOsPrintf("[ACPICA] DSDT %u bytes, OEM ",
                 (unsigned int)dsdt->Length);
    for (int i = 0; i < 6; i++) {
        char c = dsdt->OemId[i];
        AcpiOsPrintf("%c", (c >= 0x20 && c < 0x7F) ? c : ' ');
    }
    AcpiOsPrintf(" rev %u\n", (unsigned int)dsdt->OemRevision);
}

/* ACPICA 的全局开关（acglobal.h，不在公共头 acpi.h 里） */
extern BOOLEAN AcpiGbl_EnableTableValidation;

/* ============================================================
 * SCI / GPE / 固定事件（P2）
 * ============================================================ */

/* 电源键 fixed event：中断上下文里只置标志，S5 留给 shell 主循环执行。
 * 在 ISR 里直接进 S5 会带着解释器锁与未完成的 I/O 一起断电。 */
static UINT32 acpica_power_button_handler(void *Context)
{
    (void)Context;
    g_power_pending = 1;
    return ACPI_INTERRUPT_HANDLED;
}

void acpica_sci_handler(void)
{
    if (!g_sci_enabled || g_sci_irq >= 16u) {
        return;
    }
    g_sci_count++;
    /* ACPICA 的 SCI handler 会清掉 GPE / 固定事件状态并释放 SCI 电平；
     * 需要执行的 AML 方法由它内部通过 AcpiOsExecute 入队。 */
    (void)acpica_dispatch_irq(g_sci_irq);
}

/* 把 SCI 接到硬件：注册回调地址、ELCR 设电平触发、打开 PIC 掩码。 */
static void acpica_enable_sci(void)
{
    uint32_t irq = acpi_sci_irq();

    if (irq == 0u || irq > 15u) {
        AcpiOsPrintf("[ACPICA] SCI IRQ invalid (%u), stays masked\n",
                     (unsigned int)irq);
        return;
    }

    g_sci_irq = irq;

    /* ISR 只认这个地址，写入必须发生在中断被打开之前 */
    *(volatile uint64_t *)(uintptr_t)SCI_CALLBACK_ADDR =
        (uint64_t)(uintptr_t)acpica_sci_handler;

    /* ELCR：SCI 低电平有效且共享，边沿触发会漏事件 */
    {
        uint16_t elcr_port = (irq < 8u) ? 0x4D0 : 0x4D1;
        uint8_t bit = (uint8_t)(irq & 7u);
        uint8_t elcr = inb(elcr_port);
        outb(elcr_port, (uint8_t)(elcr | (uint8_t)(1u << bit)));
    }

    if (irq >= 8u) {
        /* 从片 IRQ 经主片 IRQ2 级联，两级掩码都要打开 */
        outb(0x21, (uint8_t)(inb(0x21) & ~(uint8_t)(1u << 2)));
        outb(0xA1, (uint8_t)(inb(0xA1) & ~(uint8_t)(1u << (irq - 8u))));
    } else {
        outb(0x21, (uint8_t)(inb(0x21) & ~(uint8_t)(1u << irq)));
    }

    g_sci_enabled = 1;
    AcpiOsPrintf("[ACPICA] SCI IRQ%u enabled (level triggered)\n",
                 (unsigned int)irq);
}

int acpica_ready(void)
{
    return g_acpica_ready;
}

int acpica_power_button_pending(void)
{
    int pending = g_power_pending;
    g_power_pending = 0;
    return pending;
}

uint32_t acpica_sci_count(void)
{
    return g_sci_count;
}

int acpica_request_s5(void)
{
    ACPI_STATUS status;

    if (!g_acpica_ready) {
        return 0;
    }

    /* Prep 读 \_S5 并执行 _PTS，规范要求此时中断打开 */
    status = AcpiEnterSleepStatePrep(ACPI_STATE_S5);
    if (ACPI_FAILURE(status)) {
        AcpiOsPrintf("[ACPICA] EnterSleepStatePrep(S5) failed: %s\n",
                     AcpiFormatException(status));
        return 0;
    }

    /* Enter 写 PM1_CNT，规范要求关中断 */
    __asm__ volatile ("cli" ::: "memory");
    status = AcpiEnterSleepState(ACPI_STATE_S5);
    __asm__ volatile ("sti" ::: "memory");

    if (ACPI_FAILURE(status)) {
        AcpiOsPrintf("[ACPICA] EnterSleepState(S5) failed: %s\n",
                     AcpiFormatException(status));
    }
    return 0;
}

void acpica_dump(void)
{
    UINT8 slp_a = 0;
    UINT8 slp_b = 0;
    uint32_t gpe0;

    if (!g_acpica_ready) {
        printk("ACPICA: not initialized (AML unavailable)\n");
        return;
    }

    ACPI_STATUS status = AcpiGetSleepTypeData(ACPI_STATE_S5, &slp_a, &slp_b);
    gpe0 = (uint32_t)AcpiGbl_FADT.Gpe0Block;
    if (gpe0 == 0u && AcpiGbl_FADT.XGpe0Block.Address != 0u) {
        gpe0 = (uint32_t)AcpiGbl_FADT.XGpe0Block.Address;
    }

    printk("ACPICA: AML namespace ready, %u device(s) with _HID\n",
           (unsigned int)g_device_count);
    printk("  SCI IRQ      : %u (%s)\n",
           (unsigned int)g_sci_irq,
           g_sci_enabled ? "enabled, level triggered" : "masked");
    printk("  SCI handled  : %u interrupt(s)\n", (unsigned int)g_sci_count);
    printk("  GPE count    : %u\n", (unsigned int)AcpiCurrentGpeCount);
    printk("  GPE0 block   : 0x%x (%u bytes)\n",
           (unsigned int)gpe0,
           (unsigned int)AcpiGbl_FADT.Gpe0BlockLength);
    if (ACPI_SUCCESS(status)) {
        printk("  _S5 SLP_TYP  : A=%u B=%u\n",
               (unsigned int)slp_a, (unsigned int)slp_b);
    } else {
        printk("  _S5 SLP_TYP  : not found, default %u\n",
               (unsigned int)acpi_get_s5_slp_typ());
    }
    printk("  power button : %s\n",
           g_fixed_power_installed ? "ACPI fixed event" : "PM1_STS polling fallback");
    printk("  PM1a_EVT/CNT : 0x%x / 0x%x\n",
           (unsigned int)acpi_pm1a_evt_port(),
           (unsigned int)acpi_pm1a_cnt_port());
}

int acpica_init(void)
{
    ACPI_STATUS status;

    g_device_count = 0;

    if (acpi_find_rsdp_phys() == 0) {
        AcpiOsPrintf("[ACPICA] no RSDP in EBDA/BIOS area, skipped\n");
        return 0;
    }

    status = AcpiInitializeSubsystem();
    acpica_print_status("InitializeSubsystem", status);
    if (ACPI_FAILURE(status)) {
        return 0;
    }

    /* 表校验会把每张表逐字节累加一次。某些固件（含 QEMU 的部分 SSDT）
     * 会给出长度字段与实际内容不一致的表，校验循环就会跑满整个长度，
     * 在 9600bps 串口日志下表现为启动卡死。这里与多数 OS 的做法一致：
     * 关闭表校验，由 ACPICA 的 interpreter slack 继续容错解析。 */
    AcpiGbl_EnableTableValidation = FALSE;

    /* NULL + flags 0：让 ACPICA 通过 AcpiOsGetRootPointer 从 RSDP 扫描 */
    status = AcpiInitializeTables(NULL, 0, FALSE);
    acpica_print_status("InitializeTables", status);
    if (ACPI_FAILURE(status)) {
        return 0;
    }

    acpica_print_table_info();

    status = AcpiLoadTables();
    acpica_print_status("LoadTables", status);
    if (ACPI_FAILURE(status)) {
        return 0;
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    acpica_print_status("EnableSubsystem", status);
    if (ACPI_FAILURE(status)) {
        return 0;
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    acpica_print_status("InitializeObjects", status);
    if (ACPI_FAILURE(status)) {
        return 0;
    }

    AcpiGetDevices(NULL, acpica_device_callback, NULL, NULL);
    AcpiOsPrintf("[ACPICA] namespace ready, %u device(s) with _HID\n",
                 (unsigned int)g_device_count);

    /* ---- P2：运行期路径 ---- */
    {
        UINT8 slp_a = 0;
        UINT8 slp_b = 0;

        /* \_S5 是关机参数的唯一权威来源；读不到时保留 acpi.h 的默认值，
         * acpi_power_off() 仍有 0/5 双候选回退。 */
        status = AcpiGetSleepTypeData(ACPI_STATE_S5, &slp_a, &slp_b);
        if (ACPI_SUCCESS(status)) {
            acpi_set_s5_slp_typ((uint8_t)(slp_a & 0x07u));
            AcpiOsPrintf("[ACPICA] \\_S5 SLP_TYP A=%u B=%u\n",
                         (unsigned int)slp_a, (unsigned int)slp_b);
        } else {
            AcpiOsPrintf("[ACPICA] \\_S5 unavailable, keep default SLP_TYP=%u\n",
                         (unsigned int)acpi_get_s5_slp_typ());
        }

        status = AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON,
                                              acpica_power_button_handler, NULL);
        if (ACPI_SUCCESS(status)) {
            g_fixed_power_installed = 1;
            AcpiOsPrintf("[ACPICA] power button fixed event installed\n");
        } else {
            AcpiOsPrintf("[ACPICA] power button event failed: %s\n",
                         AcpiFormatException(status));
        }

        status = AcpiUpdateAllGpes();
        if (ACPI_SUCCESS(status)) {
            AcpiOsPrintf("[ACPICA] UpdateAllGpes OK, %u GPE(s)\n",
                         (unsigned int)AcpiCurrentGpeCount);
        } else {
            AcpiOsPrintf("[ACPICA] UpdateAllGpes failed: %s\n",
                         AcpiFormatException(status));
        }

        /* 最后才接硬件中断：此前的任何事件都会在 GPE 使能时被同步清掉 */
        acpica_enable_sci();
        g_acpica_ready = 1;
    }

    return 1;
}
