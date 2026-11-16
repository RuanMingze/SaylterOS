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
#include "ime.h"
#include "unicode.h"

/* ---- 屏幕布局 ----
 * 终端行列由 tty 决定（VBE 图形终端通常大于 80x25）：
 *   第 0 行标题 / 第 1..rows-3 行编辑区 / 倒数第 2 行状态 / 最后一行提示。
 * ED_MAX_COLS 只用于静态缓冲区定尺寸。 */
#define ED_MAX_COLS    128

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
static char ed_msg[ED_MAX_COLS + 1];
static int  ed_msg_is_error = 0;

static int ed_view_rows(void);

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
    int limit = (int)tty_get_cols();
    if (limit > ED_MAX_COLS) limit = ED_MAX_COLS;
    int i = 0;
    while (text[i] && i < limit) {
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

/* 光标在行内的"显示列"：汉字占 2 列，不能再用字节差 */
static int ed_col_of(int off) {
    int line = ed_line_of(off);
    int start = ed_line_start(line);
    if (off < start) off = start;
    if (off > ed_len) off = ed_len;
    return utf8_width_n(ed_buf + start, off - start);
}

/* 行内第 want_col 个显示列对应的字节偏移；落在宽字符中间时取字符起点 */
static int ed_byte_at_col(int line, int want_col) {
    int start = ed_line_start(line);
    int end = ed_line_end(line);
    int col = 0;
    int i = start;
    while (i < end) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)ed_buf + i, end - i, &cp);
        if (used <= 0) { i++; continue; }
        int w = utf32_width(cp);
        if (w == 0) { i += used; continue; }
        if (col + w > want_col) break;
        col += w;
        i += used;
    }
    return i;
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

/* 插入一段 UTF-8 文本（输入法上屏用），光标停在插入内容之后 */
static void ed_type_utf8(const char *text) {
    int n = 0;
    while (text[n]) n++;
    if (n <= 0) return;
    if (ed_insert(ed_cursor, text, n)) {
        ed_cursor += n;
        ed_desired_col = ed_col_of(ed_cursor);
    }
}

static void ed_backspace(void) {
    if (ed_cursor <= 0) return;
    /* 一次退掉一个完整字符：中文是三字节，退一个字节会留下半个序列 */
    int prev = utf8_prev_char(ed_buf, ed_cursor);
    ed_erase(prev, ed_cursor - prev);
    ed_cursor = prev;
    ed_desired_col = ed_col_of(ed_cursor);
    ed_set_msg("", 0);
}

static void ed_delete_forward(void) {
    if (ed_cursor >= ed_len) return;
    int next = utf8_next_char(ed_buf, ed_cursor);
    ed_erase(ed_cursor, next - ed_cursor);
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

    /* 目标列是显示列：由 ed_byte_at_col 反查字节偏移 */
    ed_cursor = ed_byte_at_col(target, ed_desired_col);
}

static void ed_scroll_view(int dir) {
    int total = ed_line_count();
    int line = ed_line_of(ed_cursor);
    int target_line = line + dir * ed_view_rows();
    if (target_line < 0) target_line = 0;
    if (target_line >= total) target_line = total - 1;

    ed_cursor = ed_byte_at_col(target_line, ed_desired_col);
    ed_top_line = target_line;
}

/* ============================================================
 * 屏幕绘制 —— 统一走 tty 的单元格接口
 * ------------------------------------------------------------
 * 不再直接写 0xB8000：VBE 图形终端下字符要经 16x32 字模渲染，
 * 只有 tty 知道当前后端与字符栅格。整屏重绘包在 batch 里，
 * 所有单元格画完后只做一次帧缓冲回写。
 * ============================================================ */

static int ed_view_rows(void) {
    int r = (int)tty_get_rows() - 3;   /* 标题 1 行 + 状态 1 行 + 提示 1 行 */
    return (r > 0) ? r : 1;
}

static int ed_status_row(void) {
    int r = (int)tty_get_rows() - 2;
    return (r > 0) ? r : 0;
}

static int ed_hint_row(void) {
    int r = (int)tty_get_rows() - 1;
    return (r > 0) ? r : 0;
}

static void ed_fill_row(int row, uint8_t attr) {
    tty_clear_row((uint16_t)row, attr);
}

static int ed_put_str(int row, int col, const char *s, uint8_t attr, int limit) {
    int x = col;
    int i = 0;
    while (s[i] && x < limit) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, 4, &cp);
        if (used <= 0) { i++; continue; }
        int w = utf32_width(cp);
        if (w == 0) { i += used; continue; }
        if (x + w > limit) break;
        tty_put_at_cp((uint16_t)x, (uint16_t)row, (uint16_t)cp, attr);
        x += w;
        i += used;
    }
    return x;
}

static void ed_draw(void) {
    int cur_line = ed_line_of(ed_cursor);
    int cur_col = ed_col_of(ed_cursor);
    int total = ed_line_count();
    int cols = (int)tty_get_cols();
    int view_rows = ed_view_rows();
    int status_row = ed_status_row();
    int hint_row = ed_hint_row();

    /* 视口跟随光标 */
    if (cur_line < ed_top_line) ed_top_line = cur_line;
    if (cur_line >= ed_top_line + view_rows) {
        ed_top_line = cur_line - view_rows + 1;
    }
    if (ed_top_line > total - 1) ed_top_line = total - 1;
    if (ed_top_line < 0) ed_top_line = 0;

    /* 水平滚动跟随 */
    if (cur_col < ed_hscroll) ed_hscroll = cur_col;
    if (cur_col >= ed_hscroll + cols) {
        ed_hscroll = cur_col - cols + 1;
    }
    if (ed_hscroll < 0) ed_hscroll = 0;

    tty_begin_batch();

    /* 标题栏 */
    ed_fill_row(0, ED_ATTR_TITLE);
    int x = 1;
    x = ed_put_str(0, x, " SAYLTER NANO  ", ED_ATTR_TITLE, cols);
    int path_limit = cols - 13;
    if (path_limit < x) path_limit = x;
    x = ed_put_str(0, x, ed_path, ED_ATTR_TITLE, path_limit);
    if (ed_dirty) {
        ed_put_str(0, x, " [Modified]", ED_ATTR_TITLE, cols);
    }

    /* 编辑区 */
    for (int r = 0; r < view_rows; r++) {
        int row = 1 + r;
        if (row >= status_row) break;
        ed_fill_row(row, ED_ATTR_TEXT);
        int line = ed_top_line + r;
        if (line >= total) continue;

        int start = ed_line_start(line);
        int end = ed_line_end(line);
        int col = 0;
        int i = start;
        while (i < end) {
            uint32_t cp = 0;
            int used = utf8_decode((const uint8_t *)ed_buf + i, end - i, &cp);
            if (used <= 0) { i++; col++; continue; }

            if (cp == '\t') {
                int stop = (col / 4 + 1) * 4;
                while (col < stop && col - ed_hscroll < cols) {
                    int draw = col - ed_hscroll;
                    if (draw >= 0) tty_put_at((uint16_t)draw, (uint16_t)row, ' ', ED_ATTR_TEXT);
                    col++;
                }
                i += used;
                continue;
            }

            int w = utf32_width(cp);
            if (w == 0) { i += used; continue; }

            int draw = col - ed_hscroll;
            if (draw >= 0) {
                if (draw + w > cols) break;
                tty_put_at_cp((uint16_t)draw, (uint16_t)row, (uint16_t)cp, ED_ATTR_TEXT);
            }
            col += w;
            i += used;
        }
    }

    /* 状态栏 */
    ed_fill_row(status_row, ED_ATTR_STATUS);
    char bar[ED_MAX_COLS + 1];
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
    ed_put_str(status_row, 1, bar, ED_ATTR_STATUS, cols);

    /* 提示栏：输入法激活时优先显示拼音与候选，否则显示提示信息 */
    uint8_t hint_attr = ed_msg_is_error ? 0x4F : ED_ATTR_HINT;
    ed_fill_row(hint_row, hint_attr);
    if (ime_chinese_enabled() && ime_composing()[0] != '\0') {
        char bar[256];
        int q = 0;
        const char *comp = ime_composing();
        for (int i = 0; comp[i] && q < 20; i++) bar[q++] = comp[i];
        bar[q++] = ' ';
        for (int i = 0; i < IME_PAGE_SIZE; i++) {
            char cand[16];
            int n = ime_candidate_text(i, cand, sizeof(cand));
            if (n <= 0) break;
            if (q + n + 3 >= (int)sizeof(bar)) break;
            bar[q++] = (char)('1' + i);
            for (int k = 0; k < n; k++) bar[q++] = cand[k];
            bar[q++] = ' ';
        }
        bar[q] = '\0';
        ed_put_str(hint_row, 1, bar, hint_attr, cols);
    } else if (ed_msg[0]) {
        ed_put_str(hint_row, 1, ed_msg, hint_attr, cols);
    } else {
        ed_put_str(hint_row, 1,
                   "^S Save   ^X Exit   ^K Delete line   Arrows/Home/End/PgUp/PgDn move",
                   hint_attr, cols);
    }

    /* 光标跟随文本光标（图形终端会自动渲染方块/下划线光标） */
    int screen_row = 1 + (cur_line - ed_top_line);
    int screen_col = cur_col - ed_hscroll;
    if (screen_row < 1) screen_row = 1;
    if (screen_row > status_row - 1) screen_row = status_row - 1;
    if (screen_col < 0) screen_col = 0;
    if (screen_col >= cols) screen_col = cols - 1;
    tty_set_cursor((uint16_t)screen_col, (uint16_t)screen_row);

    tty_end_batch();
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
    /* 保持 C 字符串语义：utf8_prev_char / utf8_next_char 依赖结尾 NUL */
    ed_buf[ed_len] = '\0';
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
    char msg[ED_MAX_COLS + 1];
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
    /* 编辑器恒为插入模式：光标用半高实心块，与 shell 的覆盖模式区分 */
    tty_set_cursor_shape(0x0F, 0x0F);
    ed_draw();

    int running = 1;
    while (running) {
        char c = kb_read_char();
        if (c == 0) {
            tty_tick();
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

        /* 输入法优先：中文模式下字母先进拼音缓冲，上屏时插入 UTF-8 */
        {
            char commit[64];
            int ir = ime_key((int)(unsigned char)c, commit, sizeof(commit));
            if (ir == IME_CONSUMED) {
                ed_clamp_cursor();
                ed_draw();
                continue;
            }
            if (ir == IME_COMMIT) {
                ed_type_utf8(commit);
                ed_clamp_cursor();
                ed_draw();
                continue;
            }
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
                if (ed_cursor > 0) ed_cursor = utf8_prev_char(ed_buf, ed_cursor);
                ed_desired_col = ed_col_of(ed_cursor);
                break;
            }
            case (char)0xF2: {               /* right */
                if (ed_cursor < ed_len) ed_cursor = utf8_next_char(ed_buf, ed_cursor);
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
