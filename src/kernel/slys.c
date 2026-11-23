/* ============================================================
 * slys.c - .slys 驱动加载器与内核 API
 * ------------------------------------------------------------
 * 只支持固定加载地址：头部声明 load_addr，镜像被复制到该物理
 * 地址后直接执行。这样不需要重定位器，也不破坏内核的
 * -mcmodel=small 绝对寻址假设。
 *
 * 安全边界：
 *   - 镜像最大 4 MB，BSS 最大 16 MB；
 *   - 加载地址必须在 SLYS_LOAD_ADDR 起始、且不与内核/堆/
 *     backbuffer 重叠（本项目的驱动区从 32 MB 开始）；
 *   - FNV-1a 校验覆盖整个镜像；
 *   - 入口偏移必须落在镜像范围内。
 * ============================================================ */

#include <stdint.h>

#include "slys.h"
#include "blk.h"
#include "smp.h"
#include "mem.h"
#include "net.h"
#include "tty.h"
#include "task.h"
#include "slyzlink.h"
#include "vbe.h"
#include "slyertype.h"
#include "keyboard.h"
#include "gui.h"
#include "tty.h"

/* SlyzFS */
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern int   slyzfs_seek(void *fh, uint32_t offset);
extern void  slyzfs_close(void *fh);
extern int   slyzfs_stat(const char *path, void *out);

extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

/* 目录项布局（与 src/include/slysfs.h 的 SlysDirEntry 一致）。 */
#pragma pack(push, 1)
struct SlysDirEntry {
    uint8_t  type;
    uint8_t  flags;
    uint16_t name_len;
    char     name[256];
    uint32_t size;
    uint32_t start_lba;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)

static smp_lock_t g_slys_lock = 0;

/* 清零 BSS：模块的 BSS 可达 8MB 以上，逐字节循环在 QEMU 下要
 * 几十毫秒。rep stosq 一次写 8 字节，并对齐到 8 字节边界后使用，
 * 避免越界写。 */
static void slz_zero(uint8_t *dst, uint32_t n) {
    uint32_t i = 0;
    while (i < n && ((uintptr_t)(dst + i) & 7u)) dst[i++] = 0;

    uint32_t left = n - i;
    uint32_t qwords = left >> 3;
    if (qwords) {
        uint64_t *q = (uint64_t *)(void *)(dst + i);
        __asm__ volatile ("cld\n\trep stosq"
                          : "+D"(q), "+c"(qwords)
                          : "a"(0ull)
                          : "memory");
    }
    for (uint32_t k = qwords << 3; k < left; k++) dst[i + k] = 0;
}

void slys_lock(void) {
    smp_spin_lock(&g_slys_lock);
}

void slys_unlock(void) {
    smp_spin_unlock(&g_slys_lock);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static int read_full(void *fh, uint8_t *buf, int n) {
    int total = 0;
    while (total < n) {
        int got = slyzfs_read(fh, buf + total, n - total);
        if (got <= 0) break;
        total += got;
    }
    return total;
}

int slys_load(const char *path, void **entry, char *name, int name_cap) {
    if (!path || !entry) return 0;

    struct SlysDirEntry ent;
    if (!slyzfs_stat(path, &ent) || ent.type == 0x02) {
        kserial_puts("[SLYS] stat failed: ");
        kserial_puts(path);
        kserial_puts("\r\n");
        return 0;
    }

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        kserial_puts("[SLYS] open failed: ");
        kserial_puts(path);
        kserial_puts("\r\n");
        return 0;
    }

    uint8_t hdr[SLYS_HEADER_SIZE];
    if (read_full(fh, hdr, (int)sizeof(hdr)) != (int)sizeof(hdr)) {
        slyzfs_close(fh);
        return 0;
    }

    if (hdr[0] != SLYS_MAGIC0 || hdr[1] != SLYS_MAGIC1 ||
        hdr[2] != SLYS_MAGIC2 || hdr[3] != SLYS_MAGIC3 ||
        hdr[4] != SLYS_MAGIC4 || hdr[5] != SLYS_MAGIC5 ||
        hdr[6] != SLYS_MAGIC6 || hdr[7] != 0) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] bad magic\r\n");
        return 0;
    }

    uint16_t version = rd16(hdr + 8);
    uint16_t header_size = rd16(hdr + 10);
    uint32_t load_addr = rd32(hdr + 12);
    uint32_t image_size = rd32(hdr + 16);
    uint32_t bss_size = rd32(hdr + 20);
    uint32_t entry_off = rd32(hdr + 24);
    uint32_t checksum = rd32(hdr + 32);

    if (version != SLYS_VERSION || header_size != SLYS_HEADER_SIZE) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] version/header mismatch\r\n");
        return 0;
    }
    /* 固定基址模块共用同一套加载器：音频驱动在 0x02000000，
     * 文件系统在 0x04000000，预装图形应用在 0x08000000。
     * 这里只约束"驱动区"整体范围，并要求镜像 + BSS 不越过 512MB
     * 恒等映射上限，避免任何模块覆盖 backbuffer 或内核堆。 */
    if (load_addr < SLYS_LOAD_ADDR || load_addr >= 0x20000000u) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] unsupported load address\r\n");
        return 0;
    }
    if ((uint64_t)load_addr + (uint64_t)image_size + (uint64_t)bss_size >
        0x20000000ull) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] module exceeds mapped range\r\n");
        return 0;
    }
    if (image_size == 0 || image_size > SLYS_MAX_IMAGE ||
        bss_size > SLYS_MAX_BSS) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] image/bss too large\r\n");
        return 0;
    }
    if (entry_off >= image_size) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] bad entry offset\r\n");
        return 0;
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)load_addr;
    if (read_full(fh, dst, (int)image_size) != (int)image_size) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] short read\r\n");
        return 0;
    }
    slyzfs_close(fh);

    if (fnv1a(dst, image_size) != checksum) {
        kserial_puts("[SLYS] checksum mismatch\r\n");
        return 0;
    }

    /* 清零 BSS，保证驱动静态变量从 0 开始。 */
    slz_zero(dst + image_size, bss_size);

    if (name && name_cap > 0) {
        int n = 0;
        while (n < 23 && hdr[40 + n] && n < name_cap - 1) {
            name[n] = (char)hdr[40 + n];
            n++;
        }
        name[n] = '\0';
    }

    *entry = (void *)(uintptr_t)(load_addr + entry_off);

    kserial_puts("[SLYS] loaded ");
    kserial_puts(name ? name : path);
    kserial_puts(" image=");
    kserial_dec32(image_size);
    kserial_puts(" bss=");
    kserial_dec32(bss_size);
    kserial_puts("\r\n");
    return 1;
}

/* ============================================================
 * 从原始 LBA 加载模块
 * ------------------------------------------------------------
 * 文件系统模块是启动链上第一个 .slys：此时还没有文件系统，
 * 只能按扇区读。布局：
 *   LBA + 0        64 字节头
 *   LBA + 1..      镜像（image_size 字节，按扇区覆盖）
 * ============================================================ */
int slys_load_raw(uint32_t lba, void **entry, char *name, int name_cap) {
    if (!entry) return 0;

    uint8_t sec[512];
    if (!blk_read(lba, sec)) {
        kserial_puts("[SLYS] raw: header read failed\r\n");
        return 0;
    }
    if (sec[0] != SLYS_MAGIC0 || sec[1] != SLYS_MAGIC1 ||
        sec[2] != SLYS_MAGIC2 || sec[3] != SLYS_MAGIC3 ||
        sec[4] != SLYS_MAGIC4 || sec[5] != SLYS_MAGIC5 ||
        sec[6] != SLYS_MAGIC6 || sec[7] != 0) {
        kserial_puts("[SLYS] raw: bad magic\r\n");
        return 0;
    }

    uint16_t version = rd16(sec + 8);
    uint16_t header_size = rd16(sec + 10);
    uint32_t load_addr = rd32(sec + 12);
    uint32_t image_size = rd32(sec + 16);
    uint32_t bss_size = rd32(sec + 20);
    uint32_t entry_off = rd32(sec + 24);
    uint32_t checksum = rd32(sec + 32);

    if (version != SLYS_VERSION || header_size != SLYS_HEADER_SIZE) {
        kserial_puts("[SLYS] raw: version mismatch\r\n");
        return 0;
    }
    if (image_size == 0 || image_size > SLYS_MAX_IMAGE ||
        bss_size > SLYS_MAX_BSS) {
        kserial_puts("[SLYS] raw: image/bss too large\r\n");
        return 0;
    }
    if (entry_off >= image_size) {
        kserial_puts("[SLYS] raw: bad entry offset\r\n");
        return 0;
    }
    /* 只允许加载到内核区之上的固定物理地址，避免覆盖内核/栈/堆。 */
    if (load_addr < 0x01000000u || load_addr >= 0x20000000u) {
        kserial_puts("[SLYS] raw: unsupported load address\r\n");
        return 0;
    }
    if (load_addr + image_size + bss_size < load_addr) {
        kserial_puts("[SLYS] raw: address overflow\r\n");
        return 0;
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)load_addr;

    /* .slys 文件 = 64 字节头 + 镜像。镜像紧跟在头之后，
     * 因此第一个扇区里还剩 448 字节属于镜像，不能整扇区跳过。 */
    uint32_t done = 0;
    uint32_t first = 512u - SLYS_HEADER_SIZE;
    if (first > image_size) first = image_size;
    for (uint32_t i = 0; i < first; i++) dst[i] = sec[SLYS_HEADER_SIZE + i];
    done = first;

    uint32_t lba_cur = lba + 1u;
    while (done < image_size) {
        if (!blk_read(lba_cur, sec)) {
            kserial_puts("[SLYS] raw: image read failed\r\n");
            return 0;
        }
        uint32_t n = image_size - done;
        if (n > 512u) n = 512u;
        for (uint32_t i = 0; i < n; i++) dst[done + i] = sec[i];
        done += n;
        lba_cur++;
    }

    if (fnv1a(dst, image_size) != checksum) {
        kserial_puts("[SLYS] raw: checksum mismatch\r\n");
        return 0;
    }

    slz_zero(dst + image_size, bss_size);

    if (name && name_cap > 0) {
        int n = 0;
        while (n < 23 && sec[40 + n] && n < name_cap - 1) {            name[n] = (char)sec[40 + n];
            n++;
        }
        name[n] = '\0';
    }

    *entry = (void *)(uintptr_t)(load_addr + entry_off);
    kserial_puts("[SLYS] raw loaded ");
    kserial_puts(name ? name : "?");
    kserial_puts(" image=");
    kserial_dec32(image_size);
    kserial_puts(" bss=");
    kserial_dec32(bss_size);
    kserial_puts("\r\n");
    return 1;
}

/* ============================================================
 * 内核 API 实现
 * ============================================================ */

static void *api_alloc(uint32_t bytes) {
    return kmalloc(bytes);
}

static void api_free(void *ptr) {
    kfree(ptr);
}

static uint32_t api_now_ms(void) {
    return net_now_ms();
}

static void api_sleep_ms(uint32_t ms) {
    /* 协作式等待：把时间片让给 GUI/其他任务。 */
    uint32_t start = net_now_ms();
    while (net_now_ms() - start < ms) {
        task_yield();
    }
}

static void api_log(const char *s) {
    kserial_puts(s);
}

static void api_log_u32(uint32_t v) {
    kserial_dec32(v);
}

static void api_lock(void) {
    slys_lock();
}

static void api_unlock(void) {
    slys_unlock();
}

static void api_yield(void) {
    task_yield();
}

static int api_read_file(const char *path, uint8_t *buf, int cap) {
    if (!path || !buf || cap <= 0) return -1;

    void *fh = slyzfs_open(path, "r");
    if (!fh) return -1;

    int total = 0;
    while (total < cap) {
        int got = slyzfs_read(fh, buf + total, cap - total);
        if (got <= 0) break;
        total += got;
    }
    slyzfs_close(fh);
    return total > 0 ? total : -1;
}

static int api_file_exists(const char *path) {
    struct SlysDirEntry ent;
    return slyzfs_stat(path, &ent) ? 1 : 0;
}

/* ---- 预装应用宿主 ---- */

static void *api_fs_open(const char *path, const char *mode) {
    return slyzfs_open(path, mode);
}

static int api_fs_read(void *fh, uint8_t *buf, int n) {
    return slyzfs_read(fh, buf, n);
}

static int api_fs_write(void *fh, const uint8_t *buf, int n) {
    return slyzfs_write(fh, buf, n);
}

static void api_fs_close(void *fh) {
    slyzfs_close(fh);
}

static int api_fs_stat(const char *path, void *out) {
    return slyzfs_stat(path, out);
}

static void api_fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0 || !vbe_is_enabled()) return;
    vbe_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static int api_fb_draw_cell(int x, int y, int cw, int ch, uint32_t cp, uint32_t fg) {
    if (!vbe_is_enabled()) return 0;
    return slyertype_draw_cell_buf(x, y, cw, ch, cp, fg);
}

static void api_fb_swap(void) {
    vbe_swap();
}

static uint32_t api_fb_width(void) {
    return vbe_current.width;
}

static uint32_t api_fb_height(void) {
    return vbe_current.height;
}

static char api_kbd_read(void) {
    /* 图形会话会屏蔽 IRQ1 并自己轮询 8042；此时内核环形缓冲不会
     * 再有新按键。预装应用必须跟随当前键盘所有者，否则在全屏编辑器
     * 里收不到任何输入。 */
    if (gui_keyboard_owned()) return gui_keyboard_poll();
    return kb_read_char();
}

static void api_win_fill_rect(int x, int y, int w, int h, uint32_t color) {
    gui_ext_fill_rect(x, y, w, h, color);
}

static int api_win_draw_cell(int x, int y, int cw, int ch,
                             uint32_t cp, uint32_t fg) {
    return gui_ext_draw_cell(x, y, cw, ch, cp, fg);
}

static uint32_t api_win_width(void) {
    return gui_ext_width();
}

static uint32_t api_win_height(void) {
    return gui_ext_height();
}

static void api_win_invalidate(void) {
    gui_ext_invalidate();
}

static void api_win_close(void) {
    gui_ext_request_close();
}

static const char *g_slys_arg = "";

static const char *api_arg(void) {
    return g_slys_arg;
}

void slys_set_arg(const char *arg) {
    g_slys_arg = arg ? arg : "";
}

/* ---- POSIX 段：musl libc 的 syscall 垫片落到这里 ----
 * fd 表由内核持有：3..31 是模块打开的文件，0/1/2 保留给标准流。
 * 失败一律返回 -errno，与 Linux 语义一致。 */
#define SLYZ_FD_MAX 32
static void *g_posix_fds[SLYZ_FD_MAX];

static int api_posix_write(int fd, const void *buf, int len) {
    if (!buf || len < 0) return -22;
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;
        for (int i = 0; i < len; i++) {
            tty_putchar(p[i]);
        }
        return len;
    }
    if (fd < 3 || fd >= SLYZ_FD_MAX || !g_posix_fds[fd]) return -9;
    return slyzfs_write(g_posix_fds[fd], (const uint8_t *)buf, len);
}

static int api_posix_read(int fd, void *buf, int len) {
    if (!buf || len < 0) return -22;
    /* 标准输入还没有接到 tty 行编辑：返回 0 表示 EOF，让 getchar/fgets
     * 走正常结束路径，而不是拿到 -EBADF 变成读错误。 */
    if (fd == 0) return 0;
    if (fd < 3 || fd >= SLYZ_FD_MAX || !g_posix_fds[fd]) return -9;
    return slyzfs_read(g_posix_fds[fd], (uint8_t *)buf, len);
}

static int api_posix_open(const char *path, int flags) {
    if (!path) return -22;
    const char *mode = (flags == 0) ? "r" : ((flags == 1) ? "w" : "a");
    void *fh = slyzfs_open(path, mode);
    if (!fh) return -2;
    for (int i = 3; i < SLYZ_FD_MAX; i++) {
        if (!g_posix_fds[i]) {
            g_posix_fds[i] = fh;
            return i;
        }
    }
    slyzfs_close(fh);
    return -24;
}

static int api_posix_close(int fd) {
    if (fd < 3 || fd >= SLYZ_FD_MAX || !g_posix_fds[fd]) return -9;
    slyzfs_close(g_posix_fds[fd]);
    g_posix_fds[fd] = 0;
    return 0;
}

static int64_t api_posix_lseek(int fd, int64_t offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    /* SlyzFS v2 的句柄是顺序读写，暂不支持随机定位。 */
    return -29;
}

static int api_posix_fstat(int fd, void *st) {
    (void)fd; (void)st;
    return -38;
}

static uint64_t api_posix_time_ms(void) {
    return net_now_ms();
}

/* 匿名映射：在 kmalloc 之上做页对齐包装，头部保存原始指针。
 * 多分配 4096 + 8 字节，保证对齐后往前 8 字节仍在块内。 */
static void *api_posix_map(uint64_t len) {
    if (len == 0 || len > 0x7FFFFFFFu) return 0;
    uint8_t *raw = (uint8_t *)kmalloc((uint32_t)(len + 4096u + 8u));
    if (!raw) return 0;
    uintptr_t aligned = ((uintptr_t)raw + 8u + 4095u) & ~(uintptr_t)4095u;
    *(void **)(aligned - sizeof(void *)) = raw;
    return (void *)aligned;
}

static void api_posix_unmap(void *ptr, uint64_t len) {
    (void)len;
    if (!ptr) return;
    void *raw = *(void **)((uint8_t *)ptr - sizeof(void *));
    kfree(raw);
}

static const struct SlysKernelApi g_api = {
    2,
    api_alloc,
    api_free,
    api_now_ms,
    api_sleep_ms,
    api_log,
    api_log_u32,
    api_lock,
    api_unlock,
    api_yield,
    api_read_file,
    api_file_exists,
    api_fs_open,
    api_fs_read,
    api_fs_write,
    api_fs_close,
    api_fs_stat,
    api_fb_fill_rect,
    api_fb_draw_cell,
    api_fb_swap,
    api_fb_width,
    api_fb_height,
    api_kbd_read,
    api_arg,
    api_win_fill_rect,
    api_win_draw_cell,
    api_win_width,
    api_win_height,
    api_win_invalidate,
    api_win_close,
    api_posix_write,
    api_posix_read,
    api_posix_open,
    api_posix_close,
    api_posix_lseek,
    api_posix_fstat,
    api_posix_time_ms,
    api_posix_map,
    api_posix_unmap
};

const struct SlysKernelApi *slys_kernel_api(void) {
    return &g_api;
}

/* ============================================================
 * .slyzd 动态链接加载器
 * ------------------------------------------------------------
 * 固定基址模型：模块头声明 load_addr，加载器不做搬移。
 * 流程：校验头 -> 递归加载依赖 -> 复制镜像 -> 校验和 ->
 *       注册导出 -> 回填导入槽 -> 应用重定位 -> 返回入口。
 * 依赖从 system\modules\<name>.slyzd 读取。
 * ============================================================ */

#define SLYS_LINK_MAX        8
#define SLYS_LINK_PATH_MAX   128
#define SLYS_LINK_MIN_ADDR   0x06000000u   /* 系统模块区之上 */
#define SLYS_LINK_MAX_ADDR   0x20000000u
#define SLYS_LINK_TABLE_MAX  65536u

struct SlysLoadedModule {
    int      used;
    char     name[SLYZ_MODULE_NAME_MAX];
    uint32_t load_addr;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry_addr;
    struct SlyzSymbol *exports;
    uint32_t export_count;
};

static struct SlysLoadedModule g_linked[SLYS_LINK_MAX];

static int link_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static struct SlysLoadedModule *link_find(const char *name) {
    for (int i = 0; i < SLYS_LINK_MAX; i++) {
        if (g_linked[i].used && link_streq(g_linked[i].name, name)) {
            return &g_linked[i];
        }
    }
    return 0;
}

static int link_range_overlaps(uint32_t addr, uint32_t size) {
    uint32_t end = addr + size;
    if (end < addr) return 1;

    for (int i = 0; i < SLYS_LINK_MAX; i++) {
        if (!g_linked[i].used) continue;
        uint32_t a0 = g_linked[i].load_addr;
        uint32_t a1 = a0 + g_linked[i].image_size + g_linked[i].bss_size;
        if (addr < a1 && a0 < end) return 1;
    }
    return 0;
}

static int link_find_symbol(const char *name, uint16_t version, uint16_t kind,
                            uint32_t *out) {
    for (int i = 0; i < SLYS_LINK_MAX; i++) {
        if (!g_linked[i].used) continue;
        for (uint32_t j = 0; j < g_linked[i].export_count; j++) {
            struct SlyzSymbol *s = &g_linked[i].exports[j];
            if (s->version != version || s->kind != kind) continue;
            if (!link_streq(s->name, name)) continue;
            *out = g_linked[i].load_addr + s->offset;
            return 1;
        }
    }
    return 0;
}

static int link_load_file(const char *path, int depth, uint32_t *entry_out) {
    if (!path || !entry_out || depth > 8) return 0;

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        kserial_puts("[LINK] open failed: ");
        kserial_puts(path);
        kserial_puts("\r\n");
        return 0;
    }

    uint8_t hdr[SLYZLINK_HEADER_SIZE];
    if (read_full(fh, hdr, (int)sizeof(hdr)) != (int)sizeof(hdr)) {
        slyzfs_close(fh);
        kserial_puts("[LINK] short header\r\n");
        return 0;
    }

    if (hdr[0] != SLYZD_MAGIC0 || hdr[1] != SLYZD_MAGIC1 ||
        hdr[2] != SLYZD_MAGIC2 || hdr[3] != SLYZD_MAGIC3 ||
        hdr[4] != SLYZD_MAGIC4 || hdr[5] != SLYZD_MAGIC5 ||
        hdr[6] != SLYZD_MAGIC6 || hdr[7] != SLYZD_MAGIC7) {
        slyzfs_close(fh);
        kserial_puts("[LINK] bad magic\r\n");
        return 0;
    }

    uint16_t version = rd16(hdr + 8);
    uint16_t flags = rd16(hdr + 10);
    uint32_t header_size = rd32(hdr + 12);
    uint32_t load_addr = rd32(hdr + 16);
    uint32_t image_size = rd32(hdr + 20);
    uint32_t bss_size = rd32(hdr + 24);
    uint32_t entry_off = rd32(hdr + 28);
    uint32_t image_off = rd32(hdr + 32);
    uint32_t export_off = rd32(hdr + 36);
    uint32_t export_count = rd32(hdr + 40);
    uint32_t import_off = rd32(hdr + 44);
    uint32_t import_count = rd32(hdr + 48);
    uint32_t reloc_off = rd32(hdr + 52);
    uint32_t reloc_count = rd32(hdr + 56);
    uint32_t dep_off = rd32(hdr + 60);
    uint32_t dep_count = rd32(hdr + 64);
    uint32_t checksum = rd32(hdr + 68);

    char name[SLYZ_MODULE_NAME_MAX];
    for (int i = 0; i < (int)SLYZ_MODULE_NAME_MAX - 1; i++) {
        name[i] = (char)hdr[72 + i];
        if (name[i] == '\0') break;
    }
    name[SLYZ_MODULE_NAME_MAX - 1] = '\0';

    if (version != SLYZLINK_VERSION || header_size != SLYZLINK_HEADER_SIZE) {
        slyzfs_close(fh);
        kserial_puts("[LINK] version/header mismatch\r\n");
        return 0;
    }
    if (!(flags & SLYZ_MOD_DYNAMIC)) {
        slyzfs_close(fh);
        kserial_puts("[LINK] not a dynamic module\r\n");
        return 0;
    }
    if (image_size == 0 || image_size > SLYS_MAX_IMAGE ||
        bss_size > SLYS_MAX_BSS) {
        slyzfs_close(fh);
        kserial_puts("[LINK] image/bss too large\r\n");
        return 0;
    }
    if (entry_off >= image_size || image_off < SLYZLINK_HEADER_SIZE ||
        image_off > SLYS_LINK_TABLE_MAX) {
        slyzfs_close(fh);
        kserial_puts("[LINK] bad offsets\r\n");
        return 0;
    }
    if (load_addr < SLYS_LINK_MIN_ADDR || load_addr >= SLYS_LINK_MAX_ADDR ||
        load_addr + image_size + bss_size < load_addr ||
        link_range_overlaps(load_addr, image_size + bss_size)) {
        slyzfs_close(fh);
        kserial_puts("[LINK] bad or overlapping load address\r\n");
        return 0;
    }

    /* 已加载同名模块：直接复用。 */
    struct SlysLoadedModule *existing = link_find(name);
    if (existing) {
        slyzfs_close(fh);
        *entry_out = existing->entry_addr;
        return 1;
    }

    uint32_t table_bytes = image_off - SLYZLINK_HEADER_SIZE;
    uint8_t *tables = 0;
    if (table_bytes > 0) {
        tables = (uint8_t *)kmalloc(table_bytes);
        if (!tables) {
            slyzfs_close(fh);
            kserial_puts("[LINK] out of memory for tables\r\n");
            return 0;
        }
        if (read_full(fh, tables, (int)table_bytes) != (int)table_bytes) {
            kfree(tables);
            slyzfs_close(fh);
            kserial_puts("[LINK] short tables\r\n");
            return 0;
        }
    }

    if (table_bytes > 0) {
        if ((export_count &&
             (export_off < SLYZLINK_HEADER_SIZE ||
              export_off + export_count * sizeof(struct SlyzSymbol) > image_off)) ||
            (import_count &&
             (import_off < SLYZLINK_HEADER_SIZE ||
              import_off + import_count * sizeof(struct SlyzImport) > image_off)) ||
            (reloc_count &&
             (reloc_off < SLYZLINK_HEADER_SIZE ||
              reloc_off + reloc_count * sizeof(struct SlyzReloc) > image_off)) ||
            (dep_count &&
             (dep_off < SLYZLINK_HEADER_SIZE ||
              dep_off + dep_count * sizeof(struct SlyzDep) > image_off))) {
            kfree(tables);
            slyzfs_close(fh);
            kserial_puts("[LINK] table outside file\r\n");
            return 0;
        }
    }

    struct SlyzSymbol *exports = 0;
    struct SlyzImport *imports = 0;
    struct SlyzReloc *relocs = 0;
    struct SlyzDep *deps = 0;
    if (tables) {
        if (export_count) exports = (struct SlyzSymbol *)(void *)(tables + (export_off - SLYZLINK_HEADER_SIZE));
        if (import_count) imports = (struct SlyzImport *)(void *)(tables + (import_off - SLYZLINK_HEADER_SIZE));
        if (reloc_count) relocs = (struct SlyzReloc *)(void *)(tables + (reloc_off - SLYZLINK_HEADER_SIZE));
        if (dep_count) deps = (struct SlyzDep *)(void *)(tables + (dep_off - SLYZLINK_HEADER_SIZE));
    }

    /* 递归加载依赖（依赖先于本模块完成符号注册）。 */
    for (uint32_t i = 0; i < dep_count; i++) {
        char dep_path[SLYS_LINK_PATH_MAX];
        int n = 0;
        const char *prefix = "system\\modules\\";
        const char *suffix = ".slyzd";
        while (prefix[n] && n < SLYS_LINK_PATH_MAX - 1) { dep_path[n] = prefix[n]; n++; }
        for (int k = 0; deps[i].name[k] && n < SLYS_LINK_PATH_MAX - 1; k++) {
            dep_path[n++] = deps[i].name[k];
        }
        for (int k = 0; suffix[k] && n < SLYS_LINK_PATH_MAX - 1; k++) {
            dep_path[n++] = suffix[k];
        }
        dep_path[n] = '\0';

        uint32_t dep_entry = 0;
        if (!link_load_file(dep_path, depth + 1, &dep_entry)) {
            kfree(tables);
            slyzfs_close(fh);
            kserial_puts("[LINK] dependency failed: ");
            kserial_puts(deps[i].name);
            kserial_puts("\r\n");
            return 0;
        }
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)load_addr;
    if (!slyzfs_seek(fh, image_off) ||
        read_full(fh, dst, (int)image_size) != (int)image_size) {
        kfree(tables);
        slyzfs_close(fh);
        kserial_puts("[LINK] short image\r\n");
        return 0;
    }

    if (fnv1a(dst, image_size) != checksum) {
        kfree(tables);
        slyzfs_close(fh);
        kserial_puts("[LINK] checksum mismatch\r\n");
        return 0;
    }

    slz_zero(dst + image_size, bss_size);

    /* 注册导出。 */
    int slot = -1;
    for (int i = 0; i < SLYS_LINK_MAX; i++) {
        if (!g_linked[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        kfree(tables);
        slyzfs_close(fh);
        kserial_puts("[LINK] module table full\r\n");
        return 0;
    }

    struct SlysLoadedModule *mod = &g_linked[slot];
    mod->used = 1;
    mod->load_addr = load_addr;
    mod->image_size = image_size;
    mod->bss_size = bss_size;
    mod->entry_addr = load_addr + entry_off;
    mod->export_count = export_count;

    int k = 0;
    while (name[k] && k < (int)SLYZ_MODULE_NAME_MAX - 1) {
        mod->name[k] = name[k];
        k++;
    }
    mod->name[k] = '\0';

    mod->exports = 0;
    if (export_count) {
        mod->exports = (struct SlyzSymbol *)kmalloc(export_count * (uint32_t)sizeof(struct SlyzSymbol));
        if (!mod->exports) {
            mod->used = 0;
            kfree(tables);
            slyzfs_close(fh);
            kserial_puts("[LINK] out of memory for exports\r\n");
            return 0;
        }
        for (uint32_t i = 0; i < export_count; i++) {
            mod->exports[i] = exports[i];
        }
    }

    /* 回填导入槽。 */
    for (uint32_t i = 0; i < import_count; i++) {
        uint32_t addr = 0;
        if (!link_find_symbol(imports[i].name, imports[i].version,
                              imports[i].kind, &addr)) {
            kserial_puts("[LINK] unresolved import: ");
            kserial_puts(imports[i].name);
            kserial_puts("\r\n");
            mod->used = 0;
            if (mod->exports) kfree(mod->exports);
            kfree(tables);
            slyzfs_close(fh);
            return 0;
        }
        if (imports[i].slot_offset + 8u > image_size) {
            mod->used = 0;
            if (mod->exports) kfree(mod->exports);
            kfree(tables);
            slyzfs_close(fh);
            kserial_puts("[LINK] import slot outside image\r\n");
            return 0;
        }
        *(volatile uint64_t *)(uintptr_t)(load_addr + imports[i].slot_offset) = addr;
    }

    /* 绝对地址重定位：镜像里的值作为 addend。 */
    for (uint32_t i = 0; i < reloc_count; i++) {
        uint32_t at = load_addr + relocs[i].offset;
        if (relocs[i].type == SLYZ_RELOC_ABS64) {
            if (relocs[i].offset + 8u > image_size) continue;
            uint64_t addend = *(volatile uint64_t *)(uintptr_t)at;
            *(volatile uint64_t *)(uintptr_t)at = addend + load_addr;
        } else if (relocs[i].type == SLYZ_RELOC_ABS32) {
            if (relocs[i].offset + 4u > image_size) continue;
            uint32_t addend = *(volatile uint32_t *)(uintptr_t)at;
            *(volatile uint32_t *)(uintptr_t)at = addend + load_addr;
        }
    }

    kfree(tables);
    slyzfs_close(fh);

    kserial_puts("[LINK] loaded ");
    kserial_puts(name);
    kserial_puts(" image=");
    kserial_dec32(image_size);
    kserial_puts(" exports=");
    kserial_dec32(export_count);
    kserial_puts("\r\n");

    *entry_out = mod->entry_addr;
    return 1;
}

int slys_load_dynamic(const char *path, void **entry) {
    if (!path || !entry) return 0;
    uint32_t addr = 0;
    if (!link_load_file(path, 0, &addr)) return 0;
    *entry = (void *)(uintptr_t)addr;
    return 1;
}

int slys_link_selftest(void) {
    void *entry = 0;
    if (!slys_load_dynamic("system\\modules\\demo.slyzd", &entry)) {
        kserial_puts("[LINK] self-test: demo.slyzd load failed\r\n");
        return 0;
    }

    SlysModuleEntryFn fn = (SlysModuleEntryFn)entry;
    if (fn) (void)fn(slys_kernel_api());

    kserial_puts("[LINK] self-test: demo.slyzd completed\r\n");
    return 1;
}
