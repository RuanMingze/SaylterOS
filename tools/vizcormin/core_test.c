/* ============================================================
 * core_test.c - VizCormin 文本核心宿主测试
 * ------------------------------------------------------------
 * 覆盖：Piece Table 插入/删除、跨片段复制、UTF-8 边界、
 * 行列映射、撤销/重做、多光标插入与选区删除、随机对照测试。
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vc_text.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

static uint32_t rng_state = 123456789u;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void expect_text(VcText *t, const char *want, int line) {
    char buf[65536];
    uint32_t n = vc_copy_all(t, buf, sizeof(buf));
    if (n != strlen(want) || memcmp(buf, want, n) != 0) {
        printf("FAIL line %d: text mismatch\n  got [%.*s]\n  want [%s]\n",
               line, (int)n, buf, want);
        failures++;
    }
}

static void test_basic(void) {
    VcText *t = vc_create();
    CHECK(t != 0, "create");
    CHECK(vc_load(t, "hello\nworld\n", 12), "load");
    CHECK(vc_length(t) == 12, "length");
    expect_text(t, "hello\nworld\n", __LINE__);
    CHECK(vc_line_count(t) == 3, "line count");
    CHECK(vc_line_start(t, 1) == 6, "line 1 start");
    CHECK(vc_line_start(t, 2) == 12, "line 2 start");
    CHECK(vc_line_of(t, 7) == 1, "line of 7");
    CHECK(vc_col_of(t, 8) == 2, "col of 8");

    CHECK(vc_insert(t, 5, ",", 1), "insert comma");
    expect_text(t, "hello,\nworld\n", __LINE__);

    CHECK(vc_erase(t, 0, 6), "erase hello,");
    expect_text(t, "\nworld\n", __LINE__);

    vc_destroy(t);
}

static void test_utf8(void) {
    const char *src = "你好\n世界\n";
    VcText *t = vc_create();
    vc_load(t, src, (uint32_t)strlen(src));
    CHECK(vc_char_size(t, 0) == 3, "utf8 first char");
    CHECK(vc_char_size(t, 3) == 3, "utf8 second char");
    CHECK(vc_line_count(t) == 3, "utf8 lines");
    CHECK(vc_col_of(t, 6) == 2, "utf8 col");
    CHECK(vc_insert(t, 6, "！", 3), "utf8 insert");
    expect_text(t, "你好！\n世界\n", __LINE__);
    vc_destroy(t);
}

static void test_undo(void) {
    VcText *t = vc_create();
    vc_load(t, "abc", 3);

    CHECK(vc_undo_push(t), "undo push 1");
    CHECK(vc_insert(t, 3, "def", 3), "insert def");
    expect_text(t, "abcdef", __LINE__);

    CHECK(vc_undo_push(t), "undo push 2");
    CHECK(vc_erase(t, 0, 3), "erase abc");
    expect_text(t, "def", __LINE__);

    CHECK(vc_undo(t), "undo 1");
    expect_text(t, "abcdef", __LINE__);
    CHECK(vc_undo(t), "undo 2");
    expect_text(t, "abc", __LINE__);
    CHECK(!vc_can_undo(t), "no more undo");

    CHECK(vc_redo(t), "redo 1");
    expect_text(t, "abcdef", __LINE__);
    CHECK(vc_redo(t), "redo 2");
    expect_text(t, "def", __LINE__);
    vc_destroy(t);
}

static void test_multi(void) {
    VcText *t = vc_create();
    vc_load(t, "aa\nbb\ncc\n", 9);

    VcCursor c[3];
    c[0].pos = 0;  c[0].anchor = 0;
    c[1].pos = 3;  c[1].anchor = 3;
    c[2].pos = 6;  c[2].anchor = 6;
    CHECK(vc_insert_multi(t, c, 3, "X", 1), "multi insert");
    expect_text(t, "Xaa\nXbb\nXcc\n", __LINE__);

    /* 选中三行首的 X 并删除 */
    for (int i = 0; i < 3; i++) {
        c[i].anchor = c[i].pos - 1;
    }
    CHECK(vc_erase_multi(t, c, 3), "multi erase");
    expect_text(t, "aa\nbb\ncc\n", __LINE__);
    vc_destroy(t);
}

static void test_fuzz(void) {
    VcText *t = vc_create();
    char ref[65536];
    uint32_t rlen = 0;

    for (int step = 0; step < 3000; step++) {
        uint32_t op = rnd() % 3u;
        if (op == 0 || rlen == 0) {
            uint32_t len = 1u + rnd() % 8u;
            uint32_t pos = rlen ? rnd() % (rlen + 1u) : 0;
            char tmp[8];
            for (uint32_t i = 0; i < len; i++) tmp[i] = (char)('a' + (rnd() % 26u));
            if (rlen + len >= sizeof(ref)) continue;
            memmove(ref + pos + len, ref + pos, rlen - pos);
            memcpy(ref + pos, tmp, len);
            rlen += len;
            if (!vc_insert(t, pos, tmp, len)) { CHECK(0, "fuzz insert"); break; }
        } else {
            uint32_t len = 1u + rnd() % 8u;
            uint32_t pos = rnd() % rlen;
            if (pos + len > rlen) len = rlen - pos;
            if (len == 0) continue;
            memmove(ref + pos, ref + pos + len, rlen - pos - len);
            rlen -= len;
            if (!vc_erase(t, pos, len)) { CHECK(0, "fuzz erase"); break; }
        }

        if (vc_length(t) != rlen) {
            printf("FAIL fuzz length at step %d: %u != %u\n", step, vc_length(t), rlen);
            failures++;
            break;
        }
        char got[65536];
        uint32_t n = vc_copy_all(t, got, sizeof(got));
        if (n != rlen || memcmp(got, ref, rlen) != 0) {
            printf("FAIL fuzz text at step %d (len %u/%u)\n", step, n, rlen);
            failures++;
            break;
        }
    }
    vc_destroy(t);
}

int main(int argc, char **argv) {
    const char *w = argc > 1 ? argv[1] : "all";
    if (!strcmp(w, "basic") || !strcmp(w, "all")) test_basic();
    if (!strcmp(w, "utf8") || !strcmp(w, "all")) test_utf8();
    if (!strcmp(w, "undo") || !strcmp(w, "all")) test_undo();
    if (!strcmp(w, "multi") || !strcmp(w, "all")) test_multi();
    if (!strcmp(w, "fuzz") || !strcmp(w, "all")) test_fuzz();

    if (failures) {
        printf("EDITOR CORE: %d failure(s)\n", failures);
        return 1;
    }
    printf("EDITOR CORE: all tests passed\n");
    return 0;
}
