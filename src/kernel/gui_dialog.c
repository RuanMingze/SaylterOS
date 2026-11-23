/* ============================================================
 * gui_dialog.c - 桌面模态对话框
 * ------------------------------------------------------------
 * PrtSc 截图保存：
 *   1. 按下 PrtSc 的瞬间把 backbuffer 整屏复制到内存（之后弹出的
 *      对话框不会被拍进截图）；
 *   2. 弹出文件浏览器，可以逐级进入目录、编辑文件名；
 *   3. Save 时把内存像素编码成 24 位 BMP 写入 SlyzFS。
 *
 * 对话框是模态的：打开期间桌面点击只走 gui_dialog_click，键盘只走
 * gui_dialog_key，其他窗口不会收到输入。
 * ============================================================ */

#include <stdint.h>

#include "gui_internal.h"
#include "imgcodec.h"
#include "vbe.h"
#include "mem.h"

/* SlyzFS 目录项镜像：必须与 slyzfs_bridge.cpp 的 SlyzDirEntry
 * 逐字节一致（其它模块各有一份同样的定义）。 */
#pragma pack(push, 1)
struct GuiDlgDirEntry {
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

#define DLG_DIR_FILE 0x01
#define DLG_DIR_DIR  0x02

extern int   slyzfs_list_dir(const char *path, struct GuiDlgDirEntry *out, int out_max);
extern int   slyzfs_stat(const char *path, struct GuiDlgDirEntry *out);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);
extern int   slyzfs_mkdir(const char *path);

#define DLG_KIND_NONE  0
#define DLG_KIND_SHOT  1

#define DLG_ITEM_MAX   64
#define DLG_ROWS       6
#define DLG_ROW_H      44

struct DlgItem {
    char name[72];
    int  is_dir;
    uint32_t size;
};

static int  g_dlg_kind = DLG_KIND_NONE;
static char g_dlg_dir[192];
static char g_dlg_name[72];
static int  g_dlg_name_len = 0;
static int  g_dlg_edit = 1;
static int  g_dlg_sel = -1;
static int  g_dlg_off = 0;
static char g_dlg_msg[80];
static uint32_t g_dlg_t0 = 0;
static uint32_t g_dlg_close_t0 = 0;
static int  g_dlg_closing = 0;

static struct DlgItem g_dlg_items[DLG_ITEM_MAX];
static int g_dlg_count = 0;

/* 截图原始像素（0xAARRGGBB，自上而下） */
static uint32_t *g_shot_px = 0;
static int g_shot_w = 0;
static int g_shot_h = 0;

static char    g_toast[96];
static uint32_t g_toast_until = 0;
static int     g_toast_visible = 0;

/* ============================================================
 * 小工具
 * ============================================================ */

static int dlg_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void dlg_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int dlg_ends_with(const char *s, const char *suffix) {
    int n = dlg_strlen(s);
    int m = dlg_strlen(suffix);
    if (m > n) return 0;
    for (int i = 0; i < m; i++) {
        if (g_lower((unsigned char)s[n - m + i]) !=
            g_lower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

static void dlg_join(char *out, int cap, const char *dir, const char *name) {
    out[0] = '\0';
    if (dir && dir[0]) {
        g_copy(out, dir, cap);
        int n = dlg_strlen(out);
        if (n > 0 && out[n - 1] != '\\' && n < cap - 1) {
            out[n++] = '\\';
            out[n] = '\0';
        }
        g_copy(out + dlg_strlen(out), name ? name : "",
               cap - dlg_strlen(out));
    } else {
        g_copy(out, name ? name : "", cap);
    }
}

static void dlg_parent(char *out, int cap, const char *dir) {
    if (!dir || !dir[0]) { out[0] = '\0'; return; }
    int n = dlg_strlen(dir);
    int i = n - 1;
    while (i > 0 && dir[i] != '\\') i--;
    if (i <= 0) { out[0] = '\0'; return; }
    g_copy(out, dir, i + 1);
    out[i] = '\0';
}

void gui_toast(const char *text, uint32_t ms) {
    dlg_copy(g_toast, text, (int)sizeof(g_toast));
    g_toast_until = net_now_ms() + ms;
}

int gui_toast_active(void) {
    return (int32_t)(g_toast_until - net_now_ms()) > 0 && g_toast[0];
}

void gui_toast_render(void) {
    if (!gui_toast_active()) {
        /* 提示过期后必须让整帧重绘一次，否则局部脏矩形不会擦掉
         * 已经画在 backbuffer 上的提示条，它会一直留在屏幕上。 */
        if (g_toast_visible) {
            g_toast_visible = 0;
            dirty_all();
        }
        return;
    }

    int tw = ui_text_width(g_toast);
    int w = tw + 40;
    int h = 48;
    if (w > gW - GUI_TASKBAR_W - 32) w = gW - GUI_TASKBAR_W - 32;
    int x = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W - w) / 2;
    int y = gH - h - 28;

    ui_shadow((ui_rect){ x, y, w, h }, 14, 8);
    ui_round_panel((ui_rect){ x, y, w, h }, 14, C_ACCENT, C_TIP_BG);
    gui_text_n(x + 18, y + 8, g_toast, (w - 36) / 16, C_TEXT);
    g_toast_visible = 1;
}

/* ============================================================
 * 截图捕获与 BMP 编码
 * ============================================================ */

static void dlg_free_shot(void) {
    if (g_shot_px) {
        img_free(g_shot_px);
        g_shot_px = 0;
    }
    g_shot_w = 0;
    g_shot_h = 0;
}

static int dlg_capture(void) {
    dlg_free_shot();

    uint32_t phys = vbe_backbuffer_phys();
    uint32_t stride = vbe_backbuffer_stride();
    int w = gW;
    int h = gH;
    if (!phys || stride == 0 || w <= 0 || h <= 0) return 0;

    uint32_t *px = (uint32_t *)img_alloc((uint32_t)w * (uint32_t)h * 4u);
    if (!px) return 0;

    const uint8_t *base = (const uint8_t *)(uintptr_t)phys;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = (const uint32_t *)(base + (uint32_t)y * stride);
        uint32_t *dst = px + (uint32_t)y * (uint32_t)w;
        for (int x = 0; x < w; x++) {
            dst[x] = src[x] | 0xFF000000u;
        }
    }

    g_shot_px = px;
    g_shot_w = w;
    g_shot_h = h;
    return 1;
}

static void dlg_put_le16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void dlg_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int dlg_write_bmp(const char *path) {
    if (!g_shot_px || g_shot_w <= 0 || g_shot_h <= 0) return 0;

    int w = g_shot_w;
    int h = g_shot_h;
    uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u;
    uint32_t data = row * (uint32_t)h;
    uint32_t total = 54u + data;

    uint8_t *buf = (uint8_t *)img_alloc(total);
    if (!buf) return 0;

    buf[0] = 'B';
    buf[1] = 'M';
    dlg_put_le32(buf + 2, total);
    dlg_put_le32(buf + 6, 0);
    dlg_put_le32(buf + 10, 54);
    dlg_put_le32(buf + 14, 40);
    dlg_put_le32(buf + 18, (uint32_t)w);
    dlg_put_le32(buf + 22, (uint32_t)h);
    dlg_put_le16(buf + 26, 1);
    dlg_put_le16(buf + 28, 24);
    dlg_put_le32(buf + 30, 0);
    dlg_put_le32(buf + 34, data);
    dlg_put_le32(buf + 38, 2835);
    dlg_put_le32(buf + 42, 2835);
    dlg_put_le32(buf + 46, 0);
    dlg_put_le32(buf + 50, 0);

    for (int y = 0; y < h; y++) {
        int sy = h - 1 - y;     /* BMP 行序自下而上 */
        const uint32_t *src = g_shot_px + (uint32_t)sy * (uint32_t)w;
        uint8_t *dst = buf + 54u + (uint32_t)y * row;
        for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            dst[x * 3 + 0] = (uint8_t)(c & 0xFF);          /* B */
            dst[x * 3 + 1] = (uint8_t)((c >> 8) & 0xFF);   /* G */
            dst[x * 3 + 2] = (uint8_t)((c >> 16) & 0xFF);  /* R */
        }
    }

    void *fh = slyzfs_open(path, "w");
    if (!fh) {
        img_free(buf);
        return 0;
    }

    int written = 0;
    int ok = 1;
    while (written < (int)total) {
        int chunk = (int)total - written;
        if (chunk > 32768) chunk = 32768;
        int n = slyzfs_write(fh, buf + written, chunk);
        if (n <= 0) { ok = 0; break; }
        written += n;
    }
    slyzfs_close(fh);
    img_free(buf);
    return ok && written == (int)total;
}

/* ============================================================
 * 目录浏览
 * ============================================================ */

static void dlg_scan(void) {
    static struct GuiDlgDirEntry ents[64];
    g_dlg_count = 0;

    /* ".." 永远排在第一行（根目录除外） */
    if (g_dlg_dir[0]) {
        dlg_copy(g_dlg_items[g_dlg_count].name, "..",
                 (int)sizeof(g_dlg_items[0].name));
        g_dlg_items[g_dlg_count].is_dir = 1;
        g_dlg_items[g_dlg_count].size = 0;
        g_dlg_count++;
    }

    int n = slyzfs_list_dir(g_dlg_dir, ents, 64);
    if (n < 0) n = 0;

    for (int i = 0; i < n && g_dlg_count < DLG_ITEM_MAX; i++) {
        const char *name = ents[i].name;
        int is_dir = (ents[i].type == DLG_DIR_DIR);
        if (!is_dir && !dlg_ends_with(name, ".bmp")) {
            /* 保存截图时只显示 BMP，避免目录里塞满无关文件 */
            continue;
        }

        struct DlgItem *it = &g_dlg_items[g_dlg_count++];
        dlg_copy(it->name, name, (int)sizeof(it->name));
        it->is_dir = is_dir;
        it->size = ents[i].size;
    }

    if (g_dlg_off > g_dlg_count - DLG_ROWS) g_dlg_off = g_dlg_count - DLG_ROWS;
    if (g_dlg_off < 0) g_dlg_off = 0;
    if (g_dlg_sel >= g_dlg_count) g_dlg_sel = g_dlg_count - 1;
}

/* 生成不与现有文件冲突的默认文件名：shot-1.bmp, shot-2.bmp ... */
static void dlg_default_name(void) {
    char probe[256];
    for (int i = 1; i < 9999; i++) {
        char name[32];
        int p = 0;
        const char *prefix = "shot-";
        while (*prefix && p < (int)sizeof(name) - 8) name[p++] = *prefix++;
        char num[8];
        int n = g_utoa((uint32_t)i, num);
        for (int k = 0; k < n && p < (int)sizeof(name) - 5; k++) name[p++] = num[k];
        const char *ext = ".bmp";
        while (*ext && p < (int)sizeof(name) - 1) name[p++] = *ext++;
        name[p] = '\0';

        dlg_join(probe, (int)sizeof(probe), g_dlg_dir, name);
        struct GuiDlgDirEntry ent;
        if (!slyzfs_stat(probe, &ent)) {
            dlg_copy(g_dlg_name, name, (int)sizeof(g_dlg_name));
            g_dlg_name_len = dlg_strlen(g_dlg_name);
            return;
        }
    }
    dlg_copy(g_dlg_name, "shot.bmp", (int)sizeof(g_dlg_name));
    g_dlg_name_len = dlg_strlen(g_dlg_name);
}

/* ============================================================
 * 打开 / 关闭
 * ============================================================ */

void gui_dialog_open_screenshot(void) {
    if (g_dlg_kind != DLG_KIND_NONE) {
        /* 已有一个对话框：重新抓一张，替换旧截图 */
        dlg_free_shot();
    }

    if (!dlg_capture()) {
        gui_toast("Cannot capture the screen", 2600);
        return;
    }

    const struct AuthUser *u = auth_current();
    dlg_copy(g_dlg_dir, (u && u->home[0]) ? u->home : "", (int)sizeof(g_dlg_dir));

    /* 家目录可能不存在（新用户），退回到根目录继续可用 */
    struct GuiDlgDirEntry ent;
    if (g_dlg_dir[0] && !slyzfs_stat(g_dlg_dir, &ent)) {
        g_dlg_dir[0] = '\0';
    }

    g_dlg_kind = DLG_KIND_SHOT;
    g_dlg_closing = 0;
    g_dlg_close_t0 = 0;
    g_dlg_t0 = net_now_ms();
    g_dlg_edit = 1;
    g_dlg_sel = -1;
    g_dlg_msg[0] = '\0';
    dlg_scan();
    dlg_default_name();
    gui_anim_touch();
    dirty_all();
}

int gui_dialog_active(void) {
    return g_dlg_kind != DLG_KIND_NONE;
}

static void dlg_finish_close(void) {
    g_dlg_kind = DLG_KIND_NONE;
    g_dlg_closing = 0;
    g_dlg_close_t0 = 0;
    g_dlg_items[0].name[0] = '\0';
    g_dlg_count = 0;
    dlg_free_shot();
    dirty_all();
}

static void dlg_request_close(void) {
    if (!gui_anim_enabled()) {
        dlg_finish_close();
        return;
    }
    if (!g_dlg_closing) {
        g_dlg_closing = 1;
        g_dlg_close_t0 = net_now_ms();
        gui_anim_touch();
    }
}

/* ============================================================
 * 布局与绘制
 * ============================================================ */

static ui_rect dlg_card(void) {
    ui_rect c;
    c.w = 700;
    c.h = 540;
    int avail_w = gW - GUI_TASKBAR_W - 24;
    int avail_h = gH - 24;
    if (c.w > avail_w) c.w = avail_w;
    if (c.h > avail_h) c.h = avail_h;
    c.x = GUI_TASKBAR_W + (gW - GUI_TASKBAR_W - c.w) / 2;
    c.y = (gH - c.h) / 2;
    return c;
}

static ui_rect dlg_list_rect(const ui_rect *c) {
    return (ui_rect){ c->x + 24, c->y + 108, c->w - 48, DLG_ROWS * DLG_ROW_H };
}

static ui_rect dlg_name_rect(const ui_rect *c) {
    return (ui_rect){ c->x + 24, c->y + 380, c->w - 48, 44 };
}

static ui_rect dlg_save_rect(const ui_rect *c) {
    return (ui_rect){ c->x + c->w - 2 * 150 - 24, c->y + c->h - 64, 150, 44 };
}

static ui_rect dlg_cancel_rect(const ui_rect *c) {
    return (ui_rect){ c->x + c->w - 150 - 24, c->y + c->h - 64, 140, 44 };
}

static void dlg_render_content(const ui_rect *c) {
    ui_shadow(*c, 18, 12);
    ui_round_panel(*c, 18, C_BORDER, C_MENU_BG);

    ui_text_transparent(c->x + 24, c->y + 14, "SAVE SCREENSHOT", C_DIM);

    /* 当前目录 */
    ui_rect path = { c->x + 24, c->y + 56, c->w - 48, 40 };
    ui_round_panel(path, 10, C_BORDER, C_PANEL2);
    const char *shown = g_dlg_dir[0] ? g_dlg_dir : "\\";
    gui_text_n(path.x + 12, path.y + 4, shown, (path.w - 24) / 16, C_TEXT);

    /* 文件列表 */
    ui_rect list = dlg_list_rect(c);
    ui_round_panel(list, 12, C_BORDER, C_PANEL);

    for (int i = 0; i < DLG_ROWS; i++) {
        int idx = g_dlg_off + i;
        ui_rect row = { list.x + 6, list.y + 6 + i * DLG_ROW_H,
                        list.w - 12, DLG_ROW_H - 6 };
        if (idx >= g_dlg_count) break;

        struct DlgItem *it = &g_dlg_items[idx];
        int hover = g_point_in(&row, g_mx, g_my);
        if (idx == g_dlg_sel || hover) {
            ui_round_fill(row, 8, idx == g_dlg_sel ? C_SEL : C_HOVER);
        }

        if (it->is_dir) {
            icon_folder(row.x + 12, row.y + 6, 22, C_ACCENT3);
        } else {
            icon_file(row.x + 12, row.y + 6, 22, C_DIM);
        }
        gui_text_n(row.x + 46, row.y + 2, it->name, (row.w - 60) / 16,
                   it->is_dir ? C_TEXT : C_DIM);
    }

    if (g_dlg_count == 0) {
        ui_text_transparent(list.x + 16, list.y + 12,
                            "No BMP file or folder here.", C_DIM);
    }

    /* 文件名 */
    ui_text_transparent(c->x + 24, c->y + 388, "NAME", C_DIM);
    ui_rect nb = dlg_name_rect(c);
    nb.x += 90;
    nb.w -= 90;
    ui_round_panel(nb, 10, g_dlg_edit ? C_ACCENT : C_BORDER, C_PANEL);
    gui_text_n(nb.x + 12, nb.y + 6, g_dlg_name, (nb.w - 24) / 16, C_TEXT);
    if (g_dlg_edit && ((net_now_ms() / 500u) & 1u) == 0) {
        int cx = nb.x + 12 + g_dlg_name_len * 16;
        if (cx < nb.x + nb.w - 8) {
            vbe_fill_rect((uint16_t)cx, (uint16_t)(nb.y + 9), 2, 26, C_ACCENT);
        }
    }

    /* 提示 / 错误信息：同一行，错误优先显示 */
    if (g_dlg_msg[0]) {
        gui_text_n(c->x + 24, c->y + 432, g_dlg_msg, (c->w - 48) / 16, C_WARN);
    } else {
        ui_text_transparent(c->x + 24, c->y + 432,
                            "Enter = save    Esc = cancel", C_DIM);
    }

    ui_rect save_btn = dlg_save_rect(c);
    ui_rect cancel_btn = dlg_cancel_rect(c);
    gui_button(save_btn, "Save", g_point_in(&save_btn, g_mx, g_my), 1);
    gui_button(cancel_btn, "Cancel", g_point_in(&cancel_btn, g_mx, g_my), 0);
}

void gui_dialog_render(void) {
    if (g_dlg_kind == DLG_KIND_NONE) return;

    uint32_t *under = 0;
    ui_rect c = dlg_card();

    if (g_dlg_closing) {
        uint32_t elapsed = net_now_ms() - g_dlg_close_t0;
        if (elapsed >= 150 || !gui_anim_enabled()) {
            dlg_finish_close();
            return;
        }
        uint8_t alpha = (uint8_t)(255u - elapsed * 255u / 150u);
        under = gui_anim_begin(c);
        dlg_render_content(&c);
        if (under) gui_anim_end(alpha);
        gui_anim_touch();
        return;
    }

    under = gui_anim_begin(c);
    dlg_render_content(&c);
    if (under) {
        gui_anim_end(gui_anim_alpha(g_dlg_t0, 150));
        if (gui_anim_active()) gui_anim_touch();
    }
}

/* ============================================================
 * 交互
 * ============================================================ */

static void dlg_enter_dir(const char *name) {
    char next[192];
    if (g_strcmp(name, "..") == 0) {
        dlg_parent(next, (int)sizeof(next), g_dlg_dir);
    } else {
        dlg_join(next, (int)sizeof(next), g_dlg_dir, name);
    }
    dlg_copy(g_dlg_dir, next, (int)sizeof(g_dlg_dir));
    g_dlg_off = 0;
    g_dlg_sel = -1;
    dlg_scan();
    dirty_all();
}

static void dlg_do_save(void) {
    if (g_dlg_name_len == 0) {
        dlg_copy(g_dlg_msg, "File name is empty", (int)sizeof(g_dlg_msg));
        return;
    }

    char path[320];
    dlg_join(path, (int)sizeof(path), g_dlg_dir, g_dlg_name);

    if (dlg_write_bmp(path)) {
        char note[96];
        dlg_copy(note, "Saved: ", (int)sizeof(note));
        dlg_copy(note + 7, path, (int)sizeof(note) - 7);
        gui_toast(note, 3200);
        dlg_request_close();
    } else {
        dlg_copy(g_dlg_msg, "Cannot write the file", (int)sizeof(g_dlg_msg));
        dirty_all();
    }
}

void gui_dialog_click(int x, int y) {
    if (g_dlg_kind == DLG_KIND_NONE) return;
    if (g_dlg_closing) return;

    ui_rect c = dlg_card();
    if (!g_point_in(&c, x, y)) return;   /* 模态：外部点击忽略 */

    ui_rect save = dlg_save_rect(&c);
    ui_rect cancel = dlg_cancel_rect(&c);
    if (g_point_in(&save, x, y)) { dlg_do_save(); return; }
    if (g_point_in(&cancel, x, y)) { dlg_request_close(); return; }

    ui_rect nb = dlg_name_rect(&c);
    nb.x += 90;
    nb.w -= 90;
    if (g_point_in(&nb, x, y)) { g_dlg_edit = 1; return; }

    ui_rect list = dlg_list_rect(&c);
    if (g_point_in(&list, x, y)) {
        int idx = g_dlg_off + (y - list.y - 6) / DLG_ROW_H;
        if (idx < 0 || idx >= g_dlg_count) return;
        g_dlg_sel = idx;
        if (g_dlg_items[idx].is_dir) {
            dlg_enter_dir(g_dlg_items[idx].name);
        } else {
            dlg_copy(g_dlg_name, g_dlg_items[idx].name, (int)sizeof(g_dlg_name));
            g_dlg_name_len = dlg_strlen(g_dlg_name);
            g_dlg_edit = 1;
        }
        dirty_all();
    }
}

void gui_dialog_key(char key) {
    if (g_dlg_kind == DLG_KIND_NONE || g_dlg_closing) return;

    if (key == 27) {
        dlg_request_close();
        return;
    }
    if (key == '\r' || key == '\n') {
        dlg_do_save();
        return;
    }
    if (key == '\b' || key == 127) {
        if (g_dlg_name_len > 0) {
            g_dlg_name_len--;
            g_dlg_name[g_dlg_name_len] = '\0';
        }
        dirty_all();
        return;
    }
    if (key == (char)0xF6) {   /* up */
        if (g_dlg_sel < 0) g_dlg_sel = 0;
        else if (g_dlg_sel > 0) g_dlg_sel--;
        if (g_dlg_sel < g_dlg_off) g_dlg_off = g_dlg_sel;
        dirty_all();
        return;
    }
    if (key == (char)0xF7) {   /* down */
        if (g_dlg_sel < g_dlg_count - 1) g_dlg_sel++;
        if (g_dlg_sel >= g_dlg_off + DLG_ROWS) g_dlg_off = g_dlg_sel - DLG_ROWS + 1;
        dirty_all();
        return;
    }
    if ((unsigned char)key >= 32 && (unsigned char)key < 127) {
        if (g_dlg_name_len < (int)sizeof(g_dlg_name) - 1) {
            g_dlg_name[g_dlg_name_len++] = key;
            g_dlg_name[g_dlg_name_len] = '\0';
            dirty_all();
        }
    }
}

void gui_dialog_wheel(int delta) {
    if (g_dlg_kind == DLG_KIND_NONE || g_dlg_closing) return;
    int max_off = g_dlg_count - DLG_ROWS;
    if (max_off < 0) max_off = 0;
    g_dlg_off -= delta;
    if (g_dlg_off < 0) g_dlg_off = 0;
    if (g_dlg_off > max_off) g_dlg_off = max_off;
    dirty_all();
}
