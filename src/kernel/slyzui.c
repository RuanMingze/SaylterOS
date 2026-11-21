/* ============================================================
 * slyzui.c - SLYZ v3 声明式界面引擎实现
 * ------------------------------------------------------------
 * 解析 screen 标记与 CSS，执行自动布局、绘制、滚动、状态与动画。
 *
 * 布局原则：
 *   - 默认撑开：column 的子项横向拉满，row 的子项纵向拉满；
 *   - 文本默认自动换行并按行数长高；
 *   - 根视图装不下时整页滚动；
 *   - 只有显式 clip / overflow:hidden 才裁剪子项。
 *
 * 样式采用属性级层叠：!important 不支持，但 id/class/type 与
 * 伪类的优先级、后定义覆盖先定义、内联属性最高都完整实现。
 * ============================================================ */

#include <stdint.h>

#include "slyzui.h"
#include "ui.h"
#include "vbe.h"
#include "unicode.h"
#include "mem.h"
#include "net.h"

/* ============================================================
 * 样式属性
 * ============================================================ */

enum {
    P_BG = 0, P_FG, P_BORDER, P_SHADOW,
    P_RADIUS, P_BW,
    P_PADT, P_PADR, P_PADB, P_PADL,
    P_MART, P_MARR, P_MARB, P_MARL,
    P_GAP, P_FONT, P_WIDTH, P_HEIGHT, P_MINW, P_MINH,
    P_GROW, P_ALIGN, P_WRAP, P_CLIP, P_SCROLL,
    P_TRANS, P_COLS, P_VISIBLE, P_VALUE,
    P_COUNT
};

struct UiStyle {
    uint8_t set[P_COUNT];
    int32_t val[P_COUNT];
};

struct UiComputed {
    uint32_t bg, fg, border;
    int has_bg, has_fg, has_border, has_shadow;
    int shadow;
    int radius, bw;
    int pt, pr, pb, pl;
    int mt, mr, mb, ml;
    int gap, font;
    int width, height, minw, minh;
    int grow, align, wrap, clip, scroll;
    int trans, cols, visible, value;
};

/* ============================================================
 * 选择器与规则
 * ============================================================ */

struct UiSelPart {
    char tag[12];
    char cls[24];
    char id[24];
    int  pseudo;   /* 0 none, 1 hover, 2 pressed, 3 focus, 4 disabled */
};

struct UiRule {
    struct UiSelPart parts[4];
    int  nparts;
    int  comb[4];  /* comb[i] 描述 part[i] 与 part[i+1]: 0 后代, 1 子 */
    int  spec;
    int  order;
    struct UiStyle st;
};

/* ============================================================
 * 动画与节点
 * ============================================================ */

struct UiAnim {
    int      active;
    uint32_t start;
    int      dur;
    uint32_t from;
    uint32_t to;
    uint32_t cur;
};

struct UiNode {
    int used;
    int type;

    int parent;
    int first;
    int last;
    int next;

    char tag[12];
    char id[SLYZUI_ID_MAX];
    char cls[SLYZUI_CLASS_MAX];
    char text[SLYZUI_TEXT_MAX];

    struct UiStyle    inl;
    struct UiComputed c;

    int x, y, w, h;
    int cx, cy, cw, ch;
    int mw, mh;
    int scroll_y;
    int content_h;

    int hover, pressed, focus, disabled;

    struct UiAnim abg, afg, aborder;
    int cur_radius;
};

struct SlyzUi {
    int vx, vy, vw, vh;

    struct UiNode nodes[SLYZUI_MAX_NODES];
    int node_count;
    int root;

    struct UiRule rules[SLYZUI_MAX_RULES];
    int rule_count;
    int rule_order;

    char err[SLYZUI_ERR_MAX];

    int mouse_x, mouse_y;
    int mouse_down;
    int prev_down;
    int hover_id;
    int press_id;
    int focus_id;

    struct SlyzUiEvent queue[8];
    int qhead, qtail;

    int closed;
    int style_dirty;
    int need_redraw;
};

/* ============================================================
 * 基础工具
 * ============================================================ */

static int s_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void s_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int s_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int s_ncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static void s_trim(char *s) {
    int n = s_len(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) {
        int k = 0;
        while (s[i]) s[k++] = s[i++];
        s[k] = '\0';
    }
}

static int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int class_has(const char *list, const char *token) {
    int tl = s_len(token);
    if (tl == 0) return 0;
    int i = 0;
    while (list[i]) {
        while (list[i] == ' ') i++;
        int start = i;
        while (list[i] && list[i] != ' ') i++;
        if (i - start == tl && s_ncmp(list + start, token, tl)) return 1;
    }
    return 0;
}

static void style_set(struct UiStyle *st, int prop, int32_t v) {
    if (prop < 0 || prop >= P_COUNT) return;
    st->set[prop] = 1;
    st->val[prop] = v;
}

/* ============================================================
 * 颜色与数值解析
 * ============================================================ */

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t parse_color(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '#') {
        int n = s_len(s + 1);
        if (n == 3) {
            int r = hex_digit(s[1]), g = hex_digit(s[2]), b = hex_digit(s[3]);
            if (r < 0 || g < 0 || b < 0) return 0;
            return ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) |
                   (uint32_t)(b * 17);
        }
        if (n >= 6) {
            uint32_t v = 0;
            for (int i = 0; i < 6; i++) {
                int d = hex_digit(s[1 + i]);
                if (d < 0) return 0;
                v = (v << 4) | (uint32_t)d;
            }
            return v;
        }
        return 0;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint32_t v = 0;
        for (int i = 2; s[i]; i++) {
            int d = hex_digit(s[i]);
            if (d < 0) break;
            v = (v << 4) | (uint32_t)d;
        }
        return v & 0xFFFFFFu;
    }
    if (s_eq(s, "white")) return 0xFFFFFFu;
    if (s_eq(s, "black")) return 0x000000u;
    if (s_eq(s, "red")) return 0xF87171u;
    if (s_eq(s, "green")) return 0x34D399u;
    if (s_eq(s, "blue")) return 0x4F8EF7u;
    if (s_eq(s, "gray") || s_eq(s, "grey")) return 0x8EA0B4u;
    if (s_eq(s, "transparent")) return 0;
    return 0;
}

static int parse_int(const char *s) {
    int neg = 0;
    int v = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

static int parse_bool(const char *s) {
    if (!s) return 0;
    if (s_eq(s, "1") || s_eq(s, "true") || s_eq(s, "on") || s_eq(s, "yes")) return 1;
    return 0;
}

static int parse_align(const char *s) {
    if (!s) return 0;
    if (s_eq(s, "start") || s_eq(s, "left") || s_eq(s, "top")) return 1;
    if (s_eq(s, "center") || s_eq(s, "middle")) return 2;
    if (s_eq(s, "end") || s_eq(s, "right") || s_eq(s, "bottom")) return 3;
    return 0;
}

/* 解析 1..4 个数值（padding/margin 简写） */
static void parse_box(const char *s, struct UiStyle *st,
                      int top, int right, int bottom, int left) {
    int v[4];
    int n = 0;
    const char *p = s;
    while (*p && n < 4) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        if (*p < '0' || *p > '9') break;
        int x = 0;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); p++; }
        v[n++] = neg ? -x : x;
        while (*p == 'p' || *p == 'x' || *p == 'P' || *p == 'X') p++;
    }
    if (n == 0) return;
    if (n == 1) { v[1] = v[2] = v[3] = v[0]; }
    else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (n == 3) { v[3] = v[1]; }
    style_set(st, top, v[0]);
    style_set(st, right, v[1]);
    style_set(st, bottom, v[2]);
    style_set(st, left, v[3]);
}

/* ============================================================
 * 标记解析
 * ============================================================ */

static int markup_type(const char *tag) {
    if (s_eq(tag, "column") || s_eq(tag, "col") || s_eq(tag, "stack")) return 1;
    if (s_eq(tag, "panel")) return 11;
    if (s_eq(tag, "row")) return 2;
    if (s_eq(tag, "grid")) return 3;
    if (s_eq(tag, "text") || s_eq(tag, "label")) return 4;
    if (s_eq(tag, "button")) return 5;
    if (s_eq(tag, "spacer")) return 6;
    if (s_eq(tag, "scroll")) return 7;
    if (s_eq(tag, "input")) return 8;
    if (s_eq(tag, "checkbox")) return 9;
    if (s_eq(tag, "progress")) return 10;
    return 0;
}

enum {
    UT_COLUMN = 1, UT_ROW, UT_GRID, UT_TEXT, UT_BUTTON,
    UT_SPACER, UT_SCROLL, UT_INPUT, UT_CHECKBOX, UT_PROGRESS,
    UT_PANEL = 11
};

static int node_alloc(struct SlyzUi *ui) {
    if (ui->node_count >= SLYZUI_MAX_NODES) {
        s_copy(ui->err, "too many elements (limit 192)", SLYZUI_ERR_MAX);
        return -1;
    }
    int id = ui->node_count++;
    struct UiNode *n = &ui->nodes[id];
    __builtin_memset(n, 0, sizeof(*n));
    n->used = 1;
    n->parent = -1;
    n->first = n->last = n->next = -1;
    n->cur_radius = 0;
    return id;
}

static void node_append(struct SlyzUi *ui, int parent, int child) {
    struct UiNode *p = &ui->nodes[parent];
    ui->nodes[child].parent = parent;
    ui->nodes[child].next = -1;
    if (p->last >= 0) ui->nodes[p->last].next = child;
    else p->first = child;
    p->last = child;
}

static void attr_apply(struct UiStyle *st, const char *key, const char *val) {
    if (s_eq(key, "text") || s_eq(key, "id") || s_eq(key, "class") ||
        s_eq(key, "value") || s_eq(key, "name")) {
        return;
    }
    if (s_eq(key, "width")) style_set(st, P_WIDTH, parse_int(val));
    else if (s_eq(key, "height")) style_set(st, P_HEIGHT, parse_int(val));
    else if (s_eq(key, "min-width")) style_set(st, P_MINW, parse_int(val));
    else if (s_eq(key, "min-height")) style_set(st, P_MINH, parse_int(val));
    else if (s_eq(key, "gap")) style_set(st, P_GAP, parse_int(val));
    else if (s_eq(key, "padding")) parse_box(val, st, P_PADT, P_PADR, P_PADB, P_PADL);
    else if (s_eq(key, "margin")) parse_box(val, st, P_MART, P_MARR, P_MARB, P_MARL);
    else if (s_eq(key, "grow")) style_set(st, P_GROW, parse_bool(val) || parse_int(val));
    else if (s_eq(key, "align")) style_set(st, P_ALIGN, parse_align(val));
    else if (s_eq(key, "wrap")) style_set(st, P_WRAP, parse_bool(val));
    else if (s_eq(key, "clip")) style_set(st, P_CLIP, parse_bool(val));
    else if (s_eq(key, "scroll")) style_set(st, P_SCROLL, parse_bool(val));
    else if (s_eq(key, "font-size") || s_eq(key, "font")) style_set(st, P_FONT, parse_int(val));
    else if (s_eq(key, "radius") || s_eq(key, "border-radius")) style_set(st, P_RADIUS, parse_int(val));
    else if (s_eq(key, "bg") || s_eq(key, "background")) style_set(st, P_BG, (int32_t)parse_color(val));
    else if (s_eq(key, "color")) style_set(st, P_FG, (int32_t)parse_color(val));
    else if (s_eq(key, "border-color") || s_eq(key, "border")) style_set(st, P_BORDER, (int32_t)parse_color(val));
    else if (s_eq(key, "border-width")) style_set(st, P_BW, parse_int(val));
    else if (s_eq(key, "shadow")) style_set(st, P_SHADOW, parse_int(val));
    else if (s_eq(key, "transition")) style_set(st, P_TRANS, parse_int(val));
    else if (s_eq(key, "cols")) style_set(st, P_COLS, parse_int(val));
    else if (s_eq(key, "visible")) style_set(st, P_VISIBLE, parse_bool(val));
    else if (s_eq(key, "value")) style_set(st, P_VALUE, parse_int(val));
}

static void skip_ws_comments(const char *s, int len, int *pos) {
    for (;;) {
        while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' ||
                              s[*pos] == '\r' || s[*pos] == '\n')) {
            (*pos)++;
        }
        if (*pos + 1 < len && s[*pos] == '/' && s[*pos + 1] == '/') {
            while (*pos < len && s[*pos] != '\n') (*pos)++;
            continue;
        }
        if (*pos + 1 < len && s[*pos] == '/' && s[*pos + 1] == '*') {
            *pos += 2;
            while (*pos + 1 < len && !(s[*pos] == '*' && s[*pos + 1] == '/')) (*pos)++;
            if (*pos + 1 < len) *pos += 2;
            continue;
        }
        break;
    }
}

static int read_quoted(const char *s, int len, int *pos, char *out, int cap) {
    char quote = s[*pos];
    int n = 0;
    (*pos)++;
    while (*pos < len && s[*pos] != quote) {
        char c = s[*pos];
        if (c == '\\' && *pos + 1 < len) {
            (*pos)++;
            char e = s[*pos];
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else c = e;
        }
        if (n < cap - 1) out[n++] = c;
        (*pos)++;
    }
    if (*pos < len) (*pos)++;
    out[n] = '\0';
    return n;
}

static int read_word(const char *s, int len, int *pos, char *out, int cap) {
    int n = 0;
    while (*pos < len && s[*pos] != ' ' && s[*pos] != '\t' &&
           s[*pos] != '\r' && s[*pos] != '\n' && s[*pos] != '{' &&
           s[*pos] != '}' && s[*pos] != '=') {
        if (n < cap - 1) out[n++] = s[*pos];
        (*pos)++;
    }
    out[n] = '\0';
    return n;
}

static int parse_element(struct SlyzUi *ui, int parent, const char *s, int len, int *pos);

static int parse_children(struct SlyzUi *ui, int parent, const char *s, int len, int *pos) {
    for (;;) {
        skip_ws_comments(s, len, pos);
        if (*pos >= len) return 0;
        if (s[*pos] == '}') {
            (*pos)++;
            return 0;
        }
        if (parse_element(ui, parent, s, len, pos) != 0) return -1;
    }
}

static int parse_element(struct SlyzUi *ui, int parent, const char *s, int len, int *pos) {
    char tag[12];
    skip_ws_comments(s, len, pos);
    if (*pos >= len) return 0;

    if (!read_word(s, len, pos, tag, (int)sizeof(tag))) {
        (*pos)++;
        return 0;
    }

    int type = markup_type(tag);
    if (type == 0) {
        s_copy(ui->err, "unknown element", SLYZUI_ERR_MAX);
        return -1;
    }

    int id = node_alloc(ui);
    if (id < 0) return -1;
    struct UiNode *n = &ui->nodes[id];
    n->type = type;
    s_copy(n->tag, tag, (int)sizeof(n->tag));
    if (parent >= 0) node_append(ui, parent, id);

    int has_brace = 0;
    for (;;) {
        skip_ws_comments(s, len, pos);
        if (*pos >= len) break;
        if (s[*pos] == '{') {
            (*pos)++;
            has_brace = 1;
            break;
        }
        if (s[*pos] == '}') {
            (*pos)++;
            return 0;
        }
        if (s[*pos] == '\n') {
            (*pos)++;
            break;
        }

        char key[24];
        char val[SLYZUI_TEXT_MAX];

        if (s[*pos] == '"' || s[*pos] == '\'') {
            read_quoted(s, len, pos, n->text, (int)sizeof(n->text));
            continue;
        }

        if (!read_word(s, len, pos, key, (int)sizeof(key))) {
            (*pos)++;
            continue;
        }

        int save = *pos;
        skip_ws_comments(s, len, pos);
        if (*pos < len && s[*pos] == '=') {
            (*pos)++;
            skip_ws_comments(s, len, pos);
            if (*pos < len && (s[*pos] == '"' || s[*pos] == '\'')) {
                read_quoted(s, len, pos, val, (int)sizeof(val));
            } else {
                read_word(s, len, pos, val, (int)sizeof(val));
            }
        } else {
            *pos = save;
            val[0] = '\0';
        }

        if (s_eq(key, "id")) s_copy(n->id, val, (int)sizeof(n->id));
        else if (s_eq(key, "class")) s_copy(n->cls, val, (int)sizeof(n->cls));
        else if (s_eq(key, "text")) s_copy(n->text, val, (int)sizeof(n->text));
        else if (s_eq(key, "value")) {
            s_copy(n->text, val, (int)sizeof(n->text));
            style_set(&n->inl, P_VALUE, parse_int(val));
        } else {
            attr_apply(&n->inl, key, val);
        }
    }

    if (has_brace) {
        if (parse_children(ui, id, s, len, pos) != 0) return -1;
    }

    return 0;
}

static void recompute_all(struct SlyzUi *ui);

int slyzui_set_markup(struct SlyzUi *ui, const char *text, int len) {
    if (!ui) return -1;
    ui->node_count = 0;
    ui->err[0] = '\0';

    int root = node_alloc(ui);
    if (root < 0) return -1;
    ui->root = root;
    ui->nodes[root].type = UT_COLUMN;
    s_copy(ui->nodes[root].tag, "root", 12);
    s_copy(ui->nodes[root].id, "__root", SLYZUI_ID_MAX);
    ui->nodes[root].c.scroll = 1;

    if (text && len > 0) {
        int pos = 0;
        while (pos < len) {
            skip_ws_comments(text, len, &pos);
            if (pos >= len) break;
            if (parse_element(ui, root, text, len, &pos) != 0) {
                ui->style_dirty = 1;
                return -1;
            }
        }
    }

    ui->style_dirty = 1;
    ui->need_redraw = 1;
    return 0;
}

/* ============================================================
 * CSS 解析
 * ============================================================ */

static void rule_parse_selector(struct UiRule *r, const char *sel) {
    r->nparts = 0;
    r->spec = 0;
    int i = 0;
    int pending_child = 0;

    while (sel[i] && r->nparts < 4) {
        while (sel[i] == ' ' || sel[i] == '\t') i++;
        if (sel[i] == '>') { pending_child = 1; i++; continue; }
        if (!sel[i]) break;

        struct UiSelPart *p = &r->parts[r->nparts];
        __builtin_memset(p, 0, sizeof(*p));

        int j = 0;
        while (sel[i] && sel[i] != ' ' && sel[i] != '\t' && sel[i] != '>' &&
               sel[i] != '.' && sel[i] != '#' && sel[i] != ':') {
            if (j < (int)sizeof(p->tag) - 1) p->tag[j++] = (char)to_lower(sel[i]);
            i++;
        }

        if (sel[i] == '.') {
            i++;
            j = 0;
            while (sel[i] && sel[i] != ' ' && sel[i] != '\t' && sel[i] != '>' &&
                   sel[i] != '.' && sel[i] != '#' && sel[i] != ':') {
                if (j < (int)sizeof(p->cls) - 1) p->cls[j++] = sel[i];
                i++;
            }
        }
        if (sel[i] == '#') {
            i++;
            j = 0;
            while (sel[i] && sel[i] != ' ' && sel[i] != '\t' && sel[i] != '>' &&
                   sel[i] != '.' && sel[i] != '#' && sel[i] != ':') {
                if (j < (int)sizeof(p->id) - 1) p->id[j++] = sel[i];
                i++;
            }
        }
        if (sel[i] == ':') {
            i++;
            char ps[16];
            j = 0;
            while (sel[i] && sel[i] != ' ' && sel[i] != '\t' && sel[i] != '>' &&
                   sel[i] != '.' && sel[i] != '#' && sel[i] != ':') {
                if (j < 15) ps[j++] = (char)to_lower(sel[i]);
                i++;
            }
            ps[j] = '\0';
            if (s_eq(ps, "hover")) p->pseudo = 1;
            else if (s_eq(ps, "pressed") || s_eq(ps, "active")) p->pseudo = 2;
            else if (s_eq(ps, "focus")) p->pseudo = 3;
            else if (s_eq(ps, "disabled")) p->pseudo = 4;
        }

        if (r->nparts > 0) r->comb[r->nparts - 1] = pending_child;
        pending_child = 0;

        if (p->id[0]) r->spec += 100;
        else if (p->cls[0]) r->spec += 10;
        else if (p->tag[0]) r->spec += 1;
        if (p->pseudo) r->spec += 10;

        r->nparts++;
        while (sel[i] == ' ' || sel[i] == '\t') i++;
    }
}

static void css_prop(struct UiStyle *st, const char *key, const char *val) {
    if (s_eq(key, "background") || s_eq(key, "background-color") || s_eq(key, "bg")) {
        style_set(st, P_BG, (int32_t)parse_color(val));
    } else if (s_eq(key, "color")) {
        style_set(st, P_FG, (int32_t)parse_color(val));
    } else if (s_eq(key, "border-color")) {
        style_set(st, P_BORDER, (int32_t)parse_color(val));
    } else if (s_eq(key, "border-radius") || s_eq(key, "radius")) {
        style_set(st, P_RADIUS, parse_int(val));
    } else if (s_eq(key, "border-width")) {
        style_set(st, P_BW, parse_int(val));
    } else if (s_eq(key, "border")) {
        const char *p = val;
        int w = 0;
        while (*p >= '0' && *p <= '9') { w = w * 10 + (*p - '0'); p++; }
        style_set(st, P_BW, w);
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p) style_set(st, P_BORDER, (int32_t)parse_color(p));
    } else if (s_eq(key, "padding")) {
        parse_box(val, st, P_PADT, P_PADR, P_PADB, P_PADL);
    } else if (s_eq(key, "margin")) {
        parse_box(val, st, P_MART, P_MARR, P_MARB, P_MARL);
    } else if (s_eq(key, "gap")) {
        style_set(st, P_GAP, parse_int(val));
    } else if (s_eq(key, "font-size") || s_eq(key, "font")) {
        style_set(st, P_FONT, parse_int(val));
    } else if (s_eq(key, "width")) {
        style_set(st, P_WIDTH, parse_int(val));
    } else if (s_eq(key, "height")) {
        style_set(st, P_HEIGHT, parse_int(val));
    } else if (s_eq(key, "min-width")) {
        style_set(st, P_MINW, parse_int(val));
    } else if (s_eq(key, "min-height")) {
        style_set(st, P_MINH, parse_int(val));
    } else if (s_eq(key, "flex-grow") || s_eq(key, "grow")) {
        style_set(st, P_GROW, parse_int(val) ? parse_int(val) : parse_bool(val));
    } else if (s_eq(key, "align") || s_eq(key, "align-items") ||
               s_eq(key, "text-align")) {
        style_set(st, P_ALIGN, parse_align(val));
    } else if (s_eq(key, "white-space")) {
        style_set(st, P_WRAP, s_eq(val, "nowrap") ? 0 : 1);
    } else if (s_eq(key, "overflow")) {
        if (s_eq(val, "hidden")) {
            style_set(st, P_CLIP, 1);
            style_set(st, P_SCROLL, 0);
        } else if (s_eq(val, "scroll") || s_eq(val, "auto")) {
            style_set(st, P_SCROLL, 1);
            style_set(st, P_CLIP, 1);
        } else {
            style_set(st, P_CLIP, 0);
            style_set(st, P_SCROLL, 0);
        }
    } else if (s_eq(key, "box-shadow") || s_eq(key, "shadow")) {
        style_set(st, P_SHADOW, parse_int(val));
    } else if (s_eq(key, "transition-duration") || s_eq(key, "transition")) {
        style_set(st, P_TRANS, parse_int(val));
    } else if (s_eq(key, "visibility")) {
        style_set(st, P_VISIBLE, s_eq(val, "hidden") ? 0 : 1);
    } else if (s_eq(key, "display")) {
        if (s_eq(val, "none")) style_set(st, P_VISIBLE, 0);
    }
}

static void css_declarations(struct UiStyle *st, const char *s, int len) {
    char key[32];
    char val[128];
    int i = 0;
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                           s[i] == '\n' || s[i] == ';')) i++;
        if (i >= len || s[i] == '}') break;

        int kn = 0;
        while (i < len && s[i] != ':' && s[i] != ';' && s[i] != '}' &&
               kn < (int)sizeof(key) - 1) {
            key[kn++] = (char)to_lower(s[i++]);
        }
        key[kn] = '\0';
        s_trim(key);
        if (i >= len || s[i] != ':') {
            while (i < len && s[i] != ';' && s[i] != '}') i++;
            continue;
        }
        i++;
        int vn = 0;
        while (i < len && s[i] != ';' && s[i] != '}' && vn < (int)sizeof(val) - 1) {
            val[vn++] = s[i++];
        }
        val[vn] = '\0';
        s_trim(val);
        if (key[0]) css_prop(st, key, val);
    }
}

int slyzui_set_style(struct SlyzUi *ui, const char *css, int len) {
    if (!ui) return -1;
    ui->rule_count = 0;
    ui->rule_order = 1;
    ui->err[0] = '\0';

    int i = 0;
    while (i < len) {
        while (i < len && (css[i] == ' ' || css[i] == '\t' ||
                           css[i] == '\r' || css[i] == '\n')) i++;
        if (i >= len) break;
        if (css[i] == '/' && i + 1 < len && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2;
            continue;
        }

        int sel_start = i;
        while (i < len && css[i] != '{') i++;
        if (i >= len) break;
        int sel_len = i - sel_start;
        i++;

        int body_start = i;
        while (i < len && css[i] != '}') i++;
        int body_len = i - body_start;
        if (i < len) i++;

        char selector[256];
        int sc = sel_len < (int)sizeof(selector) - 1 ? sel_len : (int)sizeof(selector) - 1;
        for (int k = 0; k < sc; k++) selector[k] = css[sel_start + k];
        selector[sc] = '\0';

        char body[512];
        int bc = body_len < (int)sizeof(body) - 1 ? body_len : (int)sizeof(body) - 1;
        for (int k = 0; k < bc; k++) body[k] = css[body_start + k];
        body[bc] = '\0';

        int p = 0;
        while (p < sc) {
            while (p < sc && (selector[p] == ' ' || selector[p] == '\t')) p++;
            int start = p;
            while (p < sc && selector[p] != ',') p++;
            int n = p - start;
            if (p < sc) p++;

            if (n <= 0) continue;
            if (ui->rule_count >= SLYZUI_MAX_RULES) break;

            struct UiRule *r = &ui->rules[ui->rule_count++];
            __builtin_memset(r, 0, sizeof(*r));
            char one[256];
            int on = n < (int)sizeof(one) - 1 ? n : (int)sizeof(one) - 1;
            for (int k = 0; k < on; k++) one[k] = selector[start + k];
            one[on] = '\0';

            rule_parse_selector(r, one);
            if (r->nparts == 0) {
                ui->rule_count--;
                continue;
            }
            css_declarations(&r->st, body, bc);
            r->order = ui->rule_order++;
        }
    }

    ui->style_dirty = 1;
    ui->need_redraw = 1;
    return 0;
}

const char *slyzui_error(struct SlyzUi *ui) {
    return ui && ui->err[0] ? ui->err : "";
}

/* ============================================================
 * 样式层叠
 * ============================================================ */

static int pseudo_match(struct UiNode *n, int pseudo) {
    if (pseudo == 0) return 1;
    if (pseudo == 1) return n->hover;
    if (pseudo == 2) return n->pressed;
    if (pseudo == 3) return n->focus;
    if (pseudo == 4) return n->disabled;
    return 0;
}

static int part_match(struct UiNode *n, struct UiSelPart *p) {
    if (p->tag[0]) {
        /* 容器别名：column/col/panel/stack 使用真实 tag 匹配 */
        if (!s_eq(n->tag, p->tag)) {
            int is_col = n->type == UT_COLUMN;
            if (!(is_col && (s_eq(p->tag, "column") || s_eq(p->tag, "col") ||
                             s_eq(p->tag, "panel") || s_eq(p->tag, "stack")))) {
                return 0;
            }
        }
    }
    if (p->cls[0] && !class_has(n->cls, p->cls)) return 0;
    if (p->id[0] && !s_eq(n->id, p->id)) return 0;
    if (!pseudo_match(n, p->pseudo)) return 0;
    return 1;
}

static int rule_match(struct SlyzUi *ui, struct UiRule *r, int node) {
    int idx = node;
    int last = r->nparts - 1;
    if (!part_match(&ui->nodes[idx], &r->parts[last])) return 0;

    for (int i = last - 1; i >= 0; i--) {
        if (r->comb[i] == 1) {
            idx = ui->nodes[idx].parent;
            if (idx < 0 || !part_match(&ui->nodes[idx], &r->parts[i])) return 0;
        } else {
            idx = ui->nodes[idx].parent;
            while (idx >= 0 && !part_match(&ui->nodes[idx], &r->parts[i])) {
                idx = ui->nodes[idx].parent;
            }
            if (idx < 0) return 0;
        }
    }
    return 1;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int t);

static void anim_target(struct UiAnim *a, uint32_t target, int dur, uint32_t now) {    if (a->cur == target && !a->active) return;
    if (dur <= 0) {
        a->cur = target;
        a->active = 0;
        return;
    }
    a->from = a->cur;
    a->to = target;
    a->start = now;
    a->dur = dur;
    a->active = 1;
}

static void anim_step(struct UiAnim *a, uint32_t now) {
    if (!a->active) return;
    uint32_t el = now - a->start;
    if (el >= (uint32_t)a->dur) {
        a->cur = a->to;
        a->active = 0;
        return;
    }
    int t = (int)(el * 1000u / (uint32_t)a->dur);
    a->cur = lerp_color(a->from, a->to, t);
}

static void c_val_set(struct UiComputed *c, int p, int32_t v);

static void recompute_all(struct SlyzUi *ui) {
    uint32_t now = net_now_ms();

    for (int i = 0; i < ui->node_count; i++) {
        struct UiNode *n = &ui->nodes[i];
        struct UiComputed c;

        __builtin_memset(&c, 0, sizeof(c));
        c.fg = 0xE9EFF7;
        c.border = 0x2B3949;
        c.font = 32;
        c.wrap = 1;
        c.cols = 1;
        c.visible = 1;
        c.align = 0;

        /* 控件类型默认样式（可被 CSS 覆盖） */
        if (n->type == UT_BUTTON) {
            c.bg = 0x1B2532; c.has_bg = 1;
            c.border = 0x2B3949; c.has_border = 1;
            c.bw = 1; c.radius = 10;
            c.pt = 9; c.pb = 9; c.pl = 14; c.pr = 14;
            c.minh = 40;
        } else if (n->type == UT_PANEL) {
            c.bg = 0x141C27; c.has_bg = 1;
            c.radius = 12;
            c.pt = 14; c.pb = 14; c.pl = 14; c.pr = 14;
        } else if (n->type == UT_INPUT) {
            c.bg = 0x0C141E; c.has_bg = 1;
            c.border = 0x2B3949; c.has_border = 1;
            c.bw = 1; c.radius = 8;
            c.pt = 8; c.pb = 8; c.pl = 12; c.pr = 12;
            c.minh = 40;
        } else if (n->type == UT_PROGRESS) {
            c.bg = 0x1B2532; c.has_bg = 1;
            c.radius = 6;
            c.height = c.height ? c.height : 12;
            c.minh = 12;
        }

        /* 规则层叠：属性级比较 specificity 与定义顺序 */
        int winspec[P_COUNT];
        int winorder[P_COUNT];
        for (int k = 0; k < P_COUNT; k++) {
            winspec[k] = -1;
            winorder[k] = -1;
        }

        for (int r = 0; r < ui->rule_count; r++) {
            struct UiRule *rule = &ui->rules[r];
            if (!rule_match(ui, rule, i)) continue;
            for (int p = 0; p < P_COUNT; p++) {
                if (!rule->st.set[p]) continue;
                if (rule->spec > winspec[p] ||
                    (rule->spec == winspec[p] && rule->order >= winorder[p])) {
                    winspec[p] = rule->spec;
                    winorder[p] = rule->order;
                    c_val_set(&c, p, rule->st.val[p]);
                }
            }
        }

        /* 内联属性最高 */
        for (int p = 0; p < P_COUNT; p++) {
            if (n->inl.set[p]) c_val_set(&c, p, n->inl.val[p]);
        }

        /* 继承 */
        if (n->parent >= 0) {
            struct UiComputed *pc = &ui->nodes[n->parent].c;
            if (!c.has_fg) { c.fg = pc->fg; c.has_fg = pc->has_fg; }
            if (!n->inl.set[P_FONT]) {
                int inherited = 0;
                for (int r = 0; r < ui->rule_count; r++) {
                    if (ui->rules[r].st.set[P_FONT] && rule_match(ui, &ui->rules[r], i)) {
                        inherited = 1;
                        break;
                    }
                }
                if (!inherited) c.font = pc->font;
            }
        }

        if (c.cols < 1) c.cols = 1;
        if (c.radius < 0) c.radius = 0;
        if (c.bw < 0) c.bw = 0;
        if (c.font < 16) c.font = 32;
        if (i == ui->root) c.scroll = 1;

        n->c = c;

        /* 动画目标：颜色与圆角 */
        int dur = c.trans;
        anim_target(&n->abg, c.has_bg ? c.bg : 0x000000u, dur, now);
        anim_target(&n->afg, c.fg, dur, now);
        anim_target(&n->aborder, c.has_border ? c.border : 0x000000u, dur, now);
        if (dur > 0) {
            if (n->cur_radius != c.radius) {
                n->cur_radius = c.radius;
            }
        } else {
            n->cur_radius = c.radius;
        }
    }

    ui->style_dirty = 0;
}

/* UiComputed 的属性索引赋值辅助：把 UiStyle 的 val 映射到字段。
 * 放在 apply_style_props 之外是因为层叠需要在所有规则收集完后
 * 逐属性写入，不能按规则顺序直接覆盖。 */
static void c_val_set(struct UiComputed *c, int p, int32_t v) {
    switch (p) {
        case P_BG: c->bg = (uint32_t)v; c->has_bg = 1; break;
        case P_FG: c->fg = (uint32_t)v; c->has_fg = 1; break;
        case P_BORDER: c->border = (uint32_t)v; c->has_border = 1; break;
        case P_SHADOW: c->shadow = v; c->has_shadow = v > 0; break;
        case P_RADIUS: c->radius = v; break;
        case P_BW: c->bw = v; break;
        case P_PADT: c->pt = v; break;
        case P_PADR: c->pr = v; break;
        case P_PADB: c->pb = v; break;
        case P_PADL: c->pl = v; break;
        case P_MART: c->mt = v; break;
        case P_MARR: c->mr = v; break;
        case P_MARB: c->mb = v; break;
        case P_MARL: c->ml = v; break;
        case P_GAP: c->gap = v; break;
        case P_FONT: c->font = v; break;
        case P_WIDTH: c->width = v; break;
        case P_HEIGHT: c->height = v; break;
        case P_MINW: c->minw = v; break;
        case P_MINH: c->minh = v; break;
        case P_GROW: c->grow = v; break;
        case P_ALIGN: c->align = v; break;
        case P_WRAP: c->wrap = v; break;
        case P_CLIP: c->clip = v; break;
        case P_SCROLL: c->scroll = v; break;
        case P_TRANS: c->trans = v; break;
        case P_COLS: c->cols = v; break;
        case P_VISIBLE: c->visible = v; break;
        case P_VALUE: c->value = v; break;
        default: break;
    }
}

/* ============================================================
 * 文本度量与换行
 * ============================================================ */

static int font_scale(int font) {
    return font >= 48 ? 2 : 1;
}

static int line_height(int font) {
    int sc = font_scale(font);
    if (font >= 20 && font < 48) return 42;
    return sc * 34;
}

static int text_width_px(const char *s, int font) {
    return ui_text_width(s) * font_scale(font);
}

/* 取下一行；返回下一行起点。out_bytes 为不含行尾空格的字节数。 */
static const char *wrap_next(const char *p, int max_w, int font,
                             int *out_bytes, int *out_w) {
    int sc = font_scale(font);
    int limit = max_w > 0 ? max_w : 16 * sc;
    int bytes = 0;
    int w = 0;
    int last_space = -1;
    int last_space_w = 0;

    while (p[bytes]) {
        uint8_t c = (uint8_t)p[bytes];
        if (c == '\n') {
            *out_bytes = bytes;
            *out_w = w;
            return p + bytes + 1;
        }
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)p + bytes, s_len(p + bytes), &cp);
        if (used <= 0) used = 1;
        int cw = utf32_width(cp) * 16 * sc;
        if (cw <= 0) cw = 16 * sc;

        if (w + cw > limit && bytes > 0) {
            if (last_space > 0) {
                *out_bytes = last_space;
                *out_w = last_space_w;
                return p + last_space + 1;
            }
            *out_bytes = bytes;
            *out_w = w;
            return p + bytes;
        }
        if (cp == ' ') {
            last_space = bytes;
            last_space_w = w;
        }
        w += cw;
        bytes += used;
    }

    *out_bytes = bytes;
    *out_w = w;
    return p + bytes;
}

static void measure_text(const char *s, int max_w, int font, int *out_w, int *out_h) {
    int lines = 0;
    int max_line = 0;
    const char *p = s;

    if (!s || !*s) {
        *out_w = 0;
        *out_h = line_height(font);
        return;
    }

    while (*p) {
        int n = 0, w = 0;
        p = wrap_next(p, max_w, font, &n, &w);
        if (w > max_line) max_line = w;
        lines++;
        if (lines > 200) break;
    }
    if (lines == 0) lines = 1;
    *out_w = max_line;
    *out_h = lines * line_height(font);
}

/* ============================================================
 * 布局
 * ============================================================ */

static struct UiNode *N(struct SlyzUi *ui, int i) {
    return &ui->nodes[i];
}

static void measure_node(struct SlyzUi *ui, int id, int avail_w, int avail_h) {
    struct UiNode *n = N(ui, id);
    struct UiComputed *c = &n->c;
    if (!c->visible) {
        n->mw = n->mh = 0;
        return;
    }

    int pad_w = c->pl + c->pr;
    int pad_h = c->pt + c->pb;
    int inner_w = avail_w - pad_w;

    int nat_w = 0;
    int nat_h = 0;

    if (n->type == UT_TEXT || n->type == UT_BUTTON || n->type == UT_INPUT ||
        n->type == UT_CHECKBOX) {
        int tw = 0, th = 0;
        int text_avail = inner_w;
        if (n->type == UT_BUTTON || n->type == UT_CHECKBOX) {
            if (text_avail <= 0) text_avail = 200;
        }
        if (n->type == UT_INPUT && text_avail > 0) text_avail = inner_w;
        measure_text(n->text, text_avail > 0 ? text_avail : 200, c->font, &tw, &th);
        nat_w = tw + pad_w;
        nat_h = th + pad_h;
        if (n->type == UT_CHECKBOX) {
            nat_w += 30;
            if (nat_h < 34) nat_h = 34;
        }
        if (n->type == UT_BUTTON && nat_h < c->minh) nat_h = c->minh;
        if (nat_h < line_height(c->font) + pad_h) nat_h = line_height(c->font) + pad_h;
    } else if (n->type == UT_SPACER) {
        nat_w = c->width > 0 ? c->width : 0;
        nat_h = c->height > 0 ? c->height : 0;
    } else if (n->type == UT_PROGRESS) {
        nat_w = c->width > 0 ? c->width : inner_w;
        nat_h = c->height > 0 ? c->height : 14;
    } else {
        /* 容器：先量所有子项 */
        int child_avail_w = inner_w;
        if (n->type == UT_SCROLL) child_avail_w -= 12;
        if (child_avail_w < 0) child_avail_w = 0;

        int total_main = 0;
        int max_cross = 0;
        int visible_children = 0;

        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            struct UiNode *cn = &ui->nodes[ch];
            if (!cn->c.visible) continue;
            measure_node(ui, ch, child_avail_w, avail_h);

            int cmw = cn->mw + cn->c.ml + cn->c.mr;
            int cmh = cn->mh + cn->c.mt + cn->c.mb;

            if (n->type == UT_ROW) {
                total_main += cmw;
                if (cmh > max_cross) max_cross = cmh;
            } else if (n->type == UT_GRID) {
                if (cmw > max_cross) max_cross = cmw;
                total_main = 0;
            } else {
                total_main += cmh;
                if (cmw > max_cross) max_cross = cmw;
            }
            visible_children++;
        }

        int gaps = visible_children > 1 ? (visible_children - 1) * c->gap : 0;

        if (n->type == UT_GRID) {
            int cols = c->cols > 0 ? c->cols : 1;
            int cell = inner_w > 0 ? (inner_w - (cols - 1) * c->gap) / cols : 120;
            if (cell < 40) cell = 40;
            int rows = (visible_children + cols - 1) / cols;
            int row_h = 0;
            for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
                if (ui->nodes[ch].mh > row_h) row_h = ui->nodes[ch].mh;
            }
            nat_w = cell * cols + (cols - 1) * c->gap + pad_w;
            nat_h = rows * row_h + (rows > 0 ? (rows - 1) * c->gap : 0) + pad_h;
        } else if (n->type == UT_ROW) {
            nat_w = total_main + gaps + pad_w;
            nat_h = max_cross + pad_h;
        } else {
            nat_w = max_cross + pad_w;
            nat_h = total_main + gaps + pad_h;
        }

        if (n->type == UT_SCROLL) {
            nat_w += 12;
            int content = nat_h;
            n->content_h = content > avail_h ? content : avail_h;
            nat_h = c->height > 0 ? c->height : avail_h;
        }
    }

    if (nat_w < c->minw) nat_w = c->minw;
    if (nat_h < c->minh) nat_h = c->minh;
    if (c->width > 0) nat_w = c->width;
    if (c->height > 0) nat_h = c->height;

    n->mw = nat_w;
    n->mh = nat_h;
}

static void arrange_node(struct SlyzUi *ui, int id, int x, int y, int avail_w, int avail_h) {
    struct UiNode *n = N(ui, id);
    struct UiComputed *c = &n->c;
    if (!c->visible) return;

    int w = n->mw;
    int h = n->mh;

    if (c->width <= 0 && (c->align == 0 || n->type == UT_COLUMN ||
                          n->type == UT_GRID || n->type == UT_SCROLL)) {
        if (avail_w > 0 && (n->parent < 0 || c->grow || c->align == 0)) {
            if (n->parent < 0) w = avail_w;
        }
    }
    if (h > avail_h && c->height <= 0) {
        /* 高度不强行压扁：超出部分由父级/根视图滚动处理 */
    }

    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;
    n->cx = x + c->pl;
    n->cy = y + c->pt;
    n->cw = w - c->pl - c->pr;
    n->ch = h - c->pt - c->pb;
    if (n->cw < 0) n->cw = 0;
    if (n->ch < 0) n->ch = 0;

    if (n->type == UT_COLUMN || n->type == UT_PANEL || n->type == UT_SCROLL) {
        int inner_w = n->cw - (n->type == UT_SCROLL ? 12 : 0);
        int inner_h = n->ch;
        if (inner_w < 0) inner_w = 0;

        int total = 0;
        int grow_count = 0;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            if (!ui->nodes[ch].c.visible) continue;
            total += ui->nodes[ch].mh + ui->nodes[ch].c.mt + ui->nodes[ch].c.mb;
            if (ui->nodes[ch].c.grow) grow_count++;
        }
        int gaps = 0;
        int vc = 0;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            if (ui->nodes[ch].c.visible) vc++;
        }
        if (vc > 1) gaps = (vc - 1) * c->gap;
        int spare = inner_h - total - gaps;
        int grow_extra = (grow_count > 0 && spare > 0) ? spare / grow_count : 0;

        int cy = n->cy - n->scroll_y;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            struct UiNode *cn = &ui->nodes[ch];
            if (!cn->c.visible) continue;

            int cw = cn->c.width > 0 ? cn->c.width : inner_w;
            if (cw > inner_w && cn->c.width <= 0) cw = inner_w;
            if (cw < 0) cw = 0;

            int chh = cn->mh;
            if (cn->c.grow) chh += grow_extra;

            int cx = n->cx;
            if (cn->c.width <= 0 && cn->c.align == 2) cx += (inner_w - cw) / 2;
            else if (cn->c.width <= 0 && cn->c.align == 3) cx += inner_w - cw;

            arrange_node(ui, ch, cx + cn->c.ml,
                         cy + cn->c.mt, cw, chh);
            cy += cn->c.mt + chh + cn->c.mb + c->gap;
        }

        if (n->type == UT_SCROLL) {
            int view_h = n->ch;
            if (n->content_h > view_h) {
                if (n->scroll_y > n->content_h - view_h) n->scroll_y = n->content_h - view_h;
                if (n->scroll_y < 0) n->scroll_y = 0;
            } else {
                n->scroll_y = 0;
            }
        }
    } else if (n->type == UT_ROW) {
        int inner_w = n->cw;
        int inner_h = n->ch;

        int total = 0;
        int grow_count = 0;
        int vc = 0;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            if (!ui->nodes[ch].c.visible) continue;
            total += ui->nodes[ch].mw + ui->nodes[ch].c.ml + ui->nodes[ch].c.mr;
            if (ui->nodes[ch].c.grow) grow_count++;
            vc++;
        }
        int gaps = vc > 1 ? (vc - 1) * c->gap : 0;
        int spare = inner_w - total - gaps;
        int grow_extra = (grow_count > 0 && spare > 0) ? spare / grow_count : 0;

        int cx = n->cx;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            struct UiNode *cn = &ui->nodes[ch];
            if (!cn->c.visible) continue;

            int cw = cn->mw;
            if (cn->c.grow) cw += grow_extra;
            int chh = cn->c.height > 0 ? cn->c.height : inner_h;
            if (chh > inner_h && cn->c.height <= 0) chh = inner_h;
            if (chh < 0) chh = 0;

            int cy = n->cy;
            if (cn->c.height <= 0 && cn->c.align == 2) cy += (inner_h - chh) / 2;
            else if (cn->c.height <= 0 && cn->c.align == 3) cy += inner_h - chh;

            arrange_node(ui, ch, cx + cn->c.ml, cy + cn->c.mt, cw, chh);
            cx += cn->c.ml + cw + cn->c.mr + c->gap;
        }
    } else if (n->type == UT_GRID) {
        int cols = c->cols > 0 ? c->cols : 1;
        int inner_w = n->cw;
        int cell_w = (inner_w - (cols - 1) * c->gap) / cols;
        if (cell_w < 20) cell_w = 20;

        int row_h = 0;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            if (ui->nodes[ch].mh > row_h) row_h = ui->nodes[ch].mh;
        }

        int idx = 0;
        for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
            struct UiNode *cn = &ui->nodes[ch];
            if (!cn->c.visible) continue;
            int col = idx % cols;
            int row = idx / cols;
            int cx = n->cx + col * (cell_w + c->gap);
            int cy = n->cy + row * (row_h + c->gap);
            int cw = cn->c.width > 0 ? cn->c.width : cell_w;
            int chh = cn->c.height > 0 ? cn->c.height : row_h;
            arrange_node(ui, ch, cx + cn->c.ml, cy + cn->c.mt, cw, chh);
            idx++;
        }
    }
}

/* ============================================================
 * 绘制
 * ============================================================ */

static uint32_t lerp_color(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 1000) return b;
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = ar + (br - ar) * t / 1000;
    int g = ag + (bg - ag) * t / 1000;
    int bl = ab + (bb - ab) * t / 1000;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static void draw_text_lines(struct SlyzUi *ui, struct UiNode *n, int x, int y,
                            int w, int center) {
    struct UiComputed *c = &n->c;
    int lh = line_height(c->font);
    int sc = font_scale(c->font);
    int line = 0;
    const char *p = n->text;

    (void)ui;
    while (*p && line < 200) {
        int nb = 0, lw = 0;
        p = wrap_next(p, w, c->font, &nb, &lw);
        if (nb > 0) {
            char tmp[SLYZUI_TEXT_MAX];
            int k = 0;
            while (k < nb && k < (int)sizeof(tmp) - 1) { tmp[k] = p[-nb + k]; k++; }
            tmp[k] = '\0';
            int tx = x;
            if (center) tx = x + (w - lw) / 2;
            int ascii = 1;
            for (int q = 0; q < k; q++) {
                if ((unsigned char)tmp[q] >= 0x80) { ascii = 0; break; }
            }
            if (sc == 2 && ascii) ui_text_big(tx, y + line * lh, tmp, c->fg);
            else ui_text_transparent(tx, y + line * lh + (lh - 32) / 2, tmp, c->fg);
        }
        line++;
    }
}

static void draw_node(struct SlyzUi *ui, int id);

static void draw_children(struct SlyzUi *ui, int id) {
    struct UiNode *n = N(ui, id);
    for (int ch = n->first; ch >= 0; ch = ui->nodes[ch].next) {
        draw_node(ui, ch);
    }
}

static void draw_node(struct SlyzUi *ui, int id) {
    struct UiNode *n = N(ui, id);
    struct UiComputed *c = &n->c;
    if (!c->visible) return;
    if (n->w <= 0 || n->h <= 0) return;

    uint32_t bg = n->abg.cur;
    uint32_t bd = n->aborder.cur;
    int radius = n->cur_radius;

    if (c->has_shadow) {
        ui_shadow((ui_rect){ n->x, n->y, n->w, n->h }, radius, c->shadow);
    }

    if (c->has_bg || c->has_border) {
        if (c->has_border && c->bw > 0) {
            ui_round_panel((ui_rect){ n->x, n->y, n->w, n->h }, radius, bd,
                           c->has_bg ? bg : 0x0B1017);
        } else if (c->has_bg) {
            ui_round_fill((ui_rect){ n->x, n->y, n->w, n->h }, radius, bg);
        }
    }

    if (n->type == UT_TEXT) {
        draw_text_lines(ui, n, n->cx, n->cy, n->cw, c->align == 2 || c->align == 3);
    } else if (n->type == UT_BUTTON) {
        int lh = line_height(c->font);
        int text_h = n->ch;
        int ty = n->cy + (text_h - lh) / 2;
        draw_text_lines(ui, n, n->cx, ty, n->cw, 1);
        if (n->focus) {
            ui_round_panel((ui_rect){ n->x - 2, n->y - 2, n->w + 4, n->h + 4 },
                           radius + 2, 0x4F8EF7, 0x000000);
        }
    } else if (n->type == UT_INPUT) {
        int lh = line_height(c->font);
        int ty = n->cy + (n->ch - lh) / 2;
        draw_text_lines(ui, n, n->cx + 2, ty, n->cw - 4, 0);
        if (n->focus) {
            int cx = n->cx + 2 + text_width_px(n->text, c->font);
            if (cx > n->cx + n->cw - 4) cx = n->cx + n->cw - 4;
            if (((net_now_ms() / 500u) & 1u) == 0) {
                vbe_fill_rect((uint16_t)cx, (uint16_t)(ty + 2), 2,
                              (uint16_t)(lh - 4), 0x4F8EF7);
            }
        }
    } else if (n->type == UT_CHECKBOX) {
        int box = 20;
        int by = n->cy + (n->ch - box) / 2;
        ui_round_panel((ui_rect){ n->cx, by, box, box }, 5,
                       n->hover ? 0x4F8EF7 : 0x2B3949, 0x0C141E);
        if (c->value) {
            ui_line(n->cx + 4, by + 10, n->cx + 9, by + 15, 0x34D399);
            ui_line(n->cx + 9, by + 15, n->cx + 16, by + 5, 0x34D399);
        }
        int lh = line_height(c->font);
        draw_text_lines(ui, n, n->cx + box + 10, n->cy + (n->ch - lh) / 2,
                        n->cw - box - 10, 0);
    } else if (n->type == UT_PROGRESS) {
        int v = c->value;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        int fill = n->w * v / 100;
        if (fill > 0) ui_round_fill((ui_rect){ n->x, n->y, fill, n->h }, radius, 0x4F8EF7);
    }

    if (c->clip || c->scroll || n->type == UT_SCROLL) {
        int cx = n->cx;
        int cy = n->cy;
        int cw = n->cw;
        int chh = n->ch;
        if (cw < 0) cw = 0;
        if (chh < 0) chh = 0;
        vbe_push_clip(cx, cy, cw, chh);
        draw_children(ui, id);
        vbe_pop_clip();

        if (n->type == UT_SCROLL && n->content_h > chh && chh > 0) {
            int track_x = n->x + n->w - 8;
            int track_y = n->cy;
            int track_h = chh;
            ui_round_fill((ui_rect){ track_x, track_y, 5, track_h }, 2, 0x16202C);
            int thumb_h = track_h * track_h / n->content_h;
            if (thumb_h < 24) thumb_h = 24;
            int max_scroll = n->content_h - track_h;
            int thumb_y = track_y + (max_scroll > 0 ? (track_h - thumb_h) * n->scroll_y / max_scroll : 0);
            ui_round_fill((ui_rect){ track_x, thumb_y, 5, thumb_h }, 2, 0x4F8EF7);
        }
    } else {
        draw_children(ui, id);
    }

    (void)ui;
}

/* ============================================================
 * 命中测试与交互
 * ============================================================ */

static int point_in_node(struct SlyzUi *ui, struct UiNode *n, int x, int y) {
    if (!n->c.visible) return 0;
    if (x < n->x || y < n->y || x >= n->x + n->w || y >= n->y + n->h) return 0;

    /* 被滚动/裁剪祖先裁掉的部分不可命中 */
    int p = n->parent;
    while (p >= 0) {
        struct UiNode *pn = &ui->nodes[p];
        if (pn->c.clip || pn->c.scroll || pn->type == UT_SCROLL) {
            if (x < pn->cx || y < pn->cy ||
                x >= pn->cx + pn->cw || y >= pn->cy + pn->ch) {
                return 0;
            }
        }
        p = pn->parent;
    }
    return 1;
}

static int hit_node(struct SlyzUi *ui, int x, int y) {
    int best = -1;
    for (int i = 0; i < ui->node_count; i++) {
        struct UiNode *n = &ui->nodes[i];
        if (point_in_node(ui, n, x, y)) best = i;
    }
    return best;
}

static int interactive_ancestor(struct SlyzUi *ui, int id) {
    while (id >= 0) {
        struct UiNode *n = &ui->nodes[id];
        if (n->type == UT_BUTTON || n->type == UT_INPUT ||
            n->type == UT_CHECKBOX) {
            return id;
        }
        id = n->parent;
    }
    return -1;
}

static void queue_event(struct SlyzUi *ui, int type, int code, const char *id) {
    int next = (ui->qtail + 1) % 8;
    if (next == ui->qhead) {
        ui->qhead = (ui->qhead + 1) % 8;
    }
    struct SlyzUiEvent *e = &ui->queue[ui->qtail];
    e->type = type;
    e->code = code;
    s_copy(e->id, id ? id : "", (int)sizeof(e->id));
    ui->qtail = next;
}

void slyzui_mouse(struct SlyzUi *ui, int x, int y, int down) {
    if (!ui) return;
    ui->mouse_x = x;
    ui->mouse_y = y;

    int hit = hit_node(ui, x, y);
    int act = interactive_ancestor(ui, hit);

    int new_hover = act >= 0 ? act : -1;
    if (new_hover != ui->hover_id) {
        if (ui->hover_id >= 0) ui->nodes[ui->hover_id].hover = 0;
        ui->hover_id = new_hover;
        if (new_hover >= 0) ui->nodes[new_hover].hover = 1;
        ui->style_dirty = 1;
    }

    if (down && !ui->prev_down) {
        ui->press_id = act;
        if (act >= 0) ui->nodes[act].pressed = 1;
        ui->style_dirty = 1;
    } else if (!down && ui->prev_down) {
        int pressed = ui->press_id;
        if (pressed >= 0) ui->nodes[pressed].pressed = 0;
        if (pressed >= 0 && pressed == act) {
            struct UiNode *n = &ui->nodes[pressed];
            if (n->type == UT_CHECKBOX) {
                n->c.value = !n->c.value;
                queue_event(ui, SLYZUI_EV_CLICK, 0, n->id);
            } else {
                queue_event(ui, SLYZUI_EV_CLICK, 0, n->id);
            }
        }
        ui->press_id = -1;
        ui->style_dirty = 1;
    }

    ui->mouse_down = down;
    ui->prev_down = down;
}

void slyzui_wheel(struct SlyzUi *ui, int delta) {
    if (!ui || delta == 0) return;
    int hit = hit_node(ui, ui->mouse_x, ui->mouse_y);
    int target = -1;
    for (int i = hit; i >= 0; i = ui->nodes[i].parent) {
        if (ui->nodes[i].c.scroll || ui->nodes[i].type == UT_SCROLL || i == ui->root) {
            target = i;
            break;
        }
    }
    if (target < 0) return;

    struct UiNode *n = &ui->nodes[target];
    n->scroll_y -= delta * 36;
    int max_scroll = n->content_h - n->ch;
    if (target == ui->root) {
        if (n->content_h == 0) n->content_h = n->mh;
        max_scroll = n->content_h - ui->vh;
    }
    if (max_scroll < 0) max_scroll = 0;
    if (n->scroll_y < 0) n->scroll_y = 0;
    if (n->scroll_y > max_scroll) n->scroll_y = max_scroll;
    ui->need_redraw = 1;
}

static int next_focus(struct SlyzUi *ui, int dir) {
    int start = ui->focus_id;
    for (int step = 1; step <= ui->node_count; step++) {
        int id = start < 0 ? (dir > 0 ? step - 1 : ui->node_count - step)
                           : (start + step * dir + ui->node_count * 2) % ui->node_count;
        struct UiNode *n = &ui->nodes[id];
        if (n->c.visible && (n->type == UT_BUTTON || n->type == UT_INPUT ||
                             n->type == UT_CHECKBOX)) {
            return id;
        }
    }
    return -1;
}

void slyzui_key(struct SlyzUi *ui, int ch) {
    if (!ui) return;

    if (ch == 9) {   /* Tab */
        int nf = next_focus(ui, 1);
        if (ui->focus_id >= 0) ui->nodes[ui->focus_id].focus = 0;
        ui->focus_id = nf;
        if (nf >= 0) ui->nodes[nf].focus = 1;
        ui->style_dirty = 1;
        return;
    }

    if (ui->focus_id >= 0) {
        struct UiNode *n = &ui->nodes[ui->focus_id];
        if (n->type == UT_BUTTON && (ch == 13 || ch == 32)) {
            queue_event(ui, SLYZUI_EV_CLICK, 0, n->id);
            return;
        }
        if (n->type == UT_CHECKBOX && (ch == 13 || ch == 32)) {
            n->c.value = !n->c.value;
            queue_event(ui, SLYZUI_EV_CLICK, 0, n->id);
            return;
        }
        if (n->type == UT_INPUT) {
            int len = s_len(n->text);
            if ((ch == 8 || ch == 127) && len > 0) {
                n->text[len - 1] = '\0';
                queue_event(ui, SLYZUI_EV_CHANGE, 0, n->id);
                return;
            }
            if (ch >= 32 && ch < 127 && len < (int)sizeof(n->text) - 1) {
                n->text[len] = (char)ch;
                n->text[len + 1] = '\0';
                queue_event(ui, SLYZUI_EV_CHANGE, 0, n->id);
                return;
            }
        }
    }

    queue_event(ui, SLYZUI_EV_KEY, ch, "");
}

/* ============================================================
 * 对外生命周期
 * ============================================================ */

struct SlyzUi *slyzui_create(int x, int y, int w, int h) {
    struct SlyzUi *ui = (struct SlyzUi *)kmalloc((uint32_t)sizeof(struct SlyzUi));
    if (!ui) return 0;
    __builtin_memset(ui, 0, sizeof(*ui));
    ui->vx = x;
    ui->vy = y;
    ui->vw = w;
    ui->vh = h;
    ui->root = -1;
    ui->hover_id = -1;
    ui->press_id = -1;
    ui->focus_id = -1;
    ui->style_dirty = 1;
    return ui;
}

void slyzui_destroy(struct SlyzUi *ui) {
    if (ui) kfree(ui);
}

void slyzui_set_view(struct SlyzUi *ui, int x, int y, int w, int h) {
    if (!ui) return;
    ui->vx = x;
    ui->vy = y;
    ui->vw = w;
    ui->vh = h;
    ui->need_redraw = 1;
}

int slyzui_set_text(struct SlyzUi *ui, const char *id, const char *text, int len) {
    if (!ui || !id || !id[0]) return -1;
    for (int i = 0; i < ui->node_count; i++) {
        if (!s_eq(ui->nodes[i].id, id)) continue;
        int n = len < 0 ? s_len(text) : len;
        if (n > SLYZUI_TEXT_MAX - 1) n = SLYZUI_TEXT_MAX - 1;
        for (int k = 0; k < n; k++) ui->nodes[i].text[k] = text[k];
        ui->nodes[i].text[n] = '\0';
        ui->need_redraw = 1;
        return 0;
    }
    return -1;
}

int slyzui_get_text(struct SlyzUi *ui, const char *id, char *out, int cap) {
    if (!ui || !id || !out || cap <= 0) return -1;
    for (int i = 0; i < ui->node_count; i++) {
        if (!s_eq(ui->nodes[i].id, id)) continue;
        s_copy(out, ui->nodes[i].text, cap);
        return 0;
    }
    return -1;
}

int slyzui_closed(struct SlyzUi *ui) {
    return ui ? ui->closed : 1;
}

void slyzui_request_close(struct SlyzUi *ui) {
    if (ui) ui->closed = 1;
}

int slyzui_present(struct SlyzUi *ui, struct SlyzUiEvent *out) {
    if (!ui) return 0;

    uint32_t now = net_now_ms();

    if (ui->style_dirty) recompute_all(ui);

    for (int i = 0; i < ui->node_count; i++) {
        anim_step(&ui->nodes[i].abg, now);
        anim_step(&ui->nodes[i].afg, now);
        anim_step(&ui->nodes[i].aborder, now);
    }

    vbe_push_clip(ui->vx, ui->vy, ui->vw, ui->vh);
    vbe_fill_rect((uint16_t)ui->vx, (uint16_t)ui->vy,
                  (uint16_t)ui->vw, (uint16_t)ui->vh, 0x0B1017);

    if (ui->root >= 0) {
        struct UiNode *r = &ui->nodes[ui->root];

        /* 根节点铺满视图；内容超出时整页滚动 */
        r->mw = ui->vw;
        measure_node(ui, ui->root, ui->vw, ui->vh);
        if (r->mh > ui->vh) {
            r->content_h = r->mh;
        } else {
            r->content_h = ui->vh;
            r->scroll_y = 0;
        }
        r->mw = ui->vw;
        r->mh = ui->vh;
        arrange_node(ui, ui->root, ui->vx, ui->vy, ui->vw, ui->vh);

        draw_node(ui, ui->root);
    }

    vbe_pop_clip();

    if (ui->closed) {
        if (out) {
            out->type = SLYZUI_EV_CLOSE;
            out->code = 0;
            out->id[0] = '\0';
        }
        return 1;
    }

    if (ui->qhead != ui->qtail) {
        if (out) *out = ui->queue[ui->qhead];
        ui->qhead = (ui->qhead + 1) % 8;
        return 1;
    }
    return 0;
}
