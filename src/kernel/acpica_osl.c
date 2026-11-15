/* ============================================================
 * acpica_osl.c - ACPICA 操作系统服务层（OSL）
 * ------------------------------------------------------------
 * ACPICA 只提供 ACPI 机制（命名空间、AML 解释器、GPE 分发），
 * 所有与宿主 OS 的交互都通过 acpiosxf.h 里的 AcpiOs* 接口，
 * 本文件就是这些接口在 SaylterOS 上的实现。
 *
 * 设计前提：
 *   1. 单地址空间、非抢占式协作调度 —— 锁与信号量只需要屏蔽中断；
 *   2. 4GB 恒等映射 —— MapMemory / GetPhysicalAddress 可以零成本直通；
 *   3. 中断开关状态可从 RFLAGS.IF 读出，Acquire/ReleaseLock 用它配对；
 *   4. 时间基准用 PIT 通道 0（1.193182MHz），换算成 ACPI 的 100ns 单位。
 * ============================================================ */

/* 注意：内核同目录下也有一个 acpi.h（最小 ACPI 支持），这里的尖括号
 * 形式强制走 -I 路径（third_party/acpica/source/include），避免同名冲突。 */
#include <acpi.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "io.h"
#include "mem.h"
#include "tty.h"
#include "acpica_glue.h"

#define COM1_PORT 0x3F8

/* ============================================================
 * 串口 / 控制台输出
 * ============================================================ */
/* ACPICA 自身的诊断输出只走 COM1：
 *   - tty_putchar 每个字符都要写 VBE 显存并等待 9600bps 串口，
 *     表加载阶段的几十行 INFO 会把启动拖到几十秒；
 *   - 与内核 printk 共用 tty 还会让每个字符在串口重复一次。
 * 面向用户的摘要（[ACPICA] ...）走 printk，屏幕与串口都能看到。 */
static void osl_putc(char c)
{
    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
        __asm__ volatile ("pause");
    }
    outb(COM1_PORT, (uint8_t)c);
}

static void osl_puts(const char *s)
{
    while (*s) {
        osl_putc(*s++);
    }
}

/* ------------------------------------------------------------
 * 迷你格式化器：支持 flags(- 0 + 空格) / width / .precision /
 * length(l ll h hh z) / 转换符(c s d i u x X p %)
 * ACPICA 的错误输出常用 "%4.4X" 这类组合，这里必须完整支持，
 * 否则异常信息会变成错位的乱码。
 * ------------------------------------------------------------ */
static void osl_emit_padded(const char *text, int len, int width, int left, char pad)
{
    int pad_count = width - len;
    if (!left) {
        while (pad_count-- > 0) {
            osl_putc(pad);
        }
    }
    for (int i = 0; i < len; i++) {
        osl_putc(text[i]);
    }
    if (left) {
        while (pad_count-- > 0) {
            osl_putc(' ');
        }
    }
}

static void osl_format_number(uint64_t value, unsigned base, int upper,
                              char *buf, int *out_len)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    char tmp[32];
    int n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value > 0) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    for (int i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';
    *out_len = n;
}

static void osl_vprintf_impl(const char *format, va_list args)
{
    const char *p = format;

    while (*p) {
        if (*p != '%') {
            osl_putc(*p++);
            continue;
        }
        p++;

        int left = 0;
        int plus = 0;
        int space = 0;
        char pad = ' ';
        int width = 0;
        int precision = -1;
        int length = 0;   /* 0=int, 1=long, 2=long long, 3=size_t */

        for (;;) {
            if (*p == '-') { left = 1; p++; }
            else if (*p == '+') { plus = 1; p++; }
            else if (*p == ' ') { space = 1; p++; }
            else if (*p == '0') { pad = '0'; p++; }
            else break;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == 'l') {
            p++;
            if (*p == 'l') { length = 2; p++; }
            else { length = 1; }
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') { p++; }
        } else if (*p == 'z') {
            length = 3;
            p++;
        }

        char conv = *p;
        if (conv == '\0') {
            break;
        }
        p++;

        if (conv == '%') {
            osl_putc('%');
            continue;
        }

        if (conv == 'c') {
            char c = (char)va_arg(args, int);
            osl_emit_padded(&c, 1, width, left, ' ');
            continue;
        }

        if (conv == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) {
                s = "(null)";
            }
            int len = 0;
            while (s[len]) {
                len++;
            }
            if (precision >= 0 && len > precision) {
                len = precision;
            }
            osl_emit_padded(s, len, width, left, ' ');
            continue;
        }

        if (conv == 'd' || conv == 'i' || conv == 'u' ||
            conv == 'x' || conv == 'X' || conv == 'p') {

            uint64_t raw;
            if (conv == 'p') {
                raw = (uint64_t)(uintptr_t)va_arg(args, void *);
                length = 2;
            } else if (length == 2) {
                raw = (uint64_t)va_arg(args, unsigned long long);
            } else if (length == 3) {
                raw = (uint64_t)va_arg(args, size_t);
            } else if (length == 1) {
                raw = (uint64_t)va_arg(args, unsigned long);
            } else {
                raw = (uint64_t)va_arg(args, unsigned int);
            }

            char num[40];
            int num_len = 0;
            int negative = 0;

            if (conv == 'd' || conv == 'i') {
                int64_t sval = (int64_t)raw;
                if (sval < 0) {
                    negative = 1;
                    raw = (uint64_t)(-sval);
                }
            }

            unsigned base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16u : 10u;
            int upper = (conv == 'X');
            osl_format_number(raw, base, upper, num, &num_len);

            /* 精度：整数部分补前导零 */
            char digits_buf[48];
            int pos = 0;
            if (negative) {
                digits_buf[pos++] = '-';
            } else if (plus && (conv == 'd' || conv == 'i')) {
                digits_buf[pos++] = '+';
            } else if (space && (conv == 'd' || conv == 'i')) {
                digits_buf[pos++] = ' ';
            }
            if (conv == 'p') {
                digits_buf[pos++] = '0';
                digits_buf[pos++] = 'x';
            }
            int zero_pad = 0;
            if (precision > num_len) {
                zero_pad = precision - num_len;
            }
            for (int i = 0; i < zero_pad; i++) {
                digits_buf[pos++] = '0';
            }
            for (int i = 0; i < num_len; i++) {
                digits_buf[pos++] = num[i];
            }

            char pad_char = (pad == '0' && precision < 0) ? '0' : ' ';
            osl_emit_padded(digits_buf, pos, width, left, pad_char);
            continue;
        }

        /* 未知转换符：原样输出 */
        osl_putc('%');
        osl_putc(conv);
    }
}

/* ============================================================
 * 基础环境
 * ============================================================ */
ACPI_STATUS AcpiOsInitialize(void)
{
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    return (ACPI_PHYSICAL_ADDRESS)acpi_find_rsdp_phys();
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal,
                                     ACPI_STRING *NewVal)
{
    (void)InitVal;
    if (NewVal) {
        *NewVal = NULL;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                                ACPI_TABLE_HEADER **NewTable)
{
    (void)ExistingTable;
    if (NewTable) {
        *NewTable = NULL;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                                        ACPI_PHYSICAL_ADDRESS *NewAddress,
                                        UINT32 *NewTableLength)
{
    (void)ExistingTable;
    (void)NewAddress;
    (void)NewTableLength;
    return AE_OK;
}

/* ============================================================
 * 锁与信号量
 * ------------------------------------------------------------
 * 单核协作式内核：锁的唯一作用是阻止中断处理程序重入解释器，
 * 因此 Acquire 用 cli 并返回进入前的 RFLAGS，Release 按 IF 恢 复。
 * ============================================================ */
static int g_lock_object;

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
    if (!OutHandle) {
        return AE_BAD_PARAMETER;
    }
    *OutHandle = (ACPI_SPINLOCK)&g_lock_object;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
    (void)Handle;
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
    (void)Handle;
    /* 必须用显式的 64 位变量：Windows x86_64 下 long 仍是 32 位，
     * 直接对 ACPI_CPU_FLAGS 做 popq 会选到 32 位寄存器而汇编失败。 */
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli" ::: "memory");
    return (ACPI_CPU_FLAGS)flags;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    (void)Handle;
    if (Flags & 0x200u) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static int g_semaphore_object;

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits,
                                  ACPI_SEMAPHORE *OutHandle)
{
    (void)MaxUnits;
    (void)InitialUnits;
    if (!OutHandle) {
        return AE_BAD_PARAMETER;
    }
    *OutHandle = (ACPI_SEMAPHORE)&g_semaphore_object;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
{
    (void)Handle;
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout)
{
    (void)Handle;
    (void)Units;
    (void)Timeout;
    /* 单线程下永远不需要等待 */
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units)
{
    (void)Handle;
    (void)Units;
    return AE_OK;
}

/* ============================================================
 * 互斥量（ACPI_MUTEX_TYPE = ACPI_OSL_MUTEX）
 * ------------------------------------------------------------
 * ACPICA 的互斥是可递归的解释器锁。单核协作式内核里只有中断可能
 * 造成重入，而中断处理程序只置标志位、不在中断上下文里跑解释器，
 * 因此这里用空实现；真正的中断屏蔽由 AcpiOsAcquireLock 负责。
 * ============================================================ */
static int g_mutex_object;

ACPI_STATUS AcpiOsCreateMutex(ACPI_MUTEX *OutHandle)
{
    if (!OutHandle) {
        return AE_BAD_PARAMETER;
    }
    *OutHandle = (ACPI_MUTEX)(void *)&g_mutex_object;
    return AE_OK;
}

void AcpiOsDeleteMutex(ACPI_MUTEX Handle)
{
    (void)Handle;
}

ACPI_STATUS AcpiOsAcquireMutex(ACPI_MUTEX Handle, UINT16 Timeout)
{
    (void)Handle;
    (void)Timeout;
    return AE_OK;
}

void AcpiOsReleaseMutex(ACPI_MUTEX Handle)
{
    (void)Handle;
}

/* ============================================================
 * 内存
 * ------------------------------------------------------------
 * AcpiOsAllocateZeroed 由 ACPICA 的 utalloc.c 提供（未定义
 * USE_NATIVE_ALLOCATE_ZEROED 时），这里不能再定义同名符号。
 * ============================================================ */
void *AcpiOsAllocate(ACPI_SIZE Size)
{
    if (Size == 0 || Size > 0xFFFFFFFFu) {
        return NULL;
    }
    return kmalloc((uint32_t)Size);
}

void AcpiOsFree(void *Memory)
{
    if (Memory) {
        kfree(Memory);
    }
}

/* 4GB 恒等映射：物理地址就是虚拟地址。
 * 注意：MMIO（如 0xFED00000 的 HPET）也走这条路径，当前页表没有 PAT/MTRR
 * 的 UC 标记，读设备寄存器可能命中缓存；P2 引入 GPE/HPET 定时器时需要
 * 在页表中把这段区间标成不可缓存。 */
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
    (void)Length;
    return (void *)(uintptr_t)Where;
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Size)
{
    (void)LogicalAddress;
    (void)Size;
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress,
                                     ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
    if (!PhysicalAddress) {
        return AE_BAD_PARAMETER;
    }
    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)(uintptr_t)LogicalAddress;
    return AE_OK;
}

/* ============================================================
 * 中断处理注册
 * ------------------------------------------------------------
 * P1 阶段只记录 handler，真正的 IDT 挂接在 P2 与 SCI 一起做。
 * 中断到来时由 acpica_dispatch_irq() 转发。
 * ============================================================ */
#define OSL_IRQ_SLOTS 16

struct osl_irq_entry {
    ACPI_OSD_HANDLER handler;
    void *context;
    int in_use;
};

static struct osl_irq_entry g_irq_table[OSL_IRQ_SLOTS];

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber,
                                          ACPI_OSD_HANDLER ServiceRoutine,
                                          void *Context)
{
    if (InterruptNumber >= OSL_IRQ_SLOTS || !ServiceRoutine) {
        return AE_BAD_PARAMETER;
    }
    g_irq_table[InterruptNumber].handler = ServiceRoutine;
    g_irq_table[InterruptNumber].context = Context;
    g_irq_table[InterruptNumber].in_use = 1;
    /* 走串口：注册号与 FADT 的 SCI_INT 不一致时，中断永远分发不到 */
    AcpiOsPrintf("[ACPICA] OSL install handler: IRQ%u\n",
                 (unsigned int)InterruptNumber);
    return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber,
                                         ACPI_OSD_HANDLER ServiceRoutine)
{
    (void)ServiceRoutine;
    if (InterruptNumber >= OSL_IRQ_SLOTS) {
        return AE_BAD_PARAMETER;
    }
    g_irq_table[InterruptNumber].handler = NULL;
    g_irq_table[InterruptNumber].context = NULL;
    g_irq_table[InterruptNumber].in_use = 0;
    return AE_OK;
}

int acpica_dispatch_irq(uint32_t irq)
{
    if (irq >= OSL_IRQ_SLOTS) {
        return 0;
    }
    if (!g_irq_table[irq].in_use || !g_irq_table[irq].handler) {
        return 0;
    }
    return g_irq_table[irq].handler(g_irq_table[irq].context) == ACPI_INTERRUPT_HANDLED;
}

/* ============================================================
 * 线程与调度
 * ============================================================ */
#define OSL_DEFERRED_MAX 32

struct osl_deferred_call {
    ACPI_OSD_EXEC_CALLBACK function;
    void *context;
};

static struct osl_deferred_call g_deferred_queue[OSL_DEFERRED_MAX];
static volatile uint32_t g_deferred_head;
static volatile uint32_t g_deferred_tail;

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return 1;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type,
                          ACPI_OSD_EXEC_CALLBACK Function,
                          void *Context)
{
    (void)Type;
    if (!Function) {
        return AE_BAD_PARAMETER;
    }
    uint32_t next = (g_deferred_head + 1u) % OSL_DEFERRED_MAX;
    if (next == g_deferred_tail) {
        /* 队列满：宁可丢一次通知，也不能在中断上下文里同步执行 */
        return AE_ERROR;
    }
    g_deferred_queue[g_deferred_head].function = Function;
    g_deferred_queue[g_deferred_head].context = Context;
    g_deferred_head = next;
    return AE_OK;
}

void acpica_drain_deferred(void)
{
    while (g_deferred_tail != g_deferred_head) {
        struct osl_deferred_call call = g_deferred_queue[g_deferred_tail];
        g_deferred_tail = (g_deferred_tail + 1u) % OSL_DEFERRED_MAX;
        if (call.function) {
            call.function(call.context);
        }
    }
}

void AcpiOsWaitEventsComplete(void)
{
    acpica_drain_deferred();
}

/* ============================================================
 * 时间基准：PIT 通道 0，1.193182MHz → 100ns 单位
 * ============================================================ */
#define PIT_INPUT_HZ 1193182u

static uint16_t g_pit_last;
static uint64_t g_pit_total;
static int g_pit_started;

static uint16_t osl_pit_read(void)
{
    outb(0x43, 0x00);          /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

UINT64 AcpiOsGetTimer(void)
{
    uint16_t now = osl_pit_read();

    if (!g_pit_started) {
        g_pit_last = now;
        g_pit_started = 1;
        return 0;
    }

    uint16_t delta = (uint16_t)(g_pit_last - now);
    g_pit_last = now;
    g_pit_total += (uint64_t)delta;

    return (g_pit_total * 10000000ull) / (uint64_t)PIT_INPUT_HZ;
}

void AcpiOsSleep(UINT64 Milliseconds)
{
    uint64_t start = AcpiOsGetTimer();
    uint64_t target = Milliseconds * 10000ull;

    while ((AcpiOsGetTimer() - start) < target) {
        __asm__ volatile ("pause");
    }
}

void AcpiOsStall(UINT32 Microseconds)
{
    uint64_t start = AcpiOsGetTimer();
    uint64_t target = (uint64_t)Microseconds * 10ull;

    while ((AcpiOsGetTimer() - start) < target) {
        __asm__ volatile ("pause");
    }
}

/* ============================================================
 * 端口 / 内存 / PCI 配置空间访问
 * ============================================================ */
ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
    if (!Value) {
        return AE_BAD_PARAMETER;
    }
    switch (Width) {
        case 8:
            *Value = inb((uint16_t)Address);
            return AE_OK;
        case 16:
            *Value = inw((uint16_t)Address);
            return AE_OK;
        case 32: {
            uint32_t v;
            __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)Address));
            *Value = v;
            return AE_OK;
        }
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
{
    switch (Width) {
        case 8:
            outb((uint16_t)Address, (uint8_t)Value);
            return AE_OK;
        case 16:
            outw((uint16_t)Address, (uint16_t)Value);
            return AE_OK;
        case 32:
            __asm__ volatile ("outl %0, %1" :: "a"(Value), "Nd"((uint16_t)Address));
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width)
{
    if (!Value) {
        return AE_BAD_PARAMETER;
    }
    switch (Width) {
        case 8:
            *Value = *(volatile uint8_t *)(uintptr_t)Address;
            return AE_OK;
        case 16:
            *Value = *(volatile uint16_t *)(uintptr_t)Address;
            return AE_OK;
        case 32:
            *Value = *(volatile uint32_t *)(uintptr_t)Address;
            return AE_OK;
        case 64:
            *Value = *(volatile uint64_t *)(uintptr_t)Address;
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width)
{
    switch (Width) {
        case 8:
            *(volatile uint8_t *)(uintptr_t)Address = (uint8_t)Value;
            return AE_OK;
        case 16:
            *(volatile uint16_t *)(uintptr_t)Address = (uint16_t)Value;
            return AE_OK;
        case 32:
            *(volatile uint32_t *)(uintptr_t)Address = (uint32_t)Value;
            return AE_OK;
        case 64:
            *(volatile uint64_t *)(uintptr_t)Address = Value;
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

static uint32_t osl_pci_address(const ACPI_PCI_ID *PciId, uint32_t Reg)
{
    return (uint32_t)(0x80000000u |
                      ((uint32_t)PciId->Bus << 16) |
                      ((uint32_t)PciId->Device << 11) |
                      ((uint32_t)PciId->Function << 8) |
                      (Reg & 0xFCu));
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg,
                                       UINT64 *Value, UINT32 Width)
{
    if (!PciId || !Value) {
        return AE_BAD_PARAMETER;
    }
    uint32_t addr = osl_pci_address(PciId, Reg);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));

    uint32_t data;
    __asm__ volatile ("inl %1, %0" : "=a"(data) : "Nd"((uint16_t)0xCFC));

    uint32_t shift = (Reg & 3u) * 8u;
    switch (Width) {
        case 8:
            *Value = (uint64_t)((data >> shift) & 0xFFu);
            return AE_OK;
        case 16:
            *Value = (uint64_t)((data >> shift) & 0xFFFFu);
            return AE_OK;
        case 32:
            *Value = (uint64_t)data;
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg,
                                        UINT64 Value, UINT32 Width)
{
    if (!PciId) {
        return AE_BAD_PARAMETER;
    }
    uint32_t addr = osl_pci_address(PciId, Reg);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));

    uint32_t data;
    __asm__ volatile ("inl %1, %0" : "=a"(data) : "Nd"((uint16_t)0xCFC));

    uint32_t shift = (Reg & 3u) * 8u;
    switch (Width) {
        case 8:
            data = (data & ~(0xFFu << shift)) | (((uint32_t)Value & 0xFFu) << shift);
            break;
        case 16:
            data = (data & ~(0xFFFFu << shift)) | (((uint32_t)Value & 0xFFFFu) << shift);
            break;
        case 32:
            data = (uint32_t)Value;
            break;
        default:
            return AE_BAD_PARAMETER;
    }

    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("outl %0, %1" :: "a"(data), "Nd"((uint16_t)0xCFC));
    return AE_OK;
}

/* ============================================================
 * 杂项
 * ============================================================ */
BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length)
{
    (void)Pointer;
    (void)Length;
    return TRUE;
}

BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length)
{
    (void)Pointer;
    (void)Length;
    return TRUE;
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info)
{
    (void)Info;

    if (Function == ACPI_SIGNAL_FATAL) {
        printk("[ACPICA] FATAL firmware signal, halting\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue)
{
    (void)SleepState;
    (void)RegaValue;
    (void)RegbValue;
    return AE_OK;
}

void AcpiOsPrintf(const char *Format, ...)
{
    va_list args;
    va_start(args, Format);
    osl_vprintf_impl(Format, args);
    va_end(args);
}

void AcpiOsVprintf(const char *Format, va_list Args)
{
    osl_vprintf_impl(Format, Args);
}

void AcpiOsRedirectOutput(void *Destination)
{
    (void)Destination;
}
