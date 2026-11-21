/* ============================================================
 * vc_alloc.h - 核心分配器适配
 * ------------------------------------------------------------
 * 宿主测试：malloc/free。
 * 模块构建：定义 VC_KERNEL，使用内核 kmalloc/kfree。
 *
 * 每个块前面带 16 字节头部记录大小，因此 vc_realloc 不需要
 * 调用方提供旧大小；头部同时保证返回指针仍然 8 字节对齐。
 * ============================================================ */

#ifndef VC_ALLOC_H
#define VC_ALLOC_H

#include <stdint.h>

void *vc_alloc(uint32_t bytes);
void *vc_zalloc(uint32_t bytes);
void *vc_realloc(void *ptr, uint32_t bytes);
void  vc_free(void *ptr);

/* 独立模块（VC_MODULE）启动时绑定内核 API 的 alloc/free。 */
void  vc_alloc_bind(void *(*alloc_fn)(uint32_t), void (*free_fn)(void *));

#endif /* VC_ALLOC_H */
