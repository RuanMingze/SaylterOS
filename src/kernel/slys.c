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
#include "smp.h"
#include "mem.h"
#include "net.h"
#include "task.h"

/* SlyzFS */
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);
extern int   slyzfs_stat(const char *path, void *out);

extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

/* 目录项布局（只用于读文件大小，与 SlyzFS 71 字节定义一致）。 */
#pragma pack(push, 1)
struct SlysDirEntry {
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

static smp_lock_t g_slys_lock = 0;

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
    if (load_addr != SLYS_LOAD_ADDR) {
        slyzfs_close(fh);
        kserial_puts("[SLYS] unsupported load address\r\n");
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
    for (uint32_t i = 0; i < bss_size; i++) {
        dst[image_size + i] = 0;
    }

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

static const struct SlysKernelApi g_api = {
    1,
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
    api_file_exists
};

const struct SlysKernelApi *slys_kernel_api(void) {
    return &g_api;
}
