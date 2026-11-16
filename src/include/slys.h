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

#define SLYS_VERSION      1
#define SLYS_HEADER_SIZE  64

/* 驱动加载地址：位于内核 BSS（20 MB）之后、VBE backbuffer（10 MB）
 * 与内核堆（4..10 MB）之外。4 GB 恒等映射，256 MB 内存下安全。 */
#define SLYS_LOAD_ADDR    0x02000000u
#define SLYS_MAX_IMAGE    (4u * 1024u * 1024u)
#define SLYS_MAX_BSS      (16u * 1024u * 1024u)

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

/* ---- 内核侧加载器 ---- */
/* 加载 path 指向的 .slys，复制到头部声明的固定地址并校验。
 * 成功时 *entry 指向模块入口，返回 1；失败返回 0。 */
int slys_load(const char *path, void **entry, char *name, int name_cap);

/* 内核提供的 API 表（内存、时间、日志、文件、任务、锁）。 */
const struct SlysKernelApi *slys_kernel_api(void);

/* 内核与驱动共享的音频锁。 */
void slys_lock(void);
void slys_unlock(void);

#endif /* SLYS_H */
