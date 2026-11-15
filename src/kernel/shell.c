/* ============================================================
 * shell.c - SaylterOS Shell
 * ------------------------------------------------------------
 * 打印 banner + prompt + readline + 命令解析 + Levenshtein 纠错
 * ============================================================ */

#include <stdint.h>
#include "tty.h"
#include "keyboard.h"
#include "io.h"
#include "reboot.h"
#include "editor.h"
#include "acpi.h"
#include "auth.h"

/* ---- SlyzFS 目录项镜像结构 ----
 * 必须与 slyzfs.h 的 DirEntry 以及 bridge 层的 SlyzDirEntry 逐字节一致：
 * 71 = 1(type) + 52(name) + 2(start_cluster) + 4(size) + 4(created) + 4(modified) + 4(reserved)。
 * 不加 pack(1) 时编译器会按 4 字节对齐把结构撑到 72，数组下标访问会整体错位。 */
#pragma pack(push, 1)
struct SlyzDirEntry {
    uint8_t  type;
    uint8_t  name[52];
    uint16_t start_cluster;
    uint32_t size;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
};
#pragma pack(pop)

#define SLYZFS_DIR_FILE 0x01
#define SLYZFS_DIR_DIR  0x02

extern int slyzfs_try_mount(void);
extern int slyzfs_format_and_mount(void);
extern int slyzfs_mkdir(const char* path);
extern int slyzfs_rmdir(const char* path);
extern int slyzfs_list_dir(const char* path, struct SlyzDirEntry* out, int out_max);
extern int slyzfs_stat(const char* path, struct SlyzDirEntry* out);
extern int slyzfs_create(const char* path);
extern int slyzfs_remove(const char* path);
extern int slyzfs_mklazy(void);
/* 静默开关：启动页打印期间完成挂载，避免 [SlyzFS] 日志打乱启动画面 */
extern void slyzfs_set_quiet(int quiet);
/* 权限接口：登录层通过这些调用切换操作者身份 */
extern void slyzfs_set_uid(int uid);
extern int  slyzfs_get_uid(void);
extern int  slyzfs_chmod(const char* path, int mode);
extern int  slyzfs_chown(const char* path, int uid);
extern void* slyzfs_open(const char* path, const char* mode);
extern int slyzfs_read(void* fh, uint8_t* buf, int n);
extern int slyzfs_write(void* fh, const uint8_t* buf, int n);
extern void slyzfs_close(void* fh);
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

/* ---------- 简易磁盘测试（ATA PIO + 内存回退） ---------- */
#define ATA_DATA_PORT    0x1F0
#define ATA_ERR_PORT     0x1F1
#define ATA_SECCNT_PORT  0x1F2
#define ATA_LBA_LOW_PORT 0x1F3
#define ATA_LBA_MID_PORT 0x1F4
#define ATA_LBA_HIGH_PORT 0x1F5
#define ATA_DRIVE_PORT   0x1F6
#define ATA_CMD_PORT     0x1F7
#define ATA_CTRL_PORT    0x3F6
#define DISK_TEST_LBA    2048u
#define DISK_MAX_SECTORS 65536u

static uint8_t disk_test_shadow = 0;

static void serial_putc(char c) {
    while ((inb(0x3FD) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
}

static void serial_print(const char *s) {
    while (*s) serial_putc(*s++);
}

void kserial_putchar(char c) { serial_putc((unsigned char)c); }
void kserial_puts(const char *s) { serial_print(s); }
void kserial_dec32(uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}
void kserial_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) {
        serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

static void serial_hex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    serial_putc(hex[(v >> 4) & 0xF]);
    serial_putc(hex[v & 0xF]);
}

static int ata_wait_busy_clear(uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint8_t status = inb(ATA_CMD_PORT);
        if ((status & 0x80) == 0) return 1;
        delay(1000);
    }
    serial_print("[ATA] busy timeout\n");
    return 0;
}

static int ata_wait_drq(uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint8_t status = inb(ATA_CMD_PORT);
        if (status & 0x08) return 1;
        if (status & 0x01) {
            serial_print("[ATA] error status=");
            serial_hex8(status);
            serial_putc('\n');
            return 0;
        }
        delay(1000);
    }
    serial_print("[ATA] drq timeout\n");
    return 0;
}

static int ata_disk_present(void) {
    /* 探测结果缓存：该函数位于每次扇区读写的关键路径上，
     * 若每次都在这里做 1000 次空转的 delay，扫描数万扇区会慢到不可用。
     * 硬盘在位状态在系统生命周期内不会变化，只探测一次即可。 */
    static int cached = -1;
    if (cached >= 0) return cached;
    outb(ATA_DRIVE_PORT, 0xA0);
    delay(1000);
    uint8_t status = inb(ATA_CMD_PORT);
    (void)inb(ATA_LBA_MID_PORT);
    (void)inb(ATA_LBA_HIGH_PORT);
    cached = (status != 0x00 && status != 0xFF);
    return cached;
}

/* 注意：ATA 读写是高频路径（扫描可执行文件时单条命令可达 6 万次扇区访问），
 * 绝不能在成功路径上打印串口日志 —— 9600 波特率下会把 3 秒的操作拖成 77 分钟，
 * 表现为"任意命令都卡死"。仅在失败时输出诊断信息。 */
int ata_read_sector(uint32_t lba, uint8_t *buf) {
    if (!ata_disk_present()) return 0;
    if (!ata_wait_busy_clear(20000)) return 0;

    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERR_PORT, 0x00);
    outb(ATA_SECCNT_PORT, 0x01);
    outb(ATA_LBA_LOW_PORT, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID_PORT, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH_PORT, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD_PORT, 0x20);

    if (!ata_wait_drq(20000)) {
        serial_print("[ATA] read drq timeout, lba=");
        kserial_hex32(lba);
        serial_putc('\n');
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_DATA_PORT);
        buf[i * 2] = (uint8_t)(word & 0xFF);
        buf[i * 2 + 1] = (uint8_t)((word >> 8) & 0xFF);
    }
    return 1;
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    if (!ata_disk_present()) return 0;
    if (!ata_wait_busy_clear(20000)) return 0;

    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERR_PORT, 0x00);
    outb(ATA_SECCNT_PORT, 0x01);
    outb(ATA_LBA_LOW_PORT, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID_PORT, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH_PORT, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD_PORT, 0x30);

    if (!ata_wait_drq(20000)) {
        serial_print("[ATA] write drq timeout, lba=");
        kserial_hex32(lba);
        serial_putc('\n');
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8);
        outw(ATA_DATA_PORT, word);
    }
    outb(ATA_CMD_PORT, 0xE7);
    if (!ata_wait_busy_clear(20000)) {
        serial_print("[ATA] write flush timeout, lba=");
        kserial_hex32(lba);
        serial_putc('\n');
        return 0;
    }
    return 1;
}

uint32_t ata_total_sectors(void) {
    return 32u * 1024u * 1024u / 512u;
}

static int disk_write_test_value(uint32_t sector, uint8_t value) {
    if (sector >= DISK_MAX_SECTORS) return 0;
    uint8_t block[512];
    for (int i = 0; i < 512; i++) block[i] = 0;
    block[0] = value;
    if (!ata_write_sector(sector, block)) return 0;
    disk_test_shadow = value;
    return 1;
}

static int disk_read_test_value(uint32_t sector, uint8_t *out_value) {
    if (sector >= DISK_MAX_SECTORS) return 0;
    uint8_t block[512];
    for (int i = 0; i < 512; i++) block[i] = 0;
    if (!ata_read_sector(sector, block)) return 0;
    *out_value = block[0];
    disk_test_shadow = block[0];
    return 1;
}

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s || *s == '\0') return 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < '0' || c > '9') return 0;
        uint32_t digit = (uint32_t)(c - '0');
        if (v > (UINT32_MAX / 10u)) return 0;
        v = v * 10u + digit;
        s++;
    }
    *out = v;
    return 1;
}

static int parse_u8(const char *s, uint8_t *out) {
    uint32_t v;
    if (!parse_u32(s, &v) || v > 255u) return 0;
    *out = (uint8_t)v;
    return 1;
}

static void split_first_arg(char *args, char **first, char **rest) {
    char *p = args;
    while (*p == ' ' || *p == '\t') p++;
    *first = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p == '\0') {
        *rest = p;
        return;
    }
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;
    *rest = p;
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
    "  help                            Show this help",
    "  min                             Show minimal builtin commands",
    "  clear                           Clear screen",
    "  echo <text>                     Print text",
    "  dir / ls                        List current directory (SlyzFS)",
    "  pwd                             Print working directory",
    "  cd <path>                       Change directory (supports .. and /)",
    "  mkdir <path>                    Create a SlyzFS directory",
    "  rmdir <path>                    Remove an empty SlyzFS directory",
    "  run <file.slyz>                 Execute a SLYZ script by name",
    "  nano <file> / edit <file>       Full-screen editor (Ctrl+S save, Ctrl+X exit)",
    "  cat <file>                      Print file contents",
    "  ask <prompt>                    Prompt for yes/no input",
    "  exit / quit                     Exit the shell",
    "  dd if=<src> of=<dst> [bs=] [count=] [seek=] [skip=]  Raw sector / text copy (see 'dd' alone for help)",
    "  remove <start> <end>            Clear sectors",
    "  gui                             Launch the GUI demo",
    "  version                         Show OS version",
    "  uname                           Show kernel info",
    "  whoami                          Show current user",
    "  date                            Show fake system date",
    "  mem                             Show static memory summary",
    "  reboot                          Reset the machine via 8042",
    "  poweroff / shutdown [n]         ACPI S5 shutdown (optional SLP_TYP 0..7)",
    "  acpi                            Show ACPI / PM1 status",
    "  useradd <name>                  Create a user (root only)",
    "  passwd [name]                   Change password (self or root)",
    "  su [name]                       Switch user (default root)",
    "  logout                          End session and show login prompt",
    "  chmod <octal> <path>            Change permissions, e.g. chmod 600 secret.txt",
    "  chown <uid> <path>              Change owner (root only)",
    0
};

#define SHELL_LINE_WIDTH 80
#define SHELL_ROWS 25
#define SHELL_PATH_MAX 256
#define SHELL_HISTORY_CAP 16

static void cmd_strcpy(char *dst, const char *src);
static void cmd_strncpy(char *dst, const char *src, int n);
static int cmd_strcmp(const char *a, const char *b);
static int cmd_strlen(const char *s);
static int try_run_slyz_by_name(const char *name);
static int exec_cmd(const char *cmd, char *args);
static void format_mode_string(char out[11], uint16_t mode, uint8_t is_dir);

static char shell_history[SHELL_HISTORY_CAP][256];
static int shell_history_count = 0;
static int shell_history_index = -1;

/* ---------- 当前工作目录与动态提示符 ----------
 * shell_cwd 使用 SlyzFS 路径约定：反斜杠分隔，空串代表根目录。
 * shell_prompt 由 shell_refresh_prompt() 按 cwd 重新生成，
 * 同时据此动态计算 TTY 底部需要保留几行（安全区），防止大量输出把
 * 提示符顶出屏幕。 */
static char shell_cwd[SHELL_PATH_MAX] = "";
static char shell_prompt[SHELL_PATH_MAX + 32] = "saylter@os:~$";

static void shell_refresh_prompt(void);
static void shell_sync_scroll_zone(void);

static void shell_add_history(const char *text) {
    if (text == 0 || *text == '\0') return;

    for (int i = 0; i < shell_history_count; i++) {
        if (cmd_strcmp(shell_history[i], text) == 0) {
            shell_history_index = shell_history_count;
            return;
        }
    }

    if (shell_history_count < SHELL_HISTORY_CAP) {
        shell_history_count++;
    } else {
        for (int i = 1; i < SHELL_HISTORY_CAP; i++) {
            cmd_strcpy(shell_history[i - 1], shell_history[i]);
        }
    }

    cmd_strcpy(shell_history[shell_history_count - 1], text);
    shell_history_index = shell_history_count;
}

static void shell_load_history(char *buf, int maxlen, int direction) {
    if (shell_history_count == 0) {
        buf[0] = '\0';
        return;
    }

    if (direction > 0) {
        if (shell_history_index >= shell_history_count || shell_history_index < 0) {
            shell_history_index = shell_history_count - 1;
        } else if (shell_history_index > 0) {
            shell_history_index--;
        }
    } else {
        if (shell_history_index < 0) {
            shell_history_index = 0;
        }
        if (shell_history_index + 1 < shell_history_count) {
            shell_history_index++;
        } else {
            shell_history_index = shell_history_count;
            buf[0] = '\0';
            return;
        }
    }

    if (shell_history_index >= shell_history_count || shell_history_index < 0) {
        buf[0] = '\0';
        return;
    }

    cmd_strncpy(buf, shell_history[shell_history_index], maxlen);
}

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

/* 提交当前输入行：
 * readline 把 prompt 和输入画在保留区，这里把它上提到滚动区，
 * 同时向串口补一个换行，保证日志与屏幕历史一致。
 * 不能直接用 tty_putchar('\n')：那会在保留区里触发一次滚动，
 * 把光标拉到滚动区末行却不搬运输入行，结果是旧命令行既不上屏也不被清空。 */
static void shell_commit_input_line(void) {
    kserial_putchar('\n');
    tty_commit_reserved();
}

static void readline(char *buf, int maxlen, uint16_t prompt_x) {
    int pos = 0;
    int len = 0;
    uint8_t insert_mode = 1;

    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            /* 忙等间隙轮询 PM1_STS：主机把电源键实现为 ACPI 关机事件时，
             * 这里能立刻识别并按 S5 断电，而不是被用户当成"没反应"。 */
            if (acpi_poll_power_button()) {
                printk("\n[ACPI] power button: shutting down (S5)\n");
                acpi_power_off();
                /* 走到这里说明硬件忽略了 S5，恢复提示符与已输入内容 */
                printk("[ACPI] S5 request ignored by hardware\n");
                tty_set_cursor(0, (uint16_t)(tty_get_scroll_limit() + 1));
                printk(shell_prompt);
                printk(" ");
                redraw_line(buf, len, pos, prompt_x);
            }
            delay(50000);
            continue;
        }

        if (c == 3) {
            printk("^C");
            buf[0] = '\0';
            shell_commit_input_line();
            return;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            shell_add_history(buf);
            shell_commit_input_line();
            return;
        }

        if (c == (char)0xF0) {
            insert_mode = !insert_mode;
            tty_set_cursor_shape(insert_mode ? 0x0F : 0x06, insert_mode ? 0x0F : 0x06);
            tty_set_cursor(prompt_x + pos, tty_get_cursor_y());
            continue;
        }

        if (c == (char)0xF6) {
            shell_load_history(buf, maxlen, 1);
            len = cmd_strlen(buf);
            pos = len;
            redraw_line(buf, len, pos, prompt_x);
            continue;
        }

        if (c == (char)0xF7) {
            shell_load_history(buf, maxlen, -1);
            len = cmd_strlen(buf);
            pos = len;
            redraw_line(buf, len, pos, prompt_x);
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

        if ((unsigned char)c < 0x20) {
            continue;
        }

        if ((unsigned char)c >= 0x80) {
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

static void cmd_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++)) {}
}

static void cmd_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

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

/* ---------- 动态提示符与 TTY 安全区 ---------- */

/* 根据当前登录用户与 shell_cwd 重建提示符。
 * 根目录显示 "~"，其余显示完整路径；root 用 '#'，普通用户用 '$'。 */
static void shell_refresh_prompt(void) {
    const struct AuthUser *user = auth_current();
    const char *who = (user && user->name[0]) ? user->name : "root";
    const char *suffix = (user && user->uid != 0) ? "$" : "#";
    const char *shown = (shell_cwd[0] == '\0') ? "~" : shell_cwd;

    int pos = 0;
    int limit = (int)sizeof(shell_prompt) - 1;

    while (*who && pos < limit) shell_prompt[pos++] = *who++;
    const char *at = "@os:";
    while (*at && pos < limit) shell_prompt[pos++] = *at++;
    while (*shown && pos < limit) shell_prompt[pos++] = *shown++;
    while (*suffix && pos < limit) shell_prompt[pos++] = *suffix++;

    shell_prompt[pos] = '\0';
    shell_sync_scroll_zone();
}

/* 按提示符的实际显示宽度（含其后一个空格）动态计算需要保留的行数，
 * 把 TTY 滚动区压到保留区之上。提示符变长（cd 进入深层目录）时
 * 保留行数随之增加，保证输入行永远不会被上方的输出滚掉。 */
static void shell_sync_scroll_zone(void) {
    int width = cmd_strlen(shell_prompt) + 1;
    int rows = (width + SHELL_LINE_WIDTH - 1) / SHELL_LINE_WIDTH;
    if (rows < 1) rows = 1;
    if (rows > SHELL_ROWS - 2) rows = SHELL_ROWS - 2;
    tty_set_scroll_limit((uint16_t)(SHELL_ROWS - 1 - rows));
}

static int cmd_strncmp(const char *a, const char *b, int n);

static int leading_indent(const char *s) {
    int n = 0;
    while (s && (s[n] == ' ' || s[n] == '\t')) n++;
    return n;
}

static char *strip_leading_ws(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int find_matching_endif(char **lines, int count, int start_idx, int current_indent) {
    int depth = 1;
    for (int i = start_idx + 1; i < count; i++) {
        char *trim = strip_leading_ws(lines[i]);
        if (*trim == '\0') continue;

        int indent = leading_indent(lines[i]);
        if (cmd_strncmp(trim, "if ", 3) == 0 && indent > current_indent) {
            depth++;
        } else if (cmd_strcmp(trim, "endif") == 0 && indent <= current_indent) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static int cmd_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '\0' && cb == '\0') return 0;
        if (ca == '\0') return -1;
        if (cb == '\0') return 1;
        if ((ca | 32) != (cb | 32)) return ((ca | 32) - (cb | 32));
    }
    return 0;
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
    "help","min","clear","cls","echo","dir","ls","pwd","cd","run","ask","dd","remove",
    "mkdir","rmdir","nano","edit","cat","gui","version","uname","whoami","date","mem",
    "reboot","poweroff","shutdown","halt","acpi","ps",
    "useradd","passwd","su","logout","chmod","chown", 0
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

static int slyz_last_answer = 0;

/* ---------- 路径解析 ---------- */

/* 把用户输入的路径规范化为 SlyzFS 绝对路径：
 * 反斜杠分隔、无前导/尾随分隔符、空串表示根目录。
 * 支持绝对路径（以 \ 或 / 开头）、相对路径、"." 与 ".."。 */
static void shell_resolve_path(char *out, const char *cwd, const char *input) {
    char buf[SHELL_PATH_MAX];
    int pos = 0;

    buf[0] = '\0';
    if (!input || !*input) {
        cmd_strncpy(out, cwd, SHELL_PATH_MAX);
        return;
    }

    int absolute = (input[0] == '\\' || input[0] == '/');
    if (!absolute) {
        for (int i = 0; cwd[i] && pos < SHELL_PATH_MAX - 1; i++) buf[pos++] = cwd[i];
    }

    const char *p = input;
    while (*p) {
        while (*p == '\\' || *p == '/' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char seg[64];
        int sl = 0;
        while (*p && *p != '\\' && *p != '/' && *p != ' ' && *p != '\t' && sl < 63) {
            seg[sl++] = *p++;
        }
        seg[sl] = '\0';

        if (sl == 1 && seg[0] == '.') continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            while (pos > 0 && buf[pos - 1] != '\\') pos--;
            if (pos > 0) pos--;
            buf[pos] = '\0';
            continue;
        }

        if (pos > 0 && pos < SHELL_PATH_MAX - 1) buf[pos++] = '\\';
        for (int i = 0; i < sl && pos < SHELL_PATH_MAX - 1; i++) buf[pos++] = seg[i];
        buf[pos] = '\0';
    }

    buf[pos] = '\0';
    cmd_strncpy(out, buf, SHELL_PATH_MAX);
}

/* ---------- 目录浏览与导航 ---------- */

static void cmd_dir(void) {
    static struct SlyzDirEntry entries[128];
    int n = slyzfs_list_dir(shell_cwd, entries, 128);
    if (n < 0) {
        printk("dir: cannot read '%s' (permission denied or missing)\n",
               shell_cwd[0] ? shell_cwd : "\\");
        return;
    }
    if (n == 0) {
        printk("(empty directory)\n");
        return;
    }
    printk("Directory of %s\n", shell_cwd[0] ? shell_cwd : "\\");
    printk("  MODE        UID  SIZE  NAME\n");
    for (int i = 0; i < n; i++) {
        /* 注意：printk 的 vsprintf 不支持 %6u 这类宽度修饰符，
         * 写成 %6u 会让它不消费对应的可变参数，导致后续 %s 参数错位、名字乱码。 */
        uint16_t mode = entries[i].mode;
        if (mode == 0) {
            /* 旧卷条目没有权限信息，按保守默认值显示 */
            mode = (entries[i].type == SLYZFS_DIR_DIR) ? 0700 : 0600;
        }
        char mode_str[11];
        format_mode_string(mode_str, mode,
                           entries[i].type == SLYZFS_DIR_DIR ? 1 : 0);
        printk("  %s  %u  %u  %s\n",
               mode_str,
               (unsigned int)entries[i].uid,
               (unsigned int)entries[i].size,
               entries[i].name);
    }
    printk("%d item(s)\n", n);
}

static void cmd_pwd(void) {
    printk("%s\n", shell_cwd[0] ? shell_cwd : "\\");
}

static void cmd_cd(const char *args) {
    char target[SHELL_PATH_MAX];
    shell_resolve_path(target, shell_cwd, args);

    if (target[0] == '\0') {
        shell_cwd[0] = '\0';
        shell_refresh_prompt();
        return;
    }

    struct SlyzDirEntry ent;
    if (!slyzfs_stat(target, &ent)) {
        printk("cd: '%s' does not exist\n", target);
        return;
    }
    if (ent.type != SLYZFS_DIR_DIR) {
        printk("cd: '%s' is not a directory\n", target);
        return;
    }

    cmd_strncpy(shell_cwd, target, SHELL_PATH_MAX);
    shell_refresh_prompt();
}

static void cmd_mkdir(const char *args) {
    if (!args || !*args) {
        printk("usage: mkdir <path>\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (slyzfs_mkdir(path)) {
        printk("mkdir: '%s' created\n", path);
    } else {
        printk("mkdir: cannot create '%s'\n", path);
    }
}

static void cmd_rmdir(const char *args) {
    if (!args || !*args) {
        printk("usage: rmdir <path>\n");
        return;
    }
    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (slyzfs_rmdir(path)) {
        printk("rmdir: '%s' removed\n", path);
    } else {
        printk("rmdir: cannot remove '%s'\n", path);
    }
}

static void cmd_run(const char *args) {
    if (!args || !*args) {
        printk("usage: run <file.slyz>\n");
        return;
    }
    if (try_run_slyz_by_name(args)) {
        return;
    }
    printk("run: %s not found\n", args);
}

/* ---- nano / edit：全屏文本编辑器 ----
 * 路径先按当前工作目录解析成 SlyzFS 绝对路径，再交给编辑器。
 * 编辑期间编辑器独占整个屏幕，退出时自行清屏并把 TTY 交还，
 * 这里只需重新计算一次安全区。 */
static void cmd_edit(const char *args) {
    if (!args || *args == '\0') {
        printk("usage: nano <file>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (path[0] == '\0') {
        printk("nano: invalid file name\n");
        return;
    }

    /* 已存在的文件先试读打开：这样能在进入全屏界面之前就拒绝无权限的文件，
     * 而不是让用户编辑半天后在保存时才失败。 */
    struct SlyzDirEntry probe_ent;
    if (slyzfs_stat(path, &probe_ent)) {
        void *probe = slyzfs_open(path, "r");
        if (!probe) {
            printk("nano: %s: permission denied\n", path);
            return;
        }
        slyzfs_close(probe);
    }

    int rc = editor_run(path);
    shell_refresh_prompt();

    if (rc == 0) {
        printk("nano: closed %s\n", path);
    } else {
        printk("nano: cannot edit %s\n", path);
    }
}

/* ---- cat：输出文件内容 ----
 * 通过 SlyzFS 打开并分块读取，正文直接写 VGA 而不经过 tty_putchar：
 * 后者每个字符都要等一次 9600 波特率的串口发送，十几 KB 的文件会把
 * 屏幕拖成十几秒的幻灯片。串口侧只在结束时补一行统计，保留可追溯性。 */
static void cmd_cat(const char *args) {
    if (!args || *args == '\0') {
        printk("usage: cat <file>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, args);
    if (path[0] == '\0') {
        printk("cat: invalid path\n");
        return;
    }

    struct SlyzDirEntry ent;
    if (!slyzfs_stat(path, &ent)) {
        printk("cat: %s: no such file\n", path);
        return;
    }
    if (ent.type == SLYZFS_DIR_DIR) {
        printk("cat: %s: is a directory\n", path);
        return;
    }

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        /* 前面已用 stat 确认条目存在且不是目录，open 失败只可能是权限 */
        printk("cat: %s: permission denied\n", path);
        return;
    }

    uint8_t chunk[512];
    int total = 0;
    int ended_with_newline = 1;
    int n;
    while ((n = slyzfs_read(fh, chunk, (int)sizeof(chunk))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            /* CRLF 文本只保留 '\n'，否则屏幕上每行之间会多出空行 */
            if (c == '\r') continue;
            tty_putchar_vga(c);
            total++;
            ended_with_newline = (c == '\n');
        }
    }
    slyzfs_close(fh);

    /* 文件末尾没有换行时补一个，避免下一条输出粘在最后一行行尾 */
    if (!ended_with_newline) {
        tty_putchar_vga('\n');
    }

    kserial_puts("[cat] ");
    kserial_puts(path);
    kserial_puts(" ");
    kserial_dec32((uint32_t)total);
    kserial_puts(" bytes\r\n");
}

static void cmd_ask(const char *args) {
    char prompt[96];
    if (!args || !*args) {
        printk("usage: ask <prompt>\n");
        return;
    }

    cmd_strncpy(prompt, args, sizeof(prompt));
    char *p = prompt;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    char *end = p + cmd_strlen(p) - 1;
    while (end >= p && (*end == ' ' || *end == '\t' || *end == '"')) {
        *end-- = '\0';
    }

    printk("%s [Y/N] ", p);
    while (1) {
        char c = kb_read_char();
        if (c == 0) {
            delay(50000);
            continue;
        }
        if (c == 3) {
            printk("^C\n");
            slyz_last_answer = 0;
            return;
        }
        if (c == 'y' || c == 'Y') {
            printk("y\n");
            slyz_last_answer = 1;
            return;
        }
        if (c == 'n' || c == 'N') {
            printk("n\n");
            slyz_last_answer = 0;
            return;
        }
    }
}

static int try_run_slyz_by_name(const char *name) {
    if (!name || *name == '\0') return 0;

    /* 通过 SlyzFS 打开脚本，不再扫描原始扇区。
     * 先按用户给的名字找，找不到再补 .slyz 后缀。 */
    static char script[4096];
    char path[SHELL_PATH_MAX];
    int got = 0;

    for (int attempt = 0; attempt < 2 && !got; attempt++) {
        if (attempt == 0) {
            shell_resolve_path(path, shell_cwd, name);
        } else {
            char with_ext[SHELL_PATH_MAX];
            int pos = 0;
            while (name[pos] && pos < SHELL_PATH_MAX - 8) {
                with_ext[pos] = name[pos];
                pos++;
            }
            with_ext[pos++] = '.';
            with_ext[pos++] = 's';
            with_ext[pos++] = 'l';
            with_ext[pos++] = 'y';
            with_ext[pos++] = 'z';
            with_ext[pos] = '\0';
            shell_resolve_path(path, shell_cwd, with_ext);
        }

        void *fh = slyzfs_open(path, "r");
        if (!fh) continue;
        int n = slyzfs_read(fh, (uint8_t *)script, (int)sizeof(script) - 1);
        slyzfs_close(fh);
        if (n > 0) {
            script[n] = '\0';
            got = 1;
        }
    }

    if (!got) {
        return 0;
    }

    char *lines[64];
    int line_count = 0;
    char *p = script;
    while (*p && line_count < 64) {
        while (*p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;

        lines[line_count++] = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p == '\r') *p++ = '\0';
        if (*p == '\n') *p++ = '\0';
    }

    int parse_block(int start, int end) {
        for (int i = start; i < end; i++) {
            char *trim = strip_leading_ws(lines[i]);
            if (*trim == '\0') continue;
            if (cmd_strcmp(trim, "endif") == 0) continue;

            if (cmd_strncmp(trim, "if ", 3) == 0) {
                char *cond = trim + 3;
                while (*cond == ' ' || *cond == '\t') cond++;
                int expect = (cmd_strcmp(cond, "yes") == 0);
                if (cmd_strcmp(cond, "no") == 0) expect = 0;

                int current_indent = leading_indent(lines[i]);
                int end_idx = find_matching_endif(lines, line_count, i, current_indent);
                if (end_idx < 0) {
                    printk("%s: unterminated if block\n", name);
                    return 0;
                }

                int active = (slyz_last_answer && expect) || (!slyz_last_answer && !expect);
                if (active && !parse_block(i + 1, end_idx)) {
                    return 0;
                }
                i = end_idx;
                continue;
            }

            char *cmd = trim;
            while (*cmd && *cmd != ' ' && *cmd != '\t') cmd++;
            if (*cmd) {
                *cmd++ = '\0';
                while (*cmd == ' ' || *cmd == '\t') cmd++;
            }

            if (cmd_strcmp(trim, "ask") == 0) {
                cmd_ask(cmd);
                continue;
            }

            if (!exec_cmd(trim, cmd)) {
                printk("%s: invalid script command: %s\n", name, trim);
                return 0;
            }
        }
        return 1;
    }

    return parse_block(0, line_count);
}

static void cmd_help(void)  { for (int i = 0; help_text[i]; i++) printk("%s\n", help_text[i]); }
static void cmd_min(void)  { printk("help clear echo dir ls pwd cd dd mkdir rmdir nano cat run gui ps version uname whoami date mem reboot poweroff acpi useradd passwd su logout chmod chown\n"); }
static void cmd_clear(void){ tty_clear(); }
static void cmd_echo(const char *args)  { printk("%s\n", args); }

/* ============================================================
 * dd —— 统一的裸扇区 / 文本读写命令
 *
 * 原 read / write / writet / readt 四条命令合并于此，避免多个
 * 语义重叠的裸扇区出入口。语法对齐 Unix dd：
 *   dd if=<src> of=<dst> [bs=<n>] [count=<n>] [seek=<n>] [skip=<n>] [quiet]
 *
 *   <src> / <dst> 取值：
 *     sector:<N>   第 N 个扇区（LBA 从 0 起）
 *     text:<str>   字面文本（仅 if=）
 *     zero         全零填充（仅 if=）
 *     stdout       屏幕 hexdump（仅 of=）
 *
 *   bs     每块字节数，默认 512，必须为 512 的倍数，上限 4096
 *   count  块数，默认 1
 *   skip   if=sector: 时的起始块偏移
 *   seek   of=sector: 时的起始块偏移
 *
 * 示例：
 *   dd if=sector:2048 of=stdout               查看超级块
 *   dd if=text:hello of=sector:4096           把文本写入扇区
 *   dd if=zero of=sector:4096 count=8         清零 8 个扇区
 *   dd if=sector:4096 of=sector:8192 count=2  扇区拷贝
 * ============================================================ */

#define DD_BUF_SIZE 4096u

enum {
    DD_SPEC_SECTOR = 0,
    DD_SPEC_TEXT,
    DD_SPEC_ZERO,
    DD_SPEC_STDOUT
};

struct dd_spec {
    int         kind;
    uint32_t    sector;
    const char *text;
};

static int dd_parse_spec(const char *s, struct dd_spec *out) {
    if (!s || !*s) return 0;
    if (cmd_strncmp(s, "sector:", 7) == 0) {
        out->kind = DD_SPEC_SECTOR;
        out->text = 0;
        return parse_u32(s + 7, &out->sector) ? 1 : 0;
    }
    if (cmd_strncmp(s, "text:", 5) == 0) {
        out->kind = DD_SPEC_TEXT;
        out->sector = 0;
        out->text = s + 5;
        return 1;
    }
    if (cmd_strcmp(s, "zero") == 0) {
        out->kind = DD_SPEC_ZERO;
        out->sector = 0;
        out->text = 0;
        return 1;
    }
    if (cmd_strcmp(s, "stdout") == 0) {
        out->kind = DD_SPEC_STDOUT;
        out->sector = 0;
        out->text = 0;
        return 1;
    }
    return 0;
}

static void dd_hexdump(const uint8_t *buf, uint32_t n, uint32_t base) {
    static const char hexc[] = "0123456789abcdef";
    char line[96];
    for (uint32_t off = 0; off < n; off += 16u) {
        int pos = 0;
        uint32_t addr = base + off;
        for (int sh = 28; sh >= 0; sh -= 4) line[pos++] = hexc[(addr >> sh) & 0xFu];
        line[pos++] = ' ';
        line[pos++] = ' ';
        for (int i = 0; i < 16; i++) {
            if (off + (uint32_t)i < n) {
                uint8_t v = buf[off + (uint32_t)i];
                line[pos++] = hexc[v >> 4];
                line[pos++] = hexc[v & 0xFu];
            } else {
                line[pos++] = ' ';
                line[pos++] = ' ';
            }
            line[pos++] = ' ';
            if (i == 7) line[pos++] = ' ';
        }
        line[pos++] = '|';
        for (int i = 0; i < 16 && off + (uint32_t)i < n; i++) {
            uint8_t v = buf[off + (uint32_t)i];
            line[pos++] = (v >= 0x20u && v <= 0x7Eu) ? (char)v : '.';
        }
        line[pos++] = '|';
        line[pos] = '\0';
        printk("%s\n", line);
    }
}

static void dd_usage(void) {
    printk("usage: dd if=<src> of=<dst> [bs=<n>] [count=<n>] [seek=<n>] [skip=<n>] [quiet]\n");
    printk("  src/dst: sector:<N> | text:<str> | zero | stdout\n");
    printk("  example: dd if=sector:2048 of=stdout\n");
    printk("  example: dd if=text:hello of=sector:4096\n");
    printk("  example: dd if=zero of=sector:4096 count=8\n");
}

static void cmd_dd(char *args) {
    char *src_s = 0;
    char *dst_s = 0;
    uint32_t bs = 512u;
    uint32_t count = 1u;
    uint32_t seek = 0u;
    uint32_t skip = 0u;
    int quiet = 0;

    if (!args || !*args) { dd_usage(); return; }

    char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';

        if (cmd_strncmp(tok, "if=", 3) == 0) {
            src_s = tok + 3;
        } else if (cmd_strncmp(tok, "of=", 3) == 0) {
            dst_s = tok + 3;
        } else if (cmd_strncmp(tok, "bs=", 3) == 0) {
            if (!parse_u32(tok + 3, &bs)) { printk("dd: invalid bs\n"); return; }
        } else if (cmd_strncmp(tok, "count=", 6) == 0) {
            if (!parse_u32(tok + 6, &count)) { printk("dd: invalid count\n"); return; }
        } else if (cmd_strncmp(tok, "seek=", 5) == 0) {
            if (!parse_u32(tok + 5, &seek)) { printk("dd: invalid seek\n"); return; }
        } else if (cmd_strncmp(tok, "skip=", 5) == 0) {
            if (!parse_u32(tok + 5, &skip)) { printk("dd: invalid skip\n"); return; }
        } else if (cmd_strcmp(tok, "quiet") == 0) {
            quiet = 1;
        } else {
            printk("dd: unknown option '%s'\n", tok);
            return;
        }
    }

    if (!src_s || !*src_s || !dst_s || !*dst_s) { dd_usage(); return; }

    struct dd_spec src;
    struct dd_spec dst;
    if (!dd_parse_spec(src_s, &src)) { printk("dd: invalid if= spec '%s'\n", src_s); return; }
    if (!dd_parse_spec(dst_s, &dst)) { printk("dd: invalid of= spec '%s'\n", dst_s); return; }

    if (src.kind == DD_SPEC_STDOUT) { printk("dd: if= cannot be stdout\n"); return; }
    if (dst.kind == DD_SPEC_TEXT || dst.kind == DD_SPEC_ZERO) {
        printk("dd: of= must be sector:<N> or stdout\n");
        return;
    }
    if (bs == 0u || (bs % 512u) != 0u) {
        printk("dd: bs must be a non-zero multiple of 512\n");
        return;
    }
    if (bs > DD_BUF_SIZE) {
        printk("dd: bs too large (max %u)\n", (unsigned)DD_BUF_SIZE);
        return;
    }
    if (count == 0u) count = 1u;

    uint32_t sectors_per_block = bs / 512u;
    uint32_t blocks_per_chunk = DD_BUF_SIZE / bs;
    if (blocks_per_chunk == 0u) blocks_per_chunk = 1u;

    uint8_t buf[DD_BUF_SIZE];
    uint32_t done = 0;
    uint32_t total = 0;
    int text_consumed = 0;

    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > blocks_per_chunk) chunk = blocks_per_chunk;
        uint32_t bytes = chunk * bs;

        /* ---- 填充缓冲区 ---- */
        if (src.kind == DD_SPEC_ZERO) {
            for (uint32_t i = 0; i < bytes; i++) buf[i] = 0;
        } else if (src.kind == DD_SPEC_TEXT) {
            uint32_t len = 0;
            if (!text_consumed) {
                while (src.text[len]) len++;
            }
            if (len > bytes) len = bytes;
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)src.text[i];
            for (uint32_t i = len; i < bytes; i++) buf[i] = 0;
            text_consumed = 1;
        } else {
            for (uint32_t b = 0; b < chunk; b++) {
                for (uint32_t s = 0; s < sectors_per_block; s++) {
                    uint32_t lba = src.sector + (skip + done + b) * sectors_per_block + s;
                    if (lba >= DISK_MAX_SECTORS) {
                        printk("dd: if sector %u out of range\n", (unsigned)lba);
                        return;
                    }
                    if (!ata_read_sector(lba, buf + b * bs + s * 512u)) {
                        printk("dd: read failed at sector %u\n", (unsigned)lba);
                        return;
                    }
                }
            }
        }

        /* ---- 写出 ---- */
        if (dst.kind == DD_SPEC_STDOUT) {
            uint32_t base_addr = (src.kind == DD_SPEC_SECTOR)
                ? (src.sector + skip + done) * bs
                : done * bs;
            dd_hexdump(buf, bytes, base_addr);
        } else {
            for (uint32_t b = 0; b < chunk; b++) {
                for (uint32_t s = 0; s < sectors_per_block; s++) {
                    uint32_t lba = dst.sector + (seek + done + b) * sectors_per_block + s;
                    if (lba >= DISK_MAX_SECTORS) {
                        printk("dd: of sector %u out of range\n", (unsigned)lba);
                        return;
                    }
                    if (!ata_write_sector(lba, buf + b * bs + s * 512u)) {
                        printk("dd: write failed at sector %u\n", (unsigned)lba);
                        return;
                    }
                }
            }
        }

        total += bytes;
        done += chunk;
    }

    if (!quiet) {
        printk("dd: %u bytes (%u block(s) x %u) done\n",
               (unsigned)total, (unsigned)count, (unsigned)bs);
    }
}

static void cmd_remove(char *args) {
    char *start_s = 0;
    char *end_s = 0;
    if (!args || !*args) { printk("usage: remove <start> <end>\n"); return; }

    split_first_arg(args, &start_s, &end_s);
    if (!*start_s || !*end_s) { printk("usage: remove <start> <end>\n"); return; }

    uint32_t start = 0, end = 0;
    if (!parse_u32(start_s, &start) || !parse_u32(end_s, &end)) {
        printk("remove: invalid sector range\n");
        return;
    }
    if (start >= DISK_MAX_SECTORS || end >= DISK_MAX_SECTORS) {
        printk("remove: sector out of range [0..%u]\n", DISK_MAX_SECTORS - 1u);
        return;
    }
    if (end < start) {
        printk("remove: end must be >= start\n");
        return;
    }

    for (uint32_t i = start; i <= end; i++) {
        uint8_t block[512];
        for (int j = 0; j < 512; j++) block[j] = 0;
        if (!ata_write_sector(i, block)) {
            printk("remove: write failed at sector %u\n", (unsigned int)i);
            return;
        }
    }

    printk("remove: cleared sectors %u..%u\n", (unsigned int)start, (unsigned int)end);
}

#ifdef SAYLTER_VBE
#include "logo.h"

static void vbe_draw_bitmap_centered(int ox, int oy, uint16_t w, uint16_t h, const uint32_t *src) {
    if (!src) return;
    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            uint32_t color = src[(uint32_t)y * w + x];
            vbe_put_pixel((uint16_t)(ox + x), (uint16_t)(oy + y), color);
        }
    }
}

void gui_main(void) {
    tty_clear();
    tty_set_cursor(0, 0);
    vbe_init();
    if (!vbe_is_enabled()) {
        printk("[GUI] VBE not available.\n");
        return;
    }

    vbe_fill_color(0x00000000);

    const int draw_x = (vbe_current.width - SAYLTER_LOGO_W) / 2;
    const int draw_y = (vbe_current.height - SAYLTER_LOGO_H) / 2;
    vbe_draw_bitmap_centered(draw_x, draw_y, SAYLTER_LOGO_W, SAYLTER_LOGO_H, saylter_logo_pixels);
    vbe_swap();

    for (;;) {
        /* GUI 模式同样需要响应 ACPI 电源键：轮询 PM1_STS 即可，
         * 不依赖 SCI 中断路由（那需要 AML 的 _PRT 信息）。 */
        if (acpi_poll_power_button()) {
            printk("\n[ACPI] power button: shutting down (S5)\n");
            acpi_power_off();
        }
        for (volatile int i = 0; i < 200000; i++) {
            __asm__ volatile ("nop");
        }
    }
}
#endif
static void cmd_version(void) { printk("SaylterOS v0.3 (TTY shell)\n"); }
static void cmd_uname(void)   { printk("SaylterOS i386 32-bit\n"); }
static void cmd_whoami(void) {
    const struct AuthUser *u = auth_current();
    if (u) {
        printk("%s (uid %u)\n", u->name, (unsigned int)u->uid);
    } else {
        printk("root (uid 0)\n");
    }
}
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
/* 真实硬件复位：走 8042 键盘控制器的 0xFE 命令拉 CPU RESET#。
 * 旧实现只是 cli;hlt，等于关机，必须手动断电才能恢复。 */
static void cmd_reboot(void)  {
    printk("Rebooting (8042 reset line)...\n");
    reboot();
}

/* ---- ACPI S5 关机 ----
 * 只写 SLP_TYP|SLP_EN 到 PM1_CNT，不解析 DSDT 的 \_S5 包，
 * 因此默认 SLP_TYP=5（QEMU/Bochs 与多数 x86 平台）；
 * 特殊主板可用 `poweroff <0..7>` 现场试出正确值。 */
static void cmd_poweroff(const char *args) {
    uint8_t slp_typ = acpi_get_s5_slp_typ();

    if (!acpi_available()) {
        printk("poweroff: ACPI not available\n");
        printk("Halting - power off manually\n");
        while (1) __asm__ volatile ("cli; hlt");
    }

    if (args && *args) {
        uint32_t v;
        if (!parse_u32(args, &v) || v > 7) {
            printk("poweroff: slp_typ must be 0..7\n");
            return;
        }
        slp_typ = (uint8_t)v;
        acpi_set_s5_slp_typ(slp_typ);
        printk("Shutting down (ACPI S5, SLP_TYP=%u)...\n", (unsigned int)slp_typ);
        acpi_power_off_with(slp_typ);
    } else {
        printk("Shutting down (ACPI S5, SLP_TYP=%u then fallback %u)...\n",
               (unsigned int)slp_typ,
               (unsigned int)((slp_typ == 0) ? 5 : 0));
        acpi_power_off();
    }

    printk("poweroff: S5 request did not take effect\n");
    printk("Halting - power off manually\n");
    while (1) __asm__ volatile ("cli; hlt");
}

static void cmd_acpi(void) { acpi_dump(); }

/* ---- 用户与权限命令 ---- */

static int parse_octal_mode(const char *s, uint16_t *out) {
    uint16_t v = 0;
    if (!s || !*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '7') return 0;
        if (v > 0777u) return 0;
        v = (uint16_t)(v * 8u + (uint16_t)(*s - '0'));
        s++;
    }
    *out = v;
    return 1;
}

/* 把 12 位权限位转成 ls 风格的 10 字符串 */
static void format_mode_string(char out[11], uint16_t mode, uint8_t is_dir) {
    out[0] = is_dir ? 'd' : '-';
    const char *rwx = "rwx";
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & (uint16_t)(1u << (8 - i))) ? rwx[i % 3] : '-';
    }
    out[10] = '\0';
}

static void cmd_useradd(const char *args) {
    if (!args || !*args) {
        printk("usage: useradd <name>\n");
        return;
    }
    auth_useradd(args);
}

static void cmd_passwd(const char *args) {
    auth_passwd(args);
}

static void cmd_su(const char *args) {
    const char *target = (args && *args) ? args : "root";
    if (!auth_switch_to(target)) return;

    const struct AuthUser *u = auth_current();
    if (u && u->home[0]) {
        cmd_strncpy(shell_cwd, u->home, SHELL_PATH_MAX);
    } else {
        shell_cwd[0] = '\0';
    }
    shell_refresh_prompt();
}

static void cmd_logout(void) {
    printk("logging out\n");
    auth_logout();

    if (!auth_login()) return;

    const struct AuthUser *u = auth_current();
    if (u && u->home[0]) {
        cmd_strncpy(shell_cwd, u->home, SHELL_PATH_MAX);
    } else {
        shell_cwd[0] = '\0';
    }

    tty_clear();
    printk("SaylterOS v0.3 - type 'help' for commands\n\n");
    shell_refresh_prompt();
}

static void cmd_chmod(const char *args) {
    char work[160];
    cmd_strncpy(work, (args && *args) ? args : "", sizeof(work));

    char *mode_s = work;
    while (*mode_s == ' ' || *mode_s == '\t') mode_s++;
    char *path_s = mode_s;
    while (*path_s && *path_s != ' ' && *path_s != '\t') path_s++;
    if (*path_s) *path_s++ = '\0';
    while (*path_s == ' ' || *path_s == '\t') path_s++;

    if (!*mode_s || !*path_s) {
        printk("usage: chmod <octal-mode> <path>\n");
        return;
    }

    uint16_t mode;
    if (!parse_octal_mode(mode_s, &mode)) {
        printk("chmod: mode must be octal, e.g. chmod 600 secret.txt\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, path_s);
    if (slyzfs_chmod(path, (int)mode)) {
        printk("chmod: %s updated\n", path);
    } else {
        printk("chmod: permission denied or not found\n");
    }
}

static void cmd_chown(const char *args) {
    char work[160];
    cmd_strncpy(work, (args && *args) ? args : "", sizeof(work));

    char *uid_s = work;
    while (*uid_s == ' ' || *uid_s == '\t') uid_s++;
    char *path_s = uid_s;
    while (*path_s && *path_s != ' ' && *path_s != '\t') path_s++;
    if (*path_s) *path_s++ = '\0';
    while (*path_s == ' ' || *path_s == '\t') path_s++;

    if (!*uid_s || !*path_s) {
        printk("usage: chown <uid> <path>\n");
        return;
    }

    uint32_t uid;
    if (!parse_u32(uid_s, &uid) || uid > 255) {
        printk("chown: uid must be 0..255\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    shell_resolve_path(path, shell_cwd, path_s);
    if (slyzfs_chown(path, (int)uid)) {
        printk("chown: %s -> uid %u\n", path, (unsigned int)uid);
    } else {
        printk("chown: permission denied (root only) or not found\n");
    }
}
static void cmd_ps(void)      { printk("PID  STATE  NAME\n  0  RUN     idle\n  1  RUN     shell\n"); }

/* 执行命令，返回 1 表示执行了，0 表示未找到 */
static int exec_cmd(const char *cmd, char *args) {
    if (cmd_strcmp(cmd, "help") == 0)      cmd_help();
    else if (cmd_strcmp(cmd, "min") == 0)   cmd_min();
    else if (cmd_strcmp(cmd, "clear") == 0) cmd_clear();
    else if (cmd_strcmp(cmd, "cls") == 0)   cmd_clear();
    else if (cmd_strcmp(cmd, "echo") == 0)  cmd_echo(args);
    else if (cmd_strcmp(cmd, "dir") == 0 || cmd_strcmp(cmd, "ls") == 0) cmd_dir();
    else if (cmd_strcmp(cmd, "pwd") == 0) cmd_pwd();
    else if (cmd_strcmp(cmd, "cd") == 0)  cmd_cd(args);
    else if (cmd_strcmp(cmd, "mkdir") == 0 || cmd_strcmp(cmd, "md") == 0) cmd_mkdir(args);
    else if (cmd_strcmp(cmd, "rmdir") == 0 || cmd_strcmp(cmd, "rd") == 0) cmd_rmdir(args);
    else if (cmd_strcmp(cmd, "run") == 0)   cmd_run(args);
    else if (cmd_strcmp(cmd, "nano") == 0 || cmd_strcmp(cmd, "edit") == 0) cmd_edit(args);
    else if (cmd_strcmp(cmd, "cat") == 0) cmd_cat(args);
    else if (cmd_strcmp(cmd, "ask") == 0)  cmd_ask(args);
    else if (cmd_strcmp(cmd, "exit") == 0 || cmd_strcmp(cmd, "quit") == 0) {
        printk("Bye.\n");
        return 2;
    }
    else if (cmd_strcmp(cmd, "dd") == 0)     cmd_dd(args);
    else if (cmd_strcmp(cmd, "remove") == 0) cmd_remove(args);
#ifdef SAYLTER_VBE
    else if (cmd_strcmp(cmd, "gui") == 0)   gui_main();
#endif
    else if (cmd_strcmp(cmd, "version") == 0) cmd_version();
    else if (cmd_strcmp(cmd, "uname") == 0) cmd_uname();
    else if (cmd_strcmp(cmd, "whoami") == 0) cmd_whoami();
    else if (cmd_strcmp(cmd, "date") == 0)  cmd_date();
    else if (cmd_strcmp(cmd, "mem") == 0)   cmd_mem();
    else if (cmd_strcmp(cmd, "reboot") == 0) cmd_reboot();
    else if (cmd_strcmp(cmd, "poweroff") == 0 || cmd_strcmp(cmd, "shutdown") == 0 ||
             cmd_strcmp(cmd, "halt") == 0) cmd_poweroff(args);
    else if (cmd_strcmp(cmd, "acpi") == 0) cmd_acpi();
    else if (cmd_strcmp(cmd, "useradd") == 0 || cmd_strcmp(cmd, "adduser") == 0) cmd_useradd(args);
    else if (cmd_strcmp(cmd, "passwd") == 0) cmd_passwd(args);
    else if (cmd_strcmp(cmd, "su") == 0)     cmd_su(args);
    else if (cmd_strcmp(cmd, "logout") == 0) cmd_logout();
    else if (cmd_strcmp(cmd, "chmod") == 0)  cmd_chmod(args);
    else if (cmd_strcmp(cmd, "chown") == 0)  cmd_chown(args);
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
    /* 启动页静默挂载 SlyzFS：
     * 先把 bridge 的画面日志关掉，挂载/格式化完成后再打印 banner，
     * 启动画面因此不会被 [SlyzFS] 探测信息占满，进入 shell 时文件系统已就绪。 */
    slyzfs_set_quiet(1);
    int mount_ok = slyzfs_try_mount();
    int fresh_volume = 0;
    if (!mount_ok) {
        if (slyzfs_format_and_mount()) {
            /* 新格式化的卷建立一套默认目录骨架，避免根目录空空如也 */
            slyzfs_mklazy();
            fresh_volume = 1;
        }
    }
    slyzfs_set_quiet(0);

    printk(banner);
    printk("SaylterOS TTY Shell — type 'help' for commands\n");
    printk("Insert toggles edit mode, arrows move cursor, Backspace/Delete work\n");

    if (mount_ok || fresh_volume) {
        static struct SlyzDirEntry root_entries[32];
        int root_count = slyzfs_list_dir("", root_entries, 32);
        if (root_count < 0) root_count = 0;
        if (fresh_volume) {
            printk("[OK] SlyzFS formatted: new volume, %d root entries\n", root_count);
        } else {
            printk("[OK] SlyzFS mounted: %d root entries\n", root_count);
        }
    } else {
        printk("[!!] SlyzFS unavailable — file commands are disabled\n");
    }
    printk("\n");

    /* ---- 登录 ----
     * 账户库不存在时 auth_init 会创建 root 账户（无密码）；
     * 文件系统不可用时登录必然失败，此时降级为无会话的 root shell，
     * 让用户仍能使用 reboot/poweroff 等不依赖磁盘的命令。 */
    auth_init();
    if (!auth_login()) {
        printk("[!] login unavailable — continuing as root without a session\n");
    }

    /* 登录成功后从干净屏幕开始 */
    tty_clear();

    const struct AuthUser *session = auth_current();
    if (session && session->home[0]) {
        cmd_strncpy(shell_cwd, session->home, SHELL_PATH_MAX);
    } else {
        shell_cwd[0] = '\0';
    }

    printk("SaylterOS v0.3 — type 'help' for commands\n");
    printk("User: %s (uid %u)   Home: %s\n\n",
           session ? session->name : "root",
           (unsigned int)(session ? session->uid : 0),
           shell_cwd[0] ? shell_cwd : "\\");

    shell_refresh_prompt();

    char line[256];
    while (1) {
        /* 每轮重新同步一次安全区：clear 命令或编辑器退出都会把 scroll_limit
         * 复位成全屏，若不在提示符绘制前重算，prompt 会被画到第 25 行并被 clamp，
         * 提交后没有保留区可搬运，历史行就会错位。 */
        shell_sync_scroll_zone();
        /* 提示符固定在保留区首行，上方输出再长也不会把它滚走 */
        tty_set_cursor(0, (uint16_t)(tty_get_scroll_limit() + 1));
        printk(shell_prompt);
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

        /* 先查内置命令：内置命令必须优先响应。
         * 若先扫描磁盘查找同名 .slyz 可执行文件，每条命令都要遍历数万个扇区，
         * 在 9600 波特率串口下会表现为"任意命令都卡死"。 */
        int rc = exec_cmd(cmd, args);
        if (rc == 2) break;
        if (rc == 1) continue;

        /* 内置没有 → 再尝试作为 .slyz 可执行文件执行 */
        if (try_run_slyz_by_name(cmd)) continue;

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
