/* ============================================================
 * vc_view.h - VizCormin 视图模型
 * ------------------------------------------------------------
 * 负责“文档坐标 <-> 屏幕坐标”的全部换算：
 *   - 逻辑行索引（按 '\n'）；
 *   - 软换行后的可视行；
 *   - 横向滚动、竖向滚动；
 *   - UTF-8 列宽、Tab 停位、宽字符；
 *   - 像素命中测试；
 *   - 选区到每行高亮跨度。
 *
 * 渲染层只读取可视行与跨度，不再自己做文本遍历。
 * ============================================================ */

#ifndef VC_VIEW_H
#define VC_VIEW_H

#include <stdint.h>
#include "vc_text.h"

#define VC_TAB_SIZE 4

typedef struct {
    uint32_t offset;      /* 可视行起始字节 */
    uint32_t length;      /* 可视行字节数（不含换行） */
    uint32_t col_start;   /* 在逻辑行中的起始显示列 */
    uint32_t col_count;   /* 本可视行占用列数 */
    uint32_t line;        /* 逻辑行号 */
} VcVisualLine;

typedef struct {
    uint32_t start_col;
    uint32_t end_col;
} VcSpan;

typedef struct VcView VcView;

VcView *vc_view_create(uint32_t cols, uint32_t rows);
void    vc_view_destroy(VcView *v);

/* 文本变化后必须调用，重建逻辑行索引。 */
void    vc_view_set_text(VcView *v, const VcText *t);
const VcText *vc_view_text(const VcView *v);

void    vc_view_set_size(VcView *v, uint32_t cols, uint32_t rows);
uint32_t vc_view_cols(const VcView *v);
uint32_t vc_view_rows(const VcView *v);

/* 软换行开关，默认开。 */
void    vc_view_set_wrap(VcView *v, int on);
int     vc_view_wrap(const VcView *v);

uint32_t vc_view_visual_lines(const VcView *v);
int      vc_view_line_info(const VcView *v, uint32_t visual, VcVisualLine *out);

/* 竖向滚动（按可视行）。 */
uint32_t vc_view_first_visual(const VcView *v);
void     vc_view_scroll_to(VcView *v, uint32_t visual);
void     vc_view_scroll_by(VcView *v, int delta);

/* 文档偏移 <-> 可视行/列。 */
int      vc_view_offset_to_pos(const VcView *v, uint32_t offset,
                               uint32_t *visual, uint32_t *col);
uint32_t vc_view_pos_to_offset(const VcView *v, uint32_t visual, uint32_t col);

/* 像素命中测试：坐标相对于文本区左上角。 */
uint32_t vc_view_hit_test(const VcView *v, int px, int py,
                          uint32_t cell_w, uint32_t cell_h);

/* 某个可视行在 [sel_start, sel_end) 选区内的列跨度。
 * 返回写入 out 的跨度数（0 表示该行没有选区）。 */
int      vc_view_selection_spans(const VcView *v, uint32_t visual,
                                 uint32_t sel_start, uint32_t sel_end,
                                 VcSpan *out, int max);

#endif /* VC_VIEW_H */
