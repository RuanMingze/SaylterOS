/* ============================================================
 * vc_view.c - VizCormin 视图模型实现
 * ============================================================ */

#include "vc_view.h"
#include "vc_alloc.h"
#include <string.h>

struct VcView {
    const VcText *t;
    uint32_t cols;
    uint32_t rows;
    int      wrap;
    uint32_t first;

    uint32_t *line_off;
    uint32_t *line_len;
    uint32_t  nlines;
    uint32_t  lcap;

    VcVisualLine *vis;
    uint32_t      nvis;
    uint32_t      vcap;
};

/* ---------- 基础 ---------- */

static uint32_t read_cp(const VcText *t, uint32_t pos, uint32_t n) {
    char b[4];
    uint32_t got = vc_copy(t, pos, n, b, 4);
    if (got == 0) return 0;
    const uint8_t *p = (const uint8_t *)b;
    if (n == 1) return p[0];
    if (n == 2) return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (n == 3) return ((uint32_t)(p[0] & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return ((uint32_t)(p[0] & 0x07) << 18) |
           ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

/* 显示宽度：Tab 到下一个停位，CJK/假名等宽字符占 2 列。 */
static uint32_t char_cols(const VcText *t, uint32_t pos, uint32_t *bytes) {
    uint32_t n = vc_char_size(t, pos);
    if (n == 0) { *bytes = 0; return 0; }
    *bytes = n;
    uint32_t cp = read_cp(t, pos, n);
    if (cp == '\t') return 1;   /* 由调用方按停位计算 */
    if (cp >= 0x2E80u) return 2;
    if (cp < 0x20u) return 1;
    return 1;
}

static uint32_t tab_advance(uint32_t col) {
    return VC_TAB_SIZE - (col % VC_TAB_SIZE);
}

static uint32_t cols_between(const VcText *t, uint32_t from, uint32_t to) {
    uint32_t col = 0;
    uint32_t i = from;
    while (i < to) {
        uint32_t n = 0;
        uint32_t w = char_cols(t, i, &n);
        if (n == 0 || i + n > to) break;
        char b[4];
        if (vc_copy(t, i, n, b, 4) != n) break;
        if ((uint8_t)b[0] == '\t') col += tab_advance(col);
        else col += w;
        i += n;
    }
    return col;
}

/* ---------- 逻辑行 ---------- */

static int push_line(VcView *v, uint32_t off, uint32_t len) {
    if (v->nlines == v->lcap) {
        uint32_t nc = v->lcap ? v->lcap * 2u : 64u;
        uint32_t *no = (uint32_t *)vc_realloc(v->line_off, (size_t)nc * sizeof(uint32_t));
        if (!no) return 0;
        v->line_off = no;
        uint32_t *nl = (uint32_t *)vc_realloc(v->line_len, (size_t)nc * sizeof(uint32_t));
        if (!nl) return 0;
        v->line_len = nl;
        v->lcap = nc;
    }
    v->line_off[v->nlines] = off;
    v->line_len[v->nlines] = len;
    v->nlines++;
    return 1;
}

static int rebuild_lines(VcView *v) {
    v->nlines = 0;
    const VcText *t = v->t;
    uint32_t total = t ? vc_length(t) : 0;
    uint32_t start = 0;

    if (total == 0) {
        if (!push_line(v, 0, 0)) return 0;
        return 1;
    }

    for (uint32_t i = 0; i < total; i++) {
        char c;
        if (vc_copy(t, i, 1, &c, 1) != 1) break;
        if (c == '\n') {
            if (!push_line(v, start, i - start)) return 0;
            start = i + 1;
        }
    }
    if (start <= total) {
        if (!push_line(v, start, total - start)) return 0;
    }
    return 1;
}

/* ---------- 可视行 ---------- */

static int push_vis(VcView *v, uint32_t off, uint32_t len,
                    uint32_t col_start, uint32_t col_count, uint32_t line) {
    if (v->nvis == v->vcap) {
        uint32_t nc = v->vcap ? v->vcap * 2u : 128u;
        VcVisualLine *nv = (VcVisualLine *)vc_realloc(v->vis, (size_t)nc * sizeof(VcVisualLine));
        if (!nv) return 0;
        v->vis = nv;
        v->vcap = nc;
    }
    VcVisualLine *d = &v->vis[v->nvis++];
    d->offset = off;
    d->length = len;
    d->col_start = col_start;
    d->col_count = col_count;
    d->line = line;
    return 1;
}

static int rebuild_visual(VcView *v) {
    v->nvis = 0;
    if (!v->t) {
        return push_vis(v, 0, 0, 0, 0, 0);
    }

    for (uint32_t li = 0; li < v->nlines; li++) {
        uint32_t off = v->line_off[li];
        uint32_t len = v->line_len[li];

        if (!v->wrap || v->cols == 0) {
            uint32_t cols = cols_between(v->t, off, off + len);
            if (!push_vis(v, off, len, 0, cols, li)) return 0;
            continue;
        }

        uint32_t i = 0;
        uint32_t seg_start = 0;
        uint32_t col = 0;          /* 逻辑行内列（用于 Tab 与选区） */
        uint32_t seg_col_start = 0;
        uint32_t seg_cols = 0;     /* 当前可视行内列（用于换行判断） */

        while (i < len) {
            uint32_t n = 0;
            uint32_t w = char_cols(v->t, off + i, &n);
            if (n == 0) break;
            uint32_t adv = w;
            if (n == 1) {
                char c;
                vc_copy(v->t, off + i, 1, &c, 1);
                if (c == '\t') adv = tab_advance(col);
            }

            /* 换行判断必须用“当前可视行内的列”，不能用逻辑列；
             * 否则换行后列号没有归零，每个字符都会再触发一次换行。 */
            if (seg_cols > 0 && seg_cols + adv > v->cols) {
                if (!push_vis(v, off + seg_start, i - seg_start,
                              seg_col_start, seg_cols, li)) return 0;
                seg_start = i;
                seg_col_start = col;
                seg_cols = 0;
            }

            col += adv;
            seg_cols += adv;
            i += n;
        }

        if (!push_vis(v, off + seg_start, len - seg_start,
                      seg_col_start, seg_cols, li)) return 0;
    }
    return 1;
}

/* ---------- 公共 API ---------- */

VcView *vc_view_create(uint32_t cols, uint32_t rows) {
    VcView *v = (VcView *)vc_zalloc(sizeof(VcView));
    if (!v) return 0;
    v->cols = cols;
    v->rows = rows;
    v->wrap = 1;
    return v;
}

void vc_view_destroy(VcView *v) {
    if (!v) return;
    vc_free(v->line_off);
    vc_free(v->line_len);
    vc_free(v->vis);
    vc_free(v);
}

void vc_view_set_text(VcView *v, const VcText *t) {
    if (!v) return;
    v->t = t;
    if (!rebuild_lines(v) || !rebuild_visual(v)) {
        v->nlines = 0;
        v->nvis = 0;
    }
    if (v->first >= v->nvis) v->first = v->nvis ? v->nvis - 1 : 0;
}

const VcText *vc_view_text(const VcView *v) {
    return v ? v->t : 0;
}

void vc_view_set_size(VcView *v, uint32_t cols, uint32_t rows) {
    if (!v) return;
    v->cols = cols;
    v->rows = rows;
    if (!rebuild_visual(v)) v->nvis = 0;
    if (v->first >= v->nvis) v->first = v->nvis ? v->nvis - 1 : 0;
}

uint32_t vc_view_cols(const VcView *v) { return v ? v->cols : 0; }
uint32_t vc_view_rows(const VcView *v) { return v ? v->rows : 0; }

void vc_view_set_wrap(VcView *v, int on) {
    if (!v || v->wrap == (on != 0)) return;
    v->wrap = on != 0;
    if (!rebuild_visual(v)) v->nvis = 0;
    if (v->first >= v->nvis) v->first = v->nvis ? v->nvis - 1 : 0;
}

int vc_view_wrap(const VcView *v) { return v ? v->wrap : 0; }

uint32_t vc_view_visual_lines(const VcView *v) { return v ? v->nvis : 0; }

int vc_view_line_info(const VcView *v, uint32_t visual, VcVisualLine *out) {
    if (!v || visual >= v->nvis || !out) return 0;
    *out = v->vis[visual];
    return 1;
}

uint32_t vc_view_first_visual(const VcView *v) { return v ? v->first : 0; }

void vc_view_scroll_to(VcView *v, uint32_t visual) {
    if (!v) return;
    uint32_t max_first = 0;
    if (v->nvis > v->rows) max_first = v->nvis - v->rows;
    if (visual > max_first) visual = max_first;
    v->first = visual;
}

void vc_view_scroll_by(VcView *v, int delta) {
    if (!v) return;
    int64_t target = (int64_t)v->first + delta;
    if (target < 0) target = 0;
    if (target > 0x7FFFFFFF) target = 0x7FFFFFFF;
    vc_view_scroll_to(v, (uint32_t)target);
}

int vc_view_offset_to_pos(const VcView *v, uint32_t offset,
                          uint32_t *visual, uint32_t *col) {
    if (!v || !v->nvis) return 0;
    if (offset > vc_length(v->t)) offset = vc_length(v->t);

    for (uint32_t i = 0; i < v->nvis; i++) {
        const VcVisualLine *vl = &v->vis[i];
        uint32_t a = vl->offset;
        uint32_t b = vl->offset + vl->length;

        int inside = (offset >= a && offset < b);
        int at_end = (offset == b);
        int next_same = (i + 1 < v->nvis && v->vis[i + 1].line == vl->line);

        if (inside || (at_end && !next_same)) {
            if (visual) *visual = i;
            if (col) *col = vl->col_start + cols_between(v->t, a,
                                                         offset < b ? offset : b);
            return 1;
        }
        if (at_end && next_same) {
            if (visual) *visual = i + 1;
            if (col) *col = v->vis[i + 1].col_start;
            return 1;
        }
    }

    if (visual) *visual = v->nvis - 1;
    if (col) *col = v->vis[v->nvis - 1].col_start + v->vis[v->nvis - 1].col_count;
    return 1;
}

uint32_t vc_view_pos_to_offset(const VcView *v, uint32_t visual, uint32_t col) {
    if (!v || visual >= v->nvis) return 0;
    const VcVisualLine *vl = &v->vis[visual];
    if (col <= vl->col_start) return vl->offset;

    uint32_t want = col - vl->col_start;
    uint32_t acc = 0;
    uint32_t i = 0;
    while (i < vl->length) {
        uint32_t n = 0;
        uint32_t w = char_cols(v->t, vl->offset + i, &n);
        if (n == 0) break;
        uint32_t adv = w;
        if (n == 1) {
            char c;
            vc_copy(v->t, vl->offset + i, 1, &c, 1);
            if (c == '\t') adv = tab_advance(acc);
        }
        if (acc + adv > want) return vl->offset + i;
        acc += adv;
        i += n;
        if (acc == want) return vl->offset + i;
    }
    return vl->offset + vl->length;
}

uint32_t vc_view_hit_test(const VcView *v, int px, int py,
                          uint32_t cell_w, uint32_t cell_h) {
    if (!v || !cell_w || !cell_h || !v->nvis) return 0;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    uint32_t row = (uint32_t)py / cell_h;
    uint32_t col = (uint32_t)px / cell_w;
    uint32_t visual = v->first + row;
    if (visual >= v->nvis) visual = v->nvis - 1;
    return vc_view_pos_to_offset(v, visual, col);
}

int vc_view_selection_spans(const VcView *v, uint32_t visual,
                            uint32_t sel_start, uint32_t sel_end,
                            VcSpan *out, int max) {
    if (!v || visual >= v->nvis || !out || max <= 0) return 0;
    if (sel_start == sel_end) return 0;

    const VcVisualLine *vl = &v->vis[visual];
    uint32_t a = vl->offset;
    uint32_t b = vl->offset + vl->length;
    uint32_t s = sel_start > a ? sel_start : a;
    uint32_t e = sel_end < b ? sel_end : b;

    int include_newline = (sel_start <= b && sel_end > b);
    if (s >= e && !include_newline) return 0;

    uint32_t start_col = vl->col_start + cols_between(v->t, a, s);
    uint32_t end_col = include_newline
        ? vl->col_start + vl->col_count
        : vl->col_start + cols_between(v->t, a, e);

    if (start_col > vl->col_start + vl->col_count) start_col = vl->col_start + vl->col_count;
    if (end_col > vl->col_start + vl->col_count) end_col = vl->col_start + vl->col_count;
    if (end_col <= start_col) return 0;

    out[0].start_col = start_col;
    out[0].end_col = end_col;
    return 1;
}
