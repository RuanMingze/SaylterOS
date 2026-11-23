/* ============================================================
 * enroll.c - 把 SaylterOS 三大证书写入 UEFI 固件变量
 * ------------------------------------------------------------
 * 从 ESP 读取：
 *   \EFI\SAYLTER\DB.CER
 *   \EFI\SAYLTER\KEK.CER
 *   \EFI\SAYLTER\PK.CER
 *
 * 按 db -> KEK -> PK 的顺序写入。PK 一旦写入，固件退出
 * Setup Mode；先写低层级可以全程免签名认证。
 *
 * 仅在 Setup Mode（固件里没有 PK）下能成功。写入后重启即可
 * 进入 Secure Boot 模式。
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

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    void *LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(UINT16 *name, EFI_GUID *guid,
                                     UINT32 *attr, UINTN *size, void *data);
    EFI_STATUS (EFIAPI *GetNextVariableName)(UINTN *size, UINT16 *name, EFI_GUID *guid);
    EFI_STATUS (EFIAPI *SetVariable)(UINT16 *name, EFI_GUID *guid,
                                     UINT32 attr, UINTN size, void *data);
} EFI_RUNTIME_SERVICES;

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
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- 证书签名列表 ---- */
typedef struct {
    EFI_GUID SignatureType;
    UINT32   SignatureListSize;
    UINT32   SignatureHeaderSize;
    UINT32   SignatureSize;
    EFI_GUID OwnerGuid;
    UINT8    Data[1];
} __attribute__((packed)) EFI_SIGNATURE_LIST;

static EFI_SYSTEM_TABLE *g_st;

static const EFI_GUID x509_guid =
    { 0xA5C059A1, 0x94E4, 0x4AA7, { 0x87, 0xB5, 0xAB, 0x15, 0x5C, 0x2B, 0xF0, 0x72 } };
static const EFI_GUID global_var_guid =
    { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static const EFI_GUID fs_guid =
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID loaded_image_guid =
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID file_info_guid =
    { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

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
    outb(0x3F8 + 0, 0x01);
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

/* 从 ESP 读取一个证书文件，返回字节数（0 = 失败）。 */
static UINTN read_cert(EFI_FILE_PROTOCOL *root, const UINT16 *path,
                       UINT8 *buf, UINTN cap) {
    EFI_FILE_PROTOCOL *file = 0;
    if (EFI_ERROR(root->Open(root, &file, (UINT16 *)path, 1, 0)) || !file) {
        return 0;
    }

    UINTN isz = 256;
    UINT8 ibuf[256];
    if (EFI_ERROR(file->GetInfo(file, (EFI_GUID *)&file_info_guid, &isz, ibuf))) {
        file->Close(file);
        return 0;
    }
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)ibuf;
    UINTN size = (UINTN)fi->FileSize;
    if (size == 0 || size > cap) {
        file->Close(file);
        return 0;
    }

    UINTN want = size;
    if (EFI_ERROR(file->Read(file, &want, buf)) || want != size) {
        file->Close(file);
        return 0;
    }
    file->Close(file);
    return size;
}

/* 构造 X509 签名列表并写入固件变量。 */
static int enroll_one(const char *name, const UINT16 *var_name,
                      EFI_FILE_PROTOCOL *root, const UINT16 *path) {
    static UINT8 cert[4096];
    static UINT8 list[4096 + 64];

    UINTN len = read_cert(root, path, cert, sizeof(cert));
    if (len == 0) {
        serial_puts("[ENROLL] missing ");
        serial_puts(name);
        serial_puts("\n");
        return 0;
    }
    serial_puts("[ENROLL] cert ");
    serial_puts(name);
    serial_puts(" len=");
    serial_hex(len);
    serial_puts("\n");

    UINT32 sig_size = 16 + (UINT32)len;
    UINT32 list_size = 28 + sig_size;   /* EFI_SIGNATURE_LIST 头 28 字节 */

    EFI_SIGNATURE_LIST *sl = (EFI_SIGNATURE_LIST *)list;
    sl->SignatureType = x509_guid;
    sl->SignatureListSize = list_size;
    sl->SignatureHeaderSize = 0;
    sl->SignatureSize = sig_size;
    sl->OwnerGuid.Data1 = 0;
    sl->OwnerGuid.Data2 = 0;
    sl->OwnerGuid.Data3 = 0;
    for (int i = 0; i < 8; i++) sl->OwnerGuid.Data4[i] = 0;

    UINT8 *p = list + 28 + 16;          /* 跳过列表头与 OwnerGuid */
    for (UINTN i = 0; i < len; i++) p[i] = cert[i];

    EFI_STATUS st = g_st->RuntimeServices->SetVariable((UINT16 *)var_name,
                                                      (EFI_GUID *)&global_var_guid,
                                                      0x07 /* NV|BS|RT */,
                                                      list_size, list);
    serial_puts("[ENROLL] list_size=");
    serial_hex(list_size);
    serial_puts(" sig_size=");
    serial_hex(sig_size);
    serial_puts(" listhdr=");
    serial_hex(sl->SignatureListSize);
    serial_puts("\n");
    serial_puts("[ENROLL] ");
    serial_puts(name);
    serial_puts(" -> ");
    serial_hex((UINT64)st);
    serial_puts("\n");
    return !EFI_ERROR(st);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    serial_init();
    serial_puts("\n[ENROLL] SaylterOS Secure Boot certificate enrollment\n");
    g_st = st;

    {
        UINT32 attr = 0;
        UINTN vsize = 0;
        EFI_STATUS ps = st->RuntimeServices->GetVariable((UINT16 *)L"PK",
                         (EFI_GUID *)&global_var_guid, &attr, &vsize, 0);
        serial_puts("[ENROLL] PK probe status=");
        serial_hex((UINT64)ps);
        serial_puts(" attr=");
        serial_hex(attr);
        serial_puts(" size=");
        serial_hex(vsize);
        serial_puts("\n");

        UINT8 test_val[4] = { 1, 2, 3, 4 };
        EFI_STATUS ts1 = st->RuntimeServices->SetVariable((UINT16 *)L"SaylterV",
                          (EFI_GUID *)&global_var_guid, 0x06, 4, test_val);
        serial_puts("[ENROLL] SetVariable volatile=");
        serial_hex((UINT64)ts1);
        serial_puts(" ptr=");
        serial_hex((UINT64)(UINTN)st->RuntimeServices->SetVariable);
        serial_puts("\n");
        EFI_STATUS ts2 = st->RuntimeServices->SetVariable((UINT16 *)L"SaylterTest",
                          (EFI_GUID *)&global_var_guid, 0x07, 4, test_val);
        serial_puts("[ENROLL] SetVariable nv=");
        serial_hex((UINT64)ts2);
        serial_puts("\n");
    }

    void *li_void = 0;
    if (EFI_ERROR(st->BootServices->HandleProtocol(image, (EFI_GUID *)&loaded_image_guid,
                                                   &li_void)) || !li_void) {
        serial_puts("[ENROLL] loaded image protocol failed\n");
        return 1;
    }
    EFI_LOADED_IMAGE *li = (EFI_LOADED_IMAGE *)li_void;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    if (EFI_ERROR(st->BootServices->HandleProtocol(li->DeviceHandle,
                                                   (EFI_GUID *)&fs_guid,
                                                   (void **)&fs)) || !fs) {
        serial_puts("[ENROLL] no file system on ESP\n");
        return 1;
    }

    EFI_FILE_PROTOCOL *root = 0;
    if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) {
        serial_puts("[ENROLL] open volume failed\n");
        return 1;
    }

    static UINT16 p_db[]  = L"\\EFI\\SAYLTER\\DB.CER";
    static UINT16 p_kek[] = L"\\EFI\\SAYLTER\\KEK.CER";
    static UINT16 p_pk[]  = L"\\EFI\\SAYLTER\\PK.CER";
    static UINT16 v_db[]  = L"db";
    static UINT16 v_kek[] = L"KEK";
    static UINT16 v_pk[]  = L"PK";

    /* 顺序必须 db -> KEK -> PK（PK 最后写，写完退出 Setup Mode）。 */
    int ok_db  = enroll_one("db", v_db, root, p_db);
    int ok_kek = enroll_one("KEK", v_kek, root, p_kek);
    int ok_pk  = enroll_one("PK", v_pk, root, p_pk);

    serial_puts("[ENROLL] done: db=");
    serial_hex((UINT64)ok_db);
    serial_puts(" kek=");
    serial_hex((UINT64)ok_kek);
    serial_puts(" pk=");
    serial_hex((UINT64)ok_pk);
    serial_puts("\n[ENROLL] reboot to activate Secure Boot\n");

    return (ok_db && ok_kek && ok_pk) ? 0 : 1;
}
