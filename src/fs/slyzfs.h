#ifndef SLYZ_FS_H
#define SLYZ_FS_H

/*
 * SlyzFS —— SaylterOS 原生文件系统 v0.1
 *
 * 设计目标：
 *   - 教学级、零依赖、可从零理解
 *   - 路径分隔符 '\'（Windows 风格，如 "slyz\sys\boot.slyz"）
 *   - FAT-like 链式分配，支持子目录
 *   - 基于现有 ATA PIO 扇区读写（512 字节 LBA）
 *
 * 磁盘布局（默认从 LBA 2048 开始，不影响 MBR/引导）：
 *   Sector 2048        : SuperBlock   —— 格式版本/簇大小/数据区起始
 *   Sector 2049 ~ 3072 : FAT          —— 1024 sectors，16-bit 条目，共 524,288 簇
 *   Sector 3073 ~ 3328 : Root Dir     —— 256 sectors，每条目 64B，共 2048 条目
 *   Sector 3329+       : Data Area    —— 簇 2 起，每簇 4 sectors (2048B)
 */

#include <stdint.h>
#include <stddef.h>

namespace slyzfs {

static inline void slyz_memset(void* dst, int value, size_t count) {
    unsigned char* p = static_cast<unsigned char*>(dst);
    while (count--) *p++ = static_cast<unsigned char>(value);
}

static inline void slyz_memcpy(void* dst, const void* src, size_t count) {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    while (count--) *d++ = *s++;
}

static inline int slyz_memcmp(const void* a, const void* b, size_t count) {
    const unsigned char* pa = static_cast<const unsigned char*>(a);
    const unsigned char* pb = static_cast<const unsigned char*>(b);
    while (count--) {
        if (*pa != *pb) return (*pa > *pb) ? 1 : -1;
        ++pa; ++pb;
    }
    return 0;
}

static inline size_t slyz_strlen(const char* s) {
    size_t n = 0;
    while (s && s[n] != '\0') ++n;
    return n;
}

static inline char* slyz_strncpy(char* dst, const char* src, size_t count) {
    char* out = dst;
    size_t i = 0;
    while (i + 1 < count && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    if (count > 0) dst[i] = '\0';
    return out;
}

static inline int slyz_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return (unsigned char)(*a) - (unsigned char)(*b);
}

/* ======================= 常量 ======================= */

static constexpr uint32_t SLYZ_FS_OFFSET       = 2048;   /* 文件系统起始 LBA */
static constexpr uint16_t SLYZ_MAGIC           = 0x534C; /* "SL" 魔数 */
static constexpr uint16_t SLYZ_VERSION         = 0x0002; /* v0.2：目录条目改回真实尺寸 71B */
static constexpr uint16_t SLYZ_CLUSTER_SIZE    = 4;      /* 每簇 4 sectors = 2048B */
static constexpr uint16_t SLYZ_FAT_SECTORS     = 64;     /* FAT 占用 sectors 数：64*256=16384 条目，
                                                          * 覆盖 32MB 磁盘全部 15791 个簇而有余。
                                                          * 关键约束：fat_storage_ 常驻 .bss，必须让整个
                                                          * 内核 BSS 结束地址低于 0xA0000（640KB 传统内存
                                                          * 上界），否则静态变量会落进 VGA/BIOS ROM 映射区，
                                                          * 写入被硬件忽略、读回全是 ROM 垃圾数据。 */
static constexpr uint16_t SLYZ_ROOT_SECTORS    = 256;    /* 根目录占用 sectors 数 */
/* 目录条目真实字节数。DirEntry 在 pack(1) 下是 71 字节
 * （1 + 52 + 2 + 4 + 4 + 4 + 4），过去这里写成 64 是错的：
 * 会让 (DirEntry*)(sector + e*64) 逐项错位，并把第 8 项写到扇区外。 */
static constexpr uint16_t SLYZ_DIRENTRY_SIZE   = 71;
static constexpr uint16_t SLYZ_SECTOR_SIZE     = 512;
/* 单个簇的字节数 —— 所有簇级读写缓冲区必须用这个常量定尺寸。
 * 栈上缓冲区过大（早期遗留的 65536 字节）会把内核栈从 0x9FFFF 打穿到
 * IDT/ISR 所在区域（0x8820~0x9020），任何中断到来都会导致系统挂死。 */
static constexpr uint32_t SLYZ_CLUSTER_BYTES   = (uint32_t)SLYZ_CLUSTER_SIZE * SLYZ_SECTOR_SIZE;
static constexpr uint16_t SLYZ_ENTRIES_PER_SECTOR = SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE; /* 8 */

/* FAT 特殊值 */
static constexpr uint16_t FAT_FREE     = 0x0000;
static constexpr uint16_t FAT_RESERVED = 0x0001;
static constexpr uint16_t FAT_END      = 0xFFFF;
static constexpr uint16_t FAT_BAD      = 0xFFFE;

/* 目录条目类型 */
static constexpr uint8_t  DIR_EMPTY    = 0x00; /* 从未使用 */
static constexpr uint8_t  DIR_DELETED  = 0xE5; /* 曾使用但已删除 */
static constexpr uint8_t  DIR_FILE     = 0x01;
static constexpr uint8_t  DIR_DIR      = 0x02;

/* 文件名长度（8.3 风格，留 null 结尾） */
static constexpr int      SLYZ_NAME_MAX = 52;

/* ======================= 结构 ======================= */

#pragma pack(push, 1)

/* 超级块 —— 占 1 个 sector */
struct SuperBlock {
    uint16_t magic;             /* 0x534C */
    uint16_t version;           /* 0x0001 */
    uint16_t cluster_size;      /* 每簇 sectors 数（4） */
    uint16_t fat_sectors;       /* FAT 占用 sectors 数 */
    uint16_t root_sectors;      /* 根目录占用 sectors 数 */
    uint32_t data_start_sector; /* 数据区起始 sector（绝对 LBA） */
    uint32_t total_clusters;    /* 数据区总簇数 */
    uint32_t first_free_cluster; /* 空闲簇链表头（0 = 无空闲） */
    /* 根目录已分配到的条目数（高水位线）。目录条目按顺序分配，
     * 此值之外不可能存在有效条目，扫描时可据此裁剪扇区范围，
     * 避免每次查目录都读满整个根目录区。
     * 该字段取自原 reserved 区，结构体总尺寸保持 512B 对齐不变。 */
    uint32_t root_entry_count;
    uint8_t  reserved[480];      /* 对齐到 512B */
};

/* 目录条目 —— 71 字节（pack(1) 精确布局） */
struct DirEntry {
    uint8_t  type;                          /* DIR_EMPTY / DIR_DELETED / DIR_FILE / DIR_DIR */
    uint8_t  name[SLYZ_NAME_MAX];           /* 文件名，零结尾，52 字节够长 */
    uint16_t start_cluster;                 /* 起始簇（簇号） */
    uint32_t size;                          /* 文件大小（字节），目录此项为 0 */
    uint32_t created_ts;                    /* 创建时间戳（Unix time，0 = 未知） */
    uint32_t modified_ts;                   /* 修改时间戳 */
    /* 原 reserved[4] 从本版本起解释为权限字段，结构尺寸与磁盘布局完全不变：
     *   mode —— 12 位 UNIX 权限（rwx 三组），0 表示旧版本创建的条目，
     *           访问时按文件 0600 / 目录 0700、owner=root 处理；
     *   uid  —— owner 用户号（0 = root，新用户从 1 起递增）；
     *   gid  —— 预留的组号，当前始终为 0。 */
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};

/* 编译期锁死布局：任何字段增删都会立刻在这里报错，
 * 避免磁盘格式与内存结构悄悄失配。 */
static_assert(sizeof(DirEntry) == SLYZ_DIRENTRY_SIZE,
              "DirEntry size must match SLYZ_DIRENTRY_SIZE");
static_assert(sizeof(SuperBlock) == 506,
              "SuperBlock layout changed; bump SLYZ_VERSION");

/* 文件句柄 —— 打开文件时返回 */
struct FileHandle {
    uint16_t first_cluster;
    uint16_t current_cluster;   /* 当前读/写位置所在簇 */
    uint16_t parent_cluster;    /* 父目录簇，close 时用来更新目录条目 */
    uint32_t size;
    uint32_t position;          /* 当前文件内偏移（字节） */
    uint8_t  is_open;
    uint8_t  is_dir;
    uint8_t  name[SLYZ_NAME_MAX];
};

#pragma pack(pop)

/* ======================= 类 ======================= */

class SlyzFS {
public:
    static constexpr uint32_t SLYZ_MAX_FAT_ENTRIES = 16384;  /* 64 sectors * 256 条目/扇区 */
    static constexpr uint32_t SLYZ_MAX_OPEN_FILES  = 32;

    SlyzFS();
    ~SlyzFS();

    /* ---- 生命周期 ---- */

    /* 格式化：写超级块 + 清零 FAT + 初始化根目录。
     * total_sectors 用于限制数据区边界；传 0 则自动取最大可能值。 */
    bool format(uint32_t total_sectors = 0);

    /* 挂载：读超级块 + FAT 到内存缓存 + 读根目录。
     * 之后所有操作先查缓存，脏了才回写。 */
    bool mount();

    /* 卸载：把脏的 FAT 写回磁盘。 */
    void unmount();

    /* ---- 目录操作 ---- */

    /* 规范化路径：把正斜杠 '\' 转反斜杠 '\'（用户可能写错），去掉末尾分隔符 */
    static void normalize_path(char* path);

    /* 解析路径："foo\bar\baz.txt" → 父目录 "foo\bar"，最后组件 "baz.txt"。
     * 返回父目录的 dir_cluster 和最后组件名。
     * 根目录 "foo.txt" → 父目录 cluster = 0（约定 FAT 条目 0 指向根目录）。 */
    static bool split_path(const char* path, char* parent_out, char* name_out);

    /* 遍历路径：逐级下钻，找到最终目录的起始簇。
     * path 必须是目录路径（不以文件名结尾）。
     * 返回起始簇号；根目录返回 0；找不到返回 0xFFFF。 */
    uint16_t resolve_dir(const char* path);

    /* 列目录：把目录下所有条目追加写入 out 数组，最多 out_max 个。
     * 返回实际写入的数量；-1 表示路径不存在。 */
    int list_dir(const char* path, DirEntry* out, int out_max);

    /* 创建目录：父目录必须存在，name 不能含分隔符。
     * 返回 true 成功，false 失败（磁盘满 / 重名 / 父目录不存在）。 */
    bool mkdir(const char* path);

    /* 删除目录：必须空目录。递归删除不是 v0.1 的事。 */
    bool rmdir(const char* path);

    /* ---- 文件操作 ---- */

    /* 创建文件（如已存在则清空）。返回 true 成功。 */
    bool create(const char* path);
    /* 删除文件。 */
    bool remove(const char* path);

    /* 打开文件：mode = "r" 读，"w" 写（截断），"a" 追加。
     * 返回 FileHandle*；调用者负责 close。 */
    FileHandle* open(const char* path, const char* mode);

    /* 关闭文件（不卸载文件系统）。 */
    void close(FileHandle* fh);

    /* 从 fh 当前位置读最多 n 字节到 buf。返回实际字节数；0 表示 EOF 或错误。 */
    size_t read(FileHandle* fh, uint8_t* buf, size_t n);

    /* 把 buf 里的 n 字节写到 fh 当前位置（自动扩展文件）。返回实际字节数。 */
    size_t write(FileHandle* fh, const uint8_t* buf, size_t n);

    /* 把 fh 的读写位置移到 offset（SEEK_SET，其他 whence 以后再做）。 */
    bool seek(FileHandle* fh, uint32_t offset);

    /* ---- 元数据 ---- */

    /* 查找文件/目录：完整路径。返回条目指针（指向内部缓存，别 free）；找不到返回 nullptr。 */
    DirEntry* stat(const char* path);

    /* ---- 权限 ---- */

    /* 当前操作者用户号。所有权限检查都以它为准；0 表示 root，绕过全部检查。
     * 该值由登录层设置，文件系统层只负责强制执行。 */
    static void set_current_uid(uint16_t uid);
    static uint16_t current_uid();

    /* 修改权限位。需要 root 或父目录 owner 身份。 */
    bool chmod(const char* path, uint16_t mode);

    /* 修改 owner。需要 root 身份（目录 owner 也不能把文件送人）。 */
    bool chown(const char* path, uint8_t uid);

    /* 解析目录路径并返回其条目副本（根目录返回合成的 uid 0 / 0755 条目）。 */
    bool resolve_dir_entry(const char* path, DirEntry* out);

    /* ---- 调试 ---- */

    /* 打印 FAT 非空闲条目（调试用）。 */
    void debug_print_fat() const;

    /* 打印根目录所有条目（调试用）。 */
    void debug_print_root() const;

private:
    /* ---- FAT 缓存 ---- */
    uint16_t* fat_;                     /* 指向 fat_storage_；避免裸机环境使用 new[] */
    uint16_t  fat_storage_[SLYZ_MAX_FAT_ENTRIES];
    uint32_t  fat_size_;                /* FAT 总条目数 */
    bool      fat_dirty_;               /* 缓存是否脏，unmount 时要写回 */

    /* ---- 文件句柄池 ---- */
    FileHandle open_handles_[SLYZ_MAX_OPEN_FILES];

    /* ---- 超级块 ---- */
    SuperBlock sb_;
    bool       mounted_;

    /* ---- 内部工具 ---- */

    /* 计算某用户对条目的有效权限位（4=读, 2=写, 1=执行）。
     * 旧版本条目的 mode 为 0，按 0600（文件）/ 0700（目录）保守处理。 */
    static uint8_t access_bits(const DirEntry& de, uint16_t uid);
    static bool has_perm(const DirEntry& de, uint16_t uid, uint8_t want);

    /* 就地更新目录条目的 mode / uid，set_mode/set_uid 控制哪些字段生效。 */
    bool update_meta(const char* path, uint16_t mode, uint8_t uid,
                     bool set_mode, bool set_uid);

    /* 当前操作者（进程级，非线程安全；裸内核单线程） */
    static uint16_t g_current_uid;

    FileHandle* acquire_handle();
    void release_handle(FileHandle* fh);

    /* 把内存中的超级块写回磁盘（用于持久化 root_entry_count 等元数据） */
    bool flush_superblock();

    /* 分配一个空闲簇：从 first_free_cluster 链表摘，改 FAT，返回簇号。
     * 磁盘满返回 0xFFFF。 */
    uint16_t alloc_cluster();

    /* 释放从 cluster 起的整条 FAT 链。*/
    void free_chain(uint16_t cluster);

    /* 读 FAT 链：把 cluster 起的所有簇号写 out，out 够大。
     * 返回簇链长度。 */
    int read_chain(uint16_t cluster, uint16_t* out, int out_max);

    /* 簇号 → 绝对 LBA */
    uint32_t cluster_to_lba(uint16_t cluster) const;

    /* 读一个簇到 buf（必须 ≥ cluster_size * SECTOR_SIZE 字节）。*/
    bool read_cluster(uint16_t cluster, uint8_t* buf);

    /* 写 buf 到一个簇。*/
    bool write_cluster(uint16_t cluster, const uint8_t* buf);

    /* 读指定簇的目录条目区，找 name 对应条目。
     * parent_cluster = 0 表示根目录。返回条目指针；找不到 nullptr。
     * idx_out 可选：返回条目在目录中的序号（用于原地写回）。 */
    DirEntry* find_entry_in_dir(uint16_t parent_cluster, const char* name, int* idx_out = nullptr);

    /* 在指定目录下申请一个新条目（找 DIR_EMPTY 或 DIR_DELETED 的槽位）。
     * 返回条目指针 + 写入位置 index。目录满了返回 nullptr。 */
    DirEntry* alloc_entry_in_dir(uint16_t parent_cluster, int* idx_out);

    /* 写回整个目录簇（因为目录条目和 FAT 一样是簇链式的）。 */
    void flush_dir_entries(uint16_t dir_cluster, const DirEntry* entries, int count);

    /* ---- 禁止拷贝 ---- */
    SlyzFS(const SlyzFS&) = delete;
    SlyzFS& operator=(const SlyzFS&) = delete;
};

} /* namespace slyzfs */

#endif /* SLYZ_FS_H */
