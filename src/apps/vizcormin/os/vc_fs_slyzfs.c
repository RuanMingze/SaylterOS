/* ============================================================
 * vc_fs_slyzfs.c - SlyzFS 版 VcFileOps 实现
 * ============================================================ */

#include "vc_fs_slyzfs.h"
#include "../core/vc_alloc.h"

extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);
extern int   slyzfs_stat(const char *path, void *out);

#pragma pack(push, 1)
struct VcSlyzDirEntry {
    uint8_t  type;
    uint8_t  flags;
    uint16_t name_len;
    char     name[256];
    uint32_t size;
    uint32_t start_lba;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)

static int fs_slyzfs_read(void *user, const char *path, char **out, uint32_t *len) {
    (void)user;
    if (!path || !out || !len) return 0;

    struct VcSlyzDirEntry ent;
    for (uint32_t i = 0; i < sizeof(ent); i++) ((uint8_t *)&ent)[i] = 0;
    if (!slyzfs_stat(path, &ent)) return 0;
    /* 内核堆只有 6MB，编辑器当前整份读入内存；先限制 4MB。 */
    if (ent.size > 4u * 1024u * 1024u) return 0;

    void *fh = slyzfs_open(path, "r");
    if (!fh) return 0;

    char *buf = (char *)vc_alloc(ent.size ? ent.size : 1);
    if (!buf) {
        slyzfs_close(fh);
        return 0;
    }

    uint32_t total = 0;
    while (total < ent.size) {
        int got = slyzfs_read(fh, (uint8_t *)buf + total, (int)(ent.size - total));
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    slyzfs_close(fh);

    if (total != ent.size) {
        vc_free(buf);
        return 0;
    }

    *out = buf;
    *len = ent.size;
    return 1;
}

static int fs_slyzfs_write(void *user, const char *path, const char *data, uint32_t len) {
    (void)user;
    if (!path || (!data && len)) return 0;

    void *fh = slyzfs_open(path, "w");
    if (!fh) return 0;

    uint32_t total = 0;
    int ok = 1;
    while (total < len) {
        int put = slyzfs_write(fh, (const uint8_t *)data + total, (int)(len - total));
        if (put <= 0) { ok = 0; break; }
        total += (uint32_t)put;
    }
    slyzfs_close(fh);
    return ok && total == len;
}

static int fs_slyzfs_exists(void *user, const char *path) {
    (void)user;
    if (!path) return 0;
    struct VcSlyzDirEntry ent;
    for (uint32_t i = 0; i < sizeof(ent); i++) ((uint8_t *)&ent)[i] = 0;
    return slyzfs_stat(path, &ent) ? 1 : 0;
}

VcFileOps vc_fs_slyzfs(void) {
    VcFileOps ops;
    ops.user = 0;
    ops.read = fs_slyzfs_read;
    ops.write = fs_slyzfs_write;
    ops.exists = fs_slyzfs_exists;
    return ops;
}
