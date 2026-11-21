/* ============================================================
 * vc_fs_api.c - 独立模块的 SlyzFS 文件接口
 * ------------------------------------------------------------
 * 只通过 SlysKernelApi 访问文件系统，不引用内核符号。
 * 读取整份进内存（vc_alloc），写入整份覆盖。
 * ============================================================ */

#include "vc_fs_api.h"
#include "../core/vc_alloc.h"

#pragma pack(push, 1)
struct VcApiDirEntry {
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

static const struct SlysKernelApi *g_api = 0;

void vc_fs_api_bind(const struct SlysKernelApi *api) {
    g_api = api;
}

static int api_read(void *user, const char *path, char **out, uint32_t *len) {
    (void)user;
    if (!g_api || !g_api->fs_open || !g_api->fs_stat) return 0;

    struct VcApiDirEntry ent;
    for (uint32_t i = 0; i < sizeof(ent); i++) ((uint8_t *)&ent)[i] = 0;
    if (!g_api->fs_stat(path, &ent)) return 0;
    if (ent.size > 4u * 1024u * 1024u) return 0;

    void *fh = g_api->fs_open(path, "r");
    if (!fh) return 0;

    char *buf = (char *)vc_alloc(ent.size ? ent.size : 1);
    if (!buf) {
        g_api->fs_close(fh);
        return 0;
    }

    uint32_t total = 0;
    while (total < ent.size) {
        int got = g_api->fs_read(fh, (uint8_t *)buf + total, (int)(ent.size - total));
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    g_api->fs_close(fh);

    if (total != ent.size) {
        vc_free(buf);
        return 0;
    }

    *out = buf;
    *len = ent.size;
    return 1;
}

static int api_write(void *user, const char *path, const char *data, uint32_t len) {
    (void)user;
    if (!g_api || !g_api->fs_open || !g_api->fs_write) return 0;

    void *fh = g_api->fs_open(path, "w");
    if (!fh) return 0;

    uint32_t total = 0;
    int ok = 1;
    while (total < len) {
        int put = g_api->fs_write(fh, (const uint8_t *)data + total, (int)(len - total));
        if (put <= 0) { ok = 0; break; }
        total += (uint32_t)put;
    }
    g_api->fs_close(fh);
    return ok && total == len;
}

static int api_exists(void *user, const char *path) {
    (void)user;
    if (!g_api || !g_api->fs_stat) return 0;
    struct VcApiDirEntry ent;
    for (uint32_t i = 0; i < sizeof(ent); i++) ((uint8_t *)&ent)[i] = 0;
    return g_api->fs_stat(path, &ent) ? 1 : 0;
}

VcFileOps vc_fs_api(void) {
    VcFileOps ops;
    ops.user = 0;
    ops.read = api_read;
    ops.write = api_write;
    ops.exists = api_exists;
    return ops;
}
