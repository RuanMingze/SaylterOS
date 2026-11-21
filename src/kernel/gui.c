/* ============================================================
 * gui.c - SaylterOS 图形会话
 * ------------------------------------------------------------
 * 启动模式由 etc\system.cfg 的 boot.mode 决定：
 *   tty -> 原来的文本 shell（本文件完全不参与）
 *   gui -> 本文件接管：启动画面 -> 图形登录 -> 桌面
 *
 * 桌面布局刻意不用"顶部栏 + 底部 Dock"：
 *   左侧是一条竖向任务栏，头像、搜索、应用启动器、已打开窗口、
 *   电源菜单全部收纳在同一个容器里，窗口工作区从任务栏右侧开始。
 *   这样窗口标题栏本身就是唯一的"程序框"，不会再出现上下两条栏。
 *
 * 绘制约束与 ui.c 一致：只用整数运算，全部写 backbuffer，
 * ui_swap() 一次性上屏；鼠标移动只交换两个 16x24 小矩形。
 * ============================================================ */

#include <stdint.h>

#include "vbe.h"
#include "ui.h"
#include "tty.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "auth.h"
#include "system_cfg.h"
#include "acpi.h"
#include "acpica_glue.h"
#include "reboot.h"
#include "net.h"
#include "gui.h"
#include "debug.h"
#include "shell.h"
#include "html.h"
#include "mem.h"
#include "task.h"
#include "smp.h"
#include "vm.h"
#include "ata.h"
#include "ime.h"
#include "unicode.h"
#include "slyertype.h"
#include "lang.h"
#include "gui_slyz.h"
#include "image.h"
#include "audio.h"
#include "blk.h"

/* 界面文案统一走语言包：LTR("Files") 在当前语言为中文时返回"文件"，
 * 未收录的条目自动回退英文，新增语言不需要改任何调用点。 */
#define LTR(s) lang_tr(s)

/* 磁盘统计由 blk.c 提供（USB 或 ATA） */

/* ============================================================
 * SlyzFS 接口与目录项镜像
 * ------------------------------------------------------------
 * 结构必须与 slyzfs_bridge.cpp 的 SlyzDirEntry 逐字节一致（71 字节），
 * 否则按数组下标访问会整体错位。shell.c 里有一份同样的定义。
 * ============================================================ */

#pragma pack(push, 1)
struct GuiDirEntry {
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

#define GUI_DIR_FILE 0x01
#define GUI_DIR_DIR  0x02

extern int   slyzfs_mkdir(const char *path);
extern int   slyzfs_rmdir(const char *path);
extern int   slyzfs_remove(const char *path);
extern int   slyzfs_create(const char *path);
extern int   slyzfs_list_dir(const char *path, struct GuiDirEntry *out, int out_max);
extern int   slyzfs_stat(const char *path, struct GuiDirEntry *out);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);
extern void  slyzfs_set_quiet(int quiet);

/* ============================================================
 * 基础常量
 * ============================================================ */

#define GUI_TASKBAR_W 88
#define GUI_WIN_MAX   10
#define GUI_APP_COUNT 8
#define GUI_FM_MAX    128
#define GUI_ED_MAX    65536
#define GUI_CURSOR_W  16
#define GUI_CURSOR_H  24

enum {
    WIN_FILES = 0,
    WIN_EDITOR = 1,
    WIN_SETTINGS = 2,
    WIN_TERM = 3,
    WIN_BROWSER = 4,
    WIN_SLYZ = 5,
    WIN_TASKMGR = 6,
    WIN_CALC = 7,
    WIN_MEDIA = 8,
    /* 原生预装应用窗口：由 GuiExternalHost 驱动（如 VizCormin）。 */
    WIN_EXTERNAL = 9
};

enum {
    ACT_NONE = -1,
    ACT_LOGOUT = 0,
    ACT_REBOOT = 1,
    ACT_SHUTDOWN = 2,
    ACT_TTY = 3
};

#define C_BG       0x090D13
#define C_BG2      0x101722
#define C_PANEL    0x141C27
#define C_PANEL2   0x1B2532
#define C_PANEL3   0x223041
#define C_BORDER   0x2B3949
#define C_TEXT     0xE9EFF7
#define C_DIM      0x8EA0B4
#define C_ACCENT   0x4F8EF7
#define C_ACCENT2  0x2F6FE0
#define C_ACCENT3  0x7FB2FF
#define C_OK       0x34D399
#define C_ERR      0xF87171
#define C_WARN     0xFBBF24

#define C_AVATAR_A 0x3B82F6
#define C_AVATAR_B 0x8B5CF6
#define C_AVATAR_C 0x10B981
#define C_AVATAR_D 0xF59E0B

static int gW = 0;
static int gH = 0;
static ui_theme g_theme;

static int g_mx = 0;
static int g_my = 0;
static int g_mdown = 0;
static int g_mdown_r = 0;
static int g_mclicked = 0;
static int g_rclicked = 0;
static int g_mwheel = 0;
static int g_mreleased = 0;
static int g_mmoved = 0;

/* ============================================================
 * 字符串与数值工具（内核没有 libc）
 * ============================================================ */

static int g_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int g_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int g_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int g_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        if (g_lower((unsigned char)*a) != g_lower((unsigned char)*b)) {
            return g_lower((unsigned char)*a) - g_lower((unsigned char)*b);
        }
        a++;
        b++;
    }
    return g_lower((unsigned char)*a) - g_lower((unsigned char)*b);
}

static void g_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* 无符号整数转十进制；返回写入长度 */
static int g_utoa(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;
    int i = 0;

    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) out[i++] = tmp[--n];
    out[i] = '\0';
    return i;
}

static void g_itoa_pad(uint32_t v, char *out, int width) {
    char tmp[12];
    int n = g_utoa(v, tmp);
    int pad = width - n;
    int p = 0;
    while (pad-- > 0) out[p++] = ' ';
    for (int i = 0; i < n; i++) out[p++] = tmp[i];
    out[p] = '\0';
}

static int g_contains_nocase(const char *haystack, const char *needle) {
    int nl = g_strlen(needle);
    if (nl == 0) return 1;
    for (int i = 0; haystack[i]; i++) {
        int k = 0;
        while (k < nl && haystack[i + k] &&
               g_lower((unsigned char)haystack[i + k]) ==
               g_lower((unsigned char)needle[k])) {
            k++;
        }
        if (k == nl) return 1;
    }
    return 0;
}

static int g_point_in(const ui_rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void gui_delay_ms(uint32_t ms) {
    uint32_t start = net_now_ms();
    while (net_now_ms() - start < ms) {
        __asm__ volatile ("pause");
    }
}

/* ============================================================
 * 鼠标光标
 * ============================================================ */

static const char *gui_cursor_img[GUI_CURSOR_H] = {
    "X...............",
    "XX..............",
    "XWX.............",
    "XWWX............",
    "XWWWX...........",
    "XWWWWX..........",
    "XWWWWWX.........",
    "XWWWWWWX........",
    "XWWWWWWWX.......",
    "XWWWWWWWWX......",
    "XWWWWWWWWWX.....",
    "XWWWWWWWWWWX....",
    "XWWWWWWWWWWWX...",
    "XWWWWWWWWWWWWX..",
    "XWWWWWXXXXXXXX..",
    "XWWXWWX.........",
    "XWX.XWWX........",
    "XX..XWWX........",
    "X....XWWX.......",
    ".....XWWX.......",
    "......XWWX......",
    "......XWWX......",
    ".......XX.......",
    "................"
};

static uint32_t g_cursor_save[GUI_CURSOR_W * GUI_CURSOR_H];
static int g_cursor_x = -1;
static int g_cursor_y = -1;

static void gui_cursor_draw(int x, int y) {
    if (x + GUI_CURSOR_W > gW) x = gW - GUI_CURSOR_W;
    if (y + GUI_CURSOR_H > gH) y = gH - GUI_CURSOR_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    vbe_save_rect((uint16_t)x, (uint16_t)y, GUI_CURSOR_W, GUI_CURSOR_H, g_cursor_save);

    for (int row = 0; row < GUI_CURSOR_H; row++) {
        const char *line = gui_cursor_img[row];
        for (int col = 0; col < GUI_CURSOR_W; col++) {
            char c = line[col];
            if (c == 'X') {
                vbe_put_pixel((uint16_t)(x + col), (uint16_t)(y + row), 0x000000);
            } else if (c == 'W') {
                vbe_put_pixel((uint16_t)(x + col), (uint16_t)(y + row), 0xFFFFFF);
            }
        }
    }

    g_cursor_x = x;
    g_cursor_y = y;
}

static void gui_cursor_erase(void) {
    if (g_cursor_x < 0) return;
    vbe_restore_rect((uint16_t)g_cursor_x, (uint16_t)g_cursor_y,
                     GUI_CURSOR_W, GUI_CURSOR_H, g_cursor_save);
    g_cursor_x = -1;
}

static void gui_mouse_update(void) {
    int8_t dx = 0;
    int8_t dy = 0;
    int8_t wheel = 0;
    uint8_t buttons = 0;

    g_mclicked = 0;
    g_rclicked = 0;
    g_mreleased = 0;
    g_mmoved = 0;
    g_mwheel = 0;

    if (g_mx == 0 && g_my == 0) {
        g_mx = gW / 2;
        g_my = gH / 2;
    }

    while (ps2_mouse_poll_ex(&dx, &dy, &buttons, &wheel)) {
        if (dx != 0 || dy != 0) {
            g_mx += dx;
            g_my += dy;
            g_mmoved = 1;

            if (g_mx < 0) g_mx = 0;
            if (g_my < 0) g_my = 0;
            if (g_mx > gW - GUI_CURSOR_W) g_mx = gW - GUI_CURSOR_W;
            if (g_my > gH - GUI_CURSOR_H) g_my = gH - GUI_CURSOR_H;
        }

        if (wheel != 0) {
            g_mwheel += wheel;
        }

        int was_down = g_mdown;
        g_mdown = (buttons & 1u) ? 1 : 0;
        if (g_mdown && !was_down) g_mclicked = 1;

        /* 右键是独立的上升沿：桌面菜单由它触发，左键不再代替 */
        int was_r = g_mdown_r;
        g_mdown_r = (buttons & 2u) ? 1 : 0;
        if (g_mdown_r && !was_r) g_rclicked = 1;

        if (!g_mdown && was_down) g_mreleased = 1;
    }
}

/* ============================================================
 * GUI 键盘：禁用 IRQ1，改为单一读者轮询
 * ------------------------------------------------------------
 * 8042 的输出缓冲只有一个字节，键盘中断和鼠标轮询同时在读会
 * 互相抢数据（鼠标 dx/dy 被当成扫描码写进键盘环形缓冲，表现为
 * 按一个键冒出完全无关的字符）。TTY 不轮询鼠标所以看不到这个
 * 问题；GUI 需要鼠标，因此这里把键盘也改成轮询，保证：
 *   - AUX=0 的字节只由 gui_kb_read 读
 *   - AUX=1 的字节只由 ps2_mouse_poll 读
 * 进入 GUI 时屏蔽 IRQ1，退出时恢复原 IMR 并清空残留。
 * ============================================================ */

static uint8_t g_kb_saved_imr = 0;
/* GUI 独占键盘期间 IRQ1 被屏蔽，按键只能由 gui_kb_read 轮询 8042
 * 取得；预装应用（如 VizCormin）通过 SlysKernelApi 读键盘时必须
 * 走同一条路径，否则在图形会话里一个键都收不到。 */
static int g_kb_owned = 0;

static void gui_kb_begin(void) {
    g_kb_saved_imr = inb(0x21);
    outb(0x21, (uint8_t)(g_kb_saved_imr | 0x02));   /* mask IRQ1 */
    g_kb_owned = 1;

    /* 丢掉 8042 输出缓冲里残留的键盘字节 */
    for (int i = 0; i < 64; i++) {
        uint8_t status = inb(0x64);
        if ((status & 0x01) == 0) break;
        (void)inb(0x60);
    }

    kb_flush();
}

static void gui_kb_end(void) {
    /* 退出 GUI 前关掉鼠标数据上报。TTY 没有任何鼠标消费者，鼠标包会
     * 持续占满 8042 唯一的输出缓冲，键盘中断随即读不到数据，表现为
     * "从 GUI 退出到终端后键盘完全没反应"。ISR 里也补了丢弃鼠标字节
     * 的兜底，这里主动关闭是为了从源头上不再产生包。 */
    ps2_mouse_disable();
    kb_flush();
    outb(0x21, g_kb_saved_imr);
    g_kb_owned = 0;
}

static char gui_kb_read(void) {
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(0x64);
        if ((status & 0x21) != 0x01) break;   /* 没有键盘数据 */

        uint8_t sc = inb(0x60);
        char c = scancode_to_char(sc);
        if (c != 0) return c;
    }
    return 0;
}

int gui_keyboard_owned(void) {
    return g_kb_owned;
}

char gui_keyboard_poll(void) {
    if (!g_kb_owned) return 0;
    /* 鼠标上报没有中断、完全靠 GUI 主循环轮询。预装应用独占屏幕时
     * 主循环停摆，鼠标包会持续占满 8042 唯一的数据缓冲。
     *
     * 这里不能直接 inb 丢弃 AUX 字节：鼠标驱动用跨调用的相位状态
     * （g_mouse_phase）组包，绕过它丢字节会让相位永久错位，模块退出
     * 后 GUI 的鼠标就再也不动。必须交给 ps2_mouse_poll_ex 消费，
     * 同时把位移累加进桌面光标，保证状态机与坐标都保持连续。 */
    for (int i = 0; i < 32; i++) {
        uint8_t status = inb(0x64);
        if ((status & 0x01) == 0) break;

        if ((status & 0x20) != 0) {
            int8_t mdx = 0;
            int8_t mdy = 0;
            int8_t mw = 0;
            uint8_t mb = 0;
            if (ps2_mouse_poll_ex(&mdx, &mdy, &mb, &mw) != 0) {
                g_mx += mdx;
                g_my += mdy;
                if (g_mx < 0) g_mx = 0;
                if (g_my < 0) g_my = 0;
                if (g_mx > gW - GUI_CURSOR_W) g_mx = gW - GUI_CURSOR_W;
                if (g_my > gH - GUI_CURSOR_H) g_my = gH - GUI_CURSOR_H;
                g_mmoved = 1;
            }
            continue;
        }

        char c = scancode_to_char(inb(0x60));
        if (c != 0) return c;
    }
    return 0;
}

/* ============================================================
 * 基础绘制辅助
 * ============================================================ */

static void gui_text_center(const char *s, int cx, int y, uint32_t color) {
    int w = vbe_utf8_width_px(s);
    vbe_draw_utf8_transparent((uint16_t)(cx - w / 2), (uint16_t)y, s, color);
}

/* 截断绘制的文本：max_chars 是"16px 字符格"数。
 * 中文占两格，按显示宽度截断，不会把汉字劈成半个序列。 */
static void gui_text_n(int x, int y, const char *s, int max_chars, uint32_t color) {
    if (max_chars < 1) return;
    int max_px = max_chars * 16;
    int cx = x;
    int i = 0;

    while (s[i]) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, 4, &cp);
        if (used <= 0) { i++; continue; }

        int w = utf32_width(cp);
        if (w == 0) { i += used; continue; }
        if ((cx - x) + w * 16 > max_px) break;

        if (w == 2) {
            vbe_draw_cjk_transparent((uint16_t)cx, (uint16_t)y,
                                     cp, color);
        } else {
            char one[2];
            one[0] = (char)cp;
            one[1] = '\0';
            ui_text_transparent(cx, y, one, color);
        }
        cx += w * 16;
        i += used;
    }
}

/* ============================================================
 * 输入法候选窗（GUI）
 * ------------------------------------------------------------
 * 与终端里的行内候选栏不同，这里是浮在窗口之上的独立面板：
 * 深色圆角 + 阴影感边框 + 首选高亮，位置跟随文本光标。
 * 锚点由各窗口在渲染时写入（编辑器的插入点、终端的输入位置），
 * 面板在脏矩形重绘的最后一层绘制，所以永远不会被窗口盖住。
 * ============================================================ */
static int g_ime_ax = 240;
static int g_ime_ay = 220;

/* 布局缓存：每次重绘前按候选文字的实际像素宽度算一遍。
 * 1 个字和 2 个字的候选块宽度不同，后面的块必须跟着顺延，
 * 否则左边的 72px 固定框会把"世界"这种双字词挤出去。 */
static int  g_ime_block_x[IME_PAGE_SIZE];
static int  g_ime_block_w[IME_PAGE_SIZE];
static char g_ime_block_text[IME_PAGE_SIZE][16];
static int  g_ime_block_count;
static int  g_ime_panel_w;

static int gui_ime_active(void) {
    return ime_chinese_enabled() && ime_composing()[0] != '\0';
}

static void gui_ime_layout(void) {
    int n = ime_candidate_count();
    if (n > IME_PAGE_SIZE) n = IME_PAGE_SIZE;

    g_ime_block_count = 0;
    int x = 10;
    for (int i = 0; i < n; i++) {
        int len = ime_candidate_text(i, g_ime_block_text[i],
                                     (int)sizeof(g_ime_block_text[i]));
        if (len <= 0) break;
        int tw = vbe_utf8_width_px(g_ime_block_text[i]);
        /* 左右内边距 + 序号一字符 + 序号与文字之间的间距 */
        int w = 10 + 16 + 6 + tw + 10;
        g_ime_block_x[i] = x;
        g_ime_block_w[i] = w;
        x += w + 6;
        g_ime_block_count++;
    }

    int panel = (g_ime_block_count > 0) ? (x - 6 + 10) : 0;

    /* 拼音串可能比候选还长，面板不能比它窄，否则页码会压住拼音 */
    const char *comp = ime_composing();
    int comp_w = vbe_utf8_width_px(comp) + 90;
    if (panel < comp_w) panel = comp_w;
    if (panel < 184) panel = 184;

    g_ime_panel_w = panel;
}

static ui_rect gui_ime_rect(void) {
    gui_ime_layout();

    int w = g_ime_panel_w;
    int h = 86;
    if (w > gW - 20) w = gW - 20;

    int x = g_ime_ax;
    int y = g_ime_ay;
    if (x + w > gW - 10) x = gW - 10 - w;
    if (x < 10) x = 10;
    if (y + h > gH - 12) y = g_ime_ay - h - 52;
    if (y < 10) y = 10;
    return (ui_rect){ x, y, w, h };
}

static void gui_ime_render(void) {
    if (!gui_ime_active()) return;
    ui_rect p = gui_ime_rect();

    /* 用两层圆角叠出边框，不额外依赖描边接口 */
    ui_round_fill((ui_rect){ p.x - 2, p.y - 2, p.w + 4, p.h + 4 }, 15, C_BORDER);
    ui_round_fill(p, 13, 0x121A26);

    /* 拼音编辑串 */
    ui_text_transparent(p.x + 15, p.y + 7, ime_composing(), C_DIM);

    /* 页码：页数很少，单位数拼接足够 */
    if (ime_candidate_pages() > 1) {
        char pg[8];
        int q = 0;
        pg[q++] = (char)('0' + ime_candidate_page() + 1);
        pg[q++] = '/';
        pg[q++] = (char)('0' + ime_candidate_pages());
        pg[q] = '\0';
        ui_text_transparent(p.x + p.w - 52, p.y + 7, pg, C_DIM);
    }

    for (int i = 0; i < g_ime_block_count; i++) {
        int bx = p.x + g_ime_block_x[i];
        int by = p.y + 33;
        int bw = g_ime_block_w[i];
        int sel = (i == 0);
        uint32_t bg = sel ? C_ACCENT2 : C_PANEL3;

        ui_round_fill((ui_rect){ bx, by, bw, 44 }, 9, bg);

        char idx[2];
        idx[0] = (char)('1' + i);
        idx[1] = '\0';
        ui_text_transparent(bx + 10, by + 6, idx, sel ? C_TEXT : C_DIM);
        vbe_draw_utf8_px((uint16_t)(bx + 32), (uint16_t)(by + 6),
                         g_ime_block_text[i], C_TEXT, bg);
    }
}


/* 自动换行文本（行高 34）：宽度不够时按空格断行，返回占用行数。
 * 界面里的提示/消息统一走这里，避免每处单独估算字符宽度。 */
static int gui_text_wrap(int x, int y, int max_w, const char *s,
                         uint32_t color, int max_lines) {
    return ui_text_wrap(x, y, max_w, 34, s, color, max_lines);
}

static void gui_ring(int cx, int cy, int radius, int thickness, uint32_t color) {
    if (radius <= 0) return;
    int inner = radius - thickness;
    if (inner < 0) inner = 0;
    int r2 = radius * radius;
    int i2 = inner * inner;

    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r2 && d2 >= i2) {
                vbe_put_pixel((uint16_t)(cx + x), (uint16_t)(cy + y), color);
            }
        }
    }
}

static void gui_avatar(int cx, int cy, int radius, const char *name, uint32_t c1, uint32_t c2) {
    /* 配置了头像图片就优先使用：整屏重绘时会命中图片缓存，
     * 不会每帧重新解码。图片缺失或格式不支持时回退到首字母。 */
    const char *avatar = system_cfg_avatar();
    if (avatar && avatar[0]) {
        int handle = image_load(avatar);
        if (handle >= 0 && radius > 3) {
            gui_ring(cx, cy, radius, 2, C_ACCENT);
            image_draw_circle(handle, cx, cy, radius - 2);
            return;
        }
    }

    ui_rect r;
    r.x = cx - radius;
    r.y = cy - radius;
    r.w = radius * 2;
    r.h = radius * 2;

    ui_round_fill(r, radius, c2);
    ui_rect inner;
    inner.x = r.x + 3;
    inner.y = r.y + 3;
    inner.w = r.w - 6;
    inner.h = r.h - 6;
    ui_round_gradient(inner, radius - 3, c1, c2);

    char initial[2];
    initial[0] = (name && name[0]) ? name[0] : '?';
    if (initial[0] >= 'a' && initial[0] <= 'z') {
        initial[0] = (char)(initial[0] - 'a' + 'A');
    }
    initial[1] = '\0';
    ui_text_transparent(cx - 8, cy - 16, initial, 0xFFFFFF);
}

/* ---------- hover 控件追踪 ----------
 * 局部重绘需要知道"鼠标移到某个控件上"到底影响哪些像素。渲染每个
 * 可交互控件时把它的矩形登记进来，下一帧鼠标移动就只重绘"旧位置
 * 命中的控件 + 新位置命中的控件"，而不是整个窗口或整个屏幕。
 * 重复登记（同一控件被多个脏矩形各渲一次）会被去重。 */

#define GUI_HUNT_MAX 192

static ui_rect g_hunt[GUI_HUNT_MAX];
static int g_hunt_count = 0;
static ui_rect g_hunt_prev[GUI_HUNT_MAX];
static int g_hunt_prev_count = 0;

static void hunt_reset(void) {
    g_hunt_count = 0;
}

static void hunt_add(ui_rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    for (int i = 0; i < g_hunt_count; i++) {
        const ui_rect *e = &g_hunt[i];
        if (e->x == r.x && e->y == r.y && e->w == r.w && e->h == r.h) return;
    }
    if (g_hunt_count >= GUI_HUNT_MAX) return;

    g_hunt[g_hunt_count++] = r;
}

static void hunt_snapshot(void) {
    g_hunt_prev_count = g_hunt_count;
    for (int i = 0; i < g_hunt_count; i++) {
        g_hunt_prev[i] = g_hunt[i];
    }
}

static void gui_button(ui_rect r, const char *label, int hover, int primary) {
    hunt_add(r);

    if (primary) {
        ui_round_gradient(r, 12,
                          hover ? 0x6AA4FF : C_ACCENT,
                          hover ? 0x3B7BE8 : C_ACCENT2);
    } else {
        ui_round_panel(r, 12, hover ? C_ACCENT : C_BORDER,
                       hover ? 0x1B2836 : C_PANEL2);
    }

    int tw = vbe_utf8_width_px(label);
    vbe_draw_utf8_transparent((uint16_t)(r.x + (r.w - tw) / 2),
                              (uint16_t)(r.y + (r.h - 32) / 2),
                              label, primary ? 0xFFFFFF : C_TEXT);
}

/* ---------- 图标（全部几何绘制，不依赖图片资源） ---------- */

static void icon_folder(int x, int y, int s, uint32_t color) {
    int tab_w = s / 2;
    int tab_h = s / 5;
    ui_round_fill((ui_rect){ x, y + 2, tab_w, tab_h + 4 }, 3, color);
    ui_round_fill((ui_rect){ x, y + tab_h, s, s - tab_h - 2 }, 4, color);
    ui_hline(x + 4, y + tab_h + 4, s - 8, 0xFFFFFF);
}

static void icon_file(int x, int y, int s, uint32_t color) {
    int fold = s / 4;
    ui_round_fill((ui_rect){ x + 2, y + 2, s - 4, s - 4 }, 4, color);
    /* 折角 */
    for (int i = 0; i < fold; i++) {
        ui_hline(x + s - 2 - fold + i, y + 2 + i, fold - i, 0x0E1620);
    }
    for (int k = 0; k < 3; k++) {
        ui_hline(x + 6, y + s / 2 + k * 4, s - 14, 0x0E1620);
    }
}

static void icon_editor(int x, int y, int s, uint32_t color) {
    /* 文档 + 一支笔 */
    ui_round_fill((ui_rect){ x + 2, y + 2, s - 8, s - 4 }, 4, color);
    for (int k = 0; k < 3; k++) {
        ui_hline(x + 7, y + 7 + k * 5, s - 16, 0x0E1620);
    }
    int px = x + s - 9;
    int py = y + s - 10;
    for (int k = 0; k < 7; k++) {
        ui_line(px + k, py - k, px + k + 3, py - k + 3, 0xFBBF24);
    }
}

static void icon_search(int cx, int cy, int radius, uint32_t color) {
    gui_ring(cx, cy, radius, 3, color);
    for (int k = 0; k < radius; k++) {
        ui_line(cx + radius - 2 + k / 2, cy + radius - 2 + k / 2,
                cx + radius + 2 + k / 2, cy + radius + 2 + k / 2, color);
    }
}

static void icon_settings(int x, int y, int s, uint32_t color) {
    int cx = x + s / 2;
    int cy = y + s / 2;
    int r = s / 3;
    gui_ring(cx, cy, r, 3, color);
    for (int k = 0; k < 8; k++) {
        int dx = 0;
        int dy = 0;
        int d = k % 4;
        if (d == 0) dy = -1;
        else if (d == 1) { dx = 1; }
        else if (d == 2) dy = 1;
        else dx = -1;

        int bx = cx + dx * r - 2;
        int by = cy + dy * r - 2;
        ui_round_fill((ui_rect){ bx + dx * 3, by + dy * 3, 6, 6 }, 2, color);
    }
    ui_round_fill((ui_rect){ cx - 2, cy - 2, 4, 4 }, 2, color);
}

static void icon_power(int cx, int cy, int radius, uint32_t color) {
    gui_ring(cx, cy, radius, 3, color);
    /* 经典电源符号：圆环 + 从顶部伸入圆心的竖线 */
    vbe_fill_rect((uint16_t)(cx - 1), (uint16_t)(cy - radius - 3),
                  3, (uint16_t)(radius / 2 + 3), color);
}

static void icon_terminal(int x, int y, int s, uint32_t color) {
    ui_round_panel((ui_rect){ x, y, s, s }, 5, color, 0x0E1620);
    ui_line(x + 6, y + 8, x + 11, y + s / 2, color);
    ui_line(x + 11, y + s / 2, x + 6, y + s - 8, color);
    ui_hline(x + 14, y + s - 9, s - 20, color);
}

static void icon_globe(int x, int y, int s, uint32_t color) {
    /* 浏览器：圆环 + 赤道 + 一条经线 */
    int cx = x + s / 2;
    int cy = y + s / 2;
    int r = s / 2 - 2;
    gui_ring(cx, cy, r, 2, color);
    ui_hline(cx - r, cy, r * 2 + 1, color);
    ui_vline(cx, cy - r, r * 2 + 1, color);
    ui_line(cx - r / 2, cy - r + 2, cx - r / 2, cy + r - 2, color);
    ui_line(cx + r / 2, cy - r + 2, cx + r / 2, cy + r - 2, color);
}

static void icon_chart(int x, int y, int s, uint32_t color) {
    /* 任务管理器：面板 + 三根递增的柱状条 */
    int base = y + s - 5;
    ui_round_panel((ui_rect){ x + 2, y + 2, s - 4, s - 4 }, 4, color, 0x0E1620);

    int bw = (s - 14) / 4;
    if (bw < 2) bw = 2;
    int heights[3];
    heights[0] = s / 3;
    heights[1] = s / 2;
    heights[2] = (s * 2) / 3;

    for (int i = 0; i < 3; i++) {
        int bx = x + 5 + i * (bw + 2);
        vbe_fill_rect((uint16_t)bx, (uint16_t)(base - heights[i]),
                      (uint16_t)bw, (uint16_t)heights[i], color);
    }
    ui_hline(x + 4, base, s - 8, color);
}

static void icon_fault(int x, int y, int s, uint32_t color) {
    /* SylFault：警示三角 + 感叹号，和普通应用的图标明显区分 */
    int cx = x + s / 2;
    int top = y + 3;
    int bot = y + s - 5;
    int left = x + 3;
    int right = x + s - 4;

    ui_line(cx, top, left, bot, color);
    ui_line(cx, top, right, bot, color);
    ui_line(left, bot, right, bot, color);

    int bar_h = (s * 2) / 5;
    ui_vline(cx, top + s / 4, bar_h, color);
    ui_round_fill((ui_rect){ cx - 2, bot - 7, 4, 4 }, 2, color);
}

static void icon_calc(int x, int y, int s, uint32_t color) {
    /* 计算器：外框 + 顶部显示屏 + 2x2 按键 */
    ui_round_panel((ui_rect){ x, y, s, s }, 4, color, 0x0E1620);

    int pad = s / 8;
    if (pad < 2) pad = 2;
    int screen_h = s / 5;

    vbe_fill_rect((uint16_t)(x + pad), (uint16_t)(y + pad),
                  (uint16_t)(s - pad * 2), (uint16_t)screen_h, color);

    int bw = (s - pad * 3) / 2;
    int bh = (s - pad * 3 - screen_h) / 2;
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 2; c++) {
            vbe_fill_rect((uint16_t)(x + pad + c * (bw + pad)),
                          (uint16_t)(y + pad * 2 + screen_h + r * (bh + pad)),
                          (uint16_t)bw, (uint16_t)bh, color);
        }
    }
}

static void icon_net(int x, int y, int s, uint32_t color) {
    /* 控制中心：三条滑杆 + 旋钮 */
    for (int i = 0; i < 3; i++) {
        int cy = y + 6 + i * ((s - 12) / 2);
        ui_hline(x + 4, cy, s - 8, color);
        int kx = x + 5 + ((i * 11 + 7) % (s - 12));
        ui_round_fill((ui_rect){ kx - 3, cy - 3, 7, 7 }, 3, color);
    }
}

static void icon_close(int x, int y, int s, uint32_t color) {
    for (int k = 0; k < 2; k++) {
        ui_line(x + 5 + k, y + 5, x + s - 5 + k, y + s - 5, color);
        ui_line(x + s - 5 + k, y + 5, x + 5 + k, y + s - 5, color);
    }
}

static void icon_minimize(int x, int y, int s, uint32_t color) {
    vbe_fill_rect((uint16_t)(x + 4), (uint16_t)(y + s - 8),
                  (uint16_t)(s - 8), 2, color);
}

static void icon_maximize(int x, int y, int s, uint32_t color) {
    ui_hline(x + 4, y + 4, s - 8, color);
    ui_hline(x + 4, y + s - 5, s - 8, color);
    ui_vline(x + 4, y + 4, s - 8, color);
    ui_vline(x + s - 5, y + 4, s - 8, color);
}

static void icon_restore(int x, int y, int s, uint32_t color) {
    /* 两个错位方框，表示"还原" */
    ui_hline(x + 7, y + 4, s - 10, color);
    ui_hline(x + 7, y + s - 7, s - 10, color);
    ui_vline(x + 7, y + 4, s - 10, color);
    ui_vline(x + s - 3, y + 4, s - 10, color);

    ui_hline(x + 4, y + 8, s - 11, color);
    ui_hline(x + 4, y + s - 4, s - 10, color);
    ui_vline(x + 4, y + 8, s - 11, color);
}

/* ============================================================
 * 背景与时钟
 * ============================================================ */

/* 背景纯色填充的并行分片：BSP 与 AP 各写自己的行区间。
 * 目标区域已经与裁剪矩形求交，vbe_fill_rect_rows 内部还会再夹一次，
 * 保证局部重绘时不会把脏矩形之外的桌面涂掉。 */
static volatile int g_bg_x0 = 0;
static volatile int g_bg_x1 = 0;
static volatile int g_bg_y0 = 0;
static volatile int g_bg_y1 = 0;

static void gui_bg_fill_slice(uint32_t index, uint32_t slices) {
    if (slices == 0) return;

    int y0 = g_bg_y0;
    int y1 = g_bg_y1;
    int h = y1 - y0;
    if (h <= 0) return;

    int a = y0 + (h * (int)index) / (int)slices;
    int b = y0 + (h * (int)(index + 1)) / (int)slices;
    if (b <= a) return;

    vbe_fill_rect_rows((uint16_t)g_bg_x0, (uint16_t)(g_bg_x1 - g_bg_x0),
                       (uint16_t)a, (uint16_t)b, C_BG);
}

static void gui_background(void) {
    /* 配置了壁纸就直接拉伸铺满屏幕；绘制遵守当前裁剪矩形，
     * 因此局部重绘只解码一次缓存并只写脏矩形覆盖的像素。 */
    const char *wallpaper = system_cfg_wallpaper();
    if (wallpaper && wallpaper[0]) {
        int handle = image_load(wallpaper);
        if (handle >= 0) {
            image_draw_scaled(handle, 0, 0, gW, gH);

            ui_rect strip = { 0, gH - 3, gW, 3 };
            ui_gradient_v(strip, 0x1B3550, 0x0C1420);
            return;
        }
    }

    int grad_h = gH / 3;

    /* 上 1/3：垂直渐变，逐行颜色不同，保留串行实现 */
    ui_rect top = { 0, 0, gW, grad_h };
    ui_gradient_v(top, 0x0D1622, C_BG);

    /* 下 2/3：纯色背景。按行切片交给所有核并行填充；面积太小
     * （小脏矩形）时直接单核完成，避免为几行像素付一次 IPI 往返。 */
    int fx0 = 0;
    int fy0 = grad_h;
    int fx1 = gW;
    int fy1 = gH;
    if (vbe_clip_active()) {
        int cx = 0, cy = 0, cw = 0, ch = 0;
        vbe_get_clip(&cx, &cy, &cw, &ch);
        if (cx > fx0) fx0 = cx;
        if (cy > fy0) fy0 = cy;
        if (cx + cw < fx1) fx1 = cx + cw;
        if (cy + ch < fy1) fy1 = cy + ch;
    }

    if (fx1 > fx0 && fy1 > fy0) {
        g_bg_x0 = fx0;
        g_bg_x1 = fx1;
        g_bg_y0 = fy0;
        g_bg_y1 = fy1;

        int area = (fx1 - fx0) * (fy1 - fy0);

        /* 只有"全屏重绘 + 面积足够大"才并行。
         * 局部脏矩形（拖动窗口、悬停高亮、任务管理器每秒刷新）走串行：
         * 每次并行都要发 0xF0 IPI 并自旋等 AP 唤醒，在 QEMU TCG 下这段
         * 同步延迟远大于几万像素的填充时间，每帧多次并行会把 GUI 拖卡。 */
        if (!vbe_clip_active() && area >= 400000 && smp_parallel_available()) {
            uint32_t slices = 12u;
            smp_parallel_for(slices, gui_bg_fill_slice, slices);
        } else {
            gui_bg_fill_slice(0, 1);
        }
    }

    /* 裁剪生效时（局部重绘）跳过落在裁剪外的网格线，
     * 否则每条线都要逐像素走一遍再被裁掉。 */
    int cx = 0;
    int cy = 0;
    int cw = 0;
    int ch = 0;
    int clipped = vbe_clip_active();
    if (clipped) vbe_get_clip(&cx, &cy, &cw, &ch);

    for (int x = GUI_TASKBAR_W; x < gW; x += 64) {
        if (clipped && (x < cx || x >= cx + cw)) continue;
        ui_vline(x, 0, gH, 0x111A26);
    }
    for (int y = 0; y < gH; y += 64) {
        if (clipped && (y < cy || y >= cy + ch)) continue;
        ui_hline(GUI_TASKBAR_W, y, gW - GUI_TASKBAR_W, 0x111A26);
    }

    /* 底部一条很淡的强调色光带，避免大面积纯色显得空 */
    ui_rect strip = { 0, gH - 3, gW, 3 };
    ui_gradient_v(strip, 0x1B3550, 0x0C1420);
}

/* ============================================================
 * CMOS RTC
 * ------------------------------------------------------------
 * 系统把 CMOS 时间当作 UTC 存储，桌面时钟显示 UTC + 时区偏移。
 * 设置页允许直接调整小时/分钟，写回前必须置 0x0B 的 SET 位冻结
 * 计时，否则写入过程中可能跨秒进位，写进去的值会莫名其妙差一。
 * BCD/二进制、12/24 小时由 0x0B 决定，两条路径都要处理。
 * ============================================================ */

static uint8_t rtc_reg_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void rtc_reg_write(uint8_t reg, uint8_t value) {
    outb(0x70, reg);
    outb(0x71, value);
}

static int rtc_wait_ready(void) {
    for (int guard = 0; guard < 100000; guard++) {
        if ((rtc_reg_read(0x0A) & 0x80) == 0) return 1;
    }
    return 0;
}

static int rtc_bcd_to_bin(uint8_t v) {
    return ((v >> 4) & 0x0F) * 10 + (v & 0x0F);
}

static uint8_t rtc_bin_to_bcd(int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* 读取 UTC 时分秒。任一项可为 NULL。返回 1 成功。 */
static int rtc_get_utc(int *hour, int *minute, int *second) {
    if (!rtc_wait_ready()) return 0;

    uint8_t reg_b = rtc_reg_read(0x0B);
    int bcd = !(reg_b & 0x04);
    int h24 = (reg_b & 0x02) != 0;

    uint8_t s = rtc_reg_read(0x00);
    uint8_t m = rtc_reg_read(0x02);
    uint8_t h = rtc_reg_read(0x04);

    int pm = 0;
    if (!h24) {
        pm = (h & 0x80) != 0;
        h = (uint8_t)(h & 0x7F);
    }
    if (bcd) {
        s = (uint8_t)rtc_bcd_to_bin(s);
        m = (uint8_t)rtc_bcd_to_bin(m);
        h = (uint8_t)rtc_bcd_to_bin(h);
    }
    if (!h24) {
        h = (uint8_t)(h % 12);
        if (pm) h = (uint8_t)(h + 12);
    }

    if (hour) *hour = h % 24;
    if (minute) *minute = m % 60;
    if (second) *second = s % 60;
    return 1;
}

/* 写入 UTC 时分（秒保持不变）。返回 1 成功。 */
static int rtc_set_utc(int hour, int minute) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return 0;
    if (!rtc_wait_ready()) return 0;

    uint8_t reg_b = rtc_reg_read(0x0B);
    int bcd = !(reg_b & 0x04);
    int h24 = (reg_b & 0x02) != 0;

    rtc_reg_write(0x0B, (uint8_t)(reg_b | 0x80));   /* SET = 1，停止更新 */

    uint8_t hour_value;
    if (h24) {
        hour_value = bcd ? rtc_bin_to_bcd(hour) : (uint8_t)hour;
    } else {
        int pm = hour >= 12;
        int h12 = hour % 12;
        if (h12 == 0) h12 = 12;
        hour_value = bcd ? rtc_bin_to_bcd(h12) : (uint8_t)h12;
        if (pm) hour_value = (uint8_t)(hour_value | 0x80);
    }

    rtc_reg_write(0x04, hour_value);
    rtc_reg_write(0x02, bcd ? rtc_bin_to_bcd(minute) : (uint8_t)minute);
    rtc_reg_write(0x0B, reg_b);                      /* SET = 0，恢复更新 */
    return 1;
}

/* 本地时间 = RTC(UTC) + 配置时区 */
static void gui_clock_local(int *hour, int *minute) {
    int h = 0;
    int m = 0;

    if (!rtc_get_utc(&h, &m, 0)) {
        *hour = 0;
        *minute = 0;
        return;
    }

    int total = h * 60 + m + system_cfg_timezone() * 60;
    total %= 24 * 60;
    if (total < 0) total += 24 * 60;

    *hour = total / 60;
    *minute = total % 60;
}

static void gui_clock_text(char out[6]) {
    int hour = 0;
    int minute = 0;
    gui_clock_local(&hour, &minute);

    out[0] = (char)('0' + (hour / 10) % 10);
    out[1] = (char)('0' + hour % 10);
    out[2] = ':';
    out[3] = (char)('0' + (minute / 10) % 10);
    out[4] = (char)('0' + minute % 10);
    out[5] = '\0';
}

/* "UTC+08:00" / "UTC-05:00"，cap 至少 10 字节 */
static void gui_tz_text(char *out, int cap) {
    int tz = system_cfg_timezone();
    int n = 0;

    if (cap < 10) {
        if (cap > 0) out[0] = '\0';
        return;
    }

    out[n++] = 'U';
    out[n++] = 'T';
    out[n++] = 'C';
    if (tz < 0) {
        out[n++] = '-';
        tz = -tz;
    } else {
        out[n++] = '+';
    }
    out[n++] = (char)('0' + (tz / 10) % 10);
    out[n++] = (char)('0' + tz % 10);
    out[n++] = ':';
    out[n++] = '0';
    out[n++] = '0';
    out[n] = '\0';
}

/* ============================================================
 * 账户视图与登录页
 * ============================================================ */

struct GuiUserView {
    char    name[AUTH_NAME_MAX];
    uint8_t uid;
    char    home[40];
    int     has_password;
};

static struct GuiUserView g_users[AUTH_MAX_USERS];
static int g_user_count = 0;

static void gui_load_users(void) {
    auth_reload();

    int total = auth_user_count();
    int has_non_root = 0;

    for (int i = 0; i < total; i++) {
        const struct AuthUser *u = auth_user_at(i);
        if (u && u->uid != 0) {
            has_non_root = 1;
            break;
        }
    }

    g_user_count = 0;
    for (int i = 0; i < total && g_user_count < AUTH_MAX_USERS; i++) {
        const struct AuthUser *u = auth_user_at(i);
        if (!u) continue;

        /* 需求：系统中存在普通用户时隐藏 root，避免把管理员账号
         * 暴露在登录界面的用户列表里；只有 root 时才显示 root。 */
        if (has_non_root && u->uid == 0) continue;

        g_users[g_user_count].uid = u->uid;
        g_copy(g_users[g_user_count].name, u->name, AUTH_NAME_MAX);
        g_copy(g_users[g_user_count].home, u->home, (int)sizeof(g_users[0].home));
        g_users[g_user_count].has_password = (u->hash[0] != '\0');
        g_user_count++;
    }

    /* 账户库异常时至少给出一个可登录的 root，避免界面空白 */
    if (g_user_count == 0) {
        g_users[0].uid = 0;
        g_copy(g_users[0].name, "root", AUTH_NAME_MAX);
        g_copy(g_users[0].home, "home\\root", (int)sizeof(g_users[0].home));
        g_users[0].has_password = 0;
        g_user_count = 1;
    }
}

struct GuiLoginState {
    int  sel;
    char password[64];
    int  pw_len;
    char error[64];
    uint32_t error_until;
    int  blink;
};

/* 动态脏矩形（实现见文件后半部分）。
 * 登录页也要局部重绘，所以在使用点之前先声明。 */
static void dirty_reset(void);
static void dirty_add_padded(const ui_rect *r, int pad);
static int  dirty_rect_count(void);
static void dirty_rect_get(int index, ui_rect *out);
static void dirty_swap_all(int old_cx, int old_cy);

static ui_rect login_card_rect(void) {
    ui_rect r;
    r.w = 620;
    r.h = 560;
    r.x = (gW - r.w) / 2;
    r.y = (gH - r.h) / 2;
    return r;
}

static ui_rect login_pw_rect(const ui_rect *card) {
    ui_rect r;
    r.w = 400;
    r.h = 60;
    r.x = card->x + (card->w - r.w) / 2;
    r.y = card->y + 300;
    return r;
}

static ui_rect login_btn_rect(const ui_rect *card) {
    ui_rect r = login_pw_rect(card);
    r.y = card->y + 416;
    return r;
}

static ui_rect login_switch_rect(const ui_rect *card) {
    ui_rect r;
    r.w = 220;
    r.h = 46;
    r.x = card->x + (card->w - r.w) / 2;
    r.y = card->y + 496;
    return r;
}

static void gui_login_render(const struct GuiLoginState *st) {
    gui_background();

    ui_rect card = login_card_rect();
    ui_shadow(card, 22, 12);
    ui_round_gradient(card, 22, 0x18222F, 0x101720);
    ui_round_panel(card, 22, 0x2E3D50, 0x0F1621);

    ui_rect accent_line = { card.x + 180, card.y - 2, 260, 4 };
    ui_round_fill(accent_line, 2, C_ACCENT);

    gui_text_center("SAYLTER OS", card.x + card.w / 2, card.y + 34, C_TEXT);
    gui_text_center("graphical session", card.x + card.w / 2, card.y + 72, C_DIM);

    const struct GuiUserView *u = &g_users[st->sel];
    gui_avatar(card.x + card.w / 2, card.y + 178, 52, u->name,
               C_AVATAR_A, C_AVATAR_B);

    /* 用户名 */
    int name_w = g_strlen(u->name) * 16;
    ui_text_transparent(card.x + (card.w - name_w) / 2, card.y + 244,
                        u->name, C_TEXT);

    /* 密码框 */
    ui_rect pw = login_pw_rect(&card);
    ui_round_panel(pw, 12, C_ACCENT, 0x0C141E);

    char stars[65];
    for (int i = 0; i < st->pw_len; i++) stars[i] = '*';
    stars[st->pw_len] = '\0';
    ui_text_transparent(pw.x + 18, pw.y + (pw.h - 32) / 2, stars, C_TEXT);

    if (st->blink) {
        vbe_fill_rect((uint16_t)(pw.x + 18 + st->pw_len * 16),
                      (uint16_t)(pw.y + 16), 2, (uint16_t)(pw.h - 32), C_ACCENT);
    }

    /* 提示与错误共用密码框和登录按钮之间的位置：提示只在无密码
     * 账户且没有错误时出现，避免两行文字压在一起。 */
    if (st->error[0]) {
        gui_text_center(st->error, card.x + card.w / 2, card.y + 370, C_ERR);
    } else if (!u->has_password) {
        /* 卡片宽 620px；这行必须控制在 38 个字符以内 */
        gui_text_center("No password set - press Enter",
                        card.x + card.w / 2, card.y + 370, C_DIM);
    }

    ui_rect btn = login_btn_rect(&card);
    int btn_hover = g_point_in(&btn, g_mx, g_my);
    gui_button(btn, "SIGN IN", btn_hover, 1);

    if (g_user_count > 1) {
        ui_rect sw = login_switch_rect(&card);
        int sw_hover = g_point_in(&sw, g_mx, g_my);
        gui_button(sw, "SWITCH USER", sw_hover, 0);
    }

    gui_text_center("SaylterOS v0.3 Beta1", gW / 2, gH - 44, C_DIM);
}

static void login_try(struct GuiLoginState *st, int *logged_in) {
    const struct GuiUserView *u = &g_users[st->sel];
    int uid = auth_login_as(u->name, st->password);
    if (uid >= 0) {
        *logged_in = 1;
        return;
    }

    g_copy(st->error, "Incorrect password. Try again.", (int)sizeof(st->error));
    st->error_until = net_now_ms() + 2600;
    st->pw_len = 0;
    st->password[0] = '\0';
}

static int gui_login_screen(void) {
    struct GuiLoginState st;
    int logged_in = 0;
    int last_hover = -1;

    gui_load_users();

    st.sel = 0;
    st.pw_len = 0;
    st.password[0] = '\0';
    st.error[0] = '\0';
    st.error_until = 0;
    st.blink = 1;

    gui_mouse_update();
    gui_cursor_erase();
    gui_login_render(&st);
    gui_cursor_draw(g_mx, g_my);
    ui_swap();

    while (!logged_in) {
        int need_full = 0;

        gui_mouse_update();

        /* 悬停变化需要重绘按钮高亮 */
        ui_rect card = login_card_rect();
        ui_rect btn = login_btn_rect(&card);
        ui_rect sw = login_switch_rect(&card);
        ui_rect pw = login_pw_rect(&card);
        ui_rect av = { card.x + card.w / 2 - 52, card.y + 126, 104, 104 };
        int hover = g_point_in(&btn, g_mx, g_my) ? 1 :
                    (g_user_count > 1 && g_point_in(&sw, g_mx, g_my)) ? 2 :
                    g_point_in(&pw, g_mx, g_my) ? 3 : 0;
        if (hover != last_hover) {
            last_hover = hover;
            need_full = 1;
        }

        if (g_mclicked) {
            if (g_point_in(&btn, g_mx, g_my)) {
                login_try(&st, &logged_in);
                need_full = 1;
            } else if (g_user_count > 1 &&
                       (g_point_in(&sw, g_mx, g_my) || g_point_in(&av, g_mx, g_my))) {
                st.sel = (st.sel + 1) % g_user_count;
                st.pw_len = 0;
                st.password[0] = '\0';
                st.error[0] = '\0';
                need_full = 1;
            } else if (g_point_in(&pw, g_mx, g_my)) {
                need_full = 1;
            }
        }

        for (int k = 0; k < 8; k++) {
            char c = gui_kb_read();
            if (c == 0) break;

            if (c == '\r' || c == '\n') {
                login_try(&st, &logged_in);
                need_full = 1;
            } else if (c == '\b' || c == 127) {
                if (st.pw_len > 0) {
                    st.pw_len--;
                    st.password[st.pw_len] = '\0';
                }
                need_full = 1;
            } else if (c == '\t') {
                if (g_user_count > 1) {
                    st.sel = (st.sel + 1) % g_user_count;
                    st.pw_len = 0;
                    st.password[0] = '\0';
                    st.error[0] = '\0';
                }
                need_full = 1;
            } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
                if (st.pw_len < (int)sizeof(st.password) - 1) {
                    st.password[st.pw_len++] = c;
                    st.password[st.pw_len] = '\0';
                }
                need_full = 1;
            }
        }

        if (st.error[0] && net_now_ms() > st.error_until) {
            st.error[0] = '\0';
            need_full = 1;
        }

        int blink = (int)((net_now_ms() / 500u) & 1u);
        if (blink != st.blink) {
            st.blink = blink;
            need_full = 1;
        }

        if (logged_in) break;

        if (need_full) {
            /* 登录页的所有可见变化（头像、用户名、密码框、提示、按钮）
             * 都在卡片内，只重绘卡片区域，避免每敲一个键就整屏重绘
             * 加整屏 blit。卡片外的背景与版本号是静态的。 */
            ui_rect card_dirty = login_card_rect();
            int old_x = g_cursor_x;
            int old_y = g_cursor_y;

            dirty_reset();
            dirty_add_padded(&card_dirty, 26);

            gui_cursor_erase();

            int dcount = dirty_rect_count();
            for (int i = 0; i < dcount; i++) {
                ui_rect d;
                dirty_rect_get(i, &d);
                vbe_set_clip(d.x, d.y, d.w, d.h);
                gui_login_render(&st);
                vbe_clear_clip();
            }

            gui_cursor_draw(g_mx, g_my);
            dirty_swap_all(old_x, old_y);
        } else if (g_mmoved) {
            int old_x = g_cursor_x;
            int old_y = g_cursor_y;
            gui_cursor_erase();
            gui_cursor_draw(g_mx, g_my);
            if (old_x >= 0) {
                vbe_swap_rect((uint16_t)old_x, (uint16_t)old_y,
                              GUI_CURSOR_W, GUI_CURSOR_H);
            }
            vbe_swap_rect((uint16_t)g_mx, (uint16_t)g_my,
                          GUI_CURSOR_W, GUI_CURSOR_H);
        }

        for (volatile int spin = 0; spin < 12000; spin++) {
            __asm__ volatile ("nop");
        }
    }

    gui_cursor_erase();
    return (int)g_users[st.sel].uid;
}

/* ============================================================
 * 启动画面
 * ============================================================ */

static void gui_startup_stage(const char *text, int value) {
    gui_background();
    gui_cursor_erase();

    int cx = gW / 2;
    int cy = gH / 2;

    gui_text_center("SAYLTER OS", cx, cy - 150, C_TEXT);

    ui_rect line = { cx - 150, cy - 96, 300, 3 };
    ui_round_fill(line, 1, C_ACCENT);

    gui_text_center(text, cx, cy - 40, C_DIM);

    ui_rect bar;
    bar.x = cx - 260;
    bar.y = cy + 20;
    bar.w = 520;
    bar.h = 18;

    /* 轨道 + 渐变填充 + 右侧高光，不使用 ui_progress_draw：
     * 后者会在轨道右侧再打印一遍百分比，和下面的大号百分比重复。 */
    ui_round_fill(bar, 9, 0x1B2634);
    int fill_w = bar.w * value / 100;
    if (fill_w > 0) {
        ui_rect fill = { bar.x, bar.y, fill_w, bar.h };
        ui_round_fill(fill, 9, C_ACCENT2);
        if (fill_w > 8) {
            ui_round_fill((ui_rect){ bar.x + 3, bar.y + 3, fill_w - 6, bar.h / 2 }, 4,
                          0x7FB2FF);
        }
    }

    char pct[8];
    int n = g_utoa((uint32_t)value, pct);
    pct[n++] = '%';
    pct[n] = '\0';
    gui_text_center(pct, cx, cy + 62, C_TEXT);

    gui_text_center("graphical session", cx, gH - 56, C_DIM);
    ui_swap();
}

static void gui_startup_screen(void) {
    gui_startup_stage("Initializing display", 10);
    gui_delay_ms(180);
    gui_startup_stage("Loading accounts", 40);
    gui_delay_ms(180);
    gui_startup_stage("Preparing user session", 72);
    gui_delay_ms(180);
    gui_startup_stage("Starting desktop", 100);
    gui_delay_ms(260);
}

/* ============================================================
 * 桌面状态、窗口与任务栏
 * ============================================================ */

struct GuiWindow {
    int     open;
    ui_rect r;
    int     z;
    int     maximized;
    int     minimized;
    ui_rect restore;      /* 最大化前的位置，用于还原 */
};

static struct GuiWindow g_wins[GUI_WIN_MAX];
static int g_z_counter = 0;

/* ---- 原生应用窗口状态 ---- */
static const GuiExternalHost *g_ext_host = 0;
static int    g_ext_space_w = 960;
static int    g_ext_space_h = 620;
static char   g_ext_title[32] = "App";
static ui_rect g_ext_content;
static int    g_ext_content_valid = 0;

static void dirty_all(void);
static void dirty_add(int x, int y, int w, int h);
static int g_focus = -1;
static int g_dragging = -1;
static int g_drag_dx = 0;
static int g_drag_dy = 0;

/* 调整窗口大小：edge 用位标记，bit0 = 右边缘，bit1 = 下边缘，
 * 两者都置位就是右下角手柄。 */
static int g_resizing = -1;
static int g_resize_edge = 0;
static ui_rect g_resize_start;
static int g_resize_mx0 = 0;
static int g_resize_my0 = 0;

static int g_search_open = 0;
static char g_search[64];
static int g_search_len = 0;
static int g_search_sel = 0;

static int g_user_menu = 0;
static int g_power_menu = 0;
static int g_net_menu = 0;
static int g_pending_action = ACT_NONE;

struct GuiAppDef {
    const char *name;
    const char *desc;
    int         win;
    int         hidden;   /* 1 = 不在任务栏显示，只在搜索面板出现 */
};

static const struct GuiAppDef g_apps[GUI_APP_COUNT] = {
    { "Files",        "Browse your home directory",      WIN_FILES,    0 },
    { "Text Editor",  "Create and edit text files",       WIN_EDITOR,   0 },
    { "Terminal",     "Run shell commands in a window",   WIN_TERM,     0 },
    { "Browser",      "Browse http pages (HTML + CSS)",   WIN_BROWSER,  0 },
    { "Settings",     "Boot mode, clock and account",     WIN_SETTINGS, 0 },
    { "Task Manager", "CPU, memory, disk and processes",  WIN_TASKMGR,  0 },
    { "Calculator",   "Arithmetic with divide-by-zero guard", WIN_CALC, 0 },
    { "SylFault",     "Crash, dump and memory leak lab",  WIN_SLYZ,     1 }
};

/* 任务栏与启动器只排列可见应用；隐藏项不占槽位，因此 SylFault
 * 放在数组末尾不会在任务栏留下空档。 */
static int gui_visible_app_count(void) {
    int n = 0;
    for (int i = 0; i < GUI_APP_COUNT; i++) {
        if (!g_apps[i].hidden) n++;
    }
    return n;
}

/* 由文件管理器 / 编辑器 / 设置页实现（本文件后半部分） */
static void gui_files_open(const char *path);
static void gui_files_render(const ui_theme *t, ui_rect content);
static void gui_files_click(ui_rect content, int x, int y);
static void gui_files_key(char key);
static int  gui_files_modal_active(void);
static void gui_files_modal_render(const ui_theme *t, ui_rect window);
static int  gui_files_modal_key(char key);
static void gui_files_modal_click(ui_rect window, int x, int y);

static void gui_editor_open(const char *path);
static int  gui_editor_is_open(void);
static void gui_editor_render(const ui_theme *t, ui_rect content);
static void gui_editor_click(ui_rect content, int x, int y);
static void gui_editor_key(char key);
static void gui_editor_drag_to(ui_rect content, int x, int y);
static int  gui_editor_modal_active(void);
static void gui_editor_modal_render(const ui_theme *t, ui_rect window);
static int  gui_editor_modal_key(char key);
static void gui_editor_modal_click(ui_rect window, int x, int y);

/* 桌面（图标网格 + 右键菜单 + 新建提示）。
 * 实现放在文件管理器之后，这里先声明，供桌面渲染/点击/按键调用。 */
static void gui_desk_render(void);
static void gui_desk_click(int x, int y);
static int  gui_desk_key(char key);
static int  gui_desk_modal(void);
static void gui_desk_reset(void);
static void gui_desk_refresh(void);

/* Slyz GUI 程序运行时（声明式界面 + 动作绑定） */
static void gui_slyz_render(const ui_theme *t, ui_rect content);
static void gui_slyz_click(ui_rect content, int x, int y);
static void gui_slyz_key(char key);
static int  gui_slyz_modal(void);
static int  gui_slyz_is_program(const char *path);
static int  gui_slyz_open(const char *path);
static void gui_slyz_req_size(int *w, int *h);
static int  gui_app_open_path(const char *path);
static int  gui_ext_is(const char *path, const char *ext);
static int  gui_open_media(const char *path);

static void gui_settings_render(const ui_theme *t, ui_rect content);
static void gui_settings_click(ui_rect content, int x, int y);

/* 任务管理器：总览 / 进程 / 设备三个页面，数据每秒采样一次 */
static void gui_tm_render(const ui_theme *t, ui_rect content);
static void gui_tm_click(ui_rect content, int x, int y);
static void gui_tm_key(char key);
static int  gui_tm_sample(void);

/* 计算器：整数定点运算，除零单独进入错误态 */
static void gui_calc_render(const ui_theme *t, ui_rect content);
static void gui_calc_click(ui_rect content, int x, int y);
static void gui_calc_key(char key);

static void gui_term_render(const ui_theme *t, ui_rect content);
static void gui_term_click(ui_rect content, int x, int y);
static void gui_term_key(char key);
static void gui_term_scroll_by(int delta);
static void gui_term_reset(void);

static void gui_browser_render(const ui_theme *t, ui_rect content);
static void gui_browser_click(ui_rect content, int x, int y);
static void gui_browser_key(char key);
static void gui_browser_reset(void);

static ui_rect win_default_rect(int id) {
    ui_rect r;

    if (id == WIN_FILES) {
        r.w = 900;
        r.h = 600;
    } else if (id == WIN_EDITOR) {
        r.w = 960;
        r.h = 620;
    } else if (id == WIN_TERM) {
        r.w = 900;
        r.h = 600;
    } else if (id == WIN_BROWSER) {
        r.w = 1000;
        r.h = 660;
    } else if (id == WIN_SLYZ) {
        int w = 660;
        int h = 620;
        gui_slyz_req_size(&w, &h);
        r.w = w + 24;
        r.h = h + 90;
    } else if (id == WIN_TASKMGR) {
        r.w = 1040;
        r.h = 700;
    } else if (id == WIN_CALC) {
        r.w = 420;
        r.h = 560;
    } else if (id == WIN_MEDIA) {
        r.w = 600;
        r.h = 540;
    } else if (id == WIN_EXTERNAL) {
        r.w = g_ext_space_w + 20;
        r.h = g_ext_space_h + 66;
    } else {
        r.w = 640;
        r.h = 640;
    }

    int avail_w = gW - GUI_TASKBAR_W - 80;
    int avail_h = gH - 120;
    if (r.w > avail_w) r.w = avail_w;
    if (r.h > avail_h) r.h = avail_h;

    r.x = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W - r.w) / 2;
    r.y = (gH - r.h) / 2;
    return r;
}

static void win_open(int id) {
    if (!g_wins[id].open) {
        g_wins[id].open = 1;
        g_wins[id].r = win_default_rect(id);
        g_wins[id].maximized = 0;
        g_wins[id].minimized = 0;
    }
    g_wins[id].minimized = 0;
    g_wins[id].z = ++g_z_counter;
    g_focus = id;
}

static void win_focus(int id) {
    g_wins[id].z = ++g_z_counter;
    g_focus = id;
}

static void win_close(int id) {
    if (gui_app_active(id)) {
        gui_app_stop(id);
    }
    /* 原生应用窗口：先摘掉宿主再通知，避免 close 回调里再次
     * 请求关闭造成递归。 */
    if (id == WIN_EXTERNAL && g_ext_host) {
        const GuiExternalHost *h = g_ext_host;
        g_ext_host = 0;
        g_ext_content_valid = 0;
        if (h->close) h->close();
    }
    /* 媒体播放器没有独立进程：窗口关闭就停止音频，避免后台
     * 继续播放却找不到控制入口。 */
    if (id == WIN_MEDIA) {
        audio_stop();
    }
    g_wins[id].open = 0;
    g_wins[id].z = 0;
    g_wins[id].maximized = 0;
    g_wins[id].minimized = 0;
    if (g_focus == id) g_focus = -1;
    if (g_dragging == id) g_dragging = -1;
    if (g_resizing == id) g_resizing = -1;
    if (id == WIN_FILES || id == WIN_EDITOR) gui_desk_refresh();
}

/* ============================================================
 * 原生应用窗口（GuiExternalHost）
 * ============================================================ */

int gui_external_open(const char *title, int w, int h,
                      const GuiExternalHost *host) {
    if (!host) return 0;
    g_ext_host = host;
    g_ext_space_w = w > 240 ? w : 240;
    g_ext_space_h = h > 160 ? h : 160;
    int n = 0;
    if (title) {
        while (title[n] && n < (int)sizeof(g_ext_title) - 1) {
            g_ext_title[n] = title[n];
            n++;
        }
    }
    g_ext_title[n] = '\0';
    g_ext_content_valid = 0;
    win_open(WIN_EXTERNAL);
    dirty_all();
    return 1;
}

void gui_external_close(void) {
    if (g_wins[WIN_EXTERNAL].open) {
        win_close(WIN_EXTERNAL);
    } else if (g_ext_host) {
        const GuiExternalHost *h = g_ext_host;
        g_ext_host = 0;
        g_ext_content_valid = 0;
        if (h->close) h->close();
    }
    dirty_all();
}

int gui_external_is_open(void) {
    return g_wins[WIN_EXTERNAL].open && g_ext_host != 0;
}

uint32_t gui_ext_width(void) {
    /* 窗口没打开时必须返回 0：TTY 会话里应用靠这个判断该走
     * 全屏 fb_* 后端，而不是画到不存在的窗口内容区。 */
    if (!g_wins[WIN_EXTERNAL].open) return 0;
    if (g_ext_content_valid && g_ext_content.w > 0) {
        return (uint32_t)g_ext_content.w;
    }
    return (uint32_t)g_ext_space_w;
}

uint32_t gui_ext_height(void) {
    if (!g_wins[WIN_EXTERNAL].open) return 0;
    if (g_ext_content_valid && g_ext_content.h > 0) {
        return (uint32_t)g_ext_content.h;
    }
    return (uint32_t)g_ext_space_h;
}

void gui_ext_invalidate(void) {
    /* 只标脏内容区：打字时不必重绘标题栏与阴影，逐个按键整窗重绘
     * 在 QEMU 的软件渲染下会明显掉帧。 */
    if (g_wins[WIN_EXTERNAL].open && g_ext_content_valid) {
        dirty_add(g_ext_content.x, g_ext_content.y,
                  g_ext_content.w, g_ext_content.h);
    }
}

void gui_ext_request_close(void) {
    gui_external_close();
}

void gui_ext_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_ext_content_valid || w <= 0 || h <= 0) return;
    int x0 = g_ext_content.x + x;
    int y0 = g_ext_content.y + y;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < g_ext_content.x) x0 = g_ext_content.x;
    if (y0 < g_ext_content.y) y0 = g_ext_content.y;
    if (x1 > g_ext_content.x + g_ext_content.w) {
        x1 = g_ext_content.x + g_ext_content.w;
    }
    if (y1 > g_ext_content.y + g_ext_content.h) {
        y1 = g_ext_content.y + g_ext_content.h;
    }
    if (x1 <= x0 || y1 <= y0) return;
    vbe_fill_rect((uint16_t)x0, (uint16_t)y0,
                  (uint16_t)(x1 - x0), (uint16_t)(y1 - y0), color);
}

int gui_ext_draw_cell(int x, int y, int cw, int ch,
                      uint32_t cp, uint32_t fg) {
    if (!g_ext_content_valid) return 0;
    int x0 = g_ext_content.x + x;
    int y0 = g_ext_content.y + y;
    if (x0 + cw <= g_ext_content.x || y0 + ch <= g_ext_content.y) return 0;
    if (x0 >= g_ext_content.x + g_ext_content.w) return 0;
    if (y0 >= g_ext_content.y + g_ext_content.h) return 0;
    vbe_push_clip(g_ext_content.x, g_ext_content.y,
                  g_ext_content.w, g_ext_content.h);
    int r = slyertype_draw_cell_buf(x0, y0, cw, ch, cp, fg);
    vbe_pop_clip();
    return r;
}

/* 最小化：保留 open 与内容，只从桌面与命中测试里移除；
 * 任务栏的窗口按钮仍然可点，点一下还原。 */
static void win_minimize(int id) {
    g_wins[id].minimized = 1;
    if (g_focus == id) g_focus = -1;
    if (g_dragging == id) g_dragging = -1;
    if (g_resizing == id) g_resizing = -1;
}

static void win_restore_from_min(int id) {
    g_wins[id].minimized = 0;
}

static ui_rect win_max_rect(void) {
    ui_rect r;
    r.x = GUI_TASKBAR_W + 8;
    r.y = 8;
    r.w = gW - GUI_TASKBAR_W - 16;
    r.h = gH - 16;
    return r;
}

static void win_toggle_max(int id) {
    struct GuiWindow *w = &g_wins[id];

    if (w->maximized) {
        w->r = w->restore;
        w->maximized = 0;
    } else {
        w->restore = w->r;
        w->r = win_max_rect();
        w->maximized = 1;
    }

    w->minimized = 0;
    win_focus(id);
}

static ui_rect win_content_rect(const ui_rect *r) {
    ui_rect c;
    c.x = r->x + 10;
    c.y = r->y + 56;
    c.w = r->w - 20;
    c.h = r->h - 66;
    return c;
}

static ui_rect win_close_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 36;
    c.h = 34;
    c.x = r->x + r->w - 50;
    c.y = r->y + 9;
    return c;
}

static ui_rect win_max_btn_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 36;
    c.h = 34;
    c.x = r->x + r->w - 50 - 42;
    c.y = r->y + 9;
    return c;
}

static ui_rect win_min_btn_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 36;
    c.h = 34;
    c.x = r->x + r->w - 50 - 84;
    c.y = r->y + 9;
    return c;
}

/* 右下角调整手柄：18x18，画三条斜线作为视觉提示 */
static ui_rect win_grip_rect(const ui_rect *r) {
    ui_rect c;
    c.w = 18;
    c.h = 18;
    c.x = r->x + r->w - 18;
    c.y = r->y + r->h - 18;
    return c;
}

/* 判定鼠标落在窗口的哪个调整边上；0 表示不在调整区 */
static int win_resize_edge_at(const ui_rect *r, int x, int y, int maximized) {
    if (maximized) return 0;

    int edge = 0;

    if (x >= r->x + r->w - 8 && x < r->x + r->w &&
        y >= r->y + 52 && y < r->y + r->h) {
        edge |= 1;
    }
    if (y >= r->y + r->h - 8 && y < r->y + r->h &&
        x >= r->x + 8 && x < r->x + r->w) {
        edge |= 2;
    }

    return edge;
}

static int win_topmost_at(int x, int y) {
    int best = -1;
    int best_z = -1;

    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (!g_wins[i].open || g_wins[i].minimized) continue;
        if (g_point_in(&g_wins[i].r, x, y) && g_wins[i].z > best_z) {
            best_z = g_wins[i].z;
            best = i;
        }
    }
    return best;
}

/* ---------- 任务栏几何 ----------
 * 竖直空间预算（1600x900）：头像 20..76、搜索 88..144、
 * 5 个应用按钮 156..456、窗口列表 470..700、
 * 网络 gH-192、时钟 gH-128、电源 gH-64。
 * 屏幕矮于 760 时窗口列表会与底部区重叠，此时按 gH 动态压缩。 */

static ui_rect tb_avatar_rect(void) {
    return (ui_rect){ 16, 20, 56, 56 };
}

static ui_rect tb_search_rect(void) {
    return (ui_rect){ 16, 88, 56, 56 };
}

static ui_rect tb_app_rect(int i) {
    return (ui_rect){ 16, 156 + i * 54, 56, 50 };
}

static ui_rect tb_win_rect(int i) {
    /* 已打开窗口用 2 列紧凑网格：7 个应用占掉 156..530 后，
     * 单列 46px 会把第 4 个窗口按钮推到网络按钮下面。 */
    return (ui_rect){ 12 + (i % 2) * 34, 544 + (i / 2) * 30, 30, 26 };
}

static ui_rect tb_net_rect(void) {
    return (ui_rect){ 16, gH - 200, 56, 48 };
}

static ui_rect tb_clock_rect(void) {
    return (ui_rect){ 16, gH - 140, 56, 64 };
}

static ui_rect tb_power_rect(void) {
    return (ui_rect){ 16, gH - 64, 56, 48 };
}

static ui_rect search_panel_rect(void) {
    /* 高度随应用数量增长：固定 500 在应用超过 5 个后会把最后一行
     * 画到面板外面。上限夹在屏幕内，保证任何分辨率都不越界。 */
    int h = 120 + GUI_APP_COUNT * 72 + 16;
    if (96 + h > gH - 16) h = gH - 16 - 96;
    if (h < 240) h = 240;
    return (ui_rect){ GUI_TASKBAR_W + 10, 96, 440, h };
}

static ui_rect user_menu_rect(void) {
    return (ui_rect){ GUI_TASKBAR_W + 8, 20, 300, 176 };
}

static ui_rect power_menu_rect(void) {
    return (ui_rect){ GUI_TASKBAR_W + 8, gH - 250, 300, 230 };
}

static ui_rect net_menu_rect(void) {
    return (ui_rect){ GUI_TASKBAR_W + 8, gH - 560, 520, 520 };
}

static ui_rect volume_slider_rect(const ui_rect *p) {
    return (ui_rect){ p->x + 20, p->y + 128, p->w - 40, 14 };
}

/* ---------- 搜索 ---------- */

static int search_filter(int out[GUI_APP_COUNT]) {
    int n = 0;
    for (int i = 0; i < GUI_APP_COUNT; i++) {
        if (g_search_len == 0 ||
            g_contains_nocase(g_apps[i].name, g_search) ||
            g_contains_nocase(g_apps[i].desc, g_search)) {
            out[n++] = i;
        }
    }
    return n;
}

static void launch_app(int win) {
    if (win == WIN_FILES) {
        const struct AuthUser *u = auth_current();
        /* 文件管理器已迁移为独立 SLYZ 程序；镜像缺失时回退内置实现，
         * 保证系统盘被替换或损坏时桌面仍然可用。 */
        if (!gui_app_start(WIN_FILES, "system\\apps\\files.slyz")) {
            gui_files_open(u ? u->home : "");
        }
    } else if (win == WIN_EDITOR) {
        gui_editor_open("");
    } else if (win == WIN_TERM) {
        gui_term_reset();
    } else if (win == WIN_BROWSER) {
        gui_browser_reset();
    } else if (win == WIN_CALC) {
        /* 计算器已迁移为独立 SLYZ 程序。镜像缺失或损坏时保留内置实现，
         * 桌面不会因为一个应用文件丢失而无法使用。 */
        gui_app_start(WIN_CALC, "system\\apps\\calc.slyz");
    } else if (win == WIN_SETTINGS) {
        gui_app_start(WIN_SETTINGS, "system\\apps\\settings.slyz");
    } else if (win == WIN_TASKMGR) {
        gui_app_start(WIN_TASKMGR, "system\\apps\\taskmgr.slyz");
    } else if (win == WIN_SLYZ) {
        /* SylFault：开发工具，不在任务栏出现，只能从搜索启动。
         * 镜像缺失时窗口仍会打开并显示通用 Slyz 页，便于诊断系统盘。 */
        gui_app_start(WIN_SLYZ, "system\\apps\\sylfault.slyz");
    }
    win_open(win);
    g_search_open = 0;
}

static int gui_hover_id(void) {
    int x = g_mx;
    int y = g_my;

    if (g_search_open) {
        ui_rect p = search_panel_rect();
        return g_point_in(&p, x, y) ? 60 : 0;
    }
    if (g_user_menu) {
        ui_rect p = user_menu_rect();
        return g_point_in(&p, x, y) ? 61 : 0;
    }
    if (g_power_menu) {
        ui_rect p = power_menu_rect();
        return g_point_in(&p, x, y) ? 62 : 0;
    }
    if (g_net_menu) {
        ui_rect p = net_menu_rect();
        return g_point_in(&p, x, y) ? 63 : 0;
    }

    if (x < GUI_TASKBAR_W) {
        ui_rect av = tb_avatar_rect();
        if (g_point_in(&av, x, y)) return 1;
        ui_rect sr = tb_search_rect();
        if (g_point_in(&sr, x, y)) return 2;
        for (int i = 0; i < GUI_APP_COUNT; i++) {
            if (g_apps[i].hidden) continue;
            ui_rect r = tb_app_rect(i);
            if (g_point_in(&r, x, y)) return 10 + i;
        }
        for (int i = 0; i < GUI_WIN_MAX; i++) {
            if (!g_wins[i].open || i == WIN_MEDIA) continue;
            ui_rect r = tb_win_rect(i);
            if (g_point_in(&r, x, y)) return 20 + i;
        }
        ui_rect nr = tb_net_rect();
        if (g_point_in(&nr, x, y)) return 5;
        ui_rect p = tb_power_rect();
        if (g_point_in(&p, x, y)) return 3;
        return 4;
    }

    int w = win_topmost_at(x, y);
    if (w >= 0) {
        ui_rect cr = win_close_rect(&g_wins[w].r);
        if (g_point_in(&cr, x, y)) return 40 + w;
        return 50 + w;
    }
    return 0;
}

/* hover id 反过来映射到"该控件占据的矩形"，局部重绘据此擦除旧高亮。
 * 返回 1 表示该 id 有明确矩形。 */
static int gui_hover_rect(int id, ui_rect *out) {
    if (id == 1) {
        *out = tb_avatar_rect();
        return 1;
    }
    if (id == 2) {
        *out = tb_search_rect();
        return 1;
    }
    if (id == 3) {
        *out = tb_power_rect();
        return 1;
    }
    if (id == 5) {
        *out = tb_net_rect();
        return 1;
    }
    if (id == 4) {
        *out = (ui_rect){ 0, 0, GUI_TASKBAR_W, gH };
        return 1;
    }
    if (id >= 10 && id < 13) {
        *out = tb_app_rect(id - 10);
        return 1;
    }
    if (id >= 20 && id < 20 + GUI_WIN_MAX) {
        *out = tb_win_rect(id - 20);
        return 1;
    }
    if (id >= 40 && id < 40 + GUI_WIN_MAX) {
        *out = win_close_rect(&g_wins[id - 40].r);
        return 1;
    }
    if (id >= 50 && id < 50 + GUI_WIN_MAX) {
        *out = g_wins[id - 50].r;
        return 1;
    }
    if (id == 60) {
        *out = search_panel_rect();
        return 1;
    }
    if (id == 61) {
        *out = user_menu_rect();
        return 1;
    }
    if (id == 62) {
        *out = power_menu_rect();
        return 1;
    }
    if (id == 63) {
        *out = net_menu_rect();
        return 1;
    }
    return 0;
}

/* ---------- 任务栏渲染 ---------- */

static void gui_taskbar_render(void) {
    ui_rect bar = { 0, 0, GUI_TASKBAR_W, gH };
    ui_gradient_v(bar, 0x111A26, 0x0B121B);
    ui_vline(GUI_TASKBAR_W - 1, 0, gH, C_BORDER);

    const struct AuthUser *u = auth_current();
    const char *uname = u ? u->name : "?";
    ui_rect av = tb_avatar_rect();
    gui_avatar(av.x + av.w / 2, av.y + av.h / 2, 28, uname,
               C_AVATAR_A, C_AVATAR_B);

    ui_rect sr = tb_search_rect();
    int sr_hover = g_point_in(&sr, g_mx, g_my);
    hunt_add(sr);
    ui_round_panel(sr, 14, sr_hover ? C_ACCENT : C_BORDER,
                   g_search_open ? 0x1B2A3C : C_PANEL2);
    icon_search(sr.x + 28, sr.y + 26, 12, sr_hover ? C_ACCENT3 : C_DIM);

    for (int i = 0; i < GUI_APP_COUNT; i++) {
        if (g_apps[i].hidden) continue;
        ui_rect r = tb_app_rect(i);
        int hover = g_point_in(&r, g_mx, g_my);
        int open = g_wins[g_apps[i].win].open;

        hunt_add(r);
        ui_round_panel(r, 12, hover ? C_ACCENT : (open ? C_ACCENT2 : C_BORDER),
                       hover ? 0x1B2A3C : C_PANEL2);

        int ix = r.x + 14;
        int iy = r.y + 10;
        uint32_t ic = open ? C_ACCENT3 : C_DIM;
        if (g_apps[i].win == WIN_FILES) {
            icon_folder(ix, iy, 28, ic);
        } else if (g_apps[i].win == WIN_EDITOR) {
            icon_editor(ix, iy, 28, ic);
        } else if (g_apps[i].win == WIN_TERM) {
            icon_terminal(ix, iy, 28, ic);
        } else if (g_apps[i].win == WIN_BROWSER) {
            icon_globe(ix, iy, 28, ic);
        } else if (g_apps[i].win == WIN_TASKMGR) {
            icon_chart(ix, iy, 28, ic);
        } else if (g_apps[i].win == WIN_CALC) {
            icon_calc(ix, iy, 28, ic);
        } else {
            icon_settings(ix, iy, 28, ic);
        }

        if (open) {
            vbe_fill_rect((uint16_t)(r.x + 8), (uint16_t)(r.y + r.h - 3),
                          40, 3, C_ACCENT);
        }
    }

    int any_open = 0;
    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (g_wins[i].open && i != WIN_MEDIA) any_open = 1;
    }
    if (any_open) {
        ui_hline(16, 156 + gui_visible_app_count() * 54 + 4, 56, C_BORDER);
    }

    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (!g_wins[i].open || i == WIN_MEDIA) continue;
        ui_rect r = tb_win_rect(i);
        int hover = g_point_in(&r, g_mx, g_my);
        int active = (g_focus == i);
        int minimized = g_wins[i].minimized;

        hunt_add(r);
        ui_round_panel(r, 10,
                       active ? C_ACCENT : (hover ? C_ACCENT2 : C_BORDER),
                       minimized ? 0x141C27 : (active ? 0x1B2A3C : C_PANEL));

        int ix = r.x + 7;
        int iy = r.y + 5;
        uint32_t ic = minimized ? 0x5A6B7E : (active ? C_TEXT : C_DIM);
        if (i == WIN_FILES) {
            icon_folder(ix, iy, 16, ic);
        } else if (i == WIN_EDITOR) {
            icon_file(ix, iy, 16, ic);
        } else if (i == WIN_TERM) {
            icon_terminal(ix, iy, 16, ic);
        } else if (i == WIN_BROWSER) {
            icon_globe(ix, iy, 16, ic);
        } else if (i == WIN_TASKMGR) {
            icon_chart(ix, iy, 16, ic);
        } else if (i == WIN_CALC) {
            icon_calc(ix, iy, 16, ic);
        } else if (i == WIN_EXTERNAL) {
            icon_editor(ix, iy, 16, ic);
        } else {
            icon_settings(ix, iy, 16, ic);
        }
    }

    /* 网络状态：绿点 = 链路就绪且已配置，红点 = 不可用 */
    ui_rect nr = tb_net_rect();
    int nr_hover = g_point_in(&nr, g_mx, g_my);
    hunt_add(nr);
    int netok = net_ready();
    uint32_t net_color = netok ? C_OK : C_ERR;
    ui_round_panel(nr, 12, nr_hover ? C_ACCENT : C_BORDER,
                   nr_hover ? 0x1B2A3C : C_PANEL2);
    icon_net(nr.x + 14, nr.y + 8, 28, nr_hover ? C_ACCENT3 : C_DIM);
    ui_round_fill((ui_rect){ nr.x + 36, nr.y + 34, 9, 9 }, 4, net_color);

    ui_rect pr = tb_power_rect();
    int pr_hover = g_point_in(&pr, g_mx, g_my);
    hunt_add(pr);
    ui_round_panel(pr, 12, pr_hover ? C_ERR : C_BORDER,
                   pr_hover ? 0x2A1719 : C_PANEL2);
    icon_power(pr.x + 28, pr.y + 24, 10, pr_hover ? C_ERR : C_DIM);

    /* 任务栏时钟：两行显示，56px 宽刚好容纳两个数字，
     * 不会像单行 "HH:MM"（80px）那样超出任务栏宽度。 */
    char clock[6];
    gui_clock_text(clock);
    ui_rect clock_box = tb_clock_rect();
    ui_round_panel(clock_box, 12, C_BORDER, C_PANEL2);

    char hh[3];
    char mm[3];
    hh[0] = clock[0]; hh[1] = clock[1]; hh[2] = '\0';
    mm[0] = clock[3]; mm[1] = clock[4]; mm[2] = '\0';
    ui_text_transparent(clock_box.x + 12, clock_box.y + 1, hh, C_TEXT);
    ui_text_transparent(clock_box.x + 12, clock_box.y + 33, mm, C_DIM);
}

/* ---------- 窗口渲染 ---------- */

static void gui_window_frame(int id, const ui_theme *t) {
    ui_rect r = g_wins[id].r;

    ui_shadow(r, 16, 10);
    ui_round_gradient(r, 16, 0x1A2431, 0x111925);
    ui_round_panel(r, 16, (g_focus == id) ? 0x3B5570 : C_BORDER, 0x121A25);

    /* 标题栏 */
    ui_rect title = { r.x + 2, r.y + 2, r.w - 4, 50 };
    ui_round_fill(title, 14, (g_focus == id) ? 0x1E2A39 : 0x18212D);
    ui_hline(r.x + 16, r.y + 52, r.w - 32, C_BORDER);

    int ix = r.x + 18;
    int iy = r.y + 15;
    if (id == WIN_FILES) {
        icon_folder(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_EDITOR) {
        icon_file(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_TERM) {
        icon_terminal(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_BROWSER) {
        icon_globe(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_SLYZ) {
        icon_editor(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_TASKMGR) {
        icon_chart(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_CALC) {
        icon_calc(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_MEDIA) {
        icon_chart(ix, iy, 24, C_ACCENT3);
    } else if (id == WIN_EXTERNAL) {
        icon_editor(ix, iy, 24, C_ACCENT3);
    } else {
        icon_settings(ix, iy, 24, C_ACCENT3);
    }

    const char *custom_title = (id == WIN_SLYZ) ? gui_app_title(WIN_SLYZ) :
                               (id == WIN_EXTERNAL) ? g_ext_title : 0;
    const char *title_text = custom_title ? custom_title :
                             id == WIN_FILES ? "Files" :
                             id == WIN_EDITOR ? "Text Editor" :
                             id == WIN_TERM ? "Terminal" :
                             id == WIN_BROWSER ? "Browser" :
                             id == WIN_SLYZ ? "Slyz App" :
                             id == WIN_TASKMGR ? "Task Manager" :
                             id == WIN_CALC ? "Calculator" :
                             id == WIN_MEDIA ? "Media Player" : "Settings";
    ui_text_transparent(r.x + 54, r.y + 10, title_text, C_TEXT);

    ui_rect cr = win_close_rect(&r);
    int cr_hover = g_point_in(&cr, g_mx, g_my);
    hunt_add(cr);
    ui_round_fill(cr, 8, cr_hover ? 0x7F1D1D : 0x1B2532);
    icon_close(cr.x + 8, cr.y + 7, 20, cr_hover ? 0xFFFFFF : C_DIM);

    ui_rect mr = win_max_btn_rect(&r);
    int mr_hover = g_point_in(&mr, g_mx, g_my);
    hunt_add(mr);
    ui_round_fill(mr, 8, mr_hover ? 0x24405F : 0x1B2532);
    if (g_wins[id].maximized) {
        icon_restore(mr.x + 8, mr.y + 7, 20, mr_hover ? 0xFFFFFF : C_DIM);
    } else {
        icon_maximize(mr.x + 8, mr.y + 7, 20, mr_hover ? 0xFFFFFF : C_DIM);
    }

    ui_rect nr = win_min_btn_rect(&r);
    int nr_hover = g_point_in(&nr, g_mx, g_my);
    hunt_add(nr);
    ui_round_fill(nr, 8, nr_hover ? 0x24405F : 0x1B2532);
    icon_minimize(nr.x + 8, nr.y + 7, 20, nr_hover ? 0xFFFFFF : C_DIM);

    ui_rect c = win_content_rect(&r);
    if (id == WIN_FILES) {
        gui_files_render(t, c);
    } else if (id == WIN_EDITOR) {
        gui_editor_render(t, c);
    } else if (id == WIN_TERM) {
        gui_term_render(t, c);
    } else if (id == WIN_BROWSER) {
        gui_browser_render(t, c);
    } else if (id == WIN_SLYZ) {
        /* 通用 SLYZ 窗口：有独立应用时由虚拟机绘制，
         * 否则回退到旧的声明式界面渲染。 */
        if (!gui_app_render(WIN_SLYZ, c)) {
            gui_slyz_render(t, c);
        }
    } else if (id == WIN_TASKMGR) {
        gui_tm_render(t, c);
    } else if (id == WIN_CALC) {
        gui_calc_render(t, c);
    } else if (id == WIN_MEDIA) {
        if (!gui_app_render(WIN_MEDIA, c)) {
            vbe_fill_rect((uint16_t)c.x, (uint16_t)c.y, (uint16_t)c.w,
                          (uint16_t)c.h, 0x0B1017);
        }
    } else if (id == WIN_EXTERNAL) {
        /* 原生预装应用：只画内容区，标题栏/边框/层级由窗口系统负责。
         * 内容区尺寸变化时先通知应用重新布局，再让它绘制。 */
        if (g_ext_host) {
            int resized = (!g_ext_content_valid ||
                           g_ext_content.w != c.w || g_ext_content.h != c.h);
            g_ext_content = c;
            g_ext_content_valid = 1;
            if (resized && g_ext_host->resize) {
                g_ext_host->resize(c.w, c.h);
            }
            if (g_ext_host->render) {
                g_ext_host->render(c.x, c.y, c.w, c.h);
            }
        } else {
            vbe_fill_rect((uint16_t)c.x, (uint16_t)c.y, (uint16_t)c.w,
                          (uint16_t)c.h, 0x0B1017);
        }
    } else {
        gui_settings_render(t, c);
    }

    /* 右下角调整手柄：三条斜线，最大化的窗口不显示 */
    if (!g_wins[id].maximized) {
        ui_rect grip = win_grip_rect(&r);
        int grip_hover = g_point_in(&grip, g_mx, g_my);
        hunt_add(grip);

        uint32_t gc = grip_hover ? C_ACCENT3 : 0x3E4E62;
        for (int k = 0; k < 3; k++) {
            int off = 5 + k * 4;
            ui_line(grip.x + off, grip.y + grip.h - 4,
                    grip.x + grip.w - 4, grip.y + off, gc);
        }
    }

    if (id == WIN_FILES && gui_files_modal_active()) {
        gui_files_modal_render(t, r);
    } else if (id == WIN_EDITOR && gui_editor_modal_active()) {
        gui_editor_modal_render(t, r);
    }
}

/* ---------- 搜索面板 ---------- */

static void gui_search_panel_render(void) {
    ui_rect p = search_panel_rect();
    ui_shadow(p, 16, 10);
    ui_round_panel(p, 16, C_BORDER, 0x121A25);

    ui_text_transparent(p.x + 20, p.y + 12, "SEARCH", C_DIM);

    ui_rect box = { p.x + 16, p.y + 50, p.w - 32, 52 };
    ui_round_panel(box, 10, C_ACCENT, 0x0C141E);
    if (g_search_len > 0) {
        gui_text_n(box.x + 14, box.y + 10, g_search, 24, C_TEXT);
    } else {
        ui_text_transparent(box.x + 14, box.y + 10, "type an app name", C_DIM);
    }
    if (((net_now_ms() / 500u) & 1u) == 0) {
        vbe_fill_rect((uint16_t)(box.x + 14 + g_search_len * 16),
                      (uint16_t)(box.y + 12), 2, 28, C_ACCENT);
    }

    int matches[GUI_APP_COUNT];
    int n = search_filter(matches);

    int y = p.y + 120;
    for (int i = 0; i < n; i++) {
        const struct GuiAppDef *a = &g_apps[matches[i]];
        ui_rect row = { p.x + 12, y, p.w - 24, 64 };
        int hover = g_point_in(&row, g_mx, g_my);
        hunt_add(row);
        if (i == g_search_sel) {
            ui_round_fill(row, 10, 0x1E3048);
        } else if (hover) {
            ui_round_fill(row, 10, 0x18232F);
        }

        ui_rect icon = { row.x + 12, row.y + 16, 32, 32 };
        if (a->win == WIN_FILES) {
            icon_folder(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->win == WIN_EDITOR) {
            icon_editor(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->win == WIN_TASKMGR) {
            icon_chart(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->win == WIN_CALC) {
            icon_calc(icon.x, icon.y, 32, C_ACCENT3);
        } else if (a->hidden) {
            icon_fault(icon.x, icon.y, 32, C_WARN);
        } else {
            icon_settings(icon.x, icon.y, 32, C_ACCENT3);
        }

        ui_text_transparent(row.x + 60, row.y + 4, LTR(a->name), C_TEXT);
        gui_text_n(row.x + 60, row.y + 34, LTR(a->desc), 20, C_DIM);
        y += 72;
    }

    if (n == 0) {
        ui_text_transparent(p.x + 24, p.y + 140, "No applications found.", C_DIM);
    }
}

static void gui_user_menu_render(void) {
    ui_rect p = user_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, 0x121A25);

    const struct AuthUser *u = auth_current();
    gui_avatar(p.x + 38, p.y + 40, 22, u ? u->name : "?", C_AVATAR_A, C_AVATAR_B);
    ui_text_transparent(p.x + 70, p.y + 14, u ? u->name : "?", C_TEXT);

    char uid_text[24];
    int n = g_utoa((uint32_t)(u ? u->uid : 0), uid_text);
    g_copy(uid_text + n, "  (uid)", 8);

    ui_text_transparent(p.x + 70, p.y + 48, uid_text, C_DIM);
    ui_hline(p.x + 16, p.y + 86, p.w - 32, C_BORDER);

    ui_rect out = { p.x + 12, p.y + 98, p.w - 24, 54 };
    int hover = g_point_in(&out, g_mx, g_my);
    hunt_add(out);
    if (hover) ui_round_fill(out, 9, 0x1B2836);
    ui_text_transparent(out.x + 16, out.y + 11, "Log Out", C_TEXT);
}

static void gui_power_menu_render(void) {
    ui_rect p = power_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, 0x121A25);

    ui_text_transparent(p.x + 20, p.y + 14, LTR("POWER"), C_DIM);
    ui_hline(p.x + 16, p.y + 52, p.w - 32, C_BORDER);

    const char *items[4] = { "Restart", "Shut Down", "Exit to Terminal", "Cancel" };
    for (int i = 0; i < 4; i++) {
        ui_rect row = { p.x + 12, p.y + 60 + i * 42, p.w - 24, 38 };
        int hover = g_point_in(&row, g_mx, g_my);
        hunt_add(row);
        if (hover) ui_round_fill(row, 8, i == 1 ? 0x3A1A1C : 0x1B2836);
        ui_text_transparent(row.x + 16, row.y + 3, LTR(items[i]),
                            i == 1 ? (hover ? 0xFFB4B4 : C_ERR) : C_TEXT);
    }
}

/* 网络状态面板：链路、地址、速率与收发计数 */
static ui_rect net_menu_close_rect(const ui_rect *p) {
    return (ui_rect){ p->x + p->w - 132, p->y + 10, 116, 34 };
}

static void gui_net_menu_render(void) {
    ui_rect p = net_menu_rect();
    ui_shadow(p, 14, 8);
    ui_round_panel(p, 14, C_BORDER, 0x121A25);

    struct NetInfo *ni = net_info();
    int ready = net_ready();

    ui_text_transparent(p.x + 20, p.y + 14, "CONTROL CENTER", C_DIM);

    ui_rect rf = net_menu_close_rect(&p);
    int rf_hover = g_point_in(&rf, g_mx, g_my);
    gui_button(rf, "Close", rf_hover, 0);

    ui_hline(p.x + 16, p.y + 52, p.w - 32, C_BORDER);

    /* 状态行 */
    uint32_t st_color = ready ? C_OK : C_ERR;
    ui_round_fill((ui_rect){ p.x + 20, p.y + 66, 10, 10 }, 5, st_color);
    ui_text_transparent(p.x + 40, p.y + 56,
                        ready ? "Connected" : "Not connected",
                        st_color);

    /* 音量：软件增益，控制所有 MP3 播放 */
    int vol = audio_volume();
    ui_text_transparent(p.x + 20, p.y + 96, "VOLUME", C_DIM);

    char vol_text[8];
    int vn = g_utoa((uint32_t)vol, vol_text);
    vol_text[vn++] = '%';
    vol_text[vn] = '\0';
    ui_text_transparent(p.x + p.w - 40 - g_strlen(vol_text) * 16,
                        p.y + 96, vol_text, C_TEXT);

    ui_rect vs = volume_slider_rect(&p);
    hunt_add(vs);
    ui_round_fill(vs, 7, 0x223041);
    int fillw = (vs.w * vol) / 100;
    if (fillw > 0) {
        ui_round_fill((ui_rect){ vs.x, vs.y, fillw, vs.h }, 7, C_ACCENT);
    }
    int kx = vs.x + fillw;
    if (kx < vs.x + 7) kx = vs.x + 7;
    if (kx > vs.x + vs.w - 7) kx = vs.x + vs.w - 7;
    ui_round_fill((ui_rect){ kx - 7, vs.y - 3, 14, 20 }, 7, 0xF2F6FF);

    ui_hline(p.x + 16, p.y + 160, p.w - 32, C_BORDER);

    char line[96];
    char ip_text[16];
    int max_chars = (p.w - 40) / 16;

    if (ni) {
        /* 单列布局：两列在窄面板里会让右侧文本越过边界。
         * 全部字段一行一个，统一按面板宽度限字符数。 */
        int y = p.y + 182;

        net_format_ipv4(ni->ip, ip_text);
        g_copy(line, "IP      ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        net_format_ipv4(ni->netmask, ip_text);
        g_copy(line, "Mask    ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        net_format_ipv4(ni->gateway, ip_text);
        g_copy(line, "Gateway ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        net_format_ipv4(ni->dns, ip_text);
        g_copy(line, "DNS     ", sizeof(line));
        g_copy(line + 8, ip_text, 14);
        gui_text_n(p.x + 20, y, line, max_chars, C_TEXT);
        y += 32;

        char mac[18];
        net_format_mac(ni->mac, mac);
        g_copy(line, "MAC     ", sizeof(line));
        g_copy(line + 8, mac, 18);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 32;

        line[0] = '\0';
        int pos = 0;
        g_copy(line, "Speed   ", 10); pos = 8;
        pos += g_utoa(ni->speed_mbps, line + pos);
        g_copy(line + pos, " Mbps", 8);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 32;

        /* 收发统计 */
        pos = 0;
        g_copy(line, "RX ", 4); pos = 3;
        pos += g_utoa(ni->rx_packets, line + pos);
        g_copy(line + pos, " pkts / ", 10); pos += 8;
        pos += g_utoa(ni->rx_bytes, line + pos);
        g_copy(line + pos, " B", 3);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 32;

        pos = 0;
        g_copy(line, "TX ", 4); pos = 3;
        pos += g_utoa(ni->tx_packets, line + pos);
        g_copy(line + pos, " pkts / ", 10); pos += 8;
        pos += g_utoa(ni->tx_bytes, line + pos);
        g_copy(line + pos, " B", 3);
        gui_text_n(p.x + 20, y, line, max_chars, C_DIM);
        y += 36;

        if (!ready) {
            ui_text_wrap(p.x + 20, y, p.w - 40, 34,
                         "No link. Check that an e1000 adapter is attached.",
                         C_WARN, 2);
        }
    } else {
        ui_text_wrap(p.x + 20, p.y + 96, p.w - 40, 34,
                     "Network stack has not been initialized yet.", C_DIM, 2);
    }
}

/* ---------- 桌面整体渲染 ---------- */

static int gui_window_zorder(int *order) {
    int n = 0;
    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (g_wins[i].open && !g_wins[i].minimized) order[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && g_wins[order[j]].z > g_wins[v].z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    return n;
}

static void gui_clock_badge(void) {
    ui_rect badge = { gW - 116, 16, 100, 44 };
    ui_round_panel(badge, 12, C_BORDER, 0x121A25);
    char clock[6];
    gui_clock_text(clock);
    gui_text_center(clock, badge.x + badge.w / 2, badge.y + 6, C_TEXT);
}

static void gui_desktop_render(void) {
    gui_background();
    gui_clock_badge();

    /* 桌面图标位于窗口之下：窗口是前景应用，绝不能被子图标盖住。
     * 顺序必须与 gui_render_dirty_surfaces 完全一致，否则全屏刷新与
     * 局部刷新会得到不同的层级结果。 */
    gui_desk_render();

    /* 窗口按 z 值从低到高绘制 */
    int order[GUI_WIN_MAX];
    int n = gui_window_zorder(order);
    for (int i = 0; i < n; i++) {
        gui_window_frame(order[i], &g_theme);
    }

    gui_taskbar_render();

    if (g_search_open) gui_search_panel_render();
    if (g_user_menu) gui_user_menu_render();
    if (g_power_menu) gui_power_menu_render();
    if (g_net_menu) gui_net_menu_render();
}

/* ============================================================
 * 动态脏矩形
 * ------------------------------------------------------------
 * 全屏 blit（1024x768 约 78 万像素）是旧版卡顿的主因。现在每帧只
 * 收集真正变化的矩形，重绘时用 vbe_set_clip 把整棵场景树的绘制限
 * 制在单个矩形内，表面包围盒与当前裁剪不相交就整块跳过，最后逐
 * 矩形 vbe_swap_rect 上屏。
 *
 * 画面不残留脏点由三层保证：
 *   1. 每个改变外观的事件都显式 dirty_add 出它影响的区域；
 *   2. 状态指纹兜底：结构性状态变了但没人标脏时退化成全屏刷新；
 *   3. 光标单独走"擦-重绘-画"路径，并与脏矩形一起上屏。
 * ============================================================ */

#define GUI_DIRTY_MAX 12
#define GUI_SHADOW_PAD 20

static ui_rect g_dirty[GUI_DIRTY_MAX];
static int g_dirty_count = 0;
static int g_dirty_full = 0;
static int g_redraw_all_pending = 0;

static void dirty_reset(void) {
    g_dirty_count = 0;
    g_dirty_full = 0;
}

/* 应用通过 cfg.set 改了壁纸/头像后调用：下一帧丢弃局部脏矩形，
 * 整屏重绘，避免只刷新设置窗口而桌面背景保持旧图。 */
void gui_request_redraw_all(void) {
    g_redraw_all_pending = 1;
}

static void dirty_all(void) {
    g_dirty_full = 1;
    g_dirty_count = 0;
}

static int dirty_is_full(void) {
    return g_dirty_full;
}

static int g_rect_touch(const ui_rect *a, const ui_rect *b) {
    return !(a->x + a->w < b->x || b->x + b->w < a->x ||
             a->y + a->h < b->y || b->y + b->h < a->y);
}

static void dirty_add(int x, int y, int w, int h) {
    if (g_dirty_full || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gW) w = gW - x;
    if (y + h > gH) h = gH - y;
    if (w <= 0 || h <= 0) return;

    ui_rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    for (int i = 0; i < g_dirty_count; i++) {
        ui_rect *d = &g_dirty[i];
        if (!g_rect_touch(d, &r)) continue;

        int x0 = (d->x < r.x) ? d->x : r.x;
        int y0 = (d->y < r.y) ? d->y : r.y;
        int x1 = (d->x + d->w > r.x + r.w) ? d->x + d->w : r.x + r.w;
        int y1 = (d->y + d->h > r.y + r.h) ? d->y + d->h : r.y + r.h;

        int merged = (x1 - x0) * (y1 - y0);
        int apart = d->w * d->h + r.w * r.h;

        /* 对角合并会把面积放大好几倍，宁可多留一块独立矩形 */
        if (merged > apart + apart / 2 + 24576) continue;

        d->x = x0;
        d->y = y0;
        d->w = x1 - x0;
        d->h = y1 - y0;
        return;
    }

    if (g_dirty_count >= GUI_DIRTY_MAX) {
        dirty_all();
        return;
    }
    g_dirty[g_dirty_count++] = r;
}

static void dirty_add_rect(const ui_rect *r) {
    dirty_add(r->x, r->y, r->w, r->h);
}

static void dirty_add_padded(const ui_rect *r, int pad) {
    dirty_add(r->x - pad, r->y - pad, r->w + pad * 2, r->h + pad * 2);
}

/* 多个矩形互相重叠时反复合并，减少渲染与上屏次数 */
static void dirty_compact(void) {
    int i = 0;
    while (i < g_dirty_count) {
        int merged = 0;
        for (int j = i + 1; j < g_dirty_count; j++) {
            ui_rect *a = &g_dirty[i];
            ui_rect *b = &g_dirty[j];
            if (!g_rect_touch(a, b)) continue;

            int x0 = (a->x < b->x) ? a->x : b->x;
            int y0 = (a->y < b->y) ? a->y : b->y;
            int x1 = (a->x + a->w > b->x + b->w) ? a->x + a->w : b->x + b->w;
            int y1 = (a->y + a->h > b->y + b->h) ? a->y + a->h : b->y + b->h;

            int merged_area = (x1 - x0) * (y1 - y0);
            int apart = a->w * a->h + b->w * b->h;
            if (merged_area > apart + apart / 2 + 24576) continue;

            a->x = x0;
            a->y = y0;
            a->w = x1 - x0;
            a->h = y1 - y0;
            g_dirty[j] = g_dirty[g_dirty_count - 1];
            g_dirty_count--;
            merged = 1;
            break;
        }
        if (!merged) i++;
    }
}

/* 鼠标移动后：只重绘"旧指针命中的 hover 控件"与"新指针命中的"。
 * g_hunt_prev 是上一帧渲染时登记的控件矩形，正好对应屏幕上当前的
 * 高亮状态，所以用它判断需要擦除的旧高亮。 */
static void dirty_hover_scan(int old_x, int old_y, int x, int y) {
    for (int i = 0; i < g_hunt_prev_count; i++) {
        const ui_rect *r = &g_hunt_prev[i];
        if (g_point_in(r, old_x, old_y) || g_point_in(r, x, y)) {
            dirty_add_rect(r);
        }
    }
}

static void dirty_window(int id) {
    if (id < 0 || id >= GUI_WIN_MAX) return;
    if (!g_wins[id].open) return;
    dirty_add_padded(&g_wins[id].r, GUI_SHADOW_PAD);
}

/* 鼠标所在表面（点击后没有结构性变化时只重绘这一块） */
static void dirty_surface_at(int x, int y) {
    if (g_search_open) {
        ui_rect p = search_panel_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (g_user_menu) {
        ui_rect p = user_menu_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (g_power_menu) {
        ui_rect p = power_menu_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (g_net_menu) {
        ui_rect p = net_menu_rect();
        if (g_point_in(&p, x, y)) {
            dirty_add_padded(&p, 18);
            return;
        }
    }
    if (x < GUI_TASKBAR_W) {
        dirty_add(0, 0, GUI_TASKBAR_W, gH);
        return;
    }

    int id = win_topmost_at(x, y);
    if (id >= 0) {
        dirty_window(id);
        return;
    }

    /* 桌面空白：中央那块空桌面提示可能被点亮 */
    dirty_add(GUI_TASKBAR_W, gH / 2 - 100, gW - GUI_TASKBAR_W, 160);
}

/* 当前裁剪矩形与给定包围盒是否相交 */
static int gui_clip_hit(const ui_rect *r) {
    if (!vbe_clip_active()) return 1;

    int cx = 0;
    int cy = 0;
    int cw = 0;
    int ch = 0;
    vbe_get_clip(&cx, &cy, &cw, &ch);

    return !(r->x + r->w <= cx || cx + cw <= r->x ||
             r->y + r->h <= cy || cy + ch <= r->y);
}

static int dirty_rect_count(void) {
    return g_dirty_full ? 0 : g_dirty_count;
}

static void dirty_rect_get(int index, ui_rect *out) {
    if (!out || index < 0 || index >= g_dirty_count) return;
    *out = g_dirty[index];
}

/* 把"旧光标 + 全部脏矩形 + 新光标"依次上屏。
 * 旧光标位置已经被 gui_cursor_erase 恢复成背景，必须单独刷一次。 */
static void dirty_swap_all(int old_cx, int old_cy) {
    if (old_cx >= 0) {
        vbe_swap_rect((uint16_t)old_cx, (uint16_t)old_cy,
                      GUI_CURSOR_W, GUI_CURSOR_H);
    }

    int n = dirty_rect_count();
    for (int i = 0; i < n; i++) {
        ui_rect r;
        dirty_rect_get(i, &r);
        vbe_swap_rect((uint16_t)r.x, (uint16_t)r.y,
                      (uint16_t)r.w, (uint16_t)r.h);
    }

    vbe_swap_rect((uint16_t)g_mx, (uint16_t)g_my,
                  GUI_CURSOR_W, GUI_CURSOR_H);
}

/* 对每个脏矩形重绘一次场景树，绘制被裁剪在矩形内 */
static void gui_render_dirty_surfaces(void) {
    int order[GUI_WIN_MAX];
    int n = gui_window_zorder(order);

    for (int i = 0; i < g_dirty_count; i++) {
        ui_rect d = g_dirty[i];
        vbe_set_clip(d.x, d.y, d.w, d.h);

        gui_background();
        gui_desk_render();

        ui_rect badge = { gW - 116, 16, 100, 44 };
        if (gui_clip_hit(&badge)) gui_clock_badge();

        for (int k = 0; k < n; k++) {
            ui_rect box = g_wins[order[k]].r;
            box.x -= GUI_SHADOW_PAD;
            box.y -= GUI_SHADOW_PAD;
            box.w += GUI_SHADOW_PAD * 2;
            box.h += GUI_SHADOW_PAD * 2;
            if (!gui_clip_hit(&box)) continue;
            gui_window_frame(order[k], &g_theme);
        }

        ui_rect tb = { 0, 0, GUI_TASKBAR_W, gH };
        if (gui_clip_hit(&tb)) gui_taskbar_render();

        if (g_search_open) {
            ui_rect p = search_panel_rect();
            if (gui_clip_hit(&p)) gui_search_panel_render();
        }
        if (g_user_menu) {
            ui_rect p = user_menu_rect();
            if (gui_clip_hit(&p)) gui_user_menu_render();
        }
        if (g_power_menu) {
            ui_rect p = power_menu_rect();
            if (gui_clip_hit(&p)) gui_power_menu_render();
        }
        if (g_net_menu) {
            ui_rect p = net_menu_rect();
            if (gui_clip_hit(&p)) gui_net_menu_render();
        }

        /* 输入法候选窗永远画在最上层 */
        if (gui_ime_active()) {
            ui_rect ip = gui_ime_rect();
            ui_rect hit = { ip.x - 4, ip.y - 4, ip.w + 8, ip.h + 8 };
            if (gui_clip_hit(&hit)) gui_ime_render();
        }

        vbe_clear_clip();
    }
}


/* ============================================================
 * 文件管理器
 * ------------------------------------------------------------
 * 只做"浏览 + 打开 + 新建 + 删除"四件事，但每个入口都完整：
 *   - 目录优先排序，双击进入/打开
 *   - 新建文件与新建目录使用窗口内模态输入框
 *   - 删除目录时用 rmdir（非空会失败并给出提示）
 *   - 打开文件前检查 64KB 上限，避免把编辑器缓冲撑爆
 * ============================================================ */

static char g_fm_path[256] = "";
static struct GuiDirEntry g_fm_entries[GUI_FM_MAX];
static int g_fm_count = 0;
static int g_fm_sel = 0;
static int g_fm_top = 0;
static char g_fm_msg[96];
static uint32_t g_fm_msg_until = 0;
static int g_fm_modal = 0;          /* 1=new file, 2=new dir, 3=delete */
static char g_fm_input[64];
static int g_fm_input_len = 0;
static uint32_t g_fm_last_click_ms = 0;
static int g_fm_last_click_index = -1;

static void fm_set_msg(const char *text, uint32_t ms) {
    g_copy(g_fm_msg, text, (int)sizeof(g_fm_msg));
    g_fm_msg_until = net_now_ms() + ms;
}

static void fm_join(char *out, int cap, const char *base, const char *name) {
    int pos = 0;
    out[0] = '\0';
    if (base && base[0]) {
        while (base[pos] && pos < cap - 1) {
            out[pos] = base[pos];
            pos++;
        }
    }
    if (name && name[0]) {
        if (pos > 0 && pos < cap - 1) out[pos++] = '\\';
        for (int i = 0; name[i] && pos < cap - 1; i++) {
            out[pos++] = name[i];
        }
    }
    out[pos] = '\0';
}

static void fm_parent(char *out, int cap, const char *path) {
    int last = -1;
    int i = 0;
    while (path[i] && i < cap - 1) {
        out[i] = path[i];
        if (path[i] == '\\') last = i;
        i++;
    }
    out[i] = '\0';
    if (last >= 0) out[last] = '\0';
    else out[0] = '\0';
}

static int fm_is_dir(const struct GuiDirEntry *e) {
    return e->type == GUI_DIR_DIR;
}

static int fm_less(const struct GuiDirEntry *a, const struct GuiDirEntry *b) {
    int ad = fm_is_dir(a);
    int bd = fm_is_dir(b);
    if (ad != bd) return ad > bd;
    return g_strcasecmp((const char *)a->name, (const char *)b->name) < 0;
}

static void fm_sort(void) {
    for (int i = 1; i < g_fm_count; i++) {
        struct GuiDirEntry key = g_fm_entries[i];
        int j = i - 1;
        while (j >= 0 && fm_less(&key, &g_fm_entries[j])) {
            g_fm_entries[j + 1] = g_fm_entries[j];
            j--;
        }
        g_fm_entries[j + 1] = key;
    }
}

static int fm_open_path(const char *path) {
    struct GuiDirEntry tmp[GUI_FM_MAX];
    int n = slyzfs_list_dir(path ? path : "", tmp, GUI_FM_MAX);

    if (n < 0) {
        fm_set_msg("Cannot open this directory", 2600);
        return 0;
    }

    g_copy(g_fm_path, path ? path : "", (int)sizeof(g_fm_path));
    g_fm_count = n;
    for (int i = 0; i < n; i++) {
        g_fm_entries[i] = tmp[i];
    }
    fm_sort();
    g_fm_sel = 0;
    g_fm_top = 0;
    return 1;
}

static void fm_select_name(const char *name) {
    for (int i = 0; i < g_fm_count; i++) {
        if (g_strcmp((const char *)g_fm_entries[i].name, name) == 0) {
            g_fm_sel = i;
            break;
        }
    }
}

static void gui_files_open(const char *path) {
    const struct AuthUser *u = auth_current();

    if (!path || !*path) {
        path = u ? u->home : "";
    }

    g_fm_msg[0] = '\0';
    g_fm_modal = 0;
    g_fm_last_click_index = -1;

    if (!fm_open_path(path)) {
        g_fm_count = 0;
        g_copy(g_fm_path, path, (int)sizeof(g_fm_path));
    }
}

static void fm_activate_selected(void) {
    if (g_fm_sel < 0 || g_fm_sel >= g_fm_count) return;

    struct GuiDirEntry *e = &g_fm_entries[g_fm_sel];
    char full[256];
    fm_join(full, (int)sizeof(full), g_fm_path, (const char *)e->name);

    if (fm_is_dir(e)) {
        if (!fm_open_path(full)) {
            /* 打不开时保持在原目录 */
        }
        return;
    }

    if (e->size > GUI_ED_MAX) {
        fm_set_msg("File is larger than the 64 KB editor buffer", 3200);
        return;
    }

    /* MP3 交给媒体播放器；播放器不在任务栏，只有双击文件才出现 */
    if (gui_ext_is(full, ".mp3")) {
        if (!gui_open_media(full)) {
            fm_set_msg("Cannot start the media player", 3000);
        }
        return;
    }

    /* .slyz 是独立应用，不是文本：启动它自己的窗口 */
    if (gui_slyz_is_program(full)) {
        int win = gui_app_open_path(full);
        if (win >= 0) {
            win_open(win);
            g_focus = win;
        } else {
            fm_set_msg("Cannot launch this SLYZ program", 3000);
        }
        return;
    }

    gui_editor_open(full);
    win_open(WIN_EDITOR);
    g_focus = WIN_EDITOR;
}

static void fm_do_modal(void) {
    char full[256];

    if (g_fm_modal == 1 || g_fm_modal == 2) {
        if (g_fm_input_len == 0) {
            g_fm_modal = 0;
            return;
        }
        fm_join(full, (int)sizeof(full), g_fm_path, g_fm_input);

        int ok = (g_fm_modal == 1) ? slyzfs_create(full) : slyzfs_mkdir(full);
        if (!ok) {
            fm_set_msg(g_fm_modal == 1 ? "Create file failed" : "Create directory failed", 3000);
        } else {
            fm_open_path(g_fm_path);
            fm_select_name(g_fm_input);
            fm_set_msg(g_fm_modal == 1 ? "File created" : "Directory created", 2200);
        }
        g_fm_modal = 0;
        return;
    }

    if (g_fm_modal == 3) {
        if (g_fm_sel < 0 || g_fm_sel >= g_fm_count) {
            g_fm_modal = 0;
            return;
        }

        struct GuiDirEntry *e = &g_fm_entries[g_fm_sel];
        fm_join(full, (int)sizeof(full), g_fm_path, (const char *)e->name);

        int ok = fm_is_dir(e) ? slyzfs_rmdir(full) : slyzfs_remove(full);
        if (!ok) {
            fm_set_msg(fm_is_dir(e) ? "Delete failed - directory may not be empty"
                                    : "Delete failed", 3200);
        } else {
            fm_open_path(g_fm_path);
            fm_set_msg("Deleted", 1800);
        }
        g_fm_modal = 0;
    }
}

static int gui_files_modal_active(void) {
    return g_fm_modal != 0;
}

static int fm_toolbar_h(void) {
    return 40;
}

static void fm_toolbar_rects(ui_rect c, ui_rect out[5]) {
    int x = c.x;
    int y = c.y;
    int h = fm_toolbar_h();

    /* 宽度按最长标签计算：8 字符 × 16px = 128px，再留左右边距。
     * 之前的 96/100/110 宽会让 "Refresh"、"New File" 的文字
     * 压到相邻按钮上。 */
    out[0] = (ui_rect){ x, y, 64, h };      x += 72;
    out[1] = (ui_rect){ x, y, 128, h };     x += 136;
    out[2] = (ui_rect){ x, y, 148, h };     x += 156;
    out[3] = (ui_rect){ x, y, 132, h };     x += 140;
    out[4] = (ui_rect){ x, y, 116, h };
}

static int fm_list_top(ui_rect c) {
    return c.y + 94;
}

static int fm_list_height(ui_rect c) {
    return c.h - 94 - 34;
}

static int fm_visible_rows(ui_rect c) {
    int rows = fm_list_height(c) / 46;
    if (rows < 1) rows = 1;
    return rows;
}

static void gui_files_render(const ui_theme *t, ui_rect c) {
    (void)t;

    if (gui_app_render(WIN_FILES, c)) return;

    if (g_fm_msg[0] && net_now_ms() > g_fm_msg_until) {
        g_fm_msg[0] = '\0';
    }

    /* 工具栏 */
    ui_rect btns[5];
    fm_toolbar_rects(c, btns);
    const char *labels[5] = { "Up", "Refresh", "New File", "New Dir", "Delete" };

    for (int i = 0; i < 5; i++) {
        int hover = g_point_in(&btns[i], g_mx, g_my);
        gui_button(btns[i], LTR(labels[i]), hover, i == 2);
    }

    /* 路径栏 */
    ui_rect path_bar = { c.x, c.y + 52, c.w, 34 };
    ui_round_panel(path_bar, 8, C_BORDER, 0x0E1620);
    char shown[280];
    int p = 0;
    shown[p++] = '\\';
    for (int i = 0; g_fm_path[i] && p < (int)sizeof(shown) - 1; i++) {
        shown[p++] = (g_fm_path[i] == '\\') ? '\\' : g_fm_path[i];
    }
    shown[p] = '\0';
    gui_text_n(path_bar.x + 12, path_bar.y + 1, shown, (c.w - 30) / 16, C_DIM);

    /* 列表 */
    int list_y = fm_list_top(c);
    int list_h = fm_list_height(c);
    ui_round_fill((ui_rect){ c.x, list_y, c.w, list_h }, 10, 0x0C141E);

    int rows = fm_visible_rows(c);
    if (g_fm_sel < g_fm_top) g_fm_top = g_fm_sel;
    if (g_fm_sel >= g_fm_top + rows) g_fm_top = g_fm_sel - rows + 1;
    if (g_fm_top < 0) g_fm_top = 0;

    for (int r = 0; r < rows; r++) {
        int idx = g_fm_top + r;
        if (idx >= g_fm_count) break;

        struct GuiDirEntry *e = &g_fm_entries[idx];
        ui_rect row = { c.x + 6, list_y + 4 + r * 46, c.w - 24, 42 };

        hunt_add(row);

        if (idx == g_fm_sel) {
            ui_round_fill(row, 8, 0x1E3048);
            ui_round_fill((ui_rect){ row.x, row.y + 8, 3, row.h - 16 }, 1, C_ACCENT);
        } else if (g_point_in(&row, g_mx, g_my)) {
            ui_round_fill(row, 8, 0x16202C);
        }

        int iy = row.y + 7;
        if (fm_is_dir(e)) {
            icon_folder(row.x + 12, iy, 28, C_WARN);
        } else {
            icon_file(row.x + 12, iy, 28, 0xA9BDD2);
        }

        gui_text_n(row.x + 52, row.y + 5, (const char *)e->name,
                  (c.w - 260) / 16, C_TEXT);

        if (fm_is_dir(e)) {
            ui_text_transparent(row.x + c.w - 150, row.y + 5, "<DIR>", C_DIM);
        } else {
            char size_text[16];
            int n = g_utoa(e->size, size_text);
            size_text[n++] = ' ';
            size_text[n++] = 'B';
            size_text[n] = '\0';
            /* 右侧列只有 180px（11 个字符），超过就会压到文件名 */
            gui_text_n(row.x + c.w - 180, row.y + 5, size_text, 11, C_DIM);
        }
    }

    if (g_fm_count == 0) {
        ui_text_transparent(c.x + 24, list_y + 20, "This directory is empty.", C_DIM);
    }

    /* 滚动条 */
    if (g_fm_count > rows) {
        int sb_x = c.x + c.w - 10;
        ui_round_fill((ui_rect){ sb_x, list_y + 4, 8, list_h - 8 }, 4, 0x121A25);
        int track_h = list_h - 8;
        int thumb_h = track_h * rows / g_fm_count;
        if (thumb_h < 26) thumb_h = 26;
        int max_top = g_fm_count - rows;
        int thumb_y = list_y + 4;
        if (max_top > 0) {
            thumb_y += (track_h - thumb_h) * g_fm_top / max_top;
        }
        ui_round_fill((ui_rect){ sb_x, thumb_y, 8, thumb_h }, 4, C_BORDER);
    }

    /* 状态栏 */
    char status[96];
    int p2 = 0;
    p2 += g_utoa((uint32_t)g_fm_count, status + p2);
    g_copy(status + p2, " items    Enter: open   Backspace: up   Del: delete", 60);

    if (g_fm_msg[0]) {
        gui_text_n(c.x + 12, c.y + c.h - 34, g_fm_msg, (c.w - 20) / 16, C_WARN);
    } else {
        gui_text_n(c.x + 12, c.y + c.h - 34, status, (c.w - 20) / 16, C_DIM);
    }
}

static void gui_files_click(ui_rect c, int x, int y) {
    if (gui_app_active(WIN_FILES)) {
        gui_app_click(WIN_FILES, x - c.x, y - c.y, 1);
        return;
    }

    ui_rect btns[5];
    fm_toolbar_rects(c, btns);

    if (g_point_in(&btns[0], x, y)) {
        char parent[256];
        fm_parent(parent, (int)sizeof(parent), g_fm_path);
        fm_open_path(parent);
        return;
    }
    if (g_point_in(&btns[1], x, y)) {
        fm_open_path(g_fm_path);
        return;
    }
    if (g_point_in(&btns[2], x, y)) {
        g_fm_modal = 1;
        g_fm_input_len = 0;
        g_fm_input[0] = '\0';
        return;
    }
    if (g_point_in(&btns[3], x, y)) {
        g_fm_modal = 2;
        g_fm_input_len = 0;
        g_fm_input[0] = '\0';
        return;
    }
    if (g_point_in(&btns[4], x, y)) {
        if (g_fm_count > 0) g_fm_modal = 3;
        return;
    }

    int list_y = fm_list_top(c);
    int list_h = fm_list_height(c);
    int rows = fm_visible_rows(c);

    if (y >= list_y && y < list_y + list_h) {
        /* 滚动条点击：上半页 / 下半页 */
        if (x >= c.x + c.w - 14 && g_fm_count > rows) {
            if (y < list_y + list_h / 2) {
                g_fm_top -= rows;
                if (g_fm_top < 0) g_fm_top = 0;
                g_fm_sel = g_fm_top;
            } else {
                g_fm_top += rows;
                int max_top = g_fm_count - rows;
                if (max_top < 0) max_top = 0;
                if (g_fm_top > max_top) g_fm_top = max_top;
                g_fm_sel = g_fm_top;
            }
            return;
        }

        int idx = g_fm_top + (y - list_y - 4) / 46;
        if (idx >= 0 && idx < g_fm_count) {
            uint32_t now = net_now_ms();
            int dbl = (idx == g_fm_last_click_index &&
                       now - g_fm_last_click_ms < 450u);

            g_fm_sel = idx;
            g_fm_last_click_index = idx;
            g_fm_last_click_ms = now;

            if (dbl) {
                g_fm_last_click_index = -1;
                fm_activate_selected();
            }
        }
    }
}

static void gui_files_key(char key) {
    if (gui_app_active(WIN_FILES)) {
        gui_app_key(WIN_FILES, (int)(unsigned char)key);
        return;
    }

    if (g_fm_modal) return;

    int rows = fm_visible_rows(win_content_rect(&g_wins[WIN_FILES].r));

    if (key == (char)0xF6) {
        if (g_fm_sel > 0) g_fm_sel--;
    } else if (key == (char)0xF7) {
        if (g_fm_sel + 1 < g_fm_count) g_fm_sel++;
    } else if (key == (char)0xF8) {
        g_fm_sel -= rows;
        if (g_fm_sel < 0) g_fm_sel = 0;
    } else if (key == (char)0xF9) {
        g_fm_sel += rows;
        if (g_fm_sel >= g_fm_count) g_fm_sel = g_fm_count > 0 ? g_fm_count - 1 : 0;
    } else if (key == (char)0xF3) {
        g_fm_sel = 0;
    } else if (key == (char)0xF4) {
        g_fm_sel = g_fm_count > 0 ? g_fm_count - 1 : 0;
    } else if (key == '\r' || key == '\n') {
        fm_activate_selected();
    } else if (key == '\b' || key == 127) {
        char parent[256];
        fm_parent(parent, (int)sizeof(parent), g_fm_path);
        fm_open_path(parent);
    } else if (key == (char)0xF5) {
        if (g_fm_count > 0) g_fm_modal = 3;
    } else if (key == 14) {           /* Ctrl+N: new file */
        g_fm_modal = 1;
        g_fm_input_len = 0;
        g_fm_input[0] = '\0';
    } else if (key == 4) {            /* Ctrl+D: new directory */
        g_fm_modal = 2;
        g_fm_input_len = 0;
        g_fm_input[0] = '\0';
    } else if (key == 18) {           /* Ctrl+R: refresh */
        fm_open_path(g_fm_path);
    }
}

/* ---------- 文件管理器模态框 ---------- */

static ui_rect fm_modal_card(ui_rect window) {
    ui_rect r;
    r.w = 440;
    r.h = 210;
    r.x = window.x + (window.w - r.w) / 2;
    r.y = window.y + (window.h - r.h) / 2;
    return r;
}

static void fm_modal_buttons(ui_rect window, ui_rect *ok, ui_rect *cancel) {
    ui_rect card = fm_modal_card(window);
    ok->x = card.x + 40;
    ok->y = card.y + 146;
    ok->w = 150;
    ok->h = 44;
    cancel->x = card.x + card.w - 190;
    cancel->y = card.y + 146;
    cancel->w = 150;
    cancel->h = 44;
}

static void gui_files_modal_render(const ui_theme *t, ui_rect window) {
    (void)t;

    ui_rect content = win_content_rect(&window);
    vbe_blend_rect((uint16_t)content.x, (uint16_t)content.y,
                   (uint16_t)content.w, (uint16_t)content.h, 0x000000, 130);

    ui_rect card = fm_modal_card(window);
    ui_shadow(card, 14, 8);
    ui_round_panel(card, 14, C_BORDER, 0x141E2A);

    const char *title = g_fm_modal == 1 ? "New File" :
                        g_fm_modal == 2 ? "New Folder" : "Delete";
    ui_text_transparent(card.x + 24, card.y + 16, title, C_TEXT);
    ui_hline(card.x + 20, card.y + 58, card.w - 40, C_BORDER);

    if (g_fm_modal == 3) {
        gui_text_n(card.x + 24, card.y + 76,
                  "Delete the selected item?", 30, C_TEXT);
        if (g_fm_sel >= 0 && g_fm_sel < g_fm_count) {
            /* 卡片宽 440px，可用 24 个字符；文件名最长 52 字节，必须截断 */
            gui_text_n(card.x + 24, card.y + 112,
                       (const char *)g_fm_entries[g_fm_sel].name, 24, C_WARN);
        }
    } else {
        ui_text_transparent(card.x + 24, card.y + 68, "Name:", C_DIM);

        ui_rect box = { card.x + 24, card.y + 100, card.w - 48, 40 };
        ui_round_panel(box, 8, C_ACCENT, 0x0C141E);
        gui_text_n(box.x + 12, box.y + 4, g_fm_input, 22, C_TEXT);
        if (((net_now_ms() / 500u) & 1u) == 0) {
            vbe_fill_rect((uint16_t)(box.x + 12 + g_fm_input_len * 16),
                          (uint16_t)(box.y + 8), 2, 24, C_ACCENT);
        }
    }

    ui_rect ok;
    ui_rect cancel;
    fm_modal_buttons(window, &ok, &cancel);

    int ok_hover = g_point_in(&ok, g_mx, g_my);
    int cancel_hover = g_point_in(&cancel, g_mx, g_my);

    gui_button(ok, LTR(g_fm_modal == 3 ? "Delete" : "Create"), ok_hover, 1);
    gui_button(cancel, "Cancel", cancel_hover, 0);
}

static int gui_files_modal_key(char key) {
    if (key == 27) {
        g_fm_modal = 0;
        return 1;
    }

    if (g_fm_modal == 3) {
        if (key == '\r' || key == '\n' || key == 'y' || key == 'Y') {
            fm_do_modal();
        }
        return 1;
    }

    if (key == '\r' || key == '\n') {
        fm_do_modal();
        return 1;
    }
    if (key == '\b' || key == 127) {
        if (g_fm_input_len > 0) {
            g_fm_input_len--;
            g_fm_input[g_fm_input_len] = '\0';
        }
        return 1;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127 &&
        g_fm_input_len < 21) {
        g_fm_input[g_fm_input_len++] = key;
        g_fm_input[g_fm_input_len] = '\0';
    }
    return 1;
}

static void gui_files_modal_click(ui_rect window, int x, int y) {
    ui_rect ok;
    ui_rect cancel;
    fm_modal_buttons(window, &ok, &cancel);

    if (g_point_in(&ok, x, y)) {
        fm_do_modal();
    } else if (g_point_in(&cancel, x, y)) {
        g_fm_modal = 0;
    }
}

/* ============================================================
 * 文本编辑器
 * ============================================================ */

static uint8_t  g_ed_buf[GUI_ED_MAX];
static uint32_t g_ed_len = 0;
static uint32_t g_ed_cursor = 0;
static uint32_t g_ed_top = 0;
static uint32_t g_ed_hscroll = 0;

/* 选区：a 是锚点，b 是活动端；[min,max) 为选中区间。
 * 复制/剪切/粘贴共用一个 4KB 剪贴板，足够覆盖正常文本片段。 */
static uint32_t g_ed_sel_a = 0;
static uint32_t g_ed_sel_b = 0;
static int      g_ed_drag_sel = 0;
static char     g_clip[4096];
static int      g_clip_len = 0;
static uint32_t g_ed_desired_col = 0;
static int      g_ed_open = 0;
static char     g_ed_path[256];
static int      g_ed_dirty = 0;
static char     g_ed_msg[96];
static uint32_t g_ed_msg_until = 0;
static int      g_ed_ask_save = 0;
static int      g_ed_close_request = 0;

static uint32_t ed_line_of(uint32_t pos) {
    if (pos > g_ed_len) pos = g_ed_len;
    uint32_t line = 0;
    for (uint32_t i = 0; i < pos; i++) {
        if (g_ed_buf[i] == '\n') line++;
    }
    return line;
}

static uint32_t ed_offset_of_line(uint32_t line);
static uint32_t ed_line_len_at(uint32_t start);

/* 光标在行内的"显示列"：汉字占 2 列 */
static uint32_t ed_col_of(uint32_t pos) {
    if (pos > g_ed_len) pos = g_ed_len;
    uint32_t start = pos;
    while (start > 0 && g_ed_buf[start - 1] != '\n') start--;
    return (uint32_t)utf8_width_n((const char *)g_ed_buf + start,
                                  (int)(pos - start));
}

/* 行内第 want_col 个显示列的字节偏移；落在宽字符中间取字符起点 */
static uint32_t ed_byte_at_col(uint32_t line, uint32_t want_col) {
    uint32_t start = ed_offset_of_line(line);
    uint32_t end = ed_line_len_at(start) + start;
    uint32_t col = 0;
    uint32_t i = start;
    while (i < end) {
        uint32_t cp = 0;
        int used = utf8_decode(g_ed_buf + i, (int)(end - i), &cp);
        if (used <= 0) { i++; continue; }
        int w = utf32_width(cp);
        if (w == 0) { i += (uint32_t)used; continue; }
        if (col + (uint32_t)w > want_col) break;
        col += (uint32_t)w;
        i += (uint32_t)used;
    }
    return i;
}

/* 前/后一个字符边界（缓冲区不保证 NUL 结尾，必须带长度） */
static uint32_t ed_prev_boundary(uint32_t pos) {
    if (pos == 0) return 0;
    uint32_t i = pos - 1;
    while (i > 0 && (g_ed_buf[i] & 0xC0u) == 0x80u) i--;
    return i;
}

static uint32_t ed_next_boundary(uint32_t pos) {
    if (pos >= g_ed_len) return g_ed_len;
    uint32_t cp = 0;
    int used = utf8_decode(g_ed_buf + pos, (int)(g_ed_len - pos), &cp);
    if (used <= 0) used = 1;
    return pos + (uint32_t)used;
}

static uint32_t ed_offset_of_line(uint32_t line) {
    uint32_t pos = 0;
    uint32_t current = 0;
    while (current < line && pos < g_ed_len) {
        if (g_ed_buf[pos] == '\n') current++;
        pos++;
    }
    return pos;
}

static uint32_t ed_line_len_at(uint32_t start) {
    uint32_t pos = start;
    while (pos < g_ed_len && g_ed_buf[pos] != '\n') pos++;
    return pos - start;
}

static uint32_t ed_line_count(void) {
    uint32_t lines = 1;
    for (uint32_t i = 0; i < g_ed_len; i++) {
        if (g_ed_buf[i] == '\n') lines++;
    }
    return lines;
}

/* ---- 选区与剪贴板 ---- */

static uint32_t ed_sel_lo(void) {
    return (g_ed_sel_a < g_ed_sel_b) ? g_ed_sel_a : g_ed_sel_b;
}

static uint32_t ed_sel_hi(void) {
    return (g_ed_sel_a < g_ed_sel_b) ? g_ed_sel_b : g_ed_sel_a;
}

static int ed_has_selection(void) {
    return g_ed_sel_a != g_ed_sel_b;
}

static void ed_clear_selection(void) {
    g_ed_sel_a = g_ed_cursor;
    g_ed_sel_b = g_ed_cursor;
    g_ed_drag_sel = 0;
}

static void ed_set_msg(const char *text, uint32_t ms) {
    g_copy(g_ed_msg, text, (int)sizeof(g_ed_msg));
    g_ed_msg_until = net_now_ms() + ms;
}

static int ed_insert(uint32_t at, const uint8_t *data, uint32_t n) {
    if (at > g_ed_len) at = g_ed_len;
    if (g_ed_len + n > GUI_ED_MAX) {
        ed_set_msg("Editor buffer is full", 2600);
        return 0;
    }

    for (uint32_t i = g_ed_len; i > at; i--) {
        g_ed_buf[i + n - 1] = g_ed_buf[i - 1];
    }
    for (uint32_t i = 0; i < n; i++) {
        g_ed_buf[at + i] = data[i];
    }
    g_ed_len += n;
    g_ed_dirty = 1;
    return 1;
}

static void ed_delete(uint32_t at, uint32_t n) {
    if (at >= g_ed_len || n == 0) return;
    if (at + n > g_ed_len) n = g_ed_len - at;

    for (uint32_t i = at; i + n < g_ed_len; i++) {
        g_ed_buf[i] = g_ed_buf[i + n];
    }
    g_ed_len -= n;
    if (g_ed_cursor > g_ed_len) g_ed_cursor = g_ed_len;
    g_ed_dirty = 1;
}

static void ed_insert_char(char c) {
    uint8_t ch = (uint8_t)c;
    if (ed_insert(g_ed_cursor, &ch, 1)) {
        g_ed_cursor++;
        g_ed_desired_col = ed_col_of(g_ed_cursor);
    }
}

/* 插入一段 UTF-8 文本（输入法上屏） */
static void ed_insert_utf8(const char *text) {
    int n = 0;
    while (text[n]) n++;
    if (n <= 0) return;
    if (ed_insert(g_ed_cursor, (const uint8_t *)text, (uint32_t)n)) {
        g_ed_cursor += (uint32_t)n;
        g_ed_desired_col = ed_col_of(g_ed_cursor);
    }
}

static void ed_backspace(void) {
    if (g_ed_cursor == 0) return;
    uint32_t prev = ed_prev_boundary(g_ed_cursor);
    ed_delete(prev, g_ed_cursor - prev);
    g_ed_cursor = prev;
    g_ed_desired_col = ed_col_of(g_ed_cursor);
}

static void ed_delete_forward(void) {
    if (g_ed_cursor >= g_ed_len) return;
    uint32_t next = ed_next_boundary(g_ed_cursor);
    ed_delete(g_ed_cursor, next - g_ed_cursor);
    g_ed_desired_col = ed_col_of(g_ed_cursor);
}

static void ed_move_vertical(int direction) {
    uint32_t line = ed_line_of(g_ed_cursor);
    uint32_t lines = ed_line_count();

    if (direction < 0) {
        if (line == 0) return;
        line--;
    } else {
        if (line + 1 >= lines) return;
        line++;
    }

    g_ed_cursor = ed_byte_at_col(line, g_ed_desired_col);
}

static int ed_save(void) {
    if (g_ed_path[0] == '\0') {
        ed_set_msg("No file name", 2600);
        return 0;
    }

    void *fh = slyzfs_open(g_ed_path, "w");
    if (!fh) {
        ed_set_msg("Cannot open file for writing", 3200);
        return 0;
    }

    uint32_t total = 0;
    while (total < g_ed_len) {
        int n = slyzfs_write(fh, g_ed_buf + total, (int)(g_ed_len - total));
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    slyzfs_close(fh);

    if (total != g_ed_len) {
        ed_set_msg("Write failed - file may be truncated", 3200);
        return 0;
    }

    g_ed_dirty = 0;
    ed_set_msg("Saved", 1800);
    return 1;
}

static void gui_editor_open(const char *path) {
    g_ed_len = 0;
    g_ed_cursor = 0;
    g_ed_top = 0;
    g_ed_hscroll = 0;
    g_ed_desired_col = 0;
    g_ed_dirty = 0;
    g_ed_ask_save = 0;
    g_ed_close_request = 0;
    g_ed_msg[0] = '\0';
    g_ed_open = 1;
    g_ed_sel_a = 0;
    g_ed_sel_b = 0;
    g_ed_drag_sel = 0;

    if (path && *path) {
        g_copy(g_ed_path, path, (int)sizeof(g_ed_path));

        void *fh = slyzfs_open(path, "r");
        if (fh) {
            while (g_ed_len < GUI_ED_MAX) {
                int n = slyzfs_read(fh, g_ed_buf + g_ed_len,
                                    (int)(GUI_ED_MAX - g_ed_len));
                if (n <= 0) break;
                g_ed_len += (uint32_t)n;
            }
            slyzfs_close(fh);
        } else {
            ed_set_msg("New file - Ctrl+S to save", 3200);
        }
    } else {
        const struct AuthUser *u = auth_current();
        const char *home = (u && u->home[0]) ? u->home : "home\\root";
        fm_join(g_ed_path, (int)sizeof(g_ed_path), home, "untitled.txt");
        ed_set_msg("New file - Ctrl+S to save", 3200);
    }
}

static int gui_editor_is_open(void) {
    return g_ed_open && g_wins[WIN_EDITOR].open;
}

static void gui_editor_request_close(void) {
    if (g_ed_dirty) {
        g_ed_ask_save = 1;
    } else {
        g_ed_close_request = 1;
    }
}

static int gui_editor_should_close(void) {
    if (g_ed_close_request) {
        g_ed_close_request = 0;
        g_ed_open = 0;
        return 1;
    }
    return 0;
}

static int gui_editor_modal_active(void) {
    return g_ed_ask_save != 0;
}

static void gui_editor_render(const ui_theme *t, ui_rect c) {
    (void)t;

    if (g_ed_msg[0] && net_now_ms() > g_ed_msg_until) {
        g_ed_msg[0] = '\0';
    }

    int status_h = 38;
    int text_y = c.y + 8;
    int text_h = c.h - status_h - 12;
    if (text_h < 32) text_h = 32;

    ui_round_fill((ui_rect){ c.x, c.y, c.w, c.h - status_h }, 10, 0x0C141E);

    int visible = text_h / 32;
    if (visible < 1) visible = 1;

    int gutter = 84;
    int text_x = c.x + gutter;
    int max_cols = (c.w - gutter - 24) / 16;
    if (max_cols < 1) max_cols = 1;

    uint32_t cursor_line = ed_line_of(g_ed_cursor);
    uint32_t cursor_col = ed_col_of(g_ed_cursor);

    if (cursor_line < g_ed_top) g_ed_top = cursor_line;
    if (cursor_line >= g_ed_top + (uint32_t)visible) {
        g_ed_top = cursor_line - (uint32_t)visible + 1;
    }

    if (cursor_col < g_ed_hscroll) g_ed_hscroll = cursor_col;
    if (cursor_col >= g_ed_hscroll + (uint32_t)max_cols) {
        g_ed_hscroll = cursor_col - (uint32_t)max_cols + 6;
    }

    /* 行号列 */
    ui_rect gutter_rect = { c.x, c.y, gutter, text_h };
    ui_round_fill(gutter_rect, 10, 0x0A111A);

    uint32_t off = ed_offset_of_line(g_ed_top);
    for (int r = 0; r < visible; r++) {
        uint32_t line_no = g_ed_top + (uint32_t)r;
        if (off > g_ed_len && g_ed_len > 0) break;

        int y = text_y + r * 32;

        char num[12];
        g_itoa_pad(line_no + 1, num, 4);
        ui_text_transparent(c.x + 12, y, num, 0x4E6A85);

        uint32_t end = off;
        while (end < g_ed_len && g_ed_buf[end] != '\n') end++;

        uint32_t line_len = end - off;
        uint32_t sel_lo = ed_sel_lo();
        uint32_t sel_hi = ed_sel_hi();

        uint32_t i = off;
        uint32_t col = 0;
        while (i < end) {
            uint32_t cp = 0;
            int used = utf8_decode(g_ed_buf + i, (int)(end - i), &cp);
            if (used <= 0) { i++; continue; }
            if (cp == '\t') cp = ' ';

            int w = utf32_width(cp);
            if (w == 0) { i += (uint32_t)used; continue; }

            if (col < g_ed_hscroll) {
                col += (uint32_t)w;
                i += (uint32_t)used;
                continue;
            }
            int vcol = (int)(col - g_ed_hscroll);
            if (vcol + w > max_cols) break;
            int cx = text_x + vcol * 16;

            /* 选区高亮：按字符实际宽度铺底，汉字占两格 */
            if (sel_lo != sel_hi && i >= sel_lo && i < sel_hi) {
                vbe_fill_rect((uint16_t)cx, (uint16_t)y, (uint16_t)(16 * w), 32,
                              0x1E3A5F);
            }

            if (w == 2) {
                vbe_draw_cjk_transparent((uint16_t)cx, (uint16_t)y,
                                         cp, C_TEXT);
            } else {
                char ch = (char)cp;
                if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7F) ch = '.';
                ui_text_transparent(cx, y, (char[]){ ch, '\0' }, C_TEXT);
            }

            col += (uint32_t)w;
            i += (uint32_t)used;
        }

        off = (end < g_ed_len) ? end + 1 : end;
        if (end >= g_ed_len && line_no + 1 >= ed_line_count()) break;
    }

    /* 光标 */
    int anchor_cx = text_x + (int)(cursor_col - g_ed_hscroll) * 16;
    int anchor_cy = text_y + (int)(cursor_line - g_ed_top) * 32;
    if (cursor_line >= g_ed_top && cursor_line < g_ed_top + (uint32_t)visible) {
        /* 候选窗锚点：插入点正下方 */
        g_ime_ax = anchor_cx;
        g_ime_ay = anchor_cy + 40;
    }
    if (((net_now_ms() / 500u) & 1u) == 0) {
        if (cursor_line >= g_ed_top &&
            cursor_line < g_ed_top + (uint32_t)visible &&
            cursor_col >= g_ed_hscroll &&
            cursor_col < g_ed_hscroll + (uint32_t)max_cols) {
            int cx = text_x + (int)(cursor_col - g_ed_hscroll) * 16;
            int cy = text_y + (int)(cursor_line - g_ed_top) * 32;
            vbe_fill_rect((uint16_t)cx, (uint16_t)(cy + 2), 2, 28, C_ACCENT);
        }
    }

    /* 状态栏 */
    ui_hline(c.x, c.y + c.h - status_h, c.w, C_BORDER);

    /* 状态栏左侧 540px 给路径或临时消息，右侧 400px 给行列信息。
     * 路径最长 256 字节、消息最长 96 字节，都必须按像素宽度截断。 */
    int left_chars = (c.w - 420) / 16;
    if (left_chars < 8) left_chars = 8;

    if (g_ed_msg[0]) {
        gui_text_n(c.x + 12, c.y + c.h - 34, g_ed_msg, left_chars, C_OK);
    } else {
        char left[280];
        int p = 0;
        for (int i = 0; g_ed_path[i] && p < 180; i++) {
            left[p++] = g_ed_path[i];
        }
        left[p] = '\0';
        gui_text_n(c.x + 12, c.y + c.h - 34, left, left_chars, C_DIM);
    }

    /* 右侧信息必须控制在 24 个字符以内（384px < 400px） */
    char info[64];
    int q = 0;
    g_copy(info, "Ln ", 4); q += 3;
    q += g_utoa(cursor_line + 1, info + q);
    g_copy(info + q, "  Col ", 6); q += 5;
    q += g_utoa(cursor_col + 1, info + q);
    g_copy(info + q, "  ", 3); q += 2;
    q += g_utoa(g_ed_len, info + q);
    g_copy(info + q, " B", 3); q += 2;
    if (g_ed_dirty) {
        info[q++] = ' ';
        info[q++] = '*';
        info[q] = '\0';
    }

    gui_text_n(c.x + c.w - 400, c.y + c.h - 34, info, 24, C_TEXT);
}

static void ed_delete_selection(void) {
    if (!ed_has_selection()) return;

    uint32_t a = ed_sel_lo();
    uint32_t b = ed_sel_hi();

    ed_delete(a, b - a);
    g_ed_cursor = a;
    g_ed_sel_a = a;
    g_ed_sel_b = a;
    g_ed_desired_col = ed_col_of(a);
}

static void ed_copy_selection(int cut) {
    if (!ed_has_selection()) {
        g_clip_len = 0;
        ed_set_msg("Nothing selected", 1400);
        return;
    }

    uint32_t a = ed_sel_lo();
    uint32_t b = ed_sel_hi();
    uint32_t n = b - a;
    if (n > sizeof(g_clip) - 1) n = sizeof(g_clip) - 1;

    for (uint32_t i = 0; i < n; i++) {
        g_clip[i] = (char)g_ed_buf[a + i];
    }
    g_clip_len = (int)n;

    if (cut) ed_delete_selection();

    ed_set_msg(cut ? "Cut" : "Copied", 1400);
}

static void ed_paste(void) {
    if (g_clip_len <= 0) {
        ed_set_msg("Clipboard is empty", 1400);
        return;
    }

    ed_delete_selection();

    if (!ed_insert(g_ed_cursor, (const uint8_t *)g_clip, (uint32_t)g_clip_len)) {
        return;
    }

    g_ed_cursor += (uint32_t)g_clip_len;
    g_ed_sel_a = g_ed_cursor;
    g_ed_sel_b = g_ed_cursor;
    g_ed_desired_col = ed_col_of(g_ed_cursor);
    g_ed_dirty = 1;
}

/* 屏幕坐标 → 缓冲区偏移（列按等宽字符换算，越界自动夹到行尾） */
static void ed_pos_from_point(ui_rect c, int x, int y, uint32_t *out) {
    int status_h = 38;
    int text_y = c.y + 8;
    int text_h = c.h - status_h - 12;
    if (text_h < 32) text_h = 32;

    int gutter = 84;
    int text_x = c.x + gutter;

    if (y < text_y) y = text_y;

    uint32_t line = g_ed_top;
    if (y < text_y + text_h) {
        line += (uint32_t)((y - text_y) / 32);
    } else {
        line += (uint32_t)(text_h / 32);
    }

    uint32_t lines = ed_line_count();
    if (line >= lines) line = lines > 0 ? lines - 1 : 0;

    uint32_t col = g_ed_hscroll;
    if (x > text_x) col += (uint32_t)((x - text_x) / 16);

    uint32_t start = ed_offset_of_line(line);
    uint32_t len = ed_line_len_at(start);
    if (col > len) col = len;

    *out = start + col;
}

static void gui_editor_click(ui_rect c, int x, int y) {
    int status_h = 38;
    int text_y = c.y + 8;
    int text_h = c.h - status_h - 12;
    if (text_h < 32) text_h = 32;

    if (y < text_y || y >= text_y + text_h) return;

    uint32_t pos = 0;
    ed_pos_from_point(c, x, y, &pos);

    g_ed_cursor = pos;
    g_ed_desired_col = ed_col_of(pos);
    g_ed_sel_a = pos;
    g_ed_sel_b = pos;
    g_ed_drag_sel = 1;
}

/* 拖动选择：主循环在鼠标移动时调用，实时扩展选区 */
static void gui_editor_drag_to(ui_rect c, int x, int y) {
    if (!g_ed_drag_sel) return;

    uint32_t pos = 0;
    ed_pos_from_point(c, x, y, &pos);

    if (pos != g_ed_sel_b) {
        g_ed_sel_b = pos;
        g_ed_cursor = pos;
        g_ed_desired_col = ed_col_of(pos);
    }
}

static void gui_editor_key(char key) {
    if (g_ed_ask_save) return;

    switch (key) {
        case 1:    /* Ctrl+A 全选 */
            g_ed_sel_a = 0;
            g_ed_sel_b = g_ed_len;
            g_ed_cursor = g_ed_len;
            g_ed_desired_col = ed_col_of(g_ed_cursor);
            break;
        case 3:    /* Ctrl+C 复制 */
            ed_copy_selection(0);
            break;
        case 22:   /* Ctrl+V 粘贴 */
            ed_paste();
            break;
        case 24:   /* Ctrl+X 剪切 */
            ed_copy_selection(1);
            break;
        case 19:   /* Ctrl+S */
            ed_save();
            break;
        case 17:   /* Ctrl+Q */
        case 27:   /* Esc */
            gui_editor_request_close();
            break;
        case '\r':
        case '\n':
            ed_delete_selection();
            ed_insert_char('\n');
            break;
        case '\b':
        case 127:
            if (ed_has_selection()) {
                ed_delete_selection();
            } else {
                ed_backspace();
            }
            break;
        case '\t': {
            const uint8_t spaces[4] = { ' ', ' ', ' ', ' ' };
            ed_delete_selection();
            if (ed_insert(g_ed_cursor, spaces, 4)) {
                g_ed_cursor += 4;
                g_ed_desired_col = ed_col_of(g_ed_cursor);
            }
            break;
        }
        case (char)0xF1:   /* left */
            ed_clear_selection();
            if (g_ed_cursor > 0) g_ed_cursor = ed_prev_boundary(g_ed_cursor);
            g_ed_desired_col = ed_col_of(g_ed_cursor);
            break;
        case (char)0xF2:   /* right */
            ed_clear_selection();
            if (g_ed_cursor < g_ed_len) g_ed_cursor = ed_next_boundary(g_ed_cursor);
            g_ed_desired_col = ed_col_of(g_ed_cursor);
            break;
        case (char)0xF3: { /* home */
            ed_clear_selection();
            uint32_t start = ed_offset_of_line(ed_line_of(g_ed_cursor));
            g_ed_cursor = start;
            g_ed_desired_col = 0;
            break;
        }
        case (char)0xF4: { /* end */
            ed_clear_selection();
            uint32_t line = ed_line_of(g_ed_cursor);
            uint32_t start = ed_offset_of_line(line);
            g_ed_cursor = start + ed_line_len_at(start);
            g_ed_desired_col = ed_col_of(g_ed_cursor);
            break;
        }
        case (char)0xF5:   /* delete */
            if (ed_has_selection()) {
                ed_delete_selection();
            } else {
                ed_delete_forward();
            }
            break;
        case (char)0xF6:   /* up */
            ed_clear_selection();
            ed_move_vertical(-1);
            break;
        case (char)0xF7:   /* down */
            ed_clear_selection();
            ed_move_vertical(1);
            break;
        case (char)0xF8:   /* page up */
            ed_clear_selection();
            for (int i = 0; i < 10; i++) ed_move_vertical(-1);
            break;
        case (char)0xF9:   /* page down */
            ed_clear_selection();
            for (int i = 0; i < 10; i++) ed_move_vertical(1);
            break;
        default:
            if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
                ed_delete_selection();
                ed_insert_char(key);
            }
            break;
    }
}

/* ---------- 未保存确认框 ---------- */

static ui_rect ed_modal_card(ui_rect window) {
    ui_rect r;
    r.w = 460;
    r.h = 200;
    r.x = window.x + (window.w - r.w) / 2;
    r.y = window.y + (window.h - r.h) / 2;
    return r;
}

static void ed_modal_buttons(ui_rect window, ui_rect *save, ui_rect *discard, ui_rect *cancel) {
    ui_rect card = ed_modal_card(window);
    save->x = card.x + 26;
    save->y = card.y + 136;
    save->w = 120;
    save->h = 44;

    discard->x = card.x + 160;
    discard->y = card.y + 136;
    discard->w = 130;
    discard->h = 44;

    cancel->x = card.x + 304;
    cancel->y = card.y + 136;
    cancel->w = 126;
    cancel->h = 44;
}

static void gui_editor_modal_render(const ui_theme *t, ui_rect window) {
    (void)t;

    ui_rect content = win_content_rect(&window);
    vbe_blend_rect((uint16_t)content.x, (uint16_t)content.y,
                   (uint16_t)content.w, (uint16_t)content.h, 0x000000, 130);

    ui_rect card = ed_modal_card(window);
    ui_shadow(card, 14, 8);
    ui_round_panel(card, 14, C_BORDER, 0x141E2A);

    ui_text_transparent(card.x + 26, card.y + 18, "Unsaved changes", C_TEXT);
    ui_hline(card.x + 22, card.y + 62, card.w - 44, C_BORDER);
    gui_text_n(card.x + 26, card.y + 82,
              "Save this file before closing?", 34, C_DIM);

    ui_rect save;
    ui_rect discard;
    ui_rect cancel;
    ed_modal_buttons(window, &save, &discard, &cancel);

    gui_button(save, LTR("Save"), g_point_in(&save, g_mx, g_my), 1);
    gui_button(discard, "Discard", g_point_in(&discard, g_mx, g_my), 0);
    gui_button(cancel, "Cancel", g_point_in(&cancel, g_mx, g_my), 0);
}

static int gui_editor_modal_key(char key) {
    if (key == 27) {
        g_ed_ask_save = 0;
        return 1;
    }
    if (key == 'y' || key == 'Y') {
        if (ed_save()) {
            g_ed_ask_save = 0;
            g_ed_close_request = 1;
        }
        return 1;
    }
    if (key == 'n' || key == 'N') {
        g_ed_ask_save = 0;
        g_ed_close_request = 1;
        return 1;
    }
    return 1;
}

static void gui_editor_modal_click(ui_rect window, int x, int y) {
    ui_rect save;
    ui_rect discard;
    ui_rect cancel;
    ed_modal_buttons(window, &save, &discard, &cancel);

    if (g_point_in(&save, x, y)) {
        if (ed_save()) {
            g_ed_ask_save = 0;
            g_ed_close_request = 1;
        }
    } else if (g_point_in(&discard, x, y)) {
        g_ed_dirty = 0;
        g_ed_ask_save = 0;
        g_ed_close_request = 1;
    } else if (g_point_in(&cancel, x, y)) {
        g_ed_ask_save = 0;
    }
}

/* ============================================================
 * 设置
 * ============================================================ */

static char g_set_msg[96];
static uint32_t g_set_msg_until = 0;

static void set_num2(char *out, int v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = '\0';
}

/* 按本地时间加减若干分钟后写回 CMOS。CMOS 存的是 UTC，因此先把
 * 本地时间换算回 UTC 再落盘，避免用户看到的时间与调整结果不一致。 */
static int set_clock_shift(int delta_minutes) {
    int hour = 0;
    int minute = 0;
    if (!rtc_get_utc(&hour, &minute, 0)) return 0;

    int tz = system_cfg_timezone();
    int local = hour * 60 + minute + tz * 60 + delta_minutes;
    local = ((local % (24 * 60)) + (24 * 60)) % (24 * 60);

    int utc = local - tz * 60;
    utc = ((utc % (24 * 60)) + (24 * 60)) % (24 * 60);

    return rtc_set_utc(utc / 60, utc % 60);
}

static void gui_settings_render(const ui_theme *t, ui_rect c) {
    (void)t;

    if (gui_app_render(WIN_SETTINGS, c)) return;

    if (g_set_msg[0] && net_now_ms() > g_set_msg_until) {
        g_set_msg[0] = '\0';
    }

    int gui = system_cfg_boot_gui();

    ui_text_transparent(c.x + 24, c.y + 8, LTR("BOOT MODE"), C_DIM);
    ui_text_big(c.x + 24, c.y + 44, gui ? "GRAPHICAL" : "TEXT", C_TEXT);

    /* 语言切换放在标题行右侧的空白处，不额外占用垂直空间 */
    ui_text_transparent(c.x + c.w - 292, c.y + 8, LTR("LANGUAGE"), C_DIM);
    ui_rect lang_en = { c.x + c.w - 292, c.y + 38, 130, 40 };
    ui_rect lang_zh = { c.x + c.w - 150, c.y + 38, 130, 40 };
    gui_button(lang_en, "English", g_point_in(&lang_en, g_mx, g_my),
               lang_current() == LANG_EN);
    gui_button(lang_zh, "中文", g_point_in(&lang_zh, g_mx, g_my),
               lang_current() == LANG_ZH);

    ui_rect b1 = { c.x + 24,  c.y + 118, 280, 52 };
    ui_rect b2 = { c.x + 320, c.y + 118, 280, 52 };

    gui_button(b1, LTR("GRAPHICAL DESKTOP"), g_point_in(&b1, g_mx, g_my), gui);
    gui_button(b2, LTR("TEXT TERMINAL"), g_point_in(&b2, g_mx, g_my), !gui);

    ui_hline(c.x + 24, c.y + 190, c.w - 48, C_BORDER);

    ui_text_transparent(c.x + 24, c.y + 196, LTR("DATE & TIME"), C_DIM);

    int hour = 0;
    int minute = 0;
    gui_clock_local(&hour, &minute);

    /* 左列：时区偏移。CMOS 按 UTC 保存，这里决定本地时间的换算量。 */
    ui_text_transparent(c.x + 24, c.y + 236, LTR("TIMEZONE"), C_DIM);

    char tz_text[12];
    gui_tz_text(tz_text, (int)sizeof(tz_text));

    ui_rect tz_minus = { c.x + 24,  c.y + 272, 48, 40 };
    ui_rect tz_plus  = { c.x + 232, c.y + 272, 48, 40 };

    gui_button(tz_minus, "-", g_point_in(&tz_minus, g_mx, g_my), 0);
    gui_button(tz_plus, "+", g_point_in(&tz_plus, g_mx, g_my), 0);
    ui_text_transparent(c.x + 80, c.y + 276, tz_text, C_TEXT);

    int utc_hour = 0;
    int utc_minute = 0;
    rtc_get_utc(&utc_hour, &utc_minute, 0);

    char utc_line[24];
    int p = 0;
    utc_line[p++] = 'U';
    utc_line[p++] = 'T';
    utc_line[p++] = 'C';
    utc_line[p++] = ' ';
    utc_line[p++] = (char)('0' + (utc_hour / 10) % 10);
    utc_line[p++] = (char)('0' + utc_hour % 10);
    utc_line[p++] = ':';
    utc_line[p++] = (char)('0' + (utc_minute / 10) % 10);
    utc_line[p++] = (char)('0' + utc_minute % 10);
    utc_line[p] = '\0';

    ui_text_transparent(c.x + 24, c.y + 328, utc_line, C_DIM);

    /* 右列：本地时钟与手动调整。按钮作用于本地时间，回写时折算成 UTC。 */
    ui_text_transparent(c.x + 330, c.y + 236, LTR("CLOCK"), C_DIM);

    char clock_text[8];
    clock_text[0] = (char)('0' + (hour / 10) % 10);
    clock_text[1] = (char)('0' + hour % 10);
    clock_text[2] = ':';
    clock_text[3] = (char)('0' + (minute / 10) % 10);
    clock_text[4] = (char)('0' + minute % 10);
    clock_text[5] = '\0';

    ui_text_transparent(c.x + 330, c.y + 272, clock_text, C_TEXT);

    char hour_text[4];
    char minute_text[4];
    set_num2(hour_text, hour);
    set_num2(minute_text, minute);

    ui_rect h_minus = { c.x + 402, c.y + 308, 48, 40 };
    ui_rect h_plus  = { c.x + 458, c.y + 308, 48, 40 };
    ui_rect m_minus = { c.x + 402, c.y + 358, 48, 40 };
    ui_rect m_plus  = { c.x + 458, c.y + 358, 48, 40 };

    ui_text_transparent(c.x + 330, c.y + 312, LTR("HOUR"), C_DIM);
    gui_button(h_minus, "-", g_point_in(&h_minus, g_mx, g_my), 0);
    gui_button(h_plus, "+", g_point_in(&h_plus, g_mx, g_my), 0);
    ui_text_transparent(c.x + 518, c.y + 312, hour_text, C_TEXT);

    ui_text_transparent(c.x + 330, c.y + 362, LTR("MIN"), C_DIM);
    gui_button(m_minus, "-", g_point_in(&m_minus, g_mx, g_my), 0);
    gui_button(m_plus, "+", g_point_in(&m_plus, g_mx, g_my), 0);
    ui_text_transparent(c.x + 518, c.y + 362, minute_text, C_TEXT);

    ui_hline(c.x + 24, c.y + 420, c.w - 48, C_BORDER);

    ui_text_transparent(c.x + 24, c.y + 434, LTR("Config: etc\\system.cfg"), C_DIM);

    const struct AuthUser *u = auth_current();
    char user_line[160];
    p = 0;
    const char *sess = LTR("Session: ");
    g_copy(user_line, sess, 40); p += g_strlen(sess);
    g_copy(user_line + p, u ? u->name : "?", 24); p += g_strlen(u ? u->name : "?");
    const char *home_tag = LTR("   Home: ");
    g_copy(user_line + p, home_tag, 40); p += g_strlen(home_tag);
    g_copy(user_line + p, (u && u->home[0]) ? u->home : "\\", 40);

    gui_text_n(c.x + 24, c.y + 466, user_line, (c.w - 48) / 16, C_DIM);

    ui_text_transparent(c.x + 24, c.y + 498,
                        LTR("Edit: nano etc\\system.cfg   (TTY)"), C_DIM);

    if (g_set_msg[0]) {
        gui_text_n(c.x + 24, c.y + 530, g_set_msg, (c.w - 48) / 16, C_OK);
    } else {
        ui_text_transparent(c.x + 24, c.y + 530,
                            LTR("Reboot to apply boot mode."), C_WARN);
    }
}

static void gui_settings_click(ui_rect c, int x, int y) {
    if (gui_app_active(WIN_SETTINGS)) {
        gui_app_click(WIN_SETTINGS, x - c.x, y - c.y, 1);
        return;
    }

    int gui = system_cfg_boot_gui();

    ui_rect b1 = { c.x + 24,  c.y + 118, 280, 52 };
    ui_rect b2 = { c.x + 320, c.y + 118, 280, 52 };

    ui_rect lang_en = { c.x + c.w - 292, c.y + 38, 130, 40 };
    ui_rect lang_zh = { c.x + c.w - 150, c.y + 38, 130, 40 };

    ui_rect tz_minus = { c.x + 24,  c.y + 272, 48, 40 };
    ui_rect tz_plus  = { c.x + 232, c.y + 272, 48, 40 };

    ui_rect h_minus = { c.x + 402, c.y + 308, 48, 40 };
    ui_rect h_plus  = { c.x + 458, c.y + 308, 48, 40 };
    ui_rect m_minus = { c.x + 402, c.y + 358, 48, 40 };
    ui_rect m_plus  = { c.x + 458, c.y + 358, 48, 40 };

    if (g_point_in(&lang_en, x, y) && lang_current() != LANG_EN) {
        lang_set(LANG_EN);
        if (!system_cfg_set_lang(LANG_EN)) {
            g_copy(g_set_msg, "Cannot write etc\\system.cfg", 60);
        } else {
            g_copy(g_set_msg, LTR("Language switched."), 60);
        }
        g_set_msg_until = net_now_ms() + 3200;
        dirty_window(WIN_SETTINGS);
        return;
    } else if (g_point_in(&lang_zh, x, y) && lang_current() != LANG_ZH) {
        lang_set(LANG_ZH);
        if (!system_cfg_set_lang(LANG_ZH)) {
            g_copy(g_set_msg, "Cannot write etc\\system.cfg", 60);
        } else {
            g_copy(g_set_msg, LTR("Language switched."), 60);
        }
        g_set_msg_until = net_now_ms() + 3200;
        dirty_window(WIN_SETTINGS);
        return;
    }

    if (g_point_in(&b1, x, y) && !gui) {
        if (system_cfg_set_boot_gui(1)) {
            g_copy(g_set_msg, "Saved. Reboot to apply.", 60);
        } else {
            g_copy(g_set_msg, "Cannot write etc\\system.cfg", 60);
        }
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&b2, x, y) && gui) {
        if (system_cfg_set_boot_gui(0)) {
            g_copy(g_set_msg, "Saved. Reboot to apply.", 60);
        } else {
            g_copy(g_set_msg, "Cannot write etc\\system.cfg", 60);
        }
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&tz_minus, x, y)) {
        int tz = system_cfg_timezone();
        if (tz <= -12) {
            g_copy(g_set_msg, "Timezone at minimum (-12).", 60);
        } else if (system_cfg_set_timezone(tz - 1)) {
            g_copy(g_set_msg, "Timezone updated.", 60);
        } else {
            g_copy(g_set_msg, "Cannot write etc\\system.cfg", 60);
        }
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&tz_plus, x, y)) {
        int tz = system_cfg_timezone();
        if (tz >= 14) {
            g_copy(g_set_msg, "Timezone at maximum (+14).", 60);
        } else if (system_cfg_set_timezone(tz + 1)) {
            g_copy(g_set_msg, "Timezone updated.", 60);
        } else {
            g_copy(g_set_msg, "Cannot write etc\\system.cfg", 60);
        }
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&h_minus, x, y)) {
        g_copy(g_set_msg, set_clock_shift(-60) ? "Clock updated." : "Cannot write CMOS clock", 60);
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&h_plus, x, y)) {
        g_copy(g_set_msg, set_clock_shift(60) ? "Clock updated." : "Cannot write CMOS clock", 60);
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&m_minus, x, y)) {
        g_copy(g_set_msg, set_clock_shift(-1) ? "Clock updated." : "Cannot write CMOS clock", 60);
        g_set_msg_until = net_now_ms() + 3200;
    } else if (g_point_in(&m_plus, x, y)) {
        g_copy(g_set_msg, set_clock_shift(1) ? "Clock updated." : "Cannot write CMOS clock", 60);
        g_set_msg_until = net_now_ms() + 3200;
    }
}

/* ============================================================
 * 任务管理器
 * ------------------------------------------------------------
 * 三个页面：
 *   Overview  —— CPU 总占用与逐核占用、内存、磁盘、swap
 *   Processes —— 独立进程页：PID / 名称 / 状态 / CPU / 内存 / 磁盘写入
 *   Devices   —— CPU 型号、核数、内存容量、磁盘、显示、网络
 *
 * 数据来源全部是内核既有模块：
 *   smp.c   在线核数与 APIC ID
 *   task.c  任务表、steps、内存记账、磁盘写入记账
 *   mem.c   内核堆统计
 *   vm.c    物理 RAM、页帧池、swap
 *   shell.c ATA 累计读写扇区
 *   net.c   网卡与链路信息
 *
 * CPU 占用没有硬件性能计数器可用，采用"帧内忙时间比例"：
 * 主循环用 rdtsc 记录每帧从开始到 present 完成的时间，除以整帧时间，
 * 再做 8 帧滑动平均。它能真实反映 GUI/内核当前的忙碌程度，并且
 * 在打开窗口、重绘、执行命令时明显上升。
 * ============================================================ */

#define TM_TAB_OVERVIEW  0
#define TM_TAB_PROCESSES 1
#define TM_TAB_DEVICES   2
#define TM_TAB_COUNT     3

static int      g_tm_tab = TM_TAB_OVERVIEW;
static uint32_t g_tm_cpu_permille = 0;       /* 系统 CPU 占用 0..1000 */

/* SLYZ 任务管理器通过这个访问器读取 CPU 占用，避免把内部变量暴露成全局符号 */
uint32_t gui_system_cpu_permille(void) {
    return g_tm_cpu_permille;
}
static uint32_t g_tm_proc_cpu[TASK_MAX];
static uint32_t g_tm_proc_steps[TASK_MAX];
static uint32_t g_tm_last_sample_ms = 0;
static uint32_t g_tm_last_write_sectors = 0;
static uint32_t g_tm_disk_rate_kb = 0;
static uint32_t g_tm_uptime_sec = 0;
static uint32_t g_tm_core_units[SMP_MAX_CPUS];
static uint32_t g_tm_core_permille[SMP_MAX_CPUS];

static uint64_t gui_rdtsc(void) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* 十进制无符号数（uint64 版本，g_utoa 只接受 32 位） */
static void tm_utoa64(uint64_t v, char *out, int cap) {
    char tmp[24];
    int n = 0;
    if (cap <= 0) return;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
    }
    int i = 0;
    while (n > 0 && i < cap - 1) out[i++] = tmp[--n];
    out[i] = '\0';
}

/* 字节数 -> "123 B" / "4.5 KB" / "36.0 MB" */
static void tm_fmt_bytes(uint64_t v, char *out, int cap) {
    char num[24];
    uint32_t whole;
    uint32_t frac = 0;
    const char *unit;

    if (v < 1024ull) {
        whole = (uint32_t)v;
        unit = " B";
    } else if (v < 1024ull * 1024ull) {
        whole = (uint32_t)(v / 1024ull);
        frac = (uint32_t)((v % 1024ull) * 10ull / 1024ull);
        unit = " KB";
    } else if (v < 1024ull * 1024ull * 1024ull) {
        whole = (uint32_t)(v / (1024ull * 1024ull));
        frac = (uint32_t)((v % (1024ull * 1024ull)) * 10ull / (1024ull * 1024ull));
        unit = " MB";
    } else {
        whole = (uint32_t)(v / (1024ull * 1024ull * 1024ull));
        frac = (uint32_t)((v % (1024ull * 1024ull * 1024ull)) * 10ull /
                          (1024ull * 1024ull * 1024ull));
        unit = " GB";
    }

    g_utoa(whole, num);
    int p = 0;
    int i = 0;
    while (num[i] && p < cap - 1) out[p++] = num[i++];
    if (v >= 1024ull && p < cap - 4) {
        out[p++] = '.';
        out[p++] = (char)('0' + frac);
    }
    i = 0;
    while (unit[i] && p < cap - 1) out[p++] = unit[i++];
    out[p] = '\0';
}

/* CPU 品牌字符串（CPUID 0x80000002..0x80000004） */
static void tm_cpu_brand(char *out, int cap) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (a < 0x80000004u) {
        g_copy(out, "x86_64 processor", cap);
        return;
    }

    char raw[49];
    int p = 0;
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf));
        uint32_t v[4];
        v[0] = a; v[1] = b; v[2] = c; v[3] = d;
        for (int k = 0; k < 4; k++) {
            raw[p++] = (char)(v[k] & 0xFFu);
            raw[p++] = (char)((v[k] >> 8) & 0xFFu);
            raw[p++] = (char)((v[k] >> 16) & 0xFFu);
            raw[p++] = (char)((v[k] >> 24) & 0xFFu);
        }
    }
    raw[48] = '\0';

    int start = 0;
    while (start < 48 && raw[start] == ' ') start++;
    g_copy(out, raw + start, cap);
}

/* 进度条：permille = 0..1000 */
static void tm_bar(ui_rect r, uint32_t permille, uint32_t color) {
    ui_round_fill(r, r.h / 2, 0x0C141E);
    int w = (int)(((uint64_t)r.w * (uint64_t)permille) / 1000ull);
    if (w > r.w) w = r.w;
    if (w >= 4) {
        ui_round_fill((ui_rect){ r.x, r.y, w, r.h }, r.h / 2, color);
    }
}

static ui_rect tm_tab_rect(ui_rect c, int i) {
    return (ui_rect){ c.x + i * 170, c.y, 160, 44 };
}

/* 每秒采样一次：CPU 占用、每任务 CPU%、磁盘写入速率、运行时间。
 * 返回 1 表示产生了新样本，主循环据此把任务管理器窗口标脏。 */
static int gui_tm_sample(void) {
    uint32_t now = net_now_ms();
    uint32_t dt;

    if (g_tm_last_sample_ms == 0) {
        dt = 1000u;
    } else {
        dt = now - g_tm_last_sample_ms;
        if (dt < 1000u) return 0;
    }
    if (dt == 0) dt = 1u;

    uint32_t reads = 0;
    uint32_t writes = 0;
    blk_stats(&reads, &writes);
    (void)reads;

    uint32_t delta_sectors = writes - g_tm_last_write_sectors;
    g_tm_last_write_sectors = writes;
    uint64_t delta_bytes = (uint64_t)delta_sectors * 512ull;
    g_tm_disk_rate_kb = (uint32_t)((delta_bytes * 1000ull) / (uint64_t)dt / 1024ull);

    uint32_t delta_steps[TASK_MAX];
    uint32_t total_steps = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct Task *t = task_by_id(i);
        if (!t) {
            delta_steps[i] = 0;
            g_tm_proc_cpu[i] = 0;
            continue;
        }
        delta_steps[i] = t->steps - g_tm_proc_steps[i];
        g_tm_proc_steps[i] = t->steps;
        total_steps += delta_steps[i];
    }

    uint32_t user_sum = 0;
    for (int i = 1; i < TASK_MAX; i++) {
        if (total_steps == 0) {
            g_tm_proc_cpu[i] = 0;
        } else {
            g_tm_proc_cpu[i] =
                (uint32_t)(((uint64_t)g_tm_cpu_permille * (uint64_t)delta_steps[i]) /
                           (uint64_t)total_steps);
        }
        user_sum += g_tm_proc_cpu[i];
    }
    /* kernel 行吸收 GUI 与内核活动：系统占用 - 用户任务占用 */
    g_tm_proc_cpu[0] = (g_tm_cpu_permille > user_sum) ? (g_tm_cpu_permille - user_sum) : 0;

    /* 逐核占用：用每核实际执行的并行分片数做权重。
     * 系统总占用来自主循环帧内忙时间，分片权重决定它在各核之间的分配；
     * 若采样窗口内没有任何并行任务，则全部记到 BSP 上。 */
    int cpu_n = smp_cpu_count();
    if (cpu_n < 1) cpu_n = 1;
    if (cpu_n > SMP_MAX_CPUS) cpu_n = SMP_MAX_CPUS;

    uint32_t delta_units[SMP_MAX_CPUS];
    uint32_t total_units = 0;
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        uint32_t u = smp_cpu_work_units(i);
        delta_units[i] = u - g_tm_core_units[i];
        g_tm_core_units[i] = u;
        if (i < cpu_n) total_units += delta_units[i];
    }

    int bsp = smp_current_cpu();
    if (bsp < 0 || bsp >= SMP_MAX_CPUS) bsp = 0;

    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        if (i >= cpu_n) {
            g_tm_core_permille[i] = 0;
            continue;
        }
        if (total_units == 0) {
            g_tm_core_permille[i] = (i == bsp) ? g_tm_cpu_permille : 0;
        } else {
            g_tm_core_permille[i] =
                (uint32_t)(((uint64_t)g_tm_cpu_permille * (uint64_t)delta_units[i]) /
                           (uint64_t)total_units);
        }
    }

    g_tm_uptime_sec = now / 1000u;
    g_tm_last_sample_ms = now;
    return 1;
}

static const char *tm_state_name(int state) {
    if (state == TASK_RUNNING) return "running";
    if (state == TASK_READY) return "ready";
    if (state == TASK_DEAD) return "dead";
    return "free";
}

/* ---------- Overview ---------- */

static void gui_tm_render_overview(ui_rect p) {
    int left_w = (p.w * 54) / 100;
    int right_x = p.x + left_w + 12;
    int right_w = p.w - left_w - 12;

    /* 逐核列表决定 CPU 卡高度：卡片必须包住所有行，不能伸出内容区。
     * 超过 8 核时只显示前 8 个，避免页面被无限撑高。 */
    int cores = smp_cpu_count();
    if (cores < 1) cores = 1;
    if (cores > 8) cores = 8;

    int cpu_card_h = 156 + cores * 56;
    if (cpu_card_h > p.h - 130) {
        cpu_card_h = p.h - 130;
    }
    int rows = (cpu_card_h - 158) / 56;
    if (rows < 1) rows = 1;
    if (rows > cores) rows = cores;

    /* CPU 卡片 */
    ui_rect cpu_card = { p.x, p.y, left_w, cpu_card_h };
    ui_round_panel(cpu_card, 14, C_BORDER, C_PANEL);

    ui_text_transparent(cpu_card.x + 20, cpu_card.y + 12, "CPU", C_DIM);

    /* 每核累计执行的并行分片数：AP 只在全屏重绘等大任务时参与，
     * 瞬时占用可能为 0，但累计值能证明它们确实在工作。 */
    char par[72];
    int pp = 0;
    g_copy(par, "units ", sizeof(par));
    pp = 6;
    for (int ci = 0; ci < cores; ci++) {
        char nb[12];
        g_utoa(smp_cpu_work_units(ci), nb);
        if (ci > 0) par[pp++] = ' ';
        g_copy(par + pp, nb, 12);
        pp += g_strlen(nb);
    }
    par[pp] = '\0';
    gui_text_n(cpu_card.x + 160, cpu_card.y + 12, par,
               (cpu_card.w - 180) / 16, C_DIM);

    char big[16];
    g_utoa(g_tm_cpu_permille / 10u, big);
    int len = g_strlen(big);
    big[len] = '%';
    big[len + 1] = '\0';
    /* ui_text_big 是 32x64 的二倍字形，"100%" 宽 128、高 64。
     * 大数字占 y+38..y+102，进度条必须从 y+112 开始，不能压到字形上。 */
    ui_text_big(cpu_card.x + 20, cpu_card.y + 38, big, C_TEXT);

    ui_text_transparent(cpu_card.x + 160, cpu_card.y + 54,
                        "overall busy time", C_DIM);

    ui_rect bar = { cpu_card.x + 20, cpu_card.y + 112, cpu_card.w - 40, 14 };
    tm_bar(bar, g_tm_cpu_permille,
           g_tm_cpu_permille > 800u ? C_ERR :
           (g_tm_cpu_permille > 500u ? C_WARN : C_OK));

    /* 每核两行：第一行标签与百分比，第二行进度条。
     * 单行塞入 "CORE / APIC / 进度条 / 百分比" 在窄窗口下必然互相压字，
     * 纵向拆开可以从根本上避免重叠。 */
    int y = cpu_card.y + 150;
    for (int i = 0; i < rows; i++) {
        uint32_t used = g_tm_core_permille[i];

        char label[16];
        g_copy(label, "CORE ", sizeof(label));
        char idx[4];
        g_utoa((uint32_t)i, idx);
        g_copy(label + 5, idx, 8);
        ui_text_transparent(cpu_card.x + 20, y, label, C_TEXT);

        char meta[32];
        g_copy(meta, "APIC ", sizeof(meta));
        char apic[6];
        g_utoa((uint32_t)smp_cpu_apic_id(i), apic);
        g_copy(meta + 5, apic, 8);
        g_copy(meta + 5 + g_strlen(apic), smp_cpu_is_online(i) ? " up" : " down", 10);
        gui_text_n(cpu_card.x + 130, y, meta, 12, C_DIM);

        char pct[8];
        g_utoa(used / 10u, pct);
        int pl = g_strlen(pct);
        pct[pl] = '%';
        pct[pl + 1] = '\0';
        ui_text_transparent(cpu_card.x + cpu_card.w - 68, y, pct, C_TEXT);

        ui_rect cb = { cpu_card.x + 20, y + 34, cpu_card.w - 40, 10 };
        tm_bar(cb, used, used > 0 ? C_ACCENT3 : 0x2B3949);

        y += 56;
    }

    /* 内存卡片 */
    uint32_t heap_total = 0, heap_used = 0, heap_peak = 0, allocs = 0, frees = 0, failed = 0;
    mem_stats(&heap_total, &heap_used, &heap_peak, &allocs, &frees, &failed);

    uint64_t ram = vm_total_ram();
    uint32_t frames_total = 0, frames_free = 0, resident = 0;
    vm_frame_stats(&frames_total, &frames_free, &resident);

    uint64_t frame_used_bytes = (uint64_t)(frames_total - frames_free) * 4096ull;

    ui_rect mem_card = { right_x, p.y, right_w, 172 };
    ui_round_panel(mem_card, 14, C_BORDER, C_PANEL);
    ui_text_transparent(mem_card.x + 20, mem_card.y + 12, "MEMORY", C_DIM);

    char vbuf[32];
    char vbuf2[32];
    tm_fmt_bytes(ram, vbuf, sizeof(vbuf));
    tm_fmt_bytes(frame_used_bytes, vbuf2, sizeof(vbuf2));

    ui_text_transparent(mem_card.x + 20, mem_card.y + 44, vbuf, C_TEXT);
    ui_text_transparent(mem_card.x + 20, mem_card.y + 80, "physical RAM", C_DIM);

    ui_rect mb = { mem_card.x + 20, mem_card.y + 112, mem_card.w - 40, 14 };
    uint32_t mem_permille = (ram > 0) ?
        (uint32_t)((frame_used_bytes * 1000ull) / ram) : 0u;
    tm_bar(mb, mem_permille, C_ACCENT);

    char line[96];
    int lp = 0;
    g_copy(line, "paged ", sizeof(line));
    lp = 6;
    g_copy(line + lp, vbuf2, 24);
    lp += g_strlen(vbuf2);
    g_copy(line + lp, "   heap ", 10);
    lp += 7;
    tm_fmt_bytes((uint64_t)heap_used, vbuf, sizeof(vbuf));
    g_copy(line + lp, vbuf, 24);
    gui_text_n(mem_card.x + 20, mem_card.y + 134, line,
               (mem_card.w - 40) / 16, C_DIM);

    /* 磁盘卡片 */
    ui_rect disk_card = { right_x, p.y + 184, right_w, 150 };
    ui_round_panel(disk_card, 14, C_BORDER, C_PANEL);
    ui_text_transparent(disk_card.x + 20, disk_card.y + 12, "DISK", C_DIM);

    uint32_t rd = 0, wr = 0;
    blk_stats(&rd, &wr);
    uint64_t written_bytes = (uint64_t)wr * 512ull;

    char rate[32];
    g_utoa(g_tm_disk_rate_kb, rate);
    int rl = g_strlen(rate);
    g_copy(rate + rl, " KB/s", 8);
    ui_text_big(disk_card.x + 20, disk_card.y + 44, rate, C_TEXT);

    char wtxt[32];
    tm_fmt_bytes(written_bytes, wtxt, sizeof(wtxt));
    g_copy(line, "total written ", sizeof(line));
    lp = 14;
    g_copy(line + lp, wtxt, 24);
    gui_text_n(disk_card.x + 20, disk_card.y + 78, line,
               (disk_card.w - 40) / 16, C_DIM);

    char sectors[64];
    g_copy(sectors, "read ", sizeof(sectors));
    lp = 5;
    char nbuf[16];
    tm_utoa64(rd, nbuf, sizeof(nbuf));
    g_copy(sectors + lp, nbuf, 16);
    lp += g_strlen(nbuf);
    g_copy(sectors + lp, "  write ", 10);
    lp += 8;
    tm_utoa64(wr, nbuf, sizeof(nbuf));
    g_copy(sectors + lp, nbuf, 16);
    lp += g_strlen(nbuf);
    g_copy(sectors + lp, " sectors", 10);
    gui_text_n(disk_card.x + 20, disk_card.y + 112, sectors,
               (disk_card.w - 40) / 16, C_DIM);

    /* swap 卡片 */
    ui_rect swap_card = { right_x, p.y + 346, right_w, 120 };
    ui_round_panel(swap_card, 14, C_BORDER, C_PANEL);
    ui_text_transparent(swap_card.x + 20, swap_card.y + 12, "SWAP", C_DIM);

    uint32_t slots_used = vm_swap_slots_used();
    uint32_t slots_cap = vm_swap_slot_capacity();
    uint32_t slot_permille = (slots_cap > 0) ?
        (uint32_t)(((uint64_t)slots_used * 1000ull) / (uint64_t)slots_cap) : 0u;

    g_copy(line, "slots ", sizeof(line));
    lp = 6;
    g_utoa(slots_used, nbuf);
    g_copy(line + lp, nbuf, 16);
    lp += g_strlen(nbuf);
    g_copy(line + lp, " / ", 5);
    lp += 3;
    g_utoa(slots_cap, nbuf);
    g_copy(line + lp, nbuf, 16);
    ui_text_transparent(swap_card.x + 20, swap_card.y + 44, line, C_TEXT);

    ui_rect sb = { swap_card.x + 20, swap_card.y + 78, swap_card.w - 40, 8 };
    tm_bar(sb, slot_permille, C_WARN);

    g_copy(line, "out ", sizeof(line));
    lp = 4;
    g_utoa(vm_swapout_count(), nbuf);
    g_copy(line + lp, nbuf, 16);
    lp += g_strlen(nbuf);
    g_copy(line + lp, " pages   in ", 12);
    lp += 11;
    g_utoa(vm_swapin_count(), nbuf);
    g_copy(line + lp, nbuf, 16);
    gui_text_n(swap_card.x + 20, swap_card.y + 86, line,
               (swap_card.w - 40) / 16, C_DIM);

    /* 左下：进程摘要（紧跟在 CPU 卡下方，高度自适应剩余空间） */
    int proc_y = p.y + cpu_card_h + 12;
    int proc_h = p.h - cpu_card_h - 12;
    if (proc_h < 118) proc_h = 118;
    if (proc_y + proc_h > p.y + p.h) proc_h = p.y + p.h - proc_y;
    ui_rect proc_card = { p.x, proc_y, left_w, proc_h };
    ui_round_panel(proc_card, 14, C_BORDER, C_PANEL);
    ui_text_transparent(proc_card.x + 20, proc_card.y + 12, "PROCESSES", C_DIM);

    g_copy(line, "alive ", sizeof(line));
    lp = 6;
    g_utoa((uint32_t)task_count_alive(), nbuf);
    g_copy(line + lp, nbuf, 16);
    lp += g_strlen(nbuf);
    g_copy(line + lp, " / 4 slots", 12);
    ui_text_transparent(proc_card.x + 20, proc_card.y + 46, line, C_TEXT);

    char uptime[40];
    g_copy(uptime, "uptime ", sizeof(uptime));
    lp = 7;
    uint32_t up = g_tm_uptime_sec;
    uint32_t hh = up / 3600u;
    uint32_t mm = (up % 3600u) / 60u;
    uint32_t ss = up % 60u;
    g_utoa(hh, nbuf);
    g_copy(uptime + lp, nbuf, 12);
    lp += g_strlen(nbuf);
    g_copy(uptime + lp, "h ", 4);
    lp += 2;
    g_utoa(mm, nbuf);
    g_copy(uptime + lp, nbuf, 12);
    lp += g_strlen(nbuf);
    g_copy(uptime + lp, "m ", 4);
    lp += 2;
    g_utoa(ss, nbuf);
    g_copy(uptime + lp, nbuf, 12);
    lp += g_strlen(nbuf);
    g_copy(uptime + lp, "s", 4);
    ui_text_transparent(proc_card.x + 20, proc_card.y + 82, uptime, C_DIM);
}

/* ---------- Processes ---------- */

static void gui_tm_render_processes(ui_rect p) {
    int x_pid  = p.x + 16;
    int x_name = p.x + 90;
    int x_stat = p.x + 300;
    int x_cpu  = p.x + 430;
    int x_mem  = p.x + 530;
    int x_disk = p.x + 700;

    ui_text_transparent(x_pid, p.y + 8, "PID", C_DIM);
    ui_text_transparent(x_name, p.y + 8, "PROCESS", C_DIM);
    ui_text_transparent(x_stat, p.y + 8, "STATE", C_DIM);
    ui_text_transparent(x_cpu, p.y + 8, "CPU", C_DIM);
    ui_text_transparent(x_mem, p.y + 8, "MEMORY", C_DIM);
    ui_text_transparent(x_disk, p.y + 8, "DISK WRITE", C_DIM);

    ui_hline(p.x + 8, p.y + 40, p.w - 16, C_BORDER);

    uint32_t heap_total = 0, heap_used = 0, heap_peak = 0;
    uint32_t allocs = 0, frees = 0, failed = 0;
    mem_stats(&heap_total, &heap_used, &heap_peak, &allocs, &frees, &failed);

    uint32_t rd = 0, wr = 0;
    blk_stats(&rd, &wr);
    (void)rd;

    uint32_t task_mem = task_total_used();
    uint32_t task_disk = task_total_disk_written();

    uint64_t disk_total_bytes = (uint64_t)wr * 512ull;
    uint64_t disk_task_bytes = (uint64_t)task_disk;

    int y = p.y + 52;

    for (int i = 0; i < TASK_MAX; i++) {
        struct Task *t = task_by_id(i);
        if (!t) continue;

        ui_rect row = { p.x + 8, y - 4, p.w - 16, 40 };
        if ((i & 1) == 0) {
            ui_round_fill(row, 8, 0x121A25);
        }

        char num[16];
        g_utoa((uint32_t)i, num);
        ui_text_transparent(x_pid, y, num, C_TEXT);

        gui_text_n(x_name, y, t->name[0] ? t->name : "(unnamed)", 12,
                   i == 0 ? C_ACCENT3 : C_TEXT);

        uint32_t state = (uint32_t)t->state;
        const char *sn = tm_state_name((int)state);
        ui_text_transparent(x_stat, y, sn,
                           (state == TASK_RUNNING) ? C_OK :
                           (state == TASK_DEAD) ? C_ERR : C_DIM);

        char pct[8];
        g_utoa(g_tm_proc_cpu[i] / 10u, pct);
        int pl = g_strlen(pct);
        pct[pl] = '%';
        pct[pl + 1] = '\0';
        ui_text_transparent(x_cpu, y, pct, C_TEXT);

        uint32_t mem_bytes = t->used;
        if (i == 0) {
            /* kernel 行聚合内核堆与 GUI 会话的占用 */
            mem_bytes = (heap_used > task_mem) ? (heap_used - task_mem) : 0;
        }
        char mtext[24];
        tm_fmt_bytes((uint64_t)mem_bytes, mtext, sizeof(mtext));
        ui_text_transparent(x_mem, y, mtext, C_DIM);

        uint64_t disk_bytes = (uint64_t)t->disk_written;
        if (i == 0) {
            disk_bytes = (disk_total_bytes > disk_task_bytes) ?
                         (disk_total_bytes - disk_task_bytes) : 0;
        }
        char dtext[24];
        tm_fmt_bytes(disk_bytes, dtext, sizeof(dtext));
        ui_text_transparent(x_disk, y, dtext, C_DIM);

        y += 44;
        if (y > p.y + p.h - 60) break;
    }

    ui_hline(p.x + 8, p.y + p.h - 84, p.w - 16, C_BORDER);
    gui_text_n(p.x + 16, p.y + p.h - 76,
               "PID 0 aggregates kernel, GUI, filesystem and swap.",
               (p.w - 32) / 16, C_DIM);
    gui_text_n(p.x + 16, p.y + p.h - 40,
               "Keys: 1 overview / 2 processes / 3 devices",
               (p.w - 32) / 16, C_DIM);
}

/* ---------- Devices ---------- */

static void tm_hex(uint32_t v, char *out, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    for (int i = 0; i < digits; i++) {
        out[i] = hex[(v >> ((digits - 1 - i) * 4)) & 0xFu];
    }
    out[digits] = '\0';
}

static void gui_tm_render_devices(ui_rect p) {
    int col_w = (p.w - 12) / 2;
    int rx = p.x + col_w + 12;
    int card_h = 162;
    int gap = 10;

    char brand[64];
    tm_cpu_brand(brand, sizeof(brand));

    uint64_t ram = vm_total_ram();
    uint32_t frames_total = 0, frames_free = 0, resident = 0;
    vm_frame_stats(&frames_total, &frames_free, &resident);

    uint32_t heap_total = 0, heap_used = 0, heap_peak = 0;
    uint32_t allocs = 0, frees = 0, failed = 0;
    mem_stats(&heap_total, &heap_used, &heap_peak, &allocs, &frees, &failed);

    uint32_t rd = 0, wr = 0;
    blk_stats(&rd, &wr);

    char buf[96];
    char val[32];
    int bp;

    /* ---- Processor ---- */
    ui_rect c1 = { p.x, p.y, col_w, card_h };
    ui_round_panel(c1, 14, C_BORDER, C_PANEL);
    ui_text_transparent(c1.x + 18, c1.y + 10, "PROCESSOR", C_DIM);
    gui_text_n(c1.x + 18, c1.y + 42, brand, (c1.w - 36) / 16, C_TEXT);

    g_copy(buf, "logical cores ", sizeof(buf));
    bp = 14;
    g_utoa((uint32_t)smp_cpu_count(), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, "   online ", 12);
    bp += 10;
    g_utoa((uint32_t)smp_online_count(), val);
    g_copy(buf + bp, val, 16);
    gui_text_n(c1.x + 18, c1.y + 80, buf, (c1.w - 36) / 16, C_DIM);

    g_copy(buf, "BSP APIC ID ", sizeof(buf));
    bp = 12;
    g_utoa((uint32_t)smp_cpu_apic_id(0), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, "   LAPIC 0x", 14);
    bp += 11;
    tm_hex(smp_lapic_base(), val, 8);
    g_copy(buf + bp, val, 16);
    gui_text_n(c1.x + 18, c1.y + 112, buf, (c1.w - 36) / 16, C_DIM);

    /* ---- Memory ---- */
    ui_rect c2 = { p.x, p.y + card_h + gap, col_w, card_h };
    ui_round_panel(c2, 14, C_BORDER, C_PANEL);
    ui_text_transparent(c2.x + 18, c2.y + 10, "MEMORY", C_DIM);

    tm_fmt_bytes(ram, val, sizeof(val));
    g_copy(buf, "installed ", sizeof(buf));
    bp = 10;
    g_copy(buf + bp, val, 24);
    gui_text_n(c2.x + 18, c2.y + 44, buf, (c2.w - 36) / 16, C_TEXT);

    tm_fmt_bytes((uint64_t)frames_total * 4096ull, val, sizeof(val));
    g_copy(buf, "frame pool ", sizeof(buf));
    bp = 11;
    g_copy(buf + bp, val, 24);
    bp += g_strlen(val);
    g_copy(buf + bp, "   free ", 12);
    bp += 8;
    g_utoa(frames_free, val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " pages", 10);
    gui_text_n(c2.x + 18, c2.y + 78, buf, (c2.w - 36) / 16, C_DIM);

    tm_fmt_bytes((uint64_t)heap_used, val, sizeof(val));
    g_copy(buf, "kernel heap used ", sizeof(buf));
    bp = 17;
    g_copy(buf + bp, val, 24);
    bp += g_strlen(val);
    g_copy(buf + bp, " of ", 8);
    bp += 4;
    tm_fmt_bytes((uint64_t)heap_total, val, sizeof(val));
    g_copy(buf + bp, val, 24);
    gui_text_n(c2.x + 18, c2.y + 112, buf, (c2.w - 36) / 16, C_DIM);

    /* ---- Storage ---- */
    ui_rect c3 = { p.x, p.y + (card_h + gap) * 2, col_w, card_h };
    ui_round_panel(c3, 14, C_BORDER, C_PANEL);
    ui_text_transparent(c3.x + 18, c3.y + 10, "STORAGE", C_DIM);
    ui_text_transparent(c3.x + 18, c3.y + 44, "ATA PIO (IDE primary master)", C_TEXT);

    uint32_t sectors = blk_total_sectors();
    tm_fmt_bytes((uint64_t)sectors * 512ull, val, sizeof(val));
    g_copy(buf, "capacity ", sizeof(buf));
    bp = 9;
    g_copy(buf + bp, val, 24);
    bp += g_strlen(val);
    g_copy(buf + bp, "   ", 6);
    bp += 3;
    tm_utoa64((uint64_t)sectors, val, sizeof(val));
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " sectors", 10);
    gui_text_n(c3.x + 18, c3.y + 78, buf, (c3.w - 36) / 16, C_DIM);

    g_copy(buf, "reads ", sizeof(buf));
    bp = 6;
    tm_utoa64((uint64_t)rd, val, sizeof(val));
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, "   writes ", 12);
    bp += 10;
    tm_utoa64((uint64_t)wr, val, sizeof(val));
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " sectors", 10);
    gui_text_n(c3.x + 18, c3.y + 112, buf, (c3.w - 36) / 16, C_DIM);

    /* ---- Display ---- */
    ui_rect c4 = { rx, p.y, col_w, card_h };
    ui_round_panel(c4, 14, C_BORDER, C_PANEL);
    ui_text_transparent(c4.x + 18, c4.y + 10, "DISPLAY", C_DIM);

    g_copy(buf, "VBE ", sizeof(buf));
    bp = 4;
    g_utoa((uint32_t)vbe_current.width, val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " x ", 4);
    bp += 3;
    g_utoa((uint32_t)vbe_current.height, val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " x ", 4);
    bp += 3;
    g_utoa((uint32_t)vbe_current.bpp, val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " bpp", 6);
    gui_text_n(c4.x + 18, c4.y + 44, buf, (c4.w - 36) / 16, C_TEXT);

    g_copy(buf, "framebuffer 0x", sizeof(buf));
    bp = 14;
    tm_hex(vbe_current.framebuffer, val, 8);
    g_copy(buf + bp, val, 16);
    gui_text_n(c4.x + 18, c4.y + 78, buf, (c4.w - 36) / 16, C_DIM);

    g_copy(buf, "pitch ", sizeof(buf));
    bp = 6;
    g_utoa((uint32_t)vbe_current.pitch, val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " bytes   terminal ", 20);
    bp += 18;
    g_utoa((uint32_t)tty_get_cols(), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, "x", 2);
    bp += 1;
    g_utoa((uint32_t)tty_get_rows(), val);
    g_copy(buf + bp, val, 16);
    gui_text_n(c4.x + 18, c4.y + 112, buf, (c4.w - 36) / 16, C_DIM);

    /* ---- Network ---- */
    struct NetInfo *ni = net_info();
    ui_rect c5 = { rx, p.y + card_h + gap, col_w, card_h };
    ui_round_panel(c5, 14, C_BORDER, C_PANEL);
    ui_text_transparent(c5.x + 18, c5.y + 10, "NETWORK", C_DIM);

    if (ni && ni->present) {
        g_copy(buf, "Intel E1000  ", sizeof(buf));
        bp = 13;
        tm_hex(ni->vendor_id, val, 4);
        g_copy(buf + bp, val, 8);
        bp += 4;
        g_copy(buf + bp, ":", 2);
        bp += 1;
        tm_hex(ni->device_id, val, 4);
        g_copy(buf + bp, val, 8);
        bp += 4;
        g_copy(buf + bp, "  pci ", 8);
        bp += 5;
        g_utoa((uint32_t)ni->bus, val);
        g_copy(buf + bp, val, 8);
        bp += g_strlen(val);
        g_copy(buf + bp, ":", 2);
        bp += 1;
        g_utoa((uint32_t)ni->slot, val);
        g_copy(buf + bp, val, 8);
        bp += g_strlen(val);
        g_copy(buf + bp, ".", 2);
        bp += 1;
        g_utoa((uint32_t)ni->func, val);
        g_copy(buf + bp, val, 8);
        gui_text_n(c5.x + 18, c5.y + 44, buf, (c5.w - 36) / 16, C_TEXT);

        char mac[18];
        net_format_mac(ni->mac, mac);
        g_copy(buf, "MAC ", sizeof(buf));
        bp = 4;
        g_copy(buf + bp, mac, 18);
        gui_text_n(c5.x + 18, c5.y + 78, buf, (c5.w - 36) / 16, C_DIM);

        char ip[16];
        net_format_ipv4(ni->ip, ip);
        g_copy(buf, "IPv4 ", sizeof(buf));
        bp = 5;
        g_copy(buf + bp, ip, 16);
        bp += g_strlen(ip);
        g_copy(buf + bp, net_ready() ? "   link up" : "   link down", 16);
        gui_text_n(c5.x + 18, c5.y + 112, buf, (c5.w - 36) / 16,
                   net_ready() ? C_OK : C_ERR);
    } else {
        ui_text_transparent(c5.x + 18, c5.y + 44, "No E1000 adapter detected", C_DIM);
    }

    /* ---- System ---- */
    ui_rect c6 = { rx, p.y + (card_h + gap) * 2, col_w, card_h };
    ui_round_panel(c6, 14, C_BORDER, C_PANEL);
    ui_text_transparent(c6.x + 18, c6.y + 10, "SYSTEM", C_DIM);
    ui_text_transparent(c6.x + 18, c6.y + 44, "SaylterOS v0.3 Beta1  x86_64 long mode", C_TEXT);

    g_copy(buf, "SMP ", sizeof(buf));
    bp = 4;
    g_utoa((uint32_t)smp_online_count(), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " online / ", 12);
    bp += 9;
    g_utoa((uint32_t)smp_cpu_count(), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " total   boot ", 16);
    bp += 13;
    g_copy(buf + bp, system_cfg_boot_gui() ? "graphical" : "text", 12);
    gui_text_n(c6.x + 18, c6.y + 78, buf, (c6.w - 36) / 16, C_DIM);

    g_copy(buf, "swap etc\\pagefile.os  ", sizeof(buf));
    bp = 22;
    g_utoa(vm_swap_slots_used(), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " / ", 5);
    bp += 3;
    g_utoa(vm_swap_slot_capacity(), val);
    g_copy(buf + bp, val, 16);
    bp += g_strlen(val);
    g_copy(buf + bp, " slots", 8);
    gui_text_n(c6.x + 18, c6.y + 112, buf, (c6.w - 36) / 16, C_DIM);
}

/* ---------- 入口 / 交互 ---------- */

static void gui_tm_render(const ui_theme *t, ui_rect c) {
    (void)t;

    if (gui_app_render(WIN_TASKMGR, c)) return;

    static const char *labels[TM_TAB_COUNT] = { "Overview", "Processes", "Devices" };

    for (int i = 0; i < TM_TAB_COUNT; i++) {
        ui_rect r = tm_tab_rect(c, i);
        int hover = g_point_in(&r, g_mx, g_my);
        uint32_t fill = (g_tm_tab == i) ? 0x1B2A3C : (hover ? 0x18232F : C_PANEL2);
        ui_round_panel(r, 10, (g_tm_tab == i) ? C_ACCENT : C_BORDER, fill);
        ui_text_transparent(r.x + 20, r.y + 6, labels[i],
                            (g_tm_tab == i) ? C_TEXT : C_DIM);
    }
    ui_hline(c.x, c.y + 52, c.w, C_BORDER);

    ui_rect page = { c.x, c.y + 64, c.w, c.h - 64 };
    if (g_tm_tab == TM_TAB_OVERVIEW) {
        gui_tm_render_overview(page);
    } else if (g_tm_tab == TM_TAB_PROCESSES) {
        gui_tm_render_processes(page);
    } else {
        gui_tm_render_devices(page);
    }
}

static void gui_tm_click(ui_rect c, int x, int y) {
    if (gui_app_active(WIN_TASKMGR)) {
        gui_app_click(WIN_TASKMGR, x - c.x, y - c.y, 1);
        return;
    }

    for (int i = 0; i < TM_TAB_COUNT; i++) {
        ui_rect r = tm_tab_rect(c, i);
        if (g_point_in(&r, x, y)) {
            g_tm_tab = i;
            return;
        }
    }
}

static void gui_tm_key(char key) {
    if (gui_app_active(WIN_TASKMGR)) {
        gui_app_key(WIN_TASKMGR, (int)(unsigned char)key);
        return;
    }

    if (key == '1') {
        g_tm_tab = TM_TAB_OVERVIEW;
    } else if (key == '2') {
        g_tm_tab = TM_TAB_PROCESSES;
    } else if (key == '3') {
        g_tm_tab = TM_TAB_DEVICES;
    } else if (key == '\t') {
        g_tm_tab = (g_tm_tab + 1) % TM_TAB_COUNT;
    }
}

/* ============================================================
 * 计算器
 * ------------------------------------------------------------
 * 全部使用 1/10000 为单位的 int64 定点数：
 *   - 不依赖浮点，不会因 -mno-sse 引入 libgcc 软浮点；
 *   - 乘除先做溢出判断，超出 ±1e12 进入错误态。
 * 除零（0/0 与 n/0）统一进入错误态，显示
 * "Cannot divide by zero"，任何数字/小数点按键先清除错误。
 * ============================================================ */

#define CALC_SCALE     10000ll
#define CALC_MAX_INT   1000000000000ll
#define CALC_INPUT_MAX 20

static char    g_calc_input[CALC_INPUT_MAX + 1] = "0";
static int64_t g_calc_acc = 0;
static int     g_calc_pending = 0;   /* 0 none, 1 +, 2 -, 3 *, 4 / */
static int     g_calc_fresh = 1;
static int     g_calc_error = 0;     /* 1 = divide by zero, 2 = overflow */

static int64_t calc_clamp(int64_t v) {
    int64_t limit = CALC_MAX_INT * CALC_SCALE;
    if (v > limit) return limit;
    if (v < -limit) return -limit;
    return v;
}

static int64_t calc_parse(const char *s) {
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }

    int64_t ip = 0;
    int idigits = 0;
    while (*s >= '0' && *s <= '9') {
        if (idigits < 13) {
            ip = ip * 10 + (int64_t)(*s - '0');
        }
        idigits++;
        s++;
    }

    int64_t frac = 0;
    int64_t div = 1;
    if (*s == '.') {
        s++;
        for (int i = 0; i < 4 && *s >= '0' && *s <= '9'; i++, s++) {
            frac = frac * 10 + (int64_t)(*s - '0');
            div *= 10;
        }
    }

    int64_t v = ip * CALC_SCALE + frac * (CALC_SCALE / div);
    return neg ? -v : v;
}

static void calc_format(int64_t v, char *out, int cap) {
    /* 只要还能放下至少一个字符就继续写。
     * 这里曾经用 cap < 24 直接返回：调用方传入的是 19 字节的输入缓冲，
     * 于是格式化永远不执行，计算结果根本写不回显示屏。 */
    if (cap < 2) return;
    v = calc_clamp(v);

    int neg = 0;
    if (v < 0) {
        neg = 1;
        v = -v;
    }

    int64_t ip = v / CALC_SCALE;
    int64_t fr = v % CALC_SCALE;

    char tmp[24];
    int n = 0;
    if (ip == 0) tmp[n++] = '0';
    while (ip > 0) {
        tmp[n++] = (char)('0' + (int)(ip % 10));
        ip /= 10;
    }

    int p = 0;
    if (neg && p < cap - 1) out[p++] = '-';
    while (n > 0 && p < cap - 1) out[p++] = tmp[--n];

    if (fr != 0) {
        char f[4];
        f[0] = (char)('0' + (int)((fr / 1000) % 10));
        f[1] = (char)('0' + (int)((fr / 100) % 10));
        f[2] = (char)('0' + (int)((fr / 10) % 10));
        f[3] = (char)('0' + (int)(fr % 10));
        int last = 3;
        while (last > 0 && f[last] == '0') last--;
        if (p < cap - 2) out[p++] = '.';
        for (int i = 0; i <= last && p < cap - 1; i++) out[p++] = f[i];
    }
    out[p] = '\0';
}

static int64_t calc_mul(int64_t a, int64_t b, int *overflow) {
    if (a == 0 || b == 0) return 0;

    int64_t aa = (a < 0) ? -a : a;
    int64_t bb = (b < 0) ? -b : b;
    int64_t limit = CALC_MAX_INT * CALC_SCALE;

    /* 先判断 |a|*|b| 是否会越过 int64 安全范围 */
    if (bb != 0 && aa > limit / bb) {
        *overflow = 1;
        return limit;
    }
    return calc_clamp((a * b) / CALC_SCALE);
}

static int64_t calc_div(int64_t a, int64_t b) {
    return calc_clamp((a * CALC_SCALE) / b);
}

static void calc_reset(void) {
    g_calc_input[0] = '0';
    g_calc_input[1] = '\0';
    g_calc_acc = 0;
    g_calc_pending = 0;
    g_calc_fresh = 1;
    g_calc_error = 0;
}

/* 错误态下按数字/小数点：清掉旧结果，从新输入开始 */
static void calc_clear_error(void) {
    if (!g_calc_error) return;
    g_calc_error = 0;
    g_calc_input[0] = '0';
    g_calc_input[1] = '\0';
    g_calc_acc = 0;
    g_calc_pending = 0;
    g_calc_fresh = 1;
}

static void calc_apply(void) {
    int64_t cur = calc_parse(g_calc_input);
    int overflow = 0;

    if (g_calc_pending == 0) {
        g_calc_acc = cur;
        return;
    }

    if (g_calc_pending == 1) {
        g_calc_acc = calc_clamp(g_calc_acc + cur);
    } else if (g_calc_pending == 2) {
        g_calc_acc = calc_clamp(g_calc_acc - cur);
    } else if (g_calc_pending == 3) {
        g_calc_acc = calc_mul(g_calc_acc, cur, &overflow);
    } else if (g_calc_pending == 4) {
        if (cur == 0) {
            /* 0/0 与 n/0 走同一条路径：不产生任何结果，直接错误态 */
            g_calc_error = 1;
            g_calc_pending = 0;
            g_calc_fresh = 1;
            return;
        }
        g_calc_acc = calc_div(g_calc_acc, cur);
    }

    g_calc_pending = 0;
    if (overflow) {
        g_calc_error = 2;
        g_calc_fresh = 1;
        return;
    }

    calc_format(g_calc_acc, g_calc_input, CALC_INPUT_MAX + 1);
    g_calc_fresh = 1;
}

static void calc_digit(char c) {
    calc_clear_error();

    if (g_calc_fresh) {
        g_calc_input[0] = '\0';
        g_calc_fresh = 0;
    }

    int len = g_strlen(g_calc_input);
    if (len >= 13) return;

    if (len == 1 && g_calc_input[0] == '0') {
        g_calc_input[0] = c;
        g_calc_input[1] = '\0';
        return;
    }
    if (len == 2 && g_calc_input[0] == '-' && g_calc_input[1] == '0') {
        g_calc_input[1] = c;
        g_calc_input[2] = '\0';
        return;
    }

    g_calc_input[len] = c;
    g_calc_input[len + 1] = '\0';
}

static void calc_dot(void) {
    calc_clear_error();

    if (g_calc_fresh) {
        g_calc_input[0] = '0';
        g_calc_input[1] = '\0';
        g_calc_fresh = 0;
    }

    for (int i = 0; g_calc_input[i]; i++) {
        if (g_calc_input[i] == '.') return;
    }

    int len = g_strlen(g_calc_input);
    if (len >= CALC_INPUT_MAX) return;
    g_calc_input[len] = '.';
    g_calc_input[len + 1] = '\0';
}

static void calc_negate(void) {
    if (g_calc_error) return;
    if (g_strcmp(g_calc_input, "0") == 0) return;

    if (g_calc_input[0] == '-') {
        for (int i = 0; g_calc_input[i]; i++) {
            g_calc_input[i] = g_calc_input[i + 1];
        }
    } else {
        int len = g_strlen(g_calc_input);
        if (len + 1 >= CALC_INPUT_MAX) return;
        for (int i = len; i >= 0; i--) {
            g_calc_input[i + 1] = g_calc_input[i];
        }
        g_calc_input[0] = '-';
    }
    g_calc_fresh = 0;
}

static void calc_percent(void) {
    if (g_calc_error) return;
    int64_t v = calc_parse(g_calc_input);
    calc_format(v / 100, g_calc_input, CALC_INPUT_MAX + 1);
    g_calc_fresh = 0;
}

static void calc_backspace(void) {
    if (g_calc_error) return;

    if (g_calc_fresh) {
        g_calc_input[0] = '0';
        g_calc_input[1] = '\0';
        return;
    }

    int len = g_strlen(g_calc_input);
    if (len <= 1 || (len == 2 && g_calc_input[0] == '-')) {
        g_calc_input[0] = '0';
        g_calc_input[1] = '\0';
        g_calc_fresh = 1;
        return;
    }
    g_calc_input[len - 1] = '\0';
}

static void calc_operator(int op) {
    if (g_calc_error) return;

    /* 连续按运算符只替换待定运算符，不重复结算 */
    if (!g_calc_fresh || g_calc_pending == 0) {
        calc_apply();
        if (g_calc_error) return;
    }
    g_calc_pending = op;
    g_calc_fresh = 1;
}

static void calc_equals(void) {
    if (g_calc_error) return;
    calc_apply();
}

static ui_rect calc_key_rect(ui_rect c, int row, int col, int span) {
    int pad = 8;
    int gap = 8;
    int top = 110;

    int cell_w = (c.w - pad * 2 - gap * 3) / 4;
    int cell_h = (c.h - top - pad - gap * 4) / 5;
    if (cell_w < 24) cell_w = 24;
    if (cell_h < 28) cell_h = 28;

    ui_rect r;
    r.x = c.x + pad + col * (cell_w + gap);
    r.y = c.y + top + row * (cell_h + gap);
    r.w = cell_w * span + gap * (span - 1);
    r.h = cell_h;
    return r;
}

static const char *g_calc_labels[5][4] = {
    { "C", "+/-", "%", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", "", ".", "=" }
};

static void gui_calc_render(const ui_theme *t, ui_rect c) {
    (void)t;

    /* 已加载 SLYZ 计算器时由虚拟机绘制内容区；
     * 镜像缺失才走下面的内置实现。 */
    if (gui_app_render(WIN_CALC, c)) return;

    ui_rect disp = { c.x, c.y, c.w, 96 };
    ui_round_panel(disp, 12, C_BORDER, 0x0C141E);

    /* 左上角显示待定运算：按下运算符后必须有可见反馈，
     * 否则用户会以为按键没反应（主显示区此时仍是当前输入）。 */
    if (!g_calc_error && g_calc_pending != 0) {
        char expr[32];
        int p = 0;
        calc_format(g_calc_acc, expr, (int)sizeof(expr));
        p = g_strlen(expr);
        if (p < (int)sizeof(expr) - 3) {
            expr[p++] = ' ';
            expr[p++] = (g_calc_pending == 1) ? '+' :
                        (g_calc_pending == 2) ? '-' :
                        (g_calc_pending == 3) ? '*' : '/';
            expr[p] = '\0';
        }
        gui_text_n(disp.x + 16, disp.y + 6, expr, (disp.w - 32) / 16, C_DIM);
    }

    if (g_calc_error) {
        const char *msg = (g_calc_error == 2) ? "Overflow" : "Cannot divide by zero";
        ui_text_transparent(disp.x + 16, disp.y + 32, msg, C_ERR);
    } else {
        int len = g_strlen(g_calc_input);
        int max_chars = (disp.w - 32) / 16;
        if (max_chars < 1) max_chars = 1;

        char shown[24];
        int start = 0;
        if (len > max_chars) start = len - max_chars;
        g_copy(shown, g_calc_input + start, (int)sizeof(shown));

        int w = g_strlen(shown) * 16;
        ui_text_transparent(disp.x + disp.w - 16 - w, disp.y + 32, shown, C_TEXT);
    }

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (row == 4 && col == 1) continue;   /* 0 键横跨两列 */

            int span = (row == 4 && col == 0) ? 2 : 1;
            const char *label = g_calc_labels[row][col];
            if (label[0] == '\0') continue;

            ui_rect r = calc_key_rect(c, row, col, span);
            int hover = g_point_in(&r, g_mx, g_my);

            uint32_t fill = C_PANEL2;
            uint32_t fg = C_TEXT;
            uint32_t border = hover ? C_ACCENT : C_BORDER;

            int is_pending = (label[1] == '\0') &&
                             ((g_calc_pending == 1 && label[0] == '+') ||
                              (g_calc_pending == 2 && label[0] == '-') ||
                              (g_calc_pending == 3 && label[0] == '*') ||
                              (g_calc_pending == 4 && label[0] == '/'));

            if (g_strcmp(label, "=") == 0) {
                fill = C_ACCENT;
                border = C_ACCENT3;
            } else if (g_strcmp(label, "C") == 0) {
                fill = 0x2A1719;
                border = hover ? C_ERR : 0x4A2A2E;
                fg = C_ERR;
            } else if (is_pending) {
                fill = 0x24405F;
                border = C_ACCENT3;
            } else if (g_strcmp(label, "+/-") == 0 || g_strcmp(label, "%") == 0 ||
                       g_strcmp(label, "/") == 0 || g_strcmp(label, "*") == 0 ||
                       g_strcmp(label, "-") == 0 || g_strcmp(label, "+") == 0) {
                fill = hover ? 0x24405F : C_PANEL3;
            } else if (hover) {
                fill = 0x1B2A3C;
            }

            ui_round_panel(r, 10, border, fill);
            gui_text_center(label, r.x + r.w / 2, r.y + (r.h - 32) / 2, fg);
        }
    }
}

static void gui_calc_click(ui_rect c, int x, int y) {
    if (gui_app_active(WIN_CALC)) {
        gui_app_click(WIN_CALC, x - c.x, y - c.y, 1);
        return;
    }

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (row == 4 && col == 1) continue;

            int span = (row == 4 && col == 0) ? 2 : 1;
            const char *label = g_calc_labels[row][col];
            if (label[0] == '\0') continue;

            ui_rect r = calc_key_rect(c, row, col, span);
            if (!g_point_in(&r, x, y)) continue;

            if (label[0] >= '0' && label[0] <= '9' && label[1] == '\0') {
                calc_digit(label[0]);
            } else if (label[0] == '.') {
                calc_dot();
            } else if (label[0] == '=') {
                calc_equals();
            } else if (label[0] == '%') {
                calc_percent();
            } else if (label[0] == 'C') {
                calc_reset();
            } else if (g_strcmp(label, "+/-") == 0) {
                calc_negate();
            } else if (label[0] == '/') {
                calc_operator(4);
            } else if (label[0] == '*') {
                calc_operator(3);
            } else if (label[0] == '-') {
                calc_operator(2);
            } else if (label[0] == '+') {
                calc_operator(1);
            }
            return;
        }
    }
}

static void gui_calc_key(char key) {
    if (gui_app_active(WIN_CALC)) {
        gui_app_key(WIN_CALC, (int)(unsigned char)key);
        return;
    }

    /* 错误态：C 直接复位；数字/小数点清错误后继续；其它键先清错误 */
    if (g_calc_error) {
        if (key == 'c' || key == 'C') {
            calc_reset();
            return;
        }
        if ((key >= '0' && key <= '9') || key == '.') {
            calc_clear_error();
        } else {
            return;
        }
    }

    if (key >= '0' && key <= '9') {
        calc_digit(key);
    } else if (key == '.') {
        calc_dot();
    } else if (key == '+') {
        calc_operator(1);
    } else if (key == '-') {
        calc_operator(2);
    } else if (key == '*') {
        calc_operator(3);
    } else if (key == '/') {
        calc_operator(4);
    } else if (key == '=' || key == '\r' || key == '\n') {
        calc_equals();
    } else if (key == 'c' || key == 'C') {
        calc_reset();
    } else if (key == '%') {
        calc_percent();
    } else if (key == 'n' || key == 'N') {
        calc_negate();
    } else if (key == '\b' || key == 127) {
        calc_backspace();
    }
}



/* ============================================================
 * 终端应用
 * ------------------------------------------------------------
 * 与 TTY 同源的体验：同一套 shell 命令、同一个提示符、同样的
 * 字符网格语义；区别只是渲染目标是窗口内容区而不是整屏 LFB。
 * 输出通过 tty_set_output_hook 截获，命令执行期间不触碰 LFB，
 * 因此不会把桌面画花，也不需要 TTY 的保留区/滚动区机制。
 * ============================================================ */

#define GUI_TERM_COLS  76
#define GUI_TERM_ROWS  220
/* 双宽字符的续格：由头格一并绘制，本身不出墨 */
#define TERM_CONT      0xFFFFu
#define GUI_TERM_HIST  16
#define GUI_TERM_INPUT 192

/* 字符属性：0 普通 1 提示符 2 错误 3 暗淡 4 成功 */
static uint16_t g_term_ch[GUI_TERM_ROWS][GUI_TERM_COLS];
static uint8_t g_term_attr[GUI_TERM_ROWS][GUI_TERM_COLS];
static int     g_term_row = 0;
static int     g_term_col = 0;
static int     g_term_scroll = 0;
static int     g_term_vis_rows = 0;
static int     g_term_cols = 54;   /* 可见列数，由渲染时按窗口宽度更新 */

static char g_term_in[GUI_TERM_INPUT];
static int  g_term_len = 0;
static int  g_term_cur = 0;
static int  g_term_in_row = 0;
static int  g_term_in_col = 0;

static char g_term_hist[GUI_TERM_HIST][GUI_TERM_INPUT];
static int  g_term_hist_count = 0;
static int  g_term_hist_pos = 0;

static int  g_term_ready = 0;

static void term_scroll_up(void) {
    for (int r = 1; r < GUI_TERM_ROWS; r++) {
        for (int c = 0; c < GUI_TERM_COLS; c++) {
            g_term_ch[r - 1][c] = g_term_ch[r][c];
            g_term_attr[r - 1][c] = g_term_attr[r][c];
        }
    }
    for (int c = 0; c < GUI_TERM_COLS; c++) {
        g_term_ch[GUI_TERM_ROWS - 1][c] = ' ';
        g_term_attr[GUI_TERM_ROWS - 1][c] = 0;
    }
    if (g_term_row > 0) g_term_row--;
}

static void term_clear(void) {
    for (int r = 0; r < GUI_TERM_ROWS; r++) {
        for (int c = 0; c < GUI_TERM_COLS; c++) {
            g_term_ch[r][c] = ' ';
            g_term_attr[r][c] = 0;
        }
    }
    g_term_row = 0;
    g_term_col = 0;
    g_term_scroll = 0;
    g_term_len = 0;
    g_term_cur = 0;
    g_term_in[0] = '\0';
}

static void term_newline(void) {
    g_term_col = 0;
    g_term_row++;
    if (g_term_row >= GUI_TERM_ROWS) {
        term_scroll_up();
        g_term_row = GUI_TERM_ROWS - 1;
    }
}

/* 终端输出是 UTF-8 字节流（printk hook），解码状态必须跨字节保持 */
static uint32_t g_term_u8_acc;
static uint32_t g_term_u8_min;
static int      g_term_u8_need;

/* 输出一个已解码码位：宽字符占两格，第二格标 TERM_CONT */
static void term_put_cp(uint32_t cp, int attr) {
    int w = utf32_width(cp);
    if (w == 0) return;

    if (g_term_col + w > g_term_cols) term_newline();

    if (g_term_row < GUI_TERM_ROWS && g_term_col < GUI_TERM_COLS) {
        g_term_ch[g_term_row][g_term_col] = (uint16_t)cp;
        g_term_attr[g_term_row][g_term_col] = (uint8_t)attr;
        if (w == 2 && g_term_col + 1 < GUI_TERM_COLS) {
            g_term_ch[g_term_row][g_term_col + 1] = TERM_CONT;
            g_term_attr[g_term_row][g_term_col + 1] = (uint8_t)attr;
        }
    }
    g_term_col += w;
}

static void term_put(char c, int attr) {
    uint8_t b = (uint8_t)c;

    if (g_term_u8_need > 0) {
        if ((b & 0xC0u) != 0x80u) {
            g_term_u8_need = 0;
            g_term_u8_acc = 0;
            term_put_cp(0xFFFD, attr);
        } else {
            g_term_u8_acc = (g_term_u8_acc << 6) | (uint32_t)(b & 0x3Fu);
            g_term_u8_need--;
            if (g_term_u8_need == 0) {
                uint32_t cp = g_term_u8_acc;
                g_term_u8_acc = 0;
                if (cp < g_term_u8_min || cp > 0x10FFFFu ||
                    (cp >= 0xD800u && cp <= 0xDFFFu)) {
                    cp = 0xFFFD;
                }
                term_put_cp(cp, attr);
                return;
            }
            return;
        }
    }

    if (b == '\n') {
        term_newline();
        return;
    }
    if (b == '\r') {
        g_term_col = 0;
        return;
    }
    if (b == '\t') {
        int next = (g_term_col + 8) & ~7;
        while (g_term_col < next && g_term_col < g_term_cols) {
            g_term_ch[g_term_row][g_term_col] = ' ';
            g_term_attr[g_term_row][g_term_col] = (uint8_t)attr;
            g_term_col++;
        }
        if (g_term_col >= g_term_cols) term_newline();
        return;
    }

    if (b < 0x80u) {
        if (b < 32u) return;
        term_put_cp(b, attr);
        return;
    }

    if ((b & 0xE0u) == 0xC0u) {
        g_term_u8_acc = (uint32_t)(b & 0x1Fu);
        g_term_u8_min = 0x80u;
        g_term_u8_need = 1;
        return;
    }
    if ((b & 0xF0u) == 0xE0u) {
        g_term_u8_acc = (uint32_t)(b & 0x0Fu);
        g_term_u8_min = 0x800u;
        g_term_u8_need = 2;
        return;
    }
    if ((b & 0xF8u) == 0xF0u) {
        g_term_u8_acc = (uint32_t)(b & 0x07u);
        g_term_u8_min = 0x10000u;
        g_term_u8_need = 3;
        return;
    }

    term_put_cp(0xFFFD, attr);
}

static void term_print(const char *s, int attr) {
    for (int i = 0; s[i]; i++) term_put(s[i], attr);
}

static void term_sink(const char *s, int len) {
    for (int i = 0; i < len; i++) term_put(s[i], 0);
}

static void term_prompt(void) {
    term_print(shell_prompt_text(), 1);
    term_put(' ', 1);

    /* 提示符本身可能比可见宽度还长（深目录），先换行再记输入起点 */
    if (g_term_col >= g_term_cols) {
        term_newline();
    }

    g_term_in_row = g_term_row;
    g_term_in_col = g_term_col;
}

/* 输入变化后只改这一行：清掉提示符之后的旧内容再写新内容。
 * 中文按双格写入，光标列按显示列累加。 */
static void term_rewrite_input(void) {
    if (g_term_in_row >= GUI_TERM_ROWS) return;

    for (int c = g_term_in_col; c < GUI_TERM_COLS; c++) {
        g_term_ch[g_term_in_row][c] = ' ';
        g_term_attr[g_term_in_row][c] = 0;
    }

    int col = 0;
    int cur_col = 0;
    int i = 0;
    while (g_term_in[i]) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)g_term_in + i, 4, &cp);
        if (used <= 0) break;
        int w = utf32_width(cp);
        if (w == 0) { i += used; continue; }

        int screen = g_term_in_col + col;
        if (screen + w > GUI_TERM_COLS) break;

        if (i <= g_term_cur) cur_col = col;

        g_term_ch[g_term_in_row][screen] = (uint16_t)cp;
        g_term_attr[g_term_in_row][screen] = 0;
        if (w == 2) {
            g_term_ch[g_term_in_row][screen + 1] = TERM_CONT;
            g_term_attr[g_term_in_row][screen + 1] = 0;
        }
        col += w;
        i += used;
    }
    if (g_term_cur >= g_term_len) cur_col = col;

    g_term_row = g_term_in_row;
    g_term_col = g_term_in_col + cur_col;
    if (g_term_col >= g_term_cols) g_term_col = g_term_cols - 1;
}

/* 插入一段 UTF-8 文本（输入法上屏） */
static void term_insert_utf8(const char *text) {
    int n = 0;
    while (text[n]) n++;
    if (n <= 0) return;
    if (g_term_len + n >= GUI_TERM_INPUT) return;

    for (int i = g_term_len; i >= g_term_cur; i--) {
        g_term_in[i + n] = g_term_in[i];
    }
    for (int i = 0; i < n; i++) {
        g_term_in[g_term_cur + i] = text[i];
    }
    g_term_len += n;
    g_term_cur += n;
    term_rewrite_input();
}

/* ============================================================
 * 终端内置编辑器（nano 等价物）
 * ------------------------------------------------------------
 * 和 TTY 的 editor_run 行为一致：全屏字符界面、Ctrl+S 保存、
 * Ctrl+X 退出、方向键移动、退格/Delete、Home/End。
 * 区别只是渲染目标是终端窗口的内容区，而不是整屏 LFB——
 * 因此在图形会话里不会破坏桌面，也不阻塞鼠标。
 * ============================================================ */

#define TE_MAX 65536

static char g_te_buf[TE_MAX];
static int  g_te_len = 0;
static int  g_te_cursor = 0;
static int  g_te_top = 0;
static int  g_te_hscroll = 0;
static int  g_te_dirty = 0;
static int  g_te_active = 0;
static int  g_te_confirm = 0;
static int  g_te_rows = 14;      /* 可见文本行数，渲染时按窗口高度更新 */
static char g_te_path[256];
static char g_te_msg[96];
static uint32_t g_te_msg_until = 0;

static void te_set_msg(const char *m, uint32_t ms) {
    g_copy(g_te_msg, m, (int)sizeof(g_te_msg));
    g_te_msg_until = net_now_ms() + ms;
}

static int te_line_start(int line) {
    if (line <= 0) return 0;

    int idx = 0;
    int cur = 0;
    while (idx < g_te_len && cur < line) {
        if (g_te_buf[idx] == '\n') cur++;
        idx++;
    }
    return idx;
}

static int te_line_count(void) {
    int n = 1;
    for (int i = 0; i < g_te_len; i++) {
        if (g_te_buf[i] == '\n') n++;
    }
    return n;
}

static int te_line_of_cursor(void) {
    int n = 0;
    for (int i = 0; i < g_te_cursor && i < g_te_len; i++) {
        if (g_te_buf[i] == '\n') n++;
    }
    return n;
}

static int te_col_of_cursor(void) {
    int start = te_line_start(te_line_of_cursor());
    return g_te_cursor - start;
}

static void te_insert_char(char ch) {
    if (g_te_len >= TE_MAX - 1) {
        te_set_msg("Buffer full", 2000);
        return;
    }
    for (int i = g_te_len; i > g_te_cursor; i--) {
        g_te_buf[i] = g_te_buf[i - 1];
    }
    g_te_buf[g_te_cursor] = ch;
    g_te_len++;
    g_te_cursor++;
    g_te_dirty = 1;
}

static void te_delete_at(int idx) {
    if (idx < 0 || idx >= g_te_len) return;
    for (int i = idx; i < g_te_len - 1; i++) {
        g_te_buf[i] = g_te_buf[i + 1];
    }
    g_te_len--;
    g_te_dirty = 1;
}

static void te_move_vertical(int delta) {
    int line = te_line_of_cursor();
    int col = te_col_of_cursor();
    int total = te_line_count();
    int target = line + delta;

    if (target < 0) target = 0;
    if (target >= total) target = total - 1;

    int start = te_line_start(target);
    int end = start;
    while (end < g_te_len && g_te_buf[end] != '\n') end++;

    int maxcol = end - start;
    if (col > maxcol) col = maxcol;

    g_te_cursor = start + col;
}

static int te_open(const char *path) {
    g_copy(g_te_path, path, (int)sizeof(g_te_path));
    g_te_len = 0;
    g_te_cursor = 0;
    g_te_top = 0;
    g_te_hscroll = 0;
    g_te_dirty = 0;
    g_te_confirm = 0;
    g_te_msg[0] = '\0';

    void *fh = slyzfs_open(path, "r");
    if (fh) {
        while (g_te_len < TE_MAX - 1) {
            int n = slyzfs_read(fh, (uint8_t *)g_te_buf + g_te_len,
                                TE_MAX - 1 - g_te_len);
            if (n <= 0) break;
            g_te_len += n;
        }
        slyzfs_close(fh);
        te_set_msg("Loaded", 1600);
    } else {
        te_set_msg("New file - Ctrl+S to save", 3000);
    }

    g_te_active = 1;
    return 1;
}

static int te_save(void) {
    if (!g_te_path[0]) {
        te_set_msg("No file name", 2500);
        return 0;
    }

    void *fh = slyzfs_open(g_te_path, "w");
    if (!fh) {
        te_set_msg("Cannot open for writing", 3000);
        return 0;
    }

    int total = 0;
    while (total < g_te_len) {
        int n = slyzfs_write(fh, (const uint8_t *)g_te_buf + total,
                             g_te_len - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);

    if (total != g_te_len) {
        te_set_msg("Write failed (disk full?)", 3000);
        return 0;
    }

    g_te_dirty = 0;
    te_set_msg("Saved", 2000);
    return 1;
}

static void te_close(void) {
    g_te_active = 0;
    g_te_confirm = 0;
    g_term_scroll = 0;
    term_prompt();
}

static void te_render(ui_rect c) {
    ui_round_fill(c, 10, 0x0A0F16);

    int cols = (c.w - 24) / 16;
    if (cols < 24) cols = 24;

    int text_cols = cols - 5;
    if (text_cols < 10) text_cols = 10;

    /* 标题行 */
    char head[160];
    int p = 0;
    for (int i = 0; g_te_path[i] && p < 100; i++) head[p++] = g_te_path[i];
    if (g_te_dirty) {
        g_copy(head + p, "   [modified]", 16);
        p += 13;
    }
    head[p] = '\0';
    gui_text_n(c.x + 12, c.y + 2, head, cols - 1,
               g_te_dirty ? C_WARN : C_TEXT);

    /* 文本区行数 */
    int rows = (c.h - 76) / 32;
    if (rows < 1) rows = 1;
    g_te_rows = rows;

    int cur_line = te_line_of_cursor();
    int cur_col = te_col_of_cursor();

    /* 垂直滚动跟随光标 */
    if (cur_line < g_te_top) g_te_top = cur_line;
    if (cur_line >= g_te_top + rows) g_te_top = cur_line - rows + 1;
    if (g_te_top < 0) g_te_top = 0;

    /* 水平滚动跟随光标 */
    if (cur_col < g_te_hscroll) g_te_hscroll = cur_col;
    if (cur_col >= g_te_hscroll + text_cols) {
        g_te_hscroll = cur_col - text_cols + 1;
    }
    if (g_te_hscroll < 0) g_te_hscroll = 0;

    int total_lines = te_line_count();

    for (int r = 0; r < rows; r++) {
        int line = g_te_top + r;
        if (line >= total_lines) break;

        int y = c.y + 40 + r * 32;

        /* 行号：4 位右对齐 */
        char num[8];
        int n = g_utoa((uint32_t)(line + 1), num);
        for (int k = 0; k < 4; k++) {
            char one[2];
            one[0] = (k < 4 - n) ? ' ' : num[k - (4 - n)];
            one[1] = '\0';
            if (one[0] != ' ') {
                ui_text_transparent(c.x + 12 + k * 16, y, one, 0x4A5A6E);
            }
        }

        int start = te_line_start(line);
        int end = start;
        while (end < g_te_len && g_te_buf[end] != '\n') end++;

        int base_x = c.x + 12 + 5 * 16;
        for (int k = 0; k < text_cols; k++) {
            int idx = start + g_te_hscroll + k;
            if (idx >= end) break;

            char one[2];
            one[0] = g_te_buf[idx];
            one[1] = '\0';
            if (one[0] == '\t') continue;
            ui_text_transparent(base_x + k * 16, y, one, C_TEXT);
        }
    }

    /* 光标 */
    int crow = cur_line - g_te_top;
    int ccol = cur_col - g_te_hscroll;
    if (crow >= 0 && crow < rows && ccol >= 0 && ccol < text_cols &&
        ((net_now_ms() / 500u) & 1u) == 0) {
        int cx = c.x + 12 + 5 * 16 + ccol * 16;
        int cy = c.y + 40 + crow * 32;
        vbe_fill_rect((uint16_t)cx, (uint16_t)cy, 2, 28, C_ACCENT3);
    }

    /* 底部状态行 */
    int sy = c.y + c.h - 34;
    ui_hline(c.x + 12, sy - 6, c.w - 24, 0x1E2A39);

    if (g_te_confirm) {
        gui_text_n(c.x + 12, sy,
                   "Unsaved changes. Ctrl+X again to discard, Esc to cancel.",
                   cols - 1, C_ERR);
    } else if (g_te_msg[0] && net_now_ms() <= g_te_msg_until) {
        gui_text_n(c.x + 12, sy, g_te_msg, cols - 1, C_OK);
    } else {
        char status[128];
        int q = 0;
        g_copy(status, "Ln ", 6); q = 3;
        q += g_utoa((uint32_t)(cur_line + 1), status + q);
        g_copy(status + q, " Col ", 8); q += 5;
        q += g_utoa((uint32_t)(cur_col + 1), status + q);
        g_copy(status + q, "   Ctrl+S save   Ctrl+X exit", 40);
        gui_text_n(c.x + 12, sy, status, cols - 1, C_DIM);
    }
}

static void te_key(char key) {
    if (g_te_confirm) {
        if (key == 'y' || key == 'Y' || key == 24) {
            te_close();
            return;
        }
        if (key == 'n' || key == 'N' || key == 27 || key == 3) {
            g_te_confirm = 0;
            te_set_msg("Cancelled", 1600);
        }
        return;
    }

    if (key == 24) {                     /* Ctrl+X */
        if (g_te_dirty) {
            g_te_confirm = 1;
        } else {
            te_close();
        }
        return;
    }
    if (key == 19) {                     /* Ctrl+S */
        te_save();
        return;
    }
    if (key == 3) {                      /* Ctrl+C */
        te_set_msg("^C (file unchanged on disk)", 1800);
        return;
    }

    if (key == (char)0xF1) {             /* Left */
        if (g_te_cursor > 0) g_te_cursor--;
        return;
    }
    if (key == (char)0xF2) {             /* Right */
        if (g_te_cursor < g_te_len) g_te_cursor++;
        return;
    }
    if (key == (char)0xF6) {             /* Up */
        te_move_vertical(-1);
        return;
    }
    if (key == (char)0xF7) {             /* Down */
        te_move_vertical(1);
        return;
    }
    if (key == (char)0xF3) {             /* Home */
        g_te_cursor = te_line_start(te_line_of_cursor());
        return;
    }
    if (key == (char)0xF4) {             /* End */
        int line = te_line_of_cursor();
        int start = te_line_start(line);
        int end = start;
        while (end < g_te_len && g_te_buf[end] != '\n') end++;
        g_te_cursor = end;
        return;
    }
    if (key == (char)0xF8) {             /* PgUp */
        int line = te_line_of_cursor();
        int target = line - g_te_rows;
        if (target < 0) target = 0;
        g_te_cursor = te_line_start(target);
        return;
    }
    if (key == (char)0xF9) {             /* PgDn */
        int line = te_line_of_cursor();
        int total = te_line_count();
        int target = line + g_te_rows;
        if (target >= total) target = total - 1;
        g_te_cursor = te_line_start(target);
        return;
    }
    if (key == (char)0xF5) {             /* Delete */
        te_delete_at(g_te_cursor);
        return;
    }
    if (key == (char)0xF0) {             /* Insert：不需要特殊处理 */
        return;
    }

    if (key == '\r' || key == '\n') {
        te_insert_char('\n');
        return;
    }
    if (key == '\b' || key == 127) {
        if (g_te_cursor > 0) {
            g_te_cursor--;
            te_delete_at(g_te_cursor);
        }
        return;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
        te_insert_char(key);
        return;
    }
}

/* GUI 终端里必须由图形层接管的命令：
 *   - reboot/poweroff 走桌面会话的统一退出路径（恢复 IRQ1、关鼠标上报）；
 *   - gui 会递归进入图形会话，必须拦；
 *   - clear/exit 直接操作终端自身。
 * 其余命令全部交给 shell；shell 的非交互模式已经处理了
 * nano（转 GUI 编辑器）、rm（要求 -f）、logout/passwd/ask（拒绝执行）。 */
static int term_blocked(const char *cmd) {
    return g_strcmp(cmd, "gui") == 0;
}

static void term_execute(void) {
    term_newline();

    if (g_term_len == 0) {
        term_prompt();
        return;
    }

    char line[GUI_TERM_INPUT];
    g_copy(line, g_term_in, (int)sizeof(line));

    if (g_term_hist_count < GUI_TERM_HIST) {
        g_copy(g_term_hist[g_term_hist_count], line, GUI_TERM_INPUT);
        g_term_hist_count++;
    } else {
        for (int i = 1; i < GUI_TERM_HIST; i++) {
            g_copy(g_term_hist[i - 1], g_term_hist[i], GUI_TERM_INPUT);
        }
        g_copy(g_term_hist[GUI_TERM_HIST - 1], line, GUI_TERM_INPUT);
    }
    g_term_hist_pos = g_term_hist_count;

    g_term_len = 0;
    g_term_cur = 0;
    g_term_in[0] = '\0';

    char word[32];
    int wi = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && wi < (int)sizeof(word) - 1) {
        word[wi++] = *p++;
    }
    word[wi] = '\0';

    if (g_strcmp(word, "clear") == 0 || g_strcmp(word, "cls") == 0) {
        term_clear();
        term_prompt();
        return;
    }
    if (g_strcmp(word, "exit") == 0 || g_strcmp(word, "quit") == 0) {
        win_close(WIN_TERM);
        return;
    }
    if (g_strcmp(word, "reboot") == 0) {
        term_print("Rebooting...", 3);
        term_newline();
        g_pending_action = ACT_REBOOT;
        return;
    }
    if (g_strcmp(word, "poweroff") == 0 || g_strcmp(word, "shutdown") == 0 ||
        g_strcmp(word, "halt") == 0) {
        term_print("Shutting down...", 3);
        term_newline();
        g_pending_action = ACT_SHUTDOWN;
        return;
    }
    if (g_strcmp(word, "nano") == 0 || g_strcmp(word, "edit") == 0) {
        /* 与 TTY 完全一致的字符编辑器：解析相对路径后进入终端内编辑界面，
         * 不会弹出图形编辑器窗口。 */
        while (*p == ' ' || *p == '\t') p++;

        if (!*p) {
            term_print("usage: nano <file>", 2);
            term_newline();
            term_prompt();
            return;
        }

        char fname[256];
        int fn = 0;
        while (*p && fn < (int)sizeof(fname) - 1) fname[fn++] = *p++;
        while (fn > 0 && (fname[fn - 1] == ' ' || fname[fn - 1] == '\t')) fn--;
        fname[fn] = '\0';

        char path[256];
        if (!shell_resolve_input(fname, path, (int)sizeof(path))) {
            term_print("nano: invalid file name", 2);
            term_newline();
            term_prompt();
            return;
        }

        te_open(path);
        return;
    }

    /* GUI 终端里的 run：SLYZ 程序必须启动独立窗口，
     * 不能落到 TTY 宿主（那会误报"终端环境没有窗口"）。 */
    if (g_strcmp(word, "run") == 0) {
        while (*p == ' ' || *p == '\t') p++;

        if (*p) {
            char fname[256];
            int fn = 0;
            while (*p && fn < (int)sizeof(fname) - 1) fname[fn++] = *p++;
            while (fn > 0 && (fname[fn - 1] == ' ' || fname[fn - 1] == '\t')) fn--;
            fname[fn] = '\0';

            char path[256];
            if (shell_resolve_input(fname, path, (int)sizeof(path)) &&
                gui_slyz_is_program(path)) {
                int win = gui_app_open_path(path);
                if (win >= 0) {
                    term_print("Launched in its own window.", 4);
                    term_newline();
                    win_open(win);
                    g_focus = win;
                } else {
                    term_print("run: cannot start this SLYZ program", 7);
                    term_newline();
                }
                term_prompt();
                return;
            }
        }
    }

    if (term_blocked(word)) {
        term_print("Already running inside the graphical session.", 3);
        term_newline();
        term_print("Use the taskbar or the search panel to open apps.", 3);
        term_newline();
        term_prompt();
        return;
    }

    tty_set_output_hook(term_sink);
    shell_set_noninteractive(1);
    shell_exec_line(line);
    shell_set_noninteractive(0);
    tty_set_output_hook(0);

    g_term_scroll = 0;
    term_prompt();
}

static void gui_term_reset(void) {
    if (g_term_ready) return;

    term_clear();
    term_print("SaylterOS v0.3 Beta1 - GUI terminal", 4);
    term_newline();
    term_print("Same shell as the TTY session. Type help for commands.", 3);
    term_newline();
    term_print("nano opens the GUI editor. rm needs -f here.", 3);
    term_newline();
    term_print("reboot / poweroff / exit work in this window.", 3);
    term_newline();
    term_newline();

    g_term_ready = 1;
    term_prompt();
}

static uint32_t term_attr_color(uint8_t attr) {
    switch (attr) {
        case 1: return C_ACCENT3;
        case 2: return C_ERR;
        case 3: return C_DIM;
        case 4: return C_OK;
        default: return C_TEXT;
    }
}

static void gui_term_render(const ui_theme *t, ui_rect c) {
    (void)t;

    if (g_te_active) {
        te_render(c);
        return;
    }

    ui_round_fill(c, 10, 0x0A0F16);

    int cols = (c.w - 24) / 16;
    if (cols > GUI_TERM_COLS) cols = GUI_TERM_COLS;
    if (cols <= 0) return;
    g_term_cols = cols;

    int rows = (c.h - 16) / 32;
    if (rows <= 0) return;
    if (rows > GUI_TERM_ROWS) rows = GUI_TERM_ROWS;
    g_term_vis_rows = rows;

    int total = g_term_row + 1;
    int maxs = total - rows;
    if (maxs < 0) maxs = 0;
    if (g_term_scroll > maxs) g_term_scroll = maxs;
    if (g_term_scroll < 0) g_term_scroll = 0;

    int bottom = total - g_term_scroll;
    if (bottom < 1) bottom = 1;
    int top = bottom - rows;
    if (top < 0) top = 0;

    for (int r = 0; r < rows; r++) {
        int src = top + r;
        if (src >= GUI_TERM_ROWS) break;

        int y = c.y + 8 + r * 32;
        for (int col = 0; col < cols; col++) {
            uint16_t cp = g_term_ch[src][col];
            if (cp == ' ' || cp == 0 || cp == TERM_CONT) continue;

            int x = c.x + 12 + col * 16;
            uint32_t color = term_attr_color(g_term_attr[src][col]);
            if (utf32_width(cp) == 2) {
                vbe_draw_cjk_transparent((uint16_t)x, (uint16_t)y, cp, color);
                col++;   /* 续格不单独绘制 */
            } else {
                char one[2];
                one[0] = (char)cp;
                one[1] = '\0';
                ui_text_transparent(x, y, one, color);
            }
        }
    }

    if (g_term_scroll == 0 && g_focus == WIN_TERM) {
        int crow = g_term_in_row - top;
        int cx = c.x + 12 + g_term_col * 16;
        int cy = c.y + 8 + crow * 32;

        /* 候选窗锚点：输入光标正下方 */
        g_ime_ax = cx;
        g_ime_ay = cy + 40;

        if (crow >= 0 && crow < rows && ((net_now_ms() / 500u) & 1u) == 0) {
            vbe_fill_rect((uint16_t)cx, (uint16_t)cy, 2, 26, C_ACCENT3);
        }
    }

    /* 右侧滚动条：轨道 + 滑块。只有内容超过一屏才显示。
     * scroll=0 时滑块在底部（最新输出），scroll 增大时向上移动。 */
    if (total > rows && rows > 0) {
        int track_x = c.x + c.w - 10;
        int track_y = c.y + 8;
        int track_h = rows * 32;
        if (track_h > c.h - 16) track_h = c.h - 16;

        ui_round_fill((ui_rect){ track_x, track_y, 6, track_h }, 3, 0x141C27);

        int thumb_h = (int)(((long)track_h * (long)rows) / (long)total);
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > track_h) thumb_h = track_h;

        int denom = total - rows;
        int pos = (denom > 0) ?
            (int)(((long)g_term_scroll * (long)(track_h - thumb_h)) / (long)denom) : 0;
        int thumb_y = track_y + (track_h - thumb_h) - pos;

        ui_round_fill((ui_rect){ track_x, thumb_y, 6, thumb_h }, 3, C_ACCENT2);
    }

    if (g_term_scroll > 0) {
        gui_text_n(c.x + c.w - 360, c.y + 4,
                   "wheel / PgUp / PgDn scroll   Esc bottom", 22, C_DIM);
    }
}

static void gui_term_click(ui_rect c, int x, int y) {
    (void)c;
    (void)x;
    (void)y;
}

/* 统一滚动入口：正数向历史（上），负数向最新（下）。
 * 上限取决于当前可见行数，不足一屏时不允许滚动。 */
static void gui_term_scroll_by(int delta) {
    if (delta == 0) return;

    int total = g_term_row + 1;
    int maxs = total - g_term_vis_rows;
    if (maxs < 0) maxs = 0;

    g_term_scroll += delta;
    if (g_term_scroll > maxs) g_term_scroll = maxs;
    if (g_term_scroll < 0) g_term_scroll = 0;
}

static void gui_term_key(char key) {
    if (g_te_active) {
        te_key(key);
        return;
    }

    if (key == (char)0xF8) {                    /* PgUp */
        gui_term_scroll_by(8);
        return;
    }
    if (key == (char)0xF9) {                    /* PgDn */
        gui_term_scroll_by(-8);
        return;
    }
    if (key == 27) {                            /* Esc：回到最新输出 */
        g_term_scroll = 0;
        return;
    }
    if (key == 12) {                            /* Ctrl+L */
        term_clear();
        term_prompt();
        return;
    }
    if (key == 3) {                             /* Ctrl+C */
        g_term_len = 0;
        g_term_cur = 0;
        g_term_in[0] = '\0';
        term_print("^C", 3);
        term_newline();
        term_prompt();
        return;
    }
    if (key == (char)0xF1) {                    /* Left */
        if (g_term_cur > 0) {
            g_term_cur = utf8_prev_char(g_term_in, g_term_cur);
            term_rewrite_input();
        }
        return;
    }
    if (key == (char)0xF2) {                    /* Right */
        if (g_term_cur < g_term_len) {
            g_term_cur = utf8_next_char(g_term_in, g_term_cur);
            term_rewrite_input();
        }
        return;
    }
    if (key == (char)0xF3) {                    /* Home */
        g_term_cur = 0;
        term_rewrite_input();
        return;
    }
    if (key == (char)0xF4) {                    /* End */
        g_term_cur = g_term_len;
        term_rewrite_input();
        return;
    }
    if (key == (char)0xF5) {                    /* Delete */
        if (g_term_cur < g_term_len) {
            int next = utf8_next_char(g_term_in, g_term_cur);
            int del = next - g_term_cur;
            for (int i = g_term_cur; i + del <= g_term_len; i++) {
                g_term_in[i] = g_term_in[i + del];
            }
            g_term_len -= del;
            g_term_in[g_term_len] = '\0';
            term_rewrite_input();
        }
        return;
    }
    if (key == (char)0xF6) {                    /* Up：上一条历史 */
        if (g_term_hist_count > 0 && g_term_hist_pos > 0) {
            g_term_hist_pos--;
            g_copy(g_term_in, g_term_hist[g_term_hist_pos], GUI_TERM_INPUT);
            g_term_len = g_strlen(g_term_in);
            g_term_cur = g_term_len;
            term_rewrite_input();
        }
        return;
    }
    if (key == (char)0xF7) {                    /* Down */
        if (g_term_hist_count > 0) {
            if (g_term_hist_pos < g_term_hist_count - 1) {
                g_term_hist_pos++;
                g_copy(g_term_in, g_term_hist[g_term_hist_pos], GUI_TERM_INPUT);
            } else {
                g_term_hist_pos = g_term_hist_count;
                g_term_in[0] = '\0';
            }
            g_term_len = g_strlen(g_term_in);
            g_term_cur = g_term_len;
            term_rewrite_input();
        }
        return;
    }
    if (key == '\r' || key == '\n') {
        term_execute();
        return;
    }
    if (key == '\b' || key == 127) {
        if (g_term_cur > 0) {
            int prev = utf8_prev_char(g_term_in, g_term_cur);
            int del = g_term_cur - prev;
            for (int i = prev; i + del <= g_term_len; i++) {
                g_term_in[i] = g_term_in[i + del];
            }
            g_term_len -= del;
            g_term_cur = prev;
            g_term_in[g_term_len] = '\0';
            term_rewrite_input();
        }
        return;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
        if (g_term_len < GUI_TERM_INPUT - 1 &&
            g_term_in_col + utf8_width(g_term_in) + 1 < g_term_cols) {
            for (int i = g_term_len; i > g_term_cur; i--) {
                g_term_in[i] = g_term_in[i - 1];
            }
            g_term_in[g_term_cur] = key;
            g_term_len++;
            g_term_cur++;
            g_term_in[g_term_len] = '\0';
            term_rewrite_input();
        }
        return;
    }
}

/* ============================================================
 * 浏览器应用
 * ------------------------------------------------------------
 * 约束（与网络栈一致）：只支持 http://，没有 TLS；只渲染 HTML+CSS，
 * 不执行 JavaScript；页面用浅色画布渲染，白底深字最接近真实网页。
 * 加载是同步的：点击 Go 之后 GUI 会等待 TCP 收完，期间画面不动。
 * ============================================================ */

#define GUI_BR_MAX   131072
#define GUI_BR_URL   192
#define GUI_BR_HIST  8

static char g_br_src[GUI_BR_MAX];
static int  g_br_len = 0;

static char g_br_url[GUI_BR_URL];
static char g_br_input[GUI_BR_URL];
static int  g_br_input_len = 0;
static int  g_br_addr_focus = 1;

static int  g_br_scroll = 0;
static int  g_br_content_h = 0;
static int  g_br_view_h = 0;
static int  g_br_status = 0;        /* 0 空, 1 完成, 2 错误 */
static int  g_br_code = 0;
static char g_br_msg[96];

static char g_br_hist[GUI_BR_HIST][GUI_BR_URL];
static int  g_br_hist_count = 0;
static int  g_br_hist_pos = 0;

static int  g_br_ready = 0;

static ui_rect br_back_rect(ui_rect c) {
    return (ui_rect){ c.x, c.y, 96, 44 };
}

static ui_rect br_addr_rect(ui_rect c) {
    return (ui_rect){ c.x + 104, c.y, c.w - 104 - 100, 44 };
}

static ui_rect br_go_rect(ui_rect c) {
    return (ui_rect){ c.x + c.w - 92, c.y, 92, 44 };
}

static ui_rect br_view_rect(ui_rect c) {
    return (ui_rect){ c.x, c.y + 52, c.w, c.h - 52 - 36 };
}

static void br_set_msg(const char *m) {
    g_copy(g_br_msg, m, (int)sizeof(g_br_msg));
}

static void br_hist_add(const char *url) {
    if (g_br_hist_count < GUI_BR_HIST) {
        g_copy(g_br_hist[g_br_hist_count], url, GUI_BR_URL);
        g_br_hist_count++;
    } else {
        for (int i = 1; i < GUI_BR_HIST; i++) {
            g_copy(g_br_hist[i - 1], g_br_hist[i], GUI_BR_URL);
        }
        g_copy(g_br_hist[GUI_BR_HIST - 1], url, GUI_BR_URL);
    }
    g_br_hist_pos = g_br_hist_count;
}

/* 补全协议前缀：用户只输 "example.com" 也当成 http://example.com */
static void br_normalize(const char *in, char *out, int cap) {
    if (!in || !in[0]) {
        out[0] = '\0';
        return;
    }

    if (in[0] == 'h' && in[1] == 't' && in[2] == 't' && in[3] == 'p' &&
        in[4] == ':' && in[5] == '/' && in[6] == '/') {
        g_copy(out, in, cap);
        return;
    }

    int p = 0;
    const char *prefix = "http://";
    for (int i = 0; prefix[i] && p < cap - 1; i++) out[p++] = prefix[i];
    for (int i = 0; in[i] && p < cap - 1; i++) out[p++] = in[i];
    out[p] = '\0';
}

static void br_load(const char *url, int add_history) {
    char full[GUI_BR_URL];
    br_normalize(url, full, (int)sizeof(full));
    if (!full[0]) return;

    g_copy(g_br_url, full, GUI_BR_URL);
    g_copy(g_br_input, full, GUI_BR_URL);
    g_br_input_len = g_strlen(g_br_input);
    g_br_addr_focus = 0;
    g_br_scroll = 0;
    g_br_content_h = 0;

    if (add_history) br_hist_add(full);

    br_set_msg("Loading...");
    g_br_status = 0;

    char ctype[64];
    int code = 0;
    int n = net_http_get(full, g_br_src, GUI_BR_MAX, &code, ctype,
                         (int)sizeof(ctype));
    g_br_code = code;

    if (n <= 0) {
        g_br_len = 0;
        g_br_status = 2;
        br_set_msg(n == -2 ? "Incomplete HTTP response"
                           : "Cannot load page (network or DNS)");
        return;
    }

    g_br_src[n] = '\0';
    g_br_len = n;
    g_br_status = 1;

    if (code >= 300 && code < 400) {
        br_set_msg("Redirect (not followed)");
    } else if (code >= 400) {
        br_set_msg("HTTP error");
    } else {
        br_set_msg("Done");
    }
}

static void gui_browser_reset(void) {
    if (g_br_ready) return;

    g_copy(g_br_input, "http://example.com/", GUI_BR_URL);
    g_br_input_len = g_strlen(g_br_input);
    g_br_addr_focus = 1;
    g_br_status = 0;
    g_br_code = 0;
    br_set_msg("Enter an http:// address and press Enter");

    g_br_ready = 1;
}

static void gui_browser_render(const ui_theme *t, ui_rect c) {
    (void)t;

    ui_round_fill(c, 10, 0x0B1017);

    ui_rect back = br_back_rect(c);
    ui_rect addr = br_addr_rect(c);
    ui_rect go = br_go_rect(c);
    ui_rect view = br_view_rect(c);

    g_br_view_h = view.h;

    int back_hover = g_point_in(&back, g_mx, g_my) && g_br_hist_pos > 1;
    gui_button(back, "Back", back_hover, 0);

    ui_round_panel(addr, 10, g_br_addr_focus ? C_ACCENT : C_BORDER, 0x0C141E);
    gui_text_n(addr.x + 14, addr.y + 6, g_br_input,
               (addr.w - 28) / 16, C_TEXT);
    if (g_br_addr_focus && ((net_now_ms() / 500u) & 1u) == 0) {
        int cx = addr.x + 14 + g_br_input_len * 16;
        if (cx < addr.x + addr.w - 8) {
            vbe_fill_rect((uint16_t)cx, (uint16_t)(addr.y + 10), 2, 24, C_ACCENT);
        }
    }

    gui_button(go, "Go", g_point_in(&go, g_mx, g_my), 1);

    ui_round_fill(view, 6, 0xF7F9FC);

    if (g_br_status == 1 && g_br_len > 0) {
        vbe_push_clip(view.x, view.y, view.w, view.h);
        html_render(g_br_src, g_br_len, view.x + 18, view.y + 14,
                    view.w - 36, g_br_scroll, &g_br_content_h);
        vbe_pop_clip();

        /* 滚动条：内容高于视口时画一个比例指示 */
        if (g_br_content_h > view.h && g_br_view_h > 0) {
            int track_h = view.h - 8;
            int thumb_h = track_h * view.h / g_br_content_h;
            if (thumb_h < 24) thumb_h = 24;
            int max_scroll = g_br_content_h - view.h;
            int thumb_y = view.y + 4;
            if (max_scroll > 0) {
                thumb_y += (track_h - thumb_h) * g_br_scroll / max_scroll;
            }
            ui_round_fill((ui_rect){ view.x + view.w - 10, view.y + 4, 6, track_h },
                          3, 0xD3DAE4);
            ui_round_fill((ui_rect){ view.x + view.w - 10, thumb_y, 6, thumb_h },
                          3, 0x8FA0B5);
        }
    } else if (g_br_status == 2) {
        gui_text_wrap(view.x + 24, view.y + 24, view.w - 48, g_br_msg, C_ERR, 2);
        gui_text_wrap(view.x + 24, view.y + 96, view.w - 48,
                      "Only http:// is supported. https needs TLS, "
                      "which is not implemented yet.", 0x51627A, 3);
    } else {
        gui_text_wrap(view.x + 24, view.y + 24, view.w - 48,
                      "Enter an http:// address in the bar above, then press Enter or click Go.",
                      0x51627A, 3);
        gui_text_wrap(view.x + 24, view.y + 110, view.w - 48,
                      "HTML and CSS are rendered; JavaScript is not executed.",
                      0x8FA0B5, 2);
    }

    /* 状态栏 */
    ui_rect st = { c.x, c.y + c.h - 32, c.w, 32 };
    ui_hline(st.x, st.y, st.w, C_BORDER);

    char line[160];
    int p = 0;
    if (g_br_code > 0) {
        p += g_utoa((uint32_t)g_br_code, line);
        line[p++] = ' ';
        line[p++] = ' ';
    }
    for (int i = 0; g_br_msg[i] && p < (int)sizeof(line) - 1; i++) {
        line[p++] = g_br_msg[i];
    }
    line[p] = '\0';

    gui_text_n(st.x, st.y + 1, line, st.w / 16 - 12, C_DIM);

    if (g_br_status == 1 && g_br_content_h > 0) {
        char pos[48];
        char num[12];
        int q = 0;
        q += g_utoa((uint32_t)g_br_scroll, num);
        for (int i = 0; num[i]; i++) pos[q++] = num[i];
        pos[q++] = '/';
        q += g_utoa((uint32_t)g_br_content_h, num);
        for (int i = 0; num[i]; i++) pos[q++] = num[i];
        pos[q] = '\0';
        ui_text_transparent(st.x + st.w - 120, st.y + 1, pos, C_DIM);
    }
}

static void gui_browser_click(ui_rect c, int x, int y) {
    ui_rect back = br_back_rect(c);
    ui_rect addr = br_addr_rect(c);
    ui_rect go = br_go_rect(c);
    ui_rect view = br_view_rect(c);

    if (g_point_in(&back, x, y)) {
        if (g_br_hist_pos > 1) {
            g_br_hist_pos--;
            br_load(g_br_hist[g_br_hist_pos - 1], 0);
        }
        return;
    }
    if (g_point_in(&go, x, y)) {
        br_load(g_br_input, 1);
        return;
    }
    if (g_point_in(&addr, x, y)) {
        g_br_addr_focus = 1;
        return;
    }

    g_br_addr_focus = 0;

    if (g_point_in(&view, x, y)) {
        char href[128];
        if (html_link_at(x, y, href, (int)sizeof(href))) {
            br_load(href, 1);
        }
    }
}

static void gui_browser_key(char key) {
    if (key == (char)0xF8) {                 /* PgUp */
        int step = (g_br_view_h > 96) ? g_br_view_h - 64 : 64;
        g_br_scroll -= step;
        if (g_br_scroll < 0) g_br_scroll = 0;
        return;
    }
    if (key == (char)0xF9) {                 /* PgDn */
        int step = (g_br_view_h > 96) ? g_br_view_h - 64 : 64;
        int maxs = g_br_content_h - g_br_view_h;
        if (maxs < 0) maxs = 0;
        g_br_scroll += step;
        if (g_br_scroll > maxs) g_br_scroll = maxs;
        return;
    }
    if (key == '\r' || key == '\n') {
        br_load(g_br_input, 1);
        return;
    }
    if (key == '\b' || key == 127) {
        if (g_br_input_len > 0) {
            g_br_input_len--;
            g_br_input[g_br_input_len] = '\0';
        }
        g_br_addr_focus = 1;
        return;
    }
    if (key == 27) {
        g_br_addr_focus = 0;
        return;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
        if (g_br_input_len < GUI_BR_URL - 1) {
            g_br_input[g_br_input_len++] = key;
            g_br_input[g_br_input_len] = '\0';
            g_br_addr_focus = 1;
        }
    }
}

/* ============================================================
 * 桌面图标、右键菜单与新建文件/文件夹
 * ------------------------------------------------------------
 * 桌面显示当前用户 home 目录的内容：
 *   - 双击文件夹：打开文件管理器并进入该目录
 *   - 双击文件：在文本编辑器里打开
 *   - 空白处右键：新建文件 / 新建文件夹 / 刷新 / 在文件管理器中打开
 * 新建走和文件管理器一致的模态输入卡片，写入后立即刷新网格。
 * ============================================================ */

#define GUI_DESK_MAX      24
#define GUI_DESK_CELL_W   120
#define GUI_DESK_CELL_H   104
#define GUI_DESK_ICON     40

struct GuiDeskEntry {
    char     name[52];
    uint8_t  type;
    ui_rect  r;
};

static struct GuiDeskEntry g_desk[GUI_DESK_MAX];
static int      g_desk_count = 0;
static char     g_desk_path[256];
static int      g_desk_menu = 0;
static int      g_desk_menu_x = GUI_TASKBAR_W + 24;
static int      g_desk_menu_y = 24;
static int      g_desk_prompt = 0;      /* 1 = 新建文件, 2 = 新建文件夹 */
static char     g_desk_input[64];
static int      g_desk_input_len = 0;
static uint32_t g_desk_click_ms = 0;
static int      g_desk_click_idx = -1;

static int desk_is_dir(const struct GuiDeskEntry *e) {
    return e->type == GUI_DIR_DIR;
}

static int desk_less(const struct GuiDeskEntry *a, const struct GuiDeskEntry *b) {
    int ad = desk_is_dir(a);
    int bd = desk_is_dir(b);
    if (ad != bd) return ad > bd;
    return g_strcasecmp(a->name, b->name) < 0;
}

static void desk_layout(void) {
    int x0 = GUI_TASKBAR_W + 24;
    int y0 = 24;
    int avail = gW - GUI_TASKBAR_W - 48;
    int cols = avail / GUI_DESK_CELL_W;
    if (cols < 1) cols = 1;

    for (int i = 0; i < g_desk_count; i++) {
        int c = i % cols;
        int r = i / cols;
        g_desk[i].r.x = x0 + c * GUI_DESK_CELL_W;
        g_desk[i].r.y = y0 + r * GUI_DESK_CELL_H;
        g_desk[i].r.w = GUI_DESK_CELL_W - 8;
        g_desk[i].r.h = GUI_DESK_CELL_H - 8;
    }
}

static void gui_desk_refresh(void) {
    const struct AuthUser *u = auth_current();
    const char *home = (u && u->home[0]) ? u->home : "home\\root";
    g_copy(g_desk_path, home, (int)sizeof(g_desk_path));

    struct GuiDirEntry tmp[GUI_DESK_MAX];
    int n = slyzfs_list_dir(g_desk_path, tmp, GUI_DESK_MAX);
    if (n < 0) n = 0;
    if (n > GUI_DESK_MAX) n = GUI_DESK_MAX;

    g_desk_count = n;
    for (int i = 0; i < n; i++) {
        g_copy(g_desk[i].name, (const char *)tmp[i].name, (int)sizeof(g_desk[i].name));
        g_desk[i].type = tmp[i].type;
    }

    for (int i = 1; i < g_desk_count; i++) {
        struct GuiDeskEntry key = g_desk[i];
        int j = i - 1;
        while (j >= 0 && desk_less(&key, &g_desk[j])) {
            g_desk[j + 1] = g_desk[j];
            j--;
        }
        g_desk[j + 1] = key;
    }

    desk_layout();
    g_desk_click_idx = -1;
}

static void gui_desk_reset(void) {
    g_desk_menu = 0;
    g_desk_prompt = 0;
    g_desk_input[0] = '\0';
    g_desk_input_len = 0;
    gui_desk_refresh();
}

static ui_rect desk_menu_rect(void) {
    return (ui_rect){ g_desk_menu_x, g_desk_menu_y, 250, 168 };
}

static void gui_desk_menu_render(void) {
    ui_rect p = desk_menu_rect();
    int items = 4;
    const char *labels[4] = { "New File", "New Folder", "Refresh", "Open Home in Files" };

    /* 防止菜单超出屏幕右下角 */
    if (p.x + p.w > gW - 8) p.x = gW - 8 - p.w;
    if (p.y + p.h > gH - 8) p.y = gH - 8 - p.h;

    ui_shadow(p, 12, 8);
    ui_round_panel(p, 12, C_BORDER, 0x121A25);

    for (int i = 0; i < items; i++) {
        ui_rect row = { p.x + 8, p.y + 8 + i * 38, p.w - 16, 34 };
        int hover = g_point_in(&row, g_mx, g_my);
        hunt_add(row);
        if (hover) ui_round_fill(row, 8, 0x1B2836);
        ui_text_transparent(row.x + 14, row.y + 1, LTR(labels[i]), C_TEXT);
    }

    g_desk_menu_x = p.x;
    g_desk_menu_y = p.y;
}

static ui_rect desk_prompt_card(void) {
    ui_rect c;
    c.w = 480;
    c.h = 210;
    c.x = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W - c.w) / 2;
    c.y = (gH - c.h) / 2;
    return c;
}

static void gui_desk_prompt_render(void) {
    ui_rect c = desk_prompt_card();
    ui_shadow(c, 16, 10);
    ui_round_panel(c, 16, C_BORDER, 0x121A25);

    ui_text_transparent(c.x + 24, c.y + 16,
                        g_desk_prompt == 1 ? "NEW FILE" : "NEW FOLDER", C_DIM);

    ui_rect box = { c.x + 24, c.y + 60, c.w - 48, 44 };
    ui_round_panel(box, 10, C_ACCENT, 0x0C141E);
    gui_text_n(box.x + 14, box.y + 7, g_desk_input, (box.w - 28) / 16, C_TEXT);
    if (((net_now_ms() / 500u) & 1u) == 0) {
        int cx = box.x + 14 + g_desk_input_len * 16;
        if (cx < box.x + box.w - 8) {
            vbe_fill_rect((uint16_t)cx, (uint16_t)(box.y + 10), 2, 24, C_ACCENT);
        }
    }

    ui_rect ok = { c.x + 24, c.y + 128, 180, 46 };
    ui_rect cancel = { c.x + 216, c.y + 128, 180, 46 };
    gui_button(ok, "Create", g_point_in(&ok, g_mx, g_my), 1);
    gui_button(cancel, "Cancel", g_point_in(&cancel, g_mx, g_my), 0);
}

static void desk_prompt_commit(void) {
    int kind = g_desk_prompt;
    g_desk_prompt = 0;

    if (g_desk_input_len == 0) return;

    char full[512];
    fm_join(full, (int)sizeof(full), g_desk_path, g_desk_input);

    if (kind == 1) {
        (void)slyzfs_create(full);
    } else {
        (void)slyzfs_mkdir(full);
    }

    gui_desk_refresh();
}

static void gui_desk_click(int x, int y) {
    if (g_desk_prompt) {
        ui_rect c = desk_prompt_card();
        ui_rect ok = { c.x + 24, c.y + 128, 180, 46 };
        ui_rect cancel = { c.x + 216, c.y + 128, 180, 46 };

        if (g_point_in(&ok, x, y)) {
            desk_prompt_commit();
        } else if (g_point_in(&cancel, x, y)) {
            g_desk_prompt = 0;
        }
        return;
    }

    if (g_desk_menu) {
        ui_rect p = desk_menu_rect();
        if (!g_point_in(&p, x, y)) {
            g_desk_menu = 0;
            return;
        }
        for (int i = 0; i < 4; i++) {
            ui_rect row = { p.x + 8, p.y + 8 + i * 38, p.w - 16, 34 };
            if (!g_point_in(&row, x, y)) continue;

            g_desk_menu = 0;
            if (i == 0 || i == 1) {
                g_desk_prompt = (i == 0) ? 1 : 2;
                g_desk_input_len = 0;
                g_desk_input[0] = '\0';
            } else if (i == 2) {
                gui_desk_refresh();
            } else {
                gui_files_open(g_desk_path);
                win_open(WIN_FILES);
            }
            return;
        }
        return;
    }

    /* 图标：单击选中，双击打开；空白处右键弹出菜单 */
    for (int i = 0; i < g_desk_count; i++) {
        if (!g_point_in(&g_desk[i].r, x, y)) continue;

        uint32_t now = net_now_ms();
        int dbl = (g_desk_click_idx == i) && (now - g_desk_click_ms < 420u);
        g_desk_click_idx = i;
        g_desk_click_ms = now;

        if (!dbl) return;

        char full[512];
        fm_join(full, (int)sizeof(full), g_desk_path, g_desk[i].name);

        if (desk_is_dir(&g_desk[i])) {
            gui_files_open(full);
            win_open(WIN_FILES);
        } else if (gui_ext_is(full, ".mp3")) {
            gui_open_media(full);
        } else if (gui_slyz_is_program(full)) {
            int win = gui_app_open_path(full);
            if (win >= 0) {
                win_open(win);
                g_focus = win;
            }
        } else {
            gui_editor_open(full);
            win_open(WIN_EDITOR);
        }
        return;
    }

    g_desk_click_idx = -1;

    /* 左键点桌面空白处只关闭已打开的菜单。
     * 右键菜单必须由右键上升沿触发，见 gui_desktop_rclick。 */
    g_desk_menu = 0;
}

/* 桌面右键：空白处弹出菜单；图标上右键选中图标后弹出；
 * 菜单已打开时，右键在菜单内等同执行该项，在菜单外则关闭。 */
static void gui_desktop_rclick(int x, int y) {
    if (g_desk_prompt) {
        g_desk_prompt = 0;
        return;
    }

    /* 任务栏/搜索/电源/用户/网络弹出层打开时，右键先关闭它们 */
    if (g_power_menu || g_user_menu || g_net_menu || g_search_open) {
        g_power_menu = 0;
        g_user_menu = 0;
        g_net_menu = 0;
        g_search_open = 0;
        return;
    }

    if (g_desk_menu) {
        ui_rect p = desk_menu_rect();
        if (g_point_in(&p, x, y)) {
            gui_desk_click(x, y);
        } else {
            g_desk_menu = 0;
        }
        return;
    }

    if (x < GUI_TASKBAR_W) return;

    /* 右键落在窗口上：关闭桌面菜单，不做窗口级右键菜单 */
    if (win_topmost_at(x, y) >= 0) return;

    for (int i = 0; i < g_desk_count; i++) {
        if (!g_point_in(&g_desk[i].r, x, y)) continue;
        g_desk_click_idx = i;
        g_desk_menu = 1;
        g_desk_menu_x = x;
        g_desk_menu_y = y;
        return;
    }

    g_desk_click_idx = -1;
    g_desk_menu = 1;
    g_desk_menu_x = x;
    g_desk_menu_y = y;
}

static int gui_desk_modal(void) {
    return g_desk_prompt != 0;
}

static int gui_desk_key(char key) {
    if (!g_desk_prompt) {
        if (g_desk_menu && key == 27) {
            g_desk_menu = 0;
            return 1;
        }
        return 0;
    }

    if (key == 27) {
        g_desk_prompt = 0;
        return 1;
    }
    if (key == '\r' || key == '\n') {
        desk_prompt_commit();
        return 1;
    }
    if (key == '\b' || key == 127) {
        if (g_desk_input_len > 0) {
            g_desk_input_len--;
            g_desk_input[g_desk_input_len] = '\0';
        }
        return 1;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
        if (g_desk_input_len < 40) {
            g_desk_input[g_desk_input_len++] = key;
            g_desk_input[g_desk_input_len] = '\0';
        }
        return 1;
    }
    return 1;
}

static void gui_desk_render(void) {
    for (int i = 0; i < g_desk_count; i++) {
        ui_rect r = g_desk[i].r;
        int selected = (g_desk_click_idx == i);
        int hover = g_point_in(&r, g_mx, g_my);

        if (selected) {
            ui_round_fill(r, 10, 0x1E3048);
        } else if (hover) {
            ui_round_fill(r, 10, 0x16202C);
        }

        int icon = GUI_DESK_ICON;
        int ix = r.x + (r.w - icon) / 2;
        int iy = r.y + 8;

        if (desk_is_dir(&g_desk[i])) {
            icon_folder(ix, iy, icon, C_WARN);
        } else {
            icon_file(ix, iy, icon, 0xA9BDD2);
        }

        int chars = (r.w - 12) / 16;
        if (chars < 1) chars = 1;
        gui_text_n(r.x + 6, r.y + icon + 16, g_desk[i].name, chars,
                   selected ? 0xFFFFFF : C_TEXT);
    }

    if (g_desk_count == 0) {
        int cx = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W) / 2;
        gui_text_center("Empty home directory", cx, gH / 2 - 40, 0x24344A);
    }

    if (g_desk_menu) gui_desk_menu_render();
    if (g_desk_prompt) gui_desk_prompt_render();
}

/* ============================================================
 * Slyz GUI 程序运行时
 * ------------------------------------------------------------
 * 首行为 "#gui" 的 .slyz 文件是声明式界面程序：
 *   #gui
 *   title "My App"
 *   size 560 460
 *   label "Hello"
 *   input name "Your name"
 *   button "Say hi" -> set:Hi {name}
 *   button "Date"   -> shell:date
 *   button "Close"  -> close
 *   space 8
 *   hr
 * 内核在独立窗口里渲染控件，按钮点击时执行动作：
 *   close            关闭窗口
 *   set:<text>       把文本追加到输出区（支持 {输入框名} 替换）
 *   open:<path>      在文件管理器 / 编辑器里打开
 *   shell:<command>  执行 shell 命令（非交互模式），输出进输出区
 * 解析失败或文件不合法时给出明确提示，不会静默失败。
 * ============================================================ */

#define GUI_SLZ_MAX        32
#define GUI_SLZ_LOG        4096
#define GUI_SLZ_SRC        16384

enum {
    SLZ_LABEL = 0,
    SLZ_BUTTON,
    SLZ_INPUT,
    SLZ_SPACE,
    SLZ_HR
};

struct SlzCtl {
    int     kind;
    char    text[96];
    char    action[80];
    char    name[32];
    char    value[64];
    int     value_len;
    ui_rect r;
};

static struct SlzCtl g_slz[GUI_SLZ_MAX];
static int      g_slz_count = 0;
static char     g_slz_title[64];
static int      g_slz_w = 560;
static int      g_slz_h = 480;
static char     g_slz_path[256];
static char     g_slz_log[GUI_SLZ_LOG];
static int      g_slz_log_len = 0;
static int      g_slz_focus = -1;
static int      g_slz_ready = 0;
static int      g_slz_kind = 0;   /* 窗口内容类型：1 = Slyz，0 = 旧内容 */

static void slz_log_clear(void) {
    g_slz_log[0] = '\0';
    g_slz_log_len = 0;
}

static void slz_log_append(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (g_slz_log_len >= GUI_SLZ_LOG - 1) {
            /* 满了就丢弃最前面的 1/4，保留最近的输出 */
            int drop = GUI_SLZ_LOG / 4;
            for (int k = 0; k < g_slz_log_len - drop; k++) {
                g_slz_log[k] = g_slz_log[k + drop];
            }
            g_slz_log_len -= drop;
        }
        g_slz_log[g_slz_log_len++] = s[i];
    }
    g_slz_log[g_slz_log_len] = '\0';
}

static void slz_sink(const char *s, int len) {
    slz_log_append(s, len);
}

/* 读取引号字符串；没有引号时读到行尾 */
static const char *slz_arg(const char *p, char *out, int cap) {
    int n = 0;

    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && n < cap - 1) out[n++] = *p++;
    }

    out[n] = '\0';
    return p;
}

static int slz_int(const char **pp, int def) {
    const char *p = *pp;
    int v = 0;
    int digits = 0;

    while (*p == ' ' || *p == '\t') p++;
    while (*p >= '0' && *p <= '9' && digits < 6) {
        v = v * 10 + (*p - '0');
        p++;
        digits++;
    }
    *pp = p;
    return digits > 0 ? v : def;
}

static int slz_starts(const char *line, const char *word) {
    int n = g_strlen(word);
    for (int i = 0; i < n; i++) {
        if (g_lower((unsigned char)line[i]) != word[i]) return 0;
    }
    return 1;
}

/* 解析源文本，成功返回 1 */
static int slz_parse(const char *src, int len) {
    g_slz_count = 0;
    g_slz_title[0] = '\0';
    g_copy(g_slz_title, "Slyz GUI", (int)sizeof(g_slz_title));
    g_slz_w = 560;
    g_slz_h = 480;

    int i = 0;

    /* 第一行必须是 #gui */
    while (i < len && (src[i] == ' ' || src[i] == '\t')) i++;
    if (i + 4 > len || src[i] != '#' ||
        g_lower((unsigned char)src[i + 1]) != 'g' ||
        g_lower((unsigned char)src[i + 2]) != 'u' ||
        g_lower((unsigned char)src[i + 3]) != 'i') {
        return 0;
    }
    while (i < len && src[i] != '\n') i++;
    if (i < len) i++;

    while (i < len) {
        int start = i;
        while (i < len && src[i] != '\n') i++;
        int line_len = i - start;
        if (i < len) i++;

        while (line_len > 0 && (src[start] == ' ' || src[start] == '\t')) {
            start++;
            line_len--;
        }
        while (line_len > 0 && (src[start + line_len - 1] == '\r' ||
                                src[start + line_len - 1] == ' ')) {
            line_len--;
        }
        if (line_len == 0 || src[start] == '#') continue;

        char line[200];
        int n = line_len < (int)sizeof(line) - 1 ? line_len : (int)sizeof(line) - 1;
        for (int k = 0; k < n; k++) line[k] = src[start + k];
        line[n] = '\0';

        const char *p = line;

        if (slz_starts(line, "title")) {
            p += 5;
            slz_arg(p, g_slz_title, (int)sizeof(g_slz_title));
        } else if (slz_starts(line, "size")) {
            p += 4;
            g_slz_w = slz_int(&p, 560);
            g_slz_h = slz_int(&p, 480);
            if (g_slz_w < 360) g_slz_w = 360;
            if (g_slz_h < 260) g_slz_h = 260;
        } else if (g_slz_count < GUI_SLZ_MAX) {
            struct SlzCtl *c = &g_slz[g_slz_count];

            if (slz_starts(line, "label")) {
                c->kind = SLZ_LABEL;
                slz_arg(p + 5, c->text, (int)sizeof(c->text));
                g_slz_count++;
            } else if (slz_starts(line, "input")) {
                c->kind = SLZ_INPUT;
                p = slz_arg(p + 5, c->name, (int)sizeof(c->name));
                slz_arg(p, c->text, (int)sizeof(c->text));
                c->value_len = 0;
                c->value[0] = '\0';
                g_slz_count++;
            } else if (slz_starts(line, "button")) {
                c->kind = SLZ_BUTTON;
                p = slz_arg(p + 6, c->text, (int)sizeof(c->text));
                while (*p == ' ' || *p == '\t') p++;
                if (p[0] == '-' && p[1] == '>') p += 2;
                slz_arg(p, c->action, (int)sizeof(c->action));
                g_slz_count++;
            } else if (slz_starts(line, "space")) {
                c->kind = SLZ_SPACE;
                p += 5;
                c->value_len = slz_int(&p, 8);
                g_slz_count++;
            } else if (slz_starts(line, "hr")) {
                c->kind = SLZ_HR;
                g_slz_count++;
            }
        }
    }

    return 1;
}

static void gui_slyz_req_size(int *w, int *h) {
    if (!g_slz_ready) return;
    if (w) *w = g_slz_w;
    if (h) *h = g_slz_h;
}

static int gui_slyz_is_program(const char *path) {    void *fh = slyzfs_open(path, "r");
    if (!fh) return 0;

    uint8_t head[8];
    int n = slyzfs_read(fh, head, 8);
    slyzfs_close(fh);

    /* SLYZ v2 编译产物以 "SLYZBIN\0" 开头。旧的 #gui 文本程序
     * 已不再识别：驱动器/文件管理器双击时应启动独立应用窗口，
     * 而不是把二进制当文本丢进编辑器。 */
    if (n < 8) return 0;
    return head[0] == 'S' && head[1] == 'L' && head[2] == 'Y' && head[3] == 'Z' &&
           head[4] == 'B' && head[5] == 'I' && head[6] == 'N' && head[7] == 0;
}

/* 从路径取出不含目录与扩展名的文件名，用于把 .slyz 绑定到窗口 */
static void slyz_stem(const char *path, char *out, int cap) {
    const char *base = path;
    for (const char *p = path; p && *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }

    int n = 0;
    while (base[n] && base[n] != '.' && n < cap - 1) {
        out[n] = base[n];
        n++;
    }
    out[n] = '\0';
}

/* 启动一个 SLYZ 程序并返回它占用的窗口；失败返回 -1。
 * 已知应用映射到桌面固定的窗口 id，未知程序使用通用 Slyz 窗口。 */
static int gui_app_open_path(const char *path) {
    if (!path || !*path) return -1;

    char stem[64];
    slyz_stem(path, stem, (int)sizeof(stem));

    int win = WIN_SLYZ;
    if (g_strcmp(stem, "calc") == 0) win = WIN_CALC;
    else if (g_strcmp(stem, "settings") == 0) win = WIN_SETTINGS;
    else if (g_strcmp(stem, "taskmgr") == 0) win = WIN_TASKMGR;
    else if (g_strcmp(stem, "files") == 0) win = WIN_FILES;
    else if (g_strcmp(stem, "media") == 0) win = WIN_MEDIA;

    if (!gui_app_start(win, path)) return -1;
    return win;
}

/* 扩展名比较（大小写不敏感） */
static int gui_ext_is(const char *path, const char *ext) {
    int n = g_strlen(path);
    int m = g_strlen(ext);
    if (n < m) return 0;
    for (int i = 0; i < m; i++) {
        if (g_lower((unsigned char)path[n - m + i]) !=
            g_lower((unsigned char)ext[i])) {
            return 0;
        }
    }
    return 1;
}

/* 双击 MP3：把路径交给媒体播放器，再启动它自己的窗口。
 * 播放器不在应用启动器里，只有这里会打开它。 */
static int gui_open_media(const char *path) {
    if (!path || !path[0]) return 0;
    audio_set_source(path);
    if (!gui_app_start(WIN_MEDIA, "system\\apps\\media.slyz")) return 0;
    win_open(WIN_MEDIA);
    g_focus = WIN_MEDIA;
    return 1;
}

int gui_open_file_path(const char *path) {
    if (!path || !path[0]) return -1;

    if (gui_ext_is(path, ".mp3")) {
        return gui_open_media(path) ? WIN_MEDIA : -1;
    }

    if (gui_slyz_is_program(path)) {
        int win = gui_app_open_path(path);
        if (win >= 0) {
            win_open(win);
            g_focus = win;
        }
        return win;
    }

    gui_editor_open(path);
    win_open(WIN_EDITOR);
    g_focus = WIN_EDITOR;
    return WIN_EDITOR;
}

static int gui_slyz_open(const char *path) {
    void *fh = slyzfs_open(path, "r");
    if (!fh) return 0;

    char src[GUI_SLZ_SRC];
    int len = 0;

    while (len < GUI_SLZ_SRC - 1) {
        int n = slyzfs_read(fh, (uint8_t *)src + len, GUI_SLZ_SRC - 1 - len);
        if (n <= 0) break;
        len += n;
    }
    slyzfs_close(fh);
    src[len] = '\0';

    g_copy(g_slz_path, path, (int)sizeof(g_slz_path));

    if (!slz_parse(src, len)) {
        slz_log_clear();
        slz_log_append("Invalid Slyz GUI program.", 27);
        slz_log_append("The first line must be: #gui\n", 30);
        g_slz_count = 0;
        g_slz_ready = 1;
        g_slz_kind = 1;
        return 0;
    }

    slz_log_clear();
    slz_log_append("Ready. ", 7);
    slz_log_append(path, g_strlen(path));
    slz_log_append("\n", 1);

    g_slz_focus = -1;
    g_slz_ready = 1;
    g_slz_kind = 1;
    return 1;
}

/* 把 {name} 替换成对应输入框内容 */
static void slz_expand(const char *in, char *out, int cap) {
    int o = 0;
    int i = 0;

    while (in[i] && o < cap - 1) {
        if (in[i] == '{') {
            char key[32];
            int k = 0;
            int j = i + 1;
            while (in[j] && in[j] != '}' && k < (int)sizeof(key) - 1) {
                key[k++] = in[j++];
            }
            key[k] = '\0';

            if (in[j] == '}') {
                int found = 0;
                for (int c = 0; c < g_slz_count; c++) {
                    if (g_slz[c].kind != SLZ_INPUT) continue;
                    if (g_strcmp(g_slz[c].name, key) != 0) continue;

                    for (int v = 0; g_slz[c].value[v] && o < cap - 1; v++) {
                        out[o++] = g_slz[c].value[v];
                    }
                    found = 1;
                    break;
                }
                if (!found) {
                    for (int v = 0; key[v] && o < cap - 1; v++) out[o++] = key[v];
                }
                i = j + 1;
                continue;
            }
        }

        out[o++] = in[i++];
    }

    out[o] = '\0';
}

static int slz_prefixed(const char *s, const char *prefix) {
    for (int i = 0; prefix[i]; i++) {
        if (s[i] != prefix[i]) return 0;
    }
    return 1;
}

static void slz_run_action(const char *action) {
    char buf[256];
    slz_expand(action, buf, (int)sizeof(buf));

    if (g_strcmp(buf, "close") == 0) {
        win_close(WIN_SLYZ);
        return;
    }
    if (slz_prefixed(buf, "set:")) {
        slz_log_append(buf + 4, g_strlen(buf + 4));
        slz_log_append("\n", 1);
        return;
    }
    if (slz_prefixed(buf, "open:")) {
        const char *target = buf + 5;
        if (gui_slyz_is_program(target)) {
            gui_slyz_open(target);
            win_open(WIN_SLYZ);
        } else {
            gui_editor_open(target);
            win_open(WIN_EDITOR);
        }
        return;
    }
    if (slz_prefixed(buf, "shell:")) {
        slz_log_append("$ ", 2);
        slz_log_append(buf + 6, g_strlen(buf + 6));
        slz_log_append("\n", 1);

        tty_set_output_hook(slz_sink);
        shell_set_noninteractive(1);
        shell_exec_line(buf + 6);
        shell_set_noninteractive(0);
        tty_set_output_hook(0);

        slz_log_append("\n", 1);
        return;
    }

    slz_log_append("Unknown action: ", 16);
    slz_log_append(buf, g_strlen(buf));
    slz_log_append("\n", 1);
}

static void gui_slyz_render(const ui_theme *t, ui_rect c) {
    (void)t;

    ui_round_fill(c, 10, 0x0B1017);

    int x = c.x + 16;
    int y = c.y + 12;
    int w = c.w - 32;
    int bottom = c.y + c.h - 12;

    for (int i = 0; i < g_slz_count; i++) {
        struct SlzCtl *ctl = &g_slz[i];

        if (ctl->kind == SLZ_SPACE) {
            ctl->r = (ui_rect){ x, y, w, ctl->value_len };
            y += ctl->value_len;
            continue;
        }
        if (ctl->kind == SLZ_HR) {
            ctl->r = (ui_rect){ x, y + 8, w, 2 };
            ui_hline(x, y + 8, w, C_BORDER);
            y += 20;
            continue;
        }
        if (ctl->kind == SLZ_LABEL) {
            int lines = ui_text_wrap_lines(w, ctl->text);
            if (lines < 1) lines = 1;
            int h = lines * 32;
            ctl->r = (ui_rect){ x, y, w, h };
            ui_text_wrap(x, y, w, 32, ctl->text, C_TEXT, 0);
            y += h + 8;
            continue;
        }
        if (ctl->kind == SLZ_INPUT) {
            int h = 44;
            ctl->r = (ui_rect){ x, y, w, h };
            ui_round_panel(ctl->r, 10,
                           (g_slz_focus == i) ? C_ACCENT : C_BORDER, 0x0C141E);

            if (ctl->value_len > 0) {
                gui_text_n(x + 14, y + 7, ctl->value, (w - 28) / 16, C_TEXT);
            } else if (ctl->text[0]) {
                gui_text_n(x + 14, y + 7, ctl->text, (w - 28) / 16, 0x5A6B7E);
            }

            if (g_slz_focus == i && ((net_now_ms() / 500u) & 1u) == 0) {
                int cx = x + 14 + ctl->value_len * 16;
                if (cx < x + w - 8) {
                    vbe_fill_rect((uint16_t)cx, (uint16_t)(y + 9), 2, 26, C_ACCENT);
                }
            }
            y += h + 10;
            continue;
        }
        if (ctl->kind == SLZ_BUTTON) {
            int h = 44;
            ctl->r = (ui_rect){ x, y, w, h };
            gui_button(ctl->r, ctl->text, g_point_in(&ctl->r, g_mx, g_my), 1);
            y += h + 10;
            continue;
        }
    }

    /* 输出区：占满剩余空间 */
    if (y < bottom - 60) {
        ui_rect out = { x, y + 6, w, bottom - y - 6 };
        ui_round_fill(out, 10, 0x0A111A);

        int rows = (out.h - 12) / 32;
        if (rows < 1) rows = 1;

        /* 只显示日志最后 rows 行 */
        int line_count = 1;
        for (int i = 0; i < g_slz_log_len; i++) {
            if (g_slz_log[i] == '\n') line_count++;
        }
        int skip = line_count - rows;
        if (skip < 0) skip = 0;

        int idx = 0;
        int cur = 0;
        while (idx < g_slz_log_len && cur < skip) {
            if (g_slz_log[idx] == '\n') cur++;
            idx++;
        }

        int r = 0;
        int col = 0;
        int cx = out.x + 12;
        while (idx < g_slz_log_len && r < rows) {
            char ch = g_slz_log[idx++];
            if (ch == '\n') {
                r++;
                col = 0;
                continue;
            }
            if (ch == '\r' || ch == '\t') continue;
            if ((unsigned char)ch < 32) continue;
            if (col < (out.w - 24) / 16) {
                ui_text_transparent(cx + col * 16, out.y + 8 + r * 32,
                                    (char[]){ ch, '\0' },
                                    (ch == '$') ? C_ACCENT3 : C_TEXT);
                col++;
            }
        }
    }
}

static void gui_slyz_click(ui_rect c, int x, int y) {
    (void)c;

    for (int i = 0; i < g_slz_count; i++) {
        if (!g_point_in(&g_slz[i].r, x, y)) continue;

        if (g_slz[i].kind == SLZ_INPUT) {
            g_slz_focus = i;
            return;
        }
        if (g_slz[i].kind == SLZ_BUTTON) {
            slz_run_action(g_slz[i].action);
            return;
        }
    }

    g_slz_focus = -1;
}

static void gui_slyz_key(char key) {
    if (g_slz_focus < 0 || g_slz_focus >= g_slz_count) {
        if (key == 27) win_close(WIN_SLYZ);
        return;
    }

    struct SlzCtl *ctl = &g_slz[g_slz_focus];

    if (key == 27) {
        g_slz_focus = -1;
        return;
    }
    if (key == '\b' || key == 127) {
        if (ctl->value_len > 0) {
            ctl->value_len--;
            ctl->value[ctl->value_len] = '\0';
        }
        return;
    }
    if (key == '\r' || key == '\n') {
        /* 回车：执行第一个按钮，符合"填完就提交"的直觉 */
        for (int i = 0; i < g_slz_count; i++) {
            if (g_slz[i].kind == SLZ_BUTTON) {
                slz_run_action(g_slz[i].action);
                return;
            }
        }
        return;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
        if (ctl->value_len < (int)sizeof(ctl->value) - 1) {
            ctl->value[ctl->value_len++] = key;
            ctl->value[ctl->value_len] = '\0';
        }
        return;
    }
}

static int gui_slyz_modal(void) {
    return g_slz_kind == 1 && g_slz_ready;
}

/* ============================================================
 * 桌面事件
 * ============================================================ */

static void gui_desktop_reset(void) {
    for (int i = 0; i < GUI_WIN_MAX; i++) {
        g_wins[i].open = 0;
        g_wins[i].z = 0;
        g_wins[i].maximized = 0;
        g_wins[i].minimized = 0;
    }

    g_z_counter = 0;
    g_focus = -1;
    g_dragging = -1;
    g_resizing = -1;
    g_resize_edge = 0;
    g_pending_action = ACT_NONE;

    /* 任务管理器：新会话从干净的采样窗口开始 */
    g_tm_cpu_permille = 0;
    g_tm_last_sample_ms = 0;
    g_tm_last_write_sectors = 0;
    g_tm_disk_rate_kb = 0;
    g_tm_tab = TM_TAB_OVERVIEW;

    g_search_open = 0;
    g_search_len = 0;
    g_search[0] = '\0';
    g_search_sel = 0;

    g_user_menu = 0;
    g_power_menu = 0;
    g_net_menu = 0;

    g_fm_modal = 0;
    g_fm_last_click_index = -1;

    g_ed_ask_save = 0;
    g_ed_close_request = 0;
    g_ed_open = 0;

    gui_desk_reset();
}

static void dirty_key_surface(void);

static void gui_desktop_key(char key) {
    /* Ctrl+Space 是输入法开关，任何焦点下都生效 */
    if (key == (char)0xFE) {
        int had_popup = gui_ime_active();
        ui_rect before = gui_ime_rect();
        ime_toggle_chinese();
        if (had_popup) {
            dirty_add(before.x - 6, before.y - 6, before.w + 12, before.h + 12);
        }
        dirty_key_surface();
        return;
    }

    /* 拼音状态只在文本输入窗口内生效，避免在计算器里把按键吃掉 */
    int text_focus = (g_focus == WIN_EDITOR || g_focus == WIN_TERM);
    if (text_focus && (ime_chinese_enabled() || ime_composing()[0] != '\0')) {
        int had_popup = gui_ime_active();
        ui_rect before = gui_ime_rect();

        char commit[64];
        int ir = ime_key((int)(unsigned char)key, commit, sizeof(commit));
        if (ir == IME_CONSUMED || ir == IME_COMMIT) {
            if (ir == IME_COMMIT) {
                if (g_focus == WIN_EDITOR) {
                    ed_insert_utf8(commit);
                } else {
                    term_insert_utf8(commit);
                }
            }
            /* 候选窗可能在窗口边缘之外，必须单独标脏，否则会留残影 */
            if (had_popup) {
                dirty_add(before.x - 6, before.y - 6, before.w + 12, before.h + 12);
            }
            if (gui_ime_active()) {
                ui_rect now = gui_ime_rect();
                dirty_add(now.x - 6, now.y - 6, now.w + 12, now.h + 12);
            }
            dirty_key_surface();
            return;
        }
    }

    if (gui_desk_key(key)) return;

    if (gui_files_modal_active()) {
        gui_files_modal_key(key);
        return;
    }
    if (gui_editor_modal_active()) {
        gui_editor_modal_key(key);
        return;
    }

    if (g_search_open) {
        if (key == 27) {
            g_search_open = 0;
            return;
        }
        if (key == '\r' || key == '\n') {
            int matches[GUI_APP_COUNT];
            int n = search_filter(matches);
            if (n > 0) {
                if (g_search_sel >= n) g_search_sel = 0;
                launch_app(g_apps[matches[g_search_sel]].win);
            }
            return;
        }
        if (key == (char)0xF6) {
            int matches[GUI_APP_COUNT];
            int n = search_filter(matches);
            if (n > 0) g_search_sel = (g_search_sel + n - 1) % n;
            return;
        }
        if (key == (char)0xF7) {
            int matches[GUI_APP_COUNT];
            int n = search_filter(matches);
            if (n > 0) g_search_sel = (g_search_sel + 1) % n;
            return;
        }
        if (key == '\b' || key == 127) {
            if (g_search_len > 0) {
                g_search_len--;
                g_search[g_search_len] = '\0';
                g_search_sel = 0;
            }
            return;
        }
        if ((unsigned char)key >= 32 && (unsigned char)key < 127 &&
            g_search_len < (int)sizeof(g_search) - 1) {
            g_search[g_search_len++] = key;
            g_search[g_search_len] = '\0';
            g_search_sel = 0;
        }
        return;
    }

    if (g_power_menu || g_user_menu || g_net_menu) {
        if (key == 27) {
            g_power_menu = 0;
            g_user_menu = 0;
            g_net_menu = 0;
        }
        return;
    }

    /* 没有任何窗口获得焦点时，用快捷键启动应用：
     * 这既方便纯键盘用户，也让 QEMU 自动化测试不必模拟鼠标坐标。 */
    if (g_focus < 0) {
        if (key == 'f' || key == 'F') {
            launch_app(WIN_FILES);
            return;
        }
        if (key == 'e' || key == 'E') {
            launch_app(WIN_EDITOR);
            return;
        }
        if (key == 't' || key == 'T') {
            launch_app(WIN_TERM);
            return;
        }
        if (key == 'b' || key == 'B') {
            launch_app(WIN_BROWSER);
            return;
        }
        if (key == 's' || key == 'S') {
            launch_app(WIN_SETTINGS);
            return;
        }
        if (key == 'm' || key == 'M') {
            launch_app(WIN_TASKMGR);
            return;
        }
        if (key == 'c' || key == 'C') {
            launch_app(WIN_CALC);
            return;
        }
        if (key == '/') {
            g_search_open = 1;
            g_search_len = 0;
            g_search[0] = '\0';
            g_search_sel = 0;
            return;
        }
        if (key == 12) {   /* Ctrl+L: log out */
            g_pending_action = ACT_LOGOUT;
            return;
        }
    }

    if (g_focus == WIN_FILES) {
        gui_files_key(key);
    } else if (g_focus == WIN_EDITOR) {
        gui_editor_key(key);
    } else if (g_focus == WIN_TERM) {
        gui_term_key(key);
    } else if (g_focus == WIN_BROWSER) {
        gui_browser_key(key);
    } else if (g_focus == WIN_SLYZ) {
        if (gui_app_active(WIN_SLYZ)) {
            gui_app_key(WIN_SLYZ, (int)(unsigned char)key);
        } else {
            gui_slyz_key(key);
        }
    } else if (g_focus == WIN_TASKMGR) {
        gui_tm_key(key);
    } else if (g_focus == WIN_CALC) {
        gui_calc_key(key);
    } else if (g_focus == WIN_MEDIA) {
        gui_app_key(WIN_MEDIA, (int)(unsigned char)key);
    } else if (g_focus == WIN_EXTERNAL) {
        if (g_ext_host && g_ext_host->key) {
            g_ext_host->key((int)(unsigned char)key);
        }
    }
}

static void gui_taskbar_click(int x, int y) {
    ui_rect av = tb_avatar_rect();
    ui_rect sr = tb_search_rect();
    ui_rect pr = tb_power_rect();
    ui_rect nr = tb_net_rect();

    if (g_point_in(&av, x, y)) {
        g_user_menu = !g_user_menu;
        g_power_menu = 0;
        return;
    }
    if (g_point_in(&sr, x, y)) {
        g_search_open = !g_search_open;
        if (g_search_open) {
            g_search_len = 0;
            g_search[0] = '\0';
            g_search_sel = 0;
        }
        g_user_menu = 0;
        g_power_menu = 0;
        return;
    }
    if (g_point_in(&nr, x, y)) {
        g_net_menu = !g_net_menu;
        g_user_menu = 0;
        g_power_menu = 0;
        return;
    }
    if (g_point_in(&pr, x, y)) {
        g_power_menu = !g_power_menu;
        g_user_menu = 0;
        return;
    }

    for (int i = 0; i < GUI_APP_COUNT; i++) {
        if (g_apps[i].hidden) continue;
        ui_rect r = tb_app_rect(i);
        if (g_point_in(&r, x, y)) {
            launch_app(g_apps[i].win);
            return;
        }
    }

    for (int i = 0; i < GUI_WIN_MAX; i++) {
        if (!g_wins[i].open || i == WIN_MEDIA) continue;
        ui_rect r = tb_win_rect(i);
        if (g_point_in(&r, x, y)) {
            if (g_wins[i].minimized) win_restore_from_min(i);
            win_focus(i);
            return;
        }
    }
}

static void gui_user_menu_click(int x, int y) {
    ui_rect p = user_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_user_menu = 0;
        return;
    }

    ui_rect out = { p.x + 12, p.y + 98, p.w - 24, 54 };
    if (g_point_in(&out, x, y)) {
        g_pending_action = ACT_LOGOUT;
        g_user_menu = 0;
    }
}

static void gui_power_menu_click(int x, int y) {
    ui_rect p = power_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_power_menu = 0;
        return;
    }

    for (int i = 0; i < 4; i++) {
        ui_rect row = { p.x + 12, p.y + 60 + i * 42, p.w - 24, 38 };
        if (!g_point_in(&row, x, y)) continue;

        if (i == 0) g_pending_action = ACT_REBOOT;
        else if (i == 1) g_pending_action = ACT_SHUTDOWN;
        else if (i == 2) g_pending_action = ACT_TTY;
        g_power_menu = 0;
        return;
    }
}

static void gui_net_menu_click(int x, int y) {
    ui_rect p = net_menu_rect();
    if (!g_point_in(&p, x, y)) {
        g_net_menu = 0;
        return;
    }

    ui_rect rf = net_menu_close_rect(&p);
    if (g_point_in(&rf, x, y)) {
        g_net_menu = 0;
        return;
    }

    ui_rect vs = volume_slider_rect(&p);
    ui_rect hit = { vs.x, vs.y - 12, vs.w, vs.h + 24 };
    if (g_point_in(&hit, x, y)) {
        int vol = (x - vs.x) * 100 / vs.w;
        audio_set_volume(vol);
        dirty_add_padded(&p, 18);
        return;
    }
}

static void gui_search_click(int x, int y) {
    ui_rect p = search_panel_rect();
    if (!g_point_in(&p, x, y)) {
        g_search_open = 0;
        return;
    }

    int matches[GUI_APP_COUNT];
    int n = search_filter(matches);

    int row_y = p.y + 120;
    for (int i = 0; i < n; i++) {
        ui_rect row = { p.x + 12, row_y + i * 72, p.w - 24, 64 };
        if (g_point_in(&row, x, y)) {
            launch_app(g_apps[matches[i]].win);
            return;
        }
    }
}

static void gui_window_click(int id, int x, int y) {
    ui_rect r = g_wins[id].r;

    ui_rect close_r = win_close_rect(&r);
    if (g_point_in(&close_r, x, y)) {
        if (id == WIN_EDITOR && g_ed_dirty) {
            g_ed_ask_save = 1;
            win_focus(id);
        } else {
            win_close(id);
        }
        return;
    }

    ui_rect max_r = win_max_btn_rect(&r);
    if (g_point_in(&max_r, x, y)) {
        win_toggle_max(id);
        return;
    }

    ui_rect min_r = win_min_btn_rect(&r);
    if (g_point_in(&min_r, x, y)) {
        win_minimize(id);
        return;
    }

    /* 边缘/角落调整大小优先于拖动与内容点击 */
    int edge = win_resize_edge_at(&r, x, y, g_wins[id].maximized);
    if (edge) {
        win_focus(id);
        g_resizing = id;
        g_resize_edge = edge;
        g_resize_start = r;
        g_resize_mx0 = x;
        g_resize_my0 = y;
        return;
    }

    if (y >= r.y && y < r.y + 52) {
        win_focus(id);
        /* 最大化时标题栏不拖动，避免把窗口拖成"半出屏"的怪状态 */
        if (!g_wins[id].maximized) {
            g_dragging = id;
            g_drag_dx = x - r.x;
            g_drag_dy = y - r.y;
        }
        return;
    }

    win_focus(id);

    ui_rect c = win_content_rect(&r);
    if (id == WIN_FILES) {
        gui_files_click(c, x, y);
    } else if (id == WIN_EDITOR) {
        gui_editor_click(c, x, y);
    } else if (id == WIN_TERM) {
        gui_term_click(c, x, y);
    } else if (id == WIN_BROWSER) {
        gui_browser_click(c, x, y);
    } else if (id == WIN_SLYZ) {
        if (gui_app_active(WIN_SLYZ)) {
            gui_app_click(WIN_SLYZ, x - c.x, y - c.y, 1);
        } else {
            gui_slyz_click(c, x, y);
        }
    } else if (id == WIN_TASKMGR) {
        gui_tm_click(c, x, y);
    } else if (id == WIN_CALC) {
        gui_calc_click(c, x, y);
    } else if (id == WIN_MEDIA) {
        if (gui_app_active(WIN_MEDIA)) {
            gui_app_click(WIN_MEDIA, x - c.x, y - c.y, 1);
        }
    } else if (id == WIN_EXTERNAL) {
        if (g_ext_host && g_ext_host->mouse) {
            g_ext_host->mouse(x - c.x, y - c.y, 1);
        }
    } else {
        gui_settings_click(c, x, y);
    }
}

static void gui_desktop_click(int x, int y) {
    if (g_fm_modal) {
        gui_files_modal_click(g_wins[WIN_FILES].r, x, y);
        return;
    }
    if (g_ed_ask_save) {
        gui_editor_modal_click(g_wins[WIN_EDITOR].r, x, y);
        return;
    }

    if (g_power_menu) {
        gui_power_menu_click(x, y);
        return;
    }
    if (g_net_menu) {
        gui_net_menu_click(x, y);
        return;
    }
    if (g_user_menu) {
        gui_user_menu_click(x, y);
        return;
    }
    if (g_search_open) {
        gui_search_click(x, y);
        return;
    }

    if (x < GUI_TASKBAR_W) {
        gui_taskbar_click(x, y);
        return;
    }

    int id = win_topmost_at(x, y);
    if (id >= 0) {
        gui_window_click(id, x, y);
        return;
    }

    g_focus = -1;
    gui_desk_click(x, y);
}

/* ---------- 呈现 ---------- */

/* 结构性状态指纹：兜底检测"该刷新但没人标脏"的情况。
 * 刻意不含窗口坐标——拖动是逐帧标脏的，若把坐标算进来，每一帧
 * 指纹都会变化并退化成全屏刷新，动态脏矩形就白做了。 */
static uint32_t g_sig_mix(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t gui_state_sig(void) {
    uint32_t h = 2166136261u;

    h = g_sig_mix(h, (uint32_t)(g_focus + 1));
    h = g_sig_mix(h, (uint32_t)g_search_open);
    h = g_sig_mix(h, (uint32_t)g_user_menu);
    h = g_sig_mix(h, (uint32_t)g_power_menu);
    h = g_sig_mix(h, (uint32_t)g_net_menu);
    h = g_sig_mix(h, (uint32_t)g_fm_modal);
    h = g_sig_mix(h, (uint32_t)g_ed_ask_save);
    h = g_sig_mix(h, (uint32_t)g_desk_menu);
    h = g_sig_mix(h, (uint32_t)g_desk_prompt);
    h = g_sig_mix(h, (uint32_t)(g_desk_click_idx + 1));
    h = g_sig_mix(h, (uint32_t)g_desk_count);

    for (int i = 0; i < GUI_WIN_MAX; i++) {
        h = g_sig_mix(h, (uint32_t)(g_wins[i].open ? (g_wins[i].z + 1) : 0));
        h = g_sig_mix(h, (uint32_t)g_wins[i].maximized);
        h = g_sig_mix(h, (uint32_t)g_wins[i].minimized);
    }

    return h;
}

/* 键盘输入影响的表面：搜索面板 / 焦点窗口 */
static void dirty_key_surface(void) {
    if (g_search_open) {
        ui_rect p = search_panel_rect();
        dirty_add_padded(&p, 18);
        return;
    }
    if (g_focus >= 0 && g_wins[g_focus].open) {
        dirty_window(g_focus);
        return;
    }
    dirty_all();
}

/* 光标闪烁只影响"带输入框"的表面 */
static void dirty_blink_surface(void) {
    if (g_search_open) {
        ui_rect p = search_panel_rect();
        dirty_add_padded(&p, 18);
        return;
    }
    if (g_fm_modal) {
        dirty_window(WIN_FILES);
        return;
    }
    if (g_ed_ask_save) {
        dirty_window(WIN_EDITOR);
        return;
    }
    if (gui_editor_is_open()) {
        dirty_window(WIN_EDITOR);
    }
}

static void pf_account(int full, uint32_t pixels);

static void gui_present_all(void) {
    gui_cursor_erase();
    hunt_reset();
    gui_desktop_render();
    hunt_snapshot();
    gui_cursor_draw(g_mx, g_my);
    ui_swap();
}

static void gui_present_cursor(void) {
    int old_x = g_cursor_x;
    int old_y = g_cursor_y;

    gui_cursor_erase();
    gui_cursor_draw(g_mx, g_my);

    if (old_x >= 0) {
        vbe_swap_rect((uint16_t)old_x, (uint16_t)old_y,
                      GUI_CURSOR_W, GUI_CURSOR_H);
    }
    vbe_swap_rect((uint16_t)g_mx, (uint16_t)g_my,
                  GUI_CURSOR_W, GUI_CURSOR_H);

    pf_account(0, (uint32_t)GUI_CURSOR_W * GUI_CURSOR_H * 2u);
}

/* ---- 临时呈现统计（验证稳态没有全屏 blit） ---- */
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

static uint32_t g_pf_frames = 0;
static uint32_t g_pf_full = 0;
static uint32_t g_pf_pixels = 0;

static void pf_account(int full, uint32_t pixels) {
    g_pf_frames++;
    if (full) g_pf_full++;
    g_pf_pixels += pixels;

    if (g_pf_frames >= 600) {
        kserial_puts("[GUI] frames=");
        kserial_dec32(g_pf_frames);
        kserial_puts(" full=");
        kserial_dec32(g_pf_full);
        kserial_puts(" avg_px=");
        kserial_dec32(g_pf_pixels / g_pf_frames);
        kserial_puts("\r\n");
        g_pf_frames = 0;
        g_pf_full = 0;
        g_pf_pixels = 0;
    }
}

static void gui_present_dirty(void) {
    int old_x = g_cursor_x;
    int old_y = g_cursor_y;

    gui_cursor_erase();

    hunt_reset();
    if (dirty_is_full()) {
        gui_desktop_render();
    } else {
        gui_render_dirty_surfaces();
    }
    hunt_snapshot();

    gui_cursor_draw(g_mx, g_my);

    if (dirty_is_full()) {
        ui_swap();
        pf_account(1, (uint32_t)gW * (uint32_t)gH);
        return;
    }

    uint32_t px = (uint32_t)GUI_CURSOR_W * GUI_CURSOR_H * 2u;
    int n_rects = dirty_rect_count();
    for (int i = 0; i < n_rects; i++) {
        ui_rect rr;
        dirty_rect_get(i, &rr);
        px += (uint32_t)rr.w * (uint32_t)rr.h;
    }

    dirty_swap_all(old_x, old_y);
    pf_account(0, px);
}

static int gui_desktop_run(void) {
    int last_hover = -1;
    int last_blink = -1;
    char last_clock[6];
    uint32_t last_sig = 0;

    gui_desktop_reset();

    dirty_reset();

    gui_mouse_update();
    gui_present_all();

    gui_clock_text(last_clock);
    last_sig = gui_state_sig();
    last_hover = gui_hover_id();

    for (;;) {
        uint64_t frame_start = gui_rdtsc();

        if (acpica_power_button_pending()) {
            gui_cursor_erase();
            return ACT_SHUTDOWN;
        }
        acpica_drain_deferred();

        audio_pump();

        /* 协作式调度：给音频驱动任务一个执行时间片。 */
        task_yield();

        dirty_reset();
        if (g_redraw_all_pending) {
            g_redraw_all_pending = 0;
            dirty_all();
        }
        gui_app_begin_frame();

        /* SLYZ 应用按帧自绘：每帧把内容区标脏，渲染阶段由虚拟机
         * 推进一帧并完整重画。窗口尺寸变化时这里拿到的是新内容区。 */
        for (int sw = 0; sw < GUI_WIN_MAX; sw++) {
            if (g_wins[sw].open && !g_wins[sw].minimized && gui_app_active(sw)) {
                ui_rect sr = win_content_rect(&g_wins[sw].r);
                dirty_add_rect(&sr);
            }
        }

        int old_mx = g_mx;
        int old_my = g_my;

        gui_mouse_update();

        /* 鼠标滚轮：优先交给内容区内的 SLYZ 应用（v3 声明式界面可滚动），
         * 终端窗口沿用历史滚动。 */
        if (g_mwheel != 0) {
            int top = win_topmost_at(g_mx, g_my);
            if (top >= 0 && gui_app_active(top)) {
                ui_rect ac = win_content_rect(&g_wins[top].r);
                if (g_point_in(&ac, g_mx, g_my)) {
                    gui_app_wheel(top, g_mwheel);
                    dirty_window(top);
                }
            } else if (g_wins[WIN_TERM].open && !g_wins[WIN_TERM].minimized &&
                       top == WIN_TERM) {
                ui_rect tc = win_content_rect(&g_wins[WIN_TERM].r);
                if (g_point_in(&tc, g_mx, g_my)) {
                    gui_term_scroll_by(g_mwheel * 3);
                    dirty_window(WIN_TERM);
                }
            }
            g_mwheel = 0;
        }

        /* v3 应用需要逐帧的鼠标位置来更新 hover；这里统一转发，
         * 不在窗口内的坐标会作为 down=0 传入，从而清除旧 hover。 */
        for (int mw = 0; mw < GUI_WIN_MAX; mw++) {
            if (!g_wins[mw].open || g_wins[mw].minimized || !gui_app_active(mw)) continue;
            ui_rect mc = win_content_rect(&g_wins[mw].r);
            int lx = g_mx - mc.x;
            int ly = g_my - mc.y;
            int is_top = (win_topmost_at(g_mx, g_my) == mw);
            int down = (is_top && g_point_in(&mc, g_mx, g_my) && g_mdown) ? 1 : 0;
            gui_app_mouse(mw, lx, ly, down);
        }

        if (g_dragging >= 0 && g_mmoved) {
            ui_rect before = g_wins[g_dragging].r;
            ui_rect *r = &g_wins[g_dragging].r;

            r->x = g_mx - g_drag_dx;
            r->y = g_my - g_drag_dy;

            if (r->x < GUI_TASKBAR_W) r->x = GUI_TASKBAR_W;
            if (r->y < 0) r->y = 0;
            if (r->x + r->w > gW - 4) r->x = gW - 4 - r->w;
            if (r->y + r->h > gH - 4) r->y = gH - 4 - r->h;

            /* 窗口跟手：旧位置要擦，新位置要画，两个矩形都要上屏 */
            dirty_add_padded(&before, GUI_SHADOW_PAD);
            dirty_add_padded(r, GUI_SHADOW_PAD);
        } else if (g_resizing >= 0 && g_mmoved) {
            ui_rect before = g_wins[g_resizing].r;
            ui_rect *rr = &g_wins[g_resizing].r;

            int dx = g_mx - g_resize_mx0;
            int dy = g_my - g_resize_my0;

            if (g_resize_edge & 1) {
                int nw = g_resize_start.w + dx;
                int min_w = (g_resizing == WIN_TASKMGR) ? 900 : 520;
                if (nw < min_w) nw = min_w;
                if (rr->x + nw > gW - 8) nw = gW - 8 - rr->x;
                rr->w = nw;
            }
            if (g_resize_edge & 2) {
                int nh = g_resize_start.h + dy;
                int min_h = (g_resizing == WIN_TASKMGR) ? 660 : 360;
                if (nh < min_h) nh = min_h;
                if (rr->y + nh > gH - 8) nh = gH - 8 - rr->y;
                rr->h = nh;
            }

            dirty_add_padded(&before, GUI_SHADOW_PAD);
            dirty_add_padded(rr, GUI_SHADOW_PAD);
        } else if (g_mmoved) {
            /* 窗口内部按钮的高亮不由 gui_hover_id 体现，靠渲染时登记的
             * 控件矩形精确擦除；任务栏/面板元素再由 hover id 补一层。 */
            dirty_hover_scan(old_mx, old_my, g_mx, g_my);
        }

        int hover = gui_hover_id();
        if (hover != last_hover) {
            ui_rect box;
            if (gui_hover_rect(last_hover, &box)) dirty_add_rect(&box);
            if (gui_hover_rect(hover, &box)) dirty_add_rect(&box);
            last_hover = hover;
        }

        if (g_mclicked) {
            uint32_t before = gui_state_sig();
            gui_desktop_click(g_mx, g_my);
            if (gui_state_sig() != before) {
                dirty_all();
            } else {
                dirty_surface_at(g_mx, g_my);
            }
        }

        /* 右键：桌面右键菜单由它打开/关闭/执行，与左键互不替代 */
        if (g_rclicked) {
            uint32_t before_r = gui_state_sig();
            gui_desktop_rclick(g_mx, g_my);
            if (gui_state_sig() != before_r) {
                dirty_all();
            }
        }

        if (g_mreleased && g_dragging >= 0) {
            dirty_window(g_dragging);
            g_dragging = -1;
        }

        if (g_mreleased && g_resizing >= 0) {
            dirty_window(g_resizing);
            g_resizing = -1;
            g_resize_edge = 0;
        }

        /* 编辑器内拖动选择：按下时已记录锚点，这里持续更新活动端 */
        if (g_ed_drag_sel) {
            if (!g_mdown) {
                g_ed_drag_sel = 0;
            } else if (g_mmoved && g_wins[WIN_EDITOR].open &&
                       !g_wins[WIN_EDITOR].minimized) {
                ui_rect ec = win_content_rect(&g_wins[WIN_EDITOR].r);
                gui_editor_drag_to(ec, g_mx, g_my);
                dirty_window(WIN_EDITOR);
            }
        }

        int typed = 0;
        for (int k = 0; k < 8; k++) {
            char c = gui_kb_read();
            if (c == 0) break;
            gui_desktop_key(c);
            typed = 1;
        }
        if (typed) {
            dirty_key_surface();
        }

        if (gui_editor_should_close()) {
            win_close(WIN_EDITOR);
            dirty_all();
        }

        /* SLYZ 应用主动退出或请求关闭窗口 */
        for (int sw = 0; sw < GUI_WIN_MAX; sw++) {
            if (g_wins[sw].open && gui_app_active(sw) && gui_app_should_close(sw)) {
                win_close(sw);
                dirty_all();
            }
        }

        if (g_fm_msg[0] && net_now_ms() > g_fm_msg_until) {
            g_fm_msg[0] = '\0';
            dirty_window(WIN_FILES);
        }
        if (g_ed_msg[0] && net_now_ms() > g_ed_msg_until) {
            g_ed_msg[0] = '\0';
            dirty_window(WIN_EDITOR);
        }
        if (g_set_msg[0] && net_now_ms() > g_set_msg_until) {
            g_set_msg[0] = '\0';
            dirty_window(WIN_SETTINGS);
        }

        int blink = (int)((net_now_ms() / 500u) & 1u);
        if (blink != last_blink) {
            last_blink = blink;
            dirty_blink_surface();
        }

        /* 时钟每秒跳一次，只重绘右上角徽标与任务栏时钟卡片 */
        char clock[6];
        gui_clock_text(clock);
        if (g_strcmp(clock, last_clock) != 0) {
            g_copy(last_clock, clock, (int)sizeof(last_clock));
            dirty_add(gW - 116, 16, 100, 44);
            ui_rect cb = tb_clock_rect();
            dirty_add_rect(&cb);

            /* 网络链路是异步协商的：每秒重新读一次链路状态，
             * 让任务栏指示点与打开的面板能自己从红变绿。 */
            ui_rect nb = tb_net_rect();
            dirty_add_rect(&nb);
            if (g_net_menu) {
                ui_rect nm = net_menu_rect();
                dirty_add_rect(&nm);
            }
        }

        if (g_pending_action != ACT_NONE) {
            gui_cursor_erase();
            return g_pending_action;
        }

        /* 结构性状态变了却没人标脏时，兜底整帧刷新 */
        if (!dirty_is_full() && gui_state_sig() != last_sig) {
            dirty_all();
        }

        if (dirty_is_full() || g_dirty_count > 0) {
            dirty_compact();
            gui_present_dirty();
            last_sig = gui_state_sig();
        } else if (g_mmoved) {
            gui_present_cursor();
        }

        uint64_t work_end = gui_rdtsc();

        for (volatile int spin = 0; spin < 12000; spin++) {
            __asm__ volatile ("nop");
        }

        /* CPU 占用：本帧从循环开始到 present 完成的时间 ÷ 整帧时间。
         * 8 帧滑动平均，避免单帧抖动让数字乱跳。 */
        uint64_t frame_end = gui_rdtsc();
        uint64_t frame_total = frame_end - frame_start;
        if (frame_total > 0) {
            uint32_t inst = (uint32_t)(((work_end - frame_start) * 1000ull) / frame_total);
            if (inst > 1000u) inst = 1000u;
            g_tm_cpu_permille = (g_tm_cpu_permille * 7u + inst) / 8u;
        }

        /* 每秒采样一次任务/磁盘数据；任务管理器打开时同步刷新 */
        if (gui_tm_sample()) {
            if (g_wins[WIN_TASKMGR].open && !g_wins[WIN_TASKMGR].minimized) {
                dirty_window(WIN_TASKMGR);
            }
        }

        /* 调试 HUD：画在 backbuffer 上并单独上屏，覆盖在所有窗口之上。 */
        debug_hud_gui_tick();
    }
}

/* ============================================================
 * 图形会话入口
 * ============================================================ */

void gui_main(void) {
    if (!vbe_is_enabled()) return;

    gW = vbe_current.width;
    gH = vbe_current.height;
    if (gW < 800 || gH < 600) return;

    ui_theme_dark(&g_theme);
    g_theme.bg = C_BG;
    g_theme.panel = C_PANEL;
    g_theme.panel_alt = C_PANEL2;
    g_theme.border = C_BORDER;
    g_theme.text = C_TEXT;
    g_theme.text_dim = C_DIM;
    g_theme.accent = C_ACCENT;
    g_theme.accent_hover = C_ACCENT3;
    g_theme.accent_dim = C_ACCENT2;
    g_theme.ok = C_OK;
    g_theme.warn = C_WARN;
    g_theme.err = C_ERR;

    (void)ps2_mouse_init();

    /* 进入 GUI 时屏蔽 IRQ1，键盘改由 gui_kb_read 轮询。
     * 8086 的 8042 输出缓冲只有一个字节，键盘中断和鼠标轮询同时
     * 读会互抢数据（表现为按一个键出乱码），因此 GUI 全程只能有
     * 一个键盘读者。gui_kb_begin 内部已清空残留按键。 */
    gui_kb_begin();

    gui_app_init();

    /* AC'97 声卡与媒体播放服务；没有声卡时自动降级为静音。 */
    audio_init();
    audio_start_task();

    /* GUI 会话期间关闭 SlyzFS 的屏幕日志：文件管理器的 list_dir/open
     * 会在错误路径调用 printk，而 printk 直写 LFB，会直接在窗口上
     * 打出一行文本。串口日志仍然保留。 */
    slyzfs_set_quiet(1);

    gui_startup_stage("Initializing display", 8);
    gui_delay_ms(160);

    /* 网络栈只初始化一次：任务栏指示点、网络面板、浏览器都依赖它。
     * 没有网卡时 init 返回 -1，不影响后续启动。 */
    (void)net_init();

    auth_init();

    gui_startup_stage("Loading accounts", 42);
    gui_delay_ms(180);
    auth_reload();

    gui_startup_stage("Preparing user session", 74);
    gui_delay_ms(180);

    gui_startup_stage("Starting desktop", 100);
    gui_delay_ms(260);

    for (;;) {
        int uid = gui_login_screen();
        if (uid < 0) {
            slyzfs_set_quiet(0);
            gui_kb_end();
            return;
        }

        int action = gui_desktop_run();

        if (action == ACT_LOGOUT) {
            auth_logout();
            continue;
        }
        if (action == ACT_TTY) {
            auth_logout();
            slyzfs_set_quiet(0);
            gui_kb_end();
            return;
        }
        if (action == ACT_REBOOT) {
            gui_kb_end();
            reboot();
            slyzfs_set_quiet(0);
            return;
        }
        if (action == ACT_SHUTDOWN) {
            gui_kb_end();
            if (!acpica_request_s5()) {
                acpi_power_off();
            }
            slyzfs_set_quiet(0);
            return;
        }
    }
}

