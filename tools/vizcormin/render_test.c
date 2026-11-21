/* ============================================================
 * render_test.c - VizCormin 渲染层测试（内存后端）
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vc_text.h"
#include "vc_view.h"
#include "vc_render.h"

typedef struct {
    int kind;        /* 0 = rect, 1 = glyph */
    int x, y, w, h;
    uint32_t color;
    uint32_t cp;
} Op;

typedef struct {
    Op ops[1024];
    int count;
} Recorder;

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); failures++; } } while (0)

static void rec_fill(void *user, int x, int y, int w, int h, uint32_t color) {
    Recorder *r = (Recorder *)user;
    if (r->count >= 1024) return;
    Op *o = &r->ops[r->count++];
    o->kind = 0; o->x = x; o->y = y; o->w = w; o->h = h;
    o->color = color; o->cp = 0;
}

static void rec_glyph(void *user, int x, int y, uint32_t cp, uint32_t color) {
    Recorder *r = (Recorder *)user;
    if (r->count >= 1024) return;
    Op *o = &r->ops[r->count++];
    o->kind = 1; o->x = x; o->y = y; o->w = 0; o->h = 0;
    o->color = color; o->cp = cp;
}

static int has_glyph(const Recorder *r, uint32_t cp) {
    for (int i = 0; i < r->count; i++) {
        if (r->ops[i].kind == 1 && r->ops[i].cp == cp) return 1;
    }
    return 0;
}

static int has_rect(const Recorder *r, int x, int y, int w, int h, uint32_t color) {
    for (int i = 0; i < r->count; i++) {
        const Op *o = &r->ops[i];
        if (o->kind == 0 && o->x == x && o->y == y &&
            o->w == w && o->h == h && o->color == color) return 1;
    }
    return 0;
}

static VcTheme make_theme(void) {
    VcTheme t;
    t.bg = 0x101014;
    t.fg = 0xE9EFF7;
    t.dim = 0x8EA0B4;
    t.cursor = 0xFFD24A;
    t.selection = 0x2F4C8A;
    t.current_line = 0x1A1D24;
    t.gutter_bg = 0x15181E;
    t.gutter_fg = 0x6E7A8A;
    t.cell_w = 8;
    t.cell_h = 16;
    t.padding_x = 2;
    t.padding_y = 2;
    t.gutter_cols = 2;
    return t;
}

static void test_frame(void) {
    VcText *t = vc_create();
    vc_load(t, "abc\ndef", 7);
    VcView *v = vc_view_create(20, 5);
    vc_view_set_text(v, t);

    Recorder rec;
    memset(&rec, 0, sizeof(rec));
    VcSurface s;
    s.width = 200;
    s.height = 100;
    s.user = &rec;
    s.fill_rect = rec_fill;
    s.draw_glyph = rec_glyph;

    VcTheme th = make_theme();
    VcRenderer *r = vc_render_create(&s, &th);
    CHECK(r != 0, "renderer create");

    VcRenderStats st = vc_render_frame(r, v, 0, 2, 1, 3, 1);

    /* 文本 6 个字符 + 行号 2 个数字 */
    CHECK(st.glyphs == 8, "glyph count");
    CHECK(has_glyph(&rec, 'a') && has_glyph(&rec, 'b') && has_glyph(&rec, 'c'), "row 0 glyphs");
    CHECK(has_glyph(&rec, 'd') && has_glyph(&rec, 'e') && has_glyph(&rec, 'f'), "row 1 glyphs");
    CHECK(has_glyph(&rec, '1') && has_glyph(&rec, '2'), "gutter numbers");

    /* 文本区起点 = padding 2 + gutter 2*8 = 18 */
    CHECK(has_rect(&rec, 18, 2, 182, 16, th.current_line), "current line rect");
    CHECK(has_rect(&rec, 26, 2, 16, 16, th.selection), "selection rect");
    CHECK(has_rect(&rec, 34, 4, 2, 12, th.cursor), "caret rect");

    vc_render_destroy(r);
    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_scroll(void) {
    VcText *t = vc_create();
    vc_load(t, "abcdefghij", 10);
    VcView *v = vc_view_create(20, 2);
    vc_view_set_text(v, t);

    Recorder rec;
    memset(&rec, 0, sizeof(rec));
    VcSurface s;
    s.width = 200;
    s.height = 40;
    s.user = &rec;
    s.fill_rect = rec_fill;
    s.draw_glyph = rec_glyph;

    VcTheme th = make_theme();
    VcRenderer *r = vc_render_create(&s, &th);
    VcRenderStats st = vc_render_frame(r, v, 5, 0, 0, 0, 0);

    /* 列 5 是 'f'：可见 f..j 共 5 个 + 行号 1 个 */
    CHECK(st.glyphs == 6, "scrolled glyph count");
    CHECK(has_glyph(&rec, 'f') && has_glyph(&rec, 'j'), "visible glyphs");
    CHECK(!has_glyph(&rec, 'e'), "hidden glyph not submitted");

    vc_render_destroy(r);
    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_empty(void) {
    VcText *t = vc_create();
    vc_load(t, "", 0);
    VcView *v = vc_view_create(10, 2);
    vc_view_set_text(v, t);

    Recorder rec;
    memset(&rec, 0, sizeof(rec));
    VcSurface s;
    s.width = 100;
    s.height = 32;
    s.user = &rec;
    s.fill_rect = rec_fill;
    s.draw_glyph = rec_glyph;

    VcTheme th = make_theme();
    VcRenderer *r = vc_render_create(&s, &th);
    VcRenderStats st = vc_render_frame(r, v, 0, 0, 0, 0, 1);

    CHECK(st.glyphs == 1, "empty doc only line number");
    CHECK(has_glyph(&rec, '1'), "line 1 number");

    vc_render_destroy(r);
    vc_view_destroy(v);
    vc_destroy(t);
}

int main(void) {
    test_frame();
    test_scroll();
    test_empty();

    if (failures) {
        printf("RENDER CORE: %d failure(s)\n", failures);
        return 1;
    }
    printf("RENDER CORE: all tests passed\n");
    return 0;
}
