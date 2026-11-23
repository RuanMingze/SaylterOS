/* ============================================================
 * slyzfs2_bridge.c - SlyzFS v2 模块的内核桥接层
 * ------------------------------------------------------------
 * 文件系统本体是 src/fs/slyzfs2.slyz3 编译出的 .slys 模块，
 * 内核不包含任何 FS 实现，只做三件事：
 *   1. 从启动盘固定原始 LBA 加载模块（此时还没有文件系统）；
 *   2. 提供 SlysFsApi（扇区读写、容量、日志、flush）；
 *   3. 把原有的 slyzfs_* C API 转发成 SFS_OP_* 调用。
 *
 * 盘符路由：
 *   "C:\..." -> SYSTEM，可写
 *   "R:\..." -> RECOVERY，正常系统只读，RE 中可写
 *   "D:\..." -> DATA，可写
 *   没有盘符的相对路径默认走 C:（内核内部路径都这样）。
 * ============================================================ */

#include <stdint.h>
#include <stdarg.h>

#include "../include/slys.h"
#include "../include/slysfs.h"
#include "../include/slyzfs_part.h"
#include "../kernel/blk.h"

extern int  printk(const char *fmt, ...);
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

/* 模块在镜像中的位置：内核载荷结束（约 2.6MB）之后、
 * 文件系统分区（LBA8192）之前。 */
#define SLYZFS_MODULE_LBA 6144u

typedef void *(*SlysFsStartFn)(const struct SlysFsApi *api);

static SlysFsCall g_call;
static int g_loaded;
static int g_quiet;
static int g_recovery_mode;

/* ============================================================
 * 内核服务
 * ============================================================ */

/* ---- 顺序预读缓存 ----
 * 模块按 512B 请求文件内容，而 ATA PIO 每次命令握手都有固定开销，
 * 顺序读大文件（字体、壁纸、驱动镜像）会被拖到秒级。这里把相邻
 * 的 128 个扇区一次读进 64KB 缓冲，后续请求直接内存拷贝。
 * 缓冲按 4KB 对齐；DMA 层会按 64KB 边界拆成两条 PRDT。 */
#define RC_SECTORS 128u

static uint8_t  g_rc_buf[RC_SECTORS * 512u] __attribute__((aligned(4096)));
static uint32_t g_rc_lba = 0;
static uint32_t g_rc_count = 0;
static int      g_rc_valid = 0;
static uint32_t g_rc_hits = 0;
static uint32_t g_rc_misses = 0;

void slyzfs_cache_stats(uint32_t *hits, uint32_t *misses) {
    if (hits) *hits = g_rc_hits;
    if (misses) *misses = g_rc_misses;
}

static void rc_invalidate(void) {
    g_rc_valid = 0;
}

static int api_disk_read(uint32_t lba, void *buf) {
    uint8_t *dst = (uint8_t *)buf;

    if (g_rc_valid && lba >= g_rc_lba && lba < g_rc_lba + g_rc_count) {
        g_rc_hits++;
        const uint8_t *src = g_rc_buf + (lba - g_rc_lba) * 512u;
        for (uint32_t i = 0; i < 512u; i++) dst[i] = src[i];
        return 1;
    }

    uint32_t total = blk_total_sectors();
    if (lba >= total) return blk_read(lba, dst);
    if (total == 0) return 0;

    g_rc_misses++;
    uint32_t n = RC_SECTORS;
    if (lba + n > total) n = total - lba;
    if (!blk_read_multi(lba, n, g_rc_buf)) {
        rc_invalidate();
        return blk_read(lba, dst);
    }

    g_rc_lba = lba;
    g_rc_count = n;
    g_rc_valid = 1;

    for (uint32_t i = 0; i < 512u; i++) dst[i] = g_rc_buf[i];
    return 1;
}

static int api_disk_write(uint32_t lba, const void *buf) {
    rc_invalidate();
    return blk_write(lba, (const uint8_t *)buf);
}

static uint32_t api_total_sectors(void) {
    return blk_total_sectors();
}

static void api_log(const char *s) {
    kserial_puts(s);
}

static int api_flush(void) {
    rc_invalidate();
    return blk_flush();
}

static const struct SlysFsApi g_api = {
    SLYSFS_ABI,
    0,
    api_disk_read,
    api_disk_write,
    api_total_sectors,
    api_log,
    api_flush
};

/* ============================================================
 * 加载与盘符
 * ============================================================ */

static int module_init(void) {
    if (g_loaded) return 1;

    void *entry = 0;
    char name[24];
    if (!slys_load_raw(SLYZFS_MODULE_LBA, &entry, name, (int)sizeof(name))) {
        printk("[SlyzFS] cannot load filesystem module at LBA %u\n",
               (unsigned)SLYZFS_MODULE_LBA);
        return 0;
    }
    SlysFsStartFn start = (SlysFsStartFn)entry;
    g_call = (SlysFsCall)start(&g_api);
    if (!g_call) {
        printk("[SlyzFS] module returned no entry\n");
        return 0;
    }
    g_loaded = 1;
    return 1;
}

/* 返回盘符索引，并把 path 指针推进到盘符之后。 */
static int pick_drive(const char **path) {
    const char *p = *path;
    int drive = 0;
    if (p && p[0] && p[1] == ':') {
        char c = (char)(p[0] | 0x20);
        if (c == 'r') drive = 1;
        else if (c == 'd') drive = 2;
        p += 2;
        if (*p == '\\' || *p == '/') p++;
    }
    *path = p ? p : "";
    return drive;
}

static int volume_writable(int drive) {
    if (g_recovery_mode) {
        /* 恢复环境：R: 可写，C: 只读，D: 可写 */
        return drive != 0;
    }
    /* 正常系统：R: 只读 */
    return drive != 1;
}

/* ============================================================
 * 生命周期
 * ============================================================ */

void slyzfs_set_quiet(int quiet) {
    g_quiet = (quiet != 0);
}

void slyzfs_set_recovery_mode(int on) {
    g_recovery_mode = (on != 0);
}

int slyzfs_bridge_init(void) {
    return module_init();
}

int slyzfs_try_mount(void) {
    if (!module_init()) return 0;
    int64_t seen = g_call(SFS_OP_MOUNT_ALL, 0, 0, 0, 0);
    if (seen == 0) {
        printk("[SlyzFS] module could not mount any volume\n");
        return 0;
    }
    printk("[SlyzFS] v2 mounted (C=%d R=%d D=%d)\n",
           (int)(seen & 1), (int)((seen >> 1) & 1), (int)((seen >> 2) & 1));
    return 1;
}

int slyzfs_format_and_mount(void) {
    if (!module_init()) return 0;
    if (g_call(SFS_OP_FORMAT_DISK, 0, 0, 0, 0) == 0) {
        printk("[SlyzFS] format failed\n");
        return 0;
    }
    return slyzfs_try_mount();
}

void slyzfs_unmount(void) {
    if (!g_loaded) return;
    g_call(SFS_OP_FLUSH, 0, 0, 0, 0);
    g_call(SFS_OP_UNMOUNT_VOLUME, 0, 0, 0, 0);
}

int slyzfs_flush(void) {
    if (!g_loaded) return 0;
    return (int)g_call(SFS_OP_FLUSH, 0, 0, 0, 0);
}

/* ============================================================
 * 目录
 * ============================================================ */

int slyzfs_mkdir(const char *path) {
    if (!g_loaded || !path) return 0;
    int drive = pick_drive(&path);
    if (!volume_writable(drive)) return 0;
    return (int)g_call(SFS_OP_MKDIR, drive, (int64_t)(uintptr_t)path, 0, 0);
}

int slyzfs_rmdir(const char *path) {
    if (!g_loaded || !path) return 0;
    int drive = pick_drive(&path);
    if (!volume_writable(drive)) return 0;
    return (int)g_call(SFS_OP_RMDIR, drive, (int64_t)(uintptr_t)path, 0, 0);
}

int slyzfs_list_dir(const char *path, struct SlysDirEntry *out, int out_max) {
    if (!g_loaded || !out || out_max <= 0) return -1;
    const char *rel = path ? path : "";
    int drive = pick_drive(&rel);
    return (int)g_call(SFS_OP_LIST, drive, (int64_t)(uintptr_t)rel,
                       (int64_t)(uintptr_t)out, out_max);
}

int slyzfs_stat(const char *path, struct SlysDirEntry *out) {
    if (!g_loaded || !out || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    return (int)g_call(SFS_OP_STAT, drive, (int64_t)(uintptr_t)rel,
                       (int64_t)(uintptr_t)out, 0);
}

/* ============================================================
 * 文件
 * ============================================================ */

int slyzfs_create(const char *path) {
    if (!g_loaded || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    if (!volume_writable(drive)) return 0;
    return (int)g_call(SFS_OP_CREATE, drive, (int64_t)(uintptr_t)rel, 0, 0);
}

int slyzfs_remove(const char *path) {
    if (!g_loaded || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    if (!volume_writable(drive)) return 0;
    return (int)g_call(SFS_OP_REMOVE, drive, (int64_t)(uintptr_t)rel, 0, 0);
}

/* 句柄直接编码成指针：模块返回 1..32，桥接层加上一个非零基址，
 * 避免与 nullptr 混淆；所有读写再减回去。 */
#define HANDLE_BIAS 0x1000

void *slyzfs_open(const char *path, const char *mode) {
    if (!g_loaded || !path || !mode) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    int m = 0;
    if (mode[0] == 'w') m = 1;
    else if (mode[0] == 'a') m = 2;
    if (m != 0 && !volume_writable(drive)) return 0;
    int64_t h = g_call(SFS_OP_OPEN, drive, (int64_t)(uintptr_t)rel, m, 0);
    if (h <= 0) return 0;
    return (void *)(uintptr_t)(h + HANDLE_BIAS);
}

int slyzfs_read(void *fh, uint8_t *buf, int n) {
    if (!g_loaded || !fh || !buf || n <= 0) return 0;
    if (g_recovery_mode) {
        /* 恢复环境允许读 C: 与 R:，模块本身不区分，这里不做限制。 */
    }
    int64_t h = (int64_t)(uintptr_t)fh - HANDLE_BIAS;
    return (int)g_call(SFS_OP_READ, h, (int64_t)(uintptr_t)buf, n, 0);
}

int slyzfs_write(void *fh, const uint8_t *buf, int n) {
    if (!g_loaded || !fh || !buf || n <= 0) return 0;
    int64_t h = (int64_t)(uintptr_t)fh - HANDLE_BIAS;
    return (int)g_call(SFS_OP_WRITE, h, (int64_t)(uintptr_t)buf, n, 0);
}

int slyzfs_seek(void *fh, uint32_t offset) {
    if (!g_loaded || !fh) return 0;
    int64_t h = (int64_t)(uintptr_t)fh - HANDLE_BIAS;
    return (int)g_call(SFS_OP_SEEK, h, offset, 0, 0);
}

void slyzfs_close(void *fh) {
    if (!g_loaded || !fh) return;
    int64_t h = (int64_t)(uintptr_t)fh - HANDLE_BIAS;
    g_call(SFS_OP_CLOSE, h, 0, 0, 0);
}

/* 同卷改名/移动：只改父目录索引项，1 秒内完成，与文件大小无关。 */
int slyzfs_rename(const char *old_path, const char *new_path) {
    if (!g_loaded || !old_path || !new_path) return 0;
    const char *a = old_path;
    int da = pick_drive(&a);
    const char *b = new_path;
    int db = pick_drive(&b);
    if (da != db) return 0;
    if (!volume_writable(da)) return 0;
    return (int)g_call(SFS_OP_RENAME, da, (int64_t)(uintptr_t)a,
                       (int64_t)(uintptr_t)b, 0);
}

/* 跨卷（或目标已存在时）的复制：分块读写，返回复制的字节数。 */
int slyzfs_copy_file(const char *src, const char *dst) {
    if (!g_loaded || !src || !dst) return -1;

    void *in = slyzfs_open(src, "r");
    if (!in) return -1;
    void *out = slyzfs_open(dst, "w");
    if (!out) {
        slyzfs_close(in);
        return -1;
    }

    static uint8_t copy_buf[65536];
    int total = 0;
    for (;;) {
        int n = slyzfs_read(in, copy_buf, (int)sizeof(copy_buf));
        if (n <= 0) break;
        int w = slyzfs_write(out, copy_buf, n);
        if (w != n) {
            total = -1;
            break;
        }
        total += w;
    }
    slyzfs_close(out);
    slyzfs_close(in);
    return total;
}

/* 剪切/移动：优先 O(1) 改名；跨卷回退为复制后删除。 */
int slyzfs_move_file(const char *src, const char *dst) {
    if (slyzfs_rename(src, dst)) return 0;
    if (slyzfs_copy_file(src, dst) < 0) return -1;
    return slyzfs_remove(src) ? 0 : -1;
}

/* ============================================================
 * 容量与元数据
 * ============================================================ */

int slyzfs_usage(uint32_t *total_clusters, uint32_t *used_clusters,
                 uint32_t *cluster_bytes) {
    if (!g_loaded) return 0;
    uint32_t info[3] = { 0, 0, 0 };
    if (g_call(SFS_OP_USAGE, 0, (int64_t)(uintptr_t)info, 0, 0) == 0) {
        return 0;
    }
    if (total_clusters) *total_clusters = info[0];
    if (used_clusters) *used_clusters = info[1];
    if (cluster_bytes) *cluster_bytes = info[2];
    return 1;
}

/* 权限层：模块保存 mode / uid / 保护位，桥接层只做参数校验。 */
static uint16_t g_current_uid;

void slyzfs_set_uid(int uid) {
    if (uid < 0) uid = 0;
    if (uid > 255) uid = 255;
    g_current_uid = (uint16_t)uid;
}

int slyzfs_get_uid(void) {
    return (int)g_current_uid;
}

int slyzfs_chmod(const char *path, int mode) {
    if (!g_loaded || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    if (!volume_writable(drive)) return 0;
    if (mode < 0 || mode > 07777) return 0;
    return (int)g_call(SFS_OP_CHMOD, drive, (int64_t)(uintptr_t)rel, mode, 0);
}

int slyzfs_chown(const char *path, int uid) {
    if (!g_loaded || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    if (!volume_writable(drive)) return 0;
    if (uid < 0 || uid > 255) return 0;
    return (int)g_call(SFS_OP_CHOWN, drive, (int64_t)(uintptr_t)rel, uid, 0);
}

int slyzfs_set_protected(const char *path, int on) {
    if (!g_loaded || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    if (!volume_writable(drive)) return 0;
    return (int)g_call(SFS_OP_PROTECT, drive, (int64_t)(uintptr_t)rel,
                       on != 0, 0);
}

int slyzfs_is_protected(const char *path) {
    if (!g_loaded || !path) return 0;
    const char *rel = path;
    int drive = pick_drive(&rel);
    return (int)g_call(SFS_OP_IS_PROTECTED, drive, (int64_t)(uintptr_t)rel, 0, 0);
}

int slyzfs_fault_read(void *out) {
    if (!g_loaded || !out) return 0;
    return (int)g_call(SFS_OP_FAULT_READ, 0, (int64_t)(uintptr_t)out, 0, 0);
}

int slyzfs_fault_write(const void *in) {
    if (!g_loaded || !in) return 0;
    return (int)g_call(SFS_OP_FAULT_WRITE, 0, (int64_t)(uintptr_t)in, 0, 0);
}

/* ============================================================
 * 分区 / 盘符管理
 * ------------------------------------------------------------
 * 分区表格式见 slysfs.h。这里只改启动盘 LBA8192/8193，
 * 卷的格式化与挂载继续交给模块（SFS_OP_FORMAT_VOLUME / MOUNT_VOLUME），
 * 避免在内核里出现第二份文件系统实现。
 * ============================================================ */

static uint32_t spt_crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            if (c & 1u) c = (c >> 1) ^ 0xEDB88320u;
            else c >>= 1;
        }
    }
    return c ^ 0xFFFFFFFFu;
}

static int spt_valid(const struct SlysPartTable *t) {
    const uint8_t *p = (const uint8_t *)t;
    if (p[0] != 'S' || p[1] != 'L' || p[2] != 'Y' || p[3] != 'S' ||
        p[4] != 'P' || p[5] != 'T' || p[6] != '2' || p[7] != 0) {
        return 0;
    }
    if (t->version != 1) return 0;
    if (t->entry_count > 4) return 0;
    return t->crc32 == spt_crc32(p, 508);
}

/* 读主表；主表损坏时回退备份。 */
static int spt_read(struct SlysPartTable *t) {
    if (!t) return 0;
    if (!blk_read(SLYSPT_LBA_PRIMARY, (uint8_t *)t)) return 0;
    if (spt_valid(t)) return 1;
    if (!blk_read(SLYSPT_LBA_BACKUP, (uint8_t *)t)) return 0;
    return spt_valid(t);
}

static int spt_write(struct SlysPartTable *t) {
    if (!t) return 0;
    t->crc32 = spt_crc32((const uint8_t *)t, 508);
    if (!blk_write(SLYSPT_LBA_PRIMARY, (const uint8_t *)t)) return 0;
    if (!blk_write(SLYSPT_LBA_BACKUP, (const uint8_t *)t)) return 0;
    blk_flush();
    return 1;
}

static void part_set_label(char *dst, const char *src, int cap) {
    for (int i = 0; i < cap; i++) dst[i] = 0;
    if (!src) return;
    for (int i = 0; i < cap - 1 && src[i]; i++) dst[i] = src[i];
}

static int part_mounted(int drive) {
    uint32_t info[4] = { 0, 0, 0, 0 };
    return g_call(SFS_OP_VOLUME_INFO, drive,
                  (int64_t)(uintptr_t)info, 0, 0) != 0;
}

int slyzfs_part_list(struct SlyzPartInfo *out, int max) {
    if (!g_loaded || !out || max <= 0) return -1;

    struct SlysPartTable t;
    if (!spt_read(&t)) return -1;

    int n = 0;
    int count = t.entry_count;
    if (count > 4) count = 4;
    for (int i = 0; i < count && n < max; i++) {
        const struct SlysPartEntry *e = &t.entries[i];
        if (e->type < 1 || e->type > 3) continue;
        out[n].drive = e->type - 1;
        out[n].type = e->type;
        out[n].start_lba = e->start_lba;
        out[n].sectors = e->sector_count;
        out[n].mounted = part_mounted(e->type - 1);
        part_set_label(out[n].label, e->label, (int)sizeof(out[n].label));
        out[n].label[sizeof(out[n].label) - 1] = 0;
        n++;
    }
    return n;
}

/* 在 [SLYSPT_FIRST_PART, total-margin) 中找一块空闲区间。
 * used[] 已按 start 排序。sectors==0 表示自动取最大空闲区的一半。 */
static int part_pick_free(const struct SlysPartTable *t, uint32_t want,
                          uint32_t *out_start, uint32_t *out_count) {
    uint32_t total = blk_total_sectors();
    if (total <= SLYSPT_FIRST_PART + 4096u) return 0;

    uint32_t limit = total;
    if (limit > 2048u) limit -= 2048u;   /* 盘尾留 1MB，避免越界写 */

    uint32_t start[4];
    uint32_t end[4];
    int n = 0;
    int count = t->entry_count;
    if (count > 4) count = 4;
    for (int i = 0; i < count; i++) {
        const struct SlysPartEntry *e = &t->entries[i];
        if (e->type < 1 || e->type > 3) continue;
        uint32_t s = e->start_lba;
        uint32_t c = e->sector_count;
        if (c == 0 || s < SLYSPT_FIRST_PART) continue;
        uint32_t en = (s + c < s) ? limit : s + c;
        if (en > limit) en = limit;
        start[n] = s;
        end[n] = en;
        n++;
    }
    for (int i = 1; i < n; i++) {
        uint32_t s = start[i], e = end[i];
        int j = i - 1;
        while (j >= 0 && start[j] > s) {
            start[j + 1] = start[j];
            end[j + 1] = end[j];
            j--;
        }
        start[j + 1] = s;
        end[j + 1] = e;
    }

    uint32_t cur = SLYSPT_FIRST_PART;
    uint32_t best_start = 0, best_count = 0;

    for (int i = 0; i <= n; i++) {
        uint32_t gap_end = (i < n) ? start[i] : limit;
        if (gap_end > cur && gap_end - cur > best_count) {
            best_start = cur;
            best_count = gap_end - cur;
        }
        if (i < n && end[i] > cur) cur = end[i];
    }

    if (best_count < 4096u) return 0;

    uint32_t take = want;
    if (take == 0) {
        take = best_count / 2u;
        if (take > 262144u) take = 262144u;   /* 自动最多 128MB */
    }
    if (take > best_count) take = best_count;
    take &= ~7u;                              /* 8 扇区对齐（4KB） */
    if (take < 4096u) return 0;

    *out_start = best_start;
    *out_count = take;
    return 1;
}

int slyzfs_part_add(int drive, uint32_t sectors, const char *label) {
    if (!g_loaded || drive < 0 || drive > 2) return -1;

    struct SlysPartTable t;
    if (!spt_read(&t)) {
        printk("[PART] partition table is invalid\n");
        return -1;
    }

    for (int i = 0; i < 4; i++) {
        if (t.entries[i].type == drive + 1) {
            printk("[PART] drive letter already in use\n");
            return -1;
        }
    }
    if (t.entries[drive].type != 0) {
        printk("[PART] slot %d already holds another partition\n", drive);
        return -1;
    }

    uint32_t start = 0, count = 0;
    if (!part_pick_free(&t, sectors, &start, &count)) {
        printk("[PART] no free space for a new partition\n");
        return -1;
    }

    static const char *def_labels[3] = { "SYSTEM", "RECOVERY", "DATA" };
    const char *use_label = (label && *label) ? label : def_labels[drive];

    struct SlysPartEntry *e = &t.entries[drive];
    e->type = (uint8_t)(drive + 1);
    e->flags = (drive == 0) ? 1 : (drive == 1 ? 2 : 0);
    e->reserved0 = 0;
    e->start_lba = start;
    e->sector_count = count;
    part_set_label(e->label, use_label, (int)sizeof(e->label));
    e->serial = 0x534C0000u | (uint32_t)(drive + 1);

    int old_count = t.entry_count;
    if (t.entry_count < drive + 1) t.entry_count = (uint8_t)(drive + 1);

    if (!spt_write(&t)) {
        printk("[PART] cannot write partition table\n");
        return -1;
    }

    int64_t rc = g_call(SFS_OP_FORMAT_VOLUME, drive, start, count,
                        (int64_t)(uintptr_t)use_label);
    if (rc == 0) {
        /* 格式化失败：撤销表项，避免留下一个不可挂载的分区。 */
        t.entries[drive].type = 0;
        t.entries[drive].start_lba = 0;
        t.entries[drive].sector_count = 0;
        t.entries[drive].label[0] = 0;
        t.entry_count = (uint8_t)old_count;
        spt_write(&t);
        printk("[PART] format failed, entry rolled back\n");
        return -1;
    }

    if (g_call(SFS_OP_MOUNT_VOLUME, drive, start, count, 0) == 0) {
        printk("[PART] volume was formatted but mount failed\n");
        return -1;
    }
    blk_flush();
    return 0;
}

int slyzfs_part_delete(int drive) {
    if (!g_loaded || drive < 0 || drive > 2) return -1;
    if (drive == 0) {
        printk("[PART] refusing to delete the boot volume C:\n");
        return -1;
    }

    struct SlysPartTable t;
    if (!spt_read(&t)) return -1;
    if (t.entries[drive].type == 0) return -1;

    g_call(SFS_OP_UNMOUNT_VOLUME, drive, 0, 0, 0);

    t.entries[drive].type = 0;
    t.entries[drive].flags = 0;
    t.entries[drive].start_lba = 0;
    t.entries[drive].sector_count = 0;
    part_set_label(t.entries[drive].label, "", (int)sizeof(t.entries[drive].label));
    t.entries[drive].serial = 0;

    if (!spt_write(&t)) return -1;
    blk_flush();
    return 0;
}

int slyzfs_part_format(int drive, const char *label) {
    if (!g_loaded || drive < 0 || drive > 2) return -1;

    struct SlysPartTable t;
    if (!spt_read(&t)) return -1;
    struct SlysPartEntry *e = &t.entries[drive];
    if (e->type < 1 || e->type > 3) return -1;

    const char *use_label = (label && *label) ? label : e->label;
    if (!use_label || !*use_label) use_label = "VOLUME";

    g_call(SFS_OP_UNMOUNT_VOLUME, drive, 0, 0, 0);
    int64_t rc = g_call(SFS_OP_FORMAT_VOLUME, drive, e->start_lba,
                        e->sector_count, (int64_t)(uintptr_t)use_label);
    if (rc == 0) return -1;

    part_set_label(e->label, use_label, (int)sizeof(e->label));
    if (!spt_write(&t)) return -1;

    if (g_call(SFS_OP_MOUNT_VOLUME, drive, e->start_lba,
               e->sector_count, 0) == 0) {
        return -1;
    }
    blk_flush();
    return 0;
}

int slyzfs_part_mount(int drive) {
    if (!g_loaded || drive < 0 || drive > 2) return -1;

    struct SlysPartTable t;
    if (!spt_read(&t)) return -1;
    struct SlysPartEntry *e = &t.entries[drive];
    if (e->type < 1 || e->type > 3) return -1;

    if (g_call(SFS_OP_MOUNT_VOLUME, drive, e->start_lba,
               e->sector_count, 0) == 0) {
        return -1;
    }
    return 0;
}

int slyzfs_part_unmount(int drive) {
    if (!g_loaded || drive < 0 || drive > 2) return -1;
    /* C: 是正在运行的系统卷：卸载后所有文件操作都会失败。
     * 关机流程走 slyzfs_unmount()，不经过这里。 */
    if (drive == 0) {
        printk("[PART] refusing to unmount the boot volume C:\n");
        return -1;
    }
    g_call(SFS_OP_FLUSH, 0, 0, 0, 0);
    g_call(SFS_OP_UNMOUNT_VOLUME, drive, 0, 0, 0);
    blk_flush();
    return 0;
}

/* ============================================================
 * 默认目录骨架
 * ============================================================ */

int slyzfs_mklazy(void) {
    if (!g_loaded) return 0;

    slyzfs_mkdir("home");
    slyzfs_mkdir("usr");
    slyzfs_mkdir("tmp");
    slyzfs_mkdir("etc");
    slyzfs_mkdir("system");
    slyzfs_mkdir("system\\apps");
    slyzfs_mkdir("system\\drivers");
    slyzfs_mkdir("system\\fonts");
    slyzfs_mkdir("system\\logs");
    slyzfs_mkdir("system\\wallpapers");
    slyzfs_mkdir("system\\avatars");
    slyzfs_mkdir("usr\\bin");
    slyzfs_mkdir("home\\root");
    slyzfs_mkdir("home\\user");

    void *fh = slyzfs_open("README.txt", "w");
    if (fh) {
        const char *msg = "Welcome to SaylterOS v0.4\r\nSlyzFS v2 (MFT + journal) is ready.\r\n";
        int n = 0;
        while (msg[n]) n++;
        slyzfs_write(fh, (const uint8_t *)msg, n);
        slyzfs_close(fh);
    }
    fh = slyzfs_open("home\\root\\profile.txt", "w");
    if (fh) {
        const char *msg = "# SaylterOS root profile\r\n# SlyzFS v2\r\n";
        int n = 0;
        while (msg[n]) n++;
        slyzfs_write(fh, (const uint8_t *)msg, n);
        slyzfs_close(fh);
    }
    return 1;
}

/* ============================================================
 * swap 页文件
 * ============================================================ */

static void *g_swap_fh;

int slyzfs_swap_attach(void) {
    if (!g_loaded) return 0;
    if (g_swap_fh) return 1;
    g_swap_fh = slyzfs_open("etc\\pagefile.os", "w");
    if (!g_swap_fh) return 0;
    return 1;
}

int slyzfs_swap_write(uint32_t offset, const uint8_t *buf, int n) {
    if (!g_swap_fh || !buf || n <= 0) return 0;
    if (!slyzfs_seek(g_swap_fh, offset)) return 0;
    return slyzfs_write(g_swap_fh, buf, n);
}

int slyzfs_swap_read(uint32_t offset, uint8_t *buf, int n) {
    if (!g_swap_fh || !buf || n <= 0) return 0;
    if (!slyzfs_seek(g_swap_fh, offset)) return 0;
    int total = 0;
    while (total < n) {
        int got = slyzfs_read(g_swap_fh, buf + total, n - total);
        if (got <= 0) break;
        total += got;
    }
    return total;
}

uint32_t slyzfs_swap_bytes(void) {
    struct SlysDirEntry ent;
    if (!g_loaded) return 0;
    if (!slyzfs_stat("etc\\pagefile.os", &ent)) return 0;
    return ent.size;
}
