/* ============================================================
 * SlyzFs.h - SaylterOS SlyzFS v3 只读 IFS 驱动
 * ------------------------------------------------------------
 * 盘上格式（与 src\fs\slyzfs2.slyz3 一致）：
 *
 * 容器（Windows 看到的 0x7F 分区，卷内相对 LBA 0）：
 *   LBA0      分区表 "SLYSPT2"，CRC32 覆盖前 508B，存于 +508
 *   条目从 +16 起，每条 64B：
 *     +0 type(1=SYSTEM 2=RECOVERY 3=DATA)  +1 flags
 *     +4 start_lba(绝对)  +8 sector_count  +12 label[24]
 *
 * 子卷头（子卷起始 LBA，512B）：
 *   +0  magic "SLYZVOL2" (0x324C4F565A594C53)
 *   +8  version u16 = 2      +10 sector_size u16 = 512
 *   +12 cluster_bytes u32 = 4096
 *   +16 total_clusters u64
 *   +24 mft_lba u64(绝对)    +32 mft_records u32  +36 mft_next u32
 *   +40 bitmap_lba u64       +48 bitmap_sectors u32
 *   +52 log_lba u64          +60 log_sectors u32
 *   +64 root_record u64 = 5
 *   +72 volume_serial u64    +80 label[64]
 *   +152 crc32(前 152B)
 *
 * MFT 记录 4096B（mft_lba + idx*8 扇区）：
 *   +0 "FILE"  +4 usa_off u16  +6 usa_count u16
 *   +16 sequence u16  +18 link_count u16
 *   +20 attrs_off u16  +22 flags u16（bit0 使用中，bit1 目录）
 *   +24 used u32  +28 alloc u32 = 4096
 *   +32 parent_record u64  +40 next_attr_id u16  +44 record_index u32
 *   属性区从 +72 起，属性头 16B：
 *     +0 type  +4 len  +8 non_resident  +14 attr_id
 *     常驻：+16 value_len  +20 value_off
 *
 *   0x10 STD_INFO（value 48B）：created +0, modified +8, access +16,
 *        mode +24, uid +28, gid +32
 *   0x30 FILE_NAME（value 24+n）：parent u64 +0, name_len +8,
 *        name_flags +10, mode +20, uid +22, gid +23, name +24
 *   0x80 DATA（非常驻 64+16n）：ext_count +32, allocated +40,
 *        file_size +48, 段表 +64 每项 {lba u64, clusters u32}
 *   0x90 INDEX_ROOT（常驻 16B）：first_block_lba u64 +0, blocks u32 +8
 *
 * 目录索引块 4096B（"INDX"，没有 USA）：
 *   +16 next_block_lba u64  +24 entry_count u32  +28 used_bytes u32
 *   条目从 +64 起：ref u32 +0, flags u16 +4, name_len u16 +6,
 *   entry_len u16 +8, name +16，条目按 8B 对齐。
 *
 * 本驱动只读，不重放日志、不写盘。
 * ============================================================ */

#ifndef SLYZFS_H
#define SLYZFS_H

#include <ntifs.h>
#include <ntddvol.h>

#define SLZ_TAG 'FsyS'

#define SLZ_SECTOR_SIZE            512u
#define SLZ_CLUSTER_SIZE           4096u
#define SLZ_CLUSTER_SECTORS        8u
#define SLZ_RECORD_SIZE            4096u
#define SLZ_RECORD_SECTORS         8u
#define SLZ_MAX_EXTENTS            8u
#define SLZ_MAX_NAME               255u
#define SLZ_MAX_PATH               1024u
#define SLZ_MAX_SUBVOLS            3u
#define SLZ_MAX_DIR_CACHE          8192u

#define SLZ_PT_MAGIC               0x0032545053594C53ull   /* "SLYSPT2\0" */
#define SLZ_VOL_MAGIC              0x324C4F565A594C53ull   /* "SLYZVOL2"  */
#define SLZ_FILE_MAGIC             0x454C4946u             /* "FILE" */
#define SLZ_INDX_MAGIC             0x58444E49u             /* "INDX" */
#define SLZ_USN                    0x4D55u

#define SLZ_ATTR_STD_INFO          0x10u
#define SLZ_ATTR_FILE_NAME         0x30u
#define SLZ_ATTR_DATA              0x80u
#define SLZ_ATTR_INDEX_ROOT        0x90u

#define SLZ_NODE_ROOT              0u
#define SLZ_NODE_FILE              1u
#define SLZ_NODE_DIR               2u

/* 伪根也是目录：Explorer 会带 FILE_DIRECTORY_FILE 打开 F:\ */
#define SLZ_IS_DIR(f) ((f)->NodeType == SLZ_NODE_DIR || (f)->NodeType == SLZ_NODE_ROOT)

#define SLZ_FCB_MAGIC              0x42434653u   /* "SFCB" */
#define SLZ_VCB_MAGIC              0x42435653u   /* "SVCB" */
#define SLZ_CCB_MAGIC              0x42434353u   /* "SCCB" */
#define SLZ_NO_RECORD              0xFFFFFFFFu

typedef struct _SLZ_SUBVOL {
    BOOLEAN  Present;
    UCHAR    Type;                     /* 1/2/3 */
    UCHAR    Label[25];                /* UTF-8，零结尾 */
    ULONG64  BaseLba;                  /* 相对容器起始 */
    ULONG64  Sectors;
    ULONG64  TotalClusters;
    ULONG64  MftLba;                   /* 相对容器起始 */
    ULONG32  MftRecords;
    ULONG64  BitmapLba;                /* 相对容器起始 */
    ULONG32  BitmapSectors;
    ULONG64  RootRecord;
    ULONG64  UsedClusters;
    ULONG32  Serial;
    /* ---- 写入支持 ---- */
    PUCHAR   Bitmap;                   /* 位图内存副本（挂载时读入） */
    ULONG    BitmapBytes;
    ULONG64  AllocHint;
    ULONG64  LogLba;                   /* 相对容器，SLOG 头所在 */
    ULONG    LogSectors;
    ULONG    LogHead;                  /* SLOG 下一个记录槽 */
    ULONG    LogRecords;
    ULONG64  LogSeq;
    BOOLEAN  LogOk;                    /* 日志可用，允许写 */
} SLZ_SUBVOL;

typedef struct _SLZ_VCB {
    ULONG         Magic;
    PDEVICE_OBJECT DeviceObject;       /* 本驱动创建的卷设备 */
    PDEVICE_OBJECT Target;             /* 下层卷设备（读扇区用） */
    ULONG64       ContainerStart;      /* 容器在物理盘上的起始扇区 */
    ULONG64       ContainerSectors;
    ULONG         SubCount;
    SLZ_SUBVOL    Sub[SLZ_MAX_SUBVOLS];
    ULONG64       TotalClusters;
    ULONG64       UsedClusters;
    ULONG32       Serial;
    ERESOURCE     Lock;                /* 写事务串行化 */
    BOOLEAN       WriteEnabled;
    BOOLEAN       Reserved2[3];
    struct _SLZ_FCB *RootFcb;
    LONG          OpenCount;
} SLZ_VCB;

#define SLZ_TXN_MAX 40

typedef struct _SLZ_TXN_PAGE {
    ULONG64 Lba;                       /* 相对容器 */
    ULONG32 Bytes;
    PUCHAR  Data;                      /* 4096B 缓冲 */
} SLZ_TXN_PAGE;

typedef struct _SLZ_TXN {
    SLZ_VCB     *Vcb;
    SLZ_SUBVOL  *Sub;
    ULONG        Count;
    SLZ_TXN_PAGE Page[SLZ_TXN_MAX];
} SLZ_TXN;

typedef struct _SLZ_REC {
    ULONG    Index;
    USHORT   Flags;                    /* bit0 使用中，bit1 目录 */
    ULONG64  Parent;
    ULONG32  Created;
    ULONG32  Modified;
    ULONG32  Accessed;
    ULONG64  Size;
    ULONG64  Allocation;
    ULONG32  ExtCount;
    ULONG64  ExtLba[SLZ_MAX_EXTENTS];
    ULONG32  ExtClusters[SLZ_MAX_EXTENTS];
    ULONG64  IndexLba;                 /* 目录索引首块，相对容器 */
    ULONG32  IndexBlocks;
    USHORT   NameLen;
    UCHAR    Name[256];
} SLZ_REC;

typedef struct _SLZ_DIRENT {
    ULONG   Ref;
    UCHAR   Flags;
    UCHAR   NameLen;
    UCHAR   Name[256];
} SLZ_DIRENT;

typedef struct _SLZ_FCB {
    ULONG    Magic;
    UCHAR    NodeType;                 /* SLZ_NODE_* */
    UCHAR    Reserved[3];
    SLZ_VCB *Vcb;
    SLZ_SUBVOL *Sub;                   /* 伪根为 NULL */
    ULONG    RecordIndex;
    ULONG64  ParentRecord;
    ULONG64  Size;
    ULONG64  Allocation;
    ULONG32  ExtCount;
    ULONG64  ExtLba[SLZ_MAX_EXTENTS];
    ULONG32  ExtClusters[SLZ_MAX_EXTENTS];
    ULONG    FileAttributes;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    USHORT   NameLen;
    UCHAR    Name[SLZ_MAX_NAME + 1];
    USHORT   PathLen;
    UCHAR    Path[SLZ_MAX_PATH];
    SECTION_OBJECT_POINTERS Section;
} SLZ_FCB;

typedef struct _SLZ_CCB {
    ULONG    Magic;
    SLZ_FCB *Fcb;
    ULONG    DirIndex;
    ULONG    DirCount;
    SLZ_DIRENT *DirCache;
    BOOLEAN  CacheValid;
    BOOLEAN  DeleteOnClose;
    BOOLEAN  Reserved[2];
} SLZ_CCB;

/* ---- 格式解析 ---- */
NTSTATUS SlzReadSectors(PDEVICE_OBJECT Target, ULONG64 OffsetBytes,
                        PVOID Buffer, ULONG Length);
NTSTATUS SlzWriteSectors(PDEVICE_OBJECT Target, ULONG64 OffsetBytes,
                         PVOID Buffer, ULONG Length);
NTSTATUS SlzFlushDevice(PDEVICE_OBJECT Target);
NTSTATUS SlzReadRecord(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, ULONG Index, SLZ_REC *Out);
NTSTATUS SlzParseRecord(PUCHAR Buffer, ULONG Index, SLZ_REC *Out);
BOOLEAN  SlzRestoreUsa(PUCHAR Buffer);
VOID     SlzApplyUsa(PUCHAR Buffer);
NTSTATUS SlzDirLookup(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, const SLZ_REC *Dir,
                      const UCHAR *Name, USHORT NameLen, ULONG *RefOut);
NTSTATUS SlzDirCollect(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, const SLZ_REC *Dir,
                       SLZ_DIRENT **Out, ULONG *CountOut);

/* ---- 写入路径（SlyzFsWrite.c） ---- */
NTSTATUS SlzLoadBitmap(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub);
NTSTATUS SlzJournalReplay(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub);
NTSTATUS SlzCreateNode(SLZ_VCB *Vcb, SLZ_FCB *Parent, const UCHAR *Name,
                       USHORT NameLen, BOOLEAN IsDir, SLZ_FCB **Out);
NTSTATUS SlzWriteFileData(SLZ_VCB *Vcb, SLZ_FCB *Fcb, ULONG64 Offset,
                          PVOID Buffer, ULONG Length, PULONG Written);
NTSTATUS SlzSetEndOfFile(SLZ_VCB *Vcb, SLZ_FCB *Fcb, ULONG64 NewSize);
NTSTATUS SlzTruncateNode(SLZ_VCB *Vcb, SLZ_FCB *Fcb);
NTSTATUS SlzDeleteNode(SLZ_VCB *Vcb, SLZ_FCB *Fcb);
NTSTATUS SlzRenameNode(SLZ_VCB *Vcb, SLZ_FCB *Fcb, SLZ_FCB *NewParent,
                       const UCHAR *NewName, USHORT NewNameLen);
NTSTATUS SlzEnsureDirEmpty(SLZ_VCB *Vcb, SLZ_FCB *Fcb);
NTSTATUS SlzSetBasicInfo(SLZ_VCB *Vcb, SLZ_FCB *Fcb,
                         PFILE_BASIC_INFORMATION Basic);
VOID     SlzUpdateFcbFromRec(SLZ_FCB *Fcb, const SLZ_REC *Rec);
SLZ_FCB *SlzFcbFromRecord(SLZ_VCB *Vcb, SLZ_SUBVOL *Sub, const SLZ_REC *Rec,
                          const UCHAR *Path, USHORT PathLen);

/* ---- 工具 ---- */
ULONG32 SlzCrc32(const VOID *Buffer, ULONG Length);
ULONG SlzUtf8ToWide(const UCHAR *Src, ULONG SrcLen, PWCHAR Dst, ULONG DstChars);
ULONG SlzWideToUtf8(const WCHAR *Src, ULONG SrcChars, UCHAR *Dst, ULONG DstBytes);
BOOLEAN SlzWildcardMatch(const WCHAR *Pattern, ULONG PatternChars,
                         const WCHAR *Name, ULONG NameChars);
LARGE_INTEGER SlzUnixToTime(ULONG Seconds);

#endif /* SLYZFS_H */
