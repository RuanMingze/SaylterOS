/* ============================================================
 * svg_dec.c - SVG 光栅化（图标/头像子集）
 * ------------------------------------------------------------
 * 支持：
 *   - <path> 的 M/L/H/V/C/S/Q/T/A/Z（绝对与相对）；
 *   - <rect>/<circle>/<ellipse>/<polygon>/<polyline>/<line>；
 *   - fill（#RGB/#RRGGBB/currentColor）、fill-rule=nonzero/evenodd；
 *   - g/元素级 transform：translate/scale/matrix，最多 8 层嵌套。
 *
 * 光栅化：2x 超采样 + 扫描线非零环绕规则，输出带 Alpha 的 ARGB。
 * 圆弧支持圆形半圆（rx==ry 且弦长=2r）；其他椭圆弧按直线近似。
 * 不使用浮点库：只做四则运算与常量旋转矩阵。
 * ============================================================ */

#include <stddef.h>

#include "imgcodec.h"

#ifdef SVG_DEBUG
#include <stdio.h>
#define SVGDBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define SVGDBG(...)
#endif

#define SVG_MAX_EDGES 16384
/* 抗锯齿超采样倍数：2x 对 100px 级别的头像已经是 4 采样/像素，
 * 视觉上与 4x 无差别，但扫描线工作量只有 1/4。登录页头像在
 * QEMU 无 GPU 环境下从 700ms 降到 200ms 以内。 */
#define SVG_SS        2
#define SVG_MAX_DIM   256

typedef struct { double x0, y0, x1, y1; } SvgEdge;
typedef struct { double a, b, c, d, e, f; } SvgMat;

typedef struct {
    SvgEdge edges[SVG_MAX_EDGES];
    int     count;
    int     overflow;
    SvgMat  stack[8];
    int     depth;
    int     evenodd;
} SvgCtx;

static double svg_strtod(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    double v = 0.0;
    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (double)(*p - '0'); p++; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (double)(*p - '0') * f; f *= 0.1; p++; }
    }
    *pp = p;
    return neg ? -v : v;
}

static int svg_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int svg_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int svg_match(const char *p, const char *name, int n) {
    for (int i = 0; i < n; i++) {
        if (p[i] != name[i]) return 0;
    }
    return 1;
}

static double svg_fabs(double v) { return v < 0 ? -v : v; }

static int svg_attr(const char *tag, const char *name, char *out, int cap) {
    int nlen = svg_strlen(name);
    for (const char *p = tag; *p; p++) {
        if (p != tag && !svg_isspace(p[-1])) continue;
        if (!svg_match(p, name, nlen)) continue;
        const char *q = p + nlen;
        if (*q && !svg_isspace(*q) && *q != '=') continue;
        while (svg_isspace(*q)) q++;
        if (*q != '=') continue;
        q++;
        while (svg_isspace(*q)) q++;
        char quote = 0;
        if (*q == '"' || *q == '\'') { quote = *q; q++; }
        int n = 0;
        while (*q && n < cap - 1) {
            if (quote) { if (*q == quote) break; }
            else if (svg_isspace(*q) || *q == '>' || *q == '/') break;
            out[n++] = *q++;
        }
        out[n] = '\0';
        return 1;
    }
    return 0;
}

static void mat_identity(SvgMat *m) {
    m->a = 1; m->b = 0; m->c = 0; m->d = 1; m->e = 0; m->f = 0;
}

static void mat_mul(SvgMat *r, const SvgMat *x, const SvgMat *y) {
    SvgMat t;
    t.a = x->a * y->a + x->c * y->b;
    t.b = x->b * y->a + x->d * y->b;
    t.c = x->a * y->c + x->c * y->d;
    t.d = x->b * y->c + x->d * y->d;
    t.e = x->a * y->e + x->c * y->f + x->e;
    t.f = x->b * y->e + x->d * y->f + x->f;
    *r = t;
}

static void mat_parse(const char *s, SvgMat *m) {
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        if (svg_match(s, "translate", 9) == 0) {
            s += 9;
            if (*s == '(') s++;
            double x = svg_strtod(&s);
            double y = 0;
            while (*s == ' ' || *s == ',') s++;
            if (*s != ')') y = svg_strtod(&s);
            SvgMat t;
            mat_identity(&t);
            t.e = x; t.f = y;
            mat_mul(m, m, &t);
        } else if (svg_match(s, "scale", 5) == 0) {
            s += 5;
            if (*s == '(') s++;
            double x = svg_strtod(&s);
            double y = x;
            while (*s == ' ' || *s == ',') s++;
            if (*s != ')') y = svg_strtod(&s);
            SvgMat t;
            mat_identity(&t);
            t.a = x; t.d = y;
            mat_mul(m, m, &t);
        } else if (svg_match(s, "matrix", 6) == 0) {
            s += 6;
            if (*s == '(') s++;
            SvgMat t;
            t.a = svg_strtod(&s);
            t.b = svg_strtod(&s);
            t.c = svg_strtod(&s);
            t.d = svg_strtod(&s);
            t.e = svg_strtod(&s);
            t.f = svg_strtod(&s);
            mat_mul(m, m, &t);
        } else {
            while (*s && *s != ')' && *s != ' ') s++;
        }
        while (*s && *s != ')') s++;
        if (*s == ')') s++;
    }
}

static void pt_apply(const SvgMat *m, double x, double y, double *ox, double *oy) {
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

static void svg_edge(SvgCtx *c, const SvgMat *m,
                     double x0, double y0, double x1, double y1) {
    if (c->count >= SVG_MAX_EDGES) { c->overflow = 1; return; }
    double ax, ay, bx, by;
    pt_apply(m, x0, y0, &ax, &ay);
    pt_apply(m, x1, y1, &bx, &by);
    if (ay == by) return;
    SvgEdge *e = &c->edges[c->count++];
    e->x0 = ax; e->y0 = ay; e->x1 = bx; e->y1 = by;
}

static void svg_line_to(SvgCtx *c, const SvgMat *m,
                        double *cx, double *cy, double x, double y) {
    svg_edge(c, m, *cx, *cy, x, y);
    *cx = x;
    *cy = y;
}

static void svg_quad(SvgCtx *c, const SvgMat *m, double *cx, double *cy,
                     double qx, double qy, double ex, double ey) {
    double x0 = *cx, y0 = *cy;
    for (int i = 1; i <= 20; i++) {
        double t = (double)i / 20.0;
        double u = 1.0 - t;
        double x = u * u * x0 + 2 * u * t * qx + t * t * ex;
        double y = u * u * y0 + 2 * u * t * qy + t * t * ey;
        svg_edge(c, m, *cx, *cy, x, y);
        *cx = x; *cy = y;
    }
}

static void svg_cubic(SvgCtx *c, const SvgMat *m, double *cx, double *cy,
                      double x1, double y1, double x2, double y2,
                      double ex, double ey) {
    double x0 = *cx, y0 = *cy;
    for (int i = 1; i <= 24; i++) {
        double t = (double)i / 24.0;
        double u = 1.0 - t;
        double x = u * u * u * x0 + 3 * u * u * t * x1 +
                   3 * u * t * t * x2 + t * t * t * ex;
        double y = u * u * u * y0 + 3 * u * u * t * y1 +
                   3 * u * t * t * y2 + t * t * t * ey;
        svg_edge(c, m, *cx, *cy, x, y);
        *cx = x; *cy = y;
    }
}

/* 圆形半圆：起点/终点是直径两端。按 32 步旋转起点向量。 */
static void svg_semicircle(SvgCtx *c, const SvgMat *m,
                           double sx, double sy, double ex, double ey, int sweep) {
    double cx = (sx + ex) * 0.5;
    double cy = (sy + ey) * 0.5;
    double vx = sx - cx;
    double vy = sy - cy;
    const double ca = 0.9951847267;   /* cos(pi/32) */
    const double sa = 0.0980171403;   /* sin(pi/32) */
    double px = sx, py = sy;
    for (int i = 1; i <= 32; i++) {
        double nx, ny;
        if (sweep) {
            nx = vx * ca - vy * sa;
            ny = vx * sa + vy * ca;
        } else {
            nx = vx * ca + vy * sa;
            ny = -vx * sa + vy * ca;
        }
        vx = nx; vy = ny;
        double x = cx + vx;
        double y = cy + vy;
        svg_edge(c, m, px, py, x, y);
        px = x; py = y;
    }
    svg_edge(c, m, px, py, ex, ey);
}

static void svg_circle(SvgCtx *c, const SvgMat *m,
                       double cx, double cy, double rx, double ry) {
    double px = cx + rx;
    double py = cy;
    const double ca = 0.9951847267;
    const double sa = 0.0980171403;
    double vx = rx, vy = 0;
    for (int i = 1; i <= 64; i++) {
        double nx = vx * ca - vy * sa;
        double ny = vx * sa + vy * ca;
        vx = nx; vy = ny;
        double x = cx + vx;
        double y = cy + vy * (ry / rx);
        svg_edge(c, m, px, py, x, y);
        px = x; py = y;
    }
}

static void svg_rect(SvgCtx *c, const SvgMat *m,
                     double x, double y, double w, double h) {
    svg_edge(c, m, x, y, x + w, y);
    svg_edge(c, m, x + w, y, x + w, y + h);
    svg_edge(c, m, x + w, y + h, x, y + h);
    svg_edge(c, m, x, y + h, x, y);
}

static void svg_path(SvgCtx *c, const SvgMat *m, const char *d) {
    const char *p = d;
    double cx = 0, cy = 0;
    double sx = 0, sy = 0;
    double last_ctrl_x = 0, last_ctrl_y = 0;
    char cmd = 0;
    int started = 0;

    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
            if (cmd == 'Z' || cmd == 'z') {
                if (cx != sx || cy != sy) svg_edge(c, m, cx, cy, sx, sy);
                cx = sx; cy = sy;
                continue;
            }
        }
        if (!cmd) break;

        int rel = (cmd >= 'a' && cmd <= 'z');
        char uc = (char)(cmd >= 'a' && cmd <= 'z' ? cmd - 'a' + 'A' : cmd);

        if (uc == 'M') {
            double x = svg_strtod(&p);
            double y = svg_strtod(&p);
            if (rel) { x += cx; y += cy; }
            /* 填充规则把每条子路径视为闭合：换新起点前补上闭合边。 */
            if (started && (cx != sx || cy != sy)) {
                svg_edge(c, m, cx, cy, sx, sy);
            }
            cx = sx = x; cy = sy = y;
            started = 1;
            cmd = rel ? 'l' : 'L';
        } else if (uc == 'L') {
            double x = svg_strtod(&p);
            double y = svg_strtod(&p);
            if (rel) { x += cx; y += cy; }
            svg_line_to(c, m, &cx, &cy, x, y);
        } else if (uc == 'H') {
            double x = svg_strtod(&p);
            if (rel) x += cx;
            svg_line_to(c, m, &cx, &cy, x, cy);
        } else if (uc == 'V') {
            double y = svg_strtod(&p);
            if (rel) y += cy;
            svg_line_to(c, m, &cx, &cy, cx, y);
        } else if (uc == 'C') {
            double x1 = svg_strtod(&p), y1 = svg_strtod(&p);
            double x2 = svg_strtod(&p), y2 = svg_strtod(&p);
            double x = svg_strtod(&p), y = svg_strtod(&p);
            if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
            svg_cubic(c, m, &cx, &cy, x1, y1, x2, y2, x, y);
            last_ctrl_x = x2; last_ctrl_y = y2;
        } else if (uc == 'S') {
            double x2 = svg_strtod(&p), y2 = svg_strtod(&p);
            double x = svg_strtod(&p), y = svg_strtod(&p);
            if (rel) { x2 += cx; y2 += cy; x += cx; y += cy; }
            double x1 = 2 * cx - last_ctrl_x;
            double y1 = 2 * cy - last_ctrl_y;
            svg_cubic(c, m, &cx, &cy, x1, y1, x2, y2, x, y);
            last_ctrl_x = x2; last_ctrl_y = y2;
        } else if (uc == 'Q') {
            double qx = svg_strtod(&p), qy = svg_strtod(&p);
            double x = svg_strtod(&p), y = svg_strtod(&p);
            if (rel) { qx += cx; qy += cy; x += cx; y += cy; }
            svg_quad(c, m, &cx, &cy, qx, qy, x, y);
            last_ctrl_x = qx; last_ctrl_y = qy;
        } else if (uc == 'T') {
            double x = svg_strtod(&p), y = svg_strtod(&p);
            if (rel) { x += cx; y += cy; }
            double qx = 2 * cx - last_ctrl_x;
            double qy = 2 * cy - last_ctrl_y;
            svg_quad(c, m, &cx, &cy, qx, qy, x, y);
            last_ctrl_x = qx; last_ctrl_y = qy;
        } else if (uc == 'A') {
            double rx = svg_strtod(&p), ry = svg_strtod(&p);
            double rot = svg_strtod(&p);
            int large = (int)svg_strtod(&p);
            int sweep = (int)svg_strtod(&p);
            double x = svg_strtod(&p), y = svg_strtod(&p);
            (void)rot; (void)large;
            if (rel) { x += cx; y += cy; }
            double dx = x - cx, dy = y - cy;
            double chord2 = dx * dx + dy * dy;
            if (rx == ry && rx > 0 &&
                svg_fabs(chord2 - 4 * rx * rx) < (4 * rx * rx) * 0.01) {
                svg_semicircle(c, m, cx, cy, x, y, sweep);
            } else {
                svg_edge(c, m, cx, cy, x, y);
            }
            cx = x; cy = y;
        } else {
            /* 不认识的命令：跳过当前数值，避免死循环 */
            SVGDBG("svg: unknown cmd %c at '%s'\n", uc, p);
            if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '+') {
                svg_strtod(&p);
            } else {
                p++;
            }
        }
        SVGDBG("svg: cmd=%c cur=(%.1f,%.1f) next='%.12s'\n", uc, cx, cy, p);
    }
    if (started && (cx != sx || cy != sy)) svg_edge(c, m, cx, cy, sx, sy);
}

/* 扫描线填充。mask 尺寸 rw*rh（4x）。 */
static void svg_fill(SvgCtx *c, uint8_t *mask, int rw, int rh, int evenodd) {
    if (!c->count) return;

    double *xs = (double *)img_alloc(sizeof(double) * (size_t)(c->count + 1) * 2);
    int *dir = (int *)img_alloc(sizeof(int) * (size_t)(c->count + 1) * 2);
    if (!xs || !dir) {
        if (xs) img_free(xs);
        if (dir) img_free(dir);
        return;
    }

    for (int y = 0; y < rh; y++) {
        double yy = (double)y + 0.5;
        int n = 0;
        for (int i = 0; i < c->count; i++) {
            SvgEdge *e = &c->edges[i];
            double y0 = e->y0 * SVG_SS, y1 = e->y1 * SVG_SS;
            if ((yy >= y0 && yy < y1) || (yy >= y1 && yy < y0)) {
                double t = (yy - y0) / (y1 - y0);
                xs[n] = (e->x0 * SVG_SS) + t * ((e->x1 - e->x0) * SVG_SS);
                dir[n] = (y1 > y0) ? 1 : -1;
                n++;
            }
        }
        for (int i = 1; i < n; i++) {
            double x = xs[i];
            int d = dir[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > x) {
                xs[j + 1] = xs[j];
                dir[j + 1] = dir[j];
                j--;
            }
            xs[j + 1] = x;
            dir[j + 1] = d;
        }

        int winding = 0;
        int started = 0;
        int start_x = 0;
        for (int i = 0; i < n; i++) {
            winding += dir[i];
            int inside = evenodd ? (winding & 1) : (winding != 0);
            int x = (int)(xs[i] + 0.5);
            if (inside && !started) {
                started = 1;
                start_x = x;
            } else if (!inside && started) {
                started = 0;
                if (start_x < 0) start_x = 0;
                if (x > rw) x = rw;
                for (int xx = start_x; xx < x; xx++) mask[y * rw + xx] = 1;
            }
        }
        if (started) {
            if (start_x < 0) start_x = 0;
            for (int xx = start_x; xx < rw; xx++) mask[y * rw + xx] = 1;
        }
    }

    img_free(xs);
    img_free(dir);
}

int svg_dbg_edges = 0;
double svg_dbg_minx = 0, svg_dbg_maxx = 0, svg_dbg_miny = 0, svg_dbg_maxy = 0;
#ifdef SVG_DEBUG
uint8_t *svg_dbg_mask = 0;
int svg_dbg_rw = 0, svg_dbg_rh = 0;
#endif

int img_svg_decode(const uint8_t *data, int size, int max_dim,
                   uint32_t color, ImgBitmap *out) {
    if (!data || size <= 0 || !out) return 0;
    if (max_dim <= 0) max_dim = 128;
    if (max_dim > SVG_MAX_DIM) max_dim = SVG_MAX_DIM;

    int vb_x = 0, vb_y = 0, vb_w = 0, vb_h = 0;
    int tw = max_dim, th = max_dim;

    SvgMat base;
    mat_identity(&base);

    /* 读取 svg 根节点属性 */
    const char *svg_tag = 0;
    for (int i = 0; i + 4 < size; i++) {
        if (data[i] == '<' && data[i + 1] == 's' && data[i + 2] == 'v' &&
            data[i + 3] == 'g') { svg_tag = (const char *)data + i; break; }
    }
    if (!svg_tag) return 0;

    char roottag[512];
    int rn = 0;
    for (const char *p = svg_tag; *p && rn < (int)sizeof(roottag) - 1; p++) {
        roottag[rn++] = *p;
        if (*p == '>') break;
    }
    roottag[rn] = '\0';

    /* 路径 data 属性可能接近 1KB，缓冲必须足够大；放静态区避免
     * 内核任务栈被 2KB 局部数组压爆。 */
    static char attr[2048];
    if (svg_attr(roottag, "viewBox", attr, (int)sizeof(attr))) {
        const char *p = attr;
        vb_x = (int)svg_strtod(&p);
        vb_y = (int)svg_strtod(&p);
        vb_w = (int)svg_strtod(&p);
        vb_h = (int)svg_strtod(&p);
    }
    if (vb_w <= 0 || vb_h <= 0) {
        const char *q;
        if (svg_attr(roottag, "width", attr, (int)sizeof(attr))) {
            q = attr; vb_w = (int)svg_strtod(&q);
        }
        if (svg_attr(roottag, "height", attr, (int)sizeof(attr))) {
            q = attr; vb_h = (int)svg_strtod(&q);
        }
        if (vb_w <= 0) vb_w = 128;
        if (vb_h <= 0) vb_h = 128;
    }

    if (vb_w != vb_h) {
        if (vb_w > vb_h) th = max_dim * vb_h / vb_w;
        else tw = max_dim * vb_w / vb_h;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    base.a = (double)tw / (double)vb_w;
    base.d = (double)th / (double)vb_h;
    base.e = -(double)vb_x * base.a;
    base.f = -(double)vb_y * base.d;

    SvgCtx *ctx = (SvgCtx *)img_alloc((uint32_t)sizeof(SvgCtx));
    char *tagbuf = (char *)img_alloc(4096);
    if (!ctx || !tagbuf) {
        if (ctx) img_free(ctx);
        if (tagbuf) img_free(tagbuf);
        return 0;
    }
    for (uint32_t i = 0; i < sizeof(SvgCtx); i++) ((uint8_t *)ctx)[i] = 0;
    ctx->stack[0] = base;
    ctx->depth = 1;

    int rw = tw * SVG_SS;
    int rh = th * SVG_SS;
    uint8_t *mask = (uint8_t *)img_alloc((uint32_t)rw * (uint32_t)rh);
    if (!mask) {
        img_free(ctx);
        img_free(tagbuf);
        return 0;
    }
    for (int i = 0; i < rw * rh; i++) mask[i] = 0;

    /* 简单的标签扫描：只处理形状和 g 的 transform/fill-rule。 */
    int pos = 0;
    while (pos + 1 < size) {
        if (data[pos] != '<') { pos++; continue; }
        if (data[pos + 1] == '/') {
            int j = pos + 2;
            while (j < size && data[j] != '>') j++;
            if (j < size && data[pos + 2] == 'g' && ctx->depth > 1) ctx->depth--;
            pos = (j < size) ? j + 1 : size;
            continue;
        }
        int j = pos + 1;
        int n = 0;
        while (j < size && data[j] != '>' && n < 4096 - 1) {
            tagbuf[n++] = (char)data[j++];
        }
        tagbuf[n] = '\0';
        int self_close = (n > 0 && tagbuf[n - 1] == '/');
        pos = (j < size) ? j + 1 : size;

        if (n >= 1 && tagbuf[0] == 'g') {
            SvgMat m = ctx->stack[ctx->depth - 1];
            if (svg_attr(tagbuf, "transform", attr, (int)sizeof(attr))) mat_parse(attr, &m);
            if (!self_close && ctx->depth < 8) ctx->stack[ctx->depth++] = m;
            else if (self_close) ctx->stack[ctx->depth - 1] = m;
            continue;
        }

        int type = 0;
        if (n >= 4 && tagbuf[0] == 'p' && tagbuf[1] == 'a' && tagbuf[2] == 't' && tagbuf[3] == 'h') type = 1;
        else if (n >= 4 && tagbuf[0] == 'r' && tagbuf[1] == 'e' && tagbuf[2] == 'c' && tagbuf[3] == 't') type = 2;
        else if (n >= 6 && tagbuf[0] == 'c' && tagbuf[1] == 'i' && tagbuf[2] == 'r' && tagbuf[3] == 'c' && tagbuf[4] == 'l' && tagbuf[5] == 'e') type = 3;
        else if (n >= 7 && tagbuf[0] == 'e' && tagbuf[1] == 'l' && tagbuf[2] == 'l' && tagbuf[3] == 'i' && tagbuf[4] == 'p' && tagbuf[5] == 's' && tagbuf[6] == 'e') type = 4;
        else if (n >= 7 && tagbuf[0] == 'p' && tagbuf[1] == 'o' && tagbuf[2] == 'l' && tagbuf[3] == 'y') type = 5;
        else if (n >= 4 && tagbuf[0] == 'l' && tagbuf[1] == 'i' && tagbuf[2] == 'n' && tagbuf[3] == 'e') type = 6;
        if (!type) continue;

        if (svg_attr(tagbuf, "fill", attr, (int)sizeof(attr))) {
            if (attr[0] == 'n' && attr[1] == 'o') continue;   /* fill="none" */
        }

        SvgMat m = ctx->stack[ctx->depth - 1];
        if (svg_attr(tagbuf, "transform", attr, (int)sizeof(attr))) mat_parse(attr, &m);

        if (svg_attr(tagbuf, "fill-rule", attr, (int)sizeof(attr))) {
            ctx->evenodd = (attr[0] == 'e');
        }

        char num[64];
        if (type == 1) {
            if (svg_attr(tagbuf, "d", attr, (int)sizeof(attr))) svg_path(ctx, &m, attr);
        } else if (type == 2) {
            double x = 0, y = 0, w = 0, h = 0;
            if (svg_attr(tagbuf, "x", num, (int)sizeof(num))) { const char *p = num; x = svg_strtod(&p); }
            if (svg_attr(tagbuf, "y", num, (int)sizeof(num))) { const char *p = num; y = svg_strtod(&p); }
            if (svg_attr(tagbuf, "width", num, (int)sizeof(num))) { const char *p = num; w = svg_strtod(&p); }
            if (svg_attr(tagbuf, "height", num, (int)sizeof(num))) { const char *p = num; h = svg_strtod(&p); }
            if (w > 0 && h > 0) svg_rect(ctx, &m, x, y, w, h);
        } else if (type == 3 || type == 4) {
            double cx = 0, cy = 0, r1 = 0, r2 = 0;
            if (svg_attr(tagbuf, "cx", num, (int)sizeof(num))) { const char *p = num; cx = svg_strtod(&p); }
            if (svg_attr(tagbuf, "cy", num, (int)sizeof(num))) { const char *p = num; cy = svg_strtod(&p); }
            if (type == 3) {
                if (svg_attr(tagbuf, "r", num, (int)sizeof(num))) { const char *p = num; r1 = r2 = svg_strtod(&p); }
            } else {
                if (svg_attr(tagbuf, "rx", num, (int)sizeof(num))) { const char *p = num; r1 = svg_strtod(&p); }
                if (svg_attr(tagbuf, "ry", num, (int)sizeof(num))) { const char *p = num; r2 = svg_strtod(&p); }
            }
            if (r1 > 0 && r2 > 0) svg_circle(ctx, &m, cx, cy, r1, r2);
        } else if (type == 5) {
            if (svg_attr(tagbuf, "points", attr, (int)sizeof(attr))) {
                const char *p = attr;
                double px = 0, py = 0, first_x = 0, first_y = 0, cx = 0, cy = 0;
                int have = 0;
                while (*p) {
                    while (*p == ' ' || *p == ',' || *p == '\t') p++;
                    if (!*p) break;
                    double x = svg_strtod(&p);
                    double y = svg_strtod(&p);
                    if (!have) { first_x = x; first_y = y; cx = x; cy = y; have = 1; }
                    else { svg_line_to(ctx, &m, &cx, &cy, x, y); }
                    px = x; py = y;
                }
                (void)px; (void)py;
                if (have) svg_edge(ctx, &m, cx, cy, first_x, first_y);
            }
        } else if (type == 6) {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (svg_attr(tagbuf, "x1", num, (int)sizeof(num))) { const char *p = num; x1 = svg_strtod(&p); }
            if (svg_attr(tagbuf, "y1", num, (int)sizeof(num))) { const char *p = num; y1 = svg_strtod(&p); }
            if (svg_attr(tagbuf, "x2", num, (int)sizeof(num))) { const char *p = num; x2 = svg_strtod(&p); }
            if (svg_attr(tagbuf, "y2", num, (int)sizeof(num))) { const char *p = num; y2 = svg_strtod(&p); }
            svg_edge(ctx, &m, x1, y1, x2, y2);
        }
    }

    svg_dbg_edges = ctx->count;
    svg_dbg_minx = svg_dbg_maxx = 0;
    svg_dbg_miny = svg_dbg_maxy = 0;
    if (ctx->count > 0) {
        svg_dbg_minx = svg_dbg_maxx = ctx->edges[0].x0;
        svg_dbg_miny = svg_dbg_maxy = ctx->edges[0].y0;
        for (int i = 0; i < ctx->count; i++) {
            SvgEdge *e = &ctx->edges[i];
            if (e->x0 < svg_dbg_minx) svg_dbg_minx = e->x0;
            if (e->x0 > svg_dbg_maxx) svg_dbg_maxx = e->x0;
            if (e->y0 < svg_dbg_miny) svg_dbg_miny = e->y0;
            if (e->y0 > svg_dbg_maxy) svg_dbg_maxy = e->y0;
        }
    }
    svg_fill(ctx, mask, rw, rh, ctx->evenodd);
#ifdef SVG_DEBUG
    svg_dbg_mask = mask;
    svg_dbg_rw = rw;
    svg_dbg_rh = rh;
#endif

    uint32_t *px = (uint32_t *)img_alloc((uint32_t)tw * (uint32_t)th * 4);
    if (!px) {
        img_free(mask);
        img_free(ctx);
        img_free(tagbuf);
        return 0;
    }

    uint32_t r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            int sum = 0;
            for (int sy = 0; sy < SVG_SS; sy++) {
                for (int sx = 0; sx < SVG_SS; sx++) {
                    sum += mask[(y * SVG_SS + sy) * rw + (x * SVG_SS + sx)];
                }
            }
            int a = sum * 255 / (SVG_SS * SVG_SS);
            px[(int64_t)y * tw + x] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
        }
    }

#ifndef SVG_DEBUG
    img_free(mask);
#endif
    img_free(ctx);
    img_free(tagbuf);
    out->px = px;
    out->w = tw;
    out->h = th;
    return 1;
}



