/*
 * slyzfs.cpp —— SaylterOS 原生文件系统 v0.1 完整实现
 *
 * 不依赖任何 OS 提供的内存/文件/锁，仅靠 disk_io.h 两个扇区读写。
 * 代码设计遵循：所有持久化状态先改内存缓存（FAT/dir entries），
 * 只在 flush/unmount 时一次性写回磁盘，避免中间态写坏导致文件系统损坏。
 */

#include "slyzfs.h"
#include "disk_io.h"

#ifdef SLYZFS_NO_STDIO
extern "C" int slyzfs_logf(const char* fmt, ...);
#define SLYZ_PRINTF(...) slyzfs_logf(__VA_ARGS__)
#define SLYZ_FPRINTF(_unused, ...) slyzfs_logf(__VA_ARGS__)
#else
#include <cstdio>
#define SLYZ_PRINTF std::printf
#define SLYZ_FPRINTF(...) std::fprintf(__VA_ARGS__)
#endif

#include <cstdlib>

namespace slyzfs {

/* ======================= 辅助工具 ======================= */

static bool is_sep(char c) { return c == '\\' || c == '/'; }

static void copy_name(uint8_t* dst, const char* src) {
    size_t i = 0;
    while (i < SLYZ_NAME_MAX - 1 && src[i] != '\0' && !is_sep(src[i])) {
        dst[i] = (uint8_t)src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool names_equal(const uint8_t* a, const char* b) {
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != (uint8_t)b[i]) return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* ======================= 构造/析构 ======================= */

SlyzFS::SlyzFS()
    : fat_(nullptr), fat_size_(0), fat_dirty_(false), mounted_(false) {
    slyz_memset(&sb_, 0, sizeof(sb_));
    slyz_memset(fat_storage_, 0, sizeof(fat_storage_));
    slyz_memset(open_handles_, 0, sizeof(open_handles_));
    fat_ = fat_storage_;
}

SlyzFS::~SlyzFS() {
    unmount();
}

FileHandle* SlyzFS::acquire_handle() {
    for (uint32_t i = 0; i < SLYZ_MAX_OPEN_FILES; ++i) {
        if (!open_handles_[i].is_open) {
            slyz_memset(&open_handles_[i], 0, sizeof(open_handles_[i]));
            return &open_handles_[i];
        }
    }
    return nullptr;
}

/* 超级块回写：必须用完整 512 字节扇区缓冲，
 * 因为 sizeof(SuperBlock) 是 506 而磁盘层固定搬运 512。 */
bool SlyzFS::flush_superblock() {
    uint8_t sb_sector[SLYZ_SECTOR_SIZE];
    slyz_memset(sb_sector, 0, sizeof(sb_sector));
    slyz_memcpy(sb_sector, &sb_, sizeof(sb_));
    return slyz_disk_write(SLYZ_FS_OFFSET, sb_sector);
}

void SlyzFS::release_handle(FileHandle* fh) {
    if (!fh) return;
    for (uint32_t i = 0; i < SLYZ_MAX_OPEN_FILES; ++i) {
        if (&open_handles_[i] == fh) {
            slyz_memset(&open_handles_[i], 0, sizeof(open_handles_[i]));
            return;
        }
    }
}

/* ======================= 格式化 ======================= */

bool SlyzFS::format(uint32_t total_sectors) {
    /* 限制数据区大小，避免 FAT 条目不够 */
    uint32_t offset = SLYZ_FS_OFFSET;
    uint32_t super_sectors = 1;
    uint32_t fat_sectors = SLYZ_FAT_SECTORS;
    uint32_t root_sectors = SLYZ_ROOT_SECTORS;

    /* 每个 sector 有 512/2 = 256 个 FAT 条目 */
    uint32_t fat_entries = fat_sectors * 256;

    /* 数据区起始 */
    uint32_t data_start = offset + super_sectors + fat_sectors + root_sectors;

    /* 计算总簇数：限制在 FAT 能表示的范围内（fat_entries - 2，因为 cluster 0/1 保留）*/
    uint32_t available_sectors = 0;
    if (total_sectors > 0 && total_sectors > data_start) {
        available_sectors = total_sectors - data_start;
    } else {
        available_sectors = fat_entries * SLYZ_CLUSTER_SIZE;
    }
    uint32_t max_by_fat = (fat_entries - 2) * SLYZ_CLUSTER_SIZE;
    if (available_sectors > max_by_fat) available_sectors = max_by_fat;

    uint32_t total_clusters = available_sectors / SLYZ_CLUSTER_SIZE;

    /* 写超级块 */
    slyz_memset(&sb_, 0, sizeof(sb_));
    sb_.magic = SLYZ_MAGIC;
    sb_.version = SLYZ_VERSION;
    sb_.cluster_size = SLYZ_CLUSTER_SIZE;
    sb_.fat_sectors = fat_sectors;
    sb_.root_sectors = root_sectors;
    sb_.data_start_sector = data_start;
    sb_.total_clusters = total_clusters;
    sb_.first_free_cluster = 0; /* 格式化时先不管空闲链，全部 FAT_FREE */
    sb_.root_entry_count = 0;   /* 根目录尚无条目 */

    /* 写超级块：同样先落到完整的 512 字节扇区缓冲再落盘，
     * 避免磁盘层从 506 字节的 sb_ 读出越界内容。 */
    uint8_t sb_sector[SLYZ_SECTOR_SIZE];
    slyz_memset(sb_sector, 0, sizeof(sb_sector));
    slyz_memcpy(sb_sector, &sb_, sizeof(sb_));
    if (!slyz_disk_write(offset, sb_sector)) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] format: write superblock failed\n");
        return false;
    }

    /* 清零 FAT 区 + 设置簇 0 / 1 为保留 */
    uint8_t sector_buf[SLYZ_SECTOR_SIZE];
    for (uint32_t s = 0; s < fat_sectors; s++) {
        slyz_memset(sector_buf, 0, sizeof(sector_buf));
        if (s == 0) {
            /* 每个 sector 有 256 个 16-bit 条目 */
            uint16_t* first = reinterpret_cast<uint16_t*>(sector_buf);
            first[0] = FAT_RESERVED; /* cluster 0 保留（约定根目录用 0 表示） */
            first[1] = FAT_RESERVED; /* cluster 1 保留 */
        }
        if (!slyz_disk_write(offset + super_sectors + s, sector_buf)) {
            SLYZ_FPRINTF(stderr, "[SlyzFS] format: write FAT sector %u failed\n", s);
            return false;
        }
    }

    /* 清零根目录区 */
    for (uint32_t s = 0; s < root_sectors; s++) {
        slyz_memset(sector_buf, 0, sizeof(sector_buf));
        if (!slyz_disk_write(offset + super_sectors + fat_sectors + s, sector_buf)) {
            SLYZ_FPRINTF(stderr, "[SlyzFS] format: write root sector %u failed\n", s);
            return false;
        }
    }

    /* 刷新 in-memory 状态 */
    mounted_ = false;
    fat_dirty_ = false;
    fat_size_ = fat_entries;
    fat_ = fat_storage_;
    slyz_memset(fat_, 0, sizeof(uint16_t) * fat_size_);

    SLYZ_PRINTF("[SlyzFS] format OK: %u clusters, data_start=%u\n", total_clusters, data_start);
    return true;
}

/* ======================= 挂载 ======================= */

bool SlyzFS::mount() {
    uint32_t sb_lba = SLYZ_FS_OFFSET;
    /* 扇区读写必须用完整 512 字节缓冲，不能直接把 &sb_ 交给磁盘层：
     * sizeof(SuperBlock) 只有 506 字节（pack(1)），而磁盘操作固定搬运 512 字节，
     * 直接传 &sb_ 会越界写穿对象尾部，覆盖紧跟其后的静态成员。 */
    uint8_t sb_sector[SLYZ_SECTOR_SIZE];
    if (!slyz_disk_read(sb_lba, sb_sector)) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mount: read superblock failed\n");
        return false;
    }
    slyz_memcpy(&sb_, sb_sector, sizeof(sb_));
    if (sb_.magic != SLYZ_MAGIC) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mount: bad magic 0x%04X\n", sb_.magic);
        return false;
    }
    if (sb_.version != SLYZ_VERSION) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mount: unsupported version 0x%04X (want 0x%04X)\n", sb_.version, SLYZ_VERSION);
        return false;
    }
    /* 所有簇级缓冲区都按编译期常量 SLYZ_CLUSTER_BYTES 分配，
     * 必须确认磁盘记录的簇大小与之一致，否则 read/write_cluster 会越界。 */
    if (sb_.cluster_size != SLYZ_CLUSTER_SIZE) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mount: unsupported cluster size %u (want %u)\n",
                     (unsigned)sb_.cluster_size, (unsigned)SLYZ_CLUSTER_SIZE);
        return false;
    }

    /* FAT 条目数。必须校验上界：fat_storage_ 是定长静态缓冲，
     * 旧格式镜像的 fat_sectors 可能大于当前支持值，直接 memset 会越界。 */
    fat_size_ = (uint32_t)sb_.fat_sectors * (SLYZ_SECTOR_SIZE / sizeof(uint16_t));
    if (fat_size_ > SLYZ_MAX_FAT_ENTRIES) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mount: FAT too large (%u entries > %u)\n",
                     (unsigned)fat_size_, (unsigned)SLYZ_MAX_FAT_ENTRIES);
        return false;
    }
    fat_ = fat_storage_;
    slyz_memset(fat_, 0, sizeof(uint16_t) * fat_size_);
    fat_dirty_ = false;

    uint32_t fat_start = sb_lba + 1;
    for (uint32_t s = 0; s < sb_.fat_sectors; s++) {
        uint8_t tmp[SLYZ_SECTOR_SIZE];
        if (!slyz_disk_read(fat_start + s, tmp)) {
            SLYZ_FPRINTF(stderr, "[SlyzFS] mount: read FAT sector %u failed\n", s);
            fat_ = nullptr;
            return false;
        }
        slyz_memcpy(fat_ + s * 256, tmp, SLYZ_SECTOR_SIZE);
    }

    mounted_ = true;
    SLYZ_PRINTF("[mount OK] version=%u clusters=%u\n", sb_.version, sb_.total_clusters);
    SLYZ_PRINTF("[mount] FAT dump (0-9): ");
    for (int i = 0; i < 10 && i < (int)fat_size_; i++) SLYZ_PRINTF("%04X ", fat_[i]);
    SLYZ_PRINTF("\n");
    return true;
}

/* ======================= 卸载 ======================= */

void SlyzFS::unmount() {
    if (!mounted_ || !fat_) return;

    SLYZ_PRINTF("[unmount] fat_dirty=%d\n", fat_dirty_);
    if (fat_dirty_) {
        uint32_t fat_start = SLYZ_FS_OFFSET + 1;
        for (uint32_t s = 0; s < sb_.fat_sectors; s++) {
            uint8_t tmp[SLYZ_SECTOR_SIZE];
            slyz_memcpy(tmp, fat_ + s * 256, SLYZ_SECTOR_SIZE);
            if (!slyz_disk_write(fat_start + s, tmp)) {
                SLYZ_FPRINTF(stderr, "[SlyzFS] unmount: write FAT sector %u failed\n", s);
            }
        }
        /* 打印前 10 个实际内容 */
        SLYZ_PRINTF("[unmount] FAT dump (0-9): ");
        for (int i = 0; i < 10 && i < (int)fat_size_; i++) SLYZ_PRINTF("%04X ", fat_[i]);
        SLYZ_PRINTF("\n");
        fat_dirty_ = false;
    }

    /* 超级块也回写一下（可能改过 first_free_cluster 等） */
    slyz_disk_write(SLYZ_FS_OFFSET, reinterpret_cast<uint8_t*>(&sb_));

    mounted_ = false;
}

/* ======================= 路径规范化 ======================= */

void SlyzFS::normalize_path(char* path) {
    if (!path) return;
    /* 先把 '/' 全转成 '\\' */
    for (char* p = path; *p; ++p) {
        if (*p == '/') *p = '\\';
    }
    /* 去掉连续的 '\' （保留第一个） */
    char* w = path;
    char last = '\0';
    for (char* r = path; *r; ++r) {
        if (*r == '\\' && last == '\\') continue;
        *w++ = *r;
        last = *r;
    }
    *w = '\0';
    /* 去掉末尾的 '\'（单个 '\' 表示根目录，不能去掉） */
    size_t len = slyz_strlen(path);
    while (len > 1 && path[len - 1] == '\\') {
        path[len - 1] = '\0';
        len--;
    }
}

bool SlyzFS::split_path(const char* path, char* parent_out, char* name_out) {
    if (!path || !parent_out || !name_out) return false;

    char tmp[512];
    slyz_strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    normalize_path(tmp);

    /* 找最后一个分隔符 —— 必须是反斜杠 */
    const char* last_sep = nullptr;
    for (const char* p = tmp; *p; ++p) {
        if (*p == '\\') last_sep = p;
    }

    if (last_sep == nullptr) {
        parent_out[0] = '\0';
        slyz_strncpy(name_out, tmp, SLYZ_NAME_MAX - 1);
        name_out[SLYZ_NAME_MAX - 1] = '\0';
    } else {
        size_t parent_len = (size_t)(last_sep - tmp);
        if (parent_len >= 512) parent_len = 511;
        slyz_memcpy(parent_out, tmp, parent_len);
        parent_out[parent_len] = '\0';
        slyz_strncpy(name_out, last_sep + 1, SLYZ_NAME_MAX - 1);
        name_out[SLYZ_NAME_MAX - 1] = '\0';
    }
    return true;
}

/* ======================= FAT 工具 ======================= */

uint16_t SlyzFS::alloc_cluster() {
    if (!fat_) return FAT_END;

    /* 先沿着 sb_.first_free_cluster 的链表走，看是否还有空闲 */
    if (sb_.first_free_cluster >= 2 && sb_.first_free_cluster < fat_size_) {
        uint16_t c = sb_.first_free_cluster;
        uint16_t next = fat_[c];
        fat_[c] = FAT_END; /* 摘下来，先当文件尾 */
        fat_dirty_ = true;
        sb_.first_free_cluster = (next == FAT_FREE) ? 0 : next;
        return c;
    }

    /* 找不到链表，线性扫 */
    for (uint32_t i = 2; i < fat_size_; i++) {
        if (fat_[i] == FAT_FREE) {
            fat_[i] = FAT_END;
            fat_dirty_ = true;
            return (uint16_t)i;
        }
    }

    SLYZ_FPRINTF(stderr, "[SlyzFS] disk full — no free cluster\n");
    return FAT_END;
}

void SlyzFS::free_chain(uint16_t cluster) {
    if (!fat_ || cluster < 2 || cluster >= fat_size_) return;

    uint16_t c = cluster;
    while (c != FAT_END && c != FAT_FREE && c != FAT_BAD && c != FAT_RESERVED) {
        uint16_t next = fat_[c];
        fat_[c] = FAT_FREE;

        /* 维护空闲链表：让 first_free_cluster 指向这里 */
        if (sb_.first_free_cluster == 0) {
            sb_.first_free_cluster = c;
        }
        /* 这里我们不做双向链表维护 —— v0.1 简单化，分配时扫就行 */
        /* （其实 alloc_cluster 会把它摘下来，够了） */

        if (next < 2 || next >= fat_size_ || next == FAT_END) break;
        c = next;
    }
    fat_dirty_ = true;
}

int SlyzFS::read_chain(uint16_t cluster, uint16_t* out, int out_max) {
    if (!fat_ || cluster == FAT_END) return 0;
    int count = 0;
    uint16_t c = cluster;
    while (count < out_max && c >= 2 && c < fat_size_ && c != FAT_END) {
        out[count++] = c;
        c = fat_[c];
    }
    return count;
}

uint32_t SlyzFS::cluster_to_lba(uint16_t cluster) const {
    if (cluster == 0) {
        /* 根目录特殊处理：直接算 LBA */
        return SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
    }
    return sb_.data_start_sector + (uint32_t)(cluster - 2) * sb_.cluster_size;
}

bool SlyzFS::read_cluster(uint16_t cluster, uint8_t* buf) {
    uint32_t lba = cluster_to_lba(cluster);
    for (uint16_t i = 0; i < sb_.cluster_size; i++) {
        if (!slyz_disk_read(lba + i, buf + i * SLYZ_SECTOR_SIZE)) {
            return false;
        }
    }
    return true;
}

bool SlyzFS::write_cluster(uint16_t cluster, const uint8_t* buf) {
    uint32_t lba = cluster_to_lba(cluster);
    for (uint16_t i = 0; i < sb_.cluster_size; i++) {
        if (!slyz_disk_write(lba + i, buf + i * SLYZ_SECTOR_SIZE)) {
            return false;
        }
    }
    return true;
}

/* ======================= 权限 ======================= */

uint16_t SlyzFS::g_current_uid = 0;

void SlyzFS::set_current_uid(uint16_t uid) {
    g_current_uid = uid;
}

uint16_t SlyzFS::current_uid() {
    return g_current_uid;
}

uint8_t SlyzFS::access_bits(const DirEntry& de, uint16_t uid) {
    uint16_t mode = de.mode;
    if (mode == 0) {
        /* 旧版本卷的 reserved 全 0。为了不把历史文件直接暴露给所有人，
         * 按 owner 独占解释，升级后只会更严，不会更松。 */
        mode = (de.type == DIR_DIR) ? 0700 : 0600;
    }
    if (uid == 0) return 7;
    if (de.uid == (uint8_t)(uid & 0xFF)) {
        return (uint8_t)((mode >> 6) & 7);
    }
    return (uint8_t)(mode & 7);
}

bool SlyzFS::has_perm(const DirEntry& de, uint16_t uid, uint8_t want) {
    return (access_bits(de, uid) & want) == want;
}

/* ======================= 目录条目工具 ======================= */

DirEntry* SlyzFS::find_entry_in_dir(uint16_t parent_cluster, const char* name, int* idx_out) {
    /* 用一个临时数组存条目并记录每个条目的槽位序号（因为根目录槽位是固定 sector 里的 index，子目录是簇链里的偏移）。
     * 这里先简单实现：遍历所有条目，找到名字匹配的那个。返回的是静态临时对象地址 —— 调用者只能在读函数返回后立即使用。
     * 更好的做法：返回磁盘位置（cluster + 偏移），再写回时用它。先这么写，后面 flush 的时候再处理。 */

    static DirEntry scratch[4096];
    static int  slot_cluster[4096];
    static int  slot_offset_in_cluster[4096];
    static int  slot_is_root[4096];

    int count = 0;

    if (parent_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        /* 扫描上界由超级块记录的 root_entry_count 决定：
         * 已分配条目之外不可能有数据，因此无需读满整个根目录区。
         * 注意不能用"遇到空扇区就停"的启发式 —— 删除会产生 DIR_DELETED 空洞，
         * 空洞之后仍可能有有效条目，那样会漏检。 */
        uint32_t used_sectors = (sb_.root_entry_count + SLYZ_ENTRIES_PER_SECTOR - 1) / SLYZ_ENTRIES_PER_SECTOR;
        if (used_sectors > sb_.root_sectors) used_sectors = sb_.root_sectors;
        for (uint16_t s = 0; s < used_sectors && count < 4096; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) break;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && count < 4096; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de->type == DIR_EMPTY) continue;
                if (de->type == DIR_DELETED) continue;
                scratch[count] = *de;
                slot_cluster[count] = s;           /* 根目录：用 sector index 当 cluster 编号 */
                slot_offset_in_cluster[count] = e; /* sector 内的条目偏移 */
                slot_is_root[count] = 1;
                count++;
            }
        }
    } else {
        uint16_t chain[4096];
        int clen = read_chain(parent_cluster, chain, 4096);
        int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
        uint8_t buf[SLYZ_CLUSTER_BYTES];

        for (int i = 0; i < clen && count < 4096; i++) {
            if (!read_cluster(chain[i], buf)) break;
            DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
            for (int e = 0; e < entries_per_cluster && count < 4096; e++) {
                if (cluster_entries[e].type == DIR_EMPTY) continue;
                scratch[count] = cluster_entries[e];
                slot_cluster[count] = chain[i];
                slot_offset_in_cluster[count] = e;
                slot_is_root[count] = 0;
                count++;
            }
        }
    }

    /* 查找 */
    for (int i = 0; i < count; i++) {
        if (names_equal(scratch[i].name, name)) {
            if (idx_out) *idx_out = i;
            return &scratch[i];
        }
    }
    return nullptr;
}

/* ======================= 解析目录路径 ======================= */

uint16_t SlyzFS::resolve_dir(const char* path) {
    if (!path || path[0] == '\0' || (path[0] == '\\' && path[1] == '\0')) {
        return 0; /* 根目录 */
    }

    char tmp[512];
    slyz_strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    normalize_path(tmp);

    /* 分段下钻：手写 token 解析，不依赖 strtok_r / POSIX 扩展 */
    uint16_t cur_cluster = 0;
    char* cursor = tmp;
    while (*cursor != '\0') {
        char* token = cursor;
        while (*cursor != '\0' && *cursor != '\\') {
            ++cursor;
        }
        if (*cursor == '\\') {
            *cursor = '\0';
            ++cursor;
        }
        if (token[0] != '\0') {
            DirEntry* de = find_entry_in_dir(cur_cluster, token);
            if (!de || de->type != DIR_DIR) {
                return FAT_END;
            }
            cur_cluster = de->start_cluster;
        }
        if (*cursor == '\0') {
            break;
        }
    }
    return cur_cluster;
}

/* ======================= 列目录 ======================= */

bool SlyzFS::resolve_dir_entry(const char* path, DirEntry* out) {
    if (!out) return false;

    slyz_memset(out, 0, sizeof(*out));
    out->type = DIR_DIR;
    out->mode = 0755;
    out->uid = 0;
    out->gid = 0;

    if (!path || path[0] == '\0' || (path[0] == '\\' && path[1] == '\0')) {
        return true;   /* 根目录：合成的 0755 root 条目 */
    }

    char tmp[512];
    slyz_strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    normalize_path(tmp);

    uint16_t cur_cluster = 0;
    char* cursor = tmp;
    while (*cursor != '\0') {
        char* token = cursor;
        while (*cursor != '\0' && *cursor != '\\') {
            ++cursor;
        }
        if (*cursor == '\\') {
            *cursor = '\0';
            ++cursor;
        }
        if (token[0] != '\0') {
            DirEntry* de = find_entry_in_dir(cur_cluster, token);
            if (!de || de->type != DIR_DIR) return false;
            *out = *de;
            cur_cluster = de->start_cluster;
        }
        if (*cursor == '\0') break;
    }
    return true;
}

int SlyzFS::list_dir(const char* path, DirEntry* out, int out_max) {
    /* 目录读权限：没有读位就整体拒绝，避免普通用户枚举别人的目录 */
    DirEntry dir_de;
    if (!resolve_dir_entry(path, &dir_de)) return -1;
    if (!has_perm(dir_de, g_current_uid, 4)) return -1;

    uint16_t dir_cluster;
    if (!path || path[0] == '\0' || (path[0] == '\\' && path[1] == '\0')) {
        dir_cluster = 0;
    } else {
        dir_cluster = resolve_dir(path);
        if (dir_cluster == FAT_END) return -1;
    }

    /* 复用 scratch 逻辑，直接输出 */
    int written = 0;

    if (dir_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        /* 与 find_entry_in_dir 同理：只读已分配范围，避免扫描整个根目录区 */
        uint32_t used_sectors = (sb_.root_entry_count + SLYZ_ENTRIES_PER_SECTOR - 1) / SLYZ_ENTRIES_PER_SECTOR;
        if (used_sectors > sb_.root_sectors) used_sectors = sb_.root_sectors;
        for (uint16_t s = 0; s < used_sectors && written < out_max; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) break;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && written < out_max; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de->type == DIR_EMPTY || de->type == DIR_DELETED) continue;
                out[written++] = *de;
            }
        }
    } else {
        uint16_t chain[4096];
        int clen = read_chain(dir_cluster, chain, 4096);
        int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
        uint8_t buf[SLYZ_CLUSTER_BYTES];

        for (int i = 0; i < clen && written < out_max; i++) {
            if (!read_cluster(chain[i], buf)) break;
            DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
            for (int e = 0; e < entries_per_cluster && written < out_max; e++) {
                if (cluster_entries[e].type == DIR_EMPTY || cluster_entries[e].type == DIR_DELETED) continue;
                out[written++] = cluster_entries[e];
            }
        }
    }
    return written;
}

/* ======================= 创建目录 ======================= */

bool SlyzFS::mkdir(const char* path) {
    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return false;

    uint16_t parent_cluster = resolve_dir(parent);
    if (parent_cluster == FAT_END) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mkdir: parent dir not found: '%s'\n", parent);
        return false;
    }

    DirEntry parent_de;
    if (!resolve_dir_entry(parent, &parent_de)) return false;
    if (!has_perm(parent_de, g_current_uid, 6)) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mkdir: permission denied on '%s'\n", parent);
        return false;
    }

    /* 重名检查 */
    if (find_entry_in_dir(parent_cluster, name)) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] mkdir: '%s' already exists\n", name);
        return false;
    }

    /* 分配一个簇放目录内容 */
    uint16_t new_cluster = alloc_cluster();
    if (new_cluster == FAT_END) return false;

    /* 清零那个簇 */
    uint8_t zero[SLYZ_CLUSTER_BYTES];
    slyz_memset(zero, 0, sizeof(zero));
    if (!write_cluster(new_cluster, zero)) return false;

    /* 注册条目到父目录 —— 这里不能用 find_entry_in_dir 返回的 static scratch，
     * 因为我们要在磁盘上写新条目槽位。简化版：直接遍历目录区找 DIR_EMPTY 槽。 */

    bool written = false;
    DirEntry new_entry;
    slyz_memset(&new_entry, 0, sizeof(new_entry));
    new_entry.type = DIR_DIR;
    copy_name(new_entry.name, name);
    new_entry.start_cluster = new_cluster;
    new_entry.size = 0;
    new_entry.mode = 0755;
    new_entry.uid = (uint8_t)(g_current_uid & 0xFF);
    new_entry.gid = 0;

    if (parent_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        /* 只需在已分配范围内找空槽（可复用 DIR_DELETED 空洞），
         * 找不到再延展一个扇区。这样既不会漏掉碎片空洞，也不用读满整个根目录区。 */
        uint32_t used_sectors = (sb_.root_entry_count + SLYZ_ENTRIES_PER_SECTOR - 1) / SLYZ_ENTRIES_PER_SECTOR;
        uint32_t scan_limit = used_sectors + 1u;
        if (scan_limit > sb_.root_sectors) scan_limit = sb_.root_sectors;
        for (uint16_t s = 0; s < scan_limit && !written; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) break;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && !written; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de->type == DIR_EMPTY || de->type == DIR_DELETED) {
                    *de = new_entry;
                    if (!slyz_disk_write(lba + s, sector)) break;
                    written = true;
                    uint32_t idx = (uint32_t)s * SLYZ_ENTRIES_PER_SECTOR + (uint32_t)e + 1u;
                    if (idx > sb_.root_entry_count) {
                        sb_.root_entry_count = idx;
                        flush_superblock();
                    }
                }
            }
        }
    } else {
        /* 子目录：簇链上找空槽。可能所有簇都满了 → 分配新簇挂链 */
        uint16_t chain[4096];
        int clen = read_chain(parent_cluster, chain, 4096);
        int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
        uint8_t buf[SLYZ_CLUSTER_BYTES];

        for (int i = 0; i < clen && !written; i++) {
            if (!read_cluster(chain[i], buf)) break;
            DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
            for (int e = 0; e < entries_per_cluster && !written; e++) {
                if (cluster_entries[e].type == DIR_EMPTY || cluster_entries[e].type == DIR_DELETED) {
                    cluster_entries[e] = new_entry;
                    write_cluster(chain[i], buf);
                    written = true;
                }
            }
        }

        /* 没找到槽 → 分配新簇挂到链尾 */
        if (!written) {
            uint16_t new_slot_cluster = alloc_cluster();
            if (new_slot_cluster == FAT_END) return false;
            slyz_memset(buf, 0, sizeof(buf));
            DirEntry* first_entry = reinterpret_cast<DirEntry*>(buf);
            *first_entry = new_entry;
            write_cluster(new_slot_cluster, buf);

            /* 把新簇挂到当前链尾 */
            if (clen > 0) {
                uint16_t last = chain[clen - 1];
                fat_[last] = new_slot_cluster;
                fat_[new_slot_cluster] = FAT_END;
                fat_dirty_ = true;
            }
            written = true;
        }
    }

    if (!written) {
        /* 回滚：释放刚才分配的簇 */
        free_chain(new_cluster);
        SLYZ_FPRINTF(stderr, "[SlyzFS] mkdir: parent dir full\n");
        return false;
    }

    return true;
}

/* ======================= 删除目录 ======================= */

bool SlyzFS::rmdir(const char* path) {
    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return false;

    uint16_t parent_cluster = resolve_dir(parent);
    if (parent_cluster == FAT_END) return false;

    DirEntry parent_de;
    if (!resolve_dir_entry(parent, &parent_de)) return false;
    if (!has_perm(parent_de, g_current_uid, 6)) return false;

    int slot_idx = -1;
    DirEntry* de = find_entry_in_dir(parent_cluster, name, &slot_idx);
    if (!de || de->type != DIR_DIR) return false;

    /* 检查目录是否为空：只需取 1 个条目即可判断，
     * 不能开大数组（DirEntry[4096] 是 256KB，远超内核可用栈空间） */
    DirEntry child[1];
    int n = list_dir(path, child, 1);
    if (n > 0) {
        SLYZ_FPRINTF(stderr, "[SlyzFS] rmdir: '%s' not empty\n", path);
        return false;
    }

    /* 释放目录簇 */
    free_chain(de->start_cluster);

    /* 标记条目为 DIR_DELETED。这里需要精确写回磁盘 —— 直接用 slot_idx 在对应 sector/簇上改 type 为 DIR_DELETED。
     * 简化：重新遍历目录区找这个条目然后改。（slot_idx 只能表示顺序，不知道是 root 的 sector 还是子簇）。
     * 简单做法：再遍历一遍目录区，名字匹配的就改 DIR_DELETED 并回写磁盘。 */

    bool deleted = false;

    auto do_delete = [&](DirEntry* e, uint32_t lba_or_cluster, int is_root, int sec_or_cluster_idx, int e_idx) {
        if (names_equal(e->name, name)) {
            e->type = DIR_DELETED;
            if (is_root) {
                uint8_t sector[SLYZ_SECTOR_SIZE];
                slyz_disk_read(lba_or_cluster, sector);
                DirEntry* disk_de = reinterpret_cast<DirEntry*>(sector + e_idx * SLYZ_DIRENTRY_SIZE);
                disk_de->type = DIR_DELETED;
                slyz_disk_write(lba_or_cluster, sector);
            } else {
                uint8_t buf[SLYZ_CLUSTER_BYTES];
                read_cluster(lba_or_cluster, buf);
                DirEntry* disk_de = reinterpret_cast<DirEntry*>(buf);
                int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
                int e_off = sec_or_cluster_idx * entries_per_cluster + e_idx; /* 不对，子目录里每个簇独立 */
                (void)disk_de; (void)e_off;
                /* 子目录场景：e_idx 就是在那个簇内的偏移 */
                DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
                cluster_entries[e_idx].type = DIR_DELETED;
                write_cluster(lba_or_cluster, buf);
            }
            deleted = true;
        }
    };

    if (parent_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        for (uint16_t s = 0; s < sb_.root_sectors && !deleted; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) break;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && !deleted; e++) {
                DirEntry* de2 = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de2->type == DIR_EMPTY) continue;
                if (de2->type == DIR_DIR && names_equal(de2->name, name)) {
                    de2->type = DIR_DELETED;
                    slyz_disk_write(lba + s, sector);
                    deleted = true;
                }
            }
        }
    } else {
        uint16_t chain[4096];
        int clen = read_chain(parent_cluster, chain, 4096);
        int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
        uint8_t buf[SLYZ_CLUSTER_BYTES];

        for (int i = 0; i < clen && !deleted; i++) {
            if (!read_cluster(chain[i], buf)) break;
            DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
            for (int e = 0; e < entries_per_cluster && !deleted; e++) {
                if (cluster_entries[e].type == DIR_EMPTY || cluster_entries[e].type == DIR_DELETED) continue;
                if (cluster_entries[e].type == DIR_DIR && names_equal(cluster_entries[e].name, name)) {
                    cluster_entries[e].type = DIR_DELETED;
                    write_cluster(chain[i], buf);
                    deleted = true;
                }
            }
        }
    }

    return deleted;
}

/* ======================= 创建文件 ======================= */

bool SlyzFS::create(const char* path) {
    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return false;

    uint16_t parent_cluster = resolve_dir(parent);
    SLYZ_PRINTF("[create] path='%s' parent='%s' name='%s' parent_cluster=%u\n", path, parent, name, parent_cluster);
    if (parent_cluster == FAT_END) return false;

    /* 已存在且未删除 → 截断清空 */
    DirEntry* existing = find_entry_in_dir(parent_cluster, name);
    if (existing && existing->type != DIR_DELETED) {
        if (existing->type == DIR_DIR) {
            SLYZ_FPRINTF(stderr, "[SlyzFS] create: '%s' is a directory\n", name);
            return false;
        }
        free_chain(existing->start_cluster);
        remove(path);
    }

    uint16_t new_cluster = alloc_cluster();
    if (new_cluster == FAT_END) return false;

    /* 注册条目 —— 和 mkdir 类似，找空槽 */
    DirEntry new_entry;
    slyz_memset(&new_entry, 0, sizeof(new_entry));
    new_entry.type = DIR_FILE;
    copy_name(new_entry.name, name);
    new_entry.start_cluster = new_cluster;
    new_entry.size = 0;
    new_entry.mode = 0644;
    new_entry.uid = (uint8_t)(g_current_uid & 0xFF);
    new_entry.gid = 0;

    bool written = false;

    if (parent_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        /* 只需在已分配范围内找空槽（可复用 DIR_DELETED 空洞），
         * 找不到再延展一个扇区。这样既不会漏掉碎片空洞，也不用读满整个根目录区。 */
        uint32_t used_sectors = (sb_.root_entry_count + SLYZ_ENTRIES_PER_SECTOR - 1) / SLYZ_ENTRIES_PER_SECTOR;
        uint32_t scan_limit = used_sectors + 1u;
        if (scan_limit > sb_.root_sectors) scan_limit = sb_.root_sectors;
        for (uint16_t s = 0; s < scan_limit && !written; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) break;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && !written; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de->type == DIR_EMPTY || de->type == DIR_DELETED) {
                    *de = new_entry;
                    if (!slyz_disk_write(lba + s, sector)) break;
                    written = true;
                    uint32_t idx = (uint32_t)s * SLYZ_ENTRIES_PER_SECTOR + (uint32_t)e + 1u;
                    if (idx > sb_.root_entry_count) {
                        sb_.root_entry_count = idx;
                        flush_superblock();
                    }
                }
            }
        }
    } else {
        uint16_t chain[4096];
        int clen = read_chain(parent_cluster, chain, 4096);
        int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
        uint8_t buf[SLYZ_CLUSTER_BYTES];

        for (int i = 0; i < clen && !written; i++) {
            if (!read_cluster(chain[i], buf)) break;
            DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
            for (int e = 0; e < entries_per_cluster && !written; e++) {
                if (cluster_entries[e].type == DIR_EMPTY || cluster_entries[e].type == DIR_DELETED) {
                    cluster_entries[e] = new_entry;
                    write_cluster(chain[i], buf);
                    written = true;
                }
            }
        }

        if (!written) {
            uint16_t new_slot_cluster = alloc_cluster();
            if (new_slot_cluster == FAT_END) return false;
            slyz_memset(buf, 0, sizeof(buf));
            *reinterpret_cast<DirEntry*>(buf) = new_entry;
            write_cluster(new_slot_cluster, buf);
            if (clen > 0) {
                fat_[chain[clen - 1]] = new_slot_cluster;
                fat_[new_slot_cluster] = FAT_END;
                fat_dirty_ = true;
            }
            written = true;
        }
    }

    if (!written) {
        free_chain(new_cluster);
        SLYZ_FPRINTF(stderr, "[SlyzFS] create: parent dir full\n");
        return false;
    }
    return true;
}

bool SlyzFS::remove(const char* path) {
    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return false;

    uint16_t parent_cluster = resolve_dir(parent);
    if (parent_cluster == FAT_END) return false;

    DirEntry parent_de;
    if (!resolve_dir_entry(parent, &parent_de)) return false;
    if (!has_perm(parent_de, g_current_uid, 6)) return false;

    DirEntry* de = find_entry_in_dir(parent_cluster, name);
    if (!de || de->type != DIR_FILE) return false;

    free_chain(de->start_cluster);

    /* 标记 DIR_DELETED —— 和 rmdir 同样的遍历写回逻辑 */
    bool deleted = false;

    if (parent_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        for (uint16_t s = 0; s < sb_.root_sectors && !deleted; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) break;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && !deleted; e++) {
                DirEntry* de2 = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de2->type == DIR_FILE && names_equal(de2->name, name)) {
                    de2->type = DIR_DELETED;
                    slyz_disk_write(lba + s, sector);
                    deleted = true;
                }
            }
        }
    } else {
        uint16_t chain[4096];
        int clen = read_chain(parent_cluster, chain, 4096);
        int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
        uint8_t buf[SLYZ_CLUSTER_BYTES];

        for (int i = 0; i < clen && !deleted; i++) {
            if (!read_cluster(chain[i], buf)) break;
            DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
            for (int e = 0; e < entries_per_cluster && !deleted; e++) {
                if (cluster_entries[e].type == DIR_EMPTY || cluster_entries[e].type == DIR_DELETED) continue;
                if (cluster_entries[e].type == DIR_FILE && names_equal(cluster_entries[e].name, name)) {
                    cluster_entries[e].type = DIR_DELETED;
                    write_cluster(chain[i], buf);
                    deleted = true;
                }
            }
        }
    }

    return deleted;
}

/* ======================= 元数据修改 ======================= */

bool SlyzFS::update_meta(const char* path, uint16_t mode, uint8_t uid,
                         bool set_mode, bool set_uid) {
    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return false;

    uint16_t parent_cluster = resolve_dir(parent);
    if (parent_cluster == FAT_END) return false;

    if (parent_cluster == 0) {
        uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
        uint32_t used_sectors = (sb_.root_entry_count + SLYZ_ENTRIES_PER_SECTOR - 1) / SLYZ_ENTRIES_PER_SECTOR;
        if (used_sectors > sb_.root_sectors) used_sectors = sb_.root_sectors;
        for (uint16_t s = 0; s < used_sectors; s++) {
            uint8_t sector[SLYZ_SECTOR_SIZE];
            if (!slyz_disk_read(lba + s, sector)) return false;
            for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                if (de->type == DIR_EMPTY || de->type == DIR_DELETED) continue;
                if (!names_equal(de->name, name)) continue;
                if (set_mode) de->mode = mode;
                if (set_uid) de->uid = uid;
                return slyz_disk_write(lba + s, sector);
            }
        }
        return false;
    }

    uint16_t chain[4096];
    int clen = read_chain(parent_cluster, chain, 4096);
    int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
    uint8_t buf[SLYZ_CLUSTER_BYTES];

    for (int i = 0; i < clen; i++) {
        if (!read_cluster(chain[i], buf)) return false;
        DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
        for (int e = 0; e < entries_per_cluster; e++) {
            if (entries[e].type == DIR_EMPTY || entries[e].type == DIR_DELETED) continue;
            if (!names_equal(entries[e].name, name)) continue;
            if (set_mode) entries[e].mode = mode;
            if (set_uid) entries[e].uid = uid;
            return write_cluster(chain[i], buf);
        }
    }
    return false;
}

bool SlyzFS::chmod(const char* path, uint16_t mode) {
    if (!path || !*path) return false;

    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return false;

    DirEntry parent_de;
    if (!resolve_dir_entry(parent, &parent_de)) return false;
    /* root 可改任意条目；目录 owner 也可管理自己目录下的条目 */
    if (g_current_uid != 0 && parent_de.uid != (uint8_t)(g_current_uid & 0xFF)) {
        return false;
    }

    return update_meta(path, (uint16_t)(mode & 07777u), 0, true, false);
}

bool SlyzFS::chown(const char* path, uint8_t uid) {
    /* 只允许 root 转移所有权，否则普通用户可以把自己的文件"送给"别人
     * 或反过来抢占别人的文件。 */
    if (g_current_uid != 0) return false;
    return update_meta(path, 0, uid, false, true);
}

/* ======================= 打开/关闭 ======================= */

FileHandle* SlyzFS::open(const char* path, const char* mode) {
    FileHandle* fh = acquire_handle();
    if (!fh) return nullptr;
    slyz_memset(fh, 0, sizeof(*fh));
    fh->is_open = 0;

    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) { release_handle(fh); return nullptr; }

    uint16_t parent_cluster = resolve_dir(parent);
    SLYZ_PRINTF("[open] path='%s' parent='%s' name='%s' parent_cluster=%u\n", path, parent, name, parent_cluster);
    if (parent_cluster == FAT_END) { release_handle(fh); return nullptr; }

    bool truncate = (mode && mode[0] == 'w');
    bool append   = (mode && mode[0] == 'a');

    /* 先解析父目录条目并复制出来，再做权限判断。
     * 注意顺序：resolve_dir_entry 内部会重填 find_entry_in_dir 的静态 scratch，
     * 必须在获取目标文件的 de 之前调用；否则 de 会指向父目录里的其他条目，
     * 导致 fh 的 size / start_cluster / name 全部错位（曾表现为打开文件后读到 0 字节）。 */
    DirEntry parent_de;
    if (!resolve_dir_entry(parent, &parent_de)) { release_handle(fh); return nullptr; }

    DirEntry* de = find_entry_in_dir(parent_cluster, name);

    /* ---- 权限检查 ----
     * 读：文件读位；写（w）：文件写位 + 父目录写/执行（w 模式会 remove+create）；
     * 追加：文件写位；新建：父目录写/执行。root 由 has_perm 内部放行。 */
    if (truncate) {
        if (!has_perm(parent_de, g_current_uid, 6)) { release_handle(fh); return nullptr; }
        if (de && !has_perm(*de, g_current_uid, 2)) { release_handle(fh); return nullptr; }
    } else if (append) {
        if (de && !has_perm(*de, g_current_uid, 2)) { release_handle(fh); return nullptr; }
        if (!de && !has_perm(parent_de, g_current_uid, 6)) { release_handle(fh); return nullptr; }
    } else {
        if (de && !has_perm(*de, g_current_uid, 4)) { release_handle(fh); return nullptr; }
    }

    if (truncate) {
        /* w 模式：如已存在先删，再创建空文件 */
        SLYZ_PRINTF("[open] truncate: existing=%p\n", (void*)de);
        if (de) {
            remove(path);
        }
        if (!create(path)) { release_handle(fh); return nullptr; }
        de = find_entry_in_dir(parent_cluster, name);
        SLYZ_PRINTF("[open] after create, find_entry=%p\n", (void*)de);
        if (!de) { release_handle(fh); return nullptr; }
    } else if (!de) {
        /* r/a 模式但文件不存在 */
        if (mode && mode[0] == 'r') { release_handle(fh); return nullptr; }
        /* a 模式不存在也创建 */
        if (!create(path)) { release_handle(fh); return nullptr; }
        de = find_entry_in_dir(parent_cluster, name);
        if (!de) { release_handle(fh); return nullptr; }
    }

    fh->first_cluster = de->start_cluster;
    fh->current_cluster = fh->first_cluster;
    fh->parent_cluster = parent_cluster;
    fh->size = de->size;
    fh->position = append ? fh->size : 0;
    fh->is_open = 1;
    fh->is_dir = (de->type == DIR_DIR) ? 1 : 0;
slyz_memcpy(fh->name, de->name, SLYZ_NAME_MAX);

    return fh;
}

void SlyzFS::close(FileHandle* fh) {
    if (!fh) return;
    fh->is_open = 0;

    /* 把 fh->size 同步回父目录条目 */
    if (!fh->is_dir && fh->parent_cluster != FAT_END) {
        /* 遍历父目录簇/根目录，找到匹配 fh->name 的条目，更新 size */
        bool updated = false;
        if (fh->parent_cluster == 0) {
            /* 根目录 */
            uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
            for (uint16_t s = 0; s < sb_.root_sectors && !updated; s++) {
                uint8_t sector[SLYZ_SECTOR_SIZE];
                if (!slyz_disk_read(lba + s, sector)) break;
                for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR && !updated; e++) {
                    DirEntry* de = reinterpret_cast<DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
                    if (de->type != DIR_EMPTY && de->type != DIR_DELETED &&
                        slyz_memcmp(de->name, fh->name, SLYZ_NAME_MAX) == 0) {
                        de->size = fh->size;
                        slyz_disk_write(lba + s, sector);
                        updated = true;
                    }
                }
            }
        } else {
            /* 子目录 */
            uint16_t chain[4096];
            int clen = read_chain(fh->parent_cluster, chain, 4096);
            int entries_per_cluster = sb_.cluster_size * (SLYZ_SECTOR_SIZE / SLYZ_DIRENTRY_SIZE);
            uint8_t buf[SLYZ_CLUSTER_BYTES];

            for (int i = 0; i < clen && !updated; i++) {
                if (!read_cluster(chain[i], buf)) break;
                DirEntry* cluster_entries = reinterpret_cast<DirEntry*>(buf);
                for (int e = 0; e < entries_per_cluster && !updated; e++) {
                    if (cluster_entries[e].type != DIR_EMPTY && cluster_entries[e].type != DIR_DELETED &&
                        slyz_memcmp(cluster_entries[e].name, fh->name, SLYZ_NAME_MAX) == 0) {
                        cluster_entries[e].size = fh->size;
                        write_cluster(chain[i], buf);
                        updated = true;
                    }
                }
            }
        }
        if (!updated) {
            SLYZ_FPRINTF(stderr, "[SlyzFS] close: dir entry not found for '%s'\n", fh->name);
        }
    }

    release_handle(fh);
}

/* ======================= read / write / seek ======================= */

size_t SlyzFS::read(FileHandle* fh, uint8_t* buf, size_t n) {
    if (!fh || !fh->is_open || fh->is_dir || n == 0) return 0;

    uint32_t cluster_bytes = sb_.cluster_size * SLYZ_SECTOR_SIZE;

    /* 根据 position 算出起始簇号和簇内偏移 */
    uint32_t abs_pos = fh->position;
    if (abs_pos >= fh->size) return 0;

    uint32_t rel_cluster_idx = abs_pos / cluster_bytes;
    uint32_t offset_in_cluster = abs_pos % cluster_bytes;

    /* 按链跳到目标簇 */
    uint16_t target_cluster = fh->first_cluster;
    for (uint32_t i = 0; i < rel_cluster_idx && target_cluster != FAT_END; i++) {
        target_cluster = fat_[target_cluster];
    }
    if (target_cluster == FAT_END) return 0;

    /* 读簇 */
    uint8_t cluster_buf[SLYZ_CLUSTER_BYTES];
    if (!read_cluster(target_cluster, cluster_buf)) return 0;

    /* 实际可读 min(n, size - position, cluster_bytes - offset_in_cluster) */
    uint32_t available = fh->size - abs_pos;
    if (n > available) n = available;
    uint32_t in_cluster = cluster_bytes - offset_in_cluster;
    if (n > in_cluster) n = in_cluster;

    slyz_memcpy(buf, cluster_buf + offset_in_cluster, n);

    fh->position += n;
    fh->current_cluster = target_cluster;

    return n;
}

size_t SlyzFS::write(FileHandle* fh, const uint8_t* buf, size_t n) {
    if (!fh || !fh->is_open || fh->is_dir || n == 0) return 0;

    uint32_t cluster_bytes = sb_.cluster_size * SLYZ_SECTOR_SIZE;
    uint32_t abs_pos = fh->position;

    uint32_t offset_in_first = abs_pos % cluster_bytes;

    /* 跳到当前写入位置的簇（不够就扩展 FAT 链） */
    uint16_t cur_cluster = fh->first_cluster;
    uint32_t cur_start = 0;
    while (cur_start + cluster_bytes <= abs_pos && cur_cluster != FAT_END) {
        if (fat_[cur_cluster] == FAT_END) {
            /* 链不够了，扩展 */
            uint16_t next = alloc_cluster();
            if (next == FAT_END) return 0;
            fat_[cur_cluster] = next;
            fat_dirty_ = true;
        }
        cur_start += cluster_bytes;
        cur_cluster = fat_[cur_cluster];
    }
    if (cur_cluster == FAT_END) {
        /* first_cluster 对应的 FAT 链已断（不应发生） */
        return 0;
    }

    /* 逐簇写入 */
    size_t written = 0;
    size_t remaining = n;
    uint32_t offset = offset_in_first;

    while (remaining > 0) {
        /* 如果需要写满当前簇尾部但剩余数据更大，先读-改-写完整簇 */
        uint8_t cluster_buf[SLYZ_CLUSTER_BYTES];

        /* 判断本簇是否需要扩展 —— 如果写完本簇后还不够，且 FAT 没有 next，
         * 那我们需要提前挂下一个簇。 */
        if (offset + remaining > cluster_bytes) {
            /* 需要跨簇 */
            if (fat_[cur_cluster] == FAT_END || fat_[cur_cluster] == FAT_FREE) {
                uint16_t next = alloc_cluster();
                if (next == FAT_END) break;
                fat_[cur_cluster] = next;
                fat_[next] = FAT_END;
                fat_dirty_ = true;
            }
        }

        /* 如果是最后一个簇且有未写部分，先读完整簇（保留旧数据） */
        bool partial_read = (offset != 0) || (remaining < cluster_bytes - offset);
        if (partial_read) {
            if (!read_cluster(cur_cluster, cluster_buf)) break;
        } else {
            slyz_memset(cluster_buf, 0, cluster_bytes);
        }

        size_t chunk = remaining;
        if (chunk > cluster_bytes - offset) chunk = cluster_bytes - offset;
        slyz_memcpy(cluster_buf + offset, buf + written, chunk);

        if (!write_cluster(cur_cluster, cluster_buf)) break;

        written += chunk;
        remaining -= chunk;
        offset = 0;

        if (fat_[cur_cluster] == FAT_END) break; /* 满了 */
        cur_cluster = fat_[cur_cluster];
    }

    fh->position += written;
    if (fh->position > fh->size) fh->size = fh->position;
    fh->current_cluster = cur_cluster;

    return written;
}

bool SlyzFS::seek(FileHandle* fh, uint32_t offset) {
    if (!fh || !fh->is_open || fh->is_dir) return false;
    fh->position = offset;
    return true;
}

/* ======================= stat ======================= */

DirEntry* SlyzFS::stat(const char* path) {
    char parent[512], name[SLYZ_NAME_MAX];
    if (!split_path(path, parent, name)) return nullptr;

    uint16_t parent_cluster = resolve_dir(parent);
    if (parent_cluster == FAT_END) return nullptr;

    DirEntry* de = find_entry_in_dir(parent_cluster, name);
    if (!de || de->type == DIR_DELETED) return nullptr;
    return de;
}

/* ======================= 调试 ======================= */

void SlyzFS::debug_print_fat() const {
    if (!fat_) { SLYZ_PRINTF("FAT not loaded\n"); return; }
    int non_free = 0;
    for (uint32_t i = 2; i < fat_size_; i++) {
        if (fat_[i] != FAT_FREE) non_free++;
    }
    SLYZ_PRINTF("FAT: %u entries, %d non-free (%.1f%%)\n",
                fat_size_, non_free, 100.0 * non_free / (fat_size_ - 2));
}

void SlyzFS::debug_print_root() const {
    /* 直接逐扇区遍历根目录打印，避免在栈上开 DirEntry[4096]（256KB）。
     * 根目录区是连续的 sb_.root_sectors 个扇区，无需经 list_dir 中转。 */
    uint32_t lba = SLYZ_FS_OFFSET + 1 + sb_.fat_sectors;
    int total = 0;
    for (uint16_t s = 0; s < sb_.root_sectors; s++) {
        uint8_t sector[SLYZ_SECTOR_SIZE];
        if (!slyz_disk_read(lba + s, sector)) break;
        for (int e = 0; e < SLYZ_ENTRIES_PER_SECTOR; e++) {
            const DirEntry* de = reinterpret_cast<const DirEntry*>(sector + e * SLYZ_DIRENTRY_SIZE);
            if (de->type == DIR_EMPTY || de->type == DIR_DELETED) continue;
            SLYZ_PRINTF("  [%c] %s  cluster=%u size=%u\n",
                        de->type == DIR_DIR ? 'D' : 'F',
                        de->name, de->start_cluster, de->size);
            total++;
        }
    }
    SLYZ_PRINTF("Root dir (%d entries):\n", total);
}

} /* namespace slyzfs */
