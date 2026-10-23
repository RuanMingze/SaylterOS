/* ============================================================
 * shell.c - SaylterOS Shell
 * ------------------------------------------------------------
 * 打印 banner + prompt + readline + 命令解析 + Levenshtein 纠错
 * ============================================================ */

#include <stdint.h>
#include "tty.h"
#include "keyboard.h"
#include "io.h"
#include "vbe.h"

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
static void cmd_gui(void) {
    tty_clear();
    tty_set_cursor(0, 0);
    printk("[GUI] VBE mode init: 1024x768x32\n");
    if (!vbe_set_mode(VBE_WIDTH, VBE_HEIGHT, VBE_BPP)) {
        printk("[GUI] Unsupported mode.\n");
        return;
    }
    vbe_fill_sky_blue();
    printk("[GUI] Sky-blue desktop ready.\n");
}
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
    else if (cmd_strcmp(cmd, "gui") == 0)   cmd_gui();
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
