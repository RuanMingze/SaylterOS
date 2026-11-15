/* ============================================================
 * editor.c - SaylterOS 全屏文本编辑器（nano 风格）
 * ------------------------------------------------------------
 * 设计约束：
 *   1. 直接写 VGA 文本内存（0xB8000），不经过 tty_putchar。
 *      tty_putchar 每个字符都会同步回显到 COM1，9600 波特率下
 *      一次全屏重绘要 2 秒以上，编辑会变成幻灯片；编辑器必须绕开它。
 *   2. 不使用任何 libc 函数，字符串/内存操作全部本地实现。
 *   3. 所有缓冲均为静态存储，避免裸内核栈溢出。
 *   4. 文件内容以字节缓冲 + '\n' 分隔保存，按需扫描行边界；
 *      16KB 缓冲的扫描是纯内存操作，远快于任何磁盘访问。
 *
 * 快捷键：
 *   Ctrl+S  保存        Ctrl+X  退出（有未保存修改时询问）
 *   Ctrl+K  删除整行
 *   方向键 / Home / End / PageUp / PageDown 移动光标
 *   Enter 换行、Backspace 删除前一个字符、Delete 删除当前字符
 * ============================================================ */

#include <stdint.h>
#include "editor.h"
#include "tty.h"
#include "keyboard.h"
#include "acpi.h"

/* ---- 屏幕布局 ---- */
#define ED_COLS        80
#define ED_ROWS        25
#define ED_ROW_TITLE   0
#define ED_ROW_FIRST   1
#define ED_VIEW_ROWS   22          /* 行 1..22 为编辑区 */
#define ED_ROW_STATUS  23
#define ED_ROW_HINT    24

/* ---- VGA 颜色属性（高 4 位背景，低 4 位前景） ---- */
#define ED_ATTR_TITLE   0x1F       /* 蓝底亮白 */
#define ED_ATTR_TEXT    0x0F       /* 黑底亮白 */
#define ED_ATTR_STATUS  0x70       /* 灰底黑字 */
#define ED_ATTR_HINT    0x17       /* 蓝底灰白 */

/* ---- 编辑缓冲上限 ---- */
#define ED_BUF_MAX     16384

/* ---- SlyzFS 接口（与 shell.c 中的声明保持一致的 C 链接） ---- */
extern void* slyzfs_open(const char* path, const char* mode);
extern int   slyzfs_read(void* fh, uint8_t* buf, int n);
extern int   slyzfs_write(void* fh, const uint8_t* buf, int n);
extern void  slyzfs_close(void* fh);

/* ---- 编辑器状态 ---- */
static char ed_buf[ED_BUF_MAX];
static int  ed_len = 0;
static int  ed_cursor = 0;        /* 0..ed_len 的字节偏移 */
static int  ed_top_line = 0;      /* 视口首行行号 */
static int  ed_hscroll = 0;       /* 水平滚动偏移（字符列） */
static int  ed_desired_col = 0;   /* 上下移动时保持的目标列 */
static int  ed_dirty = 0;
static char ed_path[256];
static char ed_msg[ED_COLS + 1];
static int  ed_msg_is_error = 0;

/* ============================================================
 * 基础工具
 * ============================================================ */

static void ed_pause(void) {
    /* 键盘邮箱为空时空转，避免忙等把 CPU 打满导致 QEMU 抢占延迟 */
    for (volatile int i = 0; i < 20000; i++) {
        __asm__ volatile ("nop");
    }
}

static int ed_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void ed_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static int ed_append(char *dst, int pos, const char *src) {
    while (*src) dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

static int ed_append_uint(char *dst, int pos, unsigned int v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) dst[pos++] = tmp[--n];
    dst[pos] = '\0';
    return pos;
}

static void ed_set_msg(const char *text, int is_error) {
    int i = 0;
    while (text[i] && i < ED_COLS) {
        ed_msg[i] = text[i];
        i++;
    }
    ed_msg[i] = '\0';
    ed_msg_is_error = is_error;
}

/* ============================================================
 * 行/列换算 —— 缓冲内容以 '\n' 分隔，不保证以 '\0' 结尾
 * ============================================================ */

static int ed_line_count(void) {
    int lines = 1;
    for (int i = 0; i < ed_len; i++) {
        if (ed_buf[i] == '\n') lines++;
    }
    return lines;
}

static int ed_line_start(int line) {
    if (line <= 0) return 0;
    int seen = 0;
    for (int i = 0; i < ed_len; i++) {
        if (ed_buf[i] == '\n') {
            seen++;
            if (seen == line) return i + 1;
        }
    }
    return ed_len;
}

static int ed_line_end(int line) {
    int i = ed_line_start(line);
    while (i < ed_len && ed_buf[i] != '\n') i++;
    return i;
}

static int ed_line_of(int off) {
    if (off < 0) off = 0;
    if (off > ed_len) off = ed_len;
    int line = 0;
    for (int i = 0; i < off; i++) {
        if (ed_buf[i] == '\n') line++;
    }
    return line;
}

static int ed_col_of(int off) {
    int start = ed_line_start(ed_line_of(off));
    return off - start;
}

static void ed_clamp_cursor(void) {
    if (ed_cursor < 0) ed_cursor = 0;
    if (ed_cursor > ed_len) ed_cursor = ed_len;
}

/* ============================================================
 * 缓冲编辑
 * ============================================================ */

static int ed_insert(int at, const char *src, int n) {
    if (n <= 0) return 1;
    if (ed_len + n > ED_BUF_MAX) {
        ed_set_msg("Buffer full - cannot insert", 1);
        return 0;
    }
    if (at < 0) at = 0;
    if (at > ed_len) at = ed_len;

    for (int i = ed_len - 1; i >= at; i--) {
        ed_buf[i + n] = ed_buf[i];
    }
    for (int i = 0; i < n; i++) {
        ed_buf[at + i] = src[i];
    }
    ed_len += n;
    ed_dirty = 1;
    return 1;
}

static void ed_erase(int at, int n) {
    if (n <= 0 || at < 0 || at >= ed_len) return;
    if (at + n > ed_len) n = ed_len - at;
    for (int i = at; i + n < ed_len; i++) {
        ed_buf[i] = ed_buf[i + n];
    }
    ed_len -= n;
    ed_dirty = 1;
}

static void ed_type_char(char c) {
    if (ed_insert(ed_cursor, &c, 1)) {
        ed_cursor++;
        ed_desired_col = ed_col_of(ed_cursor);
    }
}

static void ed_backspace(void) {
    if (ed_cursor <= 0) return;
    ed_erase(ed_cursor - 1, 1);
    ed_cursor--;
    ed_desired_col = ed_col_of(ed_cursor);
    ed_set_msg("", 0);
}

static void ed_delete_forward(void) {
    if (ed_cursor >= ed_len) return;
    ed_erase(ed_cursor, 1);
    ed_desired_col = ed_col_of(ed_cursor);
    ed_set_msg("", 0);
}

static void ed_delete_line(void) {
    int line = ed_line_of(ed_cursor);
    int start = ed_line_start(line);
    int end = ed_line_end(line);

    /* 删除该行内容；若后面还有行，把行尾换行一并删除，
     * 最后一个空行只清内容，保证缓冲始终有至少一个可编辑行。 */
    if (end < ed_len) {
        ed_erase(start, end - start + 1);
    } else {
        ed_erase(start, end - start);
    }
    ed_cursor = start;
    ed_clamp_cursor();
    ed_desired_col = 0;
    ed_set_msg("Line deleted", 0);
}

/* ============================================================
 * 光标移动
 * ============================================================ */

static void ed_move_vertical(int dir) {
    int line = ed_line_of(ed_cursor);
    int target = line + dir;
    int total = ed_line_count();
    if (target < 0 || target >= total) return;

    int start = ed_line_start(target);
    int end = ed_line_end(target);
    int max_col = end - start;
    int col = ed_desired_col;
    if (col > max_col) col = max_col;
    ed_cursor = start + col;
}

static void ed_scroll_view(int dir) {
    int total = ed_line_count();
    int line = ed_line_of(ed_cursor);
    int target_line = line + dir * ED_VIEW_ROWS;
    if (target_line < 0) target_line = 0;
    if (target_line >= total) target_line = total - 1;

    int start = ed_line_start(target_line);
    int end = ed_line_end(target_line);
    int col = ed_desired_col;
    if (col > end - start) col = end - start;
    ed_cursor = start + col;
    ed_top_line = target_line;
}

/* ============================================================
 * VGA 直接绘制
 * ============================================================ */

static void ed_put(int row, int col, char ch, uint8_t attr) {
    if (row < 0 || row >= ED_ROWS || col < 0 || col >= ED_COLS) return;
    uint16_t *video = (uint16_t *)0xB8000;
    video[row * ED_COLS + col] = (uint16_t)(((uint16_t)attr << 8) | (uint8_t)ch);
}

static void ed_fill_row(int row, uint8_t attr) {
    for (int c = 0; c < ED_COLS; c++) ed_put(row, c, ' ', attr);
}

static int ed_put_str(int row, int col, const char *s, uint8_t attr, int limit) {
    while (*s && col < limit) {
        ed_put(row, col, *s, attr);
        col++;
        s++;
    }
    return col;
}

static void ed_draw(void) {
    int cur_line = ed_line_of(ed_cursor);
    int cur_col = ed_col_of(ed_cursor);
    int total = ed_line_count();

    /* 视口跟随光标 */
    if (cur_line < ed_top_line) ed_top_line = cur_line;
    if (cur_line >= ed_top_line + ED_VIEW_ROWS) {
        ed_top_line = cur_line - ED_VIEW_ROWS + 1;
    }
    if (ed_top_line > total - 1) ed_top_line = total - 1;
    if (ed_top_line < 0) ed_top_line = 0;

    /* 水平滚动跟随 */
    if (cur_col < ed_hscroll) ed_hscroll = cur_col;
    if (cur_col >= ed_hscroll + ED_COLS) {
        ed_hscroll = cur_col - ED_COLS + 1;
    }
    if (ed_hscroll < 0) ed_hscroll = 0;

    /* 标题栏 */
    ed_fill_row(ED_ROW_TITLE, ED_ATTR_TITLE);
    int x = 1;
    x = ed_put_str(ED_ROW_TITLE, x, " SAYLTER NANO  ", ED_ATTR_TITLE, ED_COLS);
    x = ed_put_str(ED_ROW_TITLE, x, ed_path, ED_ATTR_TITLE, ED_COLS - 13);
    if (ed_dirty) {
        ed_put_str(ED_ROW_TITLE, x, " [Modified]", ED_ATTR_TITLE, ED_COLS);
    }

    /* 编辑区 */
    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        ed_fill_row(ED_ROW_FIRST + r, ED_ATTR_TEXT);
        int line = ed_top_line + r;
        if (line >= total) continue;

        int start = ed_line_start(line);
        int end = ed_line_end(line);
        int col = 0;
        for (int i = start + ed_hscroll; i < end && col < ED_COLS; i++) {
            char ch = ed_buf[i];
            if (ch == '\t') {
                int stop = (col / 4 + 1) * 4;
                while (col < stop && col < ED_COLS) {
                    ed_put(ED_ROW_FIRST + r, col, ' ', ED_ATTR_TEXT);
                    col++;
                }
            } else {
                ed_put(ED_ROW_FIRST + r, col, ch, ED_ATTR_TEXT);
                col++;
            }
        }
    }

    /* 状态栏 */
    ed_fill_row(ED_ROW_STATUS, ED_ATTR_STATUS);
    char bar[ED_COLS + 1];
    int p = 0;
    p = ed_append(bar, p, " Line ");
    p = ed_append_uint(bar, p, (unsigned int)(cur_line + 1));
    p = ed_append(bar, p, "/");
    p = ed_append_uint(bar, p, (unsigned int)total);
    p = ed_append(bar, p, "  Col ");
    p = ed_append_uint(bar, p, (unsigned int)(cur_col + 1));
    p = ed_append(bar, p, "  ");
    p = ed_append_uint(bar, p, (unsigned int)ed_len);
    p = ed_append(bar, p, " bytes ");
    if (ed_dirty) p = ed_append(bar, p, " *");
    ed_put_str(ED_ROW_STATUS, 1, bar, ED_ATTR_STATUS, ED_COLS);

    /* 提示栏 */
    uint8_t hint_attr = ed_msg_is_error ? 0x4F : ED_ATTR_HINT;
    ed_fill_row(ED_ROW_HINT, hint_attr);
    if (ed_msg[0]) {
        ed_put_str(ED_ROW_HINT, 1, ed_msg, hint_attr, ED_COLS);
    } else {
        ed_put_str(ED_ROW_HINT, 1,
                   "^S Save   ^X Exit   ^K Delete line   Arrows/Home/End/PgUp/PgDn move",
                   hint_attr, ED_COLS);
    }

    /* 硬件光标跟随文本光标 */
    int screen_row = ED_ROW_FIRST + (cur_line - ed_top_line);
    int screen_col = cur_col - ed_hscroll;
    if (screen_row < ED_ROW_FIRST) screen_row = ED_ROW_FIRST;
    if (screen_row > ED_ROW_STATUS - 1) screen_row = ED_ROW_STATUS - 1;
    if (screen_col < 0) screen_col = 0;
    if (screen_col >= ED_COLS) screen_col = ED_COLS - 1;
    tty_set_cursor((uint16_t)screen_col, (uint16_t)screen_row);
}

/* ============================================================
 * 文件读写
 * ============================================================ */

static void ed_load(void) {
    ed_len = 0;
    ed_cursor = 0;
    ed_top_line = 0;
    ed_hscroll = 0;
    ed_desired_col = 0;
    ed_dirty = 0;
    ed_msg[0] = '\0';

    void *fh = slyzfs_open(ed_path, "r");
    if (!fh) {
        /* 文件不存在属于正常路径：从头开始编辑一个新文件 */
        return;
    }
    int n = slyzfs_read(fh, (uint8_t *)ed_buf, ED_BUF_MAX);
    slyzfs_close(fh);
    if (n < 0) n = 0;
    ed_len = n;
    if (n >= ED_BUF_MAX) {
        ed_set_msg("File truncated to 16KB buffer limit", 1);
    }
}

static int ed_save(void) {
    /* "w" 模式在 SlyzFS 中会截断旧文件并重新创建，新文件同样适用 */
    void *fh = slyzfs_open(ed_path, "w");
    if (!fh) {
        ed_set_msg("Save failed: cannot create file", 1);
        return 0;
    }

    int total = 0;
    while (total < ed_len) {
        int n = slyzfs_write(fh, (const uint8_t *)(ed_buf + total), ed_len - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);

    if (total != ed_len) {
        ed_set_msg("Save failed: short write", 1);
        return 0;
    }

    ed_dirty = 0;
    char msg[ED_COLS + 1];
    int p = 0;
    p = ed_append(msg, p, "Saved ");
    p = ed_append_uint(msg, p, (unsigned int)ed_len);
    p = ed_append(msg, p, " bytes to ");
    p = ed_append(msg, p, ed_path);
    ed_set_msg(msg, 0);
    return 1;
}

/* ============================================================
 * 退出流程
 * ============================================================ */

static int ed_request_exit(void) {
    if (!ed_dirty) return 0;

    ed_set_msg("Save modified buffer?  (y)es  (n)o  (c)ancel", 1);
    ed_draw();

    while (1) {
        char k = kb_read_char();
        if (k == 0) {
            ed_pause();
            continue;
        }
        if (k == 'y' || k == 'Y') {
            if (ed_save()) return 0;
            ed_draw();
            return 1;
        }
        if (k == 'n' || k == 'N') return 0;
        if (k == 'c' || k == 'C') {
            ed_set_msg("", 0);
            return 1;
        }
    }
}

/* ============================================================
 * 主入口
 * ============================================================ */

int editor_run(const char *path) {
    if (!path || *path == '\0') return 1;

    ed_strcpy(ed_path, path);
    ed_load();
    ed_draw();

    int running = 1;
    while (running) {
        char c = kb_read_char();
        if (c == 0) {
            /* 编辑状态下主机按电源键同样应触发 S5，否则用户会以为系统死了 */
            if (acpi_poll_power_button()) {
                tty_clear();
                printk("[ACPI] power button: shutting down (S5)\n");
                acpi_power_off();
                ed_draw();
            }
            ed_pause();
            continue;
        }

        switch (c) {
            case 19: {                       /* Ctrl+S */
                ed_save();
                break;
            }
            case 24: {                       /* Ctrl+X */
                running = ed_request_exit();
                break;
            }
            case 11: {                       /* Ctrl+K */
                ed_delete_line();
                break;
            }
            case '\r':
            case '\n': {
                ed_type_char('\n');
                break;
            }
            case '\b': {
                ed_backspace();
                break;
            }
            case '\t': {
                const char spaces[4] = { ' ', ' ', ' ', ' ' };
                if (ed_insert(ed_cursor, spaces, 4)) {
                    ed_cursor += 4;
                    ed_desired_col = ed_col_of(ed_cursor);
                }
                break;
            }
            case (char)0xF1: {               /* left */
                if (ed_cursor > 0) ed_cursor--;
                ed_desired_col = ed_col_of(ed_cursor);
                break;
            }
            case (char)0xF2: {               /* right */
                if (ed_cursor < ed_len) ed_cursor++;
                ed_desired_col = ed_col_of(ed_cursor);
                break;
            }
            case (char)0xF3: {               /* home */
                ed_cursor = ed_line_start(ed_line_of(ed_cursor));
                ed_desired_col = 0;
                break;
            }
            case (char)0xF4: {               /* end */
                ed_cursor = ed_line_end(ed_line_of(ed_cursor));
                ed_desired_col = ed_col_of(ed_cursor);
                break;
            }
            case (char)0xF6: {               /* up */
                ed_move_vertical(-1);
                break;
            }
            case (char)0xF7: {               /* down */
                ed_move_vertical(1);
                break;
            }
            case (char)0xF5: {               /* delete */
                ed_delete_forward();
                break;
            }
            case (char)0xF8: {               /* page up */
                ed_scroll_view(-1);
                break;
            }
            case (char)0xF9: {               /* page down */
                ed_scroll_view(1);
                break;
            }
            case (char)0xF0: {               /* insert（本编辑器恒为插入模式） */
                break;
            }
            default: {
                if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x80) {
                    ed_type_char(c);
                }
                break;
            }
        }

        ed_clamp_cursor();
        ed_draw();
    }

    /* 退出后清屏并把 TTY 状态交还给 shell：
     * tty_clear 会把 scroll_limit 复位，shell 随后按新 prompt 重新计算安全区。 */
    tty_clear();
    return 0;
}
