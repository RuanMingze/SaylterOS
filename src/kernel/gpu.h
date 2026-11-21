/* ============================================================
 * gpu.h - 真实 GPU（Intel Gen9 核显）2D 后端
 * ------------------------------------------------------------
 * 对外只暴露"能不能用"和"搬运一个矩形"，调用方（vbe.c）在
 * 每次尝试后都能回退到 CPU 路径，所以这里的每一个返回值都是
 * 可判定的：0 = 成功，-1 = 不可用/超时/被禁用。
 *
 * 初始化分两段：
 *   gpu_init()  : 在 vbe_init() 之后、tty_init() 之前调用。
 *                 只做 PCI 探测、MMIO/LFB 的缓存类型设置、
 *                 扫描面 GPU 地址发现。失败不打印屏幕。
 *   gpu_start() : 在 tty_init() 之后调用。分配 GGTT、初始化
 *                 BCS ring、跑离屏自测与屏幕 4x4 自测。
 *                 任何一步失败都会把后端永久禁用。
 * ============================================================ */

#ifndef GPU_H
#define GPU_H

#include <stdint.h>

typedef struct {
    uint8_t  present;        /* 探测到受支持的 Intel 显示设备 */
    uint8_t  ready;          /* blitter 自测通过，可以下发命令 */
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint32_t mmio_phys;      /* BAR0：寄存器 + GGTT 镜像 */
    uint32_t lfb_phys;       /* VBE 扫描面物理地址 */
    uint32_t lfb_gpu_va;     /* 扫描面在 GGTT 中的 GPU 地址 */
    uint32_t bb_gpu_va;      /* backbuffer 在 GGTT 中的 GPU 地址 */
    uint32_t bb_pages;       /* backbuffer 映射页数 */
    uint32_t ring_gpu_va;    /* BCS ring 的 GPU 地址 */
    uint32_t ring_size;      /* BCS ring 字节数 */
    uint32_t blits;          /* 成功下发的 2D 命令次数 */
    uint32_t bytes_copied;   /* 累计 GPU 搬运字节数 */
    uint32_t timeouts;       /* 超时次数（>0 说明后端已被禁用） */
    int      mtrr_type;      /* LFB 区间在 MTRR 中的有效类型（诊断用） */
    int      pat_ok;         /* CPU 支持 PAT */
    int      wc_enabled;     /* LFB 已成功标记为写合并 */
} gpu_info_t;

/* 探测硬件并设置缓存属性；1 = 找到受支持的 GPU，0 = 没有 */
int gpu_init(void);

/* 初始化 GGTT / ring / 自测；1 = 可用，0 = 回退 CPU */
int gpu_start(void);

/* 后端当前是否可用（热路径每次都会问） */
int gpu_ready(void);

/* 查询状态快照 */
void gpu_get_info(gpu_info_t *out);

/* 把 backbuffer 的 (x,y,w,h) 搬到扫描面同一位置。
 * 返回 0 成功，-1 表示不可用或失败（调用方必须走 CPU 路径）。 */
int gpu_copy_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* 整屏搬运 */
int gpu_copy_all(void);

/* 永久关闭后端（超时/自测失败后内部也会调用） */
void gpu_shutdown(void);

/* 型号名（静态字符串，可安全打印） */
const char *gpu_type_name(void);

#endif /* GPU_H */
