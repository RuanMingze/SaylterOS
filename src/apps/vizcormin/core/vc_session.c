/* ============================================================
 * vc_session.c - VizCormin 文档与标签页会话实现
 * ============================================================ */

#include "vc_session.h"
#include "vc_alloc.h"
#include <string.h>

struct VcSession {
    VcFileOps  ops;
    int        has_ops;
    uint32_t   cols;
    uint32_t   rows;
    VcDocument *docs;
    int        count;
    int        cap;
    int        active;
};

static int path_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void path_copy(char *dst, const char *src) {
    int i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i < VC_PATH_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void doc_vc_free(VcDocument *d) {
    if (!d) return;
    if (d->text) vc_destroy(d->text);
    if (d->view) vc_view_destroy(d->view);
    d->text = 0;
    d->view = 0;
}

static int session_push(VcSession *s, const char *path) {
    if (s->count == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        VcDocument *nd = (VcDocument *)vc_realloc(s->docs, (size_t)nc * sizeof(VcDocument));
        if (!nd) return -1;
        s->docs = nd;
        s->cap = nc;
    }
    VcDocument *d = &s->docs[s->count];
    memset(d, 0, sizeof(*d));
    d->text = vc_create();
    d->view = vc_view_create(s->cols, s->rows);
    if (!d->text || !d->view) {
        doc_vc_free(d);
        return -1;
    }
    path_copy(d->path, path);
    vc_view_set_text(d->view, d->text);
    s->count++;
    return s->count - 1;
}

VcSession *vc_session_create(const VcFileOps *ops, uint32_t cols, uint32_t rows) {
    VcSession *s = (VcSession *)vc_zalloc(sizeof(VcSession));
    if (!s) return 0;
    s->cols = cols ? cols : 80;
    s->rows = rows ? rows : 24;
    if (ops) {
        s->ops = *ops;
        s->has_ops = 1;
    }
    if (session_push(s, "") < 0) {
        vc_free(s);
        return 0;
    }
    s->active = 0;
    return s;
}

void vc_session_destroy(VcSession *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) doc_vc_free(&s->docs[i]);
    vc_free(s->docs);
    vc_free(s);
}

void vc_session_set_size(VcSession *s, uint32_t cols, uint32_t rows) {
    if (!s) return;
    s->cols = cols ? cols : 1;
    s->rows = rows ? rows : 1;
    for (int i = 0; i < s->count; i++) {
        vc_view_set_size(s->docs[i].view, s->cols, s->rows);
    }
}

int vc_session_count(const VcSession *s) { return s ? s->count : 0; }
int vc_session_active_index(const VcSession *s) { return s ? s->active : -1; }

VcDocument *vc_session_active(VcSession *s) {
    if (!s || s->active < 0 || s->active >= s->count) return 0;
    return &s->docs[s->active];
}

VcDocument *vc_session_at(VcSession *s, int index) {
    if (!s || index < 0 || index >= s->count) return 0;
    return &s->docs[index];
}

int vc_session_open(VcSession *s, const char *path) {
    if (!s || !path || !s->has_ops || !s->ops.read) return -1;

    for (int i = 0; i < s->count; i++) {
        if (path_eq(s->docs[i].path, path)) {
            s->active = i;
            return i;
        }
    }

    char *data = 0;
    uint32_t len = 0;
    if (!s->ops.read(s->ops.user, path, &data, &len)) return -1;

    int idx = session_push(s, path);
    if (idx < 0) {
        vc_free(data);
        return -1;
    }

    VcDocument *d = &s->docs[idx];
    int ok = vc_load(d->text, data ? data : "", len);
    vc_free(data);
    if (!ok) {
        doc_vc_free(d);
        s->count--;
        return -1;
    }

    vc_view_set_text(d->view, d->text);
    d->dirty = 0;
    d->cursor = 0;
    d->anchor = 0;
    d->x_scroll = 0;
    s->active = idx;
    return idx;
}

int vc_session_new(VcSession *s) {
    if (!s) return -1;
    int idx = session_push(s, "");
    if (idx < 0) return -1;
    s->active = idx;
    return idx;
}

int vc_session_save(VcSession *s, int index, const char *path) {
    if (!s || index < 0 || index >= s->count) return 0;
    if (!s->has_ops || !s->ops.write) return 0;

    VcDocument *d = &s->docs[index];
    const char *target = path ? path : d->path;
    if (!target || !target[0]) return 0;

    uint32_t len = vc_length(d->text);
    char *buf = (char *)vc_alloc(len ? len : 1);
    if (!buf) return 0;
    if (len) vc_copy_all(d->text, buf, len);

    int ok = s->ops.write(s->ops.user, target, buf, len);
    vc_free(buf);
    if (!ok) return 0;

    if (path) path_copy(d->path, path);
    d->dirty = 0;
    return 1;
}

void vc_session_close(VcSession *s, int index) {
    if (!s || index < 0 || index >= s->count) return;
    doc_vc_free(&s->docs[index]);
    for (int i = index; i < s->count - 1; i++) {
        s->docs[i] = s->docs[i + 1];
    }
    s->count--;
    if (s->count == 0) {
        if (session_push(s, "") < 0) {
            s->active = -1;
            return;
        }
        s->active = 0;
        return;
    }
    if (s->active >= s->count) s->active = s->count - 1;
    if (s->active < 0) s->active = 0;
}

void vc_session_activate(VcSession *s, int index) {
    if (!s || index < 0 || index >= s->count) return;
    s->active = index;
}

/* ---------- 编辑包装 ---------- */

static void doc_refresh_view(VcDocument *d) {
    vc_view_set_text(d->view, d->text);
    uint32_t len = vc_length(d->text);
    if (d->cursor > len) d->cursor = len;
    if (d->anchor > len) d->anchor = len;
}

int vc_doc_insert(VcSession *s, VcDocument *d, const char *bytes, uint32_t len) {
    (void)s;
    if (!d || !bytes || len == 0) return 0;
    if (!vc_undo_push(d->text)) return 0;
    if (!vc_insert(d->text, d->cursor, bytes, len)) return 0;
    d->cursor += len;
    d->anchor = d->cursor;
    d->dirty = 1;
    doc_refresh_view(d);
    return 1;
}

int vc_doc_erase_selection(VcSession *s, VcDocument *d) {
    (void)s;
    if (!d) return 0;

    uint32_t a = d->anchor;
    uint32_t b = d->cursor;
    uint32_t start = a < b ? a : b;
    uint32_t end = a < b ? b : a;

    if (start == end) {
        if (start == 0) return 0;
        uint32_t n = vc_char_size(d->text, start - 1);
        if (n == 0) n = 1;
        start -= n;
    }

    if (!vc_undo_push(d->text)) return 0;
    if (!vc_erase(d->text, start, end - start)) return 0;

    d->cursor = start;
    d->anchor = start;
    d->dirty = 1;
    doc_refresh_view(d);
    return 1;
}

int vc_doc_undo(VcDocument *d) {
    if (!d || !vc_undo(d->text)) return 0;
    d->dirty = 1;
    doc_refresh_view(d);
    return 1;
}

int vc_doc_redo(VcDocument *d) {
    if (!d || !vc_redo(d->text)) return 0;
    d->dirty = 1;
    doc_refresh_view(d);
    return 1;
}

int vc_doc_replace_all(VcDocument *d, const char *q, uint32_t qlen,
                       VcFindOptions opt, const char *rep, uint32_t rep_len) {
    if (!d || !q || qlen == 0) return 0;

    uint32_t len = vc_length(d->text);
    int max = (int)(len / qlen) + 2;
    VcRange *hits = (VcRange *)vc_alloc((size_t)max * sizeof(VcRange));
    if (!hits) return 0;

    int n = vc_find_all(d->text, q, qlen, opt, hits, max);
    if (n <= 0) {
        vc_free(hits);
        return 0;
    }
    if (!vc_undo_push(d->text)) {
        vc_free(hits);
        return 0;
    }

    for (int i = n - 1; i >= 0; i--) {
        if (!vc_erase(d->text, hits[i].start, hits[i].len)) break;
        if (rep_len && !vc_insert(d->text, hits[i].start, rep, rep_len)) break;
    }

    d->cursor = hits[0].start;
    d->anchor = d->cursor;
    d->dirty = 1;
    doc_refresh_view(d);
    vc_free(hits);
    return n;
}

int vc_doc_replace_one(VcDocument *d, const char *q, uint32_t qlen,
                       VcFindOptions opt, const char *rep, uint32_t rep_len,
                       uint32_t from) {
    if (!d || !q || qlen == 0) return 0;

    VcRange hit;
    if (!vc_find_next(d->text, q, qlen, opt, from, &hit)) return 0;
    if (!vc_undo_push(d->text)) return 0;
    if (!vc_erase(d->text, hit.start, hit.len)) return 0;
    if (rep_len && !vc_insert(d->text, hit.start, rep, rep_len)) return 0;

    d->cursor = hit.start + rep_len;
    d->anchor = d->cursor;
    d->dirty = 1;
    doc_refresh_view(d);
    return 1;
}

/* ---------- 光标移动 ---------- */

void vc_doc_select_all(VcDocument *d) {
    if (!d) return;
    d->anchor = 0;
    d->cursor = vc_length(d->text);
}

void vc_doc_move(VcDocument *d, int unit, int dir, int extend) {
    if (!d || dir == 0) return;

    uint32_t len = vc_length(d->text);
    uint32_t pos = d->cursor;

    if (unit == 0) {                       /* 字符 */
        if (dir < 0) {
            if (pos == 0) return;
            uint32_t n = vc_char_size(d->text, pos - 1);
            pos -= (n ? n : 1);
        } else {
            if (pos >= len) return;
            uint32_t n = vc_char_size(d->text, pos);
            pos += (n ? n : 1);
            if (pos > len) pos = len;
        }
    } else if (unit == 1) {                /* 可视行 */
        uint32_t vis = 0, col = 0;
        vc_view_offset_to_pos(d->view, pos, &vis, &col);
        if (dir < 0) {
            if (vis > 0) vis--;
        } else {
            if (vis + 1 < vc_view_visual_lines(d->view)) vis++;
        }
        pos = vc_view_pos_to_offset(d->view, vis, col);
    } else if (unit == 2) {                /* 页 */
        uint32_t vis = 0, col = 0;
        uint32_t rows = vc_view_rows(d->view);
        vc_view_offset_to_pos(d->view, pos, &vis, &col);
        if (dir < 0) {
            vis = (vis > rows) ? vis - rows : 0;
        } else {
            vis += rows;
            uint32_t total = vc_view_visual_lines(d->view);
            if (total && vis >= total) vis = total - 1;
        }
        pos = vc_view_pos_to_offset(d->view, vis, col);
    } else {                               /* 文档首尾 */
        pos = (dir < 0) ? 0 : len;
    }

    if (pos > len) pos = len;
    d->cursor = pos;
    if (!extend) d->anchor = pos;

    /* 保证光标在可视区 */
    uint32_t vis = 0, col = 0;
    vc_view_offset_to_pos(d->view, pos, &vis, &col);
    uint32_t first = vc_view_first_visual(d->view);
    uint32_t rows = vc_view_rows(d->view);
    if (vis < first) vc_view_scroll_to(d->view, vis);
    else if (rows && vis >= first + rows) vc_view_scroll_to(d->view, vis - rows + 1);
}
