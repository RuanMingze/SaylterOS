/* ============================================================
 * view_test.c - VizCormin 视图模型与图集测试
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vc_text.h"
#include "vc_view.h"
#include "vc_atlas.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

static void test_wrap(void) {
    VcText *t = vc_create();
    vc_load(t, "abc\ndefgh\nij", 12);

    VcView *v = vc_view_create(3, 4);
    vc_view_set_text(v, t);

    CHECK(vc_view_visual_lines(v) == 4, "wrapped visual line count");

    VcVisualLine vl;
    CHECK(vc_view_line_info(v, 2, &vl), "line info 2");
    CHECK(vl.offset == 7 && vl.length == 2, "wrapped second segment bytes");
    CHECK(vl.col_start == 3 && vl.col_count == 2, "wrapped second segment cols");
    CHECK(vl.line == 1, "wrapped second segment logical line");

    uint32_t vis = 99, col = 99;
    CHECK(vc_view_offset_to_pos(v, 7, &vis, &col), "offset 7 pos");
    CHECK(vis == 2 && col == 3, "offset 7 maps to wrap start");

    CHECK(vc_view_offset_to_pos(v, 8, &vis, &col), "offset 8 pos");
    CHECK(vis == 2 && col == 4, "offset 8 col 4");

    CHECK(vc_view_offset_to_pos(v, 3, &vis, &col), "newline pos");
    CHECK(vis == 0 && col == 3, "newline stays on first visual line");

    CHECK(vc_view_pos_to_offset(v, 2, 4) == 8, "pos to offset 2:4");

    CHECK(vc_view_hit_test(v, 32, 32, 8, 16) == 8, "hit test wraps");
    CHECK(vc_view_hit_test(v, 0, 16, 8, 16) == 4, "hit test line 1");

    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_tabs(void) {
    VcText *t = vc_create();
    vc_load(t, "a\tb", 3);

    VcView *v = vc_view_create(10, 2);
    vc_view_set_wrap(v, 0);
    vc_view_set_text(v, t);

    VcVisualLine vl;
    CHECK(vc_view_line_info(v, 0, &vl), "tab line info");
    CHECK(vl.col_count == 5, "tab advances to stop 4");

    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_nowrap(void) {
    VcText *t = vc_create();
    vc_load(t, "defgh", 5);

    VcView *v = vc_view_create(3, 2);
    vc_view_set_wrap(v, 0);
    vc_view_set_text(v, t);

    CHECK(vc_view_visual_lines(v) == 1, "no-wrap single line");
    VcVisualLine vl;
    vc_view_line_info(v, 0, &vl);
    CHECK(vl.col_count == 5, "no-wrap columns exceed width");

    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_scroll(void) {
    VcText *t = vc_create();
    vc_load(t, "abc\ndefgh\nij", 12);

    VcView *v = vc_view_create(3, 2);
    vc_view_set_text(v, t);

    CHECK(vc_view_visual_lines(v) == 4, "scroll setup");
    vc_view_scroll_to(v, 99);
    CHECK(vc_view_first_visual(v) == 2, "scroll clamp bottom");

    vc_view_scroll_by(v, -99);
    CHECK(vc_view_first_visual(v) == 0, "scroll clamp top");

    vc_view_scroll_by(v, 1);
    CHECK(vc_view_first_visual(v) == 1, "scroll by one");

    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_selection(void) {
    VcText *t = vc_create();
    vc_load(t, "abc\ndefgh\nij", 12);

    VcView *v = vc_view_create(3, 4);
    vc_view_set_text(v, t);

    VcSpan s;
    CHECK(vc_view_selection_spans(v, 0, 1, 10, &s, 1) == 1, "sel vis0");
    CHECK(s.start_col == 1 && s.end_col == 3, "sel vis0 cols");

    CHECK(vc_view_selection_spans(v, 1, 1, 10, &s, 1) == 1, "sel vis1");
    CHECK(s.start_col == 0 && s.end_col == 3, "sel vis1 cols");

    CHECK(vc_view_selection_spans(v, 2, 1, 10, &s, 1) == 1, "sel vis2");
    CHECK(s.start_col == 3 && s.end_col == 5, "sel vis2 cols");

    CHECK(vc_view_selection_spans(v, 3, 1, 10, &s, 1) == 0, "sel vis3 empty");

    vc_view_destroy(v);
    vc_destroy(t);
}

static void test_atlas(void) {
    VcAtlas *a = vc_atlas_create(32, 32);
    CHECK(a != 0, "atlas create");

    uint32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    CHECK(vc_atlas_get(a, 1, 8, 8, &x1, &y1), "atlas get 1");
    CHECK(x1 == 0 && y1 == 0, "atlas first at origin");

    CHECK(vc_atlas_get(a, 1, 8, 8, &x2, &y2), "atlas get 1 again");
    CHECK(x2 == x1 && y2 == y1, "atlas same key same slot");

    CHECK(vc_atlas_get(a, 2, 8, 8, &x2, &y2), "atlas get 2");
    CHECK(x2 == 9 && y2 == 0, "atlas second x");

    CHECK(vc_atlas_get(a, 3, 8, 8, &x2, &y2), "atlas get 3");
    CHECK(x2 == 18 && y2 == 0, "atlas third x");

    CHECK(vc_atlas_get(a, 4, 8, 8, &x2, &y2), "atlas get 4");
    CHECK(x2 == 0 && y2 == 9, "atlas new shelf");

    /* 填满后必须自动清空并继续返回有效坐标 */
    int ok = 1;
    for (uint32_t k = 10; k < 30; k++) {
        uint32_t x = 0, y = 0;
        if (!vc_atlas_get(a, k, 8, 8, &x, &y)) { ok = 0; break; }
        if (x + 8 > 32 || y + 8 > 32) { ok = 0; break; }
    }
    CHECK(ok, "atlas eviction keeps coordinates valid");

    vc_atlas_destroy(a);
}

int main(void) {
    test_wrap();
    test_tabs();
    test_nowrap();
    test_scroll();
    test_selection();
    test_atlas();

    if (failures) {
        printf("VIEW CORE: %d failure(s)\n", failures);
        return 1;
    }
    printf("VIEW CORE: all tests passed\n");
    return 0;
}
