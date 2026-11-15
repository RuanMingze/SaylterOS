/* ============================================================
 * auth.h - 用户账户与登录
 * ------------------------------------------------------------
 * 账户库保存在 SlyzFS 中：
 *   etc\passwd   name:uid:gid:home      （0644，所有人可读）
 *   etc\shadow   name:salt:hash         （0600，仅 root 可读）
 * 口令以 SHA-256(salt + password) 存储，salt 每次改密重新生成。
 * root 的 hash 为空，表示无密码账户（默认状态）。
 * ============================================================ */

#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>

#define AUTH_NAME_MAX  24
#define AUTH_MAX_USERS 16

struct AuthUser {
    char    name[AUTH_NAME_MAX];
    uint8_t uid;
    uint8_t gid;
    char    home[40];
    char    salt[17];   /* 16 个 hex 字符 + 结尾 */
    char    hash[65];   /* SHA-256 十六进制；空串表示无密码 */
};

/* 确保 etc/passwd、etc/shadow 与 root 账户存在（首次启动时创建） */
void auth_init(void);

/* 交互式登录循环，成功后把身份写入 FS 层并返回 1 */
int auth_login(void);

/* 当前登录用户；未登录返回 0 */
const struct AuthUser *auth_current(void);

/* 命令实现：创建用户 / 修改口令 */
int auth_useradd(const char *name);
int auth_passwd(const char *name);

/* su：验证目标用户口令并切换身份（root 切换无需口令） */
int auth_switch_to(const char *name);

/* 清除当前身份，回到 uid 0（root 上下文），用于 logout */
void auth_logout(void);

/* 校验用户名与口令，成功返回 uid，失败返回 -1 */
int auth_verify(const char *name, const char *password);

/* 读取一行（回显）/ 读取口令（回显 *） */
void auth_read_line(char *buf, int max);
void auth_read_password(char *buf, int max);

#endif /* AUTH_H */
