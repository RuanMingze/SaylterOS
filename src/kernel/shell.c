/* ============================================================
 * shell.c - SaylterOS Shell
 * ------------------------------------------------------------
 * 打印 banner + prompt + readline + 命令解析 + Levenshtein 纠错
 * ============================================================ */

#include <stdint.h>
#include "tty.h"
#include "keyboard.h"
#include "io.h"
#ifdef SAYLTER_VBE
#include "vbe.h"
#include "mouse.h"
#endif

/* MinGW 栈探针 stub —— 裸内核没有 libgcc 的 ___chkstk_ms */
void ___chkstk_ms(void) {}
void __chkstk_ms(void)  {}

/* 简易 delay —— 空循环 */
static void delay(uint32_t count) {
    while (count--) __asm__ volatile ("nop");
}

/* Banner */
static const char banner[] =
    "  _____              _ _          _____ _____ ______ \n"
    " / ____|            | | |        / ____| ____|  ____|\n"
    "| (___   __ _  ___  | | |_ ___  | (___ | |__ | |__   \n"
    " \\___ \\ / _` |/ _ \\ | | __/ _ \\  \\___ \\|  __||  __|  \n"
    " ____) | (_| |  __/ | | || (_) | ____) | |___| |     \n"
    "|_____/ \\__,_|\\___| |_|\\__\\___/ |_____/|_____|_|     \n"
    "\n"
    "  [ SAYLTER OPERATING SYSTEM ] v0.3 - TTY Shell\n"
    "\n";

static const char *help_text[] = {
    "  help        Show this help",
    "  min         Show minimal builtin commands",
    "  clear       Clear screen",
    "  echo <msg>  Print a message",
    "  gui         Enter VBE GUI mode (sky blue)",
    "  ps          List processes (empty)",
    "  version     Show OS version",
    "  uname       Show kernel info",
    "  whoami      Show current user",
    "  date        Show fake system date",
    "  mem         Show static memory summary",
    "  reboot      Halt (manual reset)",
    0
};

#define SHELL_LINE_WIDTH 80

/* ---------- 行编辑 ---------- */

static void redraw_line(char *buf, int len, int pos, uint16_t prompt_x) {
    uint16_t *video = (uint16_t *)0xB8000;
    uint16_t row = tty_get_cursor_y();

    for (int col = prompt_x; col < SHELL_LINE_WIDTH; col++) {
        video[row * SHELL_LINE_WIDTH + col] = (0x0F << 8) | ' ';
    }

    tty_set_cursor(prompt_x, row);
    for (int i = 0; i < len; i++) {
        tty_putchar(buf[i]);
    }

    tty_set_cursor(prompt_x + pos, row);
}

static void readline(char *buf, int maxlen, uint16_t prompt_x) {
    int pos = 0;
    int len = 0;
    uint8_t insert_mode = 1;

    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            delay(50000);
            continue;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            tty_putchar('\n');
            return;
        }

        if (c == (char)0xF0) {
            insert_mode = !insert_mode;
            tty_set_cursor_shape(insert_mode ? 0x0F : 0x06, insert_mode ? 0x0F : 0x06);
            tty_set_cursor(prompt_x + pos, tty_get_cursor_y());
            continue;
        }

        if (c == (char)0xF1) {
            if (pos > 0) {
                pos--;
                tty_set_cursor(prompt_x + pos, tty_get_cursor_y());
            }
            continue;
        }

        if (c == (char)0xF2) {
            if (pos < len) {
                pos++;
                tty_set_cursor(prompt_x + pos, tty_get_cursor_y());
            }
            continue;
        }

        if (c == '\b') {
            if (pos > 0) {
                pos--;
                for (int i = pos; i < len - 1; i++) {
                    buf[i] = buf[i + 1];
                }
                len--;
                buf[len] = '\0';
                redraw_line(buf, len, pos, prompt_x);
            }
            continue;
        }

        if (c == (char)0xF5) {
            if (pos < len) {
                for (int i = pos; i < len - 1; i++) {
                    buf[i] = buf[i + 1];
                }
                len--;
                buf[len] = '\0';
                redraw_line(buf, len, pos, prompt_x);
            }
            continue;
        }

        if (c < 0x20) {
            continue;
        }

        if (len < maxlen - 1) {
            if (insert_mode && pos < len) {
                for (int i = len; i > pos; i--) {
                    buf[i] = buf[i - 1];
                }
                buf[pos] = c;
                len++;
                pos++;
                redraw_line(buf, len, pos, prompt_x);
            } else {
                buf[pos] = c;
                pos++;
                if (pos > len) len = pos;
                buf[len] = '\0';
                tty_putchar(c);
            }
        }
    }
}

/* ---------- 字符串工具 ---------- */

static int cmd_strcmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if ((ca | 32) != (cb | 32)) return (ca | 32) - (cb | 32);
        a++; b++;
    }
    return ((*a | 32) - (*b | 32));
}

static int cmd_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ---------- Levenshtein 距离 ---------- */

static int levenshtein(const char *a, const char *b) {
    int la = cmd_strlen(a);
    int lb = cmd_strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    int prev[32], curr[32];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int x = prev[j] + 1;
            int y = curr[j-1] + 1;
            int z = prev[j-1] + cost;
            int m = (x < y) ? x : y;
            if (z < m) m = z;
            curr[j] = m;
        }
        for (int k = 0; k <= lb; k++) prev[k] = curr[k];
    }
    return prev[lb];
}

/* ---------- 内置命令表 ---------- */

static const char *builtin_cmds[] = {
    "help","min","clear","cls","echo","gui","version","uname",
    "whoami","date","mem","reboot","ps", 0
};

/* 找最接近的内置命令，仅返回编辑距离 <= 2 的 */
static const char *find_closest(const char *cmd) {
    const char *best = 0;
    int best_d = 3;  /* 只接受距离 <= 2 */
    for (int i = 0; builtin_cmds[i]; i++) {
        int d = levenshtein(cmd, builtin_cmds[i]);
        if (d < best_d) { best_d = d; best = builtin_cmds[i]; }
    }
    return best;
}

/* ---------- CMOS RTC 读真实时间 ---------- */

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}
/* BCD → 十进制 */
static int bcd2dec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }
/* 读 RTC，返回时等更新完成、处理 BCD/二进制、推算 century */
static void rtc_get(int *out_year, int *out_mon, int *out_day,
                    int *out_hour, int *out_min, int *out_sec) {
    /* 等待 RTC 更新周期结束 (bit 7 of status A = update in progress) */
    while (cmos_read(0x0A) & 0x80) {}

    uint8_t reg_b = cmos_read(0x0B);
    int bcd = !(reg_b & 0x04);  /* bit2=1 是二进制模式, 0 是 BCD (默认 BCD) */

    int s  = cmos_read(0x00);
    int mi = cmos_read(0x02);
    int h  = cmos_read(0x04);
    int d  = cmos_read(0x07);
    int mo = cmos_read(0x08);
    int y  = cmos_read(0x09);

    if (bcd) { s=bcd2dec(s); mi=bcd2dec(mi); h=bcd2dec(h);
               d=bcd2dec(d); mo=bcd2dec(mo); y=bcd2dec(y); }

    /* Century: 先试 0x32（有 BIOS 写入），否则按年份推算 */
    int century = (y <= 70) ? 2000 : 1900;
    uint8_t c = cmos_read(0x32);
    if (c != 0xFF) {
        int ci = bcd ? bcd2dec(c) : c;
        if (ci >= 19 && ci <= 20) century = ci * 100;
    }

    *out_year = century + y;
    *out_mon  = mo;
    *out_day  = d;
    *out_hour = h;
    *out_min  = mi;
    *out_sec  = s;
}
/* 把 1 位/2 位数字补零写成 2 字符 + \0，打印 %s 即可 */
static void pad2(char out[3], int v) {
    out[0] = '0' + (v / 10);
    out[1] = '0' + (v % 10);
    out[2] = '\0';
}

/* ---------- 命令实现 ---------- */

static void cmd_help(void)  { for (int i = 0; help_text[i]; i++) printk("%s\n", help_text[i]); }
static void cmd_min(void)  { printk("help clear echo gui ps version uname whoami date mem reboot\n"); }
static void cmd_clear(void){ tty_clear(); }
static void cmd_echo(const char *args)  { printk("%s\n", args); }
#ifdef SAYLTER_VBE
/* --- 按钮定义 --- */
#define BTN_X       432
#define BTN_Y       320
#define BTN_W       144
#define BTN_H       64
#define BTN_CX      28        /* 按钮中心504px → cx=27.5, 取28(偏右8px对称) */
#define BTN_CY      10        /* 按钮中心352px → cy=10.5, 取10(偏上) → 视觉居中 */
#define BTN_NORMAL  0xFF3366AA
#define BTN_HOVER   0xFF224488
#define BTN_TEXT    0xFFFFFFFF

void gui_main(void) {
    tty_clear();
    tty_set_cursor(0, 0);
    vbe_init();
    if (!vbe_is_enabled()) {
        printk("[GUI] VBE not available.\n");
        return;
    }
    printk("[GUI] %ux%ux%u — poweroff to exit.\n",
           vbe_current.width, vbe_current.height, vbe_current.bpp);

    if (!ps2_mouse_init()) {
        printk("[GUI] PS/2 mouse init failed.\n");
        return;
    }
    int16_t mx = (int16_t)(vbe_current.width / 2);
    int16_t my = (int16_t)(vbe_current.height - 100);
    const int16_t max_x = (int16_t)(vbe_current.width - MOUSE_W);
    const int16_t max_y = (int16_t)(vbe_current.height - MOUSE_HEIGHT);

    int16_t prev_mx = mx, prev_my = my;
    uint8_t prev_hit = 0;
    uint8_t prev_buttons = 0;
    uint32_t poll_fail = 0;
    uint32_t click_count = 0;
    uint32_t prev_click_count = 0;

    /* ===== 第一帧：画全屏 + 全 swap ===== */
    vbe_fill_sky_blue();
    vbe_draw_string("SaylterOS", 27, 8, 0xFF0000FF, 0x00BFE8FF);
    vbe_draw_string("v0.3 - VGA ROM Font", 22, 12, 0xFFFF0000, 0x00BFE8FF);
    vbe_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, BTN_NORMAL);
    vbe_draw_string("Click Me", BTN_CX, BTN_CY, BTN_TEXT, BTN_NORMAL);
    vbe_draw_string("Clicks: 0", BTN_X/16, (BTN_Y + BTN_H) / 32 + 1, 0xFFFFFFFF, 0x00BFE8FF);
    vbe_fill_rect((uint16_t)mx, (uint16_t)my, MOUSE_W, MOUSE_HEIGHT, 0xFF000000);
    vbe_swap();

    while (1) {
        int8_t dx, dy;
        uint8_t buttons;
        if (ps2_mouse_poll(&dx, &dy, &buttons)) {
            poll_fail = 0;
            mx += dx; my += dy;
            if (mx < 0) mx = 0;      if (my < 0) my = 0;
            if (mx > max_x) mx = max_x; if (my > max_y) my = max_y;
        } else {
            poll_fail++;
            if (poll_fail > 5000000) {
                ps2_mouse_disable();
                ps2_mouse_init();
                poll_fail = 0;
            }
            continue;
        }

        uint8_t hit = point_in_rect(mx, my, BTN_X, BTN_Y, BTN_W, BTN_H);

        if (!(prev_buttons & 1) && (buttons & 1) && hit) {
            click_count++;
        }
        prev_buttons = buttons;

        /* 静止不动 → 完全跳过重绘和 swap */
        if (mx == prev_mx && my == prev_my && hit == prev_hit && click_count == prev_click_count) continue;

        /* === 计算脏矩形 bounding box === */
        uint16_t dx1 = prev_mx, dy1 = prev_my;
        uint16_t dx2 = (uint16_t)(prev_mx + MOUSE_W - 1), dy2 = (uint16_t)(prev_my + MOUSE_HEIGHT - 1);
        uint16_t nx1 = (uint16_t)mx, ny1 = (uint16_t)my;
        uint16_t nx2 = (uint16_t)(mx + MOUSE_W - 1), ny2 = (uint16_t)(my + MOUSE_HEIGHT - 1);
        uint16_t bbox_x1 = dx1 < nx1 ? dx1 : nx1;
        uint16_t bbox_y1 = dy1 < ny1 ? dy1 : ny1;
        uint16_t bbox_x2 = dx2 > nx2 ? dx2 : nx2;
        uint16_t bbox_y2 = dy2 > ny2 ? dy2 : ny2;
        /* hover 状态变了 → 整个按钮区域也要重绘 */
        if (hit != prev_hit) {
            if (BTN_X < bbox_x1) bbox_x1 = BTN_X;
            if (BTN_Y < bbox_y1) bbox_y1 = BTN_Y;
            if ((BTN_X + BTN_W - 1) > bbox_x2) bbox_x2 = BTN_X + BTN_W - 1;
            if ((BTN_Y + BTN_H - 1) > bbox_y2) bbox_y2 = BTN_Y + BTN_H - 1;
        }
        /* click_count 变了 → counter 区域也要重绘（按钮下方一行） */
        if (click_count != prev_click_count) {
            uint16_t ccy = (BTN_Y + BTN_H) + 32; /* 按钮底 + 一行像素 32px */
            uint16_t ccx1 = BTN_X;
            uint16_t ccx2 = BTN_X + BTN_W - 1;
            uint16_t ccy1 = ccy;
            uint16_t ccy2 = ccy + 31;
            if (ccx1 < bbox_x1) bbox_x1 = ccx1;
            if (ccy1 < bbox_y1) bbox_y1 = ccy1;
            if (ccx2 > bbox_x2) bbox_x2 = ccx2;
            if (ccy2 > bbox_y2) bbox_y2 = ccy2;
        }

        prev_mx = mx; prev_my = my; prev_hit = hit; prev_click_count = click_count;

        /* ===== 全量重绘 backbuffer（RAM 写，飞快） ===== */
        vbe_fill_sky_blue();
        vbe_draw_string("SaylterOS", 27, 8, 0xFF0000FF, 0x00BFE8FF);
        vbe_draw_string("v0.3 - VGA ROM Font", 22, 12, 0xFFFF0000, 0x00BFE8FF);
        uint32_t btn_color = hit ? BTN_HOVER : BTN_NORMAL;
        vbe_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, btn_color);
        vbe_draw_string("Click Me", BTN_CX, BTN_CY, BTN_TEXT, btn_color);

        /* counter */
        char counter[16];
        counter[0] = 'C'; counter[1] = 'l'; counter[2] = 'i'; counter[3] = 'c';
        counter[4] = 'k'; counter[5] = 's'; counter[6] = ':'; counter[7] = ' ';
        int pos = 8;
        if (click_count == 0) {
            counter[pos++] = '0';
        } else {
            char tmp[10]; int t = 0;
            uint32_t c = click_count;
            while (c) { tmp[t++] = '0' + (c % 10); c /= 10; }
            while (t--) counter[pos++] = tmp[t];
        }
        counter[pos] = '\0';
        int clen = pos;
        int btn_chars_x = BTN_X / 16;
        int btn_chars_w = BTN_W / 16;
        int cx = btn_chars_x + (btn_chars_w - clen) / 2;
        if (cx < 0) cx = 0;
        int cy = (BTN_Y + BTN_H) / 32 + 1;
        vbe_draw_string(counter, cx, cy, 0xFFFFFFFF, 0x00BFE8FF);

        vbe_fill_rect((uint16_t)mx, (uint16_t)my, MOUSE_W, MOUSE_HEIGHT, 0xFF000000);

        /* ===== 只 swap 脏矩形 bounding box（可能只有几百字节） ===== */
        uint16_t bw = bbox_x2 - bbox_x1 + 1;
        uint16_t bh = bbox_y2 - bbox_y1 + 1;
        vbe_swap_rect(bbox_x1, bbox_y1, bw, bh);
    }

    ps2_mouse_disable();
}
#endif
static void cmd_version(void) { printk("SaylterOS v0.3 (TTY shell)\n"); }
static void cmd_uname(void)   { printk("SaylterOS i386 32-bit\n"); }
static void cmd_whoami(void)  { printk("root\n"); }
static void cmd_date(void) {
    int y, mo, d, h, mi, s;
    rtc_get(&y, &mo, &d, &h, &mi, &s);
    char Y[5], MO[3], D[3], H[3], MI[3], S[3];
    Y[0]='0'+(y/1000); Y[1]='0'+((y/100)%10);
    Y[2]='0'+((y/10)%10); Y[3]='0'+(y%10); Y[4]='\0';
    pad2(MO, mo); pad2(D, d); pad2(H, h); pad2(MI, mi); pad2(S, s);
    printk("%s-%s-%s %s:%s:%s UTC\n", Y, MO, D, H, MI, S);
}
static void cmd_mem(void)     { printk("kernel @ 0xA100, conventional 640KB\n"); }
static void cmd_reboot(void)  { printk("HALTED — manual reset required\n"); while (1) __asm__ volatile ("cli; hlt"); }
static void cmd_ps(void)      { printk("PID  STATE  NAME\n  0  RUN     idle\n  1  RUN     shell\n"); }

/* 执行命令，返回 1 表示执行了，0 表示未找到 */
static int exec_cmd(const char *cmd, char *args) {
    if (cmd_strcmp(cmd, "help") == 0)      cmd_help();
    else if (cmd_strcmp(cmd, "min") == 0)   cmd_min();
    else if (cmd_strcmp(cmd, "clear") == 0) cmd_clear();
    else if (cmd_strcmp(cmd, "cls") == 0)   cmd_clear();
    else if (cmd_strcmp(cmd, "echo") == 0)  cmd_echo(args);
#ifdef SAYLTER_VBE
    else if (cmd_strcmp(cmd, "gui") == 0)   gui_main();
#endif
    else if (cmd_strcmp(cmd, "version") == 0) cmd_version();
    else if (cmd_strcmp(cmd, "uname") == 0) cmd_uname();
    else if (cmd_strcmp(cmd, "whoami") == 0) cmd_whoami();
    else if (cmd_strcmp(cmd, "date") == 0)  cmd_date();
    else if (cmd_strcmp(cmd, "mem") == 0)   cmd_mem();
    else if (cmd_strcmp(cmd, "reboot") == 0) cmd_reboot();
    else if (cmd_strcmp(cmd, "ps") == 0)    cmd_ps();
    else return 0;
    return 1;
}

/* ---------- 是/否确认 ---------- */
/* 读取单个字符，y 返回 1，其他返回 0 */
static int read_yes_no(void) {
    while (1) {
        char c = kb_read_char();
        if (c == 0) { delay(50000); continue; }
        if (c == '\n' || c == '\r' || c == 'n' || c == 'N') {
            printk("n\n");
            return 0;
        }
        if (c == 'y' || c == 'Y') {
            printk("y\n");
            return 1;
        }
    }
}

/* ---------- Shell 主循环 ---------- */

void shell_main(void) {
    static const char prompt[] = "saylter@os:~$";

    printk(banner);
    printk("SaylterOS TTY Shell — type 'help' for commands\n");
    printk("Insert toggles edit mode, arrows move cursor, Backspace/Delete work\n\n");

    char line[256];
    while (1) {
        printk(prompt);
        printk(" ");

        uint16_t prompt_x = tty_get_cursor_x();
        readline(line, sizeof(line), prompt_x);

        /* 空命令跳过 */
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (*cmd == '\0') continue;

        /* 切分 cmd + args */
        char *args = cmd;
        while (*args && *args != ' ' && *args != '\t') args++;
        if (*args) {
            *args++ = '\0';
            while (*args == ' ' || *args == '\t') args++;
        }

        /* 先尝试直接执行 */
        if (exec_cmd(cmd, args)) continue;

        /* 未找到 → 试试纠错 */
        const char *guess = find_closest(cmd);
        if (guess) {
            printk("saylter: '%s' is not a command. Did you mean '%s'? [y/N] ", cmd, guess);
            if (read_yes_no()) {
                exec_cmd(guess, args);
            }
        } else {
            printk("saylter: command not found: %s\n", cmd);
        }
    }
}
