/* ============================================================
 * faultlab.c - 故障注入与崩溃转储
 * ------------------------------------------------------------
 * 本文件只依赖内核公开接口：mem / task / net / bsod / SlyzFS。
 * 所有文件写入都包在 fl_mute_begin()/fl_mute_end() 之间，使
 * kserial_putchar() 丢弃写入期间产生的任何串口字节。这样即使
 * SlyzFS 在写入路径里打印诊断，也不会反过来触发串口落盘。
 * ============================================================ */

#include <stdint.h>

#include "faultlab.h"
#include "mem.h"
#include "task.h"
#include "net.h"
#include "bsod.h"

/* SlyzFS 桥接层（与 shell.c 使用同一组 C 接口） */
extern int   slyzfs_mkdir(const char *path);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

/* 与 shell.c / gui.c 保持逐字节一致的 71 字节目录项。
 * 只用到 type，但布局必须完整，否则 stat 写入会越界。 */
#pragma pack(push, 1)
struct FlDirEntry {
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

#define FL_DIR_DIR 0x02

extern int   slyzfs_stat(const char *path, struct FlDirEntry *out);

#define FL_CFG_PATH        "etc\\faultlab.cfg"
#define FL_LOG_DIR         "system\\logs"
#define FL_LOG_PATH        "system\\logs\\serial.log"
#define FL_CRASH_DIR       "system\\crash"
#define FL_LEAK_BLOCK      (64u * 1024u)
#define FL_LEAK_MAX        64
#define FL_EXHAUST_MAX     1024
#define FL_LOG_BUF         2048

/* ============================================================
 * 串口静默闸门
 * ------------------------------------------------------------
 * 计数而不是布尔值：dump 内部可能嵌套 mkdir / open / write，
 * 每一层都成对进出，只有最外层退出后才恢复串口。
 * ============================================================ */

static int g_mute_depth = 0;

static void fl_mute_begin(void) {
    g_mute_depth++;
}

static void fl_mute_end(void) {
    if (g_mute_depth > 0) g_mute_depth--;
}

int faultlab_serial_muted(void) {
    return g_mute_depth > 0;
}

/* ============================================================
 * 基础字符串工具（内核没有 libc）
 * ============================================================ */

static int fl_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void fl_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int fl_same(const char *a, const char *b) {
    int i = 0;
    while (a && b && a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return (a && b && a[i] == b[i]) ? 1 : 0;
}

/* 把无符号整数写成十进制，返回写入长度 */
static int fl_dec(uint32_t v, char *out, int cap) {
    char tmp[12];
    int n = 0;
    int i = 0;

    if (cap <= 0) return 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && i < cap - 1) out[i++] = tmp[--n];
    out[i] = '\0';
    return i;
}

static int fl_hex8(uint32_t v, char *out, int cap) {
    static const char hex[] = "0123456789ABCDEF";
    if (cap < 9) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = hex[(v >> ((7 - i) * 4)) & 0xFu];
    }
    out[8] = '\0';
    return 8;
}

/* ============================================================
 * 目录与文件写入
 * ============================================================ */

/* 逐级确保目录存在。
 * 关键点：已存在的目录绝不能再调 mkdir —— SlyzFS 会打印
 * "already exists" 错误，用户会误以为创建失败。因此每一级
 * 先用 stat 判断：目录存在就跳过，路径被同名文件占用则失败。
 * 返回 0 表示整条路径都是目录；-1 表示无法建立。 */
static int fl_ensure_dir(const char *path) {
    char tmp[128];
    int len = 0;

    if (!path) return -1;
    while (path[len] && len < (int)sizeof(tmp) - 1) {
        tmp[len] = path[len];
        len++;
        if (path[len] == '\\' || path[len] == '/' || path[len] == '\0') {
            tmp[len] = '\0';
            if (len > 0) {
                struct FlDirEntry ent;
                if (slyzfs_stat(tmp, &ent)) {
                    if (ent.type != FL_DIR_DIR) return -1;
                } else if (!slyzfs_mkdir(tmp)) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* 只负责把字节完整写入；任何失败都只返回错误，不打印。 */
static int fl_write_all(void *fh, const char *data, int len) {
    int off = 0;
    if (!fh || len <= 0) return 0;
    while (off < len) {
        int n = slyzfs_write(fh, (const uint8_t *)data + off, len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

/* 分块写入器：把较长的转储拆成小段，避免在栈上拼大字符串。 */
struct FlWriter {
    void *fh;
    char  buf[256];
    int   len;
    int   ok;
};

static void flw_flush(struct FlWriter *w) {
    if (w->len <= 0) return;
    if (fl_write_all(w->fh, w->buf, w->len) != 0) w->ok = 0;
    w->len = 0;
}

static void flw_str(struct FlWriter *w, const char *s) {
    if (!w->ok || !s) return;
    while (*s) {
        if (w->len >= (int)sizeof(w->buf)) flw_flush(w);
        w->buf[w->len++] = *s++;
    }
}

static void flw_dec(struct FlWriter *w, uint32_t v) {
    char tmp[16];
    fl_dec(v, tmp, (int)sizeof(tmp));
    flw_str(w, tmp);
}

static void flw_hex8(struct FlWriter *w, uint32_t v) {
    char tmp[12];
    fl_hex8(v, tmp, (int)sizeof(tmp));
    flw_str(w, tmp);
}

/* ============================================================
 * 配置
 * ============================================================ */

static int g_dump_on = 0;
static int g_serial_on = 0;

static int fl_parse_flag(const char *v) {
    if (!v) return -1;
    if (fl_same(v, "1") || fl_same(v, "on") || fl_same(v, "true") ||
        fl_same(v, "yes")) {
        return 1;
    }
    if (fl_same(v, "0") || fl_same(v, "off") || fl_same(v, "false") ||
        fl_same(v, "no")) {
        return 0;
    }
    return -1;
}

static void fl_cfg_load(void) {
    void *fh = slyzfs_open(FL_CFG_PATH, "r");
    char buf[512];
    int len = 0;

    g_dump_on = 0;
    g_serial_on = 0;
    if (!fh) return;

    while (len < (int)sizeof(buf) - 1) {
        int n = slyzfs_read(fh, (uint8_t *)buf + len, (int)sizeof(buf) - 1 - len);
        if (n <= 0) break;
        len += n;
    }
    slyzfs_close(fh);
    buf[len] = '\0';

    int i = 0;
    while (i < len) {
        char key[24];
        char val[24];
        int kn = 0;
        int vn = 0;

        while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                           buf[i] == '\r' || buf[i] == '\n')) {
            i++;
        }
        if (i >= len) break;
        if (buf[i] == '#') {
            while (i < len && buf[i] != '\n') i++;
            continue;
        }

        while (i < len && buf[i] != '=' && buf[i] != '\r' && buf[i] != '\n' &&
               kn < (int)sizeof(key) - 1) {
            key[kn++] = buf[i++];
        }
        key[kn] = '\0';

        while (kn > 0 && (key[kn - 1] == ' ' || key[kn - 1] == '\t')) {
            key[--kn] = '\0';
        }

        if (i < len && buf[i] == '=') {
            i++;
            while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
            while (i < len && buf[i] != '\r' && buf[i] != '\n' &&
                   vn < (int)sizeof(val) - 1) {
                val[vn++] = buf[i++];
            }
            val[vn] = '\0';
            while (vn > 0 && (val[vn - 1] == ' ' || val[vn - 1] == '\t')) {
                val[--vn] = '\0';
            }

            if (fl_same(key, "dump")) {
                int f = fl_parse_flag(val);
                if (f >= 0) g_dump_on = f;
            } else if (fl_same(key, "serial_to_disk")) {
                int f = fl_parse_flag(val);
                if (f >= 0) g_serial_on = f;
            }
        }

        while (i < len && buf[i] != '\n') i++;
    }
}

/* 配置写回始终是完整的两个键，避免半截文件导致下次启动解析失败。
 * 返回 0 表示文件已落盘；-1 表示目录不可用或写入失败。 */
static int fl_cfg_save(void) {
    const char *dump_text = g_dump_on ? "dump=1\r\nserial_to_disk="
                                      : "dump=0\r\nserial_to_disk=";
    const char *serial_text = g_serial_on ? "1\r\n" : "0\r\n";
    int ok = 0;

    fl_mute_begin();
    if (fl_ensure_dir("etc") == 0) {
        void *fh = slyzfs_open(FL_CFG_PATH, "w");
        if (fh) {
            ok = (fl_write_all(fh, dump_text, fl_strlen(dump_text)) == 0 &&
                  fl_write_all(fh, serial_text, fl_strlen(serial_text)) == 0);
            slyzfs_close(fh);
        }
    }
    fl_mute_end();

    return ok ? 0 : -1;
}

/* ============================================================
 * 串口落盘
 * ------------------------------------------------------------
 * 日志以 "w" 打开：每次启动截断，避免日志无限增长。
 * 采集缓冲只在换行且已经积累到 512 字节以上时落盘，崩溃时由
 * faultlab_on_bsod() 强制刷盘，正常运行时最多丢最后 2KB。
 * ============================================================ */

static void *g_log_fh = 0;
static char  g_log_buf[FL_LOG_BUF];
static int   g_log_len = 0;
static int   g_capture = 0;
static int   g_log_reentry = 0;

static void fl_log_flush_locked(void) {
    if (!g_log_fh || g_log_len <= 0) {
        g_log_len = 0;
        return;
    }
    int off = 0;
    while (off < g_log_len) {
        int n = slyzfs_write(g_log_fh, (const uint8_t *)g_log_buf + off,
                             g_log_len - off);
        if (n <= 0) break;
        off += n;
    }
    g_log_len = 0;
}

/* 打开串口日志。返回 0 成功；-1 表示目录或文件不可写。 */
static int fl_capture_start(void) {
    if (g_log_fh) return 0;

    fl_mute_begin();
    int ok = (fl_ensure_dir("system") == 0 && fl_ensure_dir(FL_LOG_DIR) == 0);
    g_log_fh = ok ? slyzfs_open(FL_LOG_PATH, "w") : 0;
    g_log_len = 0;
    if (g_log_fh) {
        static const char header[] =
            "[SylFault] serial capture started\r\n";
        if (fl_write_all(g_log_fh, header, (int)sizeof(header) - 1) != 0) {
            slyzfs_close(g_log_fh);
            g_log_fh = 0;
        }
    }
    fl_mute_end();

    g_capture = g_log_fh ? 1 : 0;
    return g_capture ? 0 : -1;
}

static void fl_capture_stop(void) {
    g_capture = 0;
    fl_mute_begin();
    fl_log_flush_locked();
    if (g_log_fh) {
        slyzfs_close(g_log_fh);
        g_log_fh = 0;
    }
    g_log_len = 0;
    fl_mute_end();
}

void faultlab_serial_tap(char c) {
    if (!g_capture || !g_log_fh || g_log_reentry) return;

    g_log_reentry = 1;
    fl_mute_begin();

    if (g_log_len >= FL_LOG_BUF) fl_log_flush_locked();
    g_log_buf[g_log_len++] = c;
    if (c == '\n' && g_log_len >= 512) fl_log_flush_locked();

    fl_mute_end();
    g_log_reentry = 0;
}

void faultlab_serial_flush(void) {
    if (!g_log_fh) return;
    if (g_log_reentry) return;

    g_log_reentry = 1;
    fl_mute_begin();
    fl_log_flush_locked();
    fl_mute_end();
    g_log_reentry = 0;
}

/* ============================================================
 * 内存泄漏与耗尽模拟
 * ============================================================ */

static void *g_leak_blocks[FL_LEAK_MAX];
static int   g_leak_count = 0;

int faultlab_leak_total(void) {
    return g_leak_count * (int)(FL_LEAK_BLOCK / 1024u);
}

int faultlab_leak(int kb) {
    int want;

    if (kb <= 0) kb = 64;
    want = (kb + 63) / 64;

    for (int i = 0; i < want && g_leak_count < FL_LEAK_MAX; i++) {
        void *p = kmalloc(FL_LEAK_BLOCK);
        if (!p) break;
        g_leak_blocks[g_leak_count++] = p;
    }

    return faultlab_leak_total();
}

int faultlab_leak_free(void) {
    int freed = 0;
    for (int i = 0; i < g_leak_count; i++) {
        if (g_leak_blocks[i]) {
            kfree(g_leak_blocks[i]);
            g_leak_blocks[i] = 0;
            freed += (int)(FL_LEAK_BLOCK / 1024u);
        }
    }
    g_leak_count = 0;
    return freed;
}

/* 内存耗尽：持续申请 64KB 直到 kmalloc 失败，然后释放最后
 * 32 块（2MB）留给转储写入。释放是有意为之——没有这 2MB，
 * 崩溃后的 dump 实际上写不进 SlyzFS。 */
static void *g_exhaust_blocks[FL_EXHAUST_MAX];
static int   g_exhaust_count = 0;

static void fl_exhaust_memory(void) {
    g_exhaust_count = 0;
    while (g_exhaust_count < FL_EXHAUST_MAX) {
        void *p = kmalloc(FL_LEAK_BLOCK);
        if (!p) break;
        g_exhaust_blocks[g_exhaust_count++] = p;
    }

    int keep = g_exhaust_count;
    int free_now = keep < 32 ? keep : 32;
    for (int i = 0; i < free_now; i++) {
        int idx = keep - 1 - i;
        if (idx >= 0 && g_exhaust_blocks[idx]) {
            kfree(g_exhaust_blocks[idx]);
            g_exhaust_blocks[idx] = 0;
        }
    }
}

/* ============================================================
 * 崩溃转储
 * ============================================================ */

static int g_in_dump = 0;

static const char *fl_task_state(int state) {
    if (state == 0) return "free";
    if (state == 1) return "ready";
    if (state == 2) return "running";
    if (state == 3) return "dead";
    return "?";
}

static void fl_dump_path(char *out, int cap, uint32_t ms) {
    const char *prefix = FL_CRASH_DIR "\\dump-";
    char num[16];
    int p = 0;

    fl_dec(ms, num, (int)sizeof(num));
    while (*prefix && p < cap - 1) out[p++] = *prefix++;
    for (int i = 0; num[i] && p < cap - 1; i++) out[p++] = num[i];
    if (p < cap - 1) out[p++] = '.';
    if (p < cap - 1) out[p++] = 't';
    if (p < cap - 1) out[p++] = 'x';
    if (p < cap - 1) out[p++] = 't';
    out[p] = '\0';
}

/* 调用者必须已处于静默窗口内。stop_code 为 0 表示手动转储。 */
static int fl_dump_locked(uint32_t stop_code, const char *reason) {
    struct FlWriter w;
    char path[96];
    uint32_t total = 0, used = 0, peak = 0;
    uint32_t allocs = 0, frees = 0, failed = 0;

    if (g_in_dump) return -1;
    g_in_dump = 1;

    int dirs_ok = (fl_ensure_dir("system") == 0 &&
                   fl_ensure_dir(FL_CRASH_DIR) == 0);
    fl_dump_path(path, (int)sizeof(path), net_now_ms());

    w.fh = dirs_ok ? slyzfs_open(path, "w") : 0;
    w.len = 0;
    w.ok = w.fh ? 1 : 0;

    flw_str(&w, "SaylterOS crash dump\r\n");
    flw_str(&w, "====================\r\n");
    flw_str(&w, "generator: SylFault\r\n");
    flw_str(&w, "STOP: 0x");
    flw_hex8(&w, stop_code);
    flw_str(&w, "\r\nReason: ");
    flw_str(&w, reason ? reason : "(none)");
    flw_str(&w, "\r\nUptime: ");
    flw_dec(&w, net_now_ms());
    flw_str(&w, " ms\r\n");

    mem_stats(&total, &used, &peak, &allocs, &frees, &failed);
    flw_str(&w, "Heap total: ");
    flw_dec(&w, total);
    flw_str(&w, " bytes\r\nHeap used: ");
    flw_dec(&w, used);
    flw_str(&w, " bytes\r\nHeap peak: ");
    flw_dec(&w, peak);
    flw_str(&w, " bytes\r\nHeap allocs: ");
    flw_dec(&w, allocs);
    flw_str(&w, "\r\nHeap frees: ");
    flw_dec(&w, frees);
    flw_str(&w, "\r\nHeap failed: ");
    flw_dec(&w, failed);
    flw_str(&w, "\r\nTasks alive: ");
    flw_dec(&w, (uint32_t)task_count_alive());
    flw_str(&w, "\r\nLeaked blocks: ");
    flw_dec(&w, (uint32_t)g_leak_count);
    flw_str(&w, " (");
    flw_dec(&w, (uint32_t)faultlab_leak_total());
    flw_str(&w, " KB)\r\n");
    flw_str(&w, "Options: dump=");
    flw_str(&w, g_dump_on ? "1" : "0");
    flw_str(&w, " serial_to_disk=");
    flw_str(&w, g_serial_on ? "1" : "0");
    flw_str(&w, "\r\n\r\nTask table\r\n");
    flw_str(&w, "slot state   used      peak      quota     steps     name\r\n");

    for (int i = 0; i < TASK_MAX; i++) {
        struct Task *t = task_by_id(i);
        if (!t) continue;

        flw_dec(&w, (uint32_t)i);
        flw_str(&w, "    ");
        flw_str(&w, fl_task_state(t->state));
        flw_str(&w, "  ");
        flw_dec(&w, t->used);
        flw_str(&w, "  ");
        flw_dec(&w, t->peak);
        flw_str(&w, "  ");
        flw_dec(&w, t->quota);
        flw_str(&w, "  ");
        flw_dec(&w, t->steps);
        flw_str(&w, "  ");
        flw_str(&w, t->name);
        flw_str(&w, "\r\n");
    }

    flw_str(&w, "\r\nEnd of dump\r\n");
    flw_flush(&w);

    if (w.fh) {
        slyzfs_close(w.fh);
    }

    g_in_dump = 0;
    return w.ok ? 0 : -1;
}

int faultlab_dump_now(void) {
    int rc;
    fl_mute_begin();
    rc = fl_dump_locked(0u, "Manual dump requested from SylFault");
    fl_mute_end();
    return rc;
}

void faultlab_on_bsod(uint32_t stop_code, const char *reason) {
    fl_mute_begin();
    if (g_dump_on) {
        fl_dump_locked(stop_code, reason);
    }
    /* 崩溃前把串口缓冲强制落盘，之后的摘要由 bsod 再刷一次。
     * 两次 flush 都在静默窗口内，不会再产生串口输出。 */
    fl_log_flush_locked();
    fl_mute_end();
}

/* ============================================================
 * 故障注入
 * ============================================================ */

int faultlab_trigger(int kind) {
    switch (kind) {
        case 0:
            bsod(0xE2u, "MANUALLY_INITIATED_CRASH (SylFault simulation)");
            break;
        case 1:
            bsod(0x1Eu, "KMODE_EXCEPTION_NOT_HANDLED - divide by zero (SylFault simulation)");
            break;
        case 2:
            bsod(0x50u, "PAGE_FAULT_IN_NONPAGED_AREA - null pointer (SylFault simulation)");
            break;
        case 3:
            bsod(0x7Fu, "UNEXPECTED_KERNEL_MODE_TRAP - illegal instruction (SylFault simulation)");
            break;
        case 4:
            bsod(0xC2u, "BAD_POOL_CALLER - double free (SylFault simulation)");
            break;
        case 5:
            bsod(0xF7u, "DRIVER_OVERRAN_STACK_BUFFER - stack overflow (SylFault simulation)");
            break;
        case 6:
            fl_exhaust_memory();
            bsod(0xE4u, "OUT_OF_MEMORY - heap exhausted (SylFault simulation)");
            break;
        case 7:
            bsod(0x0Au, "IRQL_NOT_LESS_OR_EQUAL (SylFault simulation)");
            break;
        case 8:
            bsod(0x139u, "KERNEL_SECURITY_CHECK_FAILURE (SylFault simulation)");
            break;
        case 9:
            bsod(0x109u, "CRITICAL_STRUCTURE_CORRUPTION (SylFault simulation)");
            break;
        case 10:
            bsod(0xD1u, "DRIVER_IRQL_NOT_LESS_OR_EQUAL (SylFault simulation)");
            break;
        default:
            return -1;
    }

    return -1;
}

/* ============================================================
 * 对外配置接口
 * ============================================================ */

int faultlab_set(const char *key, const char *value) {
    int flag;

    if (!key || !value) return -1;

    if (fl_same(key, "dump")) {
        flag = fl_parse_flag(value);
        if (flag < 0) return -1;

        int prev = g_dump_on;
        g_dump_on = flag;
        if (fl_cfg_save() != 0) {
            /* 配置没能落盘就回滚运行时状态：界面显示的 ON/OFF
             * 必须和下次启动读到的值一致。 */
            g_dump_on = prev;
            return -1;
        }
        return 0;
    }

    if (fl_same(key, "serial_to_disk")) {
        flag = fl_parse_flag(value);
        if (flag < 0) return -1;

        if (flag && !g_serial_on) {
            g_serial_on = 1;
            if (fl_capture_start() != 0) {
                g_serial_on = 0;
                return -1;
            }
            if (fl_cfg_save() != 0) {
                g_serial_on = 0;
                fl_capture_stop();
                return -1;
            }
            return 0;
        }

        if (!flag && g_serial_on) {
            g_serial_on = 0;
            fl_capture_stop();
            if (fl_cfg_save() != 0) {
                g_serial_on = 1;
                fl_capture_start();
                return -1;
            }
            return 0;
        }

        /* 状态已经是目标值：只补写一次配置文件。 */
        return fl_cfg_save();
    }

    return -1;
}

int faultlab_get(const char *key, char *out, int cap) {
    if (!key || !out || cap <= 0) return -1;

    if (fl_same(key, "dump")) {
        fl_copy(out, g_dump_on ? "1" : "0", cap);
        return 0;
    }
    if (fl_same(key, "serial_to_disk")) {
        fl_copy(out, g_serial_on ? "1" : "0", cap);
        return 0;
    }
    if (fl_same(key, "leak_kb")) {
        char tmp[16];
        fl_dec((uint32_t)faultlab_leak_total(), tmp, (int)sizeof(tmp));
        fl_copy(out, tmp, cap);
        return 0;
    }

    return -1;
}

/* ============================================================
 * 初始化
 * ============================================================ */

void faultlab_init(void) {
    /* 复位所有运行期状态：本函数可能在系统盘重新挂载后再次调用。 */
    g_mute_depth = 0;
    g_log_fh = 0;
    g_log_buf[0] = '\0';
    g_log_len = 0;
    g_capture = 0;
    g_log_reentry = 0;
    g_in_dump = 0;
    g_leak_count = 0;
    g_exhaust_count = 0;

    for (int i = 0; i < FL_LEAK_MAX; i++) g_leak_blocks[i] = 0;
    for (int i = 0; i < FL_EXHAUST_MAX; i++) g_exhaust_blocks[i] = 0;

    fl_cfg_load();

    if (g_serial_on) {
        if (fl_capture_start() != 0) {
            /* 磁盘不可写时不要假装已开启：保持配置为 0，下次启动不重试。 */
            g_serial_on = 0;
            fl_cfg_save();
        }
    }
}
