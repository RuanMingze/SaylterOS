/* ============================================================
 * vc_command.h - 命令与快捷键注册表
 * ------------------------------------------------------------
 * 命令按字符串 id 注册，快捷键按 (修饰键, 键码) 绑定。
 * 键码为 ASCII（字母用小写），修饰键用 VC_MOD_*。
 * 同一组合键重复绑定时覆盖旧绑定。
 * ============================================================ */

#ifndef VC_COMMAND_H
#define VC_COMMAND_H

#include <stdint.h>
#include "vc_session.h"

#define VC_MOD_CTRL  1u
#define VC_MOD_SHIFT 2u
#define VC_MOD_ALT   4u

typedef struct VcCommands VcCommands;

typedef int (*VcCommandFn)(void *user, VcSession *s);

VcCommands *vc_commands_create(void);
void        vc_commands_destroy(VcCommands *c);

int         vc_commands_register(VcCommands *c, const char *id,
                                 VcCommandFn fn, void *user);
int         vc_commands_bind(VcCommands *c, uint32_t mods, uint32_t key,
                             const char *id);
int         vc_commands_dispatch(VcCommands *c, VcSession *s,
                                 uint32_t mods, uint32_t key);
int         vc_commands_count(const VcCommands *c);
const char *vc_commands_id_at(const VcCommands *c, int index);

#endif /* VC_COMMAND_H */
