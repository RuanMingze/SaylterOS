/* ============================================================
 * vc_atlas.h - 字形图集分配器
 * ------------------------------------------------------------
 * 字形只光栅化一次，按 key 放进图集；渲染时只做矩形拷贝。
 * 分配器采用 shelf packing：每行一个“货架”，高度取该行最大
 * 字形高度，宽度向右增长。图集满时整体清空重来（字形缓存
 * 丢失后按需重烘焙，不会返回错误坐标）。
 * ============================================================ */

#ifndef VC_ATLAS_H
#define VC_ATLAS_H

#include <stdint.h>

typedef struct VcAtlas VcAtlas;

VcAtlas *vc_atlas_create(uint32_t width, uint32_t height);
void     vc_atlas_destroy(VcAtlas *a);

/* 查询/分配。返回 1 表示坐标有效；0 表示参数非法。 */
int      vc_atlas_get(VcAtlas *a, uint32_t key, uint32_t w, uint32_t h,
                      uint32_t *x, uint32_t *y);

void     vc_atlas_reset(VcAtlas *a);

uint32_t vc_atlas_entries(const VcAtlas *a);
uint32_t vc_atlas_used_height(const VcAtlas *a);

#endif /* VC_ATLAS_H */
