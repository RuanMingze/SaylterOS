/* ============================================================
 * session_test.c - VizCormin 文档/标签页测试（内存文件系统）
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vc_alloc.h"
#include "vc_session.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); failures++; } } while (0)

typedef struct {
    char path[64];
    char data[1024];
    uint32_t len;
    int used;
} MemFile;

typedef struct {
    MemFile files[8];
    int count;
} MemFs;

static MemFile *mem_find(MemFs *fs, const char *path) {
    for (int i = 0; i < fs->count; i++) {
        if (fs->files[i].used && strcmp(fs->files[i].path, path) == 0) {
            return &fs->files[i];
        }
    }
    return 0;
}

static int mem_read(void *user, const char *path, char **out, uint32_t *len) {
    MemFs *fs = (MemFs *)user;
    MemFile *f = mem_find(fs, path);
    if (!f) return 0;
    char *buf = (char *)vc_alloc(f->len ? f->len : 1);
    if (!buf) return 0;
    if (f->len) memcpy(buf, f->data, f->len);
    *out = buf;
    *len = f->len;
    return 1;
}

static int mem_write(void *user, const char *path, const char *data, uint32_t len) {
    MemFs *fs = (MemFs *)user;
    MemFile *f = mem_find(fs, path);
    if (!f) {
        if (fs->count >= 8) return 0;
        f = &fs->files[fs->count++];
        memset(f, 0, sizeof(*f));
        f->used = 1;
        strncpy(f->path, path, sizeof(f->path) - 1);
    }
    if (len >= sizeof(f->data)) return 0;
    memcpy(f->data, data, len);
    f->len = len;
    return 1;
}

static int mem_exists(void *user, const char *path) {
    return mem_find((MemFs *)user, path) != 0;
}

static void mem_put(MemFs *fs, const char *path, const char *data) {
    mem_write(fs, path, data, (uint32_t)strlen(data));
}

static void expect_doc(VcDocument *d, const char *want, int line) {
    char buf[2048];
    uint32_t n = vc_copy_all(d->text, buf, sizeof(buf));
    if (n != strlen(want) || memcmp(buf, want, n) != 0) {
        printf("FAIL line %d: doc text [%.*s] != [%s]\n", line, (int)n, buf, want);
        failures++;
    }
}

int main(void) {
    MemFs fs;
    memset(&fs, 0, sizeof(fs));
    mem_put(&fs, "a.txt", "hello");
    mem_put(&fs, "b.txt", "abc\ndef");

    VcFileOps ops;
    ops.user = &fs;
    ops.read = mem_read;
    ops.write = mem_write;
    ops.exists = mem_exists;

    VcSession *s = vc_session_create(&ops, 20, 5);
    CHECK(s != 0, "session create");
    CHECK(vc_session_count(s) == 1, "initial untitled tab");
    CHECK(vc_session_active(s)->path[0] == '\0', "untitled path");

    int idx = vc_session_open(s, "a.txt");
    CHECK(idx == 1, "open a.txt index");
    CHECK(vc_session_count(s) == 2, "open adds tab");
    VcDocument *d = vc_session_active(s);
    CHECK(d && strcmp(d->path, "a.txt") == 0, "active path");
    expect_doc(d, "hello", __LINE__);
    CHECK(d->dirty == 0, "opened not dirty");

    CHECK(vc_doc_insert(s, d, "X", 1), "insert X");
    expect_doc(d, "Xhello", __LINE__);
    CHECK(d->dirty == 1, "insert marks dirty");
    CHECK(d->cursor == 1, "cursor after insert");

    CHECK(vc_session_save(s, idx, 0), "save");
    CHECK(d->dirty == 0, "save clears dirty");
    MemFile *f = mem_find(&fs, "a.txt");
    CHECK(f && f->len == 6 && memcmp(f->data, "Xhello", 6) == 0, "file contents");

    CHECK(vc_doc_undo(d), "undo");
    expect_doc(d, "hello", __LINE__);
    CHECK(d->dirty == 1, "undo marks dirty");

    CHECK(vc_doc_redo(d), "redo");
    expect_doc(d, "Xhello", __LINE__);

    CHECK(vc_session_open(s, "a.txt") == idx, "reopen activates existing");
    CHECK(vc_session_count(s) == 2, "reopen does not duplicate");

    int nb = vc_session_open(s, "b.txt");
    CHECK(nb == 2, "open b.txt index");
    VcDocument *db = vc_session_active(s);
    expect_doc(db, "abc\ndef", __LINE__);

    vc_doc_move(db, 1, 1, 0);            /* 下一可视行 */
    CHECK(db->cursor == 4, "move down to line 2");
    vc_doc_move(db, 0, 1, 0);            /* 右移一字符 */
    CHECK(db->cursor == 5, "move right");
    vc_doc_move(db, 1, -1, 0);           /* 上一行，列 1 */
    CHECK(db->cursor == 1, "move up keeps column");

    vc_doc_move(db, 3, 1, 0);
    CHECK(db->cursor == vc_length(db->text), "move to end");
    vc_doc_select_all(db);
    CHECK(db->anchor == 0 && db->cursor == vc_length(db->text), "select all");

    /* 选区删除 */
    CHECK(vc_doc_erase_selection(s, db), "erase selection");
    CHECK(vc_length(db->text) == 0, "selection erased");
    CHECK(db->dirty == 1, "erase marks dirty");

    int nt = vc_session_new(s);
    CHECK(nt == 3, "new tab index");
    CHECK(vc_session_count(s) == 4, "new tab count");
    VcDocument *dn = vc_session_active(s);
    CHECK(vc_doc_insert(s, dn, "abc", 3), "type in new tab");
    CHECK(vc_session_save(s, nt, "c.txt"), "save as c.txt");
    CHECK(mem_find(&fs, "c.txt") != 0, "c.txt written");

    vc_session_close(s, nt);
    CHECK(vc_session_count(s) == 3, "close removes tab");
    CHECK(vc_session_active_index(s) < vc_session_count(s), "active clamped");

    vc_session_destroy(s);

    if (failures) {
        printf("SESSION CORE: %d failure(s)\n", failures);
        return 1;
    }
    printf("SESSION CORE: all tests passed\n");
    return 0;
}
