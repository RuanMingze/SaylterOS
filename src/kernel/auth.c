/* ============================================================
 * auth.c - 用户账户与登录
 * ------------------------------------------------------------
 * 设计要点：
 *   1. 账户库是普通文件，放在 etc 下；shadow 用 0600 保护，
 *      普通用户即使知道路径也打不开（SlyzFS 在 open 时强制检查）。
 *   2. 口令不存明文，只存 SHA-256(salt + password)。
 *   3. root（uid 0）默认无密码；创建新用户后需用 passwd 设密码。
 *   4. 所有直接读写账户库的操作都在临时 root 身份下进行，
 *      相当于内核里的特权系统调用，完成后恢复原身份。
 * ============================================================ */

#include <stdint.h>
#include "auth.h"
#include "sha256.h"
#include "tty.h"
#include "keyboard.h"

/* ---- SlyzFS 接口 ---- */
extern void* slyzfs_open(const char* path, const char* mode);
extern int   slyzfs_read(void* fh, uint8_t* buf, int n);
extern int   slyzfs_write(void* fh, const uint8_t* buf, int n);
extern void  slyzfs_close(void* fh);
extern int   slyzfs_mkdir(const char* path);
extern int   slyzfs_chmod(const char* path, int mode);
extern int   slyzfs_chown(const char* path, int uid);
extern void  slyzfs_set_uid(int uid);
extern int   slyzfs_get_uid(void);

#define AUTH_PASSWD_PATH "etc\\passwd"
#define AUTH_SHADOW_PATH "etc\\shadow"
#define AUTH_FILE_MAX    2048
#define AUTH_PASSWORD_MAX 64

/* ---- 内部状态 ---- */
static struct AuthUser g_users[AUTH_MAX_USERS];
static int  g_user_count = 0;
static struct AuthUser g_current;
static int  g_logged_in = 0;

/* ============================================================
 * 基础工具（不依赖 libc）
 * ============================================================ */

static int auth_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int auth_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void auth_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    if (max <= 0) return;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void auth_memset(void *dst, int value, uint32_t count) {
    uint8_t *p = (uint8_t *)dst;
    while (count--) *p++ = (uint8_t)value;
}

static int auth_parse_int(const char *s, int *out) {
    int v = 0;
    if (!s || !*s) return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s != '\0') return 0;
    *out = v;
    return 1;
}

static void auth_pause(void) {
    for (volatile int i = 0; i < 20000; i++) {
        __asm__ volatile ("nop");
    }
}

static void auth_trim_line(char *line) {
    int len = auth_strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

/* ============================================================
 * 特权文件读写
 * ============================================================ */

static int auth_read_all(const char *path, char *buf, int max) {
    if (max <= 1) return -1;
    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        slyzfs_set_uid(saved);
        return -1;
    }
    int n = slyzfs_read(fh, (uint8_t *)buf, max - 1);
    slyzfs_close(fh);

    slyzfs_set_uid(saved);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

static int auth_write_all(const char *path, const char *data, int len) {
    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);

    void *fh = slyzfs_open(path, "w");
    if (!fh) {
        slyzfs_set_uid(saved);
        return 0;
    }

    int total = 0;
    while (total < len) {
        int n = slyzfs_write(fh, (const uint8_t *)data + total, len - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);

    slyzfs_set_uid(saved);
    return total == len;
}

static int auth_exists(const char *path) {
    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);
    void *fh = slyzfs_open(path, "r");
    if (fh) slyzfs_close(fh);
    slyzfs_set_uid(saved);
    return fh ? 1 : 0;
}

/* ============================================================
 * 账户库解析
 * ============================================================ */

static struct AuthUser *auth_find(const char *name) {
    for (int i = 0; i < g_user_count; i++) {
        if (auth_strcmp(g_users[i].name, name) == 0) return &g_users[i];
    }
    return 0;
}

/* passwd 行：name:uid:gid:home */
static int auth_parse_passwd_line(char *line, struct AuthUser *u) {
    char *p = line;
    char *name = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    *p++ = '\0';

    char *uid_s = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    *p++ = '\0';

    char *gid_s = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    *p++ = '\0';

    char *home = p;

    int uid = 0, gid = 0;
    if (!auth_parse_int(uid_s, &uid)) return 0;
    if (!auth_parse_int(gid_s, &gid)) return 0;
    if (uid < 0 || uid > 255 || gid < 0 || gid > 255) return 0;
    if (name[0] == '\0') return 0;

    auth_memset(u, 0, sizeof(*u));
    auth_strncpy(u->name, name, AUTH_NAME_MAX);
    u->uid = (uint8_t)uid;
    u->gid = (uint8_t)gid;
    auth_strncpy(u->home, home, sizeof(u->home));
    auth_trim_line(u->home);
    return 1;
}

/* shadow 行：name:salt:hash */
static int auth_parse_shadow_line(char *line, char *name_out, char *salt_out, char *hash_out) {
    char *p = line;
    char *name = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    *p++ = '\0';

    char *salt = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    *p++ = '\0';

    char *hash = p;

    auth_strncpy(name_out, name, AUTH_NAME_MAX);
    auth_trim_line(hash);
    auth_strncpy(salt_out, salt, 17);
    auth_strncpy(hash_out, hash, 65);
    return 1;
}

static int auth_load(void) {
    char buf[AUTH_FILE_MAX];
    g_user_count = 0;

    int n = auth_read_all(AUTH_PASSWD_PATH, buf, sizeof(buf));
    if (n <= 0) return 0;

    char *p = buf;
    while (*p && g_user_count < AUTH_MAX_USERS) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        auth_trim_line(line);
        if (line[0] == '\0') continue;

        struct AuthUser parsed;
        if (auth_parse_passwd_line(line, &parsed)) {
            g_users[g_user_count++] = parsed;
        }
    }

    /* 合并 shadow 中的 salt/hash */
    n = auth_read_all(AUTH_SHADOW_PATH, buf, sizeof(buf));
    if (n > 0) {
        p = buf;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') {
                *p = '\0';
                p++;
            }
            auth_trim_line(line);
            if (line[0] == '\0') continue;

            char name[AUTH_NAME_MAX];
            char salt[17];
            char hash[65];
            if (!auth_parse_shadow_line(line, name, salt, hash)) continue;

            struct AuthUser *u = auth_find(name);
            if (u) {
                auth_strncpy(u->salt, salt, sizeof(u->salt));
                auth_strncpy(u->hash, hash, sizeof(u->hash));
            }
        }
    }

    return g_user_count;
}

/* ============================================================
 * 口令摘要
 * ============================================================ */

static void auth_generate_salt(char out[17]) {
    static uint32_t counter = 0;
    uint32_t lo = 0, hi = 0;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));

    uint32_t seed = lo ^ (hi * 2654435761u) ^ ((++counter) * 2246822519u);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = hex[(seed >> 28) & 0x0F];
    }
    out[16] = '\0';
}

static void auth_hash_password(const char *salt, const char *password, char out[65]) {
    char material[17 + AUTH_PASSWORD_MAX + 1];
    int pos = 0;

    while (*salt && pos < 16) {
        material[pos++] = *salt++;
    }
    while (*password && pos < 16 + AUTH_PASSWORD_MAX) {
        material[pos++] = *password++;
    }
    material[pos] = '\0';

    sha256_hex((const uint8_t *)material, (uint32_t)pos, out);
}

int auth_verify(const char *name, const char *password) {
    if (!name || !*name) return -1;
    if (!auth_load()) return -1;

    struct AuthUser *u = auth_find(name);
    if (!u) return -1;

    /* 无密码账户：任何口令都通过（root 的默认状态） */
    if (u->hash[0] == '\0') return (int)u->uid;

    char computed[65];
    auth_hash_password(u->salt, password ? password : "", computed);
    return auth_strcmp(computed, u->hash) == 0 ? (int)u->uid : -1;
}

/* ============================================================
 * 初始化与交互
 * ============================================================ */

void auth_init(void) {
    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);

    int have_passwd = auth_exists(AUTH_PASSWD_PATH);
    int have_shadow = auth_exists(AUTH_SHADOW_PATH);

    if (!have_passwd || !have_shadow) {
        slyzfs_mkdir("etc");
        slyzfs_chmod("etc", 0755);

        if (!have_passwd) {
            const char *init = "root:0:0:\n";
            auth_write_all(AUTH_PASSWD_PATH, init, auth_strlen(init));
            slyzfs_chmod(AUTH_PASSWD_PATH, 0644);
        }
        if (!have_shadow) {
            const char *init = "root::\n";
            auth_write_all(AUTH_SHADOW_PATH, init, auth_strlen(init));
            slyzfs_chmod(AUTH_SHADOW_PATH, 0600);
        }
    }

    if (!auth_exists("home")) {
        slyzfs_mkdir("home");
        slyzfs_chmod("home", 0755);
    }
    if (!auth_exists("home\\root")) {
        slyzfs_mkdir("home\\root");
        slyzfs_chmod("home\\root", 0700);
    }

    slyzfs_set_uid(saved);
}

void auth_read_line(char *buf, int max) {
    int len = 0;
    if (max <= 0) return;

    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            /* 空闲等待时驱动光标闪烁 */
            tty_tick();
            auth_pause();
            continue;
        }
        if (c == '\r' || c == '\n') {
            tty_putchar('\n');
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                tty_putchar('\b');
            }
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x80) continue;
        if (len < max - 1) {
            buf[len++] = c;
            tty_putchar(c);
        }
    }
    buf[len] = '\0';
}

void auth_read_password(char *buf, int max) {
    int len = 0;
    if (max <= 0) return;

    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            /* 空闲等待时驱动光标闪烁 */
            tty_tick();
            auth_pause();
            continue;
        }
        if (c == '\r' || c == '\n') {
            tty_putchar('\n');
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                tty_putchar('\b');
                tty_putchar(' ');
                tty_putchar('\b');
            }
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x80) continue;
        if (len < max - 1) {
            buf[len++] = c;
            tty_putchar('*');
        }
    }
    buf[len] = '\0';
}

int auth_login(void) {
    auth_init();

    if (!auth_load()) {
        printk("auth: account database is missing or unreadable\n");
        return 0;
    }

    char name[AUTH_NAME_MAX];
    char password[AUTH_PASSWORD_MAX];

    while (1) {
        printk("SaylterOS login: ");
        auth_read_line(name, sizeof(name));
        if (name[0] == '\0') continue;

        struct AuthUser *u = auth_find(name);
        if (!u) {
            printk("Login incorrect\n");
            continue;
        }

        if (u->hash[0] != '\0') {
            printk("Password: ");
            auth_read_password(password, sizeof(password));
            if (auth_verify(name, password) < 0) {
                printk("Login incorrect\n");
                continue;
            }
        } else {
            /* 无密码账户（默认 root）：不要求输入口令 */
            printk("Password: (none)\n");
        }

        g_current = *u;
        g_logged_in = 1;
        slyzfs_set_uid(u->uid);
        printk("Welcome, %s (uid %u)\n", g_current.name, (unsigned int)g_current.uid);
        return 1;
    }
}

const struct AuthUser *auth_current(void) {
    return g_logged_in ? &g_current : 0;
}

int auth_switch_to(const char *name) {
    if (!name || !*name) return 0;
    if (!auth_load()) {
        printk("su: account database is unreadable\n");
        return 0;
    }

    struct AuthUser *u = auth_find(name);
    if (!u) {
        printk("su: unknown user '%s'\n", name);
        return 0;
    }

    /* root 切换身份无需口令；其他人切换到有口令账户时需要验证 */
    if (u->hash[0] != '\0' && !(g_logged_in && g_current.uid == 0)) {
        char password[AUTH_PASSWORD_MAX];
        printk("Password: ");
        auth_read_password(password, sizeof(password));
        if (auth_verify(name, password) < 0) {
            printk("su: authentication failed\n");
            return 0;
        }
    }

    g_current = *u;
    g_logged_in = 1;
    slyzfs_set_uid(u->uid);
    return 1;
}

void auth_logout(void) {
    auth_memset(&g_current, 0, sizeof(g_current));
    g_logged_in = 0;
    slyzfs_set_uid(0);
}

/* ============================================================
 * 账户管理命令
 * ============================================================ */

static int auth_valid_name(const char *name) {
    if (!name || !*name) return 0;
    int len = auth_strlen(name);
    if (len > AUTH_NAME_MAX - 1) return 0;
    if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) return 0;
    for (int i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return 1;
}

/* 追加一行到账户文件（读全文 → 追加 → 重写） */
static int auth_append_line(const char *path, const char *line) {
    char buf[AUTH_FILE_MAX];
    int n = auth_read_all(path, buf, sizeof(buf));
    if (n < 0) n = 0;

    int len = n;
    if (len > 0 && buf[len - 1] != '\n' && len < AUTH_FILE_MAX - 2) {
        buf[len++] = '\n';
    }
    const char *p = line;
    while (*p && len < AUTH_FILE_MAX - 2) buf[len++] = *p++;
    buf[len++] = '\n';
    return auth_write_all(path, buf, len);
}

/* 重写某用户在 shadow 中的行 */
static int auth_update_shadow(const char *name, const char *salt, const char *hash) {
    char buf[AUTH_FILE_MAX];
    int n = auth_read_all(AUTH_SHADOW_PATH, buf, sizeof(buf));
    if (n < 0) n = 0;

    char out[AUTH_FILE_MAX];
    int out_len = 0;
    int replaced = 0;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        auth_trim_line(line);
        if (line[0] == '\0') continue;

        char line_name[AUTH_NAME_MAX];
        char line_salt[17];
        char line_hash[65];
        if (!auth_parse_shadow_line(line, line_name, line_salt, line_hash)) continue;

        if (out_len + AUTH_NAME_MAX + 96 < AUTH_FILE_MAX) {
            int pos = out_len;
            const char *s = line_name;
            while (*s) out[pos++] = *s++;
            out[pos++] = ':';
            if (auth_strcmp(line_name, name) == 0) {
                s = salt;
                while (*s) out[pos++] = *s++;
                out[pos++] = ':';
                s = hash;
                while (*s) out[pos++] = *s++;
                replaced = 1;
            } else {
                s = line_salt;
                while (*s) out[pos++] = *s++;
                out[pos++] = ':';
                s = line_hash;
                while (*s) out[pos++] = *s++;
            }
            out[pos++] = '\n';
            out_len = pos;
        }
    }

    if (!replaced) {
        if (out_len + AUTH_NAME_MAX + 96 < AUTH_FILE_MAX) {
            int pos = out_len;
            const char *s = name;
            while (*s) out[pos++] = *s++;
            out[pos++] = ':';
            s = salt;
            while (*s) out[pos++] = *s++;
            out[pos++] = ':';
            s = hash;
            while (*s) out[pos++] = *s++;
            out[pos++] = '\n';
            out_len = pos;
        }
    }

    if (!auth_write_all(AUTH_SHADOW_PATH, out, out_len)) return 0;
    slyzfs_chmod(AUTH_SHADOW_PATH, 0600);
    return 1;
}

int auth_useradd(const char *name) {
    if (!g_logged_in || g_current.uid != 0) {
        printk("useradd: permission denied (root only)\n");
        return 0;
    }
    if (!auth_valid_name(name)) {
        printk("useradd: invalid name (letters, digits, underscore)\n");
        return 0;
    }

    auth_load();
    if (auth_find(name)) {
        printk("useradd: user '%s' already exists\n", name);
        return 0;
    }
    if (g_user_count >= AUTH_MAX_USERS) {
        printk("useradd: user table is full\n");
        return 0;
    }

    int uid = 1;
    for (int i = 0; i < g_user_count; i++) {
        if ((int)g_users[i].uid >= uid) uid = (int)g_users[i].uid + 1;
    }
    if (uid > 255) {
        printk("useradd: no free uid\n");
        return 0;
    }

    char home[40];
    int pos = 0;
    const char *prefix = "home\\";
    while (*prefix) home[pos++] = *prefix++;
    const char *n = name;
    while (*n && pos < (int)sizeof(home) - 1) home[pos++] = *n++;
    home[pos] = '\0';

    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);
    if (!auth_exists(home)) {
        slyzfs_mkdir(home);
    }
    slyzfs_chmod(home, 0700);
    slyzfs_chown(home, uid);
    slyzfs_set_uid(saved);

    char passwd_line[96];
    pos = 0;
    n = name;
    while (*n) passwd_line[pos++] = *n++;
    passwd_line[pos++] = ':';

    char num[4];
    int nlen = 0;
    if (uid == 0) {
        num[nlen++] = '0';
    } else {
        int v = uid;
        char tmp[4];
        int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) num[nlen++] = tmp[--t];
    }
    for (int i = 0; i < nlen; i++) passwd_line[pos++] = num[i];
    passwd_line[pos++] = ':';
    for (int i = 0; i < nlen; i++) passwd_line[pos++] = num[i];
    passwd_line[pos++] = ':';
    const char *h = home;
    while (*h) passwd_line[pos++] = *h++;
    passwd_line[pos] = '\0';

    if (!auth_append_line(AUTH_PASSWD_PATH, passwd_line)) {
        printk("useradd: cannot update passwd\n");
        return 0;
    }
    auth_append_line(AUTH_SHADOW_PATH, name);

    printk("useradd: created '%s' with uid %d, home %s\n", name, uid, home);
    printk("useradd: set a password with 'passwd %s'\n", name);
    return 1;
}

int auth_passwd(const char *name) {
    if (!g_logged_in) return 0;

    const char *target = (name && *name) ? name : g_current.name;
    int is_self = (auth_strcmp(target, g_current.name) == 0);
    if (!is_self && g_current.uid != 0) {
        printk("passwd: permission denied\n");
        return 0;
    }

    auth_load();
    struct AuthUser *u = auth_find(target);
    if (!u) {
        printk("passwd: unknown user '%s'\n", target);
        return 0;
    }

    if (is_self && g_current.uid != 0 && u->hash[0] != '\0') {
        char old_password[AUTH_PASSWORD_MAX];
        printk("Current password: ");
        auth_read_password(old_password, sizeof(old_password));
        if (auth_verify(target, old_password) < 0) {
            printk("passwd: incorrect password\n");
            return 0;
        }
    }

    char first[AUTH_PASSWORD_MAX];
    char second[AUTH_PASSWORD_MAX];

    printk("New password: ");
    auth_read_password(first, sizeof(first));
    if (first[0] == '\0') {
        printk("passwd: empty password is not allowed\n");
        return 0;
    }

    printk("Retype new password: ");
    auth_read_password(second, sizeof(second));
    if (auth_strcmp(first, second) != 0) {
        printk("passwd: passwords do not match\n");
        return 0;
    }

    char salt[17];
    char hash[65];
    auth_generate_salt(salt);
    auth_hash_password(salt, first, hash);

    if (!auth_update_shadow(u->name, salt, hash)) {
        printk("passwd: cannot update shadow\n");
        return 0;
    }

    printk("passwd: password updated for '%s'\n", u->name);
    return 1;
}
