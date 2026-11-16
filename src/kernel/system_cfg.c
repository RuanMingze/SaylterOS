/* ============================================================
 * system_cfg.c - 全局系统配置实现
 * ------------------------------------------------------------
 * 配置文件是纯文本，示例：
 *
 *   # SaylterOS system configuration
 *   # boot.mode = tty : text shell at startup
 *   # boot.mode = gui : graphical login and desktop
 *   boot.mode=gui
 *   timezone=8
 *
 * 读取发生在 shell_main 挂载文件系统之后、进入任何登录界面之前，
 * 因此 GUI 模式可以完全跳过 TTY banner 与 TTY 登录。
 *
 * 写回策略：配置文件里除 boot.mode / mode / timezone / tz 之外的行
 * （包括用户自己添加的注释和未知键）全部原样保留，只重建这两行，
 * 这样 `nano etc\system.cfg` 手工加的内容不会被设置页覆盖掉。
 * ============================================================ */

#include <stdint.h>
#include "system_cfg.h"

/* ---- SlyzFS 接口（与 shell.c / auth.c 使用同一组 C 入口） ---- */
extern int   slyzfs_mkdir(const char *path);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);
extern void  slyzfs_set_uid(int uid);
extern int   slyzfs_get_uid(void);

#define CFG_MAX_BYTES 2048
#define CFG_TZ_MIN    (-12)
#define CFG_TZ_MAX    14

static int g_boot_gui = 0;
static int g_timezone = 8;
static int g_loaded = 0;

/* 最近一次从磁盘读到的原始内容。写回时以它为底稿过滤托管行。 */
static char g_raw[CFG_MAX_BYTES];
static int  g_raw_len = 0;

static int cfg_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int cfg_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int cfg_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int cfg_read_all(char *buf, int cap) {
    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);

    void *fh = slyzfs_open(SYSTEM_CFG_PATH, "r");
    if (!fh) {
        slyzfs_set_uid(saved);
        return -1;
    }

    int total = 0;
    while (total < cap - 1) {
        int n = slyzfs_read(fh, (uint8_t *)buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);
    slyzfs_set_uid(saved);

    buf[total] = '\0';
    return total;
}

static int cfg_write_all(const char *data, int len) {
    int saved = slyzfs_get_uid();
    slyzfs_set_uid(0);

    void *fh = slyzfs_open(SYSTEM_CFG_PATH, "w");
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

/* 十进制解析，允许前导 +/-。成功返回 1。 */
static int cfg_parse_int(const char *s, int *out) {
    int sign = 1;
    int i = 0;
    int v = 0;
    int any = 0;

    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '-') {
        sign = -1;
        i++;
    } else if (s[i] == '+') {
        i++;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
        any = 1;
    }
    if (!any) return 0;
    *out = sign * v;
    return 1;
}

/* 解析 key=value。未知键忽略；timezone 支持带符号整数。 */
static void cfg_parse(const char *buf, int len) {
    int i = 0;

    while (i < len) {
        while (i < len && cfg_is_space(buf[i])) i++;
        if (i >= len) break;

        if (buf[i] == '#') {
            while (i < len && buf[i] != '\n') i++;
            continue;
        }

        char key[32];
        int kn = 0;
        while (i < len && buf[i] != '=' && buf[i] != '\n' && kn < (int)sizeof(key) - 1) {
            if (!cfg_is_space(buf[i])) key[kn++] = buf[i];
            i++;
        }
        key[kn] = '\0';

        if (i >= len || buf[i] != '=') {
            /* 不是 key=value 行：整行跳过。必须先吃掉的换行，
             * 否则外层循环会原地打转。 */
            while (i < len && buf[i] != '\n') i++;
            if (i < len) i++;
            continue;
        }
        i++;   /* 跳过 '=' */

        while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;

        char value[32];
        int vn = 0;
        while (i < len && buf[i] != '\n' && buf[i] != '\r' && vn < (int)sizeof(value) - 1) {
            value[vn++] = buf[i];
            i++;
        }
        value[vn] = '\0';

        /* 去掉值尾部的空白 */
        while (vn > 0 && cfg_is_space(value[vn - 1])) {
            value[--vn] = '\0';
        }

        if (cfg_strcmp(key, "boot.mode") == 0 || cfg_strcmp(key, "mode") == 0) {
            if (cfg_strcmp(value, "gui") == 0 ||
                cfg_strcmp(value, "graphical") == 0) {
                g_boot_gui = 1;
            } else {
                g_boot_gui = 0;
            }
        } else if (cfg_strcmp(key, "timezone") == 0 || cfg_strcmp(key, "tz") == 0) {
            int tz = 0;
            if (cfg_parse_int(value, &tz)) {
                if (tz < CFG_TZ_MIN) tz = CFG_TZ_MIN;
                if (tz > CFG_TZ_MAX) tz = CFG_TZ_MAX;
                g_timezone = tz;
            }
        }
    }
}

/* 判断一行是否是托管键（key 允许前导空白，'#' 开头一律不算）。 */
static int cfg_line_owns_key(const char *line, int len, const char *key) {
    int i = 0;
    int kl = cfg_strlen(key);

    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] == '#') return 0;
    if (i + kl > len) return 0;

    for (int k = 0; k < kl; k++) {
        if (line[i + k] != key[k]) return 0;
    }
    i += kl;
    if (i < len && line[i] != '=') return 0;
    return 1;
}

static int cfg_buf_append(char *out, int cap, int *o, const char *s) {
    while (*s) {
        if (*o >= cap - 1) return 0;
        out[(*o)++] = *s++;
    }
    return 1;
}

static int cfg_buf_append_line(char *out, int cap, int *o, const char *s) {
    if (!cfg_buf_append(out, cap, o, s)) return 0;
    if (*o >= cap - 1) return 0;
    out[(*o)++] = '\n';
    return 1;
}

static int cfg_buf_append_int(char *out, int cap, int *o, int v) {
    char tmp[16];
    int n = 0;

    if (v < 0) {
        if (*o >= cap - 1) return 0;
        out[(*o)++] = '-';
        v = -v;
    }
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (n > 0) {
        if (*o >= cap - 1) return 0;
        out[(*o)++] = tmp[--n];
    }
    return 1;
}

/* 用原始内容 + 新的 boot.mode / timezone 组装配置文件并落盘。 */
static int cfg_save_state(int gui, int tz) {
    char out[CFG_MAX_BYTES];
    int o = 0;

    if (g_raw_len <= 0) {
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o,
                "# SaylterOS system configuration")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o, "#")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o,
                "# boot.mode = tty : text shell at startup")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o,
                "# boot.mode = gui : graphical login and desktop")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o, "#")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o,
                "# timezone = UTC offset in hours, -12 .. 14")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o, "#")) return 0;
        if (!cfg_buf_append_line(out, (int)sizeof(out), &o,
                "# Edit with: nano etc\\system.cfg   then reboot.")) return 0;
    } else {
        int i = 0;
        while (i < g_raw_len) {
            int start = i;
            while (i < g_raw_len && g_raw[i] != '\n') i++;
            int end = i;
            if (i < g_raw_len) i++;

            if (cfg_line_owns_key(g_raw + start, end - start, "boot.mode") ||
                cfg_line_owns_key(g_raw + start, end - start, "mode") ||
                cfg_line_owns_key(g_raw + start, end - start, "timezone") ||
                cfg_line_owns_key(g_raw + start, end - start, "tz")) {
                continue;
            }

            if (o + (end - start) + 2 >= (int)sizeof(out)) return 0;
            for (int k = start; k < end; k++) out[o++] = g_raw[k];
            out[o++] = '\n';
        }
    }

    if (!cfg_buf_append_line(out, (int)sizeof(out), &o,
            gui ? "boot.mode=gui" : "boot.mode=tty")) return 0;
    if (!cfg_buf_append_line(out, (int)sizeof(out), &o, "timezone=")) return 0;
    if (!cfg_buf_append_int(out, (int)sizeof(out), &o, tz)) return 0;
    /* append_int 在 timezone= 之后追加数字，还需要换行。 */
    if (o >= (int)sizeof(out) - 1) return 0;
    out[o++] = '\n';

    if (!cfg_write_all(out, o)) return 0;

    g_raw_len = o;
    if (g_raw_len > (int)sizeof(g_raw)) g_raw_len = (int)sizeof(g_raw);
    for (int k = 0; k < g_raw_len; k++) g_raw[k] = out[k];

    g_boot_gui = gui ? 1 : 0;
    g_timezone = tz;
    return 1;
}

void system_cfg_init(void) {
    if (g_loaded) return;

    /* etc 目录可能还不存在（首次格式化后的第一次启动）。
     * mkdir 对已存在目录会返回失败，这里不需要区分。 */
    (void)slyzfs_mkdir("etc");

    char buf[CFG_MAX_BYTES];
    int n = cfg_read_all(buf, (int)sizeof(buf));

    if (n < 0) {
        g_raw_len = 0;
        if (!cfg_save_state(0, 8)) {
            /* 文件系统不可写（未挂载/只读）时保持 TTY 默认模式 */
            g_boot_gui = 0;
            g_timezone = 8;
        }
        g_loaded = 1;
        return;
    }

    g_raw_len = n;
    if (g_raw_len > (int)sizeof(g_raw)) g_raw_len = (int)sizeof(g_raw);
    for (int i = 0; i < g_raw_len; i++) g_raw[i] = buf[i];

    cfg_parse(buf, n);
    g_loaded = 1;
}

int system_cfg_boot_gui(void) {
    return g_boot_gui;
}

int system_cfg_set_boot_gui(int gui) {
    return cfg_save_state(gui ? 1 : 0, g_timezone);
}

int system_cfg_timezone(void) {
    return g_timezone;
}

int system_cfg_set_timezone(int hours) {
    if (hours < CFG_TZ_MIN) hours = CFG_TZ_MIN;
    if (hours > CFG_TZ_MAX) hours = CFG_TZ_MAX;
    return cfg_save_state(g_boot_gui, hours);
}
