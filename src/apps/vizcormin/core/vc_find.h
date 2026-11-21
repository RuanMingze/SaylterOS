/* ============================================================
 * vc_find.h - 查找引擎（字面量）
 * ------------------------------------------------------------
 * 支持大小写敏感、整词匹配、向前/向后、循环查找、全部匹配。
 * 正则表达式留到 Oniguruma 模块；现有接口不区分实现。
 * ============================================================ */

#ifndef VC_FIND_H
#define VC_FIND_H

#include <stdint.h>
#include "vc_text.h"

typedef struct {
    int case_sensitive;
    int whole_word;
} VcFindOptions;

typedef struct {
    uint32_t start;
    uint32_t len;
} VcRange;

/* 从 from 开始向后/向前查找；找不到时循环到另一端。
 * 返回 1 表示找到，0 表示全文没有匹配。 */
int vc_find_next(const VcText *t, const char *q, uint32_t qlen,
                 VcFindOptions opt, uint32_t from, VcRange *out);
int vc_find_prev(const VcText *t, const char *q, uint32_t qlen,
                 VcFindOptions opt, uint32_t from, VcRange *out);

/* 收集全部匹配，返回写入数量。 */
int vc_find_all(const VcText *t, const char *q, uint32_t qlen,
                VcFindOptions opt, VcRange *out, int max);

#endif /* VC_FIND_H */
