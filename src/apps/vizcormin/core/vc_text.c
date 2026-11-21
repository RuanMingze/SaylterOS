/* ============================================================
 * vc_text.c - VizCormin 文本核心实现
 * ============================================================ */

#include "vc_text.h"
#include "vc_alloc.h"
#include <string.h>

#define VC_MAX_SNAPSHOTS 64

typedef struct {
    int      src;     /* 0 = 原始缓冲，1 = 追加缓冲 */
    uint32_t start;
    uint32_t len;
} VcPiece;

typedef struct {
    char    *data;
    uint32_t len;
} VcSnap;

struct VcText {
    char    *orig;
    uint32_t orig_len;
    uint32_t orig_cap;

    char    *add;
    uint32_t add_len;
    uint32_t add_cap;

    VcPiece *pieces;
    uint32_t np;
    uint32_t cap;
    uint32_t length;

    VcSnap   undo[VC_MAX_SNAPSHOTS];
    VcSnap   redo[VC_MAX_SNAPSHOTS];
    int      nu;
    int      nr;
};

/* ---------- 小工具 ---------- */

static int grow_buf(char **buf, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return 1;
    uint32_t nc = *cap ? *cap : 256u;
    while (nc < need) {
        if (nc > 0x7FFFFFFFu) return 0;
        nc *= 2u;
    }
    char *nb = (char *)vc_realloc(*buf, nc);
    if (!nb) return 0;
    *buf = nb;
    *cap = nc;
    return 1;
}

static int grow_pieces(VcText *t, uint32_t need) {
    if (need <= t->cap) return 1;
    uint32_t nc = t->cap ? t->cap : 16u;
    while (nc < need) nc *= 2u;
    VcPiece *np = (VcPiece *)vc_realloc(t->pieces, (size_t)nc * sizeof(VcPiece));
    if (!np) return 0;
    t->pieces = np;
    t->cap = nc;
    return 1;
}

static void clear_pieces(VcText *t) {
    t->np = 0;
    t->length = 0;
}

static int push_piece(VcText *t, int src, uint32_t start, uint32_t len) {
    if (len == 0) return 1;
    if (!grow_pieces(t, t->np + 1)) return 0;
    t->pieces[t->np].src = src;
    t->pieces[t->np].start = start;
    t->pieces[t->np].len = len;
    t->np++;
    t->length += len;
    return 1;
}

static void free_snapshots(VcSnap *s, int *n) {
    for (int i = 0; i < *n; i++) {
        if (s[i].data) vc_free(s[i].data);
        s[i].data = 0;
        s[i].len = 0;
    }
    *n = 0;
}

/* ---------- 公共 API ---------- */

VcText *vc_create(void) {
    VcText *t = (VcText *)vc_zalloc(sizeof(VcText));
    return t;
}

void vc_destroy(VcText *t) {
    if (!t) return;
    vc_free(t->orig);
    vc_free(t->add);
    vc_free(t->pieces);
    free_snapshots(t->undo, &t->nu);
    free_snapshots(t->redo, &t->nr);
    vc_free(t);
}

static uint32_t copy_range(const VcText *t, uint32_t pos, uint32_t len,
                           char *out, uint32_t cap, int copy) {
    uint32_t done = 0;
    uint32_t acc = 0;
    uint32_t end = pos + len;

    for (uint32_t i = 0; i < t->np && done < len; i++) {
        const VcPiece *p = &t->pieces[i];
        uint32_t p0 = acc;
        uint32_t p1 = acc + p->len;
        acc = p1;
        if (p1 <= pos) continue;
        if (p0 >= end) break;

        uint32_t s = pos > p0 ? pos : p0;
        uint32_t e = end < p1 ? end : p1;
        if (s >= e) continue;

        if (copy && done < cap) {
            uint32_t want = e - s;
            uint32_t room = cap - done;
            if (want > room) want = room;
            const char *src = p->src ? t->add : t->orig;
            memcpy(out + done, src + p->start + (s - p0), want);
        }
        done += e - s;
        if (!copy && done >= len) break;
    }
    return done;
}

uint32_t vc_copy(const VcText *t, uint32_t pos, uint32_t len,
                 char *out, uint32_t cap) {
    if (!t || !out || len == 0) return 0;
    if (pos >= t->length) return 0;
    if (pos + len > t->length) len = t->length - pos;
    return copy_range(t, pos, len, out, cap, 1);
}

uint32_t vc_copy_all(const VcText *t, char *out, uint32_t cap) {
    if (!t || !out) return 0;
    return vc_copy(t, 0, t->length, out, cap);
}

int vc_load(VcText *t, const char *data, uint32_t len) {
    if (!t) return 0;
    if (!grow_buf(&t->orig, &t->orig_cap, len)) return 0;
    if (len) memcpy(t->orig, data, len);
    t->orig_len = len;
    t->add_len = 0;
    clear_pieces(t);
    return len ? push_piece(t, 0, 0, len) : 1;
}

uint32_t vc_length(const VcText *t) {
    return t ? t->length : 0;
}

int vc_insert(VcText *t, uint32_t pos, const char *s, uint32_t len) {
    if (!t || (!s && len)) return 0;
    if (pos > t->length) return 0;
    if (len == 0) return 1;

    uint32_t add_start = t->add_len;
    if (!grow_buf(&t->add, &t->add_cap, t->add_len + len)) return 0;
    memcpy(t->add + t->add_len, s, len);
    t->add_len += len;

    /* 定位插入点 */
    uint32_t acc = 0;
    uint32_t idx = t->np;
    for (uint32_t i = 0; i < t->np; i++) {
        if (pos < acc + t->pieces[i].len) { idx = i; break; }
        acc += t->pieces[i].len;
    }

    if (idx == t->np) {
        if (!push_piece(t, 1, add_start, len)) return 0;
        return 1;
    }

    if (pos == acc) {
        if (!grow_pieces(t, t->np + 1)) return 0;
        memmove(&t->pieces[idx + 1], &t->pieces[idx],
                (size_t)(t->np - idx) * sizeof(VcPiece));
        t->pieces[idx].src = 1;
        t->pieces[idx].start = add_start;
        t->pieces[idx].len = len;
        t->np++;
        t->length += len;
        return 1;
    }

    /* 在 piece 内部：拆成两段，再插入新段 */
    uint32_t off = pos - acc;
    VcPiece left = t->pieces[idx];
    left.len = off;
    VcPiece right = t->pieces[idx];
    right.start += off;
    right.len -= off;

    if (!grow_pieces(t, t->np + 2)) return 0;
    /* 原 piece 之后的片段整体后移两位：idx 放左半，idx+1 放新文本，
     * idx+2 放右半。目标必须是 idx+3，否则会覆盖被移动的旧片段。 */
    memmove(&t->pieces[idx + 3], &t->pieces[idx + 1],
            (size_t)(t->np - idx - 1) * sizeof(VcPiece));
    t->pieces[idx] = left;
    t->pieces[idx + 1].src = 1;
    t->pieces[idx + 1].start = add_start;
    t->pieces[idx + 1].len = len;
    t->pieces[idx + 2] = right;
    t->np += 2;
    t->length += len;
    return 1;
}

int vc_erase(VcText *t, uint32_t pos, uint32_t len) {
    if (!t) return 0;
    if (pos > t->length) return 0;
    if (pos + len > t->length) len = t->length - pos;
    if (len == 0) return 1;

    uint32_t end = pos + len;
    uint32_t alloc_n = t->np + 2;
    VcPiece *out = (VcPiece *)vc_alloc((size_t)alloc_n * sizeof(VcPiece));
    if (!out) return 0;
    uint32_t nout = 0;
    uint32_t acc = 0;

    for (uint32_t i = 0; i < t->np; i++) {
        VcPiece p = t->pieces[i];
        uint32_t p0 = acc;
        uint32_t p1 = acc + p.len;
        acc = p1;

        if (p1 <= pos || p0 >= end) {
            out[nout++] = p;
            continue;
        }

        if (p0 < pos) {
            VcPiece left = p;
            left.len = pos - p0;
            if (left.len) out[nout++] = left;
        }
        if (p1 > end) {
            VcPiece right = p;
            right.start += (end - p0);
            right.len = p1 - end;
            if (right.len) out[nout++] = right;
        }
    }

    vc_free(t->pieces);
    t->pieces = out;
    t->np = nout;
    t->cap = alloc_n;
    t->length -= len;
    return 1;
}

uint32_t vc_char_size(const VcText *t, uint32_t pos) {
    if (!t || pos >= t->length) return 0;
    char b[4];
    uint32_t got = vc_copy(t, pos, 4, b, 4);
    if (got == 0) return 0;
    uint8_t c = (uint8_t)b[0];
    uint32_t n = 1;
    if ((c & 0x80) == 0) n = 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (n > got) n = got;
    return n;
}

uint32_t vc_line_count(const VcText *t) {
    if (!t) return 1;
    uint32_t lines = 1;
    for (uint32_t i = 0; i < t->length; i++) {
        char c;
        if (vc_copy(t, i, 1, &c, 1) == 1 && c == '\n') lines++;
    }
    return lines;
}

uint32_t vc_line_start(const VcText *t, uint32_t line) {
    if (!t) return 0;
    if (line == 0) return 0;
    uint32_t cur = 0;
    uint32_t pos = 0;
    uint32_t i = 0;
    while (i < t->length) {
        char c;
        if (vc_copy(t, i, 1, &c, 1) != 1) break;
        i++;
        if (c == '\n') {
            cur++;
            if (cur == line) { pos = i; break; }
        }
    }
    return pos;
}

uint32_t vc_line_of(const VcText *t, uint32_t pos) {
    if (!t) return 0;
    if (pos > t->length) pos = t->length;
    uint32_t line = 0;
    for (uint32_t i = 0; i < pos; i++) {
        char c;
        if (vc_copy(t, i, 1, &c, 1) == 1 && c == '\n') line++;
    }
    return line;
}

uint32_t vc_col_of(const VcText *t, uint32_t pos) {
    if (!t) return 0;
    uint32_t start = vc_line_start(t, vc_line_of(t, pos));
    uint32_t col = 0;
    uint32_t i = start;
    while (i < pos) {
        uint32_t n = vc_char_size(t, i);
        if (n == 0) break;
        i += n;
        if (i > pos) break;
        col++;
    }
    return col;
}

/* ---------- 撤销 / 重做 ---------- */

static int snapshot_current(VcText *t, VcSnap *out) {
    out->len = t->length;
    out->data = (char *)vc_alloc(t->length ? t->length : 1);
    if (!out->data) return 0;
    if (t->length) vc_copy_all(t, out->data, t->length);
    return 1;
}

int vc_undo_push(VcText *t) {
    if (!t) return 0;
    VcSnap s;
    if (!snapshot_current(t, &s)) return 0;

    if (t->nu == VC_MAX_SNAPSHOTS) {
        vc_free(t->undo[0].data);
        memmove(&t->undo[0], &t->undo[1],
                (size_t)(VC_MAX_SNAPSHOTS - 1) * sizeof(VcSnap));
        t->nu--;
    }
    t->undo[t->nu++] = s;
    free_snapshots(t->redo, &t->nr);
    return 1;
}

static int restore_snapshot(VcText *t, VcSnap *s) {
    if (!grow_buf(&t->orig, &t->orig_cap, s->len)) return 0;
    if (s->len) memcpy(t->orig, s->data, s->len);
    t->orig_len = s->len;
    t->add_len = 0;
    clear_pieces(t);
    return s->len ? push_piece(t, 0, 0, s->len) : 1;
}

int vc_undo(VcText *t) {
    if (!t || t->nu == 0) return 0;
    VcSnap cur;
    if (!snapshot_current(t, &cur)) return 0;

    VcSnap s = t->undo[--t->nu];
    if (!restore_snapshot(t, &s)) {
        vc_free(s.data);
        return 0;
    }
    vc_free(s.data);

    if (t->nr == VC_MAX_SNAPSHOTS) {
        vc_free(t->redo[0].data);
        memmove(&t->redo[0], &t->redo[1],
                (size_t)(VC_MAX_SNAPSHOTS - 1) * sizeof(VcSnap));
        t->nr--;
    }
    t->redo[t->nr++] = cur;
    return 1;
}

int vc_redo(VcText *t) {
    if (!t || t->nr == 0) return 0;
    VcSnap cur;
    if (!snapshot_current(t, &cur)) return 0;

    VcSnap s = t->redo[--t->nr];
    if (!restore_snapshot(t, &s)) {
        vc_free(s.data);
        return 0;
    }
    vc_free(s.data);

    if (t->nu == VC_MAX_SNAPSHOTS) {
        vc_free(t->undo[0].data);
        memmove(&t->undo[0], &t->undo[1],
                (size_t)(VC_MAX_SNAPSHOTS - 1) * sizeof(VcSnap));
        t->nu--;
    }
    t->undo[t->nu++] = cur;
    return 1;
}

int vc_can_undo(const VcText *t) { return t && t->nu > 0; }
int vc_can_redo(const VcText *t) { return t && t->nr > 0; }

/* ---------- 多光标 ----------
 * 关键点：所有编辑位置必须在任何修改之前快照下来。
 * 如果在循环里边编辑边读取 cursors[].pos，前面的插入会把后面
 * 光标的位置推走，导致在错误位置重复插入。 */

static void sort_u32_desc(uint32_t *a, int n) {
    for (int i = 1; i < n; i++) {
        uint32_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] < v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

int vc_insert_multi(VcText *t, VcCursor *cursors, int n,
                    const char *s, uint32_t len) {
    if (!t || !cursors || n <= 0 || len == 0) return 0;

    uint32_t *pos = (uint32_t *)vc_alloc((size_t)n * sizeof(uint32_t));
    if (!pos) return 0;
    for (int i = 0; i < n; i++) pos[i] = cursors[i].pos;
    sort_u32_desc(pos, n);

    int ok = 1;
    uint32_t last = 0xFFFFFFFFu;

    for (int k = 0; k < n && ok; k++) {
        uint32_t p = pos[k];
        if (p == last) continue;
        last = p;

        if (!vc_insert(t, p, s, len)) { ok = 0; break; }

        for (int j = 0; j < n; j++) {
            if (cursors[j].pos >= p) cursors[j].pos += len;
            if (cursors[j].anchor >= p) cursors[j].anchor += len;
        }
    }

    vc_free(pos);
    return ok;
}

struct VcSel {
    uint32_t s;
    uint32_t e;
};

static void sort_sel_asc(struct VcSel *a, int n) {
    for (int i = 1; i < n; i++) {
        struct VcSel v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].s > v.s) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

int vc_erase_multi(VcText *t, VcCursor *cursors, int n) {
    if (!t || !cursors || n <= 0) return 0;

    struct VcSel *sel = (struct VcSel *)vc_alloc((size_t)n * sizeof(struct VcSel));
    struct VcSel *merged = (struct VcSel *)vc_alloc((size_t)n * sizeof(struct VcSel));
    if (!sel || !merged) {
        vc_free(sel);
        vc_free(merged);
        return 0;
    }

    int m = 0;
    for (int i = 0; i < n; i++) {
        uint32_t a = cursors[i].anchor;
        uint32_t b = cursors[i].pos;
        uint32_t s = a < b ? a : b;
        uint32_t e = a < b ? b : a;
        if (s == e) continue;
        sel[m].s = s;
        sel[m].e = e;
        m++;
    }

    sort_sel_asc(sel, m);

    int nm = 0;
    for (int i = 0; i < m; i++) {
        if (nm > 0 && sel[i].s <= merged[nm - 1].e) {
            if (sel[i].e > merged[nm - 1].e) merged[nm - 1].e = sel[i].e;
        } else {
            merged[nm++] = sel[i];
        }
    }

    int ok = 1;
    for (int k = nm - 1; k >= 0 && ok; k--) {
        uint32_t s = merged[k].s;
        uint32_t e = merged[k].e;
        uint32_t d = e - s;

        if (!vc_erase(t, s, d)) { ok = 0; break; }

        for (int j = 0; j < n; j++) {
            if (cursors[j].pos >= e) cursors[j].pos -= d;
            else if (cursors[j].pos > s) cursors[j].pos = s;
            if (cursors[j].anchor >= e) cursors[j].anchor -= d;
            else if (cursors[j].anchor > s) cursors[j].anchor = s;
        }
    }

    vc_free(sel);
    vc_free(merged);
    return ok;
}
