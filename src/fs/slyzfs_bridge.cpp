/*
 * slyzfs_bridge.cpp —— C++ SlyzFS → C shell 的桥接层
 *
 * 设计：
 *   - 用 unsigned char storage[sizeof(SlyzFS)] + placement new 构造实例
 *   - 避开 C++ __cxa_atexit / __dso_handle 依赖（裸内核没有 CRT）
 *   - 所有入口用 extern "C" 导出，避免 C++ 名字修饰
 *   - COM1 串口日志覆盖：init / format / mount / cluster 映射
 */

#include "slyzfs.h"
#include "disk_io.h"
#include "../kernel/ata.h"
#include "../kernel/io.h"
#include <stdarg.h>

/* C 端 printk 声明（tty.h 里的） */
extern "C" int printk(const char* fmt, ...);
extern "C" void kserial_puts(const char* s);
extern "C" void kserial_putchar(char c);
/* 任务级磁盘写入记账（task.c） */
extern "C" void task_account_disk(uint32_t bytes);

extern "C" int slyzfs_logf(const char* fmt, ...) {
    if (!fmt) return 0;

    char buf[512];
    int out = 0;
    const char* p = fmt;
    va_list args;
    va_start(args, fmt);

    while (*p) {
        if (*p == '%' && p[1] != '\0') {
            const char* q = p + 1;
            while (*q && ((*q == '0') || (*q == '-') || (*q == '+') || (*q == '#') || (*q == ' ') ||
                   (*q == '.') || (*q == 'l') || (*q == 'h') || ((*q >= '0') && (*q <= '9')))) {
                ++q;
            }
            if (*q == '\0') {
                buf[out++] = *p++;
                continue;
            }

            switch (*q) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    if (!s) s = "(null)";
                    while (*s) buf[out++] = *s++;
                    break;
                }
                case 'd':
                case 'i': {
                    int v = va_arg(args, int);
                    char tmp[32];
                    int t = 0;
                    if (v < 0) { buf[out++] = '-'; v = -v; }
                    if (v == 0) tmp[t++] = '0';
                    while (v > 0) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
                    while (t--) buf[out++] = tmp[t];
                    break;
                }
                case 'u': {
                    unsigned int v = va_arg(args, unsigned int);
                    char tmp[32];
                    int t = 0;
                    if (v == 0) tmp[t++] = '0';
                    while (v > 0) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
                    while (t--) buf[out++] = tmp[t];
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned int v = va_arg(args, unsigned int);
                    char tmp[32];
                    int t = 0;
                    if (v == 0) tmp[t++] = '0';
                    while (v > 0) {
                        unsigned int x = v & 0xF;
                        tmp[t++] = (char)((x < 10) ? ('0' + x) : ('A' + (x - 10)));
                        v >>= 4;
                    }
                    while (t--) buf[out++] = tmp[t];
                    break;
                }
                case 'c': {
                    buf[out++] = (char)va_arg(args, int);
                    break;
                }
                case 'p': {
                    uintptr_t v = (uintptr_t)va_arg(args, void*);
                    buf[out++] = '0';
                    buf[out++] = 'x';
                    char tmp[32];
                    int t = 0;
                    do {
                        uintptr_t x = v & 0xF;
                        tmp[t++] = (char)((x < 10) ? ('0' + x) : ('A' + (x - 10)));
                        v >>= 4;
                    } while (v != 0);
                    while (t--) buf[out++] = tmp[t];
                    break;
                }
                case '%':
                    buf[out++] = '%';
                    ++p;
                    break;
                default:
                    buf[out++] = '%';
                    buf[out++] = *q;
                    break;
            }
            p = q + 1;
        } else {
            buf[out++] = *p++;
        }
    }
    va_end(args);
    buf[out] = '\0';
    kserial_puts(buf);
    return out;
}

/* 串口日志（shell.c 中实现的同步输出，内部轮询 LSR.THRE） */
extern "C" void kserial_putchar(char c);
extern "C" void kserial_puts(const char* s);
extern "C" void kserial_dec32(uint32_t v);
extern "C" void kserial_hex32(uint32_t v);

#define SLOG_TAG "[SFS] "

/* 静默开关 —— 启动页需要在打印 banner 期间完成 SlyzFS 挂载，
 * 此时任何画面输出都会把启动画面顶乱，因此由 shell 在挂载前后切换该开关。
 * 串口调试日志不受影响，仍可完整观测挂载过程。 */
static bool g_quiet = false;
extern "C" void slyzfs_set_quiet(int quiet) { g_quiet = (quiet != 0); }
#define FS_PRINTK(...) do { if (!g_quiet) printk(__VA_ARGS__); } while (0)
static void slog(const char* msg) {
    kserial_puts(SLOG_TAG); kserial_puts(msg);
    kserial_putchar('\r'); kserial_putchar('\n');
}
static void slog_u32(const char* msg, uint32_t v) {
    kserial_puts(SLOG_TAG); kserial_puts(msg);
    kserial_dec32(v);
    kserial_putchar('\r'); kserial_putchar('\n');
}
static void slog_x2(const char* msg1, uint32_t a, const char* msg2, uint32_t b) {
    kserial_puts(SLOG_TAG); kserial_puts(msg1);
    kserial_dec32(a); kserial_puts(msg2); kserial_dec32(b);
    kserial_putchar('\r'); kserial_putchar('\n');
}
static void slog_hex_u32(const char* msg, uint32_t v) {
    kserial_puts(SLOG_TAG); kserial_puts(msg);
    kserial_putchar('0'); kserial_putchar('x'); kserial_hex32(v);
    kserial_putchar('\r'); kserial_putchar('\n');
}

/* C++ placement new 所需的 operator new —— 内建，不需要 libstdc++ */
#include <stddef.h>
inline void* operator new (size_t, void* p) { return p; }

namespace slyzfs {
    /* 存储区 —— 用 storage + placement new 绕开 CRT 依赖 */
    static unsigned char g_fs_storage[sizeof(SlyzFS)];
    static SlyzFS* g_fs = nullptr;
    static bool g_inited = false;
}

/* ======================= 生命周期 ======================= */

extern "C" int slyzfs_bridge_init(void) {
    slog("bridge_init called");
    if (slyzfs::g_inited) {
        slog("bridge_init: already inited");
        return 1;
    }
    slyzfs::g_fs = new (slyzfs::g_fs_storage) slyzfs::SlyzFS();
    slyzfs::g_inited = true;
    slog("bridge_init OK (placement new done)");
    return 1;
}

extern "C" int slyzfs_format_and_mount(void) {
    slog("format_and_mount START");
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    uint32_t total = ata_total_sectors();
    slog_u32("format: ata_total_sectors = ", total);
    if (total == 0) {
        FS_PRINTK("[SlyzFS] no disk - cannot format\n");
        slog("format FAIL: total=0");
        return 0;
    }
    if (!slyzfs::g_fs->format(total)) {
        FS_PRINTK("[SlyzFS] format failed\n");
        slog("format FAIL: SlyzFS::format returned false");
        return 0;
    }
    slog("format OK, calling mount...");
    if (!slyzfs::g_fs->mount()) {
        FS_PRINTK("[SlyzFS] mount after format failed\n");
        slog("mount after format FAIL");
        return 0;
    }
    FS_PRINTK("[SlyzFS] formatted and mounted OK (%u sectors total)\n", (unsigned int)total);
    slog_u32("format_and_mount OK, total=", total);
    return 1;
}

extern "C" int slyzfs_try_mount(void) {
    if (!slyzfs::g_inited) {
        slyzfs_bridge_init();
    }
    slog("try_mount START, reading SB at LBA 8192...");
    if (!slyzfs::g_fs->mount()) {
        FS_PRINTK("[SlyzFS] no valid filesystem at LBA %u\n", slyzfs::SLYZ_FS_OFFSET);
        slog("try_mount FAIL: bad magic or version");
        return 0;
    }
    FS_PRINTK("[SlyzFS] mounted OK\n");
    slog("try_mount OK");
    return 1;
}

extern "C" void slyzfs_unmount(void) {
    if (!slyzfs::g_inited) return;
    slog("unmount...");
    slyzfs::g_fs->unmount();
    FS_PRINTK("[SlyzFS] unmounted\n");
    slog("unmount OK");
}

/* ======================= 目录操作 ======================= */

extern "C" int slyzfs_mkdir(const char* path) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (!path || !*path) return 0;
    slog("mkdir: ");
    kserial_puts(path);
    kserial_putchar('\r'); kserial_putchar('\n');
    if (slyzfs::g_fs->mkdir(path)) return 1;
    FS_PRINTK("[SlyzFS] mkdir: '%s' failed\n", path);
    return 0;
}

extern "C" int slyzfs_rmdir(const char* path) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (!path || !*path) return 0;
    if (slyzfs::g_fs->rmdir(path)) return 1;
    FS_PRINTK("[SlyzFS] rmdir: '%s' failed\n", path);
    return 0;
}

/* C 端 DirEntry 结构 —— 必须和 slyzfs.h 里的内存布局逐字节一致。
 * 必须 pack(1)：默认对齐会把 71 字节撑到 72，导致 C 端按数组下标访问时错位。 */
#pragma pack(push, 1)
struct SlyzDirEntry {
    uint8_t  type;
    uint8_t  name[52];
    uint16_t start_cluster;
    uint32_t size;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)
static_assert(sizeof(SlyzDirEntry) == sizeof(slyzfs::DirEntry),
              "SlyzDirEntry must mirror DirEntry byte-for-byte");

extern "C" int slyzfs_list_dir(const char* path, struct SlyzDirEntry* out, int out_max) {
    if (!out || out_max <= 0) return 0;
    if (!slyzfs::g_inited) slyzfs_bridge_init();

    static slyzfs::DirEntry scratch[256];
    int n = slyzfs::g_fs->list_dir(path ? path : "", scratch, 256);
    if (n < 0) {
        /* 必须把"路径不存在 / 无读权限"与"空目录"区分开：
         * 两者都返回 0 会让 shell 把 `dir 不存在` 显示成 (empty directory)，
         * 用户无法判断是自己敲错了名字还是目录真的为空。 */
        FS_PRINTK("[SlyzFS] list_dir: path not found\n");
        slog("list_dir FAIL: path not found");
        return -1;
    }
    int copy = n < out_max ? n : out_max;
    for (int i = 0; i < copy; i++) {
        slyzfs::slyz_memcpy(&out[i], &scratch[i], sizeof(slyzfs::DirEntry));
    }
    return copy;
}

/* ======================= 容量统计 ======================= */

extern "C" int slyzfs_usage(uint32_t* total_clusters, uint32_t* used_clusters,
                            uint32_t* cluster_bytes) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    uint32_t total = 0, used = 0, bytes = 0;
    if (!slyzfs::g_fs->usage(&total, &used, &bytes)) {
        slog("usage FAIL: filesystem not mounted");
        return 0;
    }
    if (total_clusters) *total_clusters = total;
    if (used_clusters)  *used_clusters = used;
    if (cluster_bytes)  *cluster_bytes = bytes;
    return 1;
}

/* ======================= 文件操作 ======================= */

extern "C" int slyzfs_create(const char* path) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (!path || !*path) return 0;
    if (slyzfs::g_fs->create(path)) {
        slog("create OK: ");
        kserial_puts(path);
        kserial_putchar('\r'); kserial_putchar('\n');
        return 1;
    }
    FS_PRINTK("[SlyzFS] create: '%s' failed\n", path);
    return 0;
}

extern "C" int slyzfs_remove(const char* path) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (!path || !*path) return 0;
    if (slyzfs::g_fs->remove(path)) return 1;
    FS_PRINTK("[SlyzFS] remove: '%s' failed\n", path);
    return 0;
}

/* ======================= open / close（不透明句柄） ======================= */

extern "C" void* slyzfs_open(const char* path, const char* mode) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (!path || !mode) return nullptr;
    slyzfs::FileHandle* fh = slyzfs::g_fs->open(path, mode);
    if (fh) {
        slog("open OK: ");
        kserial_puts(path);
        kserial_putchar(' ');
        kserial_puts(mode);
        kserial_putchar('\r'); kserial_putchar('\n');
    } else {
        slog("open FAIL: ");
        kserial_puts(path);
        kserial_putchar('\r'); kserial_putchar('\n');
    }
    return static_cast<void*>(fh);
}

extern "C" void slyzfs_close(void* fh) {
    if (!fh) return;
    if (!slyzfs::g_inited) return;
    slog("close");
    slyzfs::FileHandle* real = static_cast<slyzfs::FileHandle*>(fh);
    slyzfs::g_fs->close(real);
}

extern "C" int slyzfs_read(void* fh, uint8_t* buf, int n) {
    if (!fh || !buf || n <= 0) return 0;
    if (!slyzfs::g_inited) return 0;
    slyzfs::FileHandle* real = static_cast<slyzfs::FileHandle*>(fh);
    size_t got = slyzfs::g_fs->read(real, buf, (size_t)n);
    static uint32_t rcnt = 0; rcnt++;
    if (rcnt <= 3 || (rcnt % 64) == 0) {
        slog_x2("read: got=", (uint32_t)got, " of ", (uint32_t)n);
    }
    return (int)got;
}

extern "C" int slyzfs_write(void* fh, const uint8_t* buf, int n) {
    if (!fh || !buf || n <= 0) return 0;
    if (!slyzfs::g_inited) return 0;
    slyzfs::FileHandle* real = static_cast<slyzfs::FileHandle*>(fh);
    size_t written = slyzfs::g_fs->write(real, buf, (size_t)n);
    if (written > 0) {
        task_account_disk((uint32_t)written);
    }
    slog_x2("write: wrote=", (uint32_t)written, " of ", (uint32_t)n);
    return (int)written;
}

extern "C" int slyzfs_seek(void* fh, uint32_t offset) {
    if (!fh) return 0;
    if (!slyzfs::g_inited) return 0;
    slyzfs::FileHandle* real = static_cast<slyzfs::FileHandle*>(fh);
    return slyzfs::g_fs->seek(real, offset) ? 1 : 0;
}

/* ======================= stat / 权限 ======================= */

extern "C" int slyzfs_stat(const char* path, struct SlyzDirEntry* out) {
    if (!path || !out) return 0;
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    slyzfs::DirEntry* de = slyzfs::g_fs->stat(path);
    if (!de) return 0;
    slyzfs::slyz_memcpy(out, de, sizeof(slyzfs::DirEntry));
    return 1;
}

/* 登录层通过这两个接口切换操作者身份；FS 层据此强制权限。 */
extern "C" void slyzfs_set_uid(int uid) {
    if (uid < 0) uid = 0;
    if (uid > 255) uid = 255;
    slyzfs::SlyzFS::set_current_uid((uint16_t)uid);
}

extern "C" int slyzfs_get_uid(void) {
    return (int)slyzfs::SlyzFS::current_uid();
}

extern "C" int slyzfs_chmod(const char* path, int mode) {
    if (!path || !*path) return 0;
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (mode < 0 || mode > 07777) return 0;
    return slyzfs::g_fs->chmod(path, (uint16_t)mode) ? 1 : 0;
}

extern "C" int slyzfs_chown(const char* path, int uid) {
    if (!path || !*path) return 0;
    if (uid < 0 || uid > 255) return 0;
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    return slyzfs::g_fs->chown(path, (uint8_t)uid) ? 1 : 0;
}

/* 受保护标志：设置/清除要求 root，由 SlyzFS 层强制。
 * 普通用户调用这里会直接失败，而不是靠 shell 层"自觉"。 */
extern "C" int slyzfs_set_protected(const char* path, int on) {
    if (!path || !*path) return 0;
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    return slyzfs::g_fs->set_protected(path, on != 0) ? 1 : 0;
}

extern "C" int slyzfs_is_protected(const char* path) {
    if (!path || !*path) return 0;
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    return slyzfs::g_fs->is_protected(path) ? 1 : 0;
}

/* ======================= swap pagefile =======================
 * swap 数据必须落在"文件"而不是裸卷上：pagefile.os 是普通 SlyzFS
 * 文件，受保护位保证 root 也无法删除。这里持有长生命周期句柄，
 * 直接用 SlyzFS::read/write 读写固定偏移，不走带日志的 bridge
 * 包装函数 —— 否则每次换页都会往串口打两行 "write: wrote=..."。
 * ============================================================ */
static slyzfs::FileHandle* g_swap_fh = nullptr;

extern "C" int slyzfs_swap_attach(void) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    if (g_swap_fh) return 1;

    slyzfs::DirEntry* dir = slyzfs::g_fs->stat("etc");
    if (!dir || dir->type != slyzfs::DIR_DIR) {
        slyzfs::g_fs->mkdir("etc");
    }

    /* 每次启动都重建 pagefile：
     *   1. swap 内容是易失的，跨启动复用没有意义；
     *   2. 若上次没有走 close（内核重启/断电），目录 size 与 FAT 簇链
     *      可能不一致，直接截断可以把旧簇全部归还，避免孤儿簇让 df
     *      显示的空间凭空消失；
     *   3. 受保护文件不能直接 create，必须先由内核（root）显式解除，
     *      截断后立即重新加上。用户侧仍然无法删除这个文件。 */
    slyzfs::DirEntry* file = slyzfs::g_fs->stat("etc\\pagefile.os");
    if (file && file->type == slyzfs::DIR_FILE) {
        if (!slyzfs::g_fs->set_protected("etc\\pagefile.os", false)) {
            slog("swap_attach: cannot clear protection for reset");
            return 0;
        }
    }

    if (!slyzfs::g_fs->create("etc\\pagefile.os")) {
        slog("swap_attach: create pagefile failed");
        return 0;
    }

    g_swap_fh = slyzfs::g_fs->open("etc\\pagefile.os", "a");
    if (!g_swap_fh) {
        slog("swap_attach: open pagefile failed");
        return 0;
    }

    if (!slyzfs::g_fs->set_protected("etc\\pagefile.os", true)) {
        slog("swap_attach: set_protected failed");
    }
    slog("swap_attach: pagefile ready");
    return 1;
}

extern "C" int slyzfs_swap_write(uint32_t offset, const uint8_t* buf, int n) {
    if (!g_swap_fh || !buf || n <= 0) return 0;
    if (!slyzfs::g_fs->seek(g_swap_fh, offset)) return 0;
    return (int)slyzfs::g_fs->write(g_swap_fh, buf, (size_t)n);
}

extern "C" int slyzfs_swap_read(uint32_t offset, uint8_t* buf, int n) {
    if (!g_swap_fh || !buf || n <= 0) return 0;
    if (!slyzfs::g_fs->seek(g_swap_fh, offset)) return 0;

    int total = 0;
    while (total < n) {
        size_t got = slyzfs::g_fs->read(g_swap_fh, buf + total, (size_t)(n - total));
        if (got == 0) break;
        total += (int)got;
    }
    return total;
}

extern "C" uint32_t slyzfs_swap_bytes(void) {
    if (!g_swap_fh) return 0;
    return g_swap_fh->size;
}

/* ======================= 测试目录结构 ======================= */

extern "C" int slyzfs_mklazy(void) {
    if (!slyzfs::g_inited) slyzfs_bridge_init();
    slog("mklazy: creating dirs...");
    slyzfs::g_fs->mkdir("home");
    slyzfs::g_fs->mkdir("usr");
    slyzfs::g_fs->mkdir("tmp");
    slyzfs::g_fs->mkdir("usr\\bin");
    slyzfs::g_fs->mkdir("home\\root");
    slyzfs::g_fs->mkdir("home\\user");

    void* fh = slyzfs::g_fs->open("README.txt", "w");
    if (fh) {
        const char* msg = "Welcome to SaylterOS v0.3!\r\nSlyzFS filesystem is ready.\r\n";
        slyzfs::g_fs->write(static_cast<slyzfs::FileHandle*>(fh),
                           reinterpret_cast<const uint8_t*>(msg),
                           (int)slyzfs::slyz_strlen(msg));
        slyzfs::g_fs->close(static_cast<slyzfs::FileHandle*>(fh));
    }

    fh = slyzfs::g_fs->open("home\\root\\profile.txt", "w");
    if (fh) {
        const char* msg = "# SaylterOS root profile\r\n# created by mklazy\r\n";
        slyzfs::g_fs->write(static_cast<slyzfs::FileHandle*>(fh),
                           reinterpret_cast<const uint8_t*>(msg),
                           (int)slyzfs::slyz_strlen(msg));
        slyzfs::g_fs->close(static_cast<slyzfs::FileHandle*>(fh));
    }

    FS_PRINTK("[SlyzFS] mklazy created test directory structure\n");
    slog("mklazy OK");
    return 1;
}
