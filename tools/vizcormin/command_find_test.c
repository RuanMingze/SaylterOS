/* ============================================================
 * command_find_test.c - 查找/替换与命令快捷键测试
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "vc_session.h"
#include "vc_find.h"
#include "vc_command.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); failures++; } } while (0)

static void expect_doc(VcDocument *d, const char *want, int line) {
    char buf[2048];
    uint32_t n = vc_copy_all(d->text, buf, sizeof(buf));
    if (n != strlen(want) || memcmp(buf, want, n) != 0) {
        printf("FAIL line %d: [%.*s] != [%s]\n", line, (int)n, buf, want);
        failures++;
    }
}

static void test_find(void) {
    const char *src = "foo bar Foo foo_bar foo";
    VcText *t = vc_create();
    vc_load(t, src, (uint32_t)strlen(src));

    VcRange r;
    VcFindOptions cs_word = { 1, 1 };
    VcFindOptions ci_word = { 0, 1 };

    CHECK(vc_find_next(t, "foo", 3, cs_word, 0, &r) && r.start == 0, "next first");
    CHECK(vc_find_next(t, "foo", 3, cs_word, 1, &r) && r.start == 20, "skip foo_bar");
    CHECK(vc_find_next(t, "foo", 3, ci_word, 1, &r) && r.start == 8, "case-insensitive Foo");
    CHECK(vc_find_prev(t, "foo", 3, ci_word, 22, &r) && r.start == 20, "prev");

    VcRange all[8];
    int n = vc_find_all(t, "foo", 3, cs_word, all, 8);
    CHECK(n == 2, "all case-sensitive whole word");
    CHECK(all[0].start == 0 && all[1].start == 20, "all positions");

    n = vc_find_all(t, "foo", 3, ci_word, all, 8);
    CHECK(n == 3, "all case-insensitive whole word");

    vc_destroy(t);
}

static void test_replace(void) {
    const char *src = "foo bar Foo foo_bar foo";
    VcSession *s = vc_session_create(0, 20, 5);
    VcDocument *d = vc_session_active(s);
    CHECK(vc_doc_insert(s, d, src, (uint32_t)strlen(src)), "load text");
    d->cursor = 0;
    d->anchor = 0;

    VcFindOptions cs_word = { 1, 1 };
    VcFindOptions ci_word = { 0, 1 };

    CHECK(vc_doc_replace_all(d, "foo", 3, cs_word, "X", 1) == 2, "replace all count");
    expect_doc(d, "X bar Foo foo_bar X", __LINE__);

    CHECK(vc_doc_undo(d), "undo replace");
    expect_doc(d, src, __LINE__);

    CHECK(vc_doc_replace_all(d, "foo", 3, ci_word, "Y", 1) == 3, "replace ci count");
    expect_doc(d, "Y bar Y foo_bar Y", __LINE__);

    d->cursor = 0;
    CHECK(vc_doc_replace_one(d, "bar", 3, cs_word, "BAR", 3, 0) == 1, "replace one");
    expect_doc(d, "Y BAR Y foo_bar Y", __LINE__);

    vc_session_destroy(s);
}

static int cmd_insert_x(void *user, VcSession *s) {
    (void)user;
    VcDocument *d = vc_session_active(s);
    return vc_doc_insert(s, d, "X", 1);
}

static int cmd_noop(void *user, VcSession *s) {
    (void)user;
    (void)s;
    return 1;
}

static void test_commands(void) {
    VcSession *s = vc_session_create(0, 20, 5);
    VcCommands *c = vc_commands_create();
    CHECK(s && c, "create");

    CHECK(vc_commands_register(c, "insert-x", cmd_insert_x, s), "register");
    CHECK(!vc_commands_register(c, "insert-x", cmd_insert_x, s), "duplicate rejected");
    CHECK(!vc_commands_register(c, "", cmd_insert_x, s), "empty id rejected");
    CHECK(vc_commands_register(c, "noop", cmd_noop, 0), "register noop");

    CHECK(!vc_commands_bind(c, VC_MOD_CTRL, 'q', "missing"), "bind unknown rejected");
    CHECK(vc_commands_bind(c, VC_MOD_CTRL, 'x', "insert-x"), "bind insert-x");

    CHECK(vc_commands_dispatch(c, s, VC_MOD_CTRL, 'x') == 1, "dispatch insert-x");
    expect_doc(vc_session_active(s), "X", __LINE__);

    CHECK(vc_commands_dispatch(c, s, VC_MOD_CTRL, 'q') == 0, "unbound returns 0");

    CHECK(vc_commands_bind(c, VC_MOD_CTRL, 'x', "noop"), "rebind");
    CHECK(vc_commands_dispatch(c, s, VC_MOD_CTRL, 'x') == 1, "dispatch noop");
    expect_doc(vc_session_active(s), "X", __LINE__);

    CHECK(vc_commands_count(c) == 2, "command count");
    CHECK(strcmp(vc_commands_id_at(c, 0), "insert-x") == 0, "command id at 0");

    vc_commands_destroy(c);
    vc_session_destroy(s);
}

int main(void) {
    test_find();
    test_replace();
    test_commands();

    if (failures) {
        printf("COMMAND/FIND CORE: %d failure(s)\n", failures);
        return 1;
    }
    printf("COMMAND/FIND CORE: all tests passed\n");
    return 0;
}
