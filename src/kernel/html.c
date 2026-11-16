/* ============================================================
 * html.c - 极简 HTML + CSS 渲染器（无 JavaScript）
 * ------------------------------------------------------------
 * 目标不是完整浏览器，而是在没有 libc、没有浮点、没有图片解码
 * 的内核里，把常见网页排版成"可读的文字页面"：
 *
 *   块级元素  : 上下 margin + 自动换行 + 段落间距
 *   行内元素  : 粗体（重绘 1px 偏移）、下划线、颜色、链接高亮
 *   CSS       : <style> 里的简单规则 + 内联 style
 *   选择器    : tag / .class / #id / tag.class / *（后代选择器取末段）
 *   布局单位  : 字符 16x32；标题用 32x64 放大字形
 *
 * 两遍执行：第一遍只测量总高度，第二遍按 scroll 偏移真正绘制。
 * 绘制完全依赖 vbe 的裁剪栈，调用方负责把内容区裁出来。
 * ============================================================ */

#include <stdint.h>
#include "html.h"
#include "ui.h"
#include "vbe.h"

/* ============================================================
 * 基础工具
 * ============================================================ */

#define HTML_CHAR_W   16
#define HTML_CHAR_H   32
#define HTML_BIG_W    32
#define HTML_BIG_H    64

#define HTML_LINE_MAX   256
#define HTML_STACK_MAX  32
#define HTML_LINKS_MAX  64
#define HTML_RULES_MAX  64

static int h_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int h_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int h_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void h_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int h_ieq_n(const char *a, int n, const char *b) {
    for (int i = 0; i < n; i++) {
        if (!b[i]) return 0;
        if (h_lower(a[i]) != h_lower(b[i])) return 0;
    }
    return b[n] == '\0';
}

static int h_ieq(const char *a, const char *b) {
    int n = h_strlen(b);
    return h_ieq_n(a, n, b);
}

static int h_icontains_n(const char *hay, int hay_n, const char *needle) {
    int nn = h_strlen(needle);
    if (nn == 0 || nn > hay_n) return 0;

    for (int i = 0; i + nn <= hay_n; i++) {
        int k = 0;
        while (k < nn && h_lower(hay[i + k]) == h_lower(needle[k])) k++;
        if (k == nn) return 1;
    }
    return 0;
}

static int h_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 命名颜色表：只保留排版本里最常用的 20 个 */
struct HtmlNamedColor {
    const char *name;
    uint32_t    rgb;
};

static const struct HtmlNamedColor g_named_colors[] = {
    { "black",   0x000000 }, { "white",   0xFFFFFF },
    { "red",     0xFF0000 }, { "green",   0x008000 },
    { "blue",    0x0000FF }, { "yellow",  0xFFFF00 },
    { "gray",    0x808080 }, { "grey",    0x808080 },
    { "silver",  0xC0C0C0 }, { "maroon",  0x800000 },
    { "navy",    0x000080 }, { "teal",    0x008080 },
    { "olive",   0x808000 }, { "lime",    0x00FF00 },
    { "aqua",    0x00FFFF }, { "cyan",    0x00FFFF },
    { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF },
    { "purple",  0x800080 }, { "orange",  0xFFA500 },
    { "pink",    0xFFC0CB }, { "brown",   0xA52A2A },
    { "inherit", 0x000000 }, { 0, 0 }
};

/* 返回 1 表示识别成功 */
static int html_parse_color(const char *v, uint32_t *out) {
    while (*v == ' ' || *v == '\t') v++;

    if (*v == '#') {
        v++;
        int len = 0;
        while (v[len] && v[len] != ';' && v[len] != ' ' &&
               v[len] != '\t' && v[len] != '\r' && v[len] != '\n') {
            len++;
        }

        if (len == 3) {
            int r = h_hexval(v[0]);
            int g = h_hexval(v[1]);
            int b = h_hexval(v[2]);
            if (r < 0 || g < 0 || b < 0) return 0;
            *out = ((uint32_t)(r * 17) << 16) |
                   ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
            return 1;
        }
        if (len >= 6) {
            int r = h_hexval(v[0]) * 16 + h_hexval(v[1]);
            int g = h_hexval(v[2]) * 16 + h_hexval(v[3]);
            int b = h_hexval(v[4]) * 16 + h_hexval(v[5]);
            if (r < 0 || g < 0 || b < 0) return 0;
            *out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            return 1;
        }
        return 0;
    }

    for (int i = 0; g_named_colors[i].name; i++) {
        if (h_ieq(v, g_named_colors[i].name)) {
            *out = g_named_colors[i].rgb;
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 样式
 * ============================================================ */

struct HtmlStyle {
    uint32_t color;
    uint32_t bg;
    int      has_color;
    int      has_bg;
    int      scale;          /* 1 或 2 */
    int      bold;
    int      underline;
    int      align;          /* 0 left, 1 center, 2 right */
    int      margin_top;
    int      margin_bottom;
    int      indent;
    int      line_h;
};

struct HtmlRule {
    char             selector[28];
    struct HtmlStyle style;
};

static struct HtmlRule g_rules[HTML_RULES_MAX];
static int g_rule_count = 0;

static void html_style_clear(struct HtmlStyle *st) {
    st->color = 0;
    st->bg = 0;
    st->has_color = 0;
    st->has_bg = 0;
    st->scale = 1;
    st->bold = 0;
    st->underline = 0;
    st->align = 0;
    st->margin_top = 0;
    st->margin_bottom = 0;
    st->indent = 0;
    st->line_h = HTML_CHAR_H;
}

static void html_style_inherit(const struct HtmlStyle *parent, struct HtmlStyle *st) {
    *st = *parent;
    st->margin_top = 0;
    st->margin_bottom = 0;
    st->indent = 0;
    if (st->line_h < HTML_CHAR_H) st->line_h = HTML_CHAR_H;
}

/* 解析一组 CSS 声明（"color:#fff; margin-top:4px"）并覆盖到 st */
static void html_apply_decls(const char *s, int n, struct HtmlStyle *st) {
    int i = 0;

    while (i < n) {
        while (i < n && (h_isspace(s[i]) || s[i] == ';')) i++;
        if (i >= n) break;

        int name_start = i;
        while (i < n && s[i] != ':' && s[i] != ';') i++;
        int name_len = i - name_start;
        if (i >= n || s[i] == ';') continue;
        i++;   /* 跳过 ':' */

        while (i < n && h_isspace(s[i])) i++;
        int val_start = i;
        while (i < n && s[i] != ';') i++;
        int val_len = i - val_start;

        while (val_len > 0 && h_isspace(s[val_start + val_len - 1])) val_len--;
        if (name_len <= 0 || val_len <= 0) continue;

        const char *name = s + name_start;
        const char *val = s + val_start;
        uint32_t color = 0;

        if (h_ieq_n(name, name_len, "color")) {
            if (html_parse_color(val, &color)) {
                st->color = color;
                st->has_color = 1;
            }
        } else if (h_ieq_n(name, name_len, "background") ||
                   h_ieq_n(name, name_len, "background-color")) {
            if (html_parse_color(val, &color)) {
                st->bg = color;
                st->has_bg = 1;
            }
        } else if (h_ieq_n(name, name_len, "font-size")) {
            int px = 0;
            int digits = 0;
            const char *p = val;
            while (*p >= '0' && *p <= '9' && digits < 4) {
                px = px * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) {
                st->scale = (px >= 24) ? 2 : 1;
            } else if (h_icontains_n(val, val_len, "large") ||
                       h_icontains_n(val, val_len, "x-large")) {
                st->scale = 2;
            }
        } else if (h_ieq_n(name, name_len, "font-weight")) {
            if (h_icontains_n(val, val_len, "bold") ||
                h_icontains_n(val, val_len, "700") ||
                h_icontains_n(val, val_len, "600")) {
                st->bold = 1;
            } else if (h_icontains_n(val, val_len, "normal")) {
                st->bold = 0;
            }
        } else if (h_ieq_n(name, name_len, "text-align")) {
            if (h_icontains_n(val, val_len, "center")) st->align = 1;
            else if (h_icontains_n(val, val_len, "right")) st->align = 2;
            else if (h_icontains_n(val, val_len, "left")) st->align = 0;
        } else if (h_ieq_n(name, name_len, "text-decoration")) {
            if (h_icontains_n(val, val_len, "underline")) st->underline = 1;
            else if (h_icontains_n(val, val_len, "none")) st->underline = 0;
        } else if (h_ieq_n(name, name_len, "margin-top")) {
            int px = 0;
            int digits = 0;
            const char *p = val;
            while (*p >= '0' && *p <= '9') {
                px = px * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) st->margin_top = px;
        } else if (h_ieq_n(name, name_len, "margin-bottom")) {
            int px = 0;
            int digits = 0;
            const char *p = val;
            while (*p >= '0' && *p <= '9') {
                px = px * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) st->margin_bottom = px;
        } else if (h_ieq_n(name, name_len, "margin")) {
            int px = 0;
            int digits = 0;
            const char *p = val;
            while (*p >= '0' && *p <= '9') {
                px = px * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) {
                st->margin_top = px;
                st->margin_bottom = px;
            }
        } else if (h_ieq_n(name, name_len, "padding") ||
                   h_ieq_n(name, name_len, "padding-left")) {
            int px = 0;
            int digits = 0;
            const char *p = val;
            while (*p >= '0' && *p <= '9') {
                px = px * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) st->indent = px;
        } else if (h_ieq_n(name, name_len, "line-height")) {
            int px = 0;
            int digits = 0;
            const char *p = val;
            while (*p >= '0' && *p <= '9') {
                px = px * 10 + (*p - '0');
                p++;
                digits++;
            }
            if (digits > 0) {
                st->line_h = (px > HTML_CHAR_H) ? px : HTML_CHAR_H;
            }
        }
    }
}

/* 元素默认样式（等价于浏览器 UA 样式表的子集） */
static void html_default_style(const char *name, struct HtmlStyle *st) {
    if (h_ieq(name, "h1")) {
        st->scale = 2;
        st->bold = 1;
        st->margin_top = 18;
        st->margin_bottom = 14;
        st->line_h = HTML_BIG_H;
    } else if (h_ieq(name, "h2")) {
        st->scale = 2;
        st->bold = 1;
        st->margin_top = 16;
        st->margin_bottom = 12;
        st->line_h = HTML_BIG_H;
    } else if (h_ieq(name, "h3")) {
        st->scale = 2;
        st->bold = 1;
        st->margin_top = 12;
        st->margin_bottom = 10;
        st->line_h = HTML_BIG_H;
    } else if (h_ieq(name, "h4") || h_ieq(name, "h5") || h_ieq(name, "h6")) {
        st->bold = 1;
        st->margin_top = 10;
        st->margin_bottom = 8;
    } else if (h_ieq(name, "p")) {
        st->margin_top = 10;
        st->margin_bottom = 10;
    } else if (h_ieq(name, "li")) {
        st->indent = 28;
        st->margin_top = 4;
        st->margin_bottom = 4;
    } else if (h_ieq(name, "blockquote")) {
        st->indent = 36;
        st->margin_top = 10;
        st->margin_bottom = 10;
    } else if (h_ieq(name, "pre")) {
        st->margin_top = 10;
        st->margin_bottom = 10;
        st->indent = 16;
    } else if (h_ieq(name, "hr")) {
        st->margin_top = 12;
        st->margin_bottom = 12;
    } else if (h_ieq(name, "b") || h_ieq(name, "strong")) {
        st->bold = 1;
    } else if (h_ieq(name, "u")) {
        st->underline = 1;
    } else if (h_ieq(name, "a")) {
        st->underline = 1;
        st->color = 0x1D4ED8;
        st->has_color = 1;
    } else if (h_ieq(name, "center")) {
        st->align = 1;
        st->margin_top = 10;
        st->margin_bottom = 10;
    } else if (h_ieq(name, "td")) {
        st->indent = 0;
    }
}

static int html_is_block(const char *name) {
    static const char *blocks[] = {
        "html", "body", "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "li", "blockquote", "pre", "hr", "table", "tr", "td",
        "form", "header", "footer", "nav", "section", "article", "main",
        "aside", "figure", "figcaption", "center", "address", "fieldset",
        "dl", "dt", "dd", "title", 0
    };

    for (int i = 0; blocks[i]; i++) {
        if (h_ieq(name, blocks[i])) return 1;
    }
    return 0;
}

static int html_is_void(const char *name) {
    static const char *voids[] = {
        "br", "hr", "img", "meta", "link", "input", "area", "base",
        "col", "embed", "param", "source", "track", "wbr", 0
    };

    for (int i = 0; voids[i]; i++) {
        if (h_ieq(name, voids[i])) return 1;
    }
    return 0;
}

/* 选择器匹配：支持 tag / .class / #id / tag.class / * */
static int html_selector_matches(const char *sel, const char *tag,
                                 const char *cls, const char *id) {
    if (sel[0] == '*') return 1;
    if (!sel[0]) return 0;

    const char *p = sel;
    char tag_part[24];
    int tn = 0;

    while (*p && *p != '.' && *p != '#' && tn < (int)sizeof(tag_part) - 1) {
        tag_part[tn++] = *p++;
    }
    tag_part[tn] = '\0';

    if (tn > 0 && !h_ieq(tag_part, tag)) return 0;

    while (*p == '.' || *p == '#') {
        int is_class = (*p == '.');
        p++;

        char want[32];
        int wn = 0;
        while (*p && *p != '.' && *p != '#' && wn < (int)sizeof(want) - 1) {
            want[wn++] = *p++;
        }
        want[wn] = '\0';

        if (is_class) {
            if (!cls[0]) return 0;
            int found = 0;
            int i = 0;
            while (cls[i] && !found) {
                while (cls[i] == ' ') i++;
                int start = i;
                while (cls[i] && cls[i] != ' ') i++;
                int len = i - start;
                if (len > 0 && h_ieq_n(cls + start, len, want)) found = 1;
            }
            if (!found) return 0;
        } else {
            if (!h_ieq(id, want)) return 0;
        }
    }

    return 1;
}

/* ============================================================
 * 布局状态
 * ============================================================ */

struct HtmlCell {
    char     ch;
    uint32_t color;
    uint8_t  scale;
    uint8_t  bold;
    uint8_t  underline;
    int16_t  link;
};

struct HtmlLink {
    int  used;
    int  x0;
    int  y0;
    int  x1;
    int  y1;
    char href[128];
};

static struct HtmlCell g_line[HTML_LINE_MAX];
static int g_line_n = 0;
static int g_line_w = 0;
static int g_line_h = HTML_CHAR_H;

static struct HtmlStyle g_stack[HTML_STACK_MAX];
static int g_depth = 1;

static struct HtmlLink g_links[HTML_LINKS_MAX];
static int g_link_count = 0;
static int g_cur_link = -1;

static int g_draw = 0;
static int g_x = 0;
static int g_w = 0;
static int g_cy = 0;
static int g_max_y = 0;
static int g_scroll = 0;
static int g_indent = 0;
static int g_pre = 0;
static int g_skip = 0;            /* >0：正在跳过 script/style/title 内容 */
static char g_skip_tag[16];
static int g_list_depth = 0;
static int g_pending_space = 0;

void html_reset(void) {
    g_link_count = 0;
    g_cur_link = -1;
    g_rule_count = 0;
    g_depth = 1;
    html_style_clear(&g_stack[0]);
    g_line_n = 0;
    g_line_w = 0;
    g_line_h = HTML_CHAR_H;
    g_indent = 0;
    g_pre = 0;
    g_skip = 0;
    g_list_depth = 0;
    g_pending_space = 0;
}

static struct HtmlStyle *html_top_style(void) {
    return &g_stack[g_depth - 1];
}

static void html_push_style(const struct HtmlStyle *st) {
    if (g_depth >= HTML_STACK_MAX) return;
    g_stack[g_depth] = *st;
    if (g_stack[g_depth].line_h < HTML_CHAR_H) {
        g_stack[g_depth].line_h = HTML_CHAR_H;
    }
    g_depth++;
}

static void html_pop_style(void) {
    if (g_depth > 1) g_depth--;
}

/* ============================================================
 * 行缓冲与绘制
 * ============================================================ */

static void html_draw_cell(int x, int y, const struct HtmlCell *cell) {
    char one[2];
    one[0] = cell->ch;
    one[1] = '\0';

    if (cell->scale > 1) {
        ui_text_big(x, y, one, cell->color);
    } else {
        ui_text_transparent(x, y, one, cell->color);
        if (cell->bold) {
            ui_text_transparent(x + 1, y, one, cell->color);
        }
        if (cell->underline) {
            vbe_fill_rect((uint16_t)x, (uint16_t)(y + HTML_CHAR_H - 3),
                          HTML_CHAR_W, 2, cell->color);
        }
    }
}

static void html_link_extend(int idx, int x, int y, int w, int h) {
    if (idx < 0 || idx >= HTML_LINKS_MAX) return;

    struct HtmlLink *l = &g_links[idx];
    if (!l->used) {
        l->used = 1;
        l->x0 = x;
        l->y0 = y;
        l->x1 = x + w;
        l->y1 = y + h;
        return;
    }
    if (x < l->x0) l->x0 = x;
    if (y < l->y0) l->y0 = y;
    if (x + w > l->x1) l->x1 = x + w;
    if (y + h > l->y1) l->y1 = y + h;
}

static void html_flush_line(void) {
    if (g_line_n > 0) {
        int line_h = g_line_h;
        if (line_h < HTML_CHAR_H) line_h = HTML_CHAR_H;

        int x0 = g_x + g_indent;

        int align = html_top_style()->align;
        if (align == 1) {
            x0 = g_x + (g_w - g_line_w) / 2;
        } else if (align == 2) {
            x0 = g_x + g_w - g_line_w;
        }
        if (x0 < g_x) x0 = g_x;

        int y = g_cy - g_scroll;
        int px = x0;

        for (int i = 0; i < g_line_n; i++) {
            int cw = HTML_CHAR_W * g_line[i].scale;
            if (g_draw && g_line[i].ch != ' ') {
                html_draw_cell(px, y, &g_line[i]);
            }
            if (g_line[i].link >= 0) {
                html_link_extend(g_line[i].link, px, y, cw, line_h);
            }
            px += cw;
        }

        g_cy += line_h;
        if (g_cy > g_max_y) g_max_y = g_cy;
    } else {
        g_cy += HTML_CHAR_H;
        if (g_cy > g_max_y) g_max_y = g_cy;
    }

    g_line_n = 0;
    g_line_w = 0;
    g_line_h = HTML_CHAR_H;
    g_pending_space = 0;
}

static void html_put_char(char c, const struct HtmlStyle *st) {
    g_indent = st->indent;

    int scale = (st->scale > 1) ? 2 : 1;
    int cw = HTML_CHAR_W * scale;
    int avail = g_w - g_indent;
    if (avail < cw) avail = cw;

    if (g_line_w + cw > avail && g_line_n > 0) {
        html_flush_line();
    }
    if (g_line_n >= HTML_LINE_MAX) {
        html_flush_line();
    }

    struct HtmlCell *cell = &g_line[g_line_n];
    cell->ch = c;
    cell->color = st->has_color ? st->color : 0x1A2431;
    cell->scale = (uint8_t)scale;
    cell->bold = (uint8_t)st->bold;
    cell->underline = (uint8_t)st->underline;
    cell->link = (int16_t)g_cur_link;

    g_line_n++;
    g_line_w += cw;
    if (HTML_CHAR_H * scale > g_line_h) g_line_h = HTML_CHAR_H * scale;
}

/* 按词输出：整词放不下就换行，避免单词被拦腰截断 */
static void html_put_word(const char *w, int n, const struct HtmlStyle *st) {
    g_indent = st->indent;

    int scale = (st->scale > 1) ? 2 : 1;
    int cw = HTML_CHAR_W * scale;
    int avail = g_w - g_indent;
    if (avail < cw) avail = cw;

    int word_w = n * cw;

    if (g_pending_space && g_line_n > 0) {
        if (g_line_w + cw + word_w > avail) {
            html_flush_line();
        } else {
            html_put_char(' ', st);
        }
    }
    g_pending_space = 0;

    if (word_w > avail && g_line_n > 0) {
        html_flush_line();
    }

    for (int i = 0; i < n; i++) {
        html_put_char(w[i], st);
    }
}

/* ============================================================
 * 标签处理
 * ============================================================ */

struct HtmlAttrs {
    char href[128];
    char cls[48];
    char id[24];
    char style[192];
    char alt[48];
    int  align_set;
};

static void html_attrs_clear(struct HtmlAttrs *a) {
    a->href[0] = '\0';
    a->cls[0] = '\0';
    a->id[0] = '\0';
    a->style[0] = '\0';
    a->alt[0] = '\0';
    a->align_set = 0;
}

static int html_attr_eq(const char *name, int n, const char *want) {
    return h_ieq_n(name, n, want);
}

/* 把 "k=v" 解析进 attrs；返回消耗到的位置 */
static int html_parse_attrs(const char *s, int n, struct HtmlAttrs *a) {
    int i = 0;

    while (i < n) {
        while (i < n && h_isspace(s[i])) i++;
        if (i >= n) break;

        int ns = i;
        while (i < n && s[i] != '=' && !h_isspace(s[i])) i++;
        int nl = i - ns;
        if (nl <= 0) { i++; continue; }

        int vs = -1;
        int vl = 0;

        if (i < n && s[i] == '=') {
            i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char quote = s[i];
                i++;
                vs = i;
                while (i < n && s[i] != quote) i++;
                vl = i - vs;
                if (i < n) i++;
            } else {
                vs = i;
                while (i < n && !h_isspace(s[i]) && s[i] != '>') i++;
                vl = i - vs;
            }
        }

        const char *name = s + ns;
        const char *val = (vs >= 0) ? s + vs : "";
        int vlen = (vs >= 0) ? vl : 0;

        if (html_attr_eq(name, nl, "href")) {
            int k = 0;
            while (k < vlen && k < (int)sizeof(a->href) - 1) {
                a->href[k] = val[k];
                k++;
            }
            a->href[k] = '\0';
        } else if (html_attr_eq(name, nl, "class")) {
            int k = 0;
            while (k < vlen && k < (int)sizeof(a->cls) - 1) {
                a->cls[k] = val[k];
                k++;
            }
            a->cls[k] = '\0';
        } else if (html_attr_eq(name, nl, "id")) {
            int k = 0;
            while (k < vlen && k < (int)sizeof(a->id) - 1) {
                a->id[k] = val[k];
                k++;
            }
            a->id[k] = '\0';
        } else if (html_attr_eq(name, nl, "style")) {
            int k = 0;
            while (k < vlen && k < (int)sizeof(a->style) - 1) {
                a->style[k] = val[k];
                k++;
            }
            a->style[k] = '\0';
        } else if (html_attr_eq(name, nl, "alt")) {
            int k = 0;
            while (k < vlen && k < (int)sizeof(a->alt) - 1) {
                a->alt[k] = val[k];
                k++;
            }
            a->alt[k] = '\0';
        } else if (html_attr_eq(name, nl, "align")) {
            if (h_icontains_n(val, vlen, "center")) a->align_set = 1;
            else if (h_icontains_n(val, vlen, "right")) a->align_set = 2;
            else a->align_set = 3;
        }
    }

    return i;
}

static int html_link_new(const char *href) {
    if (g_link_count >= HTML_LINKS_MAX) return -1;
    if (!href || !href[0]) return -1;

    int idx = g_link_count++;
    g_links[idx].used = 0;
    g_links[idx].x0 = 0;
    g_links[idx].y0 = 0;
    g_links[idx].x1 = 0;
    g_links[idx].y1 = 0;
    h_copy(g_links[idx].href, href, (int)sizeof(g_links[idx].href));
    return idx;
}

static void html_start_tag(const char *tag, const struct HtmlAttrs *at) {
    struct HtmlStyle st;
    html_style_inherit(html_top_style(), &st);
    html_default_style(tag, &st);

    /* 类选择器与 id 选择器按出现顺序覆盖 */
    for (int i = 0; i < g_rule_count; i++) {
        if (html_selector_matches(g_rules[i].selector, tag, at->cls, at->id)) {
            struct HtmlStyle tmp = st;
            /* 只覆盖规则里显式设置的字段 */
            if (g_rules[i].style.has_color) {
                tmp.color = g_rules[i].style.color;
                tmp.has_color = 1;
            }
            if (g_rules[i].style.has_bg) {
                tmp.bg = g_rules[i].style.bg;
                tmp.has_bg = 1;
            }
            if (g_rules[i].style.scale > 1) tmp.scale = g_rules[i].style.scale;
            if (g_rules[i].style.bold) tmp.bold = 1;
            if (g_rules[i].style.underline) tmp.underline = 1;
            if (g_rules[i].style.align != 0) tmp.align = g_rules[i].style.align;
            if (g_rules[i].style.margin_top > 0) tmp.margin_top = g_rules[i].style.margin_top;
            if (g_rules[i].style.margin_bottom > 0) tmp.margin_bottom = g_rules[i].style.margin_bottom;
            if (g_rules[i].style.indent > 0) tmp.indent = g_rules[i].style.indent;
            if (g_rules[i].style.line_h > HTML_CHAR_H) tmp.line_h = g_rules[i].style.line_h;
            st = tmp;
        }
    }

    if (at->style[0]) {
        html_apply_decls(at->style, h_strlen(at->style), &st);
    }

    if (at->align_set == 1) st.align = 1;
    else if (at->align_set == 2) st.align = 2;
    else if (at->align_set == 3) st.align = 0;

    if (html_is_block(tag)) {
        if (g_line_n > 0) html_flush_line();
        g_cy += st.margin_top;
    }

    if (h_ieq(tag, "br")) {
        html_flush_line();
        return;
    }

    if (h_ieq(tag, "hr")) {
        if (g_line_n > 0) html_flush_line();
        if (g_draw) {
            int y = g_cy - g_scroll + 8;
            vbe_fill_rect((uint16_t)(g_x + g_indent), (uint16_t)y,
                          (uint16_t)(g_w - g_indent), 2, 0x2B3949);
        }
        g_cy += 20;
        if (g_cy > g_max_y) g_max_y = g_cy;
        return;
    }

    if (h_ieq(tag, "img")) {
        if (at->alt[0]) {
            char tmp[56];
            int p = 0;
            tmp[p++] = '[';
            for (int i = 0; at->alt[i] && p < (int)sizeof(tmp) - 3; i++) {
                tmp[p++] = at->alt[i];
            }
            tmp[p++] = ']';
            tmp[p] = '\0';
            html_put_word(tmp, p, html_top_style());
        } else {
            html_put_word("[image]", 7, html_top_style());
        }
        return;
    }

    if (h_ieq(tag, "pre")) {
        g_pre = 1;
    }

    if (h_ieq(tag, "ul") || h_ieq(tag, "ol")) {
        if (g_list_depth < 8) g_list_depth++;
        st.indent += 20;
    }

    if (h_ieq(tag, "li")) {
        if (g_line_n > 0) html_flush_line();
        html_put_word("-", 1, &st);
    }

    if (h_ieq(tag, "td")) {
        if (g_line_n > 0) {
            html_put_word(" | ", 3, html_top_style());
        }
    }

    if (h_ieq(tag, "a")) {
        g_cur_link = html_link_new(at->href);
        st.has_color = 1;
        st.color = 0x6AA4FF;
        st.underline = 1;
    }

    if (h_ieq(tag, "script") || h_ieq(tag, "style") || h_ieq(tag, "title")) {
        g_skip = 1;
        h_copy(g_skip_tag, tag, (int)sizeof(g_skip_tag));
    }

    html_push_style(&st);
}

static void html_end_tag(const char *tag) {
    if (h_ieq(tag, "script") || h_ieq(tag, "style") || h_ieq(tag, "title")) {
        g_skip = 0;
        g_skip_tag[0] = '\0';
        html_pop_style();
        return;
    }

    if (g_cur_link >= 0 && h_ieq(tag, "a")) {
        g_cur_link = -1;
    }

    if (h_ieq(tag, "pre")) {
        g_pre = 0;
    }

    if (h_ieq(tag, "ul") || h_ieq(tag, "ol")) {
        if (g_list_depth > 0) g_list_depth--;
    }

    struct HtmlStyle st = *html_top_style();

    if (html_is_block(tag)) {
        if (g_line_n > 0) html_flush_line();
        g_cy += st.margin_bottom;
    }

    html_pop_style();
}

/* 从标签文本里取名字（到空白或 '/' 为止） */
static int html_tag_name(const char *t, int n, char *out, int cap) {
    int i = 0;
    int p = 0;

    while (i < n && h_isspace(t[i])) i++;
    while (i < n && !h_isspace(t[i]) && t[i] != '/' && p < cap - 1) {
        out[p++] = t[i++];
    }
    out[p] = '\0';
    return i;
}

static void html_handle_tag(const char *t, int n) {
    if (n <= 0) return;

    if (t[0] == '!') {
        /* <!DOCTYPE ...> 与注释已在主循环处理，这里兜底忽略 */
        return;
    }

    int closing = 0;
    int i = 0;
    if (t[0] == '/') {
        closing = 1;
        i = 1;
    }

    char name[24];
    int used = html_tag_name(t + i, n - i, name, (int)sizeof(name));
    if (!name[0]) return;

    if (closing) {
        html_end_tag(name);
        return;
    }

    struct HtmlAttrs at;
    html_attrs_clear(&at);
    html_parse_attrs(t + i + used, n - i - used, &at);

    html_start_tag(name, &at);

    if (html_is_void(name)) {
        /* 空元素没有闭合标签，立刻退栈 */
        html_pop_style();
    }
}

/* ============================================================
 * CSS 提取
 * ============================================================ */

static void html_add_rule(const char *sel, int sel_n, const char *decls, int decl_n) {
    if (g_rule_count >= HTML_RULES_MAX || sel_n <= 0) return;

    struct HtmlStyle st;
    html_style_clear(&st);
    html_apply_decls(decls, decl_n, &st);

    /* 逗号分隔的多个选择器拆成多条规则 */
    int i = 0;
    while (i < sel_n && g_rule_count < HTML_RULES_MAX) {
        while (i < sel_n && (h_isspace(sel[i]) || sel[i] == ',')) i++;
        int start = i;
        while (i < sel_n && sel[i] != ',' && !h_isspace(sel[i])) i++;
        int len = i - start;
        if (len <= 0) continue;
        if (len >= (int)sizeof(g_rules[0].selector)) {
            len = (int)sizeof(g_rules[0].selector) - 1;
        }

        /* 后代选择器只取末段，保证 ".nav a" 至少能命中 <a> */
        int tail = start + len;
        int k = tail;
        while (k > start && sel[k - 1] != ' ') k--;

        struct HtmlRule *r = &g_rules[g_rule_count];
        int cp = 0;
        for (int q = k; q < tail && cp < (int)sizeof(r->selector) - 1; q++) {
            r->selector[cp++] = sel[q];
        }
        r->selector[cp] = '\0';
        r->style = st;
        g_rule_count++;
    }
}

static void html_parse_css(const char *src, int len) {
    int i = 0;

    while (i < len) {
        if (src[i] != '<') { i++; continue; }

        int j = i + 1;
        int closing = 0;
        if (j < len && src[j] == '/') {
            closing = 1;
            j++;
        }

        char name[16];
        int p = 0;
        int k = j;
        while (k < len && !h_isspace(src[k]) && src[k] != '>' && p < 15) {
            name[p++] = h_lower((unsigned char)src[k]);
            k++;
        }
        name[p] = '\0';

        while (k < len && src[k] != '>') k++;
        if (k >= len) break;
        i = k + 1;

        if (!h_ieq_n(name, p, "style") || closing) continue;

        /* 线性查找 </style>：不能用"每次从头比较"的写法，
         * 128KB 页面会退化成 O(n²)。 */
        int end = i;
        int body_end = len;
        while (end + 7 <= len) {
            if (src[end] == '<' && src[end + 1] == '/' &&
                h_lower((unsigned char)src[end + 2]) == 's' &&
                h_lower((unsigned char)src[end + 3]) == 't' &&
                h_lower((unsigned char)src[end + 4]) == 'y' &&
                h_lower((unsigned char)src[end + 5]) == 'l' &&
                h_lower((unsigned char)src[end + 6]) == 'e') {
                body_end = end;
                break;
            }
            end++;
        }

        int q = i;
        while (q < body_end) {
            while (q < body_end && h_isspace(src[q])) q++;
            if (q >= body_end) break;
            if (src[q] == '@') {
                while (q < body_end && src[q] != '}') q++;
                q++;
                continue;
            }

            int sel_start = q;
            while (q < body_end && src[q] != '{') q++;
            if (q >= body_end) break;
            int sel_n = q - sel_start;
            q++;

            int decl_start = q;
            while (q < body_end && src[q] != '}') q++;
            int decl_n = q - decl_start;
            if (q < body_end) q++;

            html_add_rule(src + sel_start, sel_n, src + decl_start, decl_n);
        }

        i = body_end + 7;
        if (i > len) i = len;
    }
}

/* ============================================================
 * 主循环
 * ============================================================ */

static void html_run_text(const char *src, int len) {
    int i = 0;

    while (i < len) {
        if (src[i] == '<') {
            if (i + 3 < len && src[i + 1] == '!' &&
                src[i + 2] == '-' && src[i + 3] == '-') {
                i += 4;
                while (i + 2 < len &&
                       !(src[i] == '-' && src[i + 1] == '-' && src[i + 2] == '>')) {
                    i++;
                }
                i += 3;
                continue;
            }

            int j = i + 1;
            while (j < len && src[j] != '>') j++;
            if (j >= len) break;

            if (g_skip) {
                /* 跳过 script/style/title 内部：只等同名结束标签 */
                if (src[i + 1] == '/') {
                    char nm[16];
                    int p = 0;
                    int k = i + 2;
                    while (k < j && !h_isspace(src[k]) && p < 15) {
                        nm[p++] = h_lower((unsigned char)src[k]);
                        k++;
                    }
                    nm[p] = '\0';
                    if (h_ieq_n(nm, p, g_skip_tag)) {
                        g_skip = 0;
                        g_skip_tag[0] = '\0';
                    }
                }
                i = j + 1;
                continue;
            }

            html_handle_tag(src + i + 1, j - i - 1);
            i = j + 1;
            continue;
        }

        if (g_skip) {
            i++;
            continue;
        }

        struct HtmlStyle *st = html_top_style();

        if (g_pre) {
            if (src[i] == '\n') {
                html_flush_line();
                i++;
                continue;
            }
            if (src[i] == '\r') { i++; continue; }
            html_put_char(src[i], st);
            i++;
            continue;
        }

        if (h_isspace(src[i])) {
            g_pending_space = 1;
            i++;
            continue;
        }

        int start = i;
        while (i < len && src[i] != '<' && !h_isspace(src[i])) i++;
        html_put_word(src + start, i - start, st);
    }
}

int html_render(const char *src, int len, int x, int y, int w, int scroll,
                int *content_h) {
    if (content_h) *content_h = 0;
    if (!src || len <= 0 || w < HTML_CHAR_W * 2) return 0;

    html_reset();
    html_parse_css(src, len);

    /* 第一遍：测量 */
    g_draw = 0;
    g_x = x;
    g_w = w;
    g_cy = y;
    g_max_y = y;
    g_scroll = 0;

    html_run_text(src, len);
    if (g_line_n > 0) html_flush_line();

    int total_h = g_max_y - y;
    if (total_h < 0) total_h = 0;

    /* 第二遍：绘制（链接表在绘制时重建） */
    g_link_count = 0;
    g_cur_link = -1;
    g_draw = 1;
    g_depth = 1;
    html_style_clear(&g_stack[0]);
    g_cy = y;
    g_max_y = y;
    g_scroll = scroll;
    g_indent = 0;
    g_pre = 0;
    g_skip = 0;
    g_pending_space = 0;
    g_line_n = 0;
    g_line_w = 0;
    g_line_h = HTML_CHAR_H;

    html_run_text(src, len);
    if (g_line_n > 0) html_flush_line();

    if (content_h) *content_h = total_h;
    return total_h;
}

int html_link_at(int px, int py, char *out, int cap) {
    for (int i = 0; i < g_link_count; i++) {
        const struct HtmlLink *l = &g_links[i];
        if (!l->used) continue;
        if (px >= l->x0 && px < l->x1 && py >= l->y0 && py < l->y1) {
            if (out && cap > 0) h_copy(out, l->href, cap);
            return 1;
        }
    }
    return 0;
}
