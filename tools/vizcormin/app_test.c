/* ============================================================
 * app_test.c - VizCormin 应用外壳测试
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vc_alloc.h"
#include "vc_app.h"

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
        if (fs->files[i].used && strcmp(fs->files[i].path, path) == 0) return &fs->files[i];
    }
    return 0;
}

static int mem_read(void *user, const char *path, char **out, uint32_t *len) {
    MemFile *f = mem_find((MemFs *)user, path);
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

typedef struct {
    int glyphs;
    uint32_t cps[256];
    int ncp;
} Recorder;

static void rec_fill(void *user, int x, int y, int w, int h, uint32_t color) {
    (void)user; (void)x; (void)y; (void)w; (void)h; (void)color;
}

static void rec_glyph(void *user, int x, int y, uint32_t cp, uint32_t color) {
    (void)x; (void)y; (void)color;
    Recorder *r = (Recorder *)user;
    r->glyphs++;
    if (r->ncp < 256) r->cps[r->ncp++] = cp;
}

static int has_cp(const Recorder *r, uint32_t cp) {
    for (int i = 0; i < r->ncp; i++) if (r->cps[i] == cp) return 1;
    return 0;
}

int main(void) {
    MemFs fs;
    memset(&fs, 0, sizeof(fs));
    mem_write(&fs, "a.txt", "hello\nworld", 11);

    VcFileOps ops;
    ops.user = &fs;
    ops.read = mem_read;
    ops.write = mem_write;
    ops.exists = mem_exists;

    VcApp *a = vc_app_create(&ops, 320, 160, 8, 16);
    CHECK(a != 0, "app create");

    CHECK(vc_app_open(a, "a.txt") == 1, "open a.txt");
    VcSession *s = vc_app_session(a);
    VcDocument *d = vc_session_active(s);
    CHECK(d && strcmp(d->path, "a.txt") == 0, "active doc");

    Recorder rec;
    memset(&rec, 0, sizeof(rec));
    VcSurface surf;
    surf.width = 320;
    surf.height = 160;
    surf.user = &rec;
    surf.fill_rect = rec_fill;
    surf.draw_glyph = rec_glyph;

    VcRenderStats st = vc_app_render(a, &surf);
    CHECK(st.glyphs > 0 && rec.glyphs > 0, "render glyphs");
    CHECK(has_cp(&rec, 'h') && has_cp(&rec, 'w'), "document glyphs");
    CHECK(has_cp(&rec, 'a'), "tab title glyph");

    vc_app_text(a, 'X');
    char buf[64];
    uint32_t n = vc_copy_all(d->text, buf, sizeof(buf));
    CHECK(n == 12 && memcmp(buf, "Xhello\nworld", 12) == 0, "typed text");
    CHECK(d->cursor == 1, "cursor after typing");

    vc_app_key(a, 's', VC_MOD_CTRL);
    MemFile *f = mem_find(&fs, "a.txt");
    CHECK(f && f->len == 12 && memcmp(f->data, "Xhello\nworld", 12) == 0, "ctrl-s saved");
    CHECK(d->dirty == 0, "save clears dirty");

    vc_app_key(a, 'f', VC_MOD_CTRL);
    CHECK(vc_app_find_active(a) == 1, "find bar open");
    const char *q = "world";
    for (int i = 0; q[i]; i++) vc_app_text(a, (uint32_t)(uint8_t)q[i]);
    vc_app_key(a, 13, 0);
    CHECK(d->anchor == 7 && d->cursor == 12, "find selects world");

    vc_app_key(a, VC_KEY_ESC, 0);
    CHECK(vc_app_find_active(a) == 0, "find bar closed");

    vc_app_render(a, &surf);

    /* 点击编辑区第二行 */
    vc_app_mouse(a, 50, 40, 1);
    CHECK(vc_line_of(d->text, d->cursor) == 1, "mouse click line 2");

    vc_app_key(a, 'n', VC_MOD_CTRL);
    CHECK(vc_session_count(s) == 3, "ctrl-n adds tab");
    CHECK(vc_session_active_index(s) == 2, "new tab active");

    /* 点击第一个标签 */
    vc_app_mouse(a, 10, 5, 1);
    CHECK(vc_session_active_index(s) == 0, "tab click activates");

    vc_app_render(a, &surf);

    vc_app_destroy(a);

    if (failures) {
        printf("APP CORE: %d failure(s)\n", failures);
        return 1;
    }
    printf("APP CORE: all tests passed\n");
    return 0;
}
