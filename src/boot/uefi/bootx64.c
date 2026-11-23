/* ============================================================
 * bootx64.c - SaylterOS UEFI 引导器
 * ------------------------------------------------------------
 * 固件把本程序作为 EFI\BOOT\BOOTX64.EFI 启动。它负责：
 *   1. 用 GOP（Graphics Output Protocol）设置 32bpp 线性帧缓冲，
 *      UEFI 下不调用任何 VBE/INT10 BIOS 服务；
 *   2. 从 ESP 读取 EFI\SaylterOS\kernel.bin 到 0x100000；
 *   3. 把帧缓冲信息写入内核约定的 0x7DF0（与 pm.asm 同一布局，
 *      内核 vbe_init 无需区分 VBE/GOP）；
 *   4. 建立 0..4GB 恒等页表，ExitBootServices 后跳入内核。
 *
 * 只使用 UEFI 规范定义的最小协议子集，不链接任何 C 运行时。
 * 串口日志走 COM1，方便 QEMU -serial 观察 UEFI 启动过程。
 * ============================================================ */

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef UINT64             UINTN;
typedef long long          INT64;
typedef UINTN              EFI_STATUS;
typedef void              *EFI_HANDLE;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0ULL

#define EFI_ERROR(x) ((INT64)(x) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- 文件协议 ---- */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **out,
                              UINT16 *name, UINT64 mode, UINT64 attr);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *self);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *self);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *self, UINTN *size, void *buf);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *self, UINTN *size, void *buf);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *self, UINT64 *pos);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *self, UINT64 pos);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *self, EFI_GUID *info_type,
                                 UINTN *size, void *buf);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *self, EFI_GUID *info_type,
                                 UINTN size, void *buf);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *self);
};

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *self, EFI_FILE_PROTOCOL **root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT8  CreateTime[16];
    UINT8  LastAccessTime[16];
    UINT8  ModificationTime[16];
    UINT64 Attribute;
    UINT16 FileName[1];
} EFI_FILE_INFO;

/* ---- 加载映像协议（只需要 DeviceHandle） ---- */
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE;

/* ---- GOP ---- */
typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;      /* 0 = RGB, 1 = BGR, 2 = BitMask, 3 = BltOnly */
    UINT32 PixelInformation[4];
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN  SizeOfInfo;
    UINT64 FrameBufferBase;
    UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (EFIAPI *QueryMode)(void *self, UINT32 mode, UINTN *size,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
    EFI_STATUS (EFIAPI *SetMode)(void *self, UINT32 mode);
    EFI_STATUS (EFIAPI *Blt)(void *self, void *blt, UINT32 src_x, UINT32 src_y,
                             UINT32 dst_x, UINT32 dst_y, UINT32 w, UINT32 h,
                             UINT32 delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- 块设备协议（UEFI Block I/O）---- */
typedef struct {
    UINT32 MediaId;
    UINT8  RemovableMedia;
    UINT8  MediaPresent;
    UINT8  LogicalPartition;
    UINT8  ReadOnly;
    UINT8  WriteCached;
    UINT8  Pad[3];
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;
    UINT32 LowestAlignedLba;
    UINT32 LogicalBlocksPerPhysicalBlock;
    UINT32 OptimalTransferLengthGranularity;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(void *self, UINT8 extended);
    EFI_STATUS (EFIAPI *ReadBlocks)(void *self, UINT32 media, UINT64 lba,
                                    UINTN size, void *buf);
    EFI_STATUS (EFIAPI *WriteBlocks)(void *self, UINT32 media, UINT64 lba,
                                     UINTN size, void *buf);
    EFI_STATUS (EFIAPI *FlushBlocks)(void *self);
} EFI_BLOCK_IO_PROTOCOL;

/* ---- Boot Services（字段顺序必须与 UEFI 规范一致） ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;

    EFI_STATUS (EFIAPI *RaiseTPL)(UINTN);
    void       (EFIAPI *RestoreTPL)(UINTN);
    EFI_STATUS (EFIAPI *AllocatePages)(UINT32, UINT32, UINTN, UINT64 *);
    EFI_STATUS (EFIAPI *FreePages)(UINT64, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, void *, UINTN *, UINTN *, UINT32 *);
    EFI_STATUS (EFIAPI *AllocatePool)(UINT32, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    EFI_STATUS (EFIAPI *CreateEvent)(UINT32, UINTN, void *, void *, void **);
    EFI_STATUS (EFIAPI *SetTimer)(void *, UINT64, UINT64);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN, void **, UINTN *);
    EFI_STATUS (EFIAPI *SignalEvent)(void *);
    EFI_STATUS (EFIAPI *CloseEvent)(void *);
    EFI_STATUS (EFIAPI *CheckEvent)(void *);
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *ReinstallProtocolInterface)(EFI_HANDLE, EFI_GUID *, void *, void *);
    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE, EFI_GUID *, void *);
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved;
    EFI_STATUS (EFIAPI *RegisterProtocolNotify)(EFI_GUID *, void *, void **);
    EFI_STATUS (EFIAPI *LocateHandle)(UINT32, EFI_GUID *, void *, UINTN *, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *LocateDevicePath)(EFI_GUID *, EFI_HANDLE *, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *InstallConfigurationTable)(EFI_GUID *, void *);
    EFI_STATUS (EFIAPI *LoadImage)(UINT32, EFI_HANDLE, void *, void *, UINTN, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, UINT16 **);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, UINT16 *);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    EFI_STATUS (EFIAPI *GetNextMonotonicCount)(UINT64 *);
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, UINT16 *);
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE, EFI_HANDLE *, void *, UINT32);
    EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE);
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE, EFI_GUID *, void **, EFI_HANDLE, EFI_HANDLE, UINT32);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE, EFI_GUID *, EFI_HANDLE, EFI_HANDLE);
    EFI_STATUS (EFIAPI *OpenProtocolInformation)(EFI_HANDLE, EFI_GUID *, void ***, UINTN *);
    EFI_STATUS (EFIAPI *ProtocolsPerHandle)(EFI_HANDLE, EFI_GUID ***, UINTN *);
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(UINT32, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *, ...);
    EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE, ...);
    EFI_STATUS (EFIAPI *CalculateCrc32)(void *, UINTN, UINT32 *);
    void       (EFIAPI *CopyMem)(void *, void *, UINTN);
    void       (EFIAPI *SetMem)(void *, UINTN, UINT8);
    EFI_STATUS (EFIAPI *CreateEventEx)(UINT32, UINTN, void *, void *, void **);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    UINT16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ---- 全局 ---- */
static EFI_SYSTEM_TABLE *g_st;

static void serial_puts(const char *s);
static void serial_hex(UINT64 v);

static const EFI_GUID gop_guid =
    { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static const EFI_GUID fs_guid =
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID loaded_image_guid =
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID file_info_guid =
    { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID block_io_guid =
    { 0x964E5B21, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID acpi20_guid =
    { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
static const EFI_GUID acpi10_guid =
    { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* UEFI 的 ACPI 表只通过 ConfigurationTable 暴露，不在 EBDA。
 * 内核 acpi.c 只扫 EBDA/0xE0000，因此这里把 RSDP 复制到 0xE0000，
 * 否则 ACPI 电源管理、SMP MADT 全部不可用。 */
static void *g_acpi_rsdp;
static UINT32 g_acpi_len;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (int i = 0; i < 16; i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static void find_acpi_table(void) {
    if (!g_st->ConfigurationTable || g_st->NumberOfTableEntries == 0) {
        serial_puts("[UEFI] no UEFI configuration table\n");
        return;
    }
    EFI_CONFIGURATION_TABLE *ct = (EFI_CONFIGURATION_TABLE *)g_st->ConfigurationTable;

    serial_puts("[UEFI] config tables=");
    serial_hex(g_st->NumberOfTableEntries);
    serial_puts("\n");
    for (UINTN i = 0; i < g_st->NumberOfTableEntries && i < 16; i++) {
        serial_puts("[UEFI]   cfg ");
        serial_hex(ct[i].VendorGuid.Data1);
        serial_puts(" table=");
        serial_hex((UINT64)(UINTN)ct[i].VendorTable);
        serial_puts("\n");
    }

    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        if (guid_eq(&ct[i].VendorGuid, &acpi20_guid) && ct[i].VendorTable) {
            g_acpi_rsdp = ct[i].VendorTable;
            g_acpi_len = 36;
            serial_puts("[UEFI] ACPI 2.0 RSDP found\n");
            return;
        }
    }
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        if (guid_eq(&ct[i].VendorGuid, &acpi10_guid) && ct[i].VendorTable) {
            g_acpi_rsdp = ct[i].VendorTable;
            g_acpi_len = 20;
            serial_puts("[UEFI] ACPI 1.0 RSDP found\n");
            return;
        }
    }
    serial_puts("[UEFI] ACPI RSDP not present\n");
}

static void relocate_acpi_table(void) {
    if (!g_acpi_rsdp || g_acpi_len == 0) return;
    if ((UINT64)(UINTN)g_acpi_rsdp >= 0x100000000ULL) return;

    UINT8 *src = (UINT8 *)g_acpi_rsdp;
    UINT8 *dst = (UINT8 *)(UINTN)0xE0000;
    for (UINT32 i = 0; i < g_acpi_len; i++) dst[i] = src[i];
}

/* ---------------- 串口日志 ---------------- */

static inline void outb(UINT16 port, UINT8 v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);      /* 115200 */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void serial_putc(char c) {
    for (int i = 0; i < 100000 && (inb(0x3F8 + 5) & 0x20) == 0; i++) { }
    outb(0x3F8, (UINT8)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_hex(UINT64 v) {
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) serial_putc(hex[(v >> i) & 0xF]);
}

/* ---------------- 基础内存操作 ---------------- */

static void mem_zero(void *dst, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    while (n--) *d++ = 0;
}

/* ---------------- 内核启动参数（0x7DF0，与 pm.asm 一致） ---------------- */

typedef struct __attribute__((packed)) {
    UINT16 magic;
    UINT32 framebuffer;
    UINT16 width;
    UINT16 height;
    UINT8  bpp;
    UINT16 pitch;
} uefi_boot_video;

/* ---------------- 页表 ---------------- */
/* 内核 vm.c 固定使用 0x70000/0x71000/0x72000 这套物理页表，
 * 引导器必须把恒等映射建在同一位置，否则 demand paging 会失败。 */
#define PML4_PHYS 0x70000ULL
#define PDPT_PHYS 0x71000ULL
#define PD_PHYS   0x72000ULL
#define IDT_PHYS  0x60000ULL

static void paging_init(void) {
    UINT64 *pml4 = (UINT64 *)(UINTN)PML4_PHYS;
    UINT64 *pdpt = (UINT64 *)(UINTN)PDPT_PHYS;
    UINT64 *pd = (UINT64 *)(UINTN)PD_PHYS;

    mem_zero(pml4, 4096);
    mem_zero(pdpt, 4096);
    mem_zero(pd, 4 * 4096);

    /* 0..4GB，每 2MB 一个页。P|RW|PS。 */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 512; j++) {
            UINT64 phys = ((UINT64)i * 512ULL + (UINT64)j) * 0x200000ULL;
            pd[i * 512 + j] = phys | 0x83ULL;
        }
        pdpt[i] = (UINT64)(UINTN)&pd[i * 512] | 0x03ULL;
    }
    pml4[0] = (UINT64)(UINTN)pdpt | 0x03ULL;
}

/* ---------------- 中断环境 ---------------- */

typedef struct __attribute__((packed)) {
    UINT16 off_lo;
    UINT16 sel;
    UINT8  ist;
    UINT8  type;
    UINT16 off_mid;
    UINT32 off_hi;
    UINT32 zero;
} idt_gate;

typedef struct __attribute__((packed)) {
    UINT16 limit;
    UINT64 base;
} idtr_t;

extern void isr_default(void);
extern void isr_timer(void);
extern void isr_keyboard(void);
extern void isr_sci(void);
extern void isr_spurious(void);
extern void isr_ipi(void);
extern void isr_exc14(void);
extern void isr_exc_halt(void);

static void idt_set(int vec, UINT64 handler, UINT16 cs) {
    idt_gate *g = &((idt_gate *)(UINTN)IDT_PHYS)[vec];
    g->off_lo = (UINT16)(handler & 0xFFFF);
    g->sel = cs;
    g->ist = 0;
    g->type = 0x8E;   /* P=1, DPL=0, 64 位中断门 */
    g->off_mid = (UINT16)((handler >> 16) & 0xFFFF);
    g->off_hi = (UINT32)(handler >> 32);
    g->zero = 0;
}

static void idt_init(void) {
    UINT16 cs = 0;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    for (int i = 0; i < 256; i++) {
        idt_set(i, (UINT64)(UINTN)&isr_default, cs);
    }
    for (int i = 0; i < 32; i++) {
        idt_set(i, (UINT64)(UINTN)&isr_exc_halt, cs);
    }
    idt_set(14, (UINT64)(UINTN)&isr_exc14, cs);
    idt_set(0x20, (UINT64)(UINTN)&isr_timer, cs);
    idt_set(0x21, (UINT64)(UINTN)&isr_keyboard, cs);
    idt_set(0x29, (UINT64)(UINTN)&isr_sci, cs);
    idt_set(0xF0, (UINT64)(UINTN)&isr_ipi, cs);
    idt_set(0xFF, (UINT64)(UINTN)&isr_spurious, cs);

    idtr_t idtr;
    idtr.limit = (UINT16)(256 * 16 - 1);
    idtr.base = IDT_PHYS;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

/* PIC 8259 重映射，与 pm.asm 完全一致：
 * IRQ0/1/2 与 IRQ9 开放，其余屏蔽。 */
static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xF8);
    outb(0xA1, 0xFD);
}

/* PIT 通道 0：BIOS 下 SeaBIOS 保持 18.2Hz（divisor=65536），
 * 内核 tty_tick 直接按 1.193182MHz 计数计时。OVMF 会把 PIT
 * 重编程成 1000Hz，回绕周期变短 55 倍，表现就是光标闪烁飞快。
 * 这里恢复成与 BIOS 路径相同的 18.2Hz。 */
static void pit_init(void) {
    outb(0x43, 0x34);    /* 通道0，先低后高，模式2（rate generator） */
    outb(0x40, 0x00);
    outb(0x40, 0x00);    /* divisor = 65536 */
}

/* 恢复 x87/SSE 到引导默认状态：ClearType 光栅化依赖一致的
 * 舍入模式与 MXCSR（FTZ/DAZ 关闭）。 */
static void fpu_init(void) {
    UINT64 cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~0x0CULL;             /* 清 EM/TS */
    cr0 |= 0x02ULL;              /* MP = 1 */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x600ULL;             /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("fninit");
    UINT32 mxcsr = 0x1F80u;      /* 默认：四舍五入，屏蔽所有异常 */
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
}

/* GOP 模式下立即擦掉固件 logo，让用户尽快看到系统画面。 */
static void gop_clear(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    if (!gop || !gop->Mode || gop->Mode->FrameBufferBase == 0) return;
    volatile UINT32 *fb = (volatile UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    UINTN px = (UINTN)gop->Mode->FrameBufferSize / 4;
    for (UINTN i = 0; i < px; i++) fb[i] = 0;
}

/* ---- MTRR：把 GOP 线性帧缓冲标成 WC（写合并）----
 * OVMF 的 LFB 默认是 UC，内核 TTY 启动画面逐像素直写会慢到几十秒。
 * 与 BIOS 路径保持一致，必须在跳转内核前设置好。 */
static UINT64 rdmsr(UINT32 msr) {
    UINT32 lo = 0, hi = 0;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

static void wrmsr(UINT32 msr, UINT64 v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((UINT32)v), "d"((UINT32)(v >> 32)));
}

#define MTRR_CAP        0xFEu
#define MTRR_DEF_TYPE   0x2FFu
#define MTRR_PHYSBASE0  0x200u

static void mtrr_wc_lfb(UINT64 fb, UINT64 bytes) {
    UINT64 cap = rdmsr(MTRR_CAP);
    UINT32 vcnt = (UINT32)(cap & 0xFFu);
    if (vcnt == 0 || vcnt > 16) return;

    UINT64 def = rdmsr(MTRR_DEF_TYPE);
    wrmsr(MTRR_DEF_TYPE, def | (1ULL << 11));

    UINT64 size = 1ULL << 20;
    while (size < bytes && size < (1ULL << 30)) size <<= 1;
    UINT64 base = fb & ~(size - 1);

    /* 先禁用与 LFB 区间重叠的旧范围（固件可能把它设成 UC）。 */
    for (UINT32 i = 0; i < vcnt; i++) {
        UINT64 m = rdmsr(MTRR_PHYSBASE0 + i * 2 + 1);
        if ((m & (1ULL << 11)) == 0) continue;
        UINT64 b = rdmsr(MTRR_PHYSBASE0 + i * 2) & ~0xFFULL;
        UINT64 sz = 0;
        for (int bit = 12; bit < 52; bit++) {
            if ((m & (1ULL << bit)) == 0) {
                sz = 1ULL << bit;
                break;
            }
        }
        if (sz == 0 || b + sz <= base || base + size <= b) continue;
        wrmsr(MTRR_PHYSBASE0 + i * 2 + 1, 0);
    }

    /* 找一个空闲范围写 WC。 */
    for (UINT32 i = 0; i < vcnt; i++) {
        UINT64 m = rdmsr(MTRR_PHYSBASE0 + i * 2 + 1);
        if (m & (1ULL << 11)) continue;
        UINT64 mask = (~(size - 1) & 0x000FFFFFFFFFF000ULL) | (1ULL << 11);
        wrmsr(MTRR_PHYSBASE0 + i * 2, base | 1ULL);   /* 类型 1 = WC */
        wrmsr(MTRR_PHYSBASE0 + i * 2 + 1, mask);
        serial_puts("[UEFI] MTRR WC for framebuffer, base=");
        serial_hex(base);
        serial_puts(" size=");
        serial_hex(size);
        serial_puts("\n");
        return;
    }
}

/* ---------------- GOP 模式选择 ---------------- */

struct want_mode {
    UINT32 w;
    UINT32 h;
};

static const struct want_mode g_want[] = {
    { 1600, 900 },
    { 1440, 900 },
    { 1280, 800 },
    { 1280, 720 },
    { 1024, 768 },
    { 800,  600 },
};

static int gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **out_info) {
    if (!gop || !gop->Mode) return 0;

    serial_puts("[UEFI] GOP modes: max=");
    serial_hex(gop->Mode->MaxMode);
    serial_puts(" current=");
    serial_hex(gop->Mode->Mode);
    serial_puts("\n");
    for (UINT32 m = 0; m < gop->Mode->MaxMode && m < 24; m++) {
        UINTN sz = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        EFI_STATUS st = gop->QueryMode(gop, m, &sz, &info);
        if (EFI_ERROR(st) || !info) {
            serial_puts("[UEFI]   mode ");
            serial_hex(m);
            serial_puts(" <query failed>\n");
            continue;
        }
        serial_puts("[UEFI]   mode ");
        serial_hex(m);
        serial_puts(" ");
        serial_hex(info->HorizontalResolution);
        serial_puts("x");
        serial_hex(info->VerticalResolution);
        serial_puts(" pf=");
        serial_hex(info->PixelFormat);
        serial_puts(" stride=");
        serial_hex(info->PixelsPerScanLine);
        serial_puts("\n");
    }

    UINTN nr = sizeof(g_want) / sizeof(g_want[0]);
    for (UINTN wi = 0; wi < nr; wi++) {
        for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
            UINTN sz = 0;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
            EFI_STATUS st = gop->QueryMode(gop, m, &sz, &info);
            if (EFI_ERROR(st) || !info) continue;
            if (info->HorizontalResolution != g_want[wi].w) continue;
            if (info->VerticalResolution != g_want[wi].h) continue;

            st = gop->SetMode(gop, m);
            if (EFI_ERROR(st)) {
                serial_puts("[UEFI] SetMode failed for ");
                serial_hex(m);
                serial_puts("\n");
                continue;
            }
            *out_info = gop->Mode->Info;
            return 1;
        }
    }

    /* 没有精确命中：退而求其次，选最大的模式（不超过 1920x1200）。 */
    UINT32 best = 0;
    UINT32 best_px = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN sz = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)) || !info) continue;
        UINT32 px = info->HorizontalResolution * info->VerticalResolution;
        if (px > 1920u * 1200u) continue;
        if (px > best_px) { best_px = px; best = m; }
    }
    if (best_px != 0) {
        if (!EFI_ERROR(gop->SetMode(gop, best))) {
            *out_info = gop->Mode->Info;
            return 1;
        }
    }

    /* 最后兜底：固件启动画面已经设置过模式，直接沿用当前模式。 */
    if (gop->Mode->Info && gop->Mode->FrameBufferBase) {
        serial_puts("[UEFI] GOP: fallback to current mode\n");
        *out_info = gop->Mode->Info;
        return 2;
    }
    return 0;
}

/* ---------------- 内核加载 ---------------- */

#define KERNEL_LOAD_ADDR 0x100000ULL

static int load_kernel(EFI_HANDLE image, void **out_base, UINTN *out_size) {
    void *li_void = 0;
    EFI_STATUS st = g_st->BootServices->HandleProtocol(image, (EFI_GUID *)&loaded_image_guid,
                                                      &li_void);
    if (EFI_ERROR(st) || !li_void) {
        serial_puts("[UEFI] loaded image protocol failed\n");
        return 0;
    }
    EFI_LOADED_IMAGE *li = (EFI_LOADED_IMAGE *)li_void;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    st = g_st->BootServices->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&fs_guid, (void **)&fs);
    if (EFI_ERROR(st) || !fs) {
        serial_puts("[UEFI] simple file system not found\n");
        return 0;
    }

    EFI_FILE_PROTOCOL *root = 0;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st) || !root) {
        serial_puts("[UEFI] open volume failed\n");
        return 0;
    }

    /* ESP 内使用 8.3 短名目录（FAT16 无 LFN）：EFI\SAYLTER\kernel.bin */
    static UINT16 path[] = L"\\EFI\\SAYLTER\\kernel.bin";
    EFI_FILE_PROTOCOL *file = 0;
    st = root->Open(root, &file, path, 1 /* READ */, 0);
    if (EFI_ERROR(st) || !file) {
        serial_puts("[UEFI] kernel.bin not found on ESP\n");
        return 0;
    }

    UINTN isz = 256;
    UINT8 ibuf[256];
    st = file->GetInfo(file, (EFI_GUID *)&file_info_guid, &isz, ibuf);
    if (EFI_ERROR(st)) {
        serial_puts("[UEFI] kernel.bin get info failed\n");
        file->Close(file);
        return 0;
    }
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)ibuf;
    UINTN size = (UINTN)fi->FileSize;
    if (size == 0 || size > 8u * 1024u * 1024u) {
        serial_puts("[UEFI] kernel.bin size invalid\n");
        file->Close(file);
        return 0;
    }

    UINTN want = size;
    st = file->Read(file, &want, (void *)(UINTN)KERNEL_LOAD_ADDR);
    file->Close(file);
    if (EFI_ERROR(st) || want != size) {
        serial_puts("[UEFI] kernel.bin read failed\n");
        return 0;
    }

    serial_puts("[UEFI] kernel.bin loaded, size=");
    serial_hex(size);
    serial_puts("\n");

    *out_base = (void *)(UINTN)KERNEL_LOAD_ADDR;
    *out_size = size;
    return 1;
}

/* ESP 里没有 kernel.bin 时，直接从裸盘 LBA19 读取。
 * 镜像布局由 pm.asm/build.ps1 固定：LBA19 起是内核 flat binary，
 * 最多读到 2MB（远小于内核暂存区上界与 4MB 堆起点）。 */
#define KERNEL_LBA       19ULL
#define KERNEL_RAW_BYTES (2ULL * 1024ULL * 1024ULL)

static int load_kernel_raw(void **out_base, UINTN *out_size) {
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS st = bs->LocateHandleBuffer(2 /* ByProtocol */, (EFI_GUID *)&block_io_guid,
                                           0, &count, &handles);
    if (EFI_ERROR(st) || !handles) {
        serial_puts("[UEFI] no block io handles\n");
        return 0;
    }

    /* 选 LastBlock 最大的块设备：分区设备 LogicalPartition=true 会被跳过，
     * 剩下的就是整块磁盘。 */
    EFI_BLOCK_IO_PROTOCOL *disk = 0;
    UINT64 best_last = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], (EFI_GUID *)&block_io_guid,
                                         (void **)&bio)) || !bio || !bio->Media) {
            continue;
        }
        if (!bio->Media->MediaPresent || bio->Media->LogicalPartition) continue;
        if (bio->Media->LastBlock > best_last) {
            best_last = bio->Media->LastBlock;
            disk = bio;
        }
    }
    bs->FreePool(handles);
    if (!disk || disk->Media->BlockSize == 0) {
        serial_puts("[UEFI] no whole-disk block io\n");
        return 0;
    }

    UINT64 bytes = KERNEL_RAW_BYTES;
    UINT64 avail = (best_last - KERNEL_LBA + 1) * disk->Media->BlockSize;
    if (bytes > avail) bytes = avail;
    bytes &= ~((UINT64)disk->Media->BlockSize - 1);
    if (bytes < 512 * 1024) {
        serial_puts("[UEFI] raw kernel area too small\n");
        return 0;
    }

    st = disk->ReadBlocks(disk, disk->Media->MediaId, KERNEL_LBA, (UINTN)bytes,
                          (void *)(UINTN)KERNEL_LOAD_ADDR);
    if (EFI_ERROR(st)) {
        serial_puts("[UEFI] raw kernel read failed\n");
        return 0;
    }

    serial_puts("[UEFI] kernel raw loaded from LBA19, bytes=");
    serial_hex(bytes);
    serial_puts("\n");

    *out_base = (void *)(UINTN)KERNEL_LOAD_ADDR;
    *out_size = (UINTN)bytes;
    return 1;
}

/* ---------------- 启动服务收尾并跳转 ---------------- */

static UINT8 g_mmap_buf[128 * 1024] __attribute__((aligned(16)));

/* Win64：entry=rcx、cr3=rdx、rsp=r8。切换页表与栈后直接跳内核，
 * 不再返回固件环境。 */
static void __attribute__((naked, noreturn)) enter_kernel(UINT64, UINT64, UINT64) {
    __asm__ volatile (
        "cli\n\t"
        "mov %rdx, %cr3\n\t"
        "mov %r8, %rsp\n\t"
        "xor %ebp, %ebp\n\t"
        "sti\n\t"
        "jmp *%rcx\n\t"
    );
}

static int exit_and_jump(void (*entry)(void)) {
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    bs->SetWatchdogTimer(0, 0, 0, 0);

    for (int try = 0; try < 4; try++) {
        UINTN size = sizeof(g_mmap_buf);
        UINTN key = 0;
        UINTN dsz = 0;
        UINT32 dver = 0;
        EFI_STATUS st = bs->GetMemoryMap(&size, g_mmap_buf, &key, &dsz, &dver);
        if (EFI_ERROR(st)) {
            serial_puts("[UEFI] GetMemoryMap failed\n");
            return 0;
        }
        st = bs->ExitBootServices(g_st, key);
        if (!EFI_ERROR(st)) {
            serial_puts("[UEFI] boot services exited, jumping to kernel\n");
            /* 固件已停止，低地址页表/IDT 可以安全重建。 */
            paging_init();
            idt_init();
            pic_remap();
            pit_init();
            fpu_init();
            enter_kernel((UINT64)(UINTN)entry, PML4_PHYS, 0x3FF000ULL);
        }
    }
    serial_puts("[UEFI] ExitBootServices failed\n");
    return 0;
}

/* ---------------- 入口 ---------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    serial_init();
    serial_puts("\n[UEFI] SaylterOS UEFI loader v0.3 Beta2\n");
    g_st = st;
    find_acpi_table();
    /* 把 RSDP 物理地址交给内核（0x7DC0 为引导器预留槽）。
     * UEFI 下 0xE0000 区域不可靠，直接传地址最稳妥。 */
    if (g_acpi_rsdp) {
        volatile UINT64 *slot = (volatile UINT64 *)(UINTN)0x7DC0;
        *slot = (UINT64)(UINTN)g_acpi_rsdp;
    }
    /* 标记 UEFI 引导：内核据此跳过依赖实模式 trampoline 的 AP 启动。 */
    *(volatile UINT32 *)(UINTN)0x7DBC = 1;
    /* 双保险：同时复制到内核会扫描的 0xE0000。 */
    relocate_acpi_table();

    /* GOP：UEFI 下只用图形输出协议，没有任何 VBE/INT10。 */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS gs = st->BootServices->LocateProtocol((EFI_GUID *)&gop_guid, 0, (void **)&gop);
    if (EFI_ERROR(gs) || !gop) {
        serial_puts("[UEFI] GOP not available\n");
        return 1;
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
    if (!gop_set_mode(gop, &info) || !info) {
        serial_puts("[UEFI] GOP set mode failed\n");
        return 1;
    }

    serial_puts("[UEFI] GOP mode ");
    serial_hex(info->HorizontalResolution);
    serial_puts("x");
    serial_hex(info->VerticalResolution);
    serial_puts(" pf=");
    serial_hex(info->PixelFormat);
    serial_puts(" fb=");
    serial_hex(gop->Mode->FrameBufferBase);
    serial_puts(" pitch=");
    serial_hex((UINT64)info->PixelsPerScanLine * 4);
    serial_puts("\n");

    if ((info->PixelFormat != 0 && info->PixelFormat != 1) ||
        info->HorizontalResolution == 0 || info->VerticalResolution == 0 ||
        gop->Mode->FrameBufferBase == 0 || gop->Mode->FrameBufferBase >= 0x100000000ULL) {
        serial_puts("[UEFI] GOP framebuffer unusable\n");
        return 1;
    }

    /* 交给内核的帧缓冲描述（0x7DF0，和 pm.asm 完全一致）。 */
    uefi_boot_video *v = (uefi_boot_video *)(UINTN)0x7DF0;
    v->magic = 0xA1B2;
    v->framebuffer = (UINT32)gop->Mode->FrameBufferBase;
    v->width = (UINT16)info->HorizontalResolution;
    v->height = (UINT16)info->VerticalResolution;
    v->bpp = 32;
    v->pitch = (UINT16)(info->PixelsPerScanLine * 4);

    /* 先开写合并再清屏，否则 1600x900 的 UC 直写会卡几十秒。 */
    mtrr_wc_lfb(gop->Mode->FrameBufferBase, (UINT64)v->pitch * v->height);
    gop_clear(gop);

    void *kbase = 0;
    UINTN ksize = 0;
    if (!load_kernel(image, &kbase, &ksize) &&
        !load_kernel_raw(&kbase, &ksize)) {
        return 1;
    }

    return exit_and_jump((void (*)(void))(UINTN)KERNEL_LOAD_ADDR) ? 0 : 1;
}
