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

static int api_disk_read(uint32_t lba, void *buf) {
    return blk_read(lba, (uint8_t *)buf);
}

static int api_disk_write(uint32_t lba, const void *buf) {
    return blk_write(lba, (const uint8_t *)buf);
}

static uint32_t api_total_sectors(void) {
    return blk_total_sectors();
}

static void api_log(const char *s) {
    kserial_puts(s);
}

static int api_flush(void) {
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
