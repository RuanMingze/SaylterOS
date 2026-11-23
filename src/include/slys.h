/* ============================================================
 * slys.h - SaylterOS 可加载驱动（.slys）格式与音频驱动 ABI
 * ------------------------------------------------------------
 * .slys 是一段在固定物理地址执行的裸机二进制。加载器只做：
 *   1. 读取并校验 64 字节头；
 *   2. 把镜像复制到 load_addr；
 *   3. 清零 bss；
 *   4. 调用 entry 指向的入口函数，传入内核 API 表。
 *
 * 驱动不直接引用内核符号，只通过 SlysKernelApi 里的函数指针
 * 使用内存、日志、文件、任务与同步原语。这样驱动可以在不重
 * 新链接内核的情况下独立构建与替换。
 * ============================================================ */

#ifndef SLYS_H
#define SLYS_H

#include <stdint.h>

#define SLYS_MAGIC0 'S'
#define SLYS_MAGIC1 'L'
#define SLYS_MAGIC2 'Y'
#define SLYS_MAGIC3 'S'
#define SLYS_MAGIC4 'D'
#define SLYS_MAGIC5 'R'
#define SLYS_MAGIC6 'V'
#define SLYS_MAGIC7 0x00

/* .slys 容器格式版本：只随 64 字节头布局变化而递增。
 * 它与 struct SlysKernelApi 的 version 字段是两套独立版本号：
 * 后者表示函数指针表的长度/语义，追加字段时才递增。 */
#define SLYS_VERSION      1
#define SLYS_HEADER_SIZE  64

/* 驱动加载地址：位于内核 BSS（20 MB）之后、VBE backbuffer（10 MB）
 * 与内核堆（4..10 MB）之外。4 GB 恒等映射，256 MB 内存下安全。 */
#define SLYS_LOAD_ADDR    0x02000000u
#define SLYS_MAX_IMAGE    (4u * 1024u * 1024u)
/* 文件系统模块自带 48MB 堆（512GB 卷的位图约 16.8MB，
 * 加上 MFT/索引缓冲仍在 48MB 以内）。 */
#define SLYS_MAX_BSS      (64u * 1024u * 1024u)

#pragma pack(push, 1)
struct SlysHeader {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t load_addr;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry_offset;
    uint32_t api_offset;
    uint32_t checksum;
    uint32_t reserved;
    char     name[24];
};
#pragma pack(pop)

/* ============================================================
 * 内核提供给驱动的 API
 * ============================================================ */

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

    /* 把整个文件读进 buf，返回字节数；失败返回 -1。 */
    int (*read_file)(const char *path, uint8_t *buf, int cap);
    /* 文件是否存在。 */
    int (*file_exists)(const char *path);

    /* ---- 预装应用宿主 ----
     * 以下字段只能追加在末尾：现有模块按旧偏移访问前面的字段，
     * 追加不会改变它们的 ABI。 */
    void *(*fs_open)(const char *path, const char *mode);
    int   (*fs_read)(void *fh, uint8_t *buf, int n);
    int   (*fs_write)(void *fh, const uint8_t *buf, int n);
    void  (*fs_close)(void *fh);
    int   (*fs_stat)(const char *path, void *out);   /* 280 字节目录项 */

    void (*fb_fill_rect)(int x, int y, int w, int h, uint32_t color);
    int  (*fb_draw_cell)(int x, int y, int cw, int ch, uint32_t cp, uint32_t fg);
    void (*fb_swap)(void);
    uint32_t (*fb_width)(void);
    uint32_t (*fb_height)(void);

    char (*kbd_read)(void);

    /* shell 启动应用时传入的参数（文件路径）；无参数时返回 ""。 */
    const char *(*arg)(void);

    /* ---- 窗口化宿主 ----
     * 预装 GUI 应用不直接改写全屏：以下接口把绘制限制在应用窗口的
     * 内容区，并由窗口系统统一处理标题栏、拖动、缩放与层级。
     * 坐标相对内容区左上角；TTY 会话中这些指针为 0，应用应回退到
     * fb_* 全屏接口。同样只能在末尾追加。 */
    void (*win_fill_rect)(int x, int y, int w, int h, uint32_t color);
    int  (*win_draw_cell)(int x, int y, int cw, int ch,
                          uint32_t cp, uint32_t fg);
    uint32_t (*win_width)(void);
    uint32_t (*win_height)(void);
    void (*win_invalidate)(void);   /* 请求窗口系统重绘本窗口 */
    void (*win_close)(void);        /* 请求关闭本窗口 */

    /* ---- POSIX 段（版本 2 追加，供 musl libc 使用）----
     * 返回值一律遵循 POSIX：>=0 成功，负数为 -errno。
     * fd 的分配、标准流语义由模块侧的 port 层与这里共同维护。 */
    int (*posix_write)(int fd, const void *buf, int len);
    int (*posix_read)(int fd, void *buf, int len);
    int (*posix_open)(const char *path, int flags);
    int (*posix_close)(int fd);
    int64_t (*posix_lseek)(int fd, int64_t offset, int whence);
    int (*posix_fstat)(int fd, void *st);
    uint64_t (*posix_time_ms)(void);
    void *(*posix_map)(uint64_t len);
    void (*posix_unmap)(void *ptr, uint64_t len);
};

/* ============================================================
 * 音频驱动导出表
 * ------------------------------------------------------------
 * 版本号由内核与驱动共同校验：内核拿到入口返回的表后先比对
 * version，不匹配就拒绝使用，避免新旧驱动混用时按错误的
 * 函数指针布局调用。
 * ============================================================ */

#define SLYS_AUDIO_ABI 1

struct SlysAudioDriver {
    uint32_t version;

    int      (*init)(const struct SlysKernelApi *api);
    int      (*available)(void);

    int      (*play)(const char *path);
    void     (*stop)(void);
    void     (*pause)(int paused);
    int      (*state)(void);
    uint32_t (*position_ms)(void);
    uint32_t (*duration_ms)(void);

    void (*set_volume)(int volume);
    int  (*volume)(void);

    void (*pump)(void);
    void (*beep)(uint32_t freq, uint32_t ms);

    /* 后台任务入口：循环解码并填充 DMA，内部调用 api->yield()。 */
    void (*run)(void);
};

typedef const struct SlysAudioDriver *(*SlysEntryFn)(
    const struct SlysKernelApi *api);

/* 通用模块入口：返回值由模块类型决定（驱动导出表 / 0）。 */
typedef void *(*SlysModuleEntryFn)(const struct SlysKernelApi *api);

/* ============================================================
 * 窗口化应用导出表（VizCormin）
 * ------------------------------------------------------------
 * 入口只做绑定并返回这张表，不再自己死循环：
 *   - GUI 会话：宿主窗口每帧调用 render，按键/点击转发到 key/mouse；
 *   - TTY 会话：shell 自己驱动 render/key，并负责 fb_swap。
 * 坐标全部相对窗口内容区，尺寸由 win_width/win_height 给出。
 * ============================================================ */

#define SLYS_VIZCORMIN_ABI 1

struct SlysVizCorminHost {
    uint32_t version;

    /* 绑定 API 并创建会话；path 为空时打开空白文档。失败返回 <0。 */
    int  (*init)(const struct SlysKernelApi *api, const char *path);
    void (*render)(void);
    /* 原始键码：ASCII 控制码与 0xF1.. 特殊键；由模块内部映射。 */
    void (*key)(uint32_t raw);
    void (*mouse)(int x, int y, int button);
    /* 窗口内容区尺寸变化（首次布局也会调用）。 */
    void (*resize)(void);
    void (*shutdown)(void);
    /* TTY 驱动循环用它判断用户是否按了退出键（Ctrl+Q）。 */
    int  (*finished)(void);
};

typedef const struct SlysVizCorminHost *(*SlysVizCorminEntryFn)(
    const struct SlysKernelApi *api);

/* ---- 内核侧加载器 ---- */
/* 加载 path 指向的 .slys，复制到头部声明的固定地址并校验。
 * 成功时 *entry 指向模块入口，返回 1；失败返回 0。 */
int slys_load(const char *path, void **entry, char *name, int name_cap);

/* 从启动盘的固定原始 LBA 加载模块。
 * 文件系统模块必须在"还没有文件系统"时就能启动，因此走裸扇区：
 * 头部 64 字节 + 镜像，随后按 bss_size 清零。 */
int slys_load_raw(uint32_t lba, void **entry, char *name, int name_cap);

/* 加载 .slyzd 动态模块：解析依赖、回填导入槽、应用重定位。
 * 依赖从 system\modules\<name>.slyzd 读取。 */
int slys_load_dynamic(const char *path, void **entry);

/* 动态链接自检：加载 demo.slyzd，调用 libmath 的 slyz_add，
 * 结果通过内核 API 打到串口。返回 1 表示链路可用。 */
int slys_link_selftest(void);

/* 内核提供的 API 表（内存、时间、日志、文件、任务、锁）。 */
const struct SlysKernelApi *slys_kernel_api(void);

/* shell 在加载预装应用前设置启动参数。 */
void slys_set_arg(const char *arg);

/* 内核与驱动共享的音频锁。 */
void slys_lock(void);
void slys_unlock(void);

#endif /* SLYS_H */
