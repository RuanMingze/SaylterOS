/* gui_core.c - framebuffer/input/surface primitives: text, icons, cursor, mouse, keyboard, IME, background, clock. */

#include "gui_internal.h"
#include "slyz.h"
#include "imgcodec.h"

int gW = 0;
int gH = 0;
ui_theme g_theme;
struct GuiPalette g_pal;

/* ============================================================
 * 主题
 * ------------------------------------------------------------
 * 两套出厂配色。所有绘制代码引用 C_* 宏，因此切换主题后只需要
 * 标脏重绘，不需要重建任何窗口。
 *
 *   深色（GUI_THEME_DARK_ORANGE）：接近黑的暖底 + 高饱和橙色强调，
 *     面向夜间和长时间使用；标题栏、任务栏用暖棕层次区分。
 *   浅色（GUI_THEME_LIGHT_GREEN）：纸白/浅薄荷底 + 清晰绿强调，
 *     面向白天；文字用深绿黑保证对比度，边框用低饱和绿灰。
 * ============================================================ */

void gui_theme_apply(int id) {
    struct GuiPalette *p = &g_pal;

    if (id == GUI_THEME_LIGHT_GREEN) {
        p->bg           = 0xE9F4EB;
        p->bg2          = 0xF6FBF7;
        p->panel        = 0xFFFFFF;
        p->panel2       = 0xF1F7F2;
        p->panel3       = 0xE2EFE5;
        p->border       = 0xC6DCCB;
        p->text         = 0x15271B;
        p->dim          = 0x5B7263;
        p->accent       = 0x1FA45A;
        p->accent2      = 0x177F46;
        p->accent3      = 0x2FBF6E;
        p->ok           = 0x1FA45A;
        p->err          = 0xC63C3C;
        p->warn         = 0xAD7200;

        p->avatar_a     = 0x2FBF6E;
        p->avatar_b     = 0x177F46;
        p->avatar_c     = 0x1FA45A;
        p->avatar_d     = 0xC97A00;

        p->bar_top      = 0xF9FCFA;
        p->bar_bottom   = 0xE7F2EA;
        p->hover        = 0xE3F1E7;
        p->selected     = 0xCFE9D6;
        p->win_body     = 0xFFFFFF;
        p->title        = 0xEDF7F0;
        p->title_active = 0xDCF0E2;
        p->shadow       = 0x4A5C4F;
        p->menu_bg      = 0xFFFFFF;
        p->tip_bg       = 0x2A3D30;
    } else {
        p->bg           = 0x0E0B08;
        p->bg2          = 0x14100C;
        p->panel        = 0x1A140F;
        p->panel2       = 0x231A12;
        p->panel3       = 0x2D2117;
        p->border       = 0x3C2C1D;
        p->text         = 0xF7EFE5;
        p->dim          = 0xB39C83;
        p->accent       = 0xFF7A18;
        p->accent2      = 0xD85B05;
        p->accent3      = 0xFFA24D;
        p->ok           = 0x3DD68C;
        p->err          = 0xFF5F56;
        p->warn         = 0xFFC145;

        p->avatar_a     = 0xFF8A3D;
        p->avatar_b     = 0xD85B05;
        p->avatar_c     = 0x3DD68C;
        p->avatar_d     = 0xFFC145;

        p->bar_top      = 0x1D140C;
        p->bar_bottom   = 0x100B07;
        p->hover        = 0x2B1F13;
        p->selected     = 0x3B2816;
        p->win_body     = 0x160F0A;
        p->title        = 0x23180F;
        p->title_active = 0x2E2115;
        p->shadow       = 0x050301;
        p->menu_bg      = 0x1A130D;
        p->tip_bg       = 0x2A1E13;
    }

    /* 组件库（ui.c）的颜色字段同步一份，保持窗口装饰一致。 */
    g_theme.bg           = p->bg;
    g_theme.panel        = p->panel;
    g_theme.panel_alt    = p->panel2;
    g_theme.border       = p->border;
    g_theme.text         = p->text;
    g_theme.text_dim     = p->dim;
    g_theme.accent       = p->accent;
    g_theme.accent_hover = p->accent3;
    g_theme.accent_dim   = p->accent2;
    g_theme.ok           = p->ok;
    g_theme.warn         = p->warn;
    g_theme.err          = p->err;
}

/* ============================================================
 * 界面动画
 * ============================================================ */

static uint32_t g_anim_until = 0;
static uint32_t *g_fade_buf = 0;
static uint32_t  g_fade_cap = 0;
static int       g_fade_x = 0;
static int       g_fade_y = 0;
static int       g_fade_w = 0;
static int       g_fade_h = 0;

int gui_anim_enabled(void) {
    return system_cfg_anim() != 0;
}

void gui_anim_touch(void) {
    if (gui_anim_enabled()) {
        g_anim_until = net_now_ms() + 220u;
    }
}

int gui_anim_active(void) {
    return gui_anim_enabled() && (int32_t)(g_anim_until - net_now_ms()) > 0;
}

uint8_t gui_anim_alpha(uint32_t start_ms, uint32_t dur_ms) {
    if (!gui_anim_enabled() || dur_ms == 0) return 255;
    uint32_t now = net_now_ms();
    if ((int32_t)(now - start_ms) <= 0) return 0;
    uint32_t d = now - start_ms;
    if (d >= dur_ms) return 255;
    return (uint8_t)(d * 255u / dur_ms);
}

uint32_t *gui_anim_begin(ui_rect r) {
    if (!gui_anim_enabled()) return 0;

    int x = r.x, y = r.y, w = r.w, h = r.h;
    if (w <= 0 || h <= 0) return 0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gW) w = gW - x;
    if (y + h > gH) h = gH - y;
    if (w <= 0 || h <= 0) return 0;

    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
    if (bytes > g_fade_cap) {
        if (g_fade_buf) img_free(g_fade_buf);
        g_fade_buf = (uint32_t *)img_alloc(bytes);
        g_fade_cap = g_fade_buf ? bytes : 0;
        if (!g_fade_buf) return 0;
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            g_fade_buf[row * w + col] = vbe_get_pixel((uint16_t)(x + col),
                                                      (uint16_t)(y + row));
        }
    }

    g_fade_x = x;
    g_fade_y = y;
    g_fade_w = w;
    g_fade_h = h;
    return g_fade_buf;
}

void gui_anim_end(uint8_t alpha) {
    if (!g_fade_buf || g_fade_w <= 0 || g_fade_h <= 0) return;
    if (alpha >= 255) return;

    uint32_t a = alpha;
    uint32_t ia = 255u - a;
    int w = g_fade_w;

    for (int row = 0; row < g_fade_h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t dst = vbe_get_pixel((uint16_t)(g_fade_x + col),
                                         (uint16_t)(g_fade_y + row));
            uint32_t src = g_fade_buf[row * w + col];

            uint32_t r = (((dst >> 16) & 0xFFu) * a + ((src >> 16) & 0xFFu) * ia) / 255u;
            uint32_t g = (((dst >> 8) & 0xFFu) * a + ((src >> 8) & 0xFFu) * ia) / 255u;
            uint32_t b = ((dst & 0xFFu) * a + (src & 0xFFu) * ia) / 255u;

            vbe_put_pixel((uint16_t)(g_fade_x + col), (uint16_t)(g_fade_y + row),
                          (r << 16) | (g << 8) | b);
        }
    }
}

/* Slyz 应用取主题色：槽位编号见 slyz.h 的 SLE_THEME_*。 */
uint32_t gui_theme_color(int slot) {
    const struct GuiPalette *p = &g_pal;
    switch (slot) {
        case SLE_THEME_BG:       return p->bg;
        case SLE_THEME_BG2:      return p->bg2;
        case SLE_THEME_PANEL:    return p->panel;
        case SLE_THEME_PANEL2:   return p->panel2;
        case SLE_THEME_PANEL3:   return p->panel3;
        case SLE_THEME_BORDER:   return p->border;
        case SLE_THEME_TEXT:     return p->text;
        case SLE_THEME_DIM:      return p->dim;
        case SLE_THEME_ACCENT:   return p->accent;
        case SLE_THEME_ACCENT2:  return p->accent2;
        case SLE_THEME_ACCENT3:  return p->accent3;
        case SLE_THEME_OK:       return p->ok;
        case SLE_THEME_ERR:      return p->err;
        case SLE_THEME_WARN:     return p->warn;
        case SLE_THEME_HOVER:    return p->hover;
        case SLE_THEME_SEL:      return p->selected;
        case SLE_THEME_WIN_BG:   return p->win_body;
        case SLE_THEME_TITLE:    return p->title;
        case SLE_THEME_MENU_BG:  return p->menu_bg;
        default:                 return 0;
    }
}

int g_mx = 0;
int g_my = 0;
int g_mdown = 0;
int g_mdown_r = 0;
int g_mclicked = 0;
int g_rclicked = 0;
int g_mwheel = 0;
int g_mreleased = 0;
int g_mmoved = 0;

/* ============================================================
 * 字符串与数值工具（内核没有 libc）
 * ============================================================ */

int g_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

int g_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int g_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

int g_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        if (g_lower((unsigned char)*a) != g_lower((unsigned char)*b)) {
            return g_lower((unsigned char)*a) - g_lower((unsigned char)*b);
        }
        a++;
        b++;
    }
    return g_lower((unsigned char)*a) - g_lower((unsigned char)*b);
}

void g_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* 无符号整数转十进制；返回写入长度 */
int g_utoa(uint32_t v, char *out) {
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

void g_itoa_pad(uint32_t v, char *out, int width) {
    char tmp[12];
    int n = g_utoa(v, tmp);
    int pad = width - n;
    int p = 0;
    while (pad-- > 0) out[p++] = ' ';
    for (int i = 0; i < n; i++) out[p++] = tmp[i];
    out[p] = '\0';
}

int g_contains_nocase(const char *haystack, const char *needle) {
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

int g_point_in(const ui_rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

void gui_delay_ms(uint32_t ms) {
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
int g_cursor_x = -1;
int g_cursor_y = -1;

void gui_cursor_draw(int x, int y) {
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

void gui_cursor_erase(void) {
    if (g_cursor_x < 0) return;
    vbe_restore_rect((uint16_t)g_cursor_x, (uint16_t)g_cursor_y,
                     GUI_CURSOR_W, GUI_CURSOR_H, g_cursor_save);
    g_cursor_x = -1;
}

void gui_mouse_update(void) {
    int8_t dx = 0;
    int8_t dy = 0;
    int8_t wheel = 0;
    uint8_t buttons = 0;

    g_mclicked = 0;
    g_rclicked = 0;
    g_mreleased = 0;
    g_mmoved = 0;
    g_mwheel = 0;

    /* 只在第一次进入 GUI 时把光标放到屏幕中央。旧实现用
     * "坐标等于 (0,0)" 判断未初始化，导致用户永远无法把光标移到
     * 左上角（每次到达都会被瞬移回中心），也让自动化测试的
     * 越界定位失效。 */
    static int mouse_pos_init = 0;
    if (!mouse_pos_init) {
        g_mx = gW / 2;
        g_my = gH / 2;
        mouse_pos_init = 1;
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

void gui_kb_begin(void) {
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

void gui_kb_end(void) {
    /* 退出 GUI 前关掉鼠标数据上报。TTY 没有任何鼠标消费者，鼠标包会
     * 持续占满 8042 唯一的输出缓冲，键盘中断随即读不到数据，表现为
     * "从 GUI 退出到终端后键盘完全没反应"。ISR 里也补了丢弃鼠标字节
     * 的兜底，这里主动关闭是为了从源头上不再产生包。 */
    ps2_mouse_disable();
    kb_flush();
    outb(0x21, g_kb_saved_imr);
    g_kb_owned = 0;
}

char gui_kb_read(void) {
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(0x64);
        if ((status & 0x21) != 0x01) break;   /* 没有键盘数据 */

        uint8_t sc = inb(0x60);
        char c = scancode_to_char(sc);
        if (c != 0) return c;
    }
    /* 8042 没有按键时消费 USB 键盘注入的扫描码（内部会轮询 xHCI） */
    return kb_read_char();
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
    /* USB 键盘（含 2.4G 接收器）：注入的扫描码走同一环形缓冲 */
    return kb_read_char();
}

/* ============================================================
 * 基础绘制辅助
 * ============================================================ */

void gui_text_center(const char *s, int cx, int y, uint32_t color) {
    int w = vbe_utf8_width_px(s);
    vbe_draw_utf8_transparent((uint16_t)(cx - w / 2), (uint16_t)y, s, color);
}

/* 截断绘制的文本：max_chars 是"16px 字符格"数。
 * 中文占两格，按显示宽度截断，不会把汉字劈成半个序列。 */
void gui_text_n(int x, int y, const char *s, int max_chars, uint32_t color) {
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
int g_ime_ax = 240;
int g_ime_ay = 220;

/* 布局缓存：每次重绘前按候选文字的实际像素宽度算一遍。
 * 1 个字和 2 个字的候选块宽度不同，后面的块必须跟着顺延，
 * 否则左边的 72px 固定框会把"世界"这种双字词挤出去。 */
static int  g_ime_block_x[IME_PAGE_SIZE];
static int  g_ime_block_w[IME_PAGE_SIZE];
static char g_ime_block_text[IME_PAGE_SIZE][16];
static int  g_ime_block_count;
static int  g_ime_panel_w;

int gui_ime_active(void) {
    return ime_chinese_enabled() && ime_composing()[0] != '\0';
}

void gui_ime_layout(void) {
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

ui_rect gui_ime_rect(void) {
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

void gui_ime_render(void) {
    if (!gui_ime_active()) return;
    ui_rect p = gui_ime_rect();

    /* 用两层圆角叠出边框，不额外依赖描边接口 */
    ui_round_fill((ui_rect){ p.x - 2, p.y - 2, p.w + 4, p.h + 4 }, 15, C_BORDER);
    ui_round_fill(p, 13, C_MENU_BG);

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
int gui_text_wrap(int x, int y, int max_w, const char *s,
                         uint32_t color, int max_lines) {
    return ui_text_wrap(x, y, max_w, 34, s, color, max_lines);
}

void gui_ring(int cx, int cy, int radius, int thickness, uint32_t color) {
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

void gui_avatar(int cx, int cy, int radius, const char *name, uint32_t c1, uint32_t c2) {
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
ui_rect g_hunt_prev[GUI_HUNT_MAX];
int g_hunt_prev_count = 0;

void hunt_reset(void) {
    g_hunt_count = 0;
}

void hunt_add(ui_rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    for (int i = 0; i < g_hunt_count; i++) {
        const ui_rect *e = &g_hunt[i];
        if (e->x == r.x && e->y == r.y && e->w == r.w && e->h == r.h) return;
    }
    if (g_hunt_count >= GUI_HUNT_MAX) return;

    g_hunt[g_hunt_count++] = r;
}

void hunt_snapshot(void) {
    g_hunt_prev_count = g_hunt_count;
    for (int i = 0; i < g_hunt_count; i++) {
        g_hunt_prev[i] = g_hunt[i];
    }
}

void gui_button(ui_rect r, const char *label, int hover, int primary) {
    hunt_add(r);

    if (primary) {
        ui_round_gradient(r, 12,
                          hover ? C_ACCENT3 : C_ACCENT,
                          hover ? C_ACCENT : C_ACCENT2);
    } else {
        ui_round_panel(r, 12, hover ? C_ACCENT : C_BORDER,
                       hover ? C_HOVER : C_PANEL2);
    }

    int tw = vbe_utf8_width_px(label);
    vbe_draw_utf8_transparent((uint16_t)(r.x + (r.w - tw) / 2),
                              (uint16_t)(r.y + (r.h - 32) / 2),
                              label, primary ? 0xFFFFFF : C_TEXT);
}

/* ---------- 图标（全部几何绘制，不依赖图片资源） ---------- */

void icon_folder(int x, int y, int s, uint32_t color) {
    int tab_w = s / 2;
    int tab_h = s / 5;
    ui_round_fill((ui_rect){ x, y + 2, tab_w, tab_h + 4 }, 3, color);
    ui_round_fill((ui_rect){ x, y + tab_h, s, s - tab_h - 2 }, 4, color);
    ui_hline(x + 4, y + tab_h + 4, s - 8, 0xFFFFFF);
}

void icon_file(int x, int y, int s, uint32_t color) {
    int fold = s / 4;
    ui_round_fill((ui_rect){ x + 2, y + 2, s - 4, s - 4 }, 4, color);
    /* 折角 */
    for (int i = 0; i < fold; i++) {
        ui_hline(x + s - 2 - fold + i, y + 2 + i, fold - i, C_PANEL2);
    }
    for (int k = 0; k < 3; k++) {
        ui_hline(x + 6, y + s / 2 + k * 4, s - 14, C_PANEL2);
    }
}

void icon_editor(int x, int y, int s, uint32_t color) {
    /* 文档 + 一支笔 */
    ui_round_fill((ui_rect){ x + 2, y + 2, s - 8, s - 4 }, 4, color);
    for (int k = 0; k < 3; k++) {
        ui_hline(x + 7, y + 7 + k * 5, s - 16, C_PANEL2);
    }
    int px = x + s - 9;
    int py = y + s - 10;
    for (int k = 0; k < 7; k++) {
        ui_line(px + k, py - k, px + k + 3, py - k + 3, C_WARN);
    }
}

void icon_search(int cx, int cy, int radius, uint32_t color) {
    gui_ring(cx, cy, radius, 3, color);
    for (int k = 0; k < radius; k++) {
        ui_line(cx + radius - 2 + k / 2, cy + radius - 2 + k / 2,
                cx + radius + 2 + k / 2, cy + radius + 2 + k / 2, color);
    }
}

void icon_settings(int x, int y, int s, uint32_t color) {
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

void icon_power(int cx, int cy, int radius, uint32_t color) {
    gui_ring(cx, cy, radius, 3, color);
    /* 经典电源符号：圆环 + 从顶部伸向圆心的竖线。
     * 竖线起点必须离开圆环内侧几个像素，留出"缺口"；
     * 直接从 cy-radius-3 画会把环顶盖住，看起来圆和竖连成一体。 */
    int gap = 4;
    vbe_fill_rect((uint16_t)(cx - 1), (uint16_t)(cy - radius + gap),
                  3, (uint16_t)(radius / 2 + 2), color);
}

void icon_terminal(int x, int y, int s, uint32_t color) {
    ui_round_panel((ui_rect){ x, y, s, s }, 5, color, C_PANEL2);
    ui_line(x + 6, y + 8, x + 11, y + s / 2, color);
    ui_line(x + 11, y + s / 2, x + 6, y + s - 8, color);
    ui_hline(x + 14, y + s - 9, s - 20, color);
}

void icon_globe(int x, int y, int s, uint32_t color) {
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

void icon_chart(int x, int y, int s, uint32_t color) {
    /* 任务管理器：面板 + 三根递增的柱状条 */
    int base = y + s - 5;
    ui_round_panel((ui_rect){ x + 2, y + 2, s - 4, s - 4 }, 4, color, C_PANEL2);

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

void icon_fault(int x, int y, int s, uint32_t color) {
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

void icon_calc(int x, int y, int s, uint32_t color) {
    /* 计算器：外框 + 顶部显示屏 + 2x2 按键 */
    ui_round_panel((ui_rect){ x, y, s, s }, 4, color, C_PANEL2);

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

void icon_net(int x, int y, int s, uint32_t color) {
    /* 控制中心：三条滑杆 + 旋钮 */
    for (int i = 0; i < 3; i++) {
        int cy = y + 6 + i * ((s - 12) / 2);
        ui_hline(x + 4, cy, s - 8, color);
        int kx = x + 5 + ((i * 11 + 7) % (s - 12));
        ui_round_fill((ui_rect){ kx - 3, cy - 3, 7, 7 }, 3, color);
    }
}

void icon_close(int x, int y, int s, uint32_t color) {
    for (int k = 0; k < 2; k++) {
        ui_line(x + 5 + k, y + 5, x + s - 5 + k, y + s - 5, color);
        ui_line(x + s - 5 + k, y + 5, x + 5 + k, y + s - 5, color);
    }
}

void icon_minimize(int x, int y, int s, uint32_t color) {
    vbe_fill_rect((uint16_t)(x + 4), (uint16_t)(y + s - 8),
                  (uint16_t)(s - 8), 2, color);
}

void icon_maximize(int x, int y, int s, uint32_t color) {
    ui_hline(x + 4, y + 4, s - 8, color);
    ui_hline(x + 4, y + s - 5, s - 8, color);
    ui_vline(x + 4, y + 4, s - 8, color);
    ui_vline(x + s - 5, y + 4, s - 8, color);
}

void icon_restore(int x, int y, int s, uint32_t color) {
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

/* ---- 壁纸离屏缓存 ----
 * 大尺寸 JPEG 首次解码后先缩放到屏幕尺寸，把结果留在像素缓冲里。
 * 之后每次重绘（启动画面、登录页、窗口拖动、悬停）只按当前裁剪
 * 矩形拷贝像素，不再重复执行上百万次采样——这是输入延迟与启动
 * 阶段卡顿的主要来源。 */
static uint32_t *g_wall_px = 0;
static int g_wall_w = 0;
static int g_wall_h = 0;
static char g_wall_src[128];

static void wall_cache_drop(void) {
    if (g_wall_px) {
        image_free_buffer(g_wall_px);
        g_wall_px = 0;
    }
    g_wall_w = 0;
    g_wall_h = 0;
    g_wall_src[0] = '\0';
}

static int wall_cache_ensure(const char *path, int handle) {
    if (g_wall_px && g_wall_w == gW && g_wall_h == gH &&
        g_strcmp(g_wall_src, path) == 0) {
        return 1;
    }

    wall_cache_drop();

    uint32_t bytes = (uint32_t)gW * (uint32_t)gH * 4u;
    g_wall_px = (uint32_t *)image_alloc_buffer(bytes);
    if (!g_wall_px) return 0;

    if (!image_scale_to_buffer(handle, g_wall_px, gW, gH, C_BG)) {
        wall_cache_drop();
        return 0;
    }

    g_wall_w = gW;
    g_wall_h = gH;
    g_copy(g_wall_src, path, (int)sizeof(g_wall_src));
    return 1;
}

void gui_background(void) {
    /* 配置了壁纸就直接拉伸铺满屏幕；绘制遵守当前裁剪矩形，
     * 因此局部重绘只解码一次缓存并只写脏矩形覆盖的像素。 */
    const char *wallpaper = system_cfg_wallpaper();
    if (wallpaper && wallpaper[0]) {
        int handle = image_load(wallpaper);
        if (handle >= 0) {
            if (wall_cache_ensure(wallpaper, handle)) {
                vbe_blit_argb(g_wall_px, gW, 0, 0, gW, gH);
            } else {
                image_draw_scaled(handle, 0, 0, gW, gH);
            }

            ui_rect strip = { 0, gH - 3, gW, 3 };
            ui_gradient_v(strip, C_ACCENT2, C_BG2);
            return;
        }
    }

    int grad_h = gH / 3;

    /* 上 1/3：垂直渐变，逐行颜色不同，保留串行实现 */
    ui_rect top = { 0, 0, gW, grad_h };
    ui_gradient_v(top, C_BG2, C_BG);

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
        ui_vline(x, 0, gH, C_BORDER);
    }
    for (int y = 0; y < gH; y += 64) {
        if (clipped && (y < cy || y >= cy + ch)) continue;
        ui_hline(GUI_TASKBAR_W, y, gW - GUI_TASKBAR_W, C_BORDER);
    }

    /* 底部一条很淡的强调色光带，避免大面积纯色显得空 */
    ui_rect strip = { 0, gH - 3, gW, 3 };
    ui_gradient_v(strip, C_ACCENT2, C_BG2);
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
int rtc_get_utc(int *hour, int *minute, int *second) {
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

/* 读取 RTC 日期。year 返回 2000+，weekday 0=Sunday。 */
int rtc_get_date(int *year, int *month, int *day, int *weekday) {
    if (!rtc_wait_ready()) return 0;

    uint8_t reg_b = rtc_reg_read(0x0B);
    int bcd = !(reg_b & 0x04);

    uint8_t y = rtc_reg_read(0x09);
    uint8_t mo = rtc_reg_read(0x08);
    uint8_t d = rtc_reg_read(0x07);
    uint8_t wd = rtc_reg_read(0x06);

    if (bcd) {
        y = (uint8_t)rtc_bcd_to_bin(y);
        mo = (uint8_t)rtc_bcd_to_bin(mo);
        d = (uint8_t)rtc_bcd_to_bin(d);
        wd = (uint8_t)rtc_bcd_to_bin(wd);
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;

    if (year) *year = 2000 + y;
    if (month) *month = mo;
    if (day) *day = d;
    if (weekday) *weekday = wd % 7;
    return 1;
}

/* 写入 UTC 时分（秒保持不变）。返回 1 成功。 */
int rtc_set_utc(int hour, int minute) {
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
void gui_clock_local(int *hour, int *minute) {
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

void gui_clock_text(char out[6]) {
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
void gui_tz_text(char *out, int cap) {
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