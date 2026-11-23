/* ============================================================
 * SlyzFsWrite.c - SlyzFS v3 写路径
 * ------------------------------------------------------------
 * 完全对齐 src\fs\slyzfs2.slyz3 的写规则：
 *   - MFT 记录 4096B，写盘前套 USA；
 *   - 目录索引块 4096B（INDX），条目 8B 对齐；
 *   - 簇位图分段提交；
 *   - 元数据先写 SLOG（PAGE* + COMMIT），flush 后再落最终位置，
 *     最后推进 checkpoint。挂载时只重放带 COMMIT 的事务。
 *
 * 文件数据与 OS 一致：直接写最终位置，不进日志。
 * 所有入口在 PASSIVE_LEVEL 且由卷锁串行化。
 * ============================================================ */

#include "SlyzFs.h"

#ifdef SLZ_SYNTAX_CHECK
#define __try if (1)
#define __except(x) else if (0)
#endif

#define SLZ_LOG_MAGIC      0x474F4C53u   /* "SLOG" */
#define SLZ_LOGREC_MAGIC   0x4345524Cu   /* "SLREC" */
#define SLZ_LOG_TYPE_PAGE  1u
#define SLZ_LOG_TYPE_COMMIT 2u
#define SLZ_ATTR_END       0xFFFFFFFFu

#define U16(p, o) (*(USHORT UNALIGNED *)((PUCHAR)(p) + (o)))
#define U32(p, o) (*(ULONG UNALIGNED *)((PUCHAR)(p) + (o)))
#define U64(p, o) (*(ULONG64 UNALIGNED *)((PUCHAR)(p) + (o)))
#define W16(p, o, v) (*(USHORT UNALIGNED *)((PUCHAR)(p) + (o)) = (USHORT)(v))
#define W32(p, o, v) (*(ULONG UNALIGNED *)((PUCHAR)(p) + (o)) = (ULONG)(v))
#define W64(p, o, v) (*(ULONG64 UNALIGNED *)((PUCHAR)(p) + (o)) = (ULONG64)(v))

static ULONG64 SlzMin64(ULONG64 a, ULONG64 b) { return a < b ? a : b; }

static ULONG32 SlzNowUnix(VOID)
{
    LARGE_INTEGER t;
    LONGLONG v;

    KeQuerySystemTime(&t);
    v = (t.QuadPart - 116444736000000000LL) / 10000000LL;
    if (v < 0) return 0;
    if (v > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
    return (ULONG32)v;
}

static void SlzTxnFree(SLZ_TXN *T)
{
    ULONG i;
    for (i = 0; i < T->Count; i++) {
        if (T->Page[i].Data) {
            ExFreePoolWithTag(T->Page[i].Data, SLZ_TAG);
            T->Page[i].Data = NULL;
        }
    }
    T->Count = 0;
}

static void SlzTxnInit(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub)
{
    RtlZeroMemory(T, sizeof(*T));
    T->Vcb = Vcb;
    T->Sub = Sub;
}

static NTSTATUS SlzTxnStage(SLZ_TXN *T, ULONG64 Lba, PVOID Data, ULONG Bytes)
{
    ULONG i;
    PUCHAR copy;
    ULONG n;

    if (Bytes == 0 || Bytes > SLZ_RECORD_SIZE) return STATUS_INVALID_PARAMETER;
    n = (Bytes + SLZ_SECTOR_SIZE - 1) & ~(SLZ_SECTOR_SIZE - 1);

    for (i = 0; i < T->Count; i++) {
        if (T->Page[i].Lba == Lba) {
            RtlCopyMemory(T->Page[i].Data, Data, Bytes);
            if (n > T->Page[i].Bytes) T->Page[i].Bytes = n;
            return STATUS_SUCCESS;
        }
    }
    if (T->Count >= SLZ_TXN_MAX) return STATUS_INSUFFICIENT_RESOURCES;

    copy = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!copy) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(copy, SLZ_RECORD_SIZE);
    RtlCopyMemory(copy, Data, Bytes);
    T->Page[T->Count].Lba = Lba;
    T->Page[T->Count].Bytes = n;
    T->Page[T->Count].Data = copy;
    T->Count++;
    return STATUS_SUCCESS;
}

static NTSTATUS SlzLogWriteSlog(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub)
{
    UCHAR hdr[SLZ_SECTOR_SIZE];
    NTSTATUS st;
    ULONG head;

    RtlZeroMemory(hdr, sizeof(hdr));
    head = Sub->LogHead;
    if (Sub->LogRecords) head %= Sub->LogRecords;
    W32(hdr, 0, SLZ_LOG_MAGIC);
    W32(hdr, 4, 2);
    W32(hdr, 8, head);
    W32(hdr, 12, head);
    W64(hdr, 16, Sub->LogSeq);
    W32(hdr, 28, SlzCrc32(hdr, 28));

    st = SlzWriteSectors(Vcb->Target, Sub->LogLba * SLZ_SECTOR_SIZE, hdr,
                         SLZ_SECTOR_SIZE);
    if (NT_SUCCESS(st)) Sub->LogHead = head;
    return st;
}

static NTSTATUS SlzLogAppendPage(SLZ_TXN *T, ULONG64 Lba, PVOID Data, ULONG Bytes)
{
    SLZ_SUBVOL *s = T->Sub;
    UCHAR hdr[SLZ_SECTOR_SIZE];
    ULONG head;
    ULONG64 base;
    NTSTATUS st;

    if (!s->LogOk || s->LogRecords == 0) return STATUS_MEDIA_WRITE_PROTECTED;
    if (Bytes == 0 || Bytes > SLZ_RECORD_SIZE) return STATUS_INVALID_PARAMETER;

    head = s->LogHead;
    if (head >= s->LogRecords) head = 0;

    RtlZeroMemory(hdr, sizeof(hdr));
    W32(hdr, 0, SLZ_LOGREC_MAGIC);
    W32(hdr, 4, SLZ_LOG_TYPE_PAGE);
    W64(hdr, 8, Lba);
    W32(hdr, 16, Bytes);
    W32(hdr, 20, SlzCrc32(Data, Bytes));
    W64(hdr, 24, s->LogSeq);

    base = s->LogLba + 1 + (ULONG64)head * 9;
    st = SlzWriteSectors(T->Vcb->Target, base * SLZ_SECTOR_SIZE, hdr,
                         SLZ_SECTOR_SIZE);
    if (!NT_SUCCESS(st)) return st;
    st = SlzWriteSectors(T->Vcb->Target, (base + 1) * SLZ_SECTOR_SIZE, Data, Bytes);
    if (!NT_SUCCESS(st)) return st;

    s->LogHead = head + 1;
    s->LogSeq++;
    return STATUS_SUCCESS;
}

static NTSTATUS SlzLogAppendCommit(SLZ_TXN *T)
{
    SLZ_SUBVOL *s = T->Sub;
    UCHAR hdr[SLZ_SECTOR_SIZE];
    ULONG head;
    ULONG64 base;
    NTSTATUS st;

    if (!s->LogOk || s->LogRecords == 0) return STATUS_MEDIA_WRITE_PROTECTED;
    head = s->LogHead;
    if (head >= s->LogRecords) head = 0;

    RtlZeroMemory(hdr, sizeof(hdr));
    W32(hdr, 0, SLZ_LOGREC_MAGIC);
    W32(hdr, 4, SLZ_LOG_TYPE_COMMIT);
    W64(hdr, 24, s->LogSeq);

    base = s->LogLba + 1 + (ULONG64)head * 9;
    st = SlzWriteSectors(T->Vcb->Target, base * SLZ_SECTOR_SIZE, hdr,
                         SLZ_SECTOR_SIZE);
    if (!NT_SUCCESS(st)) return st;

    s->LogHead = head + 1;
    s->LogSeq++;
    return STATUS_SUCCESS;
}

static NTSTATUS SlzTxnCommit(SLZ_TXN *T)
{
    SLZ_VCB *v = T->Vcb;
    SLZ_SUBVOL *s = T->Sub;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG i;

    if (T->Count == 0) return STATUS_SUCCESS;
    if (!s->LogOk) { SlzTxnFree(T); return STATUS_MEDIA_WRITE_PROTECTED; }

    for (i = 0; i < T->Count; i++) {
        st = SlzLogAppendPage(T, T->Page[i].Lba, T->Page[i].Data,
                              T->Page[i].Bytes);
        if (!NT_SUCCESS(st)) break;
    }
    if (NT_SUCCESS(st)) st = SlzLogAppendCommit(T);
    if (NT_SUCCESS(st)) st = SlzFlushDevice(v->Target);
    if (NT_SUCCESS(st)) {
        for (i = 0; i < T->Count; i++) {
            st = SlzWriteSectors(v->Target, T->Page[i].Lba * SLZ_SECTOR_SIZE,
                                 T->Page[i].Data, T->Page[i].Bytes);
            if (!NT_SUCCESS(st)) break;
        }
    }
    if (NT_SUCCESS(st)) st = SlzFlushDevice(v->Target);
    if (NT_SUCCESS(st)) st = SlzLogWriteSlog(v, s);
    if (NT_SUCCESS(st)) st = SlzFlushDevice(v->Target);

    if (!NT_SUCCESS(st)) {
        /* 内存里的位图/日志状态已不可信：本卷立即转只读，等重新挂载 */
        s->LogOk = FALSE;
        v->WriteEnabled = FALSE;
    }
    SlzTxnFree(T);
    return st;
}

/* ============================================================
 * 日志重放
 * ============================================================ */

NTSTATUS SlzJournalReplay(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub)
{
    UCHAR slog[SLZ_SECTOR_SIZE];
    ULONG records, head, ckpt, idx, n, i;
    ULONG64 seq;
    NTSTATUS st;
    BOOLEAN applied;
    struct {
        ULONG64 Lba;
        ULONG   Bytes;
        PUCHAR  Data;
    } pages[SLZ_TXN_MAX];
    ULONG pageCount;

    Sub->LogOk = FALSE;
    if (Sub->LogSectors < 10) return STATUS_SUCCESS;
    records = (Sub->LogSectors - 1) / 9;
    if (records == 0) return STATUS_SUCCESS;
    Sub->LogRecords = records;

    st = SlzReadSectors(Vcb->Target, Sub->LogLba * SLZ_SECTOR_SIZE, slog,
                        SLZ_SECTOR_SIZE);
    if (!NT_SUCCESS(st)) return st;
    if (U32(slog, 0) != SLZ_LOG_MAGIC) return STATUS_SUCCESS;
    if (U32(slog, 4) != 2) return STATUS_SUCCESS;
    if (U32(slog, 28) != SlzCrc32(slog, 28)) return STATUS_SUCCESS;

    head = U32(slog, 8);
    ckpt = U32(slog, 12);
    seq = U64(slog, 16);
    if (head > records) head = records;
    if (ckpt > records) ckpt = records;
    if (head == records) head = 0;

    n = (head + records - ckpt) % records;
    pageCount = 0;
    applied = TRUE;

    for (i = 0; i < n; i++) {
        UCHAR rec[SLZ_SECTOR_SIZE];
        ULONG64 base;
        ULONG type, bytes;

        idx = (ckpt + i) % records;
        base = Sub->LogLba + 1 + (ULONG64)idx * 9;
        st = SlzReadSectors(Vcb->Target, base * SLZ_SECTOR_SIZE, rec,
                            SLZ_SECTOR_SIZE);
        if (!NT_SUCCESS(st)) { applied = FALSE; break; }
        if (U32(rec, 0) != SLZ_LOGREC_MAGIC) { applied = FALSE; break; }

        type = U32(rec, 4);
        if (type == SLZ_LOG_TYPE_PAGE) {
            bytes = U32(rec, 16);
            if (bytes == 0 || bytes > SLZ_RECORD_SIZE ||
                pageCount >= SLZ_TXN_MAX) { applied = FALSE; break; }
            pages[pageCount].Data = ExAllocatePoolWithTag(NonPagedPool,
                                                          SLZ_RECORD_SIZE,
                                                          SLZ_TAG);
            if (!pages[pageCount].Data) { applied = FALSE; break; }
            RtlZeroMemory(pages[pageCount].Data, SLZ_RECORD_SIZE);
            pages[pageCount].Lba = U64(rec, 8);
            pages[pageCount].Bytes = bytes;
            st = SlzReadSectors(Vcb->Target,
                                (base + 1) * SLZ_SECTOR_SIZE,
                                pages[pageCount].Data,
                                (bytes + SLZ_SECTOR_SIZE - 1) &
                                ~(SLZ_SECTOR_SIZE - 1));
            if (!NT_SUCCESS(st)) { applied = FALSE; break; }
            if (U32(rec, 20) != SlzCrc32(pages[pageCount].Data, bytes)) {
                applied = FALSE;
                break;
            }
            pageCount++;
        } else if (type == SLZ_LOG_TYPE_COMMIT) {
            ULONG p;
            for (p = 0; p < pageCount; p++) {
                st = SlzWriteSectors(Vcb->Target,
                                     pages[p].Lba * SLZ_SECTOR_SIZE,
                                     pages[p].Data, pages[p].Bytes);
                if (!NT_SUCCESS(st)) break;
            }
            if (NT_SUCCESS(st)) st = SlzFlushDevice(Vcb->Target);
            for (p = 0; p < pageCount; p++) {
                ExFreePoolWithTag(pages[p].Data, SLZ_TAG);
                pages[p].Data = NULL;
            }
            pageCount = 0;
            if (!NT_SUCCESS(st)) { applied = FALSE; break; }
        }
    }

    for (i = 0; i < pageCount; i++) {
        if (pages[i].Data) ExFreePoolWithTag(pages[i].Data, SLZ_TAG);
    }

    Sub->LogHead = head;
    Sub->LogSeq = seq ? seq : 1;
    Sub->LogOk = applied;
    if (applied) {
        SlzLogWriteSlog(Vcb, Sub);
        SlzFlushDevice(Vcb->Target);
    }
    return STATUS_SUCCESS;
}

/* ============================================================
 * 位图
 * ============================================================ */

NTSTATUS SlzLoadBitmap(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub)
{
    ULONG bytes;
    ULONG64 need;
    NTSTATUS st;
    ULONG64 used;
    ULONG64 i;

    if (Sub->BitmapSectors == 0 || Sub->BitmapSectors > 16384) {
        Sub->LogOk = FALSE;
        return STATUS_INVALID_PARAMETER;
    }
    bytes = Sub->BitmapSectors * SLZ_SECTOR_SIZE;
    need = (Sub->TotalClusters + 7) / 8;
    if (bytes == 0 || bytes < need || bytes > 8 * 1024 * 1024) {
        Sub->LogOk = FALSE;
        return STATUS_INVALID_PARAMETER;
    }

    Sub->Bitmap = ExAllocatePoolWithTag(NonPagedPool, bytes, SLZ_TAG);
    if (!Sub->Bitmap) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Sub->Bitmap, bytes);
    st = SlzReadSectors(Vcb->Target, Sub->BitmapLba * SLZ_SECTOR_SIZE,
                        Sub->Bitmap, bytes);
    if (!NT_SUCCESS(st)) {
        ExFreePoolWithTag(Sub->Bitmap, SLZ_TAG);
        Sub->Bitmap = NULL;
        return st;
    }
    Sub->BitmapBytes = bytes;

    used = 0;
    for (i = 0; i < Sub->TotalClusters; i++) {
        if ((Sub->Bitmap[i >> 3] >> (i & 7)) & 1) used++;
    }
    Sub->UsedClusters = used;
    Sub->AllocHint = 8;
    return STATUS_SUCCESS;
}

static BOOLEAN SlzBitGet(SLZ_SUBVOL *Sub, ULONG64 Cluster)
{
    return (BOOLEAN)((Sub->Bitmap[Cluster >> 3] >> (Cluster & 7)) & 1);
}

static void SlzBitSet(SLZ_SUBVOL *Sub, ULONG64 Cluster, BOOLEAN Value)
{
    ULONG byte = (ULONG)(Cluster >> 3);
    UCHAR mask = (UCHAR)(1u << (Cluster & 7));
    if (Value) Sub->Bitmap[byte] |= mask;
    else Sub->Bitmap[byte] &= (UCHAR)~mask;
}

static NTSTATUS SlzStageBitmapRange(SLZ_TXN *T, SLZ_SUBVOL *Sub,
                                    ULONG64 First, ULONG64 Count)
{
    ULONG64 s0, s1, s;
    NTSTATUS st;

    if (Count == 0) return STATUS_SUCCESS;
    s0 = (First >> 3) >> 9;
    s1 = ((First + Count - 1) >> 3) >> 9;
    if (s1 >= Sub->BitmapSectors) return STATUS_INVALID_PARAMETER;

    for (s = s0; s <= s1; s++) {
        st = SlzTxnStage(T, Sub->BitmapLba + s, Sub->Bitmap + s * SLZ_SECTOR_SIZE,
                         SLZ_SECTOR_SIZE);
        if (!NT_SUCCESS(st)) return st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SlzAllocRun(SLZ_TXN *T, SLZ_SUBVOL *Sub, ULONG32 Clusters,
                            ULONG64 *Out)
{
    ULONG64 total = Sub->TotalClusters;
    ULONG64 c, scanned, i;
    NTSTATUS st;

    if (Clusters == 0 || Clusters > total) return STATUS_DISK_FULL;
    c = Sub->AllocHint;
    if (c < 8 || c + Clusters > total) c = 8;

    scanned = 0;
    while (scanned < total) {
        BOOLEAN ok = TRUE;
        if (c + Clusters > total) { c = 8; scanned++; continue; }
        for (i = 0; i < Clusters; i++) {
            if (SlzBitGet(Sub, c + i)) { ok = FALSE; break; }
        }
        if (ok) {
            for (i = 0; i < Clusters; i++) SlzBitSet(Sub, c + i, TRUE);
            st = SlzStageBitmapRange(T, Sub, c, Clusters);
            if (!NT_SUCCESS(st)) {
                for (i = 0; i < Clusters; i++) SlzBitSet(Sub, c + i, FALSE);
                return st;
            }
            Sub->UsedClusters += Clusters;
            Sub->AllocHint = c + Clusters;
            if (Sub->AllocHint >= total) Sub->AllocHint = 8;
            *Out = c;
            return STATUS_SUCCESS;
        }
        c++;
        scanned++;
    }
    return STATUS_DISK_FULL;
}

static NTSTATUS SlzFreeRun(SLZ_TXN *T, SLZ_SUBVOL *Sub, ULONG64 First,
                           ULONG32 Clusters)
{
    ULONG64 i;
    NTSTATUS st;

    if (Clusters == 0) return STATUS_SUCCESS;
    if (First >= Sub->TotalClusters) return STATUS_INVALID_PARAMETER;
    if (First + Clusters > Sub->TotalClusters)
        Clusters = (ULONG32)(Sub->TotalClusters - First);

    for (i = 0; i < Clusters; i++) SlzBitSet(Sub, First + i, FALSE);
    st = SlzStageBitmapRange(T, Sub, First, Clusters);
    if (!NT_SUCCESS(st)) return st;
    if (Sub->UsedClusters >= Clusters) Sub->UsedClusters -= Clusters;
    else Sub->UsedClusters = 0;
    if (First < Sub->AllocHint) Sub->AllocHint = First;
    return STATUS_SUCCESS;
}

/* ============================================================
 * MFT 记录
 * ============================================================ */

static NTSTATUS SlzReadRawRecord(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, ULONG Index,
                                 PUCHAR Buf)
{
    NTSTATUS st;
    ULONG64 lba;

    if (!Sub || Index >= Sub->MftRecords) return STATUS_FILE_CORRUPT_ERROR;
    lba = Sub->MftLba + (ULONG64)Index * SLZ_RECORD_SECTORS;
    if (lba + SLZ_RECORD_SECTORS > Sub->BaseLba + Sub->Sectors)
        return STATUS_FILE_CORRUPT_ERROR;

    st = SlzReadSectors(Vcb->Target, lba * SLZ_SECTOR_SIZE, Buf, SLZ_RECORD_SIZE);
    if (!NT_SUCCESS(st)) return st;
    if (U32(Buf, 0) != SLZ_FILE_MAGIC) return STATUS_FILE_CORRUPT_ERROR;
    if (!SlzRestoreUsa(Buf)) return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS SlzStageRawRecord(SLZ_TXN *T, ULONG Index, PUCHAR Buf)
{
    NTSTATUS st;
    ULONG64 lba = T->Sub->MftLba + (ULONG64)Index * SLZ_RECORD_SECTORS;

    SlzApplyUsa(Buf);
    st = SlzTxnStage(T, lba, Buf, SLZ_RECORD_SIZE);
    SlzRestoreUsa(Buf);
    return st;
}

static ULONG SlzFindAttrRaw(PUCHAR Rec, ULONG Type)
{
    ULONG off = U16(Rec, 20);
    ULONG used = U32(Rec, 24);
    ULONG guard = 0;
    ULONG len;

    if (off < 48 || off + 16 > SLZ_RECORD_SIZE) return 0;
    if (used > SLZ_RECORD_SIZE) used = SLZ_RECORD_SIZE;

    while (guard++ < 32 && off + 16 <= used) {
        ULONG t = U32(Rec, off);
        if (t == SLZ_ATTR_END) return 0;
        len = U32(Rec, off + 4);
        if (len < 16 || off + len > used) return 0;
        if (t == Type) return off;
        off += len;
    }
    return 0;
}

static void SlzRecInitRaw(PUCHAR Rec, ULONG Index, USHORT Flags, ULONG64 Parent)
{
    RtlZeroMemory(Rec, SLZ_RECORD_SIZE);
    W32(Rec, 0, SLZ_FILE_MAGIC);
    W16(Rec, 4, 48);
    W16(Rec, 6, 9);
    W16(Rec, 16, 1);
    W16(Rec, 18, 1);
    W16(Rec, 20, 72);
    W16(Rec, 22, Flags);
    W32(Rec, 24, 72);
    W32(Rec, 28, SLZ_RECORD_SIZE);
    W64(Rec, 32, Parent);
    W16(Rec, 40, 1);
    W32(Rec, 44, Index);
    W16(Rec, 48, SLZ_USN);
}

static NTSTATUS SlzAddResidentRaw(PUCHAR Rec, ULONG Type, ULONG ValueLen,
                                  ULONG *OutOff)
{
    ULONG used = U32(Rec, 24);
    ULONG off = used;
    ULONG total = 24 + ValueLen;

    if (used + total > SLZ_RECORD_SIZE) return STATUS_DISK_FULL;
    W32(Rec, off, Type);
    W32(Rec, off + 4, total);
    Rec[off + 8] = 0;
    Rec[off + 9] = 0;
    W16(Rec, off + 10, 0);
    W16(Rec, off + 12, 0);
    W16(Rec, off + 14, U16(Rec, 40));
    W16(Rec, 40, U16(Rec, 40) + 1);
    W32(Rec, off + 16, ValueLen);
    W16(Rec, off + 20, 24);
    W32(Rec, 24, used + total);
    if (OutOff) *OutOff = off;
    return STATUS_SUCCESS;
}

static NTSTATUS SlzBuildRecord(PUCHAR Rec, ULONG Index, USHORT Flags,
                               ULONG64 Parent, const UCHAR *Name, USHORT NameLen,
                               ULONG64 IndexBlockAbs, BOOLEAN IsDir)
{
    ULONG stdOff, nameOff, dataOff;
    NTSTATUS st;
    ULONG32 now;

    SlzRecInitRaw(Rec, Index, Flags, Parent);
    now = SlzNowUnix();

    st = SlzAddResidentRaw(Rec, SLZ_ATTR_STD_INFO, 48, &stdOff);
    if (!NT_SUCCESS(st)) return st;
    W32(Rec, stdOff + 24, now);       /* created  */
    W32(Rec, stdOff + 32, now);       /* modified */
    W32(Rec, stdOff + 40, 0);         /* accessed */
    W32(Rec, stdOff + 48, 420);       /* mode     */
    W32(Rec, stdOff + 52, 0);
    W32(Rec, stdOff + 56, 0);
    W32(Rec, stdOff + 60, 0);

    st = SlzAddResidentRaw(Rec, SLZ_ATTR_FILE_NAME, 24 + NameLen, &nameOff);
    if (!NT_SUCCESS(st)) return st;
    W64(Rec, nameOff + 24, Parent);
    W16(Rec, nameOff + 32, NameLen);
    W16(Rec, nameOff + 34, 0);
    W32(Rec, nameOff + 36, 0);
    W32(Rec, nameOff + 40, 0);
    W16(Rec, nameOff + 44, 420);
    Rec[nameOff + 46] = 0;
    Rec[nameOff + 47] = 0;
    RtlCopyMemory(Rec + nameOff + 48, Name, NameLen);

    if (IsDir) {
        st = SlzAddResidentRaw(Rec, SLZ_ATTR_INDEX_ROOT, 16, &dataOff);
        if (!NT_SUCCESS(st)) return st;
        W64(Rec, dataOff + 24, IndexBlockAbs);
        W32(Rec, dataOff + 32, 1);
        W32(Rec, dataOff + 36, 0);
    } else {
        ULONG used = U32(Rec, 24);
        if (used + 64 > SLZ_RECORD_SIZE) return STATUS_DISK_FULL;
        W32(Rec, used, SLZ_ATTR_DATA);
        W32(Rec, used + 4, 64);
        Rec[used + 8] = 1;            /* 非常驻 */
        Rec[used + 9] = 0;
        W16(Rec, used + 10, 0);
        W16(Rec, used + 12, 0);
        W16(Rec, used + 14, U16(Rec, 40));
        W16(Rec, 40, U16(Rec, 40) + 1);
        W64(Rec, used + 16, 0);
        W64(Rec, used + 24, 0);
        W32(Rec, used + 32, 0);
        W32(Rec, used + 36, 0);
        W64(Rec, used + 40, 0);
        W64(Rec, used + 48, 0);
        W64(Rec, used + 56, 0);
        W32(Rec, 24, used + 64);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SlzAllocRecord(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub,
                               ULONG *IndexOut)
{
    UCHAR hdr[SLZ_SECTOR_SIZE];
    UCHAR rec[SLZ_SECTOR_SIZE];
    ULONG next, i, scanned;
    NTSTATUS st;
    ULONG64 baseLba;

    st = SlzReadSectors(Vcb->Target, Sub->BaseLba * SLZ_SECTOR_SIZE, hdr,
                        SLZ_SECTOR_SIZE);
    if (!NT_SUCCESS(st)) return st;

    next = U32(hdr, 36);
    if (next < 16 || next >= Sub->MftRecords) next = 16;

    i = next;
    scanned = 0;
    while (scanned < Sub->MftRecords) {
        USHORT flags;
        ULONG64 lba;

        if (i < 16) { i = 16; scanned++; continue; }
        if (i >= Sub->MftRecords) { i = 16; scanned++; continue; }

        lba = Sub->MftLba + (ULONG64)i * SLZ_RECORD_SECTORS;
        st = SlzReadSectors(Vcb->Target, lba * SLZ_SECTOR_SIZE, rec,
                            SLZ_SECTOR_SIZE);
        if (!NT_SUCCESS(st)) return st;
        flags = (U32(rec, 0) == SLZ_FILE_MAGIC) ? U16(rec, 22) : 0;

        if ((flags & 1) == 0) {
            W32(hdr, 36, i + 1);
            W32(hdr, 152, SlzCrc32(hdr, 152));
            st = SlzTxnStage(T, Sub->BaseLba, hdr, SLZ_SECTOR_SIZE);
            if (!NT_SUCCESS(st)) return st;
            *IndexOut = i;
            return STATUS_SUCCESS;
        }
        i++;
        scanned++;
    }
    (void)baseLba;
    return STATUS_DISK_FULL;
}

/* ============================================================
 * 文件数据段
 * ============================================================ */

static NTSTATUS SlzDataAppend(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub,
                              PUCHAR Rec, ULONG Attr, ULONG32 Clusters)
{
    ULONG32 cnt = U32(Rec, Attr + 32);
    ULONG64 cl;
    ULONG64 absLba;
    NTSTATUS st;

    if (Clusters == 0) return STATUS_SUCCESS;
    if (cnt >= SLZ_MAX_EXTENTS) return STATUS_FILE_TOO_LARGE;
    if (U32(Rec, 24) + 16 > SLZ_RECORD_SIZE) return STATUS_FILE_TOO_LARGE;

    st = SlzAllocRun(T, Sub, Clusters, &cl);
    if (!NT_SUCCESS(st)) return st;
    absLba = Vcb->ContainerStart + Sub->BaseLba + cl * SLZ_CLUSTER_SECTORS;

    if (cnt > 0) {
        ULONG64 last = Attr + 64 + (ULONG64)(cnt - 1) * 16;
        ULONG64 lastEnd = U64(Rec, (ULONG)last) + (ULONG64)U32(Rec, (ULONG)last + 8)
                          * SLZ_CLUSTER_SECTORS;
        if (lastEnd == absLba) {
            W32(Rec, (ULONG)last + 8, U32(Rec, (ULONG)last + 8) + Clusters);
            W64(Rec, Attr + 40, U64(Rec, Attr + 40) +
                (ULONG64)Clusters * SLZ_CLUSTER_SIZE);
            return STATUS_SUCCESS;
        }
    }

    W64(Rec, Attr + 64 + (ULONG64)cnt * 16, absLba);
    W32(Rec, Attr + 72 + (ULONG64)cnt * 16, Clusters);
    W32(Rec, Attr + 32, cnt + 1);
    W32(Rec, Attr + 4, 64 + (cnt + 1) * 16);
    W32(Rec, 24, U32(Rec, 24) + 16);
    W64(Rec, Attr + 40, U64(Rec, Attr + 40) +
        (ULONG64)Clusters * SLZ_CLUSTER_SIZE);
    return STATUS_SUCCESS;
}

static NTSTATUS SlzMapRaw(PUCHAR Rec, ULONG Attr, ULONG64 Pos,
                          ULONG64 *AbsLba, ULONG64 *InExtent,
                          ULONG64 *ExtentBytes)
{
    ULONG32 cnt = U32(Rec, Attr + 32);
    ULONG32 e;
    ULONG64 base = 0;

    if (cnt > SLZ_MAX_EXTENTS) return STATUS_FILE_CORRUPT_ERROR;
    for (e = 0; e < cnt; e++) {
        ULONG64 bytes = (ULONG64)U32(Rec, Attr + 72 + (ULONG64)e * 16) *
                        SLZ_CLUSTER_SIZE;
        if (Pos >= base && Pos < base + bytes) {
            *AbsLba = U64(Rec, Attr + 64 + (ULONG64)e * 16);
            *InExtent = Pos - base;
            *ExtentBytes = bytes;
            return STATUS_SUCCESS;
        }
        base += bytes;
    }
    return STATUS_END_OF_FILE;
}

static NTSTATUS SlzFreeData(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub,
                            PUCHAR Rec, ULONG Attr)
{
    ULONG32 cnt = U32(Rec, Attr + 32);
    ULONG32 i;
    NTSTATUS st;

    if (cnt > SLZ_MAX_EXTENTS) return STATUS_FILE_CORRUPT_ERROR;
    for (i = 0; i < cnt; i++) {
        ULONG64 absLba = U64(Rec, Attr + 64 + (ULONG64)i * 16);
        ULONG32 clusters = U32(Rec, Attr + 72 + (ULONG64)i * 16);
        ULONG64 rel;
        ULONG64 cluster;

        if (absLba < Vcb->ContainerStart) continue;
        rel = absLba - Vcb->ContainerStart;
        if (rel < Sub->BaseLba) continue;
        cluster = (rel - Sub->BaseLba) / SLZ_CLUSTER_SECTORS;
        st = SlzFreeRun(T, Sub, cluster, clusters);
        if (!NT_SUCCESS(st)) return st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SlzWriteBytes(SLZ_VCB *Vcb, ULONG64 OffsetBytes, PVOID Source,
                              ULONG Length)
{
    PUCHAR src = (PUCHAR)Source;
    PUCHAR bounce;
    NTSTATUS st = STATUS_SUCCESS;

    if (Length == 0) return STATUS_SUCCESS;
    bounce = ExAllocatePoolWithTag(NonPagedPool, 64 * 1024, SLZ_TAG);
    if (!bounce) return STATUS_INSUFFICIENT_RESOURCES;

    while (Length > 0) {
        ULONG secOff = (ULONG)(OffsetBytes & (SLZ_SECTOR_SIZE - 1));
        if (secOff == 0 && Length >= SLZ_SECTOR_SIZE) {
            ULONG chunk = Length & ~(SLZ_SECTOR_SIZE - 1);
            if (chunk > 64 * 1024) chunk = 64 * 1024;
            st = SlzWriteSectors(Vcb->Target, OffsetBytes, src, chunk);
            if (!NT_SUCCESS(st)) break;
            src += chunk;
            OffsetBytes += chunk;
            Length -= chunk;
        } else {
            ULONG take;
            st = SlzReadSectors(Vcb->Target,
                                OffsetBytes - secOff, bounce, SLZ_SECTOR_SIZE);
            if (!NT_SUCCESS(st)) break;
            take = SLZ_SECTOR_SIZE - secOff;
            if (take > Length) take = Length;
            RtlCopyMemory(bounce + secOff, src, take);
            st = SlzWriteSectors(Vcb->Target, OffsetBytes - secOff, bounce,
                                 SLZ_SECTOR_SIZE);
            if (!NT_SUCCESS(st)) break;
            src += take;
            OffsetBytes += take;
            Length -= take;
        }
    }
    ExFreePoolWithTag(bounce, SLZ_TAG);
    return st;
}

static NTSTATUS SlzZeroBytes(SLZ_VCB *Vcb, ULONG64 OffsetBytes, ULONG64 Length)
{
    PUCHAR zeros;
    NTSTATUS st = STATUS_SUCCESS;

    if (Length == 0) return STATUS_SUCCESS;
    zeros = ExAllocatePoolWithTag(NonPagedPool, 64 * 1024, SLZ_TAG);
    if (!zeros) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(zeros, 64 * 1024);

    while (Length > 0) {
        ULONG chunk = 64 * 1024;
        if ((ULONG64)chunk > Length) chunk = (ULONG)Length;
        st = SlzWriteBytes(Vcb, OffsetBytes, zeros, chunk);
        if (!NT_SUCCESS(st)) break;
        OffsetBytes += chunk;
        Length -= chunk;
    }
    ExFreePoolWithTag(zeros, SLZ_TAG);
    return st;
}

/* ============================================================
 * 目录索引块
 * ============================================================ */

static ULONG SlzEntryLen(USHORT NameLen)
{
    return (16 + NameLen + 7) & ~7u;
}

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

static void SlzInitIndxBlock(PUCHAR Block)
{
    RtlZeroMemory(Block, SLZ_RECORD_SIZE);
    W32(Block, 0, SLZ_INDX_MAGIC);
    W16(Block, 4, 40);
    W16(Block, 6, 9);
    W64(Block, 16, 0);
    W32(Block, 24, 0);
    W32(Block, 28, 64);
    W16(Block, 40, SLZ_USN);
}

static NTSTATUS SlzBlockInsert(PUCHAR Block, ULONG Ref, USHORT Flags,
                               const UCHAR *Name, USHORT NameLen)
{
    ULONG used = U32(Block, 28);
    ULONG cnt = U32(Block, 24);
    ULONG need = SlzEntryLen(NameLen);
    ULONG off;

    if (used + need > SLZ_RECORD_SIZE) return STATUS_BUFFER_TOO_SMALL;
    off = used;
    W32(Block, off, Ref);
    W16(Block, off + 4, Flags);
    W16(Block, off + 6, NameLen);
    W16(Block, off + 8, need);
    RtlZeroMemory(Block + off + 16, need - 16);
    RtlCopyMemory(Block + off + 16, Name, NameLen);
    W32(Block, 24, cnt + 1);
    W32(Block, 28, used + need);
    return STATUS_SUCCESS;
}

static NTSTATUS SlzDirInsert(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub,
                             ULONG DirIndex, const UCHAR *Name, USHORT NameLen,
                             ULONG Ref, USHORT Flags)
{
    UCHAR *rec;
    UCHAR *block;
    ULONG attr;
    ULONG64 rel;
    ULONG guard = 0;
    NTSTATUS st;
    ULONG need = SlzEntryLen(NameLen);

    if (need > SLZ_RECORD_SIZE) return STATUS_INVALID_PARAMETER;
    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    block = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec || !block) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    st = SlzReadRawRecord(Vcb, Sub, DirIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    attr = SlzFindAttrRaw(rec, SLZ_ATTR_INDEX_ROOT);
    if (!attr) { st = STATUS_NOT_A_DIRECTORY; goto done; }

    rel = U64(rec, attr + 24);
    if (rel == 0) {
        ULONG64 cl, absLba;
        st = SlzAllocRun(T, Sub, 1, &cl);
        if (!NT_SUCCESS(st)) goto done;
        rel = Sub->BaseLba + cl * SLZ_CLUSTER_SECTORS;
        absLba = Vcb->ContainerStart + rel;
        SlzInitIndxBlock(block);
        st = SlzTxnStage(T, rel, block, SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) goto done;
        W64(rec, attr + 24, absLba);
        W32(rec, attr + 32, 1);
        st = SlzStageRawRecord(T, DirIndex, rec);
        if (!NT_SUCCESS(st)) goto done;
    } else {
        if (rel < Vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
        rel -= Vcb->ContainerStart;
    }

    for (;;) {
        ULONG64 next;
        if (guard++ > (ULONG)U32(rec, attr + 32) + 4) {
            st = STATUS_FILE_CORRUPT_ERROR;
            goto done;
        }
        if (rel + SLZ_CLUSTER_SECTORS >
            Sub->BaseLba + Sub->Sectors) {
            st = STATUS_FILE_CORRUPT_ERROR;
            goto done;
        }
        st = SlzReadSectors(Vcb->Target, rel * SLZ_SECTOR_SIZE, block,
                            SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) goto done;
        if (U32(block, 0) != SLZ_INDX_MAGIC) {
            st = STATUS_FILE_CORRUPT_ERROR;
            goto done;
        }
        if (U32(block, 28) + need <= SLZ_RECORD_SIZE) {
            st = SlzBlockInsert(block, Ref, Flags, Name, NameLen);
            if (!NT_SUCCESS(st)) goto done;
            st = SlzTxnStage(T, rel, block, SLZ_RECORD_SIZE);
            goto done;
        }
        next = U64(block, 16);
        if (next == 0) break;
        if (next < Vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
        rel = next - Vcb->ContainerStart;
    }

    {
        ULONG64 cl, newRel, newAbs;
        ULONG blocks;

        st = SlzAllocRun(T, Sub, 1, &cl);
        if (!NT_SUCCESS(st)) goto done;
        newRel = Sub->BaseLba + cl * SLZ_CLUSTER_SECTORS;
        newAbs = Vcb->ContainerStart + newRel;
        SlzInitIndxBlock(block);
        st = SlzBlockInsert(block, Ref, Flags, Name, NameLen);
        if (!NT_SUCCESS(st)) goto done;
        st = SlzTxnStage(T, newRel, block, SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) goto done;

        /* 旧尾块链接到新块 */
        st = SlzReadSectors(Vcb->Target, rel * SLZ_SECTOR_SIZE, block,
                            SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) goto done;
        W64(block, 16, newAbs);
        st = SlzTxnStage(T, rel, block, SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) goto done;

        blocks = U32(rec, attr + 32);
        W32(rec, attr + 32, blocks + 1);
        st = SlzStageRawRecord(T, DirIndex, rec);
    }

done:
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    if (block) ExFreePoolWithTag(block, SLZ_TAG);
    return st;
}

static NTSTATUS SlzDirRemove(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub,
                             ULONG DirIndex, const UCHAR *Name, USHORT NameLen)
{
    UCHAR *block;
    SLZ_REC dir;
    ULONG64 rel;
    ULONG guard = 0;
    NTSTATUS st;

    block = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!block) return STATUS_INSUFFICIENT_RESOURCES;

    st = SlzReadRecord(Vcb, Sub, DirIndex, &dir);
    if (!NT_SUCCESS(st)) goto done;
    if (!(dir.Flags & 2) || dir.IndexLba == 0) { st = STATUS_NOT_FOUND; goto done; }
    if (dir.IndexLba < Vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

    rel = dir.IndexLba - Vcb->ContainerStart;
    for (;;) {
        ULONG cnt, used, off, i;
        ULONG64 next;

        if (guard++ > dir.IndexBlocks + 4) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
        if (rel + SLZ_CLUSTER_SECTORS > Sub->BaseLba + Sub->Sectors) {
            st = STATUS_FILE_CORRUPT_ERROR;
            goto done;
        }
        st = SlzReadSectors(Vcb->Target, rel * SLZ_SECTOR_SIZE, block,
                            SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) goto done;
        if (U32(block, 0) != SLZ_INDX_MAGIC) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

        cnt = U32(block, 24);
        used = U32(block, 28);
        if (used > SLZ_RECORD_SIZE) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

        off = 64;
        for (i = 0; i < cnt; i++) {
            USHORT nl, el;
            if (off + 16 > used) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
            nl = U16(block, off + 6);
            el = U16(block, off + 8);
            if (el < 16 || off + el > used) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
            if (nl == NameLen &&
                SlzNameEqual(block + off + 16, nl, Name, NameLen)) {
                RtlMoveMemory(block + off, block + off + el, used - off - el);
                W32(block, 24, cnt - 1);
                W32(block, 28, used - el);
                st = SlzTxnStage(T, rel, block, SLZ_RECORD_SIZE);
                goto done;
            }
            off += el;
        }
        next = U64(block, 16);
        if (next == 0) { st = STATUS_NOT_FOUND; goto done; }
        if (next < Vcb->ContainerStart) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
        rel = next - Vcb->ContainerStart;
    }

done:
    ExFreePoolWithTag(block, SLZ_TAG);
    return st;
}

static NTSTATUS SlzFreeIndexBlocks(SLZ_TXN *T, SLZ_VCB *Vcb, SLZ_SUBVOL *Sub,
                                   const SLZ_REC *Dir)
{
    ULONG64 rel;
    ULONG guard = 0;
    NTSTATUS st = STATUS_SUCCESS;
    UCHAR *block;

    if (Dir->IndexLba == 0) return STATUS_SUCCESS;
    if (Dir->IndexLba < Vcb->ContainerStart) return STATUS_FILE_CORRUPT_ERROR;
    rel = Dir->IndexLba - Vcb->ContainerStart;
    block = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!block) return STATUS_INSUFFICIENT_RESOURCES;

    while (rel != 0 && guard++ < 65536) {
        ULONG64 next;
        ULONG64 cluster;

        if (rel < Sub->BaseLba ||
            rel + SLZ_CLUSTER_SECTORS > Sub->BaseLba + Sub->Sectors) {
            st = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        st = SlzReadSectors(Vcb->Target, rel * SLZ_SECTOR_SIZE, block,
                            SLZ_RECORD_SIZE);
        if (!NT_SUCCESS(st)) break;
        if (U32(block, 0) != SLZ_INDX_MAGIC) {
            st = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        next = U64(block, 16);
        cluster = (rel - Sub->BaseLba) / SLZ_CLUSTER_SECTORS;
        st = SlzFreeRun(T, Sub, cluster, 1);
        if (!NT_SUCCESS(st)) break;

        if (next == 0) { st = STATUS_SUCCESS; break; }
        if (next < Vcb->ContainerStart) {
            st = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        rel = next - Vcb->ContainerStart;
    }
    ExFreePoolWithTag(block, SLZ_TAG);
    return st;
}

/* ============================================================
 * 高层操作
 * ============================================================ */

VOID SlzUpdateFcbFromRec(SLZ_FCB *Fcb, const SLZ_REC *Rec)
{
    ULONG i;

    Fcb->Size = Rec->Size;
    Fcb->Allocation = Rec->Allocation;
    Fcb->ExtCount = Rec->ExtCount;
    for (i = 0; i < Rec->ExtCount && i < SLZ_MAX_EXTENTS; i++) {
        Fcb->ExtLba[i] = Rec->ExtLba[i];
        Fcb->ExtClusters[i] = Rec->ExtClusters[i];
    }
    Fcb->CreationTime = SlzUnixToTime(Rec->Created);
    Fcb->LastWriteTime = SlzUnixToTime(Rec->Modified);
    Fcb->LastAccessTime = SlzUnixToTime(Rec->Accessed);
    Fcb->ChangeTime = SlzUnixToTime(Rec->Modified);
    if (Rec->Flags & 2) {
        Fcb->NodeType = SLZ_NODE_DIR;
        Fcb->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        Fcb->NodeType = SLZ_NODE_FILE;
        Fcb->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
}

NTSTATUS SlzCreateNode(SLZ_VCB *Vcb, SLZ_FCB *Parent, const UCHAR *Name,
                       USHORT NameLen, BOOLEAN IsDir, SLZ_FCB **Out)
{
    SLZ_TXN txn;
    UCHAR *rec;
    ULONG index, existing;
    NTSTATUS st;
    UCHAR path[SLZ_MAX_PATH];
    USHORT pathLen;
    SLZ_REC parentRec;

    *Out = NULL;
    if (!Parent || Parent->NodeType == SLZ_NODE_ROOT || !Parent->Sub)
        return STATUS_ACCESS_DENIED;
    if (Parent->NodeType != SLZ_NODE_DIR)
        return STATUS_NOT_A_DIRECTORY;
    if (NameLen == 0 || NameLen > SLZ_MAX_NAME)
        return STATUS_OBJECT_NAME_INVALID;

    st = SlzReadRecord(Vcb, Parent->Sub, Parent->RecordIndex, &parentRec);
    if (!NT_SUCCESS(st)) return st;
    st = SlzDirLookup(Vcb, Parent->Sub, &parentRec, Name, NameLen, &existing);
    if (NT_SUCCESS(st) && existing != 0)
        return STATUS_OBJECT_NAME_COLLISION;

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;

    SlzTxnInit(&txn, Vcb, Parent->Sub);
    st = SlzAllocRecord(&txn, Vcb, Parent->Sub, &index);
    if (!NT_SUCCESS(st)) goto done;

    if (IsDir) {
        ULONG64 cl, rel, absLba;
        UCHAR *block;

        st = SlzAllocRun(&txn, Parent->Sub, 1, &cl);
        if (!NT_SUCCESS(st)) goto done;
        rel = Parent->Sub->BaseLba + cl * SLZ_CLUSTER_SECTORS;
        absLba = Vcb->ContainerStart + rel;
        block = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
        if (!block) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
        SlzInitIndxBlock(block);
        st = SlzTxnStage(&txn, rel, block, SLZ_RECORD_SIZE);
        ExFreePoolWithTag(block, SLZ_TAG);
        if (!NT_SUCCESS(st)) goto done;

        st = SlzBuildRecord(rec, index, 3, Parent->RecordIndex, Name, NameLen,
                            absLba, TRUE);
    } else {
        st = SlzBuildRecord(rec, index, 1, Parent->RecordIndex, Name, NameLen,
                            0, FALSE);
    }
    if (!NT_SUCCESS(st)) goto done;

    st = SlzStageRawRecord(&txn, index, rec);
    if (!NT_SUCCESS(st)) goto done;

    st = SlzDirInsert(&txn, Vcb, Parent->Sub, Parent->RecordIndex, Name,
                      NameLen, index, IsDir ? 1 : 0);
    if (!NT_SUCCESS(st)) goto done;

    st = SlzTxnCommit(&txn);
    if (!NT_SUCCESS(st)) goto done;

    /* 重新读记录，构建 FCB */
    if (Parent->PathLen) {
        if (Parent->PathLen + 1 + NameLen >= SLZ_MAX_PATH) {
            st = STATUS_OBJECT_NAME_INVALID;
            goto done;
        }
        RtlCopyMemory(path, Parent->Path, Parent->PathLen);
        path[Parent->PathLen] = '\\';
        RtlCopyMemory(path + Parent->PathLen + 1, Name, NameLen);
        pathLen = (USHORT)(Parent->PathLen + 1 + NameLen);
    } else {
        RtlCopyMemory(path, Name, NameLen);
        pathLen = NameLen;
    }

    {
        SLZ_REC newRec;
        st = SlzReadRecord(Vcb, Parent->Sub, index, &newRec);
        if (!NT_SUCCESS(st)) goto done;
        *Out = SlzFcbFromRecord(Vcb, Parent->Sub, &newRec, path, pathLen);
        if (!*Out) st = STATUS_INSUFFICIENT_RESOURCES;
    }

done:
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    return st;
}

NTSTATUS SlzWriteFileData(SLZ_VCB *Vcb, SLZ_FCB *Fcb, ULONG64 Offset,
                          PVOID Buffer, ULONG Length, PULONG Written)
{
    SLZ_TXN txn;
    UCHAR *rec = NULL;
    ULONG attr;
    NTSTATUS st;
    ULONG64 need, allocated;
    ULONG64 done = 0;
    ULONG32 now;

    *Written = 0;
    if (!Vcb->WriteEnabled || !Fcb->Sub || !Fcb->Sub->LogOk)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Fcb->NodeType != SLZ_NODE_FILE) return STATUS_INVALID_DEVICE_REQUEST;
    if (Length == 0) return STATUS_SUCCESS;
    if (Offset + Length < Offset) return STATUS_INVALID_PARAMETER;

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;

    SlzTxnInit(&txn, Vcb, Fcb->Sub);
    st = SlzReadRawRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    attr = SlzFindAttrRaw(rec, SLZ_ATTR_DATA);
    if (!attr) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

    need = Offset + Length;
    allocated = U64(rec, attr + 40);
    {
        ULONG64 oldSize = U64(rec, attr + 48);
        if (need > allocated) {
            ULONG64 more = need - allocated;
            ULONG32 clusters = (ULONG32)((more + SLZ_CLUSTER_SIZE - 1) /
                                         SLZ_CLUSTER_SIZE);
            st = SlzDataAppend(&txn, Vcb, Fcb->Sub, rec, attr, clusters);
            if (!NT_SUCCESS(st)) goto done;
            allocated = U64(rec, attr + 40);
        }

        /* 稀疏区间补零 */
        if (Offset > oldSize) {
            ULONG64 gapPos = oldSize;
            while (gapPos < Offset) {
                ULONG64 absLba, inExt, extBytes, chunk;
                st = SlzMapRaw(rec, attr, gapPos, &absLba, &inExt, &extBytes);
                if (!NT_SUCCESS(st)) goto done;
                chunk = SlzMin64(Offset - gapPos, extBytes - inExt);
                if (chunk > 64 * 1024) chunk = 64 * 1024;
                st = SlzZeroBytes(Vcb,
                                  (absLba - Vcb->ContainerStart) * SLZ_SECTOR_SIZE +
                                  inExt, chunk);
                if (!NT_SUCCESS(st)) goto done;
                gapPos += chunk;
            }
        }

        while (done < Length) {
            ULONG64 absLba, inExt, extBytes, chunk;
            st = SlzMapRaw(rec, attr, Offset + done, &absLba, &inExt, &extBytes);
            if (!NT_SUCCESS(st)) goto done;
            chunk = SlzMin64((ULONG64)Length - done, extBytes - inExt);
            if (chunk > 64 * 1024) chunk = 64 * 1024;
            st = SlzWriteBytes(Vcb,
                               (absLba - Vcb->ContainerStart) * SLZ_SECTOR_SIZE +
                               inExt, (PUCHAR)Buffer + done, (ULONG)chunk);
            if (!NT_SUCCESS(st)) goto done;
            done += chunk;
        }

        if (need > oldSize) W64(rec, attr + 48, need);
    }
    now = SlzNowUnix();
    {
        ULONG std = SlzFindAttrRaw(rec, SLZ_ATTR_STD_INFO);
        if (std) {
            W32(rec, std + 32, now);
            W32(rec, std + 40, now);
        }
    }
    st = SlzStageRawRecord(&txn, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzTxnCommit(&txn);
    if (!NT_SUCCESS(st)) goto done;

    {
        SLZ_REC fresh;
        st = SlzReadRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, &fresh);
        if (NT_SUCCESS(st)) SlzUpdateFcbFromRec(Fcb, &fresh);
    }
    *Written = Length;

done:
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    return st;
}

NTSTATUS SlzSetEndOfFile(SLZ_VCB *Vcb, SLZ_FCB *Fcb, ULONG64 NewSize)
{
    SLZ_TXN txn;
    UCHAR *rec = NULL;
    ULONG attr, std;
    NTSTATUS st;
    ULONG32 now;

    if (!Vcb->WriteEnabled || !Fcb->Sub || !Fcb->Sub->LogOk)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Fcb->NodeType != SLZ_NODE_FILE) return STATUS_INVALID_DEVICE_REQUEST;

    if (NewSize == 0) return SlzTruncateNode(Vcb, Fcb);

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;

    SlzTxnInit(&txn, Vcb, Fcb->Sub);
    st = SlzReadRawRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    attr = SlzFindAttrRaw(rec, SLZ_ATTR_DATA);
    if (!attr) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

    if (NewSize > U64(rec, attr + 40)) {
        ULONG64 more = NewSize - U64(rec, attr + 40);
        ULONG32 clusters = (ULONG32)((more + SLZ_CLUSTER_SIZE - 1) /
                                     SLZ_CLUSTER_SIZE);
        st = SlzDataAppend(&txn, Vcb, Fcb->Sub, rec, attr, clusters);
        if (!NT_SUCCESS(st)) goto done;
    }

    if (NewSize > U64(rec, attr + 48)) {
        ULONG64 pos = U64(rec, attr + 48);
        while (pos < NewSize) {
            ULONG64 absLba, inExt, extBytes, chunk;
            st = SlzMapRaw(rec, attr, pos, &absLba, &inExt, &extBytes);
            if (!NT_SUCCESS(st)) goto done;
            chunk = SlzMin64(NewSize - pos, extBytes - inExt);
            if (chunk > 64 * 1024) chunk = 64 * 1024;
            st = SlzZeroBytes(Vcb,
                              (absLba - Vcb->ContainerStart) * SLZ_SECTOR_SIZE +
                              inExt, chunk);
            if (!NT_SUCCESS(st)) goto done;
            pos += chunk;
        }
    }

    W64(rec, attr + 48, NewSize);
    now = SlzNowUnix();
    std = SlzFindAttrRaw(rec, SLZ_ATTR_STD_INFO);
    if (std) { W32(rec, std + 32, now); W32(rec, std + 40, now); }
    st = SlzStageRawRecord(&txn, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzTxnCommit(&txn);
    if (!NT_SUCCESS(st)) goto done;

    Fcb->Size = NewSize;

done:
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    return st;
}

NTSTATUS SlzTruncateNode(SLZ_VCB *Vcb, SLZ_FCB *Fcb)
{
    SLZ_TXN txn;
    UCHAR *rec = NULL;
    ULONG attr;
    NTSTATUS st;

    if (!Vcb->WriteEnabled || !Fcb->Sub || !Fcb->Sub->LogOk)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Fcb->NodeType != SLZ_NODE_FILE) return STATUS_INVALID_DEVICE_REQUEST;

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;

    SlzTxnInit(&txn, Vcb, Fcb->Sub);
    st = SlzReadRawRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    attr = SlzFindAttrRaw(rec, SLZ_ATTR_DATA);
    if (!attr) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

    st = SlzFreeData(&txn, Vcb, Fcb->Sub, rec, attr);
    if (!NT_SUCCESS(st)) goto done;
    W32(rec, attr + 32, 0);
    W32(rec, attr + 4, 64);
    W64(rec, attr + 40, 0);
    W64(rec, attr + 48, 0);
    W64(rec, attr + 56, 0);
    W32(rec, 24, attr + 64);

    st = SlzStageRawRecord(&txn, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzTxnCommit(&txn);
    if (!NT_SUCCESS(st)) goto done;

    Fcb->Size = 0;
    Fcb->Allocation = 0;
    Fcb->ExtCount = 0;

done:
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    return st;
}

NTSTATUS SlzEnsureDirEmpty(SLZ_VCB *Vcb, SLZ_FCB *Fcb)
{
    SLZ_REC rec;
    SLZ_DIRENT *ents = NULL;
    ULONG count = 0;
    NTSTATUS st;

    st = SlzReadRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, &rec);
    if (!NT_SUCCESS(st)) return st;
    if (!(rec.Flags & 2)) return STATUS_NOT_A_DIRECTORY;
    st = SlzDirCollect(Vcb, Fcb->Sub, &rec, &ents, &count);
    if (!NT_SUCCESS(st)) return st;
    if (ents) ExFreePoolWithTag(ents, SLZ_TAG);
    return count ? STATUS_DIRECTORY_NOT_EMPTY : STATUS_SUCCESS;
}

NTSTATUS SlzDeleteNode(SLZ_VCB *Vcb, SLZ_FCB *Fcb)
{
    SLZ_TXN txn;
    UCHAR *rec = NULL;
    SLZ_REC parsed;
    ULONG attr, nameAttr;
    ULONG64 parent;
    UCHAR name[256];
    USHORT nameLen;
    NTSTATUS st;

    if (!Vcb->WriteEnabled || !Fcb->Sub || !Fcb->Sub->LogOk)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Fcb->NodeType == SLZ_NODE_ROOT) return STATUS_ACCESS_DENIED;

    if (Fcb->NodeType == SLZ_NODE_DIR) {
        st = SlzEnsureDirEmpty(Vcb, Fcb);
        if (!NT_SUCCESS(st)) return st;
    }

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;

    SlzTxnInit(&txn, Vcb, Fcb->Sub);
    st = SlzReadRawRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzParseRecord(rec, Fcb->RecordIndex, &parsed);
    if (!NT_SUCCESS(st)) goto done;

    nameAttr = SlzFindAttrRaw(rec, SLZ_ATTR_FILE_NAME);
    if (!nameAttr) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
    parent = U64(rec, nameAttr + 24);
    nameLen = U16(rec, nameAttr + 32);
    if (nameLen == 0 || nameLen > 255) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
    RtlCopyMemory(name, rec + nameAttr + 48, nameLen);

    attr = SlzFindAttrRaw(rec, SLZ_ATTR_DATA);
    if (attr) {
        st = SlzFreeData(&txn, Vcb, Fcb->Sub, rec, attr);
        if (!NT_SUCCESS(st)) goto done;
    }
    if (parsed.Flags & 2) {
        st = SlzFreeIndexBlocks(&txn, Vcb, Fcb->Sub, &parsed);
        if (!NT_SUCCESS(st)) goto done;
    }

    st = SlzDirRemove(&txn, Vcb, Fcb->Sub, (ULONG)parent, name, nameLen);
    if (!NT_SUCCESS(st)) goto done;

    W16(rec, 22, 0);
    W32(rec, 24, 72);
    st = SlzStageRawRecord(&txn, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzTxnCommit(&txn);

done:
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    return st;
}

static BOOLEAN SlzIsDescendant(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, ULONG64 Parent,
                               ULONG Target)
{
    ULONG guard = 0;
    SLZ_REC rec;

    while (Parent != 0 && guard++ < 4096) {
        if (Parent == Target) return TRUE;
        if (!NT_SUCCESS(SlzReadRecord(Vcb, Sub, (ULONG)Parent, &rec)))
            return TRUE;   /* 链不完整时保守拒绝 */
        if (rec.Parent == Parent) break;
        Parent = rec.Parent;
    }
    return FALSE;
}

/* 用新名字重建记录：避免改名长度变化时覆盖后面的属性 */
static NTSTATUS SlzRebuildRecord(PUCHAR OldRaw, const SLZ_REC *Old,
                                 PUCHAR NewRaw, const UCHAR *Name,
                                 USHORT NameLen, ULONG64 NewParent)
{
    BOOLEAN isDir = (Old->Flags & 2) ? TRUE : FALSE;
    ULONG oldStd, newStd, newData, i;
    NTSTATUS st;

    st = SlzBuildRecord(NewRaw, Old->Index, Old->Flags, NewParent, Name,
                        NameLen, Old->IndexLba, isDir);
    if (!NT_SUCCESS(st)) return st;

    oldStd = SlzFindAttrRaw(OldRaw, SLZ_ATTR_STD_INFO);
    newStd = SlzFindAttrRaw(NewRaw, SLZ_ATTR_STD_INFO);
    if (oldStd && newStd)
        RtlCopyMemory(NewRaw + newStd + 24, OldRaw + oldStd + 24, 48);

    if (isDir) {
        newData = SlzFindAttrRaw(NewRaw, SLZ_ATTR_INDEX_ROOT);
        if (!newData) return STATUS_FILE_CORRUPT_ERROR;
        W32(NewRaw, newData + 32, Old->IndexBlocks ? Old->IndexBlocks : 1);
    } else {
        newData = SlzFindAttrRaw(NewRaw, SLZ_ATTR_DATA);
        if (!newData) return STATUS_FILE_CORRUPT_ERROR;
        W32(NewRaw, newData + 32, Old->ExtCount);
        W32(NewRaw, newData + 4, 64 + Old->ExtCount * 16);
        W64(NewRaw, newData + 40, Old->Allocation);
        W64(NewRaw, newData + 48, Old->Size);
        for (i = 0; i < Old->ExtCount; i++) {
            W64(NewRaw, newData + 64 + (ULONG64)i * 16, Old->ExtLba[i]);
            W32(NewRaw, newData + 72 + (ULONG64)i * 16, Old->ExtClusters[i]);
        }
        W32(NewRaw, 24, newData + 64 + Old->ExtCount * 16);
    }
    return STATUS_SUCCESS;
}

NTSTATUS SlzRenameNode(SLZ_VCB *Vcb, SLZ_FCB *Fcb, SLZ_FCB *NewParent,
                       const UCHAR *NewName, USHORT NewNameLen)
{
    SLZ_TXN txn;
    UCHAR *rec = NULL;
    UCHAR *newrec = NULL;
    SLZ_REC parsed;
    ULONG nameAttr;
    ULONG64 oldParent;
    UCHAR oldName[256];
    USHORT oldLen;
    ULONG existing;
    NTSTATUS st;
    SLZ_REC newParentRec;

    if (!Vcb->WriteEnabled || !Fcb->Sub || !Fcb->Sub->LogOk)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (!NewParent || NewParent->NodeType == SLZ_NODE_ROOT ||
        !NewParent->Sub || NewParent->Sub != Fcb->Sub)
        return STATUS_NOT_SAME_DEVICE;
    if (NewParent->NodeType != SLZ_NODE_DIR) return STATUS_NOT_A_DIRECTORY;
    if (NewNameLen == 0 || NewNameLen > SLZ_MAX_NAME)
        return STATUS_OBJECT_NAME_INVALID;
    if (SlzIsDescendant(Vcb, Fcb->Sub, NewParent->RecordIndex,
                        (ULONG)Fcb->RecordIndex))
        return STATUS_INVALID_PARAMETER;

    st = SlzReadRecord(Vcb, NewParent->Sub, NewParent->RecordIndex,
                       &newParentRec);
    if (!NT_SUCCESS(st)) return st;
    st = SlzDirLookup(Vcb, Fcb->Sub, &newParentRec, NewName, NewNameLen,
                      &existing);
    if (NT_SUCCESS(st) && existing != 0) return STATUS_OBJECT_NAME_COLLISION;

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    newrec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec || !newrec) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    SlzTxnInit(&txn, Vcb, Fcb->Sub);
    st = SlzReadRawRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzParseRecord(rec, Fcb->RecordIndex, &parsed);
    if (!NT_SUCCESS(st)) goto done;
    nameAttr = SlzFindAttrRaw(rec, SLZ_ATTR_FILE_NAME);
    if (!nameAttr) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
    oldParent = U64(rec, nameAttr + 24);
    oldLen = U16(rec, nameAttr + 32);
    if (oldLen == 0 || oldLen > 255) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }
    RtlCopyMemory(oldName, rec + nameAttr + 48, oldLen);

    st = SlzDirRemove(&txn, Vcb, Fcb->Sub, (ULONG)oldParent, oldName, oldLen);
    if (!NT_SUCCESS(st)) goto done;

    st = SlzRebuildRecord(rec, &parsed, newrec, NewName, NewNameLen,
                          NewParent->RecordIndex);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzStageRawRecord(&txn, Fcb->RecordIndex, newrec);
    if (!NT_SUCCESS(st)) goto done;

    st = SlzDirInsert(&txn, Vcb, Fcb->Sub, NewParent->RecordIndex, NewName,
                      NewNameLen, Fcb->RecordIndex,
                      (Fcb->NodeType == SLZ_NODE_DIR) ? 1 : 0);
    if (!NT_SUCCESS(st)) goto done;

    st = SlzTxnCommit(&txn);
    if (!NT_SUCCESS(st)) goto done;

    Fcb->ParentRecord = NewParent->RecordIndex;
    Fcb->NameLen = NewNameLen;
    RtlZeroMemory(Fcb->Name, sizeof(Fcb->Name));
    RtlCopyMemory(Fcb->Name, NewName, NewNameLen);
    if (NewParent->PathLen) {
        if ((ULONG)NewParent->PathLen + 1 + NewNameLen >= SLZ_MAX_PATH) {
            st = STATUS_OBJECT_NAME_INVALID;
            goto done;
        }
        RtlCopyMemory(Fcb->Path, NewParent->Path, NewParent->PathLen);
        Fcb->Path[NewParent->PathLen] = '\\';
        RtlCopyMemory(Fcb->Path + NewParent->PathLen + 1, NewName, NewNameLen);
        Fcb->PathLen = (USHORT)(NewParent->PathLen + 1 + NewNameLen);
    } else {
        RtlCopyMemory(Fcb->Path, NewName, NewNameLen);
        Fcb->PathLen = NewNameLen;
    }

done:
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    if (newrec) ExFreePoolWithTag(newrec, SLZ_TAG);
    return st;
}

static ULONG32 SlzTimeToUnix(LARGE_INTEGER Time)
{
    LONGLONG v;
    if (Time.QuadPart == 0) return 0;
    v = (Time.QuadPart - 116444736000000000LL) / 10000000LL;
    if (v < 0) return 0;
    if (v > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
    return (ULONG32)v;
}

NTSTATUS SlzSetBasicInfo(SLZ_VCB *Vcb, SLZ_FCB *Fcb,
                         PFILE_BASIC_INFORMATION Basic)
{
    SLZ_TXN txn;
    UCHAR *rec = NULL;
    ULONG attr;
    NTSTATUS st;

    if (!Vcb->WriteEnabled || !Fcb->Sub || !Fcb->Sub->LogOk)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Fcb->NodeType == SLZ_NODE_ROOT) return STATUS_SUCCESS;

    rec = ExAllocatePoolWithTag(NonPagedPool, SLZ_RECORD_SIZE, SLZ_TAG);
    if (!rec) return STATUS_INSUFFICIENT_RESOURCES;

    SlzTxnInit(&txn, Vcb, Fcb->Sub);
    st = SlzReadRawRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    attr = SlzFindAttrRaw(rec, SLZ_ATTR_STD_INFO);
    if (!attr) { st = STATUS_FILE_CORRUPT_ERROR; goto done; }

    if (Basic->CreationTime.QuadPart != 0)
        W32(rec, attr + 24, SlzTimeToUnix(Basic->CreationTime));
    if (Basic->LastWriteTime.QuadPart != 0)
        W32(rec, attr + 32, SlzTimeToUnix(Basic->LastWriteTime));
    if (Basic->LastAccessTime.QuadPart != 0)
        W32(rec, attr + 40, SlzTimeToUnix(Basic->LastAccessTime));
    if (Basic->ChangeTime.QuadPart != 0)
        W32(rec, attr + 32, SlzTimeToUnix(Basic->ChangeTime));

    st = SlzStageRawRecord(&txn, Fcb->RecordIndex, rec);
    if (!NT_SUCCESS(st)) goto done;
    st = SlzTxnCommit(&txn);
    if (!NT_SUCCESS(st)) goto done;

    {
        SLZ_REC fresh;
        st = SlzReadRecord(Vcb, Fcb->Sub, Fcb->RecordIndex, &fresh);
        if (NT_SUCCESS(st)) SlzUpdateFcbFromRec(Fcb, &fresh);
    }

done:
    if (!NT_SUCCESS(st)) SlzTxnFree(&txn);
    if (rec) ExFreePoolWithTag(rec, SLZ_TAG);
    return st;
}
