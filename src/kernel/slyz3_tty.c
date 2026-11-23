/* ============================================================
 * slyz3_tty.c - SLYZ v3 程序的终端宿主
 * ------------------------------------------------------------
 * v3 程序默认是声明式 GUI 应用，但文件系统工具、批处理脚本这类
 * 命令行程序不需要窗口。这里提供一套只依赖 TTY 的宿主实现：
 *
 *   print/println/eprint  -> printk
 *   input(prompt)         -> 可编辑整行输入
 *   fs.*                  -> SlyzFS v2（支持 fs.rmdir）
 *   cfg.* / time.* / os.* -> 与图形宿主相同的语义
 *   fault.*               -> faultlab
 *   win/gfx/ui/event/img  -> 无窗口，返回空值或 -1
 *
 * 只处理 v3 镜像；v2 仍走 shell.c 里原有的 slyz_host_sys。
 * ============================================================ */

#include <stdint.h>

#include "slyz3.h"
#include "slyzvm.h"
#include "tty.h"
#include "keyboard.h"
#include "io.h"
#include "mem.h"
#include "task.h"
#include "net.h"
#include "system_cfg.h"
#include "faultlab.h"
#include "audio.h"
#include "slyzfs_part.h"

#define TTY3_PATH_MAX   512
#define TTY3_READ_MAX   (1024 * 1024)

/* 目录项镜像结构：必须与 SlyzFS 桥接层的 280 字节布局一致。 */
struct Tty3DirEntry {
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
} __attribute__((packed));

#define TTY3_DIR_FILE 0x01
#define TTY3_DIR_DIR  0x02

extern int  slyzfs_stat(const char *path, struct Tty3DirEntry *out);
extern int  slyzfs_list_dir(const char *path, struct Tty3DirEntry *out, int out_max);
extern int  slyzfs_mkdir(const char *path);
extern int  slyzfs_rmdir(const char *path);
extern int  slyzfs_remove(const char *path);
extern int  slyzfs_copy_file(const char *src, const char *dst);
extern int  slyzfs_move_file(const char *src, const char *dst);
extern void *slyzfs_open(const char *path, const char *mode);
extern int  slyzfs_read(void *fh, uint8_t *buf, int n);
extern int  slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void slyzfs_close(void *fh);

/* ============================================================
 * 小工具
 * ============================================================ */

static int t3_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int t3_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void t3_itoa(int64_t v, char *out, int cap) {
    char tmp[24];
    int n = 0;
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;

    if (u == 0) tmp[n++] = '0';
    while (u > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    }
    int p = 0;
    if (neg && p < cap - 1) out[p++] = '-';
    while (n > 0 && p < cap - 1) out[p++] = tmp[--n];
    out[p] = '\0';
}

static const char *arg_str(struct SlyzVal *args, uint16_t argc, int i, int *len) {
    if (!args || i < 0 || i >= argc) {
        if (len) *len = 0;
        return 0;
    }
    return slyz_val_str(&args[i], len);
}

static int64_t arg_int(struct SlyzVal *args, uint16_t argc, int i) {
    if (!args || i < 0 || i >= argc) return 0;
    return slyz_val_int(&args[i]);
}

/* 把值渲染成文本后写到终端；字符串直接原样输出，避免引号。 */
static void print_value(struct Slyz3Vm *vm, struct SlyzVal *v) {
    (void)vm;
    if (!v) return;

    int len = 0;
    const char *s = slyz_val_str(v, &len);
    if (s) {
        for (int i = 0; i < len; i++) printk("%c", s[i]);
        return;
    }

    if (v->type == SLYZ_T_INT) {
        char buf[24];
        t3_itoa(v->u.i, buf, (int)sizeof(buf));
        for (int i = 0; buf[i]; i++) printk("%c", buf[i]);
        return;
    }
    if (v->type == SLYZ_T_NIL) {
        printk("nil");
        return;
    }
    if (v->type == SLYZ_T_ARR) {
        printk("[array]");
        return;
    }
    printk("?");
}

/* ============================================================
 * 行输入
 * ============================================================ */

static void t3_console_read_line(const char *prompt, char *out, int cap) {
    int len = 0;
    if (!out || cap <= 0) return;
    out[0] = '\0';
    if (prompt && *prompt) printk("%s", prompt);

    /* 任务第一次恢复时 IF=0，键盘 IRQ 不会进入；让出一次等 sti。 */
    task_yield();

    for (;;) {
        char c = kb_read_char();
        if (c == 0) {
            tty_tick();
            for (volatile int spin = 0; spin < 20000; spin++) {
                __asm__ volatile ("nop");
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            printk("\n");
            break;
        }
        if (c == 3) {                 /* Ctrl+C */
            printk("^C\n");
            len = 0;
            break;
        }
        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                printk("\b \b");
            }
            continue;
        }
        if ((unsigned char)c >= 32 && (unsigned char)c < 127 && len < cap - 1) {
            out[len++] = c;
            printk("%c", c);
        }
    }

    out[len] = '\0';
}

/* ============================================================
 * 文件与配置
 * ============================================================ */

static int t3_fs_read(struct Slyz3Vm *vm, const char *path, struct SlyzVal *ret) {
    struct Tty3DirEntry ent;
    if (!path || !*path || !slyzfs_stat(path, &ent)) {
        slyz_ret_nil(ret);
        return 0;
    }
    if (ent.type == TTY3_DIR_DIR || ent.size > TTY3_READ_MAX) {
        slyz_ret_nil(ret);
        return 0;
    }

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        slyz_ret_nil(ret);
        return 0;
    }

    uint8_t *buf = (uint8_t *)kmalloc(ent.size + 1);
    if (!buf) {
        slyzfs_close(fh);
        slyz_ret_nil(ret);
        return 0;
    }

    int total = 0;
    while (total < (int)ent.size) {
        int n = slyzfs_read(fh, buf + total, (int)ent.size - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);
    buf[total] = 0;

    int ok = slyz3_ret_str(vm, ret, (const char *)buf, total);
    kfree(buf);
    return ok ? 0 : -1;
}

static int t3_fs_write(const char *path, const char *text, int len) {
    if (!path || !*path) return -1;
    void *fh = slyzfs_open(path, "w");
    if (!fh) return -1;
    int total = 0;
    while (total < len) {
        int n = slyzfs_write(fh, (const uint8_t *)text + total, len - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);
    return total;
}

static int t3_fs_list(struct Slyz3Vm *vm, const char *path, struct SlyzVal *ret) {
    static struct Tty3DirEntry entries[64];
    int count = slyzfs_list_dir(path, entries, 64);
    if (count < 0) count = 0;
    if (count > 64) count = 64;

    if (!slyz3_ret_arr(vm, ret, count)) return -1;
    for (int i = 0; i < count; i++) {
        struct SlyzVal item;
        if (!slyz3_ret_str(vm, &item, entries[i].name, t3_strlen(entries[i].name))) return -1;
        slyz3_arr_set(vm, ret, i, item);
    }
    return 0;
}

static int t3_fs_entries(struct Slyz3Vm *vm, const char *path, struct SlyzVal *ret) {
    static struct Tty3DirEntry entries[64];
    int count = slyzfs_list_dir(path, entries, 64);
    if (count < 0) count = 0;
    if (count > 64) count = 64;

    if (!slyz3_ret_arr(vm, ret, count)) return -1;
    for (int i = 0; i < count; i++) {
        struct SlyzVal row;
        if (!slyz3_ret_arr(vm, &row, 3)) return -1;

        struct SlyzVal name;
        if (!slyz3_ret_str(vm, &name, entries[i].name, t3_strlen(entries[i].name))) return -1;
        slyz3_arr_set(vm, &row, 0, name);

        struct SlyzVal type;
        type.type = SLYZ_T_INT;
        type.aux = 0;
        type.u.i = (entries[i].type == TTY3_DIR_DIR) ? 2 : 1;
        slyz3_arr_set(vm, &row, 1, type);

        struct SlyzVal size;
        size.type = SLYZ_T_INT;
        size.aux = 0;
        size.u.i = (int64_t)entries[i].size;
        slyz3_arr_set(vm, &row, 2, size);

        slyz3_arr_set(vm, ret, i, row);
    }
    return 0;
}

static int t3_cfg_get(const char *key, char *out, int cap) {
    if (!key || !out || cap <= 0) return 0;
    out[0] = '\0';

    if (t3_strcmp(key, "boot.mode") == 0) {
        const char *v = system_cfg_boot_gui() ? "gui" : "tty";
        int i = 0;
        while (v[i] && i < cap - 1) { out[i] = v[i]; i++; }
        out[i] = '\0';
        return 1;
    }
    if (t3_strcmp(key, "lang") == 0) {
        const char *v = system_cfg_lang() ? "zh" : "en";
        int i = 0;
        while (v[i] && i < cap - 1) { out[i] = v[i]; i++; }
        out[i] = '\0';
        return 1;
    }
    if (t3_strcmp(key, "timezone") == 0) {
        t3_itoa(system_cfg_timezone(), out, cap);
        return 1;
    }
    if (t3_strcmp(key, "wallpaper") == 0) {
        const char *v = system_cfg_wallpaper();
        int i = 0;
        while (v && v[i] && i < cap - 1) { out[i] = v[i]; i++; }
        out[i] = '\0';
        return 1;
    }
    if (t3_strcmp(key, "avatar") == 0) {
        const char *v = system_cfg_avatar();
        int i = 0;
        while (v && v[i] && i < cap - 1) { out[i] = v[i]; i++; }
        out[i] = '\0';
        return 1;
    }
    return 0;
}

static int t3_cfg_set(const char *key, const char *value) {
    if (!key || !value) return -1;
    if (t3_strcmp(key, "boot.mode") == 0) {
        return system_cfg_set_boot_gui(t3_strcmp(value, "gui") == 0) ? 0 : -1;
    }
    if (t3_strcmp(key, "lang") == 0) {
        return system_cfg_set_lang(t3_strcmp(value, "en") == 0 ? 0 : 1) ? 0 : -1;
    }
    if (t3_strcmp(key, "timezone") == 0) {
        int v = 0;
        int neg = 0;
        const char *p = value;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        return system_cfg_set_timezone(neg ? -v : v) ? 0 : -1;
    }
    if (t3_strcmp(key, "wallpaper") == 0) {
        return system_cfg_set_wallpaper(value) ? 0 : -1;
    }
    if (t3_strcmp(key, "avatar") == 0) {
        return system_cfg_set_avatar(value) ? 0 : -1;
    }
    return -1;
}

/* 分区列表 -> [[drive, start, sectors, mounted, label], ...] */
static int t3_part_list(struct Slyz3Vm *vm, struct SlyzVal *ret) {
    struct SlyzPartInfo info[4];
    int n = slyzfs_part_list(info, 4);
    if (n < 0) n = 0;

    if (!slyz3_ret_arr(vm, ret, n)) return -1;
    for (int i = 0; i < n; i++) {
        struct SlyzVal row;
        if (!slyz3_ret_arr(vm, &row, 5)) return -1;

        struct SlyzVal v;
        v.type = SLYZ_T_INT;
        v.aux = 0;
        v.u.i = info[i].drive;
        slyz3_arr_set(vm, &row, 0, v);
        v.u.i = (int64_t)info[i].start_lba;
        slyz3_arr_set(vm, &row, 1, v);
        v.u.i = (int64_t)info[i].sectors;
        slyz3_arr_set(vm, &row, 2, v);
        v.u.i = info[i].mounted ? 1 : 0;
        slyz3_arr_set(vm, &row, 3, v);

        struct SlyzVal label;
        if (!slyz3_ret_str(vm, &label, info[i].label, t3_strlen(info[i].label))) return -1;
        slyz3_arr_set(vm, &row, 4, label);

        slyz3_arr_set(vm, ret, i, row);
    }
    return 0;
}

/* ============================================================
 * 系统调用分发
 * ============================================================ */

static int tty3_sys(struct Slyz3Vm *vm, uint16_t id,
                    struct SlyzVal *args, uint16_t argc,
                    struct SlyzVal *ret) {
    switch (id) {
        case SLE_SYS_PRINT:
            if (argc >= 1) print_value(vm, &args[0]);
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_PRINTLN:
            if (argc >= 1) print_value(vm, &args[0]);
            printk("\n");
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EPRINT:
            if (argc >= 1) print_value(vm, &args[0]);
            printk("\n");
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_INPUT: {
            int plen = 0;
            const char *prompt = arg_str(args, argc, 0, &plen);
            char line[256];
            t3_console_read_line(prompt, line, (int)sizeof(line));
            if (!slyz3_ret_str(vm, ret, line, t3_strlen(line))) return -1;
            return 0;
        }

        case SLE_SYS_FS_READ: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            return t3_fs_read(vm, path, ret);
        }

        case SLE_SYS_FS_WRITE: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            int tlen = 0;
            const char *text = arg_str(args, argc, 1, &tlen);
            if (!text) { text = ""; tlen = 0; }
            slyz_ret_int(ret, (int64_t)t3_fs_write(path, text, tlen));
            return 0;
        }

        case SLE_SYS_FS_EXISTS: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            struct Tty3DirEntry ent;
            slyz_ret_int(ret, (path && slyzfs_stat(path, &ent)) ? 1 : 0);
            return 0;
        }

        case SLE_SYS_FS_LIST: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            return t3_fs_list(vm, path, ret);
        }

        case SLE_SYS_FS_ENTRIES: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            return t3_fs_entries(vm, path, ret);
        }

        case SLE_SYS_FS_REMOVE: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (path && slyzfs_remove(path)) ? 0 : -1);
            return 0;
        }

        case SLE_SYS_FS_MKDIR: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (path && slyzfs_mkdir(path)) ? 0 : -1);
            return 0;
        }

        case SLE_SYS_FS_RMDIR: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (path && slyzfs_rmdir(path)) ? 0 : -1);
            return 0;
        }

        case SLE_SYS_FS_COPY: {
            const char *src = arg_str(args, argc, 0, 0);
            const char *dst = arg_str(args, argc, 1, 0);
            slyz_ret_int(ret, (src && dst) ? slyzfs_copy_file(src, dst) : -1);
            return 0;
        }

        case SLE_SYS_FS_MOVE: {
            const char *src = arg_str(args, argc, 0, 0);
            const char *dst = arg_str(args, argc, 1, 0);
            slyz_ret_int(ret, (src && dst) ? slyzfs_move_file(src, dst) : -1);
            return 0;
        }

        case SLE_SYS_CFG_GET: {
            int len = 0;
            const char *key = arg_str(args, argc, 0, &len);
            (void)len;
            char value[128];
            if (!t3_cfg_get(key, value, (int)sizeof(value))) value[0] = '\0';
            if (!slyz3_ret_str(vm, ret, value, t3_strlen(value))) return -1;
            return 0;
        }

        case SLE_SYS_CFG_SET: {
            int len = 0;
            const char *key = arg_str(args, argc, 0, &len);
            (void)len;
            const char *value = arg_str(args, argc, 1, 0);
            slyz_ret_int(ret, (int64_t)t3_cfg_set(key, value));
            return 0;
        }

        case SLE_SYS_THEME_GET:
            /* 终端宿主没有图形主题，返回 0 让调用方回退默认值。 */
            slyz_ret_int(ret, 0);
            return 0;

        case SLE_SYS_CFG_KEYS: {
            static const char *keys[7] = {
                "boot.mode", "lang", "timezone", "theme", "anim",
                "wallpaper", "avatar"
            };
            if (!slyz3_ret_arr(vm, ret, 7)) return -1;
            for (int i = 0; i < 7; i++) {
                struct SlyzVal item;
                if (!slyz3_ret_str(vm, &item, keys[i], t3_strlen(keys[i]))) return -1;
                slyz3_arr_set(vm, ret, i, item);
            }
            return 0;
        }

        case SLE_SYS_PART_LIST:
            return t3_part_list(vm, ret);

        case SLE_SYS_PART_ADD: {
            int drive = (int)arg_int(args, argc, 0);
            uint32_t sectors = (uint32_t)arg_int(args, argc, 1);
            int len = 0;
            const char *label = arg_str(args, argc, 2, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)slyzfs_part_add(drive, sectors, label));
            return 0;
        }

        case SLE_SYS_PART_DELETE:
            slyz_ret_int(ret, (int64_t)slyzfs_part_delete((int)arg_int(args, argc, 0)));
            return 0;

        case SLE_SYS_PART_FORMAT: {
            int drive = (int)arg_int(args, argc, 0);
            int len = 0;
            const char *label = arg_str(args, argc, 1, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)slyzfs_part_format(drive, label));
            return 0;
        }

        case SLE_SYS_PART_MOUNT:
            slyz_ret_int(ret, (int64_t)slyzfs_part_mount((int)arg_int(args, argc, 0)));
            return 0;

        case SLE_SYS_PART_UNMOUNT:
            slyz_ret_int(ret, (int64_t)slyzfs_part_unmount((int)arg_int(args, argc, 0)));
            return 0;

        case SLE_SYS_TIME_NOW:
            slyz_ret_int(ret, (int64_t)(net_now_ms() / 1000u));
            return 0;

        case SLE_SYS_TIME_MS:
            slyz_ret_int(ret, (int64_t)net_now_ms());
            return 0;

        case SLE_SYS_TIME_TEXT: {
            uint32_t ms = net_now_ms();
            char buf[9];
            int sec = (int)(ms / 1000u) % 86400;
            int h = sec / 3600;
            int m = (sec / 60) % 60;
            int s = sec % 60;
            buf[0] = (char)('0' + (h / 10) % 10);
            buf[1] = (char)('0' + h % 10);
            buf[2] = ':';
            buf[3] = (char)('0' + (m / 10) % 10);
            buf[4] = (char)('0' + m % 10);
            buf[5] = ':';
            buf[6] = (char)('0' + (s / 10) % 10);
            buf[7] = (char)('0' + s % 10);
            buf[8] = '\0';
            if (!slyz3_ret_str(vm, ret, buf, 8)) return -1;
            return 0;
        }

        case SLE_SYS_DATE_TEXT: {
            if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
            return 0;
        }

        case SLE_SYS_SLEEP: {
            uint32_t ms = (uint32_t)arg_int(args, argc, 0);
            if (ms > 10000u) ms = 10000u;
            uint32_t start = net_now_ms();
            while (net_now_ms() - start < ms) task_yield();
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_INFO: {
            int key = (int)arg_int(args, argc, 0);
            uint32_t total = 0, used = 0, peak = 0;
            uint32_t allocs = 0, frees = 0, failed = 0;
            mem_stats(&total, &used, &peak, &allocs, &frees, &failed);

            int64_t v = -1;
            if (key == SLE_INFO_MEM_TOTAL) v = (int64_t)total;
            else if (key == SLE_INFO_MEM_USED) v = (int64_t)used;
            else if (key == SLE_INFO_MEM_PEAK) v = (int64_t)peak;
            else if (key == SLE_INFO_MEM_FREE) v = (int64_t)mem_free_bytes();
            else if (key == SLE_INFO_TASKS_ALIVE) v = (int64_t)task_count_alive();
            else if (key == SLE_INFO_TASK_MEM) v = (int64_t)task_total_used();
            else if (key == SLE_INFO_DISK_WRITTEN) v = (int64_t)task_total_disk_written();
            else if (key == SLE_INFO_UPTIME_MS) v = (int64_t)net_now_ms();
            else if (key == SLE_INFO_CPU_PERMILLE) v = 0;
            else if (key == SLE_INFO_TASK_SLOTS) v = (int64_t)TASK_MAX;

            slyz_ret_int(ret, v);
            return 0;
        }

        case SLE_SYS_PROC: {
            int idx = (int)arg_int(args, argc, 0);
            int field = (int)arg_int(args, argc, 1);
            struct Task *t = (idx >= 0 && idx < TASK_MAX) ? task_by_id(idx) : 0;
            if (!t) { slyz_ret_int(ret, -1); return 0; }

            int64_t v = -1;
            if (field == SLE_PROC_ID) v = t->id;
            else if (field == SLE_PROC_STATE) v = t->state;
            else if (field == SLE_PROC_STEPS) v = t->steps;
            else if (field == SLE_PROC_QUOTA) v = (int64_t)t->quota;
            else if (field == SLE_PROC_USED) v = (int64_t)t->used;
            else if (field == SLE_PROC_EXIT) v = t->exit_code;

            slyz_ret_int(ret, v);
            return 0;
        }

        case SLE_SYS_PROC_NAME: {
            int idx = (int)arg_int(args, argc, 0);
            struct Task *t = (idx >= 0 && idx < TASK_MAX) ? task_by_id(idx) : 0;
            if (!t) {
                if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            if (!slyz3_ret_str(vm, ret, t->name, t3_strlen(t->name))) return -1;
            return 0;
        }

        case SLE_SYS_EXEC: {
            /* 终端宿主不递归执行 shell 命令，避免与当前命令栈冲突。 */
            slyz_ret_int(ret, -1);
            return 0;
        }

        case SLE_SYS_EXIT:
            slyz3vm_request_exit(vm, (int)arg_int(args, argc, 0));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_BEEP:
            audio_beep((uint32_t)arg_int(args, argc, 0),
                       (uint32_t)arg_int(args, argc, 1));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_OPEN_UI:
            slyz_ret_int(ret, -1);
            return 0;

        case SLE_SYS_MEDIA_REQ: {
            char path[160];
            audio_take_source(path, (int)sizeof(path));
            if (!slyz3_ret_str(vm, ret, path, t3_strlen(path))) return -1;
            return 0;
        }

        case SLE_SYS_AUDIO_PLAY: {
            int len = 0;
            const char *path = arg_str(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)audio_play(path));
            return 0;
        }
        case SLE_SYS_AUDIO_STOP: audio_stop(); slyz_ret_nil(ret); return 0;
        case SLE_SYS_AUDIO_PAUSE: audio_pause((int)arg_int(args, argc, 0)); slyz_ret_nil(ret); return 0;
        case SLE_SYS_AUDIO_STATE: slyz_ret_int(ret, (int64_t)audio_state()); return 0;
        case SLE_SYS_AUDIO_POS: slyz_ret_int(ret, (int64_t)audio_position_ms()); return 0;
        case SLE_SYS_AUDIO_DUR: slyz_ret_int(ret, (int64_t)audio_duration_ms()); return 0;
        case SLE_SYS_AUDIO_VOL: audio_set_volume((int)arg_int(args, argc, 0)); slyz_ret_nil(ret); return 0;
        case SLE_SYS_AUDIO_GETVOL: slyz_ret_int(ret, (int64_t)audio_volume()); return 0;

        case SLE_SYS_FAULT_TRIGGER:
            slyz_ret_int(ret, (int64_t)faultlab_trigger((int)arg_int(args, argc, 0)));
            return 0;
        case SLE_SYS_FAULT_DUMP:
            slyz_ret_int(ret, (int64_t)faultlab_dump_now());
            return 0;
        case SLE_SYS_FAULT_LEAK:
            slyz_ret_int(ret, (int64_t)faultlab_leak((int)arg_int(args, argc, 0)));
            return 0;
        case SLE_SYS_FAULT_LEAK_FREE:
            slyz_ret_int(ret, (int64_t)faultlab_leak_free());
            return 0;
        case SLE_SYS_FAULT_SET: {
            int len = 0;
            const char *key = arg_str(args, argc, 0, &len);
            (void)len;
            const char *value = arg_str(args, argc, 1, 0);
            slyz_ret_int(ret, (int64_t)faultlab_set(key, value));
            return 0;
        }
        case SLE_SYS_FAULT_GET: {
            int len = 0;
            const char *key = arg_str(args, argc, 0, &len);
            (void)len;
            char value[32];
            if (faultlab_get(key, value, (int)sizeof(value)) != 0) {
                if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            if (!slyz3_ret_str(vm, ret, value, t3_strlen(value))) return -1;
            return 0;
        }

        /* 无窗口宿主：窗口/绘图/事件/图片全部是安全空实现。 */
        case SLE_SYS_WIN_OPEN:
        case SLE_SYS_WIN_SIZE:
        case SLE_SYS_WIN_HEIGHT:
        case SLE_SYS_GFX_TWIDTH:
            slyz_ret_int(ret, 0);
            return 0;

        case SLE_SYS_WIN_CLOSE:
        case SLE_SYS_WIN_TITLE:
        case SLE_SYS_GFX_CLEAR:
        case SLE_SYS_GFX_RECT:
        case SLE_SYS_GFX_FRAME:
        case SLE_SYS_GFX_TEXT:
        case SLE_SYS_GFX_LINE:
        case SLE_SYS_GFX_CIRCLE:
        case SLE_SYS_GFX_FLUSH:
        case SLE_SYS_UI_MARKUP:
        case SLE_SYS_UI_STYLE:
        case SLE_SYS_UI_PRESENT:
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_UI_SET:
            slyz_ret_int(ret, -1);
            return 0;

        case SLE_SYS_UI_GET:
        case SLE_SYS_EVT_POLL:
        case SLE_SYS_EVT_WAIT:
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_IMG_LOAD:
            slyz_ret_int(ret, -1);
            return 0;

        case SLE_SYS_IMG_WIDTH:
        case SLE_SYS_IMG_HEIGHT:
            slyz_ret_int(ret, -1);
            return 0;

        case SLE_SYS_IMG_DRAW:
        case SLE_SYS_IMG_DRAW_SCALED:
        case SLE_SYS_IMG_DRAW_CIRCLE:
        case SLE_SYS_IMG_FREE:
            slyz_ret_nil(ret);
            return 0;

        default:
            slyz_ret_nil(ret);
            return 0;
    }
}

static void *tty3_alloc(uint32_t bytes) {
    return kmalloc(bytes);
}

static void tty3_free(void *ptr) {
    kfree(ptr);
}

static void tty3_log(const char *text) {
    if (text) printk("[SLYZ3] %s\n", text);
}

static const struct Slyz3Ops g_slyz3_tty_ops = {
    tty3_sys,
    tty3_alloc,
    tty3_free,
    0,
    tty3_log
};

int slyz3_tty_run(const uint8_t *image, int size, const char *name) {
    if (!image || size <= 0) return 0;
    if (!slyz3_is_binary(image, size)) return 0;

    struct Slyz3Vm *vm = slyz3vm_create(image, size, &g_slyz3_tty_ops, name);
    if (!vm) {
        printk("run: cannot load SLYZ v3 image (format or memory)\n");
        return 0;
    }
    if (slyz3vm_uses_gui(vm)) {
        printk("run: this SLYZ v3 program requires the graphical session\n");
        slyz3vm_destroy(vm);
        return 0;
    }

    for (;;) {
        int st = slyz3vm_step(vm, 20000);
        if (st == 0) break;
        if (st < 0) {
            const char *err = slyz3vm_error(vm);
            printk("run: SLYZ v3 error: %s\n", err ? err : "unknown");
            slyz3vm_destroy(vm);
            return 0;
        }
        task_yield();
    }

    int code = slyz3vm_exit_code(vm);
    slyz3vm_destroy(vm);
    return code == 0 ? 1 : 0;
}
