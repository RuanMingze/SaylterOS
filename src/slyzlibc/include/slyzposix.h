/* ============================================================
 * slyzposix.h - SaylterOS 内核 API 的 Slyztrix 侧镜像
 * ------------------------------------------------------------
 * 字段顺序必须与 SaylterOS 的 src/include/slys.h 完全一致。
 * 内核只能在其末尾追加字段；本文件同步追加，绝不插入或重排。
 * 版本号不匹配时模块入口会拒绝启动，避免按错误偏移调用函数。
 * ============================================================ */

#ifndef SLYZ_POSIX_H
#define SLYZ_POSIX_H

#include <stdint.h>

#define SLYZ_POSIX_ABI 2

struct SlysKernelApi {
    uint32_t version;

    void *(*alloc)(uint32_t bytes);
    void  (*free)(void *ptr);

    uint32_t (*now_ms)(void);
    void     (*sleep_ms)(uint32_t ms);

    void (*log)(const char *s);
    void (*log_u32)(uint32_t v);

    void (*lock)(void);
    void (*unlock)(void);
    void (*yield)(void);

    int (*read_file)(const char *path, uint8_t *buf, int cap);
    int (*file_exists)(const char *path);

    void *(*fs_open)(const char *path, const char *mode);
    int   (*fs_read)(void *fh, uint8_t *buf, int n);
    int   (*fs_write)(void *fh, const uint8_t *buf, int n);
    void  (*fs_close)(void *fh);
    int   (*fs_stat)(const char *path, void *out);

    void (*fb_fill_rect)(int x, int y, int w, int h, uint32_t color);
    int  (*fb_draw_cell)(int x, int y, int cw, int ch, uint32_t cp, uint32_t fg);
    void (*fb_swap)(void);
    uint32_t (*fb_width)(void);
    uint32_t (*fb_height)(void);

    char (*kbd_read)(void);

    const char *(*arg)(void);

    void (*win_fill_rect)(int x, int y, int w, int h, uint32_t color);
    int  (*win_draw_cell)(int x, int y, int cw, int ch,
                          uint32_t cp, uint32_t fg);
    uint32_t (*win_width)(void);
    uint32_t (*win_height)(void);
    void (*win_invalidate)(void);
    void (*win_close)(void);

    /* ---- POSIX 段（ABI 2 追加，供 musl libc 使用）----
     * fd 语义由 port 层维护：0/1/2 是标准流，>=3 由内核分配句柄。
     * 所有返回值遵循 POSIX：>=0 成功，负数见下方错误码约定。 */
    int (*posix_write)(int fd, const void *buf, int len);
    int (*posix_read)(int fd, void *buf, int len);
    /* flags 使用 POSIX O_* 位；返回 fd 或负错误码。 */
    int (*posix_open)(const char *path, int flags);
    int (*posix_close)(int fd);
    int64_t (*posix_lseek)(int fd, int64_t offset, int whence);
    /* st 指向 Linux x86_64 struct stat；成功返回 0。 */
    int (*posix_fstat)(int fd, void *st);
    /* 毫秒级时间戳（开机以来的单调计数由内核决定）。 */
    uint64_t (*posix_time_ms)(void);
    /* 匿名内存：返回页对齐地址，失败返回 0。 */
    void *(*posix_map)(uint64_t len);
    void (*posix_unmap)(void *ptr, uint64_t len);
};

/* 内核错误码约定：POSIX 段返回 -errno（如 -2 = -ENOENT）。 */

/* ---- 跨 ABI 调用桥 ----
 * 内核用 MinGW（Microsoft x64 ABI）编译，本模块用 x86_64-elf
 * （SysV ABI）编译。所有通过 SlysKernelApi 函数指针发起的调用都必须
 * 经过 port/msabi.s 的 thunk，否则参数会落在错误的寄存器里：
 * SysV 第一个参数在 rdi，MS 第一个参数在 rcx。 */
long __slyz_api_call0(const void *fn);
long __slyz_api_call1(const void *fn, long a1);
long __slyz_api_call2(const void *fn, long a1, long a2);
long __slyz_api_call3(const void *fn, long a1, long a2, long a3);
long __slyz_api_call4(const void *fn, long a1, long a2, long a3, long a4);
long __slyz_api_call5(const void *fn, long a1, long a2, long a3, long a4,
                      long a5);
long __slyz_api_call6(const void *fn, long a1, long a2, long a3, long a4,
                      long a5, long a6);

/* 由 crt 在调用 __libc_start_main 之前绑定。 */
void __slyz_bind_api(const struct SlysKernelApi *api);
const struct SlysKernelApi *__slyz_api(void);

#endif /* SLYZ_POSIX_H */
