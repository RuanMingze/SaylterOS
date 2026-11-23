/* ============================================================
 * slysfs.h - SlyzFS v2 模块 ABI（.slys 裸机文件系统）
 * ------------------------------------------------------------
 * 这里只有"内核如何调用模块"的 ABI 定义，没有任何文件系统实现。
 * SlyzFS v2 本体在 src/fs/slyzfs2.slyz3，用 Slyz v3 编写，由
 * slyz3c --baremetal 编译成 .slys 模块；内核只负责加载与转发。
 *
 * 模块入口遵循 Microsoft x64 ABI：
 *     void *slyzfs_start(SlysFsApi *api);
 * 返回值是 SlysFsCall 函数指针（5 个 64 位参数）。
 *
 * v2 磁盘布局（NTFS 风格，非 FAT）：
 *   8192  分区表主副本 "SLYSPT2"
 *   8193  分区表备份
 *   8194  严重故障计数 "SLYZERR2"
 *   8196  第一个分区（SYSTEM / C:）
 *   之后  RECOVERY / R:、DATA / D:
 *
 * 每个卷：
 *   簇 0        卷头 "SLYZVOL2"（512B）
 *   簇 1..      簇位图（bitmap_sectors）
 *   之后        环形日志（4MB）
 *   之后        MFT（4KB 记录，USA 防撕裂写）
 *   之后        数据区
 *
 * 目录使用 4KB 有序索引块链；单个文件名最长 255 字节 UTF-8，
 * 路径总长 1024 字节。
 * ============================================================ */

#ifndef SLYSFS_H
#define SLYSFS_H

#include <stdint.h>

#define SLYSFS_ABI 2

#define SLYZ_NAME_MAX_BYTES 255
#define SLYZ_PATH_MAX_BYTES 1024

/* 内核与模块交换的目录条目（变长名字，固定 280 字节）。
 * 内核现有的 SlyzDirEntry 必须按这个布局更新。 */
#pragma pack(push, 1)
struct SlysDirEntry {
    uint8_t  type;            /* 1=文件 2=目录 */
    uint8_t  flags;           /* bit0 受保护 */
    uint16_t name_len;        /* UTF-8 字节数，<=255 */
    char     name[256];       /* 零结尾；name_len 之外的字节为 0 */
    uint32_t size;            /* 文件字节数 */
    uint32_t start_lba;       /* 数据首段 LBA（0 = 无数据） */
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)

#define SLYS_DIRENTRY_SIZE 280

/* 内核提供给模块的服务。偏移固定为 0/8/16/24/32/40。 */
struct SlysFsApi {
    uint32_t version;                          /* +0  */
    uint32_t reserved;                         /* +4  */
    int (*disk_read)(uint32_t lba, void *buf); /* +8  */
    int (*disk_write)(uint32_t lba, const void *buf); /* +16 */
    uint32_t (*total_sectors)(void);           /* +24 */
    void (*log)(const char *text);             /* +32 */
    int (*flush)(void);                        /* +40 */
};

/* 操作码：a/b/c/d 四个 64 位参数，返回值 64 位。
 * 字符串一律是内核地址空间里的零结尾指针（模块与内核同一地址空间）。 */
enum {
    SFS_OP_FORMAT_DISK = 1,   /* -> 1/0：写 SPT、故障扇区并格式化 C/R/D */
    SFS_OP_MOUNT_ALL,         /* -> 位图：bit0 C:，bit1 R:，bit2 D: */
    SFS_OP_FORMAT_VOLUME,     /* a=drive, b=base, c=sectors, d=label -> 1/0 */
    SFS_OP_MOUNT_VOLUME,      /* a=drive, b=base, c=sectors -> 1/0 */
    SFS_OP_UNMOUNT_VOLUME,    /* a=drive -> 1/0 */
    SFS_OP_FLUSH,             /* 提交日志并刷盘 -> 1/0 */

    SFS_OP_LIST,              /* a=drive, b=path, c=out, d=max -> count / -1 */
    SFS_OP_STAT,              /* a=drive, b=path, c=out(280B) -> 1/0 */
    SFS_OP_MKDIR,             /* a=drive, b=path -> 1/0 */
    SFS_OP_RMDIR,
    SFS_OP_REMOVE,
    SFS_OP_CREATE,
    SFS_OP_USAGE,             /* a=drive, b=out[3] u32 -> 1/0 */

    SFS_OP_OPEN,              /* a=drive, b=path, c=mode(0=r,1=w,2=a) -> 句柄 */
    SFS_OP_READ,              /* a=handle, b=buf, c=len -> 实际字节数 */
    SFS_OP_WRITE,             /* a=handle, b=buf, c=len -> 实际字节数 */
    SFS_OP_SEEK,              /* a=handle, b=offset -> 1/0 */
    SFS_OP_CLOSE,             /* a=handle -> 1/0 */

    SFS_OP_CHMOD,             /* a=drive, b=path, c=mode -> 1/0 */
    SFS_OP_CHOWN,             /* a=drive, b=path, c=uid -> 1/0 */
    SFS_OP_PROTECT,           /* a=drive, b=path, c=on -> 1/0 */
    SFS_OP_IS_PROTECTED,      /* a=drive, b=path -> 1/0 */
    SFS_OP_COUNT_VOLUMES,     /* -> 实际挂载的卷数 */
    SFS_OP_VOLUME_INFO,       /* a=drive, b=out[4] u32（base,sectors,clusters,flags） -> 1/0 */
    SFS_OP_FAULT_READ,        /* b=out SlysFaultRecord -> 1/0 */
    SFS_OP_FAULT_WRITE,       /* b=in SlysFaultRecord -> 1/0 */
    SFS_OP_RENAME             /* a=drive, b=old, c=new -> 1/0（同卷改名/移动） */
};

typedef int64_t (*SlysFsCall)(int64_t op, int64_t a, int64_t b,
                              int64_t c, int64_t d);

/* 分区表（主/备副本） */
#pragma pack(push, 1)
struct SlysPartEntry {
    uint8_t  type;            /* 0=空 1=SYSTEM 2=RECOVERY 3=DATA */
    uint8_t  flags;           /* bit0 可引导，bit1 正常系统只读 */
    uint16_t reserved0;
    uint32_t start_lba;
    uint32_t sector_count;
    char     label[24];
    uint32_t serial;
    uint32_t reserved1[6];
};

struct SlysPartTable {
    char     magic[8];        /* "SLYSPT2\0" */
    uint16_t version;
    uint8_t  entry_count;
    uint8_t  reserved0;
    uint32_t disk_serial;
    struct SlysPartEntry entries[4];
    uint8_t  reserved1[236];
    uint32_t crc32;
};

struct SlysFaultRecord {
    char     magic[8];        /* "SLYZERR2" */
    uint16_t serious_count;
    uint16_t clean_boots;
    uint32_t last_code;
    uint32_t last_tick;
    uint8_t  reserved[40];
    uint32_t crc32;
};
#pragma pack(pop)

#define SLYSPT_LBA_PRIMARY 8192u
#define SLYSPT_LBA_BACKUP  8193u
#define SLYZERR_LBA        8194u
#define SLYSPT_FIRST_PART  8196u

#endif /* SLYSFS_H */
