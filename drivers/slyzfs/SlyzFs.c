/* ============================================================
 * SlyzFs.c - SaylterOS SlyzFS v3 IFS 驱动（可读可写）
 * ------------------------------------------------------------
 * 卷呈现：Windows 看到的 0x7F 分区挂成单个盘符，根目录下出现
 * SYSTEM / RECOVERY / DATA 三个子卷目录。
 *
 * 挂载识别：直接读卷内 LBA0 的 "SLYSPT2" 分区表，CRC 与各子卷
 * 卷头全部校验通过才挂载；任何异常都返回 UNRECOGNIZED_VOLUME，
 * 不做"尽力而为"的猜测。
 *
 * 防蓝屏：所有磁盘数据先过边界/魔数/CRC/USA 校验；用户缓冲区
 * 一律 Probe + SEH；卷级 ERESOURCE 串行化元数据写；写事务走
 * SLOG，失败即把该子卷降为只读。
 * ============================================================ */

#include "SlyzFs.h"

#ifdef SLZ_SYNTAX_CHECK
/* 仅用于 MinGW/Clang 语法自检；真实构建不开这个宏 */
#define __try if (1)
#define __except(x) else if (0)
#endif

#define U16(p, o) (*(USHORT UNALIGNED *)((PUCHAR)(p) + (o)))
#define U32(p, o) (*(ULONG UNALIGNED *)((PUCHAR)(p) + (o)))
#define U64(p, o) (*(ULONG64 UNALIGNED *)((PUCHAR)(p) + (o)))
#define W16(p, o, v) (*(USHORT UNALIGNED *)((PUCHAR)(p) + (o)) = (USHORT)(v))
#define W32(p, o, v) (*(ULONG UNALIGNED *)((PUCHAR)(p) + (o)) = (ULONG)(v))
#define W64(p, o, v) (*(ULONG64 UNALIGNED *)((PUCHAR)(p) + (o)) = (ULONG64)(v))

#define SLZ_LOG_MAGIC   0x474F4C53u
#define SLZ_ATTR_END    0xFFFFFFFFu

/* WDK 不同版本对 DISK_EXTENT / FILE_FS_*_SIZE_INFORMATION 的字段名
 * 有差异（PartitionLength/ExtentLength、BytesPerAllocationUnit/
 * BytesPerSector），这里按固定布局自己定义，避免绑定头文件命名。 */
typedef struct _SLZ_DISK_EXTENT {
    ULONG         DiskNumber;
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER Length;
} SLZ_DISK_EXTENT;

typedef struct _SLZ_VOLUME_EXTENTS {
    ULONG           NumberOfDiskExtents;
    SLZ_DISK_EXTENT Extents[1];
} SLZ_VOLUME_EXTENTS;

typedef struct _SLZ_FS_SIZE_INFO {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG         SectorsPerAllocationUnit;
    ULONG         BytesPerAllocationUnit;
} SLZ_FS_SIZE_INFO;

typedef struct _SLZ_FS_FULL_SIZE_INFO {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG         SectorsPerAllocationUnit;
    ULONG         BytesPerAllocationUnit;
} SLZ_FS_FULL_SIZE_INFO;

static PDEVICE_OBJECT g_ControlDevice;
static PDRIVER_OBJECT g_DriverObject;

/* ============================================================
 * 基础工具
 * ============================================================ */

ULONG32 SlzCrc32(const VOID *Buffer, ULONG Length)
{
    const UCHAR *p = (const UCHAR *)Buffer;
    ULONG32 c = 0xFFFFFFFFu;
    ULONG i, k;

    for (i = 0; i < Length; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) {
            if (c & 1) c = (c >> 1) ^ 0xEDB88320u;
            else c >>= 1;
        }
    }
    return c ^ 0xFFFFFFFFu;
}

ULONG SlzUtf8ToWide(const UCHAR *Src, ULONG SrcLen, PWCHAR Dst, ULONG DstChars)
{
    ULONG i = 0, o = 0;

    while (i < SrcLen) {
        UCHAR b = Src[i];
        ULONG cp;
        ULONG need;

        if (b < 0x80) {
            cp = b;
            need = 1;
        } else if ((b & 0xE0) == 0xC0 && b >= 0xC2) {
            cp = b & 0x1F;
            need = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            need = 3;
        } else if ((b & 0xF8) == 0xF0 && b <= 0xF4) {
            cp = b & 0x07;
            need = 4;
        } else {
            return (ULONG)-1;
        }
        if (i + need > SrcLen) return (ULONG)-1;
        {
            ULONG j;
            for (j = 1; j < need; j++) {
                UCHAR cb = Src[i + j];
                if ((cb & 0xC0) != 0x80) return (ULONG)-1;
                cp = (cp << 6) | (cb & 0x3F);
            }
        }
        if (need == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)))
            return (ULONG)-1;
        if (need == 4 && (cp < 0x10000 || cp > 0x10FFFF))
            return (ULONG)-1;

        if (cp <= 0xFFFF) {
            if (o + 1 >= DstChars) return (ULONG)-1;
            Dst[o++] = (WCHAR)cp;
        } else {
            if (o + 2 >= DstChars) return (ULONG)-1;
            cp -= 0x10000;
            Dst[o++] = (WCHAR)(0xD800 + (cp >> 10));
            Dst[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        }
        i += need;
    }
    Dst[o] = 0;
    return o;
}

ULONG SlzWideToUtf8(const WCHAR *Src, ULONG SrcChars, UCHAR *Dst, ULONG DstBytes)
{
    ULONG i = 0, o = 0;

    while (i < SrcChars) {
        ULONG cp = Src[i++];
        UCHAR tmp[4];
        ULONG n = 0, j;

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i >= SrcChars) return (ULONG)-1;
            {
                ULONG lo = Src[i];
                if (lo < 0xDC00 || lo > 0xDFFF) return (ULONG)-1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return (ULONG)-1;
        }

        if (cp < 0x80) {
            tmp[n++] = (UCHAR)cp;
        } else if (cp < 0x800) {
            tmp[n++] = (UCHAR)(0xC0 | (cp >> 6));
            tmp[n++] = (UCHAR)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            tmp[n++] = (UCHAR)(0xE0 | (cp >> 12));
            tmp[n++] = (UCHAR)(0x80 | ((cp >> 6) & 0x3F));
            tmp[n++] = (UCHAR)(0x80 | (cp & 0x3F));
        } else {
            tmp[n++] = (UCHAR)(0xF0 | (cp >> 18));
            tmp[n++] = (UCHAR)(0x80 | ((cp >> 12) & 0x3F));
            tmp[n++] = (UCHAR)(0x80 | ((cp >> 6) & 0x3F));
            tmp[n++] = (UCHAR)(0x80 | (cp & 0x3F));
        }
        if (o + n > DstBytes) return (ULONG)-1;
        for (j = 0; j < n; j++) Dst[o++] = tmp[j];
    }
    return o;
}

BOOLEAN SlzWildcardMatch(const WCHAR *Pattern, ULONG PatternChars,
                         const WCHAR *Name, ULONG NameChars)
{
    ULONG pi = 0, ni = 0, star = (ULONG)-1, mark = 0;

    while (ni < NameChars) {
        if (pi < PatternChars) {
            WCHAR pc = RtlUpcaseUnicodeChar(Pattern[pi]);
            WCHAR nc = RtlUpcaseUnicodeChar(Name[ni]);
            if (pc == '?' || pc == nc) { pi++; ni++; continue; }
            if (pc == '*') { star = pi++; mark = ni; continue; }
        }
        if (star != (ULONG)-1) { pi = star + 1; ni = ++mark; continue; }
        return FALSE;
    }
    while (pi < PatternChars && Pattern[pi] == '*') pi++;
    return pi == PatternChars;
}

LARGE_INTEGER SlzUnixToTime(ULONG Seconds)
{
    LARGE_INTEGER r;
    r.QuadPart = (LONGLONG)Seconds * 10000000LL + 116444736000000000LL;
    return r;
}

/* ============================================================
 * USA（MFT 记录防撕裂写）
 * ============================================================ */

BOOLEAN SlzRestoreUsa(PUCHAR Buf)
{
    USHORT off = U16(Buf, 4);
    USHORT cnt = U16(Buf, 6);
    USHORT usn;
    ULONG i;

    if (cnt == 0) return TRUE;
    if (cnt != 9) return FALSE;
    if ((ULONG)off + (ULONG)cnt * 2 > SLZ_SECTOR_SIZE) return FALSE;
    usn = U16(Buf, off);
    for (i = 1; i < cnt; i++) {
        if (U16(Buf, i * SLZ_SECTOR_SIZE - 2) != usn) return FALSE;
    }
    for (i = 1; i < cnt; i++) {
        W16(Buf, i * SLZ_SECTOR_SIZE - 2, U16(Buf, off + i * 2));
    }
    return TRUE;
}

VOID SlzApplyUsa(PUCHAR Buf)
{
    USHORT off = U16(Buf, 4);
    USHORT cnt = U16(Buf, 6);
    USHORT usn = U16(Buf, off);
    ULONG i;

    if (cnt == 0 || cnt > 9) return;
    if ((ULONG)off + (ULONG)cnt * 2 > SLZ_SECTOR_SIZE) return;
    for (i = 1; i < cnt; i++) {
        W16(Buf, off + i * 2, U16(Buf, i * SLZ_SECTOR_SIZE - 2));
        W16(Buf, i * SLZ_SECTOR_SIZE - 2, usn);
    }
}

/* ============================================================
 * 同步扇区 I/O
 * ============================================================ */

static NTSTATUS SlzIoDone(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SlzTransfer(PDEVICE_OBJECT Target, ULONG64 OffsetBytes,
                            PVOID Buffer, ULONG Length, BOOLEAN Write)
{
    PIRP irp;
    PIO_STACK_LOCATION sp;
    KEVENT event;
    NTSTATUS st;
    PMDL mdl;

    if (!Target || !Buffer || Length == 0) return STATUS_INVALID_PARAMETER;
    if (Length > 0x01000000) return STATUS_INVALID_PARAMETER;
    if ((OffsetBytes & (SLZ_SECTOR_SIZE - 1)) != 0)
        return STATUS_INVALID_PARAMETER;
    if ((Length & (SLZ_SECTOR_SIZE - 1)) != 0)
        return STATUS_INVALID_PARAMETER;

    irp = IoAllocateIrp(Target->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = Write ? IRP_MJ_WRITE : IRP_MJ_READ;
    sp->Parameters.Write.Length = Length;
    sp->Parameters.Write.ByteOffset.QuadPart = (LONGLONG)OffsetBytes;
    sp->Parameters.Write.Key = 0;

    irp->RequestorMode = KernelMode;
    irp->Flags |= IRP_NOCACHE;

    if (Target->Flags & DO_DIRECT_IO) {
        mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, irp);
        if (!mdl) {
            IoFreeIrp(irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(mdl);
    } else {
        irp->UserBuffer = Buffer;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(irp, SlzIoDone, &event, TRUE, TRUE, TRUE);

    st = IoCallDriver(Target, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    if (NT_SUCCESS(st) && irp->IoStatus.Information != Length)
        st = STATUS_IO_DEVICE_ERROR;

    IoFreeIrp(irp);
    return st;
}

NTSTATUS SlzReadSectors(PDEVICE_OBJECT Target, ULONG64 OffsetBytes,
                        PVOID Buffer, ULONG Length)
{
    return SlzTransfer(Target, OffsetBytes, Buffer, Length, FALSE);
}

NTSTATUS SlzWriteSectors(PDEVICE_OBJECT Target, ULONG64 OffsetBytes,
                         PVOID Buffer, ULONG Length)
{
    return SlzTransfer(Target, OffsetBytes, Buffer, Length, TRUE);
}

NTSTATUS SlzFlushDevice(PDEVICE_OBJECT Target)
{
    PIRP irp;
    PIO_STACK_LOCATION sp;
    KEVENT event;
    NTSTATUS st;

    if (!Target) return STATUS_INVALID_PARAMETER;
    irp = IoAllocateIrp(Target->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_FLUSH_BUFFERS;
    irp->RequestorMode = KernelMode;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(irp, SlzIoDone, &event, TRUE, TRUE, TRUE);

    st = IoCallDriver(Target, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    IoFreeIrp(irp);

    if (st == STATUS_INVALID_DEVICE_REQUEST) st = STATUS_SUCCESS;
    return st;
}

/* ============================================================
 * MFT 记录解析
 * ============================================================ */

NTSTATUS SlzParseRecord(PUCHAR B, ULONG Index, SLZ_REC *Out)
{
    ULONG used, off, guard;
    BOOLEAN haveName = FALSE;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Index = Index;
    if (U32(B, 0) != SLZ_FILE_MAGIC) return STATUS_FILE_CORRUPT_ERROR;

    Out->Flags = U16(B, 22);
    used = U32(B, 24);
    off = U16(B, 20);
    if (used > SLZ_RECORD_SIZE || used < 48) return STATUS_FILE_CORRUPT_ERROR;
    if (off < 48 || off + 16 > SLZ_RECORD_SIZE) return STATUS_FILE_CORRUPT_ERROR;

    guard = 0;
    while (guard++ < 32) {
        ULONG type, len;

        if (off + 16 > used) break;
        type = U32(B, off);
        if (type == SLZ_ATTR_END) break;
        len = U32(B, off + 4);
        if (len < 16 || off + len > used) return STATUS_FILE_CORRUPT_ERROR;

        if (B[off + 8] == 0) {
            ULONG vlen = U32(B, off + 16);
            ULONG voff = U16(B, off + 20);
            PUCHAR v;

            if (voff < 16 || voff + vlen > len) return STATUS_FILE_CORRUPT_ERROR;
            v = B + off + voff;

            if (type == SLZ_ATTR_STD_INFO && vlen >= 36) {
                Out->Created = U32(v, 0);
                Out->Modified = U32(v, 8);
                Out->Accessed = U32(v, 16);
            } else if (type == SLZ_ATTR_FILE_NAME && vlen >= 24) {
                USHORT nl = U16(v, 8);
                if (nl <= 255 && 24 + (ULONG)nl <= vlen) {
                    Out->Parent = U64(v, 0);
                    Out->NameLen = nl;
                    RtlCopyMemory(Out->Name, v + 24, nl);
                    Out->Name[nl] = 0;
                    haveName = TRUE;
                }
            } else if (type == SLZ_ATTR_INDEX_ROOT && vlen >= 12) {
                Out->IndexLba = U64(v, 0);      /* 绝对 LBA */
                Out->IndexBlocks = U32(v, 8);
            }
        } else if (type == SLZ_ATTR_DATA) {
            ULONG32 cnt = U32(B, off + 32);
            ULONG32 i;

            if (cnt > SLZ_MAX_EXTENTS || len < 64 + (ULONG)cnt * 16)
                return STATUS_FILE_CORRUPT_ERROR;
            Out->ExtCount = cnt;
            Out->Allocation = U64(B, off + 40);
            Out->Size = U64(B, off + 48);
            if (Out->Size > Out->Allocation)
                return STATUS_FILE_CORRUPT_ERROR;
            for (i = 0; i < cnt; i++) {
                Out->ExtLba[i] = U64(B, off + 64 + (ULONG64)i * 16);
                Out->ExtClusters[i] = U32(B, off + 72 + (ULONG64)i * 16);
            }
        }
        off += len;
    }

    if (!haveName) return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS SlzValidateRec(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, SLZ_REC *Rec)
{
    ULONG32 i;

    if (Rec->ExtCount > SLZ_MAX_EXTENTS) return STATUS_FILE_CORRUPT_ERROR;
    for (i = 0; i < Rec->ExtCount; i++) {
        ULONG64 rel;
        if (Rec->ExtClusters[i] == 0) return STATUS_FILE_CORRUPT_ERROR;
        if (Rec->ExtLba[i] < Vcb->ContainerStart) return STATUS_FILE_CORRUPT_ERROR;
        rel = Rec->ExtLba[i] - Vcb->ContainerStart;
        if (rel < Sub->BaseLba) return STATUS_FILE_CORRUPT_ERROR;
        if (rel + (ULONG64)Rec->ExtClusters[i] * SLZ_CLUSTER_SECTORS >
            Sub->BaseLba + Sub->Sectors)
            return STATUS_FILE_CORRUPT_ERROR;
    }
    if (Rec->IndexLba != 0) {
        ULONG64 rel;
        if (Rec->IndexLba < Vcb->ContainerStart) return STATUS_FILE_CORRUPT_ERROR;
        rel = Rec->IndexLba - Vcb->ContainerStart;
        if (rel < Sub->BaseLba ||
            rel + SLZ_CLUSTER_SECTORS > Sub->BaseLba + Sub->Sectors)
            return STATUS_FILE_CORRUPT_ERROR;
        if (Rec->IndexBlocks == 0 || Rec->IndexBlocks > 65536)
            return STATUS_FILE_CORRUPT_ERROR;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SlzReadRecord(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, ULONG Index, SLZ_REC *Out)
{
    PUCHAR buf;
    NTSTATUS st;
    ULONG64 lba;

    if (!Vcb || !Sub || Index >= Sub->MftRecords)
        return STATUS_FILE_CORRUPT_ERROR;
    lba = Sub->MftLba + (ULONG64)Index * SLZ_RECORD_SECTORS;
    if (lba + SLZ_RECORD_SECTORS > Sub->BaseLba + Sub->Sectors)
        return STATUS_FILE_CORRUPT_ERROR;

    buf = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

    st = SlzReadSectors(Vcb->Target, lba * SLZ_SECTOR_SIZE, buf, SLZ_RECORD_SIZE);
    if (!NT_SUCCESS(st)) goto done;
    if (U32(buf, 0) != SLZ_FILE_MAGIC) {
        st = STATUS_FILE_CORRUPT_ERROR;
        goto done;
    }
    if (!SlzRestoreUsa(buf)) {
        st = STATUS_FILE_CORRUPT_ERROR;
        goto done;
    }
    st = SlzParseRecord(buf, Index, Out);
    if (NT_SUCCESS(st)) st = SlzValidateRec(Vcb, Sub, Out);

done:
    ExFreePoolWithTag(buf, SLZ_TAG);
    return st;
}

/* ============================================================
 * 目录索引
 * ============================================================ */

static BOOLEAN SlzNameEqual(const UCHAR *A, USHORT ALen, const UCHAR *B,
                            USHORT BLen)
{
    WCHAR wa[256], wb[256];
    ULONG ca, cb, i;

    if (ALen != BLen || ALen == 0 || ALen > 255) return FALSE;
    ca = SlzUtf8ToWide(A, ALen, wa, 256);
    cb = SlzUtf8ToWide(B, BLen, wb, 256);
    if (ca == (ULONG)-1 || cb == (ULONG)-1 || ca != cb) return FALSE;
    for (i = 0; i < ca; i++) {
        if (RtlUpcaseUnicodeChar(wa[i]) != RtlUpcaseUnicodeChar(wb[i]))
            return FALSE;
    }
    return TRUE;
}

NTSTATUS SlzDirLookup(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, const SLZ_REC *Dir,
                      const UCHAR *Name, USHORT NameLen, ULONG *RefOut)
{
    PUCHAR block;
    ULONG64 rel;
    ULONG guard = 0;
    NTSTATUS st = STATUS_OBJECT_NAME_NOT_FOUND;

    *RefOut = 0;
    if (!Dir || !(Dir->Flags & 2)) return STATUS_NOT_A_DIRECTORY;
    if (Dir->IndexLba == 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (Dir->IndexLba < Vcb->ContainerStart) return STATUS_FILE_CORRUPT_ERROR;
    rel = Dir->IndexLba - Vcb->ContainerStart;

    block = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!block) return STATUS_INSUFFICIENT_RESOURCES;

    for (;;) {
        ULONG cnt, used, off, i;
        ULONG64 next;

        if (guard++ > Dir->IndexBlocks + 4) { st = STATUS_FILE_CORRUPT_ERROR; break; }
        if (rel < Sub->BaseLba ||
            rel + SLZ_CLUSTER_SECTORS > Sub->BaseLba + Sub->Sectors) {
            st = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        st = SlzReadSectors(Vcb->Target, rel * SLZ_SECTOR_SIZE, block,
                            SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) break;
        if (U32(block, 0) != SLZ_INDX_MAGIC) { st = STATUS_FILE_CORRUPT_ERROR; break; }

        cnt = U32(block, 24);
        used = U32(block, 28);
        if (used > SLZ_RECORD_SIZE || used < 64) { st = STATUS_FILE_CORRUPT_ERROR; break; }

        off = 64;
        for (i = 0; i < cnt; i++) {
            USHORT nl, el;
            if (off + 16 > used) { st = STATUS_FILE_CORRUPT_ERROR; break; }
            nl = U16(block, off + 6);
            el = U16(block, off + 8);
            if (el < 16 || off + el > used || nl > 255) {
                st = STATUS_FILE_CORRUPT_ERROR;
                break;
            }
            if (nl == NameLen && SlzNameEqual(block + off + 16, nl, Name, NameLen)) {
                ULONG ref = U32(block, off);
                if (ref >= Sub->MftRecords) { st = STATUS_FILE_CORRUPT_ERROR; break; }
                *RefOut = ref;
                st = STATUS_SUCCESS;
                goto done;
            }
            off += el;
        }
        if (st != STATUS_SUCCESS && st != STATUS_OBJECT_NAME_NOT_FOUND) break;
        st = STATUS_OBJECT_NAME_NOT_FOUND;

        next = U64(block, 16);
        if (next == 0) break;
        if (next < Vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; break; }
        rel = next - Vcb->ContainerStart;
    }

done:
    ExFreePoolWithTag(block, SLZ_TAG);
    return st;
}

NTSTATUS SlzDirCollect(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, const SLZ_REC *Dir,
                       SLZ_DIRENT **Out, ULONG *CountOut)
{
    PUCHAR block;
    ULONG64 rel;
    ULONG guard = 0;
    ULONG cap = 0, count = 0;
    SLZ_DIRENT *list = NULL;
    NTSTATUS st = STATUS_SUCCESS;

    *Out = NULL;
    *CountOut = 0;
    if (!Dir || !(Dir->Flags & 2)) return STATUS_NOT_A_DIRECTORY;
    if (Dir->IndexLba == 0) return STATUS_SUCCESS;
    if (Dir->IndexLba < Vcb->ContainerStart) return STATUS_FILE_CORRUPT_ERROR;
    rel = Dir->IndexLba - Vcb->ContainerStart;

    block = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!block) return STATUS_INSUFFICIENT_RESOURCES;

    for (;;) {
        ULONG cnt, used, off, i;
        ULONG64 next;

        if (guard++ > Dir->IndexBlocks + 4) { st = STATUS_FILE_CORRUPT_ERROR; break; }
        if (rel < Sub->BaseLba ||
            rel + SLZ_CLUSTER_SECTORS > Sub->BaseLba + Sub->Sectors) {
            st = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        st = SlzReadSectors(Vcb->Target, rel * SLZ_SECTOR_SIZE, block,
                            SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) break;
        if (U32(block, 0) != SLZ_INDX_MAGIC) { st = STATUS_FILE_CORRUPT_ERROR; break; }

        cnt = U32(block, 24);
        used = U32(block, 28);
        if (used > SLZ_RECORD_SIZE || used < 64) { st = STATUS_FILE_CORRUPT_ERROR; break; }

        off = 64;
        for (i = 0; i < cnt; i++) {
            USHORT nl, el;
            if (off + 16 > used) { st = STATUS_FILE_CORRUPT_ERROR; break; }
            nl = U16(block, off + 6);
            el = U16(block, off + 8);
            if (el < 16 || off + el > used || nl > 255) {
                st = STATUS_FILE_CORRUPT_ERROR;
                break;
            }
            if (count >= SLZ_MAX_DIR_CACHE) { st = STATUS_FILE_TOO_LARGE; break; }
            if (count == cap) {
                ULONG ncap = cap ? cap * 2 : 64;
                SLZ_DIRENT *nl2;
                if (ncap > SLZ_MAX_DIR_CACHE) ncap = SLZ_MAX_DIR_CACHE;
                nl2 = ExAllocatePoolWithTag(NonPagedPool,
                                            ncap * sizeof(SLZ_DIRENT), SLZ_TAG);
                if (!nl2) { st = STATUS_INSUFFICIENT_RESOURCES; break; }
                RtlZeroMemory(nl2, ncap * sizeof(SLZ_DIRENT));
                if (list) {
                    RtlCopyMemory(nl2, list, count * sizeof(SLZ_DIRENT));
                    ExFreePoolWithTag(list, SLZ_TAG);
                }
                list = nl2;
                cap = ncap;
            }
            list[count].Ref = U32(block, off);
            list[count].Flags = (UCHAR)U16(block, off + 4);
            list[count].NameLen = (UCHAR)nl;
            RtlCopyMemory(list[count].Name, block + off + 16, nl);
            list[count].Name[nl] = 0;
            count++;
            off += el;
        }
        if (!NT_SUCCESS(st)) break;

        next = U64(block, 16);
        if (next == 0) break;
        if (next < Vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; break; }
        rel = next - Vcb->ContainerStart;
    }

    ExFreePoolWithTag(block, SLZ_TAG);
    if (!NT_SUCCESS(st)) {
        if (list) ExFreePoolWithTag(list, SLZ_TAG);
        return st;
    }
    *Out = list;
    *CountOut = count;
    return STATUS_SUCCESS;
}

/* ============================================================
 * FCB
 * ============================================================ */

SLZ_FCB *SlzFcbFromRecord(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, const SLZ_REC *Rec,
                          const UCHAR *Path, USHORT PathLen)
{
    SLZ_FCB *f;

    if (PathLen >= SLZ_MAX_PATH) return NULL;
    f = ExAllocatePoolWithTag(NonPagedPool, sizeof(SLZ_FCB), SLZ_TAG);
    if (!f) return NULL;
    RtlZeroMemory(f, sizeof(*f));
    f->Magic = SLZ_FCB_MAGIC;
    f->Vcb = Vcb;
    f->Sub = Sub;
    f->RecordIndex = Rec->Index;
    f->ParentRecord = Rec->Parent;
    f->NameLen = Rec->NameLen;
    RtlCopyMemory(f->Name, Rec->Name, Rec->NameLen);
    f->Name[Rec->NameLen] = 0;
    RtlCopyMemory(f->Path, Path, PathLen);
    f->Path[PathLen] = 0;
    f->PathLen = PathLen;
    SlzUpdateFcbFromRec(f, Rec);
    return f;
}

static SLZ_FCB *SlzAllocPseudoRoot(SLZ_VCB *Vcb)
{
    SLZ_FCB *f = ExAllocatePoolWithTag(NonPagedPool, sizeof(SLZ_FCB), SLZ_TAG);
    if (!f) return NULL;
    RtlZeroMemory(f, sizeof(*f));
    f->Magic = SLZ_FCB_MAGIC;
    f->NodeType = SLZ_NODE_ROOT;
    f->Vcb = Vcb;
    f->RecordIndex = SLZ_NO_RECORD;
    f->ParentRecord = SLZ_NO_RECORD;
    f->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    return f;
}

static SLZ_FCB *SlzCloneFcb(const SLZ_FCB *Src)
{
    SLZ_FCB *f = ExAllocatePoolWithTag(NonPagedPool, sizeof(SLZ_FCB), SLZ_TAG);
    if (!f) return NULL;
    RtlCopyMemory(f, Src, sizeof(*f));
    RtlZeroMemory(&f->Section, sizeof(f->Section));
    return f;
}

static void SlzFreeFcb(SLZ_FCB *Fcb)
{
    if (!Fcb) return;
    if (Fcb->Magic != SLZ_FCB_MAGIC) return;
    Fcb->Magic = 0;
    ExFreePoolWithTag(Fcb, SLZ_TAG);
}

/* ============================================================
 * 路径解析
 * ============================================================ */

static USHORT SlzParentPathLen(const UCHAR *Path, USHORT Len)
{
    while (Len > 0 && Path[Len - 1] != '\\') Len--;
    if (Len > 0) Len--;
    return Len;
}

static BOOLEAN SlzSubvolNameMatches(const SLZ_SUBVOL *Sub, const WCHAR *Name,
                                    ULONG Chars)
{
    WCHAR w[32];
    ULONG n, i;
    ULONG labelLen = 0;

    while (labelLen < 24 && Sub->Label[labelLen]) labelLen++;
    n = SlzUtf8ToWide(Sub->Label, labelLen, w, 32);
    if (n == (ULONG)-1 || n != Chars) return FALSE;
    for (i = 0; i < n; i++) {
        if (RtlUpcaseUnicodeChar(w[i]) != RtlUpcaseUnicodeChar(Name[i]))
            return FALSE;
    }
    return TRUE;
}

static NTSTATUS SlzLocate(SLZ_VCB *Vcb, SLZ_FCB *Base, PUNICODE_STRING Name,
                          SLZ_FCB **Out)
{
    SLZ_FCB *cur;
    const WCHAR *p;
    ULONG chars, i = 0;

    *Out = NULL;
    cur = SlzCloneFcb(Base ? Base : Vcb->RootFcb);
    if (!cur) return STATUS_INSUFFICIENT_RESOURCES;

    if (!Name || Name->Length == 0) {
        *Out = cur;
        return STATUS_SUCCESS;
    }
    p = Name->Buffer;
    chars = Name->Length / sizeof(WCHAR);
    if (!p) { SlzFreeFcb(cur); return STATUS_INVALID_PARAMETER; }

    if (p[0] == '\\') {
        SlzFreeFcb(cur);
        cur = SlzCloneFcb(Vcb->RootFcb);
        if (!cur) return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (i < chars) {
        ULONG start, segChars;
        UCHAR utf8[256];
        USHORT utf8Len;
        ULONG j;

        while (i < chars && p[i] == '\\') i++;
        if (i >= chars) break;
        start = i;
        while (i < chars && p[i] != '\\') i++;
        segChars = i - start;
        if (segChars == 0) continue;

        /* 去掉 NTFS 风格流后缀 file:stream */
        for (j = 0; j < segChars; j++) {
            if (p[start + j] == ':') { segChars = j; break; }
        }
        if (segChars == 0) continue;

        if (segChars == 1 && p[start] == '.') continue;

        if (segChars == 2 && p[start] == '.' && p[start + 1] == '.') {
            if (cur->NodeType == SLZ_NODE_ROOT) continue;
            if (cur->RecordIndex == cur->Sub->RootRecord) {
                SLZ_FCB *root = SlzCloneFcb(Vcb->RootFcb);
                if (!root) { SlzFreeFcb(cur); return STATUS_INSUFFICIENT_RESOURCES; }
                SlzFreeFcb(cur);
                cur = root;
            } else {
                SLZ_REC prec;
                USHORT plen = SlzParentPathLen(cur->Path, cur->PathLen);
                SLZ_FCB *par;
                NTSTATUS st = SlzReadRecord(Vcb, cur->Sub, (ULONG)cur->ParentRecord,
                                            &prec);
                if (!NT_SUCCESS(st)) { SlzFreeFcb(cur); return st; }
                par = SlzFcbFromRecord(Vcb, cur->Sub, &prec, cur->Path, plen);
                if (!par) { SlzFreeFcb(cur); return STATUS_INSUFFICIENT_RESOURCES; }
                SlzFreeFcb(cur);
                cur = par;
            }
            continue;
        }

        if (segChars > 255) { SlzFreeFcb(cur); return STATUS_OBJECT_NAME_INVALID; }
        utf8Len = (USHORT)SlzWideToUtf8(p + start, segChars, utf8, 255);
        if (utf8Len == (USHORT)-1 || utf8Len == 0) {
            SlzFreeFcb(cur);
            return STATUS_OBJECT_NAME_INVALID;
        }
        utf8[utf8Len] = 0;

        if (cur->NodeType == SLZ_NODE_ROOT) {
            ULONG k;
            SLZ_FCB *next = NULL;
            for (k = 0; k < SLZ_MAX_SUBVOLS; k++) {
                SLZ_SUBVOL *sv = &Vcb->Sub[k];
                if (!sv->Present) continue;
                if (!SlzSubvolNameMatches(sv, p + start, segChars)) continue;
                {
                    SLZ_REC rr;
                    NTSTATUS st;
                    ULONG labelLen = 0;
                    while (labelLen < 24 && sv->Label[labelLen]) labelLen++;
                    st = SlzReadRecord(Vcb, sv, (ULONG)sv->RootRecord, &rr);
                    if (!NT_SUCCESS(st)) { SlzFreeFcb(cur); return st; }
                    next = SlzFcbFromRecord(Vcb, sv, &rr,
                                            sv->Label, (USHORT)labelLen);
                    if (!next) { SlzFreeFcb(cur); return STATUS_INSUFFICIENT_RESOURCES; }
                }
                break;
            }
            if (!next) { SlzFreeFcb(cur); return STATUS_OBJECT_NAME_NOT_FOUND; }
            SlzFreeFcb(cur);
            cur = next;
            continue;
        }

        if (cur->NodeType != SLZ_NODE_DIR) {
            SlzFreeFcb(cur);
            return STATUS_NOT_A_DIRECTORY;
        }

        {
            SLZ_REC rec, child;
            ULONG ref = 0;
            NTSTATUS st = SlzReadRecord(Vcb, cur->Sub, cur->RecordIndex, &rec);
            if (!NT_SUCCESS(st)) { SlzFreeFcb(cur); return st; }
            st = SlzDirLookup(Vcb, cur->Sub, &rec, utf8, utf8Len, &ref);
            if (!NT_SUCCESS(st)) { SlzFreeFcb(cur); return st; }
            st = SlzReadRecord(Vcb, cur->Sub, ref, &child);
            if (!NT_SUCCESS(st)) { SlzFreeFcb(cur); return st; }

            {
                UCHAR path[SLZ_MAX_PATH];
                USHORT pathLen;
                SLZ_FCB *next;
                if (cur->PathLen) {
                    if ((ULONG)cur->PathLen + 1 + utf8Len >= SLZ_MAX_PATH) {
                        SlzFreeFcb(cur);
                        return STATUS_OBJECT_NAME_INVALID;
                    }
                    RtlCopyMemory(path, cur->Path, cur->PathLen);
                    path[cur->PathLen] = '\\';
                    RtlCopyMemory(path + cur->PathLen + 1, utf8, utf8Len);
                    pathLen = (USHORT)(cur->PathLen + 1 + utf8Len);
                } else {
                    if (utf8Len >= SLZ_MAX_PATH) {
                        SlzFreeFcb(cur);
                        return STATUS_OBJECT_NAME_INVALID;
                    }
                    RtlCopyMemory(path, utf8, utf8Len);
                    pathLen = utf8Len;
                }
                next = SlzFcbFromRecord(Vcb, cur->Sub, &child, path, pathLen);
                if (!next) { SlzFreeFcb(cur); return STATUS_INSUFFICIENT_RESOURCES; }
                SlzFreeFcb(cur);
                cur = next;
            }
        }
    }

    *Out = cur;
    return STATUS_SUCCESS;
}

/* 拆出父路径与最后一段叶子名（都指向原缓冲） */
static BOOLEAN SlzSplitLast(PUNICODE_STRING Full, PUNICODE_STRING Parent,
                            PUNICODE_STRING Leaf)
{
    ULONG chars, end, i, cut;

    if (!Full || !Full->Buffer || Full->Length == 0) return FALSE;
    chars = Full->Length / sizeof(WCHAR);
    end = chars;
    while (end > 0 && Full->Buffer[end - 1] == '\\') end--;

    cut = end;
    while (cut > 0 && Full->Buffer[cut - 1] != '\\') cut--;
    if (cut == end) return FALSE;

    for (i = cut; i < end; i++) {
        if (Full->Buffer[i] == ':') { end = i; break; }
    }
    if (cut == end) return FALSE;

    Parent->Buffer = Full->Buffer;
    Parent->Length = (USHORT)(cut * sizeof(WCHAR));
    Parent->MaximumLength = Parent->Length;

    Leaf->Buffer = Full->Buffer + cut;
    Leaf->Length = (USHORT)((end - cut) * sizeof(WCHAR));
    Leaf->MaximumLength = Leaf->Length;
    return TRUE;
}

/* ============================================================
 * 缓冲区 / 完成
 * ============================================================ */

static NTSTATUS SlzFinish(PIRP Irp, NTSTATUS Status, ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static PVOID SlzMapBuffer(PIRP Irp, ULONG Length, BOOLEAN ForWrite,
                          PBOOLEAN Ok)
{
    PVOID p = NULL;

    *Ok = TRUE;
    if (Length == 0) return NULL;

    /* 卷查询/FSCTL 可能是 buffered IRP：必须写系统缓冲，
     * 写 UserBuffer 会被 I/O 管理器返回的系统缓冲覆盖。 */
    if (Irp->Flags & IRP_BUFFERED_IO) {
        p = Irp->AssociatedIrp.SystemBuffer;
        if (!p) { *Ok = FALSE; return NULL; }
        return p;
    }

    if (Irp->MdlAddress) {
        p = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (!p) { *Ok = FALSE; return NULL; }
        if (MmGetMdlByteCount(Irp->MdlAddress) < Length) *Ok = FALSE;
        return p;
    }

    p = Irp->UserBuffer;
    if (!p) p = Irp->AssociatedIrp.SystemBuffer;
    if (!p) { *Ok = FALSE; return NULL; }

    if (Irp->RequestorMode == UserMode) {
        __try {
            if (ForWrite) ProbeForWrite(p, Length, 1);
            else ProbeForRead(p, Length, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *Ok = FALSE;
        }
    }
    return p;
}

/* ============================================================
 * 挂载
 * ============================================================ */

static NTSTATUS SlzQueryExtents(PDEVICE_OBJECT Target, ULONG64 *StartSector,
                                ULONG64 *Sectors)
{
    PVOID buf;
    ULONG size = sizeof(SLZ_VOLUME_EXTENTS) + 3 * sizeof(SLZ_DISK_EXTENT);
    PIRP irp;
    PIO_STACK_LOCATION sp;
    KEVENT event;
    NTSTATUS st;

    buf = ExAllocatePoolWithTag(NonPagedPool, size, SLZ_TAG);
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(buf, size);

    irp = IoAllocateIrp(Target->StackSize, FALSE);
    if (!irp) {
        ExFreePoolWithTag(buf, SLZ_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    sp->Parameters.DeviceIoControl.OutputBufferLength = size;
    sp->Parameters.DeviceIoControl.InputBufferLength = 0;
    sp->Parameters.DeviceIoControl.IoControlCode =
        IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS;
    irp->AssociatedIrp.SystemBuffer = buf;
    irp->UserBuffer = buf;
    irp->RequestorMode = KernelMode;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(irp, SlzIoDone, &event, TRUE, TRUE, TRUE);
    st = IoCallDriver(Target, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    if (NT_SUCCESS(st)) {
        SLZ_VOLUME_EXTENTS *ext = (SLZ_VOLUME_EXTENTS *)buf;
        if (ext->NumberOfDiskExtents < 1) st = STATUS_INVALID_DEVICE_REQUEST;
        else {
            *StartSector = ext->Extents[0].StartingOffset.QuadPart / SLZ_SECTOR_SIZE;
            *Sectors = ext->Extents[0].Length.QuadPart / SLZ_SECTOR_SIZE;
        }
    }
    IoFreeIrp(irp);
    ExFreePoolWithTag(buf, SLZ_TAG);
    return st;
}

static NTSTATUS SlzLoadSubvol(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, UCHAR Type,
                              ULONG64 RelLba, ULONG64 Sectors,
                              const UCHAR *EntryLabel)
{
    UCHAR hdr[SLZ_SECTOR_SIZE];
    NTSTATUS st;
    ULONG64 total, mftAbs, bmpAbs, logAbs, root;
    ULONG records, bmpSectors, logSectors;

    RtlZeroMemory(Sub, sizeof(*Sub));
    if (Sectors < 256) return STATUS_INVALID_PARAMETER;
    if (RelLba + Sectors > Vcb->ContainerSectors) return STATUS_INVALID_PARAMETER;

    st = SlzReadSectors(Vcb->Target, RelLba * SLZ_SECTOR_SIZE, hdr,
                        SLZ_SECTOR_SIZE);
    if (!NT_SUCCESS(st)) return st;
    if (U64(hdr, 0) != SLZ_VOL_MAGIC) return STATUS_UNRECOGNIZED_VOLUME;
    if (U16(hdr, 8) != 2) return STATUS_UNRECOGNIZED_VOLUME;
    if (U32(hdr, 12) != SLZ_CLUSTER_SIZE) return STATUS_UNRECOGNIZED_VOLUME;
    if (U32(hdr, 152) != SlzCrc32(hdr, 152)) return STATUS_UNRECOGNIZED_VOLUME;

    total = U64(hdr, 16);
    mftAbs = U64(hdr, 24);
    records = U32(hdr, 32);
    bmpAbs = U64(hdr, 40);
    bmpSectors = U32(hdr, 48);
    logAbs = U64(hdr, 52);
    logSectors = U32(hdr, 60);
    root = U64(hdr, 64);

    if (total == 0 || total > Sectors / SLZ_CLUSTER_SECTORS) return STATUS_UNRECOGNIZED_VOLUME;
    if (records < 16 || records > 65536) return STATUS_UNRECOGNIZED_VOLUME;
    if (root >= records) return STATUS_UNRECOGNIZED_VOLUME;
    if (mftAbs < Vcb->ContainerStart || bmpAbs < Vcb->ContainerStart ||
        logAbs < Vcb->ContainerStart)
        return STATUS_UNRECOGNIZED_VOLUME;

    Sub->Present = TRUE;
    Sub->Type = Type;
    if (EntryLabel && EntryLabel[0]) {
        ULONG i;
        for (i = 0; i < 24 && EntryLabel[i]; i++) Sub->Label[i] = EntryLabel[i];
        Sub->Label[i] = 0;
    } else {
        const char *def = (Type == 1) ? "SYSTEM" : (Type == 2 ? "RECOVERY" : "DATA");
        ULONG i;
        for (i = 0; def[i] && i < 24; i++) Sub->Label[i] = (UCHAR)def[i];
        Sub->Label[i] = 0;
    }

    Sub->BaseLba = RelLba;
    Sub->Sectors = Sectors;
    Sub->TotalClusters = total;
    Sub->MftLba = mftAbs - Vcb->ContainerStart;
    Sub->MftRecords = records;
    Sub->BitmapLba = bmpAbs - Vcb->ContainerStart;
    Sub->BitmapSectors = bmpSectors;
    Sub->LogLba = logAbs - Vcb->ContainerStart;
    Sub->LogSectors = logSectors;
    Sub->RootRecord = root;
    Sub->Serial = U32(hdr, 72);
    Sub->AllocHint = 8;

    if (Sub->MftLba < Sub->BaseLba || Sub->BitmapLba < Sub->BaseLba ||
        Sub->LogLba < Sub->BaseLba)
        return STATUS_UNRECOGNIZED_VOLUME;
    if (Sub->MftLba + (ULONG64)records * SLZ_RECORD_SECTORS >
        Sub->BaseLba + Sub->Sectors)
        return STATUS_UNRECOGNIZED_VOLUME;
    if (Sub->BitmapLba + bmpSectors > Sub->BaseLba + Sub->Sectors)
        return STATUS_UNRECOGNIZED_VOLUME;
    if (Sub->LogLba + logSectors > Sub->BaseLba + Sub->Sectors)
        return STATUS_UNRECOGNIZED_VOLUME;

    return STATUS_SUCCESS;
}

static NTSTATUS SlzMountVolume(PIO_STACK_LOCATION IrpSp)
{
    PVPB vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT target = IrpSp->Parameters.MountVolume.DeviceObject;
    UCHAR sb[SLZ_SECTOR_SIZE];
    SLZ_SUBVOL subs[SLZ_MAX_SUBVOLS];
    ULONG count = 0, i, entries;
    ULONG64 start = 0, sectors = 0;
    ULONG64 maxEnd = 0, firstStart = 0;
    NTSTATUS st;
    PDEVICE_OBJECT dev = NULL;
    SLZ_VCB *vcb;
    SLZ_VCB probe;
    BOOLEAN haveExtents;

    if (!vpb) return STATUS_INVALID_PARAMETER;
    if (!target) target = vpb->RealDevice;
    if (!target) return STATUS_INVALID_PARAMETER;
    if (vpb->Flags & VPB_MOUNTED) return STATUS_UNRECOGNIZED_VOLUME;

    st = SlzReadSectors(target, 0, sb, SLZ_SECTOR_SIZE);
    if (!NT_SUCCESS(st)) return STATUS_UNRECOGNIZED_VOLUME;
    if (U64(sb, 0) != SLZ_PT_MAGIC) return STATUS_UNRECOGNIZED_VOLUME;
    if (U16(sb, 8) != 1) return STATUS_UNRECOGNIZED_VOLUME;
    if (U32(sb, 508) != SlzCrc32(sb, 508)) return STATUS_UNRECOGNIZED_VOLUME;

    entries = sb[10];
    if (entries == 0 || entries > 4) return STATUS_UNRECOGNIZED_VOLUME;

    /* 从分区表推出容器起点（格式化器固定把表放在容器 LBA0，首分区 +4） */
    for (i = 0; i < entries; i++) {
        ULONG off = 16 + i * 64;
        UCHAR type = sb[off];
        ULONG64 s, n;
        if (type < 1 || type > 3) continue;
        s = U32(sb, off + 4);
        n = U32(sb, off + 8);
        if (n == 0) continue;
        if (firstStart == 0 || s < firstStart) firstStart = s;
        if (s + n > maxEnd) maxEnd = s + n;
    }
    if (firstStart < 4 || maxEnd <= firstStart) return STATUS_UNRECOGNIZED_VOLUME;

    haveExtents = FALSE;
    UNREFERENCED_PARAMETER(haveExtents);
    /* 不走 IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS：无 FileObject 的
     * 卷 IOCTL 在部分卷管理器实现上会解引用空 FileObject。
     * 格式化器固定把容器表放在卷内 LBA0、首子卷放在 +4。 */
    start = firstStart - 4;
    sectors = maxEnd - start;
    if (sectors == 0) return STATUS_UNRECOGNIZED_VOLUME;
    if (maxEnd > start + sectors) return STATUS_UNRECOGNIZED_VOLUME;

    RtlZeroMemory(subs, sizeof(subs));

    for (i = 0; i < entries; i++) {
        ULONG off = 16 + i * 64;
        UCHAR type = sb[off];
        ULONG64 s, n, rel;
        SLZ_SUBVOL tmp;

        if (type < 1 || type > 3) continue;
        s = U32(sb, off + 4);
        n = U32(sb, off + 8);
        if (n == 0 || s < start) continue;
        rel = s - start;
        if (rel + n > sectors) continue;

        RtlZeroMemory(&probe, sizeof(probe));
        probe.ContainerStart = start;
        probe.ContainerSectors = sectors;
        probe.Target = target;
        st = SlzLoadSubvol(&probe, &tmp, type, rel, n, sb + off + 12);
        if (NT_SUCCESS(st)) {
            subs[type - 1] = tmp;
            count++;
        }
    }
    if (count == 0) return STATUS_UNRECOGNIZED_VOLUME;

    st = IoCreateDevice(g_DriverObject, sizeof(SLZ_VCB), NULL,
                        FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;

    vcb = (SLZ_VCB *)dev->DeviceExtension;
    RtlZeroMemory(vcb, sizeof(*vcb));
    vcb->Magic = SLZ_VCB_MAGIC;
    vcb->DeviceObject = dev;
    vcb->Target = target;
    vcb->ContainerStart = start;
    vcb->ContainerSectors = sectors;
    vcb->SubCount = count;
    RtlCopyMemory(vcb->Sub, subs, sizeof(subs));
    vcb->Serial = 0x534C595Au;

    ExInitializeResourceLite(&vcb->Lock);

    /* 先重放日志，再读位图（日志可能改过位图扇区） */
    for (i = 0; i < SLZ_MAX_SUBVOLS; i++) {
        if (!vcb->Sub[i].Present) continue;
        SlzJournalReplay(vcb, &vcb->Sub[i]);
    }
    for (i = 0; i < SLZ_MAX_SUBVOLS; i++) {
        if (!vcb->Sub[i].Present) continue;
        st = SlzLoadBitmap(vcb, &vcb->Sub[i]);
        if (!NT_SUCCESS(st)) vcb->Sub[i].LogOk = FALSE;
        vcb->TotalClusters += vcb->Sub[i].TotalClusters;
        vcb->UsedClusters += vcb->Sub[i].UsedClusters;
        if (vcb->Sub[i].Serial) vcb->Serial = vcb->Sub[i].Serial;
    }

    vcb->WriteEnabled = FALSE;
    for (i = 0; i < SLZ_MAX_SUBVOLS; i++) {
        if (vcb->Sub[i].Present && vcb->Sub[i].LogOk) {
            vcb->WriteEnabled = TRUE;
            break;
        }
    }

    vcb->RootFcb = SlzAllocPseudoRoot(vcb);
    if (!vcb->RootFcb) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    dev->Vpb = vpb;
    dev->StackSize = (CCHAR)(target->StackSize + 1);
    dev->AlignmentRequirement = target->AlignmentRequirement;
    dev->SectorSize = SLZ_SECTOR_SIZE;
    if (!vcb->WriteEnabled)
        dev->Characteristics |= FILE_READ_ONLY_DEVICE;

    vpb->DeviceObject = dev;
    /* 不能覆盖 Vpb->RealDevice：它必须保持为物理磁盘设备 */
    vpb->Flags |= VPB_MOUNTED;
    vpb->SerialNumber = vcb->Serial;
    RtlCopyMemory(vpb->VolumeLabel, L"SlyzFS", 6 * sizeof(WCHAR));
    vpb->VolumeLabel[6] = 0;
    vpb->VolumeLabelLength = 6 * sizeof(WCHAR);

    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;

fail:
    if (vcb->RootFcb) SlzFreeFcb(vcb->RootFcb);
    for (i = 0; i < SLZ_MAX_SUBVOLS; i++) {
        if (vcb->Sub[i].Bitmap) ExFreePoolWithTag(vcb->Sub[i].Bitmap, SLZ_TAG);
    }
    ExDeleteResourceLite(&vcb->Lock);
    IoDeleteDevice(dev);
    return st;
}

/* ============================================================
 * IRP: CREATE / CLOSE / CLEANUP
 * ============================================================ */

static SLZ_VCB *SlzGetVcb(PDEVICE_OBJECT DeviceObject)
{
    SLZ_VCB *vcb;

    if (!DeviceObject || DeviceObject == g_ControlDevice) return NULL;
    vcb = (SLZ_VCB *)DeviceObject->DeviceExtension;
    if (!vcb || vcb->Magic != SLZ_VCB_MAGIC) return NULL;
    return vcb;
}

static BOOLEAN SlzIsWriteAccess(ACCESS_MASK Access)
{
    if (Access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                  FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE |
                  GENERIC_WRITE | WRITE_DAC | WRITE_OWNER))
        return TRUE;
    return FALSE;
}

static NTSTATUS SlzCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *base = NULL, *fcb = NULL;
    SLZ_CCB *ccb = NULL;
    ULONG disposition, options;
    NTSTATUS st;
    BOOLEAN locked = FALSE;

    if (!vcb || !fo) return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    disposition = (sp->Parameters.Create.Options >> 24) & 0xFF;
    options = sp->Parameters.Create.Options & 0x00FFFFFF;

    if (fo->RelatedFileObject) {
        base = (SLZ_FCB *)fo->RelatedFileObject->FsContext;
        if (!base || base->Magic != SLZ_FCB_MAGIC)
            return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        if (fo->FileName.Length != 0 && !SLZ_IS_DIR(base))
            return SlzFinish(Irp, STATUS_NOT_A_DIRECTORY, 0);
    }

    ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
    locked = TRUE;

    if (SlzIsWriteAccess(sp->Parameters.Create.SecurityContext ?
                         sp->Parameters.Create.SecurityContext->DesiredAccess : 0)
        && !vcb->WriteEnabled) {
        st = STATUS_MEDIA_WRITE_PROTECTED;
        goto done;
    }

    st = SlzLocate(vcb, base, &fo->FileName, &fcb);
    if (!NT_SUCCESS(st)) {
        /* 目标不存在：按 disposition 决定是否新建 */
        if (st == STATUS_OBJECT_NAME_NOT_FOUND &&
            (disposition == FILE_CREATE || disposition == FILE_OPEN_IF ||
             disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE)) {
            UNICODE_STRING parentName, leaf;
            SLZ_FCB *parent = NULL;
            UCHAR name[256];
            USHORT nameLen;
            BOOLEAN isDir = (options & FILE_DIRECTORY_FILE) ? TRUE : FALSE;

            if (!vcb->WriteEnabled ||
                (fo->FileName.Length == 0) ||
                !SlzSplitLast(&fo->FileName, &parentName, &leaf)) {
                st = STATUS_OBJECT_NAME_NOT_FOUND;
                goto done;
            }
            st = SlzLocate(vcb, base, &parentName, &parent);
            if (!NT_SUCCESS(st)) goto done;

            nameLen = (USHORT)SlzWideToUtf8(leaf.Buffer, leaf.Length / sizeof(WCHAR),
                                            name, 255);
            if (nameLen == (USHORT)-1 || nameLen == 0) {
                SlzFreeFcb(parent);
                st = STATUS_OBJECT_NAME_INVALID;
                goto done;
            }
            name[nameLen] = 0;
            st = SlzCreateNode(vcb, parent, name, nameLen, isDir, &fcb);
            SlzFreeFcb(parent);
            if (!NT_SUCCESS(st)) goto done;
            disposition = FILE_OPEN;
        } else {
            goto done;
        }
    } else {
        if (disposition == FILE_CREATE) { st = STATUS_OBJECT_NAME_COLLISION; goto done; }
        if (disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE ||
            disposition == FILE_OVERWRITE_IF) {
            if (fcb->NodeType != SLZ_NODE_FILE) { st = STATUS_ACCESS_DENIED; goto done; }
            st = SlzTruncateNode(vcb, fcb);
            if (!NT_SUCCESS(st)) goto done;
        }
    }

    if ((options & FILE_DIRECTORY_FILE) && !SLZ_IS_DIR(fcb)) {
        st = STATUS_NOT_A_DIRECTORY;
        goto done;
    }
    if ((options & FILE_NON_DIRECTORY_FILE) && SLZ_IS_DIR(fcb)) {
        st = STATUS_FILE_IS_A_DIRECTORY;
        goto done;
    }
    if (options & FILE_DELETE_ON_CLOSE) {
        if (!vcb->WriteEnabled) { st = STATUS_MEDIA_WRITE_PROTECTED; goto done; }
    }

    ccb = ExAllocatePoolWithTag(NonPagedPool, sizeof(SLZ_CCB), SLZ_TAG);
    if (!ccb) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    RtlZeroMemory(ccb, sizeof(*ccb));
    ccb->Magic = SLZ_CCB_MAGIC;
    ccb->Fcb = fcb;
    if (options & FILE_DELETE_ON_CLOSE) ccb->DeleteOnClose = TRUE;

    fo->FsContext = fcb;
    fo->FsContext2 = ccb;
    fo->SectionObjectPointer = &fcb->Section;
    fo->CurrentByteOffset.QuadPart = 0;
    fo->DeletePending = ccb->DeleteOnClose;

    st = STATUS_SUCCESS;

done:
    if (locked) ExReleaseResourceLite(&vcb->Lock);
    if (!NT_SUCCESS(st) && fcb) {
        SlzFreeFcb(fcb);
        if (ccb) ExFreePoolWithTag(ccb, SLZ_TAG);
        fo->FsContext = NULL;
        fo->FsContext2 = NULL;
    }
    return SlzFinish(Irp, st, 0);
}

static NTSTATUS SlzCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *fcb;
    SLZ_CCB *ccb;

    if (vcb && fo && fo->FsContext) {
        fcb = (SLZ_FCB *)fo->FsContext;
        ccb = (SLZ_CCB *)fo->FsContext2;
        if (ccb && ccb->Magic == SLZ_CCB_MAGIC && ccb->DeleteOnClose) {
            ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
            SlzDeleteNode(vcb, fcb);
            ccb->DeleteOnClose = FALSE;
            ExReleaseResourceLite(&vcb->Lock);
        }
    }
    fo->Flags |= FO_CLEANUP_COMPLETE;
    return SlzFinish(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS SlzClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (fo) {
        SLZ_CCB *ccb = (SLZ_CCB *)fo->FsContext2;
        SLZ_FCB *fcb = (SLZ_FCB *)fo->FsContext;
        if (ccb && ccb->Magic == SLZ_CCB_MAGIC) {
            ccb->Magic = 0;
            if (ccb->DirCache) ExFreePoolWithTag(ccb->DirCache, SLZ_TAG);
            ExFreePoolWithTag(ccb, SLZ_TAG);
        }
        if (fcb) SlzFreeFcb(fcb);
        fo->FsContext = NULL;
        fo->FsContext2 = NULL;
    }
    return SlzFinish(Irp, STATUS_SUCCESS, 0);
}

/* ============================================================
 * IRP: READ / WRITE
 * ============================================================ */

/* 刷新 FCB 的大小/段表：其他句柄写入后，本句柄读取前必须重读记录 */
static void SlzRefreshFcb(SLZ_VCB *Vcb, SLZ_FCB *Fcb)
{
    SLZ_REC rec;

    if (!Fcb || !Fcb->Sub || Fcb->NodeType == SLZ_NODE_ROOT) return;
    if (NT_SUCCESS(SlzReadRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, &rec)))
        SlzUpdateFcbFromRec(Fcb, &rec);
}

static NTSTATUS SlzRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *fcb = (SLZ_FCB *)sp->FileObject->FsContext;
    ULONG length = sp->Parameters.Read.Length;
    LONGLONG offset = sp->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR dst;
    PUCHAR bounce;
    BOOLEAN ok;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG done = 0;

    if (!vcb || !fcb || fcb->Magic != SLZ_FCB_MAGIC)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (SLZ_IS_DIR(fcb))
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_STATE, 0);

    if (offset < 0) offset = sp->FileObject->CurrentByteOffset.QuadPart;
    if (offset < 0) offset = 0;

    ExAcquireResourceSharedLite(&vcb->Lock, TRUE);
    SlzRefreshFcb(vcb, fcb);

    if ((ULONG64)offset >= fcb->Size || length == 0) {
        ExReleaseResourceLite(&vcb->Lock);
        return SlzFinish(Irp, STATUS_SUCCESS, 0);
    }
    if ((ULONG64)offset + length > fcb->Size)
        length = (ULONG)(fcb->Size - (ULONG64)offset);

    dst = (PUCHAR)SlzMapBuffer(Irp, length, TRUE, &ok);
    if (!ok || !dst) {
        ExReleaseResourceLite(&vcb->Lock);
        return SlzFinish(Irp, STATUS_INVALID_USER_BUFFER, 0);
    }

    bounce = ExAllocatePoolWithTag(NonPagedPool, 66 * 1024, SLZ_TAG);
    if (!bounce) {
        ExReleaseResourceLite(&vcb->Lock);
        return SlzFinish(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    while (done < length) {
        ULONG64 pos = (ULONG64)offset + done;
        ULONG64 base = 0;
        ULONG32 e;
        BOOLEAN found = FALSE;

        for (e = 0; e < fcb->ExtCount; e++) {
            ULONG64 bytes = (ULONG64)fcb->ExtClusters[e] * SLZ_CLUSTER_SIZE;
            if (pos >= base && pos < base + bytes) {
                ULONG64 inExt = pos - base;
                ULONG64 absLba = fcb->ExtLba[e];
                ULONG64 rel;
                ULONG64 byteOff, secBase;
                ULONG shift, readLen;
                ULONG chunk;

                if (absLba < vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; break; }
                rel = absLba - vcb->ContainerStart;
                chunk = (ULONG)(bytes - inExt);
                if (chunk > length - done) chunk = length - done;
                if (chunk > 64 * 1024) chunk = 64 * 1024;

                byteOff = rel * SLZ_SECTOR_SIZE + inExt;
                secBase = byteOff & ~(ULONG64)(SLZ_SECTOR_SIZE - 1);
                shift = (ULONG)(byteOff - secBase);
                readLen = (shift + chunk + SLZ_SECTOR_SIZE - 1) &
                          ~(SLZ_SECTOR_SIZE - 1);

                st = SlzReadSectors(vcb->Target, secBase, bounce, readLen);
                if (!NT_SUCCESS(st)) break;
                if (Irp->RequestorMode == UserMode && !Irp->MdlAddress) {
                    __try {
                        RtlCopyMemory(dst + done, bounce + shift, chunk);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        st = STATUS_INVALID_USER_BUFFER;
                    }
                } else {
                    RtlCopyMemory(dst + done, bounce + shift, chunk);
                }
                if (!NT_SUCCESS(st)) break;
                done += chunk;
                found = TRUE;
                break;
            }
            base += bytes;
        }
        if (!found) { st = STATUS_FILE_CORRUPT_ERROR; break; }
    }
    ExReleaseResourceLite(&vcb->Lock);

    ExFreePoolWithTag(bounce, SLZ_TAG);
    if (!NT_SUCCESS(st)) return SlzFinish(Irp, st, done);
    sp->FileObject->CurrentByteOffset.QuadPart = offset + done;
    return SlzFinish(Irp, STATUS_SUCCESS, done);
}

static NTSTATUS SlzWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *fcb = (SLZ_FCB *)sp->FileObject->FsContext;
    ULONG length = sp->Parameters.Write.Length;
    LONGLONG offset = sp->Parameters.Write.ByteOffset.QuadPart;
    PUCHAR src;
    BOOLEAN ok;
    ULONG written = 0;
    NTSTATUS st;

    if (!vcb || !fcb || fcb->Magic != SLZ_FCB_MAGIC)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (!vcb->WriteEnabled)
        return SlzFinish(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
    if (fcb->NodeType != SLZ_NODE_FILE)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_STATE, 0);

    if (offset < 0) offset = sp->FileObject->CurrentByteOffset.QuadPart;
    if (offset < 0) offset = 0;
    if (length == 0) return SlzFinish(Irp, STATUS_SUCCESS, 0);

    src = (PUCHAR)SlzMapBuffer(Irp, length, FALSE, &ok);
    if (!ok || !src) return SlzFinish(Irp, STATUS_INVALID_USER_BUFFER, 0);

    ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
    st = SlzWriteFileData(vcb, fcb, (ULONG64)offset, src, length, &written);
    ExReleaseResourceLite(&vcb->Lock);

    if (!NT_SUCCESS(st)) return SlzFinish(Irp, st, written);
    sp->FileObject->CurrentByteOffset.QuadPart = offset + written;
    return SlzFinish(Irp, STATUS_SUCCESS, written);
}

/* ============================================================
 * IRP: QUERY_INFORMATION
 * ============================================================ */

static ULONG SlzFullPathWide(SLZ_FCB *Fcb, WCHAR *Out, ULONG MaxChars)
{
    ULONG need = 1 + Fcb->PathLen + 1;
    ULONG n;

    if (need > MaxChars) return 0;
    Out[0] = '\\';
    if (Fcb->PathLen == 0) {
        Out[1] = 0;
        return 1;
    }
    n = SlzUtf8ToWide(Fcb->Path, Fcb->PathLen, Out + 1, MaxChars - 1);
    if (n == (ULONG)-1 || n == 0) return 0;
    return 1 + n;
}

static NTSTATUS SlzQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *fcb = (SLZ_FCB *)sp->FileObject->FsContext;
    ULONG length = sp->Parameters.QueryFile.Length;
    FILE_INFORMATION_CLASS cls = sp->Parameters.QueryFile.FileInformationClass;
    PUCHAR out;
    BOOLEAN ok;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    WCHAR name[SLZ_MAX_PATH];

    if (!vcb || !fcb) return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    SlzRefreshFcb(vcb, fcb);

    out = (PUCHAR)SlzMapBuffer(Irp, length, TRUE, &ok);
    if (!ok || (!out && length)) return SlzFinish(Irp, STATUS_INVALID_USER_BUFFER, 0);

    switch (cls) {
    case FileBasicInformation:
        if (length < sizeof(FILE_BASIC_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            PFILE_BASIC_INFORMATION p = (PFILE_BASIC_INFORMATION)out;
            RtlZeroMemory(p, sizeof(*p));
            p->CreationTime = fcb->CreationTime;
            p->LastAccessTime = fcb->LastAccessTime;
            p->LastWriteTime = fcb->LastWriteTime;
            p->ChangeTime = fcb->ChangeTime;
            p->FileAttributes = fcb->FileAttributes;
            info = sizeof(*p);
        }
        break;

    case FileStandardInformation:
        if (length < sizeof(FILE_STANDARD_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            PFILE_STANDARD_INFORMATION p = (PFILE_STANDARD_INFORMATION)out;
            RtlZeroMemory(p, sizeof(*p));
            p->AllocationSize.QuadPart = (LONGLONG)fcb->Allocation;
            p->EndOfFile.QuadPart = (LONGLONG)fcb->Size;
            p->NumberOfLinks = 1;
            p->DeletePending = sp->FileObject->DeletePending;
            p->Directory = SLZ_IS_DIR(fcb);
            info = sizeof(*p);
        }
        break;

    case FileInternalInformation:
        if (length < sizeof(FILE_INTERNAL_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            PFILE_INTERNAL_INFORMATION p = (PFILE_INTERNAL_INFORMATION)out;
            p->IndexNumber.QuadPart = fcb->RecordIndex;
            info = sizeof(*p);
        }
        break;

    case FileEaInformation:
        if (length < sizeof(FILE_EA_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        ((PFILE_EA_INFORMATION)out)->EaSize = 0;
        info = sizeof(FILE_EA_INFORMATION);
        break;

    case FileAccessInformation:
        if (length < sizeof(FILE_ACCESS_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        ((PFILE_ACCESS_INFORMATION)out)->AccessFlags =
            FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA |
            (vcb->WriteEnabled ? (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES |
                                  FILE_APPEND_DATA | DELETE) : 0);
        info = sizeof(FILE_ACCESS_INFORMATION);
        break;

    case FilePositionInformation:
        if (length < sizeof(FILE_POSITION_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        ((PFILE_POSITION_INFORMATION)out)->CurrentByteOffset =
            sp->FileObject->CurrentByteOffset;
        info = sizeof(FILE_POSITION_INFORMATION);
        break;

    case FileModeInformation:
        if (length < sizeof(FILE_MODE_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        ((PFILE_MODE_INFORMATION)out)->Mode = 0;
        info = sizeof(FILE_MODE_INFORMATION);
        break;

    case FileAlignmentInformation:
        if (length < sizeof(FILE_ALIGNMENT_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        ((PFILE_ALIGNMENT_INFORMATION)out)->AlignmentRequirement = 0;
        info = sizeof(FILE_ALIGNMENT_INFORMATION);
        break;

    case FileNameInformation:
        {
            ULONG chars = SlzFullPathWide(fcb, name, SLZ_MAX_PATH);
            ULONG need = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) +
                         chars * sizeof(WCHAR);
            if (chars == 0) { st = STATUS_OBJECT_NAME_INVALID; break; }
            if (length < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName)) {
                st = STATUS_BUFFER_OVERFLOW;
                break;
            }
            ((PFILE_NAME_INFORMATION)out)->FileNameLength = chars * sizeof(WCHAR);
            if (length < need) { st = STATUS_BUFFER_OVERFLOW; break; }
            RtlCopyMemory(((PFILE_NAME_INFORMATION)out)->FileName, name,
                          chars * sizeof(WCHAR));
            info = need;
        }
        break;

    case FileAllInformation:
        {
            ULONG chars = SlzFullPathWide(fcb, name, SLZ_MAX_PATH);
            ULONG nameOff = FIELD_OFFSET(FILE_ALL_INFORMATION,
                                         NameInformation.FileName);
            ULONG need = nameOff + chars * sizeof(WCHAR);
            PFILE_ALL_INFORMATION p = (PFILE_ALL_INFORMATION)out;

            if (chars == 0) { st = STATUS_OBJECT_NAME_INVALID; break; }
            if (length < nameOff) { st = STATUS_BUFFER_OVERFLOW; break; }

            RtlZeroMemory(p, nameOff);
            p->BasicInformation.CreationTime = fcb->CreationTime;
            p->BasicInformation.LastAccessTime = fcb->LastAccessTime;
            p->BasicInformation.LastWriteTime = fcb->LastWriteTime;
            p->BasicInformation.ChangeTime = fcb->ChangeTime;
            p->BasicInformation.FileAttributes = fcb->FileAttributes;
            p->StandardInformation.AllocationSize.QuadPart = (LONGLONG)fcb->Allocation;
            p->StandardInformation.EndOfFile.QuadPart = (LONGLONG)fcb->Size;
            p->StandardInformation.NumberOfLinks = 1;
            p->StandardInformation.DeletePending = sp->FileObject->DeletePending;
            p->StandardInformation.Directory = SLZ_IS_DIR(fcb);
            p->InternalInformation.IndexNumber.QuadPart = fcb->RecordIndex;
            p->EaInformation.EaSize = 0;
            p->AccessInformation.AccessFlags =
                FILE_READ_DATA | FILE_READ_ATTRIBUTES |
                (vcb->WriteEnabled ? (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES |
                                      FILE_APPEND_DATA | DELETE) : 0);
            p->PositionInformation.CurrentByteOffset =
                sp->FileObject->CurrentByteOffset;
            p->ModeInformation.Mode = 0;
            p->AlignmentInformation.AlignmentRequirement = 0;
            p->NameInformation.FileNameLength = chars * sizeof(WCHAR);
            if (length < need) { st = STATUS_BUFFER_OVERFLOW; break; }
            RtlCopyMemory(p->NameInformation.FileName, name,
                          chars * sizeof(WCHAR));
            info = need;
        }
        break;

    case FileNetworkOpenInformation:
        if (length < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
            st = STATUS_BUFFER_OVERFLOW;
            break;
        }
        {
            PFILE_NETWORK_OPEN_INFORMATION p =
                (PFILE_NETWORK_OPEN_INFORMATION)out;
            RtlZeroMemory(p, sizeof(*p));
            p->CreationTime = fcb->CreationTime;
            p->LastAccessTime = fcb->LastAccessTime;
            p->LastWriteTime = fcb->LastWriteTime;
            p->ChangeTime = fcb->ChangeTime;
            p->AllocationSize.QuadPart = (LONGLONG)fcb->Allocation;
            p->EndOfFile.QuadPart = (LONGLONG)fcb->Size;
            p->FileAttributes = fcb->FileAttributes;
            info = sizeof(*p);
        }
        break;

    case FileAttributeTagInformation:
        if (length < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
            st = STATUS_BUFFER_OVERFLOW;
            break;
        }
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION p =
                (PFILE_ATTRIBUTE_TAG_INFORMATION)out;
            p->FileAttributes = fcb->FileAttributes;
            p->ReparseTag = 0;
            info = sizeof(*p);
        }
        break;

    default:
        st = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return SlzFinish(Irp, st, info);
}

/* ============================================================
 * IRP: SET_INFORMATION
 * ============================================================ */

static NTSTATUS SlzSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *fcb = (SLZ_FCB *)sp->FileObject->FsContext;
    FILE_INFORMATION_CLASS cls = sp->Parameters.SetFile.FileInformationClass;
    ULONG length = sp->Parameters.SetFile.Length;
    PVOID in;
    BOOLEAN ok;
    NTSTATUS st = STATUS_SUCCESS;

    if (!vcb || !fcb) return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (!vcb->WriteEnabled && cls != FilePositionInformation)
        return SlzFinish(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    in = SlzMapBuffer(Irp, length, FALSE, &ok);
    if (!ok || (!in && length)) return SlzFinish(Irp, STATUS_INVALID_USER_BUFFER, 0);

    switch (cls) {
    case FilePositionInformation:
        if (length < sizeof(FILE_POSITION_INFORMATION)) { st = STATUS_INVALID_PARAMETER; break; }
        sp->FileObject->CurrentByteOffset =
            ((PFILE_POSITION_INFORMATION)in)->CurrentByteOffset;
        break;

    case FileEndOfFileInformation:
        if (length < sizeof(FILE_END_OF_FILE_INFORMATION)) { st = STATUS_INVALID_PARAMETER; break; }
        if (fcb->NodeType != SLZ_NODE_FILE) { st = STATUS_INVALID_DEVICE_REQUEST; break; }
        ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
        st = SlzSetEndOfFile(vcb, fcb,
                             (ULONG64)((PFILE_END_OF_FILE_INFORMATION)in)->EndOfFile.QuadPart);
        ExReleaseResourceLite(&vcb->Lock);
        break;

    case FileDispositionInformation:
        if (length < sizeof(FILE_DISPOSITION_INFORMATION)) { st = STATUS_INVALID_PARAMETER; break; }
        if (fcb->NodeType == SLZ_NODE_ROOT) { st = STATUS_ACCESS_DENIED; break; }
        {
            SLZ_CCB *ccb = (SLZ_CCB *)sp->FileObject->FsContext2;
            BOOLEAN del = ((PFILE_DISPOSITION_INFORMATION)in)->DeleteFile;
            if (ccb && ccb->Magic == SLZ_CCB_MAGIC) ccb->DeleteOnClose = del;
            sp->FileObject->DeletePending = del;
        }
        break;

    case FileRenameInformation:
        {
            PFILE_RENAME_INFORMATION p = (PFILE_RENAME_INFORMATION)in;
            UNICODE_STRING full, parentName, leaf;
            SLZ_FCB *parent = NULL, *base = NULL;
            UCHAR nameBuf[256];
            USHORT nameLen;

            if (length < (ULONG)FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName)) {
                st = STATUS_INVALID_PARAMETER;
                break;
            }
            if (p->RootDirectory) { st = STATUS_NOT_SUPPORTED; break; }
            if (p->FileNameLength == 0 ||
                p->FileNameLength > 2 * SLZ_MAX_PATH) {
                st = STATUS_OBJECT_NAME_INVALID;
                break;
            }
            full.Buffer = p->FileName;
            full.Length = (USHORT)p->FileNameLength;
            full.MaximumLength = full.Length;
            if (!SlzSplitLast(&full, &parentName, &leaf)) {
                st = STATUS_OBJECT_NAME_INVALID;
                break;
            }
            ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
            st = SlzLocate(vcb, base, &parentName, &parent);
            if (NT_SUCCESS(st)) {
                nameLen = (USHORT)SlzWideToUtf8(leaf.Buffer,
                                                leaf.Length / sizeof(WCHAR),
                                                nameBuf, 255);
                if (nameLen == (USHORT)-1 || nameLen == 0) {
                    st = STATUS_OBJECT_NAME_INVALID;
                } else {
                    nameBuf[nameLen] = 0;
                    st = SlzRenameNode(vcb, fcb, parent, nameBuf, nameLen);
                }
                SlzFreeFcb(parent);
            }
            ExReleaseResourceLite(&vcb->Lock);
        }
        break;

    case FileBasicInformation:
        if (length < sizeof(FILE_BASIC_INFORMATION)) { st = STATUS_INVALID_PARAMETER; break; }
        ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
        st = SlzSetBasicInfo(vcb, fcb, (PFILE_BASIC_INFORMATION)in);
        ExReleaseResourceLite(&vcb->Lock);
        break;

    case FileAllocationInformation:
        /* 只做容量提示，不改变文件长度 */
        break;

    case FileValidDataLengthInformation:
        break;

    case FileLinkInformation:
        st = STATUS_NOT_SUPPORTED;
        break;

    default:
        st = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return SlzFinish(Irp, st, 0);
}

/* ============================================================
 * IRP: QUERY_VOLUME_INFORMATION
 * ============================================================ */

static NTSTATUS SlzQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    FS_INFORMATION_CLASS cls = sp->Parameters.QueryVolume.FsInformationClass;
    ULONG length = sp->Parameters.QueryVolume.Length;
    PUCHAR out;
    BOOLEAN ok;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;

    if (!vcb) return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    out = (PUCHAR)SlzMapBuffer(Irp, length, TRUE, &ok);
    if (!ok || (!out && length)) return SlzFinish(Irp, STATUS_INVALID_USER_BUFFER, 0);

    switch (cls) {
    case FileFsVolumeInformation:
        {
            ULONG baseSize = (ULONG)FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION,
                                                  VolumeLabel);
            ULONG labelBytes = 7 * sizeof(WCHAR);   /* SlyzFS + NUL */
            ULONG need = baseSize + labelBytes;
            PFILE_FS_VOLUME_INFORMATION p = (PFILE_FS_VOLUME_INFORMATION)out;

            if (length < sizeof(FILE_FS_VOLUME_INFORMATION)) {
                st = STATUS_BUFFER_OVERFLOW;
                break;
            }
            RtlZeroMemory(p, (length < need) ? sizeof(*p) : need);
            p->VolumeCreationTime.QuadPart = 0;
            p->VolumeSerialNumber = vcb->Serial;
            p->VolumeLabelLength = (USHORT)labelBytes;
            p->SupportsObjects = FALSE;
            if (length < need) {
                info = sizeof(*p);
                st = STATUS_BUFFER_OVERFLOW;
                break;
            }
            RtlCopyMemory(p->VolumeLabel, L"SlyzFS", labelBytes);
            info = need;
        }
        break;

    case FileFsSizeInformation:
        if (length < sizeof(SLZ_FS_SIZE_INFO)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            SLZ_FS_SIZE_INFO *p = (SLZ_FS_SIZE_INFO *)out;
            p->TotalAllocationUnits.QuadPart = (LONGLONG)vcb->TotalClusters;
            p->AvailableAllocationUnits.QuadPart =
                (LONGLONG)(vcb->TotalClusters - vcb->UsedClusters);
            p->SectorsPerAllocationUnit = SLZ_CLUSTER_SECTORS;
            p->BytesPerAllocationUnit = SLZ_CLUSTER_SIZE;
            info = sizeof(*p);
        }
        break;

    case FileFsFullSizeInformation:
        if (length < sizeof(SLZ_FS_FULL_SIZE_INFO)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            SLZ_FS_FULL_SIZE_INFO *p = (SLZ_FS_FULL_SIZE_INFO *)out;
            p->TotalAllocationUnits.QuadPart = (LONGLONG)vcb->TotalClusters;
            p->CallerAvailableAllocationUnits.QuadPart =
                (LONGLONG)(vcb->TotalClusters - vcb->UsedClusters);
            p->ActualAvailableAllocationUnits.QuadPart =
                (LONGLONG)(vcb->TotalClusters - vcb->UsedClusters);
            p->SectorsPerAllocationUnit = SLZ_CLUSTER_SECTORS;
            p->BytesPerAllocationUnit = SLZ_CLUSTER_SIZE;
            info = sizeof(*p);
        }
        break;

    case FileFsAttributeInformation:
        {
            ULONG baseSize = (ULONG)FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION,
                                                 FileSystemName);
            ULONG nameBytes = 7 * sizeof(WCHAR);    /* SlyzFS + NUL */
            ULONG need = baseSize + nameBytes;
            PFILE_FS_ATTRIBUTE_INFORMATION p = (PFILE_FS_ATTRIBUTE_INFORMATION)out;

            if (length < baseSize) { st = STATUS_BUFFER_OVERFLOW; break; }
            RtlZeroMemory(p, (length < need) ? baseSize : need);
            p->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH |
                                      FILE_CASE_PRESERVED_NAMES |
                                      FILE_UNICODE_ON_DISK;
            p->MaximumComponentNameLength = SLZ_MAX_NAME;
            p->FileSystemNameLength = (ULONG)nameBytes;
            if (length < need) {
                info = baseSize;
                st = STATUS_BUFFER_OVERFLOW;
                break;
            }
            RtlCopyMemory(p->FileSystemName, L"SlyzFS", nameBytes);
            info = need;
        }
        break;

    case FileFsDeviceInformation:
        if (length < sizeof(FILE_FS_DEVICE_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            PFILE_FS_DEVICE_INFORMATION p = (PFILE_FS_DEVICE_INFORMATION)out;
            p->DeviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
            p->Characteristics = vcb->WriteEnabled ? 0 : FILE_READ_ONLY_DEVICE;
            info = sizeof(*p);
        }
        break;

#ifndef SLZ_NO_SECTOR_SIZE_INFO
    case FileFsSectorSizeInformation:
        if (length < sizeof(FILE_FS_SECTOR_SIZE_INFORMATION)) { st = STATUS_BUFFER_OVERFLOW; break; }
        {
            PFILE_FS_SECTOR_SIZE_INFORMATION p = (PFILE_FS_SECTOR_SIZE_INFORMATION)out;
            RtlZeroMemory(p, sizeof(*p));
            p->LogicalBytesPerSector = SLZ_SECTOR_SIZE;
            p->PhysicalBytesPerSectorForAtomicity = SLZ_CLUSTER_SIZE;
            p->PhysicalBytesPerSectorForPerformance = SLZ_CLUSTER_SIZE;
            p->FileSystemEffectivePhysicalBytesPerSectorForAtomicity = SLZ_SECTOR_SIZE;
            p->Flags = 0;
            p->ByteOffsetForSectorAlignment = 0;
            p->ByteOffsetForPartitionAlignment = 0;
            info = sizeof(*p);
        }
        break;
#endif

    case FileFsVolumeFlagsInformation:
        if (length < sizeof(FILE_FS_VOLUME_FLAGS_INFORMATION)) {
            st = STATUS_BUFFER_OVERFLOW;
            break;
        }
        ((PFILE_FS_VOLUME_FLAGS_INFORMATION)out)->Flags = 0;
        info = sizeof(FILE_FS_VOLUME_FLAGS_INFORMATION);
        break;

    default:
        st = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return SlzFinish(Irp, st, info);
}

/* ============================================================
 * IRP: DIRECTORY_CONTROL
 * ============================================================ */

typedef struct _SLZ_DIR_INFO {
    WCHAR Name[256];
    USHORT NameChars;
    ULONG Id;
    ULONG64 Size;
    ULONG64 Allocation;
    ULONG Attributes;
    LARGE_INTEGER Creation;
    LARGE_INTEGER Access;
    LARGE_INTEGER Write;
    LARGE_INTEGER Change;
} SLZ_DIR_INFO;

static void SlzFillInfoFromRec(SLZ_DIR_INFO *I, const SLZ_REC *R)
{
    ULONG n = SlzUtf8ToWide(R->Name, R->NameLen, I->Name, 256);
    if (n == (ULONG)-1) n = 0;
    I->NameChars = (USHORT)n;
    I->Id = R->Index;
    I->Size = R->Size;
    I->Allocation = R->Allocation;
    I->Attributes = (R->Flags & 2) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    I->Creation = SlzUnixToTime(R->Created);
    I->Access = SlzUnixToTime(R->Accessed);
    I->Write = SlzUnixToTime(R->Modified);
    I->Change = SlzUnixToTime(R->Modified);
}

static ULONG SlzWriteDirEntry(PUCHAR Buf, ULONG Max, ULONG Index,
                              const SLZ_DIR_INFO *I,
                              FILE_INFORMATION_CLASS Class)
{
    ULONG nameBytes = (ULONG)I->NameChars * sizeof(WCHAR);
    ULONG base, total;

    switch (Class) {
    case FileDirectoryInformation:
        base = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        total = (base + nameBytes + 7) & ~7u;
        if (total > Max) return 0;
        {
            PFILE_DIRECTORY_INFORMATION p = (PFILE_DIRECTORY_INFORMATION)Buf;
            RtlZeroMemory(p, base);
            p->NextEntryOffset = 0;
            p->FileIndex = Index;
            p->CreationTime = I->Creation;
            p->LastAccessTime = I->Access;
            p->LastWriteTime = I->Write;
            p->ChangeTime = I->Change;
            p->EndOfFile.QuadPart = (LONGLONG)I->Size;
            p->AllocationSize.QuadPart = (LONGLONG)I->Allocation;
            p->FileAttributes = I->Attributes;
            p->FileNameLength = nameBytes;
            RtlCopyMemory(p->FileName, I->Name, nameBytes);
        }
        return total;

    case FileFullDirectoryInformation:
        base = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        total = (base + nameBytes + 7) & ~7u;
        if (total > Max) return 0;
        {
            PFILE_FULL_DIR_INFORMATION p = (PFILE_FULL_DIR_INFORMATION)Buf;
            RtlZeroMemory(p, base);
            p->NextEntryOffset = 0;
            p->FileIndex = Index;
            p->CreationTime = I->Creation;
            p->LastAccessTime = I->Access;
            p->LastWriteTime = I->Write;
            p->ChangeTime = I->Change;
            p->EndOfFile.QuadPart = (LONGLONG)I->Size;
            p->AllocationSize.QuadPart = (LONGLONG)I->Allocation;
            p->FileAttributes = I->Attributes;
            p->EaSize = 0;
            p->FileNameLength = nameBytes;
            RtlCopyMemory(p->FileName, I->Name, nameBytes);
        }
        return total;

    case FileBothDirectoryInformation:
        base = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        total = (base + nameBytes + 7) & ~7u;
        if (total > Max) return 0;
        {
            PFILE_BOTH_DIR_INFORMATION p = (PFILE_BOTH_DIR_INFORMATION)Buf;
            RtlZeroMemory(p, base);
            p->NextEntryOffset = 0;
            p->FileIndex = Index;
            p->CreationTime = I->Creation;
            p->LastAccessTime = I->Access;
            p->LastWriteTime = I->Write;
            p->ChangeTime = I->Change;
            p->EndOfFile.QuadPart = (LONGLONG)I->Size;
            p->AllocationSize.QuadPart = (LONGLONG)I->Allocation;
            p->FileAttributes = I->Attributes;
            p->EaSize = 0;
            p->ShortNameLength = 0;
            p->FileNameLength = nameBytes;
            RtlCopyMemory(p->FileName, I->Name, nameBytes);
        }
        return total;

    case FileIdBothDirectoryInformation:
        base = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName);
        total = (base + nameBytes + 7) & ~7u;
        if (total > Max) return 0;
        {
            PFILE_ID_BOTH_DIR_INFORMATION p = (PFILE_ID_BOTH_DIR_INFORMATION)Buf;
            RtlZeroMemory(p, base);
            p->NextEntryOffset = 0;
            p->FileIndex = Index;
            p->CreationTime = I->Creation;
            p->LastAccessTime = I->Access;
            p->LastWriteTime = I->Write;
            p->ChangeTime = I->Change;
            p->EndOfFile.QuadPart = (LONGLONG)I->Size;
            p->AllocationSize.QuadPart = (LONGLONG)I->Allocation;
            p->FileAttributes = I->Attributes;
            p->EaSize = 0;
            p->ShortNameLength = 0;
            p->FileId.QuadPart = I->Id;
            p->FileNameLength = nameBytes;
            RtlCopyMemory(p->FileName, I->Name, nameBytes);
        }
        return total;

    case FileIdFullDirectoryInformation:
        base = FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName);
        total = (base + nameBytes + 7) & ~7u;
        if (total > Max) return 0;
        {
            PFILE_ID_FULL_DIR_INFORMATION p = (PFILE_ID_FULL_DIR_INFORMATION)Buf;
            RtlZeroMemory(p, base);
            p->NextEntryOffset = 0;
            p->FileIndex = Index;
            p->CreationTime = I->Creation;
            p->LastAccessTime = I->Access;
            p->LastWriteTime = I->Write;
            p->ChangeTime = I->Change;
            p->EndOfFile.QuadPart = (LONGLONG)I->Size;
            p->AllocationSize.QuadPart = (LONGLONG)I->Allocation;
            p->FileAttributes = I->Attributes;
            p->EaSize = 0;
            p->FileId.QuadPart = I->Id;
            p->FileNameLength = nameBytes;
            RtlCopyMemory(p->FileName, I->Name, nameBytes);
        }
        return total;

    case FileNamesInformation:
        base = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        total = (base + nameBytes + 7) & ~7u;
        if (total > Max) return 0;
        {
            PFILE_NAMES_INFORMATION p = (PFILE_NAMES_INFORMATION)Buf;
            RtlZeroMemory(p, base);
            p->NextEntryOffset = 0;
            p->FileIndex = Index;
            p->FileNameLength = nameBytes;
            RtlCopyMemory(p->FileName, I->Name, nameBytes);
        }
        return total;

    default:
        return 0;
    }
}

static ULONG SlzDirLogicalCount(SLZ_VCB *Vcb, SLZ_FCB *Fcb, SLZ_CCB *Ccb)
{
    if (Fcb->NodeType == SLZ_NODE_ROOT) {
        ULONG n = 0, i;
        for (i = 0; i < SLZ_MAX_SUBVOLS; i++) if (Vcb->Sub[i].Present) n++;
        return n;
    }
    return 2 + Ccb->DirCount;
}

static NTSTATUS SlzGetDirInfoAt(SLZ_VCB *Vcb, SLZ_FCB *Fcb, SLZ_CCB *Ccb,
                                ULONG Index, SLZ_DIR_INFO *Info)
{
    RtlZeroMemory(Info, sizeof(*Info));

    if (Fcb->NodeType == SLZ_NODE_ROOT) {
        ULONG seen = 0, i;
        for (i = 0; i < SLZ_MAX_SUBVOLS; i++) {
            if (!Vcb->Sub[i].Present) continue;
            if (seen == Index) {
                ULONG labelLen = 0;
                ULONG n;
                SLZ_REC rr;
                NTSTATUS st;
                while (labelLen < 24 && Vcb->Sub[i].Label[labelLen]) labelLen++;
                n = SlzUtf8ToWide(Vcb->Sub[i].Label, labelLen,
                                  Info->Name, 256);
                Info->NameChars = (n == (ULONG)-1) ? 0 : (USHORT)n;
                Info->Attributes = FILE_ATTRIBUTE_DIRECTORY;
                Info->Id = 0x100 + i;
                st = SlzReadRecord(Vcb, &Vcb->Sub[i],
                                   (ULONG)Vcb->Sub[i].RootRecord, &rr);
                if (NT_SUCCESS(st)) {
                    Info->Creation = SlzUnixToTime(rr.Created);
                    Info->Access = SlzUnixToTime(rr.Accessed);
                    Info->Write = SlzUnixToTime(rr.Modified);
                    Info->Change = SlzUnixToTime(rr.Modified);
                }
                return STATUS_SUCCESS;
            }
            seen++;
        }
        return STATUS_NO_MORE_FILES;
    }

    if (Index == 0) {
        Info->Name[0] = '.';
        Info->Name[1] = 0;
        Info->NameChars = 1;
        Info->Id = Fcb->RecordIndex;
        Info->Attributes = FILE_ATTRIBUTE_DIRECTORY;
        Info->Creation = Fcb->CreationTime;
        Info->Access = Fcb->LastAccessTime;
        Info->Write = Fcb->LastWriteTime;
        Info->Change = Fcb->ChangeTime;
        return STATUS_SUCCESS;
    }
    if (Index == 1) {
        Info->Name[0] = '.';
        Info->Name[1] = '.';
        Info->Name[2] = 0;
        Info->NameChars = 2;
        Info->Id = (ULONG)Fcb->ParentRecord;
        Info->Attributes = FILE_ATTRIBUTE_DIRECTORY;
        Info->Creation = Fcb->CreationTime;
        Info->Access = Fcb->LastAccessTime;
        Info->Write = Fcb->LastWriteTime;
        Info->Change = Fcb->ChangeTime;
        return STATUS_SUCCESS;
    }

    Index -= 2;
    if (Index >= Ccb->DirCount || !Ccb->DirCache)
        return STATUS_NO_MORE_FILES;
    {
        SLZ_REC rec;
        NTSTATUS st = SlzReadRecord(Vcb, Fcb->Sub, Ccb->DirCache[Index].Ref, &rec);
        if (!NT_SUCCESS(st)) return st;
        SlzFillInfoFromRec(Info, &rec);
        return STATUS_SUCCESS;
    }
}

static BOOLEAN SlzGetPattern(PIO_STACK_LOCATION Sp, WCHAR *Buf, ULONG *Chars)
{
    PUNICODE_STRING pat = Sp->Parameters.QueryDirectory.FileName;
    ULONG len;
    BOOLEAN ok = TRUE;

    Buf[0] = 0;
    *Chars = 0;
    if (!pat) return TRUE;

    __try {
        len = pat->Length;
        if (len != 0 && pat->Buffer) {
            if (len > 2 * 255) {
                ok = FALSE;
            } else {
                RtlCopyMemory(Buf, pat->Buffer, len);
                *Chars = len / sizeof(WCHAR);
                Buf[*Chars] = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    return ok;
}

static NTSTATUS SlzDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    SLZ_FCB *fcb = (SLZ_FCB *)sp->FileObject->FsContext;
    SLZ_CCB *ccb = (SLZ_CCB *)sp->FileObject->FsContext2;
    NTSTATUS st = STATUS_SUCCESS;
    PUCHAR out;
    BOOLEAN ok;
    ULONG length, offset = 0, wrote = 0, lastOff = (ULONG)-1;
    ULONG total, index;
    FILE_INFORMATION_CLASS cls;
    WCHAR pattern[256];
    ULONG patternChars = 0;
    BOOLEAN restart, single;

    if (!vcb || !fcb || !ccb) return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (sp->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (fcb->NodeType != SLZ_NODE_DIR && fcb->NodeType != SLZ_NODE_ROOT)
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    cls = sp->Parameters.QueryDirectory.FileInformationClass;
    length = sp->Parameters.QueryDirectory.Length;
    restart = (sp->Flags & SL_RESTART_SCAN) ? TRUE : FALSE;
    single = (sp->Flags & SL_RETURN_SINGLE_ENTRY) ? TRUE : FALSE;

    out = (PUCHAR)SlzMapBuffer(Irp, length, TRUE, &ok);
    if (!ok || (!out && length)) return SlzFinish(Irp, STATUS_INVALID_USER_BUFFER, 0);
    if (length == 0) return SlzFinish(Irp, STATUS_BUFFER_OVERFLOW, 0);
    if (!SlzGetPattern(sp, pattern, &patternChars))
        return SlzFinish(Irp, STATUS_INVALID_PARAMETER, 0);
    /* DOS 通配符语义："*.*" 等价于 "*" */
    if (patternChars == 3 && pattern[0] == '*' && pattern[1] == '.' &&
        pattern[2] == '*')
        patternChars = 0;

    ExAcquireResourceSharedLite(&vcb->Lock, TRUE);

    if (restart || !ccb->CacheValid) {
        if (ccb->DirCache) {
            ExFreePoolWithTag(ccb->DirCache, SLZ_TAG);
            ccb->DirCache = NULL;
        }
        ccb->DirCount = 0;
        if (fcb->NodeType != SLZ_NODE_ROOT) {
            SLZ_REC rec;
            st = SlzReadRecord(vcb, fcb->Sub, fcb->RecordIndex, &rec);
            if (!NT_SUCCESS(st)) goto unlock;
            st = SlzDirCollect(vcb, fcb->Sub, &rec, &ccb->DirCache,
                               &ccb->DirCount);
            if (!NT_SUCCESS(st)) goto unlock;
        }
        ccb->CacheValid = TRUE;
        ccb->DirIndex = 0;
    }

    index = ccb->DirIndex;

    for (;;) {
        SLZ_DIR_INFO info;

        if (index >= SlzDirLogicalCount(vcb, fcb, ccb)) {
            st = wrote ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
            break;
        }

        st = SlzGetDirInfoAt(vcb, fcb, ccb, index, &info);
        if (!NT_SUCCESS(st)) break;

        if (patternChars && !SlzWildcardMatch(pattern, patternChars,
                                              info.Name, info.NameChars)) {
            index++;
            ccb->DirIndex = index;
            continue;
        }

        total = SlzWriteDirEntry(out + offset, length - offset, index, &info, cls);
        if (total == 0) {
            if (wrote == 0) {
                st = (cls == FileDirectoryInformation ||
                      cls == FileFullDirectoryInformation ||
                      cls == FileBothDirectoryInformation ||
                      cls == FileIdBothDirectoryInformation ||
                      cls == FileIdFullDirectoryInformation ||
                      cls == FileNamesInformation)
                     ? STATUS_BUFFER_OVERFLOW
                     : STATUS_INVALID_INFO_CLASS;
            } else {
                st = STATUS_SUCCESS;
            }
            break;
        }

        if (lastOff != (ULONG)-1)
            *(PULONG)(out + lastOff) = offset - lastOff;
        lastOff = offset;
        offset += total;
        wrote += total;
        index++;
        ccb->DirIndex = index;

        if (single) { st = STATUS_SUCCESS; break; }
        if (offset >= length) { st = STATUS_SUCCESS; break; }
    }

unlock:
    ExReleaseResourceLite(&vcb->Lock);
    return SlzFinish(Irp, st, wrote);
}

/* ============================================================
 * IRP: FILE_SYSTEM_CONTROL
 * ============================================================ */

static NTSTATUS SlzFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;
    PVPB vpb;

    switch (sp->MinorFunction) {
    case IRP_MN_MOUNT_VOLUME:
        st = SlzMountVolume(sp);
        return SlzFinish(Irp, st, 0);

    case IRP_MN_VERIFY_VOLUME:
        vpb = sp->Parameters.VerifyVolume.Vpb;
        if (vpb && (vpb->Flags & VPB_MOUNTED))
            return SlzFinish(Irp, STATUS_SUCCESS, 0);
        return SlzFinish(Irp, STATUS_UNRECOGNIZED_VOLUME, 0);

    case IRP_MN_LOAD_FILE_SYSTEM:
        return SlzFinish(Irp, STATUS_SUCCESS, 0);

    case IRP_MN_USER_FS_REQUEST:
        switch (sp->Parameters.FileSystemControl.FsControlCode) {
        case FSCTL_IS_VOLUME_MOUNTED:
        case FSCTL_LOCK_VOLUME:
        case FSCTL_UNLOCK_VOLUME:
            return SlzFinish(Irp, STATUS_SUCCESS, 0);
        case FSCTL_DISMOUNT_VOLUME:
            return SlzFinish(Irp, STATUS_SUCCESS, 0);
        default:
            return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }

    default:
        UNREFERENCED_PARAMETER(DeviceObject);
        return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* ============================================================
 * 其余 IRP
 * ============================================================ */

static NTSTATUS SlzUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return SlzFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS SlzFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);
    NTSTATUS st = STATUS_SUCCESS;

    if (vcb && vcb->WriteEnabled) {
        ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
        st = SlzFlushDevice(vcb->Target);
        ExReleaseResourceLite(&vcb->Lock);
    }
    return SlzFinish(Irp, st, 0);
}

static NTSTATUS SlzQueryEa(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return SlzFinish(Irp, STATUS_NO_EAS_ON_FILE, 0);
}

static NTSTATUS SlzShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    SLZ_VCB *vcb = SlzGetVcb(DeviceObject);

    if (vcb && vcb->WriteEnabled) {
        ExAcquireResourceExclusiveLite(&vcb->Lock, TRUE);
        SlzFlushDevice(vcb->Target);
        ExReleaseResourceLite(&vcb->Lock);
    }
    return SlzFinish(Irp, STATUS_SUCCESS, 0);
}

/* ============================================================
 * DriverEntry
 * ============================================================ */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS st;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    g_DriverObject = DriverObject;
    /* 不设置 DriverUnload：卷设备存在时卸载没有意义，留空最安全 */
    DriverObject->DriverUnload = NULL;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = SlzUnsupported;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = SlzCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SlzClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = SlzCleanup;
    DriverObject->MajorFunction[IRP_MJ_READ] = SlzRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = SlzWrite;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] = SlzQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] = SlzSetInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
        SlzQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = SlzDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = SlzFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = SlzFlushBuffers;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = SlzShutdown;
    DriverObject->MajorFunction[IRP_MJ_QUERY_EA] = SlzQueryEa;
    DriverObject->MajorFunction[IRP_MJ_SET_EA] = SlzUnsupported;

    st = IoCreateDevice(DriverObject, 0, NULL, FILE_DEVICE_DISK_FILE_SYSTEM, 0,
                        FALSE, &g_ControlDevice);
    if (!NT_SUCCESS(st)) return st;

    IoRegisterFileSystem(g_ControlDevice);
    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
