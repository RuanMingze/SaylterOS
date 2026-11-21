/* ============================================================
 * vc_alloc.c - 核心分配器实现
 * ============================================================ */

#include "vc_alloc.h"

#if defined(VC_MODULE)
static void *(*g_alloc_fn)(uint32_t) = 0;
static void (*g_free_fn)(void *) = 0;
#define VC_RAW_ALLOC(n) (g_alloc_fn ? g_alloc_fn((uint32_t)(n)) : 0)
#define VC_RAW_FREE(p)  do { if (g_free_fn) g_free_fn(p); } while (0)
void vc_alloc_bind(void *(*alloc_fn)(uint32_t), void (*free_fn)(void *)) {
    g_alloc_fn = alloc_fn;
    g_free_fn = free_fn;
}
#elif defined(VC_KERNEL)
#include "mem.h"
#define VC_RAW_ALLOC(n) kmalloc((uint32_t)(n))
#define VC_RAW_FREE(p)  kfree(p)
void vc_alloc_bind(void *(*alloc_fn)(uint32_t), void (*free_fn)(void *)) {
    (void)alloc_fn;
    (void)free_fn;
}
#else
#include <stdlib.h>
#define VC_RAW_ALLOC(n) malloc(n)
#define VC_RAW_FREE(p)  free(p)
void vc_alloc_bind(void *(*alloc_fn)(uint32_t), void (*free_fn)(void *)) {
    (void)alloc_fn;
    (void)free_fn;
}
#endif

#define VC_ALLOC_HDR 16u

static void vc_copy_bytes(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void vc_zero_bytes(void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}

void *vc_alloc(uint32_t bytes) {
    uint8_t *raw = (uint8_t *)VC_RAW_ALLOC(bytes + VC_ALLOC_HDR);
    if (!raw) return 0;
    *(uint32_t *)raw = bytes;
    return raw + VC_ALLOC_HDR;
}

void *vc_zalloc(uint32_t bytes) {
    void *p = vc_alloc(bytes);
    if (p) vc_zero_bytes(p, bytes);
    return p;
}

void *vc_realloc(void *ptr, uint32_t bytes) {
    if (!ptr) return vc_alloc(bytes);
    if (bytes == 0) {
        vc_free(ptr);
        return 0;
    }

    uint8_t *raw = (uint8_t *)ptr - VC_ALLOC_HDR;
    uint32_t old = *(uint32_t *)raw;

    uint8_t *np = (uint8_t *)VC_RAW_ALLOC(bytes + VC_ALLOC_HDR);
    if (!np) return 0;
    *(uint32_t *)np = bytes;
    vc_copy_bytes(np + VC_ALLOC_HDR, ptr, old < bytes ? old : bytes);
    VC_RAW_FREE(raw);
    return np + VC_ALLOC_HDR;
}

void vc_free(void *ptr) {
    if (!ptr) return;
    VC_RAW_FREE((uint8_t *)ptr - VC_ALLOC_HDR);
}
