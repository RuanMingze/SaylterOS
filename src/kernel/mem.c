/* ============================================================
 * mem.c - 内核堆分配器（first-fit + 相邻块合并）
 * ------------------------------------------------------------
 * 堆区固定在 0x100000（1MB）起 1MB。选择该位置的原因：
 *   1. 内核镜像与 BSS 都在 640KB 以下，1MB 以上不会被覆盖；
 *   2. 传统 PC 的 1MB 以上一定是可用 RAM（E820 type 1），
 *      不需要引入完整内存探测即可安全使用；
 *   3. 不占用 .bss，避免把内核静态区顶过 0xA0000 红线。
 * ============================================================ */

#include "mem.h"

#define KHEAP_BASE   0x100000u
#define KHEAP_SIZE   (1024u * 1024u)
#define MEM_ALIGN    8u

struct MemBlock {
    uint32_t size;              /* 数据区字节数（不含块头） */
    uint32_t free;              /* 1 = 未使用 */
    struct MemBlock *next;
};

static struct MemBlock *g_head = 0;
static int g_initialized = 0;

static uint32_t g_total = 0;
static uint32_t g_used = 0;
static uint32_t g_peak = 0;
static uint32_t g_allocs = 0;
static uint32_t g_frees = 0;
static uint32_t g_failed = 0;

static uint32_t mem_align_up(uint32_t value) {
    return (value + MEM_ALIGN - 1u) & ~(MEM_ALIGN - 1u);
}

void mem_init(void) {
    if (g_initialized) return;

    g_head = (struct MemBlock *)(uintptr_t)KHEAP_BASE;
    g_head->size = KHEAP_SIZE - (uint32_t)sizeof(struct MemBlock);
    g_head->free = 1;
    g_head->next = 0;

    g_total = KHEAP_SIZE;
    g_used = 0;
    g_peak = 0;
    g_allocs = 0;
    g_frees = 0;
    g_failed = 0;
    g_initialized = 1;
}

void *kmalloc(uint32_t size) {
    if (!g_initialized) mem_init();
    if (size == 0) return 0;

    uint32_t need = mem_align_up(size);

    struct MemBlock *block = g_head;
    while (block) {
        if (block->free && block->size >= need) {
            /* 如果剩余空间够放一个新块头 + 最小数据，就分裂 */
            if (block->size >= need + (uint32_t)sizeof(struct MemBlock) + MEM_ALIGN) {
                struct MemBlock *split =
                    (struct MemBlock *)((uint8_t *)block + sizeof(struct MemBlock) + need);
                split->size = block->size - need - (uint32_t)sizeof(struct MemBlock);
                split->free = 1;
                split->next = block->next;

                block->size = need;
                block->next = split;
            }

            block->free = 0;
            g_used += block->size + (uint32_t)sizeof(struct MemBlock);
            g_allocs++;
            if (g_used > g_peak) g_peak = g_used;
            return (uint8_t *)block + sizeof(struct MemBlock);
        }
        block = block->next;
    }

    g_failed++;
    return 0;
}

void kfree(void *ptr) {
    if (!ptr || !g_initialized) return;

    struct MemBlock *target =
        (struct MemBlock *)((uint8_t *)ptr - sizeof(struct MemBlock));
    if (target->free) return;   /* 重复释放保护 */

    target->free = 1;
    g_used -= target->size + (uint32_t)sizeof(struct MemBlock);
    g_frees++;

    /* 向后合并连续空闲块 */
    struct MemBlock *block = g_head;
    while (block) {
        if (block->free && block->next && block->next->free) {
            block->size += (uint32_t)sizeof(struct MemBlock) + block->next->size;
            block->next = block->next->next;
            continue;   /* 合并后可能还能继续合并 */
        }
        block = block->next;
    }
}

void mem_stats(uint32_t *total, uint32_t *used, uint32_t *peak,
               uint32_t *allocs, uint32_t *frees, uint32_t *failed) {
    if (!g_initialized) mem_init();
    if (total) *total = g_total;
    if (used) *used = g_used;
    if (peak) *peak = g_peak;
    if (allocs) *allocs = g_allocs;
    if (frees) *frees = g_frees;
    if (failed) *failed = g_failed;
}

uint32_t mem_free_bytes(void) {
    if (!g_initialized) mem_init();
    return g_total > g_used ? g_total - g_used : 0;
}

uint32_t mem_heap_base(void) {
    return KHEAP_BASE;
}

uint32_t mem_heap_size(void) {
    return KHEAP_SIZE;
}
