/* ============================================================
 * debug.c - 调试模式实现
 * ------------------------------------------------------------
 * 早期调试控制台运行在 kernel_main 的显示初始化之后、正常系统
 * 启动之前。此阶段刻意不挂载 SlyzFS：
 *   - 字体来自内核内嵌的 Ubuntu Mono，平滑渲染仍然可用；
 *   - 磁盘只做只读探测，绝不写主硬盘；
 *   - 用户可以在这里打开"串口落盘 / HUD / 禁止自动格式化"，
 *     再输入 next 进入正常系统。
 *
 * 串口落盘在 SlyzFS 就绪后写入 **当前启动盘** 的
 * system\logs\debug-serial.log。写盘严格遵守：
 *   - 只有 ring 中存在数据才调用 write；
 *   - write 返回 <=0 时不推进读指针，下一轮重试；
 *   - 单次刷新有字节预算，避免阻塞输入。
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>

#include "debug.h"
#include "tty.h"
#include "vbe.h"
#include "io.h"
#include "keyboard.h"
#include "mem.h"
#include "blk.h"
#include "reboot.h"
#include "slyertype.h"

#if SAYLTER_DEBUG

/* ============================================================
 * 设置
 * ============================================================ */

static int g_serial_on;
static int g_hud_on;
static int g_safe_format = 1;      /* 调试模式默认禁止自动格式化 */
static int g_storage_ready;

/* ============================================================
 * 串口日志环形缓冲
 * ============================================================ */

#define DBG_LOG_RING   65536u
#define DBG_LOG_BUDGET 16384u

static uint8_t  g_log[DBG_LOG_RING];
static volatile uint32_t g_log_head;
static volatile uint32_t g_log_tail;
static int      g_fs_ready;
static int      g_log_started;
static int      g_log_retry;

extern int   slyzfs_mkdir(const char *path);
extern int   slyzfs_stat(const char *path, void *out);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

/* ============================================================
 * 基础工具
 * ============================================================ */

static uint64_t debug_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int dbg_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static int dbg_prefix(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static char *dbg_put(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *dbg_u32(char *p, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) *p++ = tmp[--n];
    return p;
}

static char *dbg_u32_3(char *p, uint32_t v) {
    if (v > 999u) v = 999u;
    *p++ = (char)('0' + (v / 100u));
    *p++ = (char)('0' + ((v / 10u) % 10u));
    *p++ = (char)('0' + (v % 10u));
    return p;
}

static void dbg_terminate(char *p) {
    *p = '\0';
}

/* ============================================================
 * 串口镜像与落盘
 * ============================================================ */

void debug_serial_byte(char c) {
    if (!g_serial_on) return;

    uint32_t head = g_log_head;
    uint32_t next = (head + 1u) % DBG_LOG_RING;
    if (next == g_log_tail) {
        /* ring 满：丢弃最旧的一个字节，让最新日志始终可见。 */
        g_log_tail = (g_log_tail + 1u) % DBG_LOG_RING;
    }
    g_log[head] = (uint8_t)c;
    g_log_head = next;
}

void debug_serial_flush(void) {
    if (!g_serial_on || !g_fs_ready) return;

    /* 没有待写数据时绝不打开/写入文件——这是"写入空"的第一道防线。 */
    uint32_t head = g_log_head;
    uint32_t tail = g_log_tail;
    if (head == tail) return;

    if (g_log_retry > 0) {
        g_log_retry--;
        return;
    }

    /* 目录只需在本次启动第一次落盘前确认一次：
     * 反复 mkdir 已存在的目录会让 SlyzFS 打印 failed，污染日志。 */
    if (!g_log_started) {
        uint8_t stbuf[80];
        if (!slyzfs_stat("system\\logs", stbuf)) {
            (void)slyzfs_mkdir("system\\logs");
        }
    }

    /* 每次刷新都 close：SlyzFS 只在 close 时把 size 回写目录项。
     * 长期保持打开会让 dir 看到 0 字节，掉电后日志等于没写。 */    const char *mode = g_log_started ? "a" : "w";
    void *fh = slyzfs_open("system\\logs\\debug-serial.log", mode);
    if (!fh) {
        g_log_retry = 15;
        return;
    }
    g_log_started = 1;

    uint32_t budget = DBG_LOG_BUDGET;
    while (budget > 0) {
        head = g_log_head;
        tail = g_log_tail;
        if (head == tail) break;

        uint32_t n;
        if (head > tail) n = head - tail;
        else n = DBG_LOG_RING - tail;
        if (n > budget) n = budget;
        if (n == 0) break;

        int wrote = slyzfs_write(fh, g_log + tail, (int)n);
        if (wrote <= 0) {
            /* 写失败：保留未写数据，关闭后稍后重试。 */
            slyzfs_close(fh);
            g_log_retry = 15;
            return;
        }
        g_log_tail = (tail + (uint32_t)wrote) % DBG_LOG_RING;
        budget -= (uint32_t)wrote;
    }

    slyzfs_close(fh);
}

void debug_serial_fs_ready(void) {
    g_fs_ready = 1;
    debug_serial_flush();
}

void debug_storage_ready(void) {
    g_storage_ready = 1;
}

/* ============================================================
 * CPU 空闲采样
 * ============================================================ */

static uint64_t g_idle_last_tsc;
static uint64_t g_idle_cycles;

void debug_cpu_idle_mark(void) {
    uint64_t now = debug_rdtsc();
    if (g_idle_last_tsc != 0) {
        uint64_t d = now - g_idle_last_tsc;
        /* 只在两次空闲采样间隔很短时计入，避免把命令执行时间
         * 误算成空闲。 */
        if (d < 20000000ull) g_idle_cycles += d;
    }
    g_idle_last_tsc = now;
}

/* ============================================================
 * HUD
 * ============================================================ */

#define DBG_TICK_CYCLES  900000000ull   /* 约 0.3 秒（按 3GHz 估算） */
#define HUD_COLS         20

static uint64_t g_last_tick;
static uint64_t g_last_idle_snapshot;

extern uint32_t gui_system_cpu_permille(void);

static void hud_build(char lines[3][28], uint32_t cpu_pct) {
    uint32_t mem_total = 0, mem_used = 0, mem_peak = 0;
    uint32_t allocs = 0, frees = 0, failed = 0;
    mem_stats(&mem_total, &mem_used, &mem_peak, &allocs, &frees, &failed);

    char *p = lines[0];
    p = dbg_put(p, "MEM ");
    p = dbg_u32(p, mem_used / 1024u);
    p = dbg_put(p, "/");
    p = dbg_u32(p, mem_total / 1024u);
    p = dbg_put(p, "K");
    dbg_terminate(p);

    uint32_t reads = 0, writes = 0;
    if (g_storage_ready) {
        blk_stats(&reads, &writes);
    }

    p = lines[1];
    p = dbg_put(p, "CPU ");
    p = dbg_u32_3(p, cpu_pct);
    p = dbg_put(p, "% R");
    p = dbg_u32(p, reads);
    p = dbg_put(p, " W");
    p = dbg_u32(p, writes);
    dbg_terminate(p);

    p = lines[2];
    p = dbg_put(p, "BLK ");
    const char *be = g_storage_ready ? blk_backend() : "not-ready";
    int n = 0;
    while (*be && n < 16) { *p++ = *be++; n++; }
    dbg_terminate(p);
}

static uint32_t hud_cpu_from_idle(uint64_t now) {
    uint64_t wall = now - g_last_tick;
    if (wall == 0) return 0;

    uint64_t idle = g_idle_cycles - g_last_idle_snapshot;
    g_last_idle_snapshot = g_idle_cycles;
    if (idle > wall) idle = wall;

    uint32_t cpu = 100u;
    if (wall != 0) {
        cpu = (uint32_t)(100u - (idle * 100u) / wall);
    }
    if (cpu > 100u) cpu = 100u;
    return cpu;
}

static void hud_render_fb(const char lines[3][28]) {
    if (!vbe_is_enabled()) return;
    uint16_t cols = tty_get_cols();
    if (cols <= HUD_COLS) return;

    uint16_t cx = (uint16_t)(cols - HUD_COLS);
    uint16_t x = (uint16_t)(cx * 16);
    uint16_t w = (uint16_t)(HUD_COLS * 16);

    vbe_fb_fill_rect(x, 0, w, 96, 0x0B1016u);
    /* 左侧强调色竖条 */
    vbe_fb_fill_rect(x, 0, 2, 96, 0x3B82F6u);

    for (int row = 0; row < 3; row++) {
        const char *s = lines[row];
        for (int i = 0; i < HUD_COLS - 1 && s[i]; i++) {
            vbe_fb_draw_char((uint16_t)(cx + 1 + i), (uint16_t)row,
                             s[i], 0xE6EDF3u, 0x0B1016u);
        }
    }
}

static void hud_render_buf(const char lines[3][28]) {
    if (!vbe_is_enabled()) return;

    int w = HUD_COLS * 16;
    int h = 96;
    int x = (int)vbe_current.width - w - 8;
    int y = 8;
    if (x < 0) x = 0;

    vbe_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, 0x0B1016u);
    vbe_fill_rect((uint16_t)x, (uint16_t)y, 2, (uint16_t)h, 0x3B82F6u);

    for (int row = 0; row < 3; row++) {
        vbe_draw_text_px((uint16_t)(x + 8), (uint16_t)(y + row * 32),
                         lines[row], 0xE6EDF3u, 0x0B1016u);
    }

    vbe_swap_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
}

void debug_tick(void) {
    uint64_t now = debug_rdtsc();
    if (g_last_tick != 0 && now - g_last_tick < DBG_TICK_CYCLES) return;
    g_last_tick = now;

    debug_serial_flush();

    if (!g_hud_on) return;

    uint32_t cpu = hud_cpu_from_idle(now);
    char lines[3][28];
    hud_build(lines, cpu);
    hud_render_fb(lines);
}

void debug_hud_gui_tick(void) {
    if (!g_hud_on) return;

    static uint64_t last;
    uint64_t now = debug_rdtsc();
    if (last != 0 && now - last < DBG_TICK_CYCLES) return;
    last = now;

    uint32_t cpu = gui_system_cpu_permille() / 10u;
    char lines[3][28];
    hud_build(lines, cpu);
    hud_render_buf(lines);

    debug_serial_flush();
}

int debug_hud_enabled(void) {
    return g_hud_on;
}

int debug_serial_enabled(void) {
    return g_serial_on;
}

int debug_auto_format_allowed(void) {
    return !g_safe_format;
}

/* ============================================================
 * 早期调试控制台
 * ============================================================ */

#define DBG_LINE_MAX 96

static int dbg_getkey(void) {
    char c = kb_read_char();
    if (c) {
        if (c == '\r') c = '\n';
        return (int)(uint8_t)c;
    }

    /* COM1 轮询输入：方便无头环境/工业设备用串口操作。 */
    if (inb(0x3FD) & 0x01u) {
        int s = (int)inb(0x3F8);
        if (s == '\r') s = '\n';
        if (s == 0x7F) s = '\b';
        return s;
    }
    return 0;
}

static void con_header(void) {
    tty_set_color(0x0E);
    printk("============================================================\n");
    printk(" SAYLTEROS DEBUG CONSOLE  (pre-SlyzFS)\n");
    printk("============================================================\n");
    tty_set_color(0x0F);
    printk(" SlyerType : %s\n", slyertype_status());
    printk(" Faces     : %d\n", slyertype_face_count());
    printk(" SlyzFS    : NOT loaded in this stage\n");
    printk(" Storage   : not probed (use 'disk' for read-only probe)\n");
    printk("\n");
    printk(" 'help' commands, 'next' continues normal boot.\n");
    printk("\n");
}

static void con_settings(void) {
    printk(" seriallog : %s\n", g_serial_on ? "ON  -> system\\logs\\debug-serial.log" : "OFF");
    printk(" hud       : %s\n", g_hud_on ? "ON  -> top-right overlay" : "OFF");
    printk(" safeformat: %s\n", g_safe_format ? "ON  (never auto-format)" : "OFF (mount failure may format)");
}

static void cmd_help(void) {
    printk(" next                 continue normal kernel boot\n");
    printk(" settings             show debug settings\n");
    printk(" seriallog on|off     mirror serial output to current boot disk\n");
    printk(" hud on|off           persistent memory/CPU/disk overlay\n");
    printk(" safeformat on|off    disable/enable auto-format after mount failure\n");
    printk(" disk                 probe block backend and read LBA0 (read-only)\n");
    printk(" selftest             block read + font coverage self test\n");
    printk(" mem                  kernel heap statistics\n");
    printk(" font                 SlyerType status\n");
    printk(" log                  flush pending serial log\n");
    printk(" clear                clear screen\n");
    printk(" reboot               hard reset\n");
    printk(" halt                 stop the CPU\n");
}

static void cmd_disk(void) {
    static uint8_t probe[512] __attribute__((aligned(512)));

    blk_init();
    printk(" backend : %s\n", blk_backend());
    printk(" present : %s\n", blk_present() ? "yes" : "no");
    printk(" sectors : %u\n", blk_total_sectors());

    if (blk_present()) {
        if (blk_read(0, probe)) {
            uint32_t sum = 0;
            for (int i = 0; i < 512; i++) sum += probe[i];
            printk(" read LBA0: OK (checksum 0x%x)\n", sum);
        } else {
            printk(" read LBA0: FAILED\n");
        }
    }
}

static void cmd_selftest(void) {
    int ok = 1;

    blk_init();
    printk(" block backend : %s\n", blk_backend());
    if (!blk_present()) {
        printk(" block present : FAIL\n");
        ok = 0;
    } else {
        static uint8_t probe[512] __attribute__((aligned(512)));
        int r = blk_read(0, probe);
        printk(" block read    : %s\n", r ? "PASS" : "FAIL");
        if (!r) ok = 0;
    }

    int fa = slyertype_covers('A');
    int fc = slyertype_covers(0x4E2Du);   /* 中 */
    printk(" font ASCII    : %s\n", fa ? "PASS" : "FAIL");
    printk(" font CJK      : %s\n", fc ? "PASS" : "SKIP (no CJK face yet)");

    printk(" result        : %s\n", ok ? "PASS" : "FAIL");
}

static void cmd_mem(void) {
    uint32_t total = 0, used = 0, peak = 0, allocs = 0, frees = 0, failed = 0;
    mem_stats(&total, &used, &peak, &allocs, &frees, &failed);
    printk(" heap total : %u bytes\n", total);
    printk(" used       : %u bytes\n", used);
    printk(" free       : %u bytes\n", total - used);
    printk(" peak       : %u bytes\n", peak);
    printk(" allocs     : %u  frees: %u  failed: %u\n", allocs, frees, failed);
}

static void cmd_font(void) {
    printk(" status : %s\n", slyertype_status());
    printk(" faces  : %d\n", slyertype_face_count());
    printk(" covers : ASCII=%s  CJK=%s\n",
           slyertype_covers('A') ? "yes" : "no",
           slyertype_covers(0x4E2Du) ? "yes" : "no");
}

static void cmd_halt(void) {
    printk(" system halted. power cycle to continue.\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* 返回 1 表示离开控制台，继续正常启动。 */
static int handle_command(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return 0;

    const char *arg = line;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    int cmdlen = (int)(arg - line);
    while (*arg == ' ' || *arg == '\t') arg++;

    char cmd[24];
    if (cmdlen >= (int)sizeof(cmd)) cmdlen = (int)sizeof(cmd) - 1;
    for (int i = 0; i < cmdlen; i++) cmd[i] = line[i];
    cmd[cmdlen] = '\0';

    if (dbg_streq(cmd, "next") || dbg_streq(cmd, "boot") ||
        dbg_streq(cmd, "continue")) {
        printk(" continuing normal boot...\n");
        return 1;
    }
    if (dbg_streq(cmd, "help") || dbg_streq(cmd, "?")) {
        cmd_help();
        return 0;
    }
    if (dbg_streq(cmd, "settings")) {
        con_settings();
        return 0;
    }
    if (dbg_streq(cmd, "seriallog")) {
        if (dbg_streq(arg, "on")) {
            g_serial_on = 1;
            printk(" seriallog ON: buffering, will write after SlyzFS mount\n");
        } else if (dbg_streq(arg, "off")) {
            g_serial_on = 0;
            printk(" seriallog OFF\n");
        } else {
            printk(" usage: seriallog on|off\n");
        }
        return 0;
    }
    if (dbg_streq(cmd, "hud")) {
        if (dbg_streq(arg, "on")) {
            g_hud_on = 1;
            printk(" hud ON: overlay starts after normal boot\n");
        } else if (dbg_streq(arg, "off")) {
            g_hud_on = 0;
            printk(" hud OFF\n");
        } else {
            printk(" usage: hud on|off\n");
        }
        return 0;
    }
    if (dbg_streq(cmd, "safeformat")) {
        if (dbg_streq(arg, "on")) {
            g_safe_format = 1;
            printk(" safeformat ON: mount failure will NOT format the disk\n");
        } else if (dbg_streq(arg, "off")) {
            g_safe_format = 0;
            printk(" WARNING: safeformat OFF, mount failure may format the disk\n");
        } else {
            printk(" usage: safeformat on|off\n");
        }
        return 0;
    }
    if (dbg_streq(cmd, "disk")) { cmd_disk(); return 0; }
    if (dbg_streq(cmd, "selftest")) { cmd_selftest(); return 0; }
    if (dbg_streq(cmd, "mem")) { cmd_mem(); return 0; }
    if (dbg_streq(cmd, "font")) { cmd_font(); return 0; }
    if (dbg_streq(cmd, "log")) {
        printk(" seriallog=%s fs_ready=%s\n",
               g_serial_on ? "on" : "off",
               g_fs_ready ? "yes" : "no");
        debug_serial_flush();
        return 0;
    }
    if (dbg_streq(cmd, "clear") || dbg_streq(cmd, "cls")) {
        tty_clear();
        con_header();
        return 0;
    }
    if (dbg_streq(cmd, "reboot")) {
        printk(" rebooting...\n");
        reboot();
        return 0;
    }
    if (dbg_streq(cmd, "halt")) { cmd_halt(); return 0; }

    printk(" unknown command: %s (try 'help')\n", cmd);
    return 0;
}

void debug_early_console(void) {
    /* 内嵌字体让调试控制台也使用平滑字形；此阶段不访问 SlyzFS。 */
    (void)slyertype_init_early();

    tty_clear();
    con_header();
    con_settings();
    printk("\n");

    char line[DBG_LINE_MAX];
    int len = 0;
    int boot = 0;

    tty_set_color(0x0A);
    printk("dbg> ");
    tty_set_color(0x0F);

    while (!boot) {
        int c = dbg_getkey();
        if (c == 0) {
            debug_serial_flush();
            continue;
        }

        if (c == '\n' || c == '\r') {
            tty_putchar('\n');
            line[len] = '\0';
            boot = handle_command(line);
            len = 0;
            if (!boot) {
                tty_set_color(0x0A);
                printk("dbg> ");
                tty_set_color(0x0F);
            }
            continue;
        }
        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                printk("\b \b");
            }
            continue;
        }
        if (c == 0x0C) {          /* Ctrl+L */
            tty_clear();
            con_header();
            tty_set_color(0x0A);
            printk("dbg> ");
            tty_set_color(0x0F);
            len = 0;
            continue;
        }
        if (c >= 32 && c < 127) {
            if (len < DBG_LINE_MAX - 1) {
                line[len++] = (char)c;
                tty_putchar((char)c);
            }
        }
    }

    tty_set_color(0x0F);
}

#endif /* SAYLTER_DEBUG */
