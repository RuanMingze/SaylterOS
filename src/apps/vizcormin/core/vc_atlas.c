/* ============================================================
 * vc_atlas.c - 字形图集分配器实现
 * ============================================================ */

#include "vc_atlas.h"
#include "vc_alloc.h"
#include <string.h>

typedef struct {
    uint32_t y;
    uint32_t h;
    uint32_t x;
} VcShelf;

typedef struct {
    uint32_t key;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} VcGlyphEntry;

struct VcAtlas {
    uint32_t width;
    uint32_t height;
    uint32_t top;          /* 已使用到的 y */

    VcShelf *shelves;
    uint32_t nshelf;
    uint32_t scap;

    VcGlyphEntry *entries;
    uint32_t nentry;
    uint32_t ecap;
};

#define VC_ATLAS_PAD 1u

VcAtlas *vc_atlas_create(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return 0;
    VcAtlas *a = (VcAtlas *)vc_zalloc(sizeof(VcAtlas));
    if (!a) return 0;
    a->width = width;
    a->height = height;
    return a;
}

void vc_atlas_destroy(VcAtlas *a) {
    if (!a) return;
    vc_free(a->shelves);
    vc_free(a->entries);
    vc_free(a);
}

void vc_atlas_reset(VcAtlas *a) {
    if (!a) return;
    a->top = 0;
    a->nshelf = 0;
    a->nentry = 0;
}

uint32_t vc_atlas_entries(const VcAtlas *a) { return a ? a->nentry : 0; }
uint32_t vc_atlas_used_height(const VcAtlas *a) { return a ? a->top : 0; }

static int push_shelf(VcAtlas *a, uint32_t y, uint32_t h) {
    if (a->nshelf == a->scap) {
        uint32_t nc = a->scap ? a->scap * 2u : 16u;
        VcShelf *ns = (VcShelf *)vc_realloc(a->shelves, (size_t)nc * sizeof(VcShelf));
        if (!ns) return 0;
        a->shelves = ns;
        a->scap = nc;
    }
    a->shelves[a->nshelf].y = y;
    a->shelves[a->nshelf].h = h;
    a->shelves[a->nshelf].x = 0;
    a->nshelf++;
    return 1;
}

static int push_entry(VcAtlas *a, uint32_t key, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h) {
    if (a->nentry == a->ecap) {
        uint32_t nc = a->ecap ? a->ecap * 2u : 64u;
        VcGlyphEntry *ne = (VcGlyphEntry *)vc_realloc(a->entries,
                                                   (size_t)nc * sizeof(VcGlyphEntry));
        if (!ne) return 0;
        a->entries = ne;
        a->ecap = nc;
    }
    VcGlyphEntry *e = &a->entries[a->nentry++];
    e->key = key;
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
    return 1;
}

static int alloc_once(VcAtlas *a, uint32_t key, uint32_t w, uint32_t h,
                      uint32_t *x, uint32_t *y) {
    for (uint32_t i = 0; i < a->nshelf; i++) {
        VcShelf *s = &a->shelves[i];
        if (s->h < h) continue;
        if (s->x + w + VC_ATLAS_PAD > a->width) continue;
        *x = s->x;
        *y = s->y;
        s->x += w + VC_ATLAS_PAD;
        return push_entry(a, key, *x, *y, w, h);
    }

    if (a->top + h + VC_ATLAS_PAD > a->height) return 0;
    if (!push_shelf(a, a->top, h)) return 0;
    VcShelf *s = &a->shelves[a->nshelf - 1];
    *x = 0;
    *y = s->y;
    s->x = w + VC_ATLAS_PAD;
    a->top += h + VC_ATLAS_PAD;
    return push_entry(a, key, *x, *y, w, h);
}

int vc_atlas_get(VcAtlas *a, uint32_t key, uint32_t w, uint32_t h,
                 uint32_t *x, uint32_t *y) {
    if (!a || !x || !y || w == 0 || h == 0) return 0;
    if (w > a->width || h > a->height) return 0;

    for (uint32_t i = 0; i < a->nentry; i++) {
        if (a->entries[i].key == key) {
            *x = a->entries[i].x;
            *y = a->entries[i].y;
            return 1;
        }
    }

    if (alloc_once(a, key, w, h, x, y)) return 1;

    /* 图集满：整体清空后重试一次。 */
    vc_atlas_reset(a);
    return alloc_once(a, key, w, h, x, y);
}
