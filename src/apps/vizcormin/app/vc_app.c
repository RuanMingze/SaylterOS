/* ============================================================
 * vc_app.c - VizCormin 应用外壳实现
 * ============================================================ */

#include "vc_app.h"
#include "../core/vc_alloc.h"
#include "../core/vc_find.h"

typedef struct {
    VcSurface base;
    int       ox;
    int       oy;
} OffsetCtx;

struct VcApp {
    VcSession  *s;
    VcCommands *cmds;
    VcRenderer *edit;
    VcTheme     theme;
    VcSurface   full;

    OffsetCtx   octx;
    VcSurface   edit_surface;

    uint32_t width;
    uint32_t height;
    uint32_t cw;
    uint32_t ch;

    uint32_t tab_h;
    uint32_t status_h;

    int      find_active;
    char     find_q[128];
    uint32_t find_len;

    uint32_t last_cols;
    uint32_t last_rows;
};

/* ---------- 后端偏移包装 ---------- */

static void off_fill(void *user, int x, int y, int w, int h, uint32_t color) {
    OffsetCtx *o = (OffsetCtx *)user;
    if (!o || !o->base.fill_rect) return;
    o->base.fill_rect(o->base.user, o->ox + x, o->oy + y, w, h, color);
}

static void off_glyph(void *user, int x, int y, uint32_t cp, uint32_t color) {
    OffsetCtx *o = (OffsetCtx *)user;
    if (!o || !o->base.draw_glyph) return;
    o->base.draw_glyph(o->base.user, o->ox + x, o->oy + y, cp, color);
}

/* ---------- 小工具 ---------- */

static uint32_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static uint32_t cp_cols(uint32_t cp) {
    return cp >= 0x2E80u ? 2u : 1u;
}

static void draw_text(VcApp *a, const VcSurface *s, int x, int y,
                      const char *text, uint32_t color) {
    if (!text) return;
    uint32_t i = 0;
    int cx = x;
    while (text[i]) {
        uint8_t b = (uint8_t)text[i];
        uint32_t n = 1;
        uint32_t cp = b;
        if ((b & 0xE0) == 0xC0) { n = 2; cp = b & 0x1Fu; }
        else if ((b & 0xF0) == 0xE0) { n = 3; cp = b & 0x0Fu; }
        else if ((b & 0xF8) == 0xF0) { n = 4; cp = b & 0x07u; }
        for (uint32_t k = 1; k < n; k++) {
            uint8_t c = (uint8_t)text[i + k];
            if ((c & 0xC0) != 0x80) { n = k; break; }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        s->draw_glyph(s->user, cx, y, cp, color);
        cx += (int)(cp_cols(cp) * a->cw);
        i += n;
    }
}

static const char *base_name(const char *path) {
    if (!path) return "";
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

static uint32_t utf8_prev(const char *s, uint32_t len) {
    if (len == 0) return 0;
    uint32_t i = len - 1;
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
    return i;
}

/* ---------- 布局 ---------- */

static void app_layout(VcApp *a, uint32_t *ed_y, uint32_t *ed_h,
                       uint32_t *cols, uint32_t *rows) {
    uint32_t find_h = a->find_active ? a->ch + 6u : 0u;
    uint32_t y = a->tab_h + find_h;
    uint32_t h = (a->height > y + a->status_h) ? a->height - y - a->status_h : 0;
    uint32_t c = 0, r = 0;
    if (a->width > 2u * a->theme.padding_x + a->theme.gutter_cols * a->cw) {
        c = (a->width - 2u * a->theme.padding_x -
             a->theme.gutter_cols * a->cw) / a->cw;
    }
    if (h > 2u * a->theme.padding_y) r = (h - 2u * a->theme.padding_y) / a->ch;
    if (c == 0) c = 1;
    if (r == 0) r = 1;
    *ed_y = y;
    *ed_h = h;
    *cols = c;
    *rows = r;
}

static void ensure_cursor_visible(VcDocument *d) {
    uint32_t vis = 0, col = 0;
    vc_view_offset_to_pos(d->view, d->cursor, &vis, &col);
    uint32_t first = vc_view_first_visual(d->view);
    uint32_t rows = vc_view_rows(d->view);
    uint32_t cols = vc_view_cols(d->view);
    if (vis < first) vc_view_scroll_to(d->view, vis);
    else if (rows && vis >= first + rows) vc_view_scroll_to(d->view, vis - rows + 1);
    if (col < d->x_scroll) d->x_scroll = col;
    else if (cols && col >= d->x_scroll + cols) d->x_scroll = col - cols + 1;
}

/* ---------- 命令回调 ---------- */

static int cmd_save(void *user, VcSession *s) {
    (void)user;
    return vc_session_save(s, vc_session_active_index(s), 0);
}

static int cmd_undo(void *user, VcSession *s) {
    (void)user;
    return vc_doc_undo(vc_session_active(s));
}

static int cmd_redo(void *user, VcSession *s) {
    (void)user;
    return vc_doc_redo(vc_session_active(s));
}

static int cmd_select_all(void *user, VcSession *s) {
    (void)user;
    vc_doc_select_all(vc_session_active(s));
    return 1;
}

static int cmd_new(void *user, VcSession *s) {
    (void)user;
    return vc_session_new(s) >= 0;
}

static int cmd_close(void *user, VcSession *s) {
    (void)user;
    vc_session_close(s, vc_session_active_index(s));
    return 1;
}

static int cmd_find(void *user, VcSession *s) {
    (void)s;
    VcApp *a = (VcApp *)user;
    a->find_active = 1;
    a->find_len = 0;
    a->find_q[0] = '\0';
    return 1;
}

static void app_register_commands(VcApp *a) {
    vc_commands_register(a->cmds, "save", cmd_save, a);
    vc_commands_register(a->cmds, "undo", cmd_undo, a);
    vc_commands_register(a->cmds, "redo", cmd_redo, a);
    vc_commands_register(a->cmds, "select-all", cmd_select_all, a);
    vc_commands_register(a->cmds, "new", cmd_new, a);
    vc_commands_register(a->cmds, "close", cmd_close, a);
    vc_commands_register(a->cmds, "find", cmd_find, a);

    vc_commands_bind(a->cmds, VC_MOD_CTRL, 's', "save");
    vc_commands_bind(a->cmds, VC_MOD_CTRL, 'z', "undo");
    vc_commands_bind(a->cmds, VC_MOD_CTRL, 'y', "redo");
    vc_commands_bind(a->cmds, VC_MOD_CTRL, 'a', "select-all");
    vc_commands_bind(a->cmds, VC_MOD_CTRL, 'n', "new");
    vc_commands_bind(a->cmds, VC_MOD_CTRL, 'w', "close");
    vc_commands_bind(a->cmds, VC_MOD_CTRL, 'f', "find");
}

/* ---------- 生命周期 ---------- */

VcApp *vc_app_create(const VcFileOps *ops, uint32_t width, uint32_t height,
                     uint32_t cell_w, uint32_t cell_h) {
    if (!cell_w || !cell_h) return 0;
    VcApp *a = (VcApp *)vc_zalloc(sizeof(VcApp));
    if (!a) return 0;

    a->cw = cell_w;
    a->ch = cell_h;
    a->width = width;
    a->height = height;
    a->tab_h = cell_h + 6u;
    a->status_h = cell_h + 4u;

    a->theme.bg = 0x101014;
    a->theme.fg = 0xE9EFF7;
    a->theme.dim = 0x8EA0B4;
    a->theme.cursor = 0xFFD24A;
    a->theme.selection = 0x2F4C8A;
    a->theme.current_line = 0x1A1D24;
    a->theme.gutter_bg = 0x15181E;
    a->theme.gutter_fg = 0x6E7A8A;
    a->theme.cell_w = cell_w;
    a->theme.cell_h = cell_h;
    a->theme.padding_x = 2;
    a->theme.padding_y = 2;
    a->theme.gutter_cols = 2;

    uint32_t ed_y = 0, ed_h = 0, cols = 1, rows = 1;
    app_layout(a, &ed_y, &ed_h, &cols, &rows);

    a->s = vc_session_create(ops, cols, rows);
    a->cmds = vc_commands_create();
    if (!a->s || !a->cmds) {
        vc_app_destroy(a);
        return 0;
    }
    app_register_commands(a);
    return a;
}

void vc_app_destroy(VcApp *a) {
    if (!a) return;
    if (a->edit) vc_render_destroy(a->edit);
    if (a->cmds) vc_commands_destroy(a->cmds);
    if (a->s) vc_session_destroy(a->s);
    vc_free(a);
}

void vc_app_set_size(VcApp *a, uint32_t width, uint32_t height) {
    if (!a) return;
    a->width = width;
    a->height = height;
    uint32_t ed_y = 0, ed_h = 0, cols = 1, rows = 1;
    app_layout(a, &ed_y, &ed_h, &cols, &rows);
    vc_session_set_size(a->s, cols, rows);
}

int vc_app_open(VcApp *a, const char *path) {
    if (!a) return -1;
    int idx = vc_session_open(a->s, path);
    if (idx >= 0) ensure_cursor_visible(vc_session_active(a->s));
    return idx;
}

int vc_app_find_active(const VcApp *a) { return a ? a->find_active : 0; }

VcSession *vc_app_session(VcApp *a) { return a ? a->s : 0; }

/* ---------- 输入 ---------- */

void vc_app_text(VcApp *a, uint32_t cp) {
    if (!a || cp == 0) return;
    if (a->find_active) {
        char buf[4];
        uint32_t n = utf8_encode(cp, buf);
        if (a->find_len + n < sizeof(a->find_q)) {
            for (uint32_t i = 0; i < n; i++) a->find_q[a->find_len + i] = buf[i];
            a->find_len += n;
            a->find_q[a->find_len] = '\0';
        }
        return;
    }

    VcDocument *d = vc_session_active(a->s);
    if (!d) return;
    char buf[4];
    uint32_t n = utf8_encode(cp, buf);
    if (vc_doc_insert(a->s, d, buf, n)) ensure_cursor_visible(d);
}

static void find_next(VcApp *a) {
    if (!a || a->find_len == 0) return;
    VcDocument *d = vc_session_active(a->s);
    if (!d) return;

    VcFindOptions opt;
    opt.case_sensitive = 0;
    opt.whole_word = 0;

    VcRange hit;
    uint32_t from = d->cursor;
    if (d->anchor != d->cursor) from = d->anchor < d->cursor ? d->cursor : d->anchor;
    if (!vc_find_next(d->text, a->find_q, a->find_len, opt, from, &hit)) {
        if (!vc_find_next(d->text, a->find_q, a->find_len, opt, 0, &hit)) return;
    }

    d->anchor = hit.start;
    d->cursor = hit.start + hit.len;
    ensure_cursor_visible(d);
}

void vc_app_key(VcApp *a, uint32_t key, uint32_t mods) {
    if (!a) return;

    if (a->find_active) {
        if (key == VC_KEY_ESC) {
            a->find_active = 0;
            return;
        }
        if (key == 13) {
            find_next(a);
            return;
        }
        if (key == 8) {
            if (a->find_len) {
                uint32_t p = utf8_prev(a->find_q, a->find_len);
                a->find_len = p;
                a->find_q[p] = '\0';
            }
            return;
        }
        if (key >= 32 && key < 0x110000) vc_app_text(a, key);
        return;
    }

    if (mods & VC_MOD_CTRL) {
        vc_commands_dispatch(a->cmds, a->s, mods, key);
        return;
    }

    VcDocument *d = vc_session_active(a->s);
    if (!d) return;

    int extend = (mods & VC_MOD_SHIFT) ? 1 : 0;

    switch (key) {
        case 13: vc_app_text(a, '\n'); break;
        case 9:  vc_app_text(a, '\t'); break;
        case 8:  vc_doc_erase_selection(a->s, d); ensure_cursor_visible(d); break;
        case VC_KEY_LEFT:  vc_doc_move(d, 0, -1, extend); break;
        case VC_KEY_RIGHT: vc_doc_move(d, 0, 1, extend); break;
        case VC_KEY_UP:    vc_doc_move(d, 1, -1, extend); break;
        case VC_KEY_DOWN:  vc_doc_move(d, 1, 1, extend); break;
        case VC_KEY_PGUP:  vc_doc_move(d, 2, -1, extend); break;
        case VC_KEY_PGDN:  vc_doc_move(d, 2, 1, extend); break;
        case VC_KEY_HOME: {
            uint32_t line = vc_line_of(d->text, d->cursor);
            d->cursor = vc_line_start(d->text, line);
            if (!extend) d->anchor = d->cursor;
            break;
        }
        case VC_KEY_END: {
            uint32_t line = vc_line_of(d->text, d->cursor);
            uint32_t next = vc_line_start(d->text, line + 1);
            uint32_t len = vc_length(d->text);
            d->cursor = next ? next - 1 : len;
            if (!extend) d->anchor = d->cursor;
            break;
        }
        case VC_KEY_DELETE: {
            if (d->anchor != d->cursor) {
                vc_doc_erase_selection(a->s, d);
            } else {
                d->anchor = d->cursor;
                vc_doc_move(d, 0, 1, 1);
                vc_doc_erase_selection(a->s, d);
            }
            ensure_cursor_visible(d);
            break;
        }
        default:
            if (key >= 32 && key < 0x110000) vc_app_text(a, key);
            break;
    }
    ensure_cursor_visible(d);
}

void vc_app_mouse(VcApp *a, int x, int y, int button) {
    if (!a || button != 1) return;
    VcDocument *d = vc_session_active(a->s);
    if (!d) return;

    uint32_t ed_y = 0, ed_h = 0, cols = 1, rows = 1;
    app_layout(a, &ed_y, &ed_h, &cols, &rows);

    if (y >= 0 && (uint32_t)y < a->tab_h) {
        int count = vc_session_count(a->s);
        if (count <= 0 || x < 0) return;
        uint32_t tab_w = a->width / (uint32_t)count;
        if (tab_w > 160u) tab_w = 160u;
        if (tab_w < 60u) tab_w = 60u;
        int idx = (int)((uint32_t)x / tab_w);
        if (idx >= 0 && idx < count) vc_session_activate(a->s, idx);
        return;
    }

    if ((uint32_t)y < ed_y || (uint32_t)y >= ed_y + ed_h) return;
    int px = x - (int)a->theme.padding_x;
    int py = y - (int)ed_y - (int)a->theme.padding_y;
    if (px < 0 || py < 0) return;

    uint32_t pos = vc_view_hit_test(d->view, px, py, a->cw, a->ch);
    d->cursor = pos;
    d->anchor = pos;
    ensure_cursor_visible(d);
}

/* ---------- 渲染 ---------- */

VcRenderStats vc_app_render(VcApp *a, const VcSurface *surface) {
    VcRenderStats zero = { 0, 0 };
    if (!a || !surface || !surface->fill_rect || !surface->draw_glyph) return zero;

    uint32_t ed_y = 0, ed_h = 0, cols = 1, rows = 1;
    app_layout(a, &ed_y, &ed_h, &cols, &rows);
    if (cols != a->last_cols || rows != a->last_rows) {
        vc_session_set_size(a->s, cols, rows);
        a->last_cols = cols;
        a->last_rows = rows;
    }

    surface->fill_rect(surface->user, 0, 0, (int)a->width, (int)a->height,
                       a->theme.bg);

    /* 标签栏 */
    surface->fill_rect(surface->user, 0, 0, (int)a->width, (int)a->tab_h,
                       0x171A21);
    int count = vc_session_count(a->s);
    uint32_t tab_w = count > 0 ? a->width / (uint32_t)count : a->width;
    if (tab_w > 160u) tab_w = 160u;
    if (tab_w < 60u) tab_w = 60u;
    for (int i = 0; i < count; i++) {
        VcDocument *d = vc_session_at(a->s, i);
        int tx = (int)(i * tab_w);
        if (i == vc_session_active_index(a->s)) {
            surface->fill_rect(surface->user, tx, 0, (int)tab_w, (int)a->tab_h,
                               a->theme.bg);
        }
        const char *name = d->path[0] ? base_name(d->path) : "Untitled";
        draw_text(a, surface, tx + 8, 3, name,
                  i == vc_session_active_index(a->s) ? a->theme.fg : a->theme.dim);
        if (d->dirty) draw_text(a, surface, tx + (int)tab_w - 16, 3, "*",
                                a->theme.cursor);
    }

    /* 查找栏 */
    if (a->find_active) {
        uint32_t fy = a->tab_h;
        surface->fill_rect(surface->user, 0, (int)fy, (int)a->width,
                           (int)a->ch + 6, 0x1E2A3A);
        draw_text(a, surface, 8, (int)fy + 3, "Find:", a->theme.dim);
        draw_text(a, surface, 8 + (int)(5 * a->cw), (int)fy + 3,
                  a->find_q, a->theme.fg);
    }

    /* 编辑区 */
    a->octx.base = *surface;
    a->octx.ox = 0;
    a->octx.oy = (int)ed_y;
    a->edit_surface.width = a->width;
    a->edit_surface.height = ed_h;
    a->edit_surface.user = &a->octx;
    a->edit_surface.fill_rect = off_fill;
    a->edit_surface.draw_glyph = off_glyph;

    if (!a->edit) a->edit = vc_render_create(&a->edit_surface, &a->theme);

    VcRenderStats st = zero;
    VcDocument *d = vc_session_active(a->s);
    if (d && a->edit) {
        uint32_t sel_start = d->anchor < d->cursor ? d->anchor : d->cursor;
        uint32_t sel_end = d->anchor < d->cursor ? d->cursor : d->anchor;
        a->edit_surface.width = a->width;
        a->edit_surface.height = ed_h;
        vc_render_set_surface(a->edit, &a->edit_surface);
        st = vc_render_frame(a->edit, d->view, d->x_scroll, d->cursor,
                             sel_start, sel_end, 1);
    }

    /* 状态栏 */
    uint32_t sy = a->height > a->status_h ? a->height - a->status_h : 0;
    surface->fill_rect(surface->user, 0, (int)sy, (int)a->width,
                       (int)a->status_h, 0x171A21);
    if (d) {
        uint32_t line = vc_line_of(d->text, d->cursor) + 1;
        uint32_t col = vc_col_of(d->text, d->cursor) + 1;
        char buf[64];
        int n = 0;
        const char *pfx = "Ln ";
        while (*pfx) buf[n++] = *pfx++;
        char num[12];
        int k = 0;
        uint32_t v = line;
        if (v == 0) num[k++] = '0';
        while (v && k < 12) { num[k++] = (char)('0' + v % 10u); v /= 10u; }
        while (k > 0) buf[n++] = num[--k];
        buf[n++] = ',';
        buf[n++] = ' ';
        buf[n++] = 'C';
        buf[n++] = 'o';
        buf[n++] = 'l';
        buf[n++] = ' ';
        v = col; k = 0;
        if (v == 0) num[k++] = '0';
        while (v && k < 12) { num[k++] = (char)('0' + v % 10u); v /= 10u; }
        while (k > 0) buf[n++] = num[--k];
        buf[n] = '\0';
        draw_text(a, surface, 8, (int)sy + 2, buf, a->theme.dim);
        if (d->path[0]) draw_text(a, surface, (int)a->width / 2,
                                  (int)sy + 2, d->path, a->theme.dim);
    }

    return st;
}
