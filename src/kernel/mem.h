/* ============================================================
 * mem.h - 内核堆分配器
 * ------------------------------------------------------------
 * 使用 1MB 以上的物理内存作为堆区（0x100000 起 1MB），
 * 不占用 .bss —— 当前内核 BSS 已接近 640KB 红线，没有空间再放数组。
 * 分配器为 first-fit + 相邻合并，提供总/已用/峰值统计。
 * ============================================================ */

#ifndef MEM_H
#define MEM_H

#include <stdint.h>

/* 初始化堆：在 KHEAP_BASE 处建立一个大空闲块。重复调用无副作用。 */
void mem_init(void);

/* 分配 / 释放。size 为 0 或内存不足返回 0。 */
void *kmalloc(uint32_t size);
void kfree(void *ptr);

/* 统计信息 */
void mem_stats(uint32_t *total, uint32_t *used, uint32_t *peak,
               uint32_t *allocs, uint32_t *frees, uint32_t *failed);

/* 当前空闲字节数 */
uint32_t mem_free_bytes(void);

/* 堆区物理范围，供内存命令展示 */
uint32_t mem_heap_base(void);
uint32_t mem_heap_size(void);

#endif /* MEM_H */
