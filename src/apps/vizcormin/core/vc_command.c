/* ============================================================
 * vc_command.c - 命令与快捷键注册表实现
 * ============================================================ */

#include "vc_command.h"
#include "vc_alloc.h"
#include <string.h>

#define VC_CMD_MAX    64
#define VC_BIND_MAX   128
#define VC_ID_MAX     48

typedef struct {
    char        id[VC_ID_MAX];
    VcCommandFn fn;
    void       *user;
} VcCommand;

typedef struct {
    uint32_t mods;
    uint32_t key;
    char     id[VC_ID_MAX];
} VcBinding;

struct VcCommands {
    VcCommand cmds[VC_CMD_MAX];
    int       ncmd;
    VcBinding binds[VC_BIND_MAX];
    int       nbind;
};

static int id_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void id_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < VC_ID_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

VcCommands *vc_commands_create(void) {
    return (VcCommands *)vc_zalloc(sizeof(VcCommands));
}

void vc_commands_destroy(VcCommands *c) {
    vc_free(c);
}

int vc_commands_register(VcCommands *c, const char *id,
                         VcCommandFn fn, void *user) {
    if (!c || !id || !id[0] || !fn) return 0;
    for (int i = 0; i < c->ncmd; i++) {
        if (id_eq(c->cmds[i].id, id)) return 0;
    }
    if (c->ncmd >= VC_CMD_MAX) return 0;
    VcCommand *cmd = &c->cmds[c->ncmd++];
    id_copy(cmd->id, id);
    cmd->fn = fn;
    cmd->user = user;
    return 1;
}

int vc_commands_bind(VcCommands *c, uint32_t mods, uint32_t key,
                     const char *id) {
    if (!c || !id || !id[0]) return 0;

    int exists = 0;
    for (int i = 0; i < c->ncmd; i++) {
        if (id_eq(c->cmds[i].id, id)) { exists = 1; break; }
    }
    if (!exists) return 0;

    for (int i = 0; i < c->nbind; i++) {
        if (c->binds[i].mods == mods && c->binds[i].key == key) {
            id_copy(c->binds[i].id, id);
            return 1;
        }
    }
    if (c->nbind >= VC_BIND_MAX) return 0;
    VcBinding *b = &c->binds[c->nbind++];
    b->mods = mods;
    b->key = key;
    id_copy(b->id, id);
    return 1;
}

int vc_commands_dispatch(VcCommands *c, VcSession *s,
                         uint32_t mods, uint32_t key) {
    if (!c) return 0;
    for (int i = 0; i < c->nbind; i++) {
        if (c->binds[i].mods != mods || c->binds[i].key != key) continue;
        for (int j = 0; j < c->ncmd; j++) {
            if (!id_eq(c->cmds[j].id, c->binds[i].id)) continue;
            (void)c->cmds[j].fn(c->cmds[j].user, s);
            return 1;
        }
        return 0;
    }
    return 0;
}

int vc_commands_count(const VcCommands *c) {
    return c ? c->ncmd : 0;
}

const char *vc_commands_id_at(const VcCommands *c, int index) {
    if (!c || index < 0 || index >= c->ncmd) return 0;
    return c->cmds[index].id;
}
