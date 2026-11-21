/* ============================================================
 * gui_slyz.c - SLYZ 桌面应用宿主实现
 * ------------------------------------------------------------
 * 负责四件事：
 *   1. 从 SlyzFS 加载 .slyz 镜像并为窗口创建独立虚拟机；
 *   2. 实现窗口/绘图/事件系统调用，把应用坐标转换为屏幕坐标；
 *   3. 每帧执行有限步指令，gfx.flush 后暂停，保证界面不被
 *      单个应用长期占用；
 *   4. 崩溃隔离：虚拟机返回错误只标记该应用，绘制错误页，
 *      其他窗口与内核继续运行。
 *
 * 裁剪策略：
 *   应用绘制期间用 vbe_push_clip 把绘制范围钉在内容区，
 *   vbe_push_clip 是"直接替换"而不是求交，因此即使桌面正在
 *   做局部脏矩形重绘，应用也能完整画完自己的内容区；外层裁剪
 *   在 vbe_pop_clip 后恢复。
 * ============================================================ */

#include <stdint.h>

#include "gui_slyz.h"
#include "slyzvm.h"
#include "ui.h"
#include "vbe.h"
#include "tty.h"
#include "mem.h"
#include "net.h"
#include "system_cfg.h"
#include "task.h"
#include "image.h"
#include "audio.h"
#include "gui.h"
#include "faultlab.h"
#include "slyz3.h"
#include "slyzui.h"

/* gui.c 每帧统计的 CPU 占用（千分比），由 gui.c 提供的访问器读取 */
extern uint32_t gui_system_cpu_permille(void);

/* shell.c 的命令行执行入口：v3 的 os.exec 复用同一套命令解析 */
extern int shell_exec_line(char *line);

/* 串口诊断输出（shell.c 实现），v3 的 print 系列走这里 */
extern void kserial_putchar(char c);
extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

/* ============================================================
 * SlyzFS 接口（与 shell.c 保持同一份 71 字节目录项定义）
 * ============================================================ */

#pragma pack(push, 1)
struct GuiSlyzDirEntry {
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

#define GUI_SLYZ_DIR_DIR 0x02

extern int   slyzfs_stat(const char *path, struct GuiSlyzDirEntry *out);
extern int   slyzfs_list_dir(const char *path, struct GuiSlyzDirEntry *out, int out_max);
extern int   slyzfs_mkdir(const char *path);
extern int   slyzfs_remove(const char *path);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

/* ============================================================
 * 应用实例
 * ============================================================ */

#define SLYZ_EVT_MAX 32
#define SLYZ_TEXT_MAX 4096

struct SlyzEvent {
    int type;
    int a;
    int b;
    int c;
};

struct GuiSlyzApp {
    int             used;
    int             win;
    int             state;          /* 0 已结束, 1 运行中, 2 崩溃 */
    struct SlyzVm  *vm;
    struct Slyz3Vm *vm3;
    struct SlyzUi  *ui;
    int             is_v3;
    uint8_t        *image;
    int             image_size;
    char            path[128];
    char            error[96];
    char            title[64];

    ui_rect         content;
    uint32_t        epoch;

    struct SlyzEvent evts[SLYZ_EVT_MAX];
    int              evt_head;
    int              evt_tail;

    int             close_requested;
};

static struct GuiSlyzApp g_slyz_apps[GUI_SLYZ_MAX];
static uint32_t g_slyz_epoch = 1;
static struct GuiSlyzApp *g_slyz_current = 0;
static uint8_t g_slyz_text[SLYZ_TEXT_MAX];

/* ============================================================
 * 基础工具
 * ============================================================ */

static int slz_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void slz_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int slz_isqrt(int v) {
    if (v <= 0) return 0;
    int x = v;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

static struct GuiSlyzApp *app_for_win(int win) {
    for (int i = 0; i < GUI_SLYZ_MAX; i++) {
        if (g_slyz_apps[i].used && g_slyz_apps[i].win == win) return &g_slyz_apps[i];
    }
    return 0;
}

static struct GuiSlyzApp *app_for_vm(struct SlyzVm *vm) {
    for (int i = 0; i < GUI_SLYZ_MAX; i++) {
        if (g_slyz_apps[i].used && g_slyz_apps[i].vm == vm) return &g_slyz_apps[i];
    }
    return 0;
}

static struct GuiSlyzApp *app_for_vm3(struct Slyz3Vm *vm) {
    for (int i = 0; i < GUI_SLYZ_MAX; i++) {
        if (g_slyz_apps[i].used && g_slyz_apps[i].vm3 == vm) return &g_slyz_apps[i];
    }
    return 0;
}

static void evt_push(struct GuiSlyzApp *a, int type, int x, int y, int z) {
    int next = (a->evt_tail + 1) % SLYZ_EVT_MAX;
    if (next == a->evt_head) {
        /* 队列满：丢弃最旧事件，保证新输入不被静默吞掉 */
        a->evt_head = (a->evt_head + 1) % SLYZ_EVT_MAX;
    }
    a->evts[a->evt_tail].type = type;
    a->evts[a->evt_tail].a = x;
    a->evts[a->evt_tail].b = y;
    a->evts[a->evt_tail].c = z;
    a->evt_tail = next;
}

static int evt_pop(struct GuiSlyzApp *a, struct SlyzEvent *out) {
    if (a->evt_head == a->evt_tail) return 0;
    *out = a->evts[a->evt_head];
    a->evt_head = (a->evt_head + 1) % SLYZ_EVT_MAX;
    return 1;
}

/* ============================================================
 * 宿主内存回调
 * ============================================================ */

static void *slyz_gui_alloc(uint32_t bytes) {
    return kmalloc(bytes);
}

static void slyz_gui_free(void *ptr) {
    kfree(ptr);
}

static void slyz_gui_log(const char *text) {
    (void)text;
}

/* ============================================================
 * 参数读取
 * ============================================================ */

static int64_t arg_i(struct SlyzVal *args, uint16_t argc, int i) {
    if (i < 0 || i >= (int)argc) return 0;
    if (args[i].type == SLYZ_T_INT) return args[i].u.i;

    int n = 0;
    const char *s = slyz_val_str(&args[i], &n);
    if (!s) return 0;

    int k = 0;
    int neg = 0;
    int64_t v = 0;
    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k < n && (s[k] == '-' || s[k] == '+')) { neg = (s[k] == '-'); k++; }
    while (k < n && s[k] >= '0' && s[k] <= '9') {
        v = v * 10 + (s[k] - '0');
        k++;
    }
    return neg ? -v : v;
}

static const char *arg_s(struct SlyzVal *args, uint16_t argc, int i, int *len) {
    if (i < 0 || i >= (int)argc) {
        if (len) *len = 0;
        return "";
    }
    int n = 0;
    const char *s = slyz_val_str(&args[i], &n);
    if (len) *len = n;
    return s ? s : "";
}

/* ============================================================
 * 绘图：所有坐标都是内容区局部坐标
 * ============================================================ */

static void gfx_fill(struct GuiSlyzApp *a, int x, int y, int w, int h, uint32_t color) {
    ui_rect c = a->content;
    if (w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c.w) w = c.w - x;
    if (y + h > c.h) h = c.h - y;
    if (w <= 0 || h <= 0) return;

    vbe_fill_rect((uint16_t)(c.x + x), (uint16_t)(c.y + y),
                  (uint16_t)w, (uint16_t)h, color);
}

static void gfx_text(struct GuiSlyzApp *a, int x, int y, const char *s, uint32_t color) {
    if (!s) return;
    ui_text_transparent(a->content.x + x, a->content.y + y, s, color);
}

static void gfx_line(struct GuiSlyzApp *a, int x0, int y0, int x1, int y1, uint32_t color) {
    ui_line(a->content.x + x0, a->content.y + y0,
            a->content.x + x1, a->content.y + y1, color);
}

static void gfx_circle(struct GuiSlyzApp *a, int cx, int cy, int r, uint32_t color) {
    if (r <= 0) return;
    if (r > 512) r = 512;

    for (int dy = -r; dy <= r; dy++) {
        int span = slz_isqrt(r * r - dy * dy);
        gfx_fill(a, cx - span, cy + dy, span * 2 + 1, 1, color);
    }
}

/* ============================================================
 * 系统调用实现
 * ============================================================ */

static int fs_read_all(const char *path, struct SlyzVal *ret, struct SlyzVm *vm) {
    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        slyz_ret_nil(ret);
        return 0;
    }

    int total = 0;
    while (total < (int)sizeof(g_slyz_text) - 1) {
        int n = slyzfs_read(fh, g_slyz_text + total,
                            (int)sizeof(g_slyz_text) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);

    g_slyz_text[total] = '\0';
    if (!slyz_ret_str(vm, ret, (const char *)g_slyz_text, total)) return -1;
    return 0;
}

static int fs_write_all(const char *path, const char *text, int len) {
    void *fh = slyzfs_open(path, "w");
    if (!fh) return -1;

    int written = 0;
    while (written < len) {
        int n = slyzfs_write(fh, (const uint8_t *)text + written, len - written);
        if (n <= 0) break;
        written += n;
    }
    slyzfs_close(fh);
    return written == len ? written : -1;
}

static int fs_list_dir(struct SlyzVm *vm, const char *path, struct SlyzVal *ret) {
    static struct GuiSlyzDirEntry entries[64];
    int n = slyzfs_list_dir(path, entries, 64);
    if (n < 0) n = 0;

    if (!slyz_ret_arr(vm, ret, n)) return -1;

    for (int i = 0; i < n; i++) {
        char name[53];
        int k = 0;
        while (k < 52 && entries[i].name[k]) {
            name[k] = (char)entries[i].name[k];
            k++;
        }
        name[k] = '\0';

        struct SlyzVal item;
        if (!slyz_ret_str(vm, &item, name, k)) return -1;
        slyz_arr_set(vm, ret, i, item);
    }
    return 0;
}

/* 目录项三元组：[name, type, size]。
 * 文件管理器需要类型与大小来区分目录、显示尺寸，仅靠名字数组不够。 */
static int fs_entries(struct SlyzVm *vm, const char *path, struct SlyzVal *ret) {
    static struct GuiSlyzDirEntry entries[64];
    int n = slyzfs_list_dir(path, entries, 64);
    if (n < 0) n = 0;

    if (!slyz_ret_arr(vm, ret, n)) return -1;

    for (int i = 0; i < n; i++) {
        char name[53];
        int k = 0;
        while (k < 52 && entries[i].name[k]) {
            name[k] = (char)entries[i].name[k];
            k++;
        }
        name[k] = '\0';

        struct SlyzVal row;
        if (!slyz_ret_arr(vm, &row, 3)) return -1;

        struct SlyzVal item;
        if (!slyz_ret_str(vm, &item, name, k)) return -1;
        slyz_arr_set(vm, &row, 0, item);

        item.type = SLYZ_T_INT;
        item.aux = 0;
        item.u.i = entries[i].type;
        slyz_arr_set(vm, &row, 1, item);

        item.type = SLYZ_T_INT;
        item.aux = 0;
        item.u.i = entries[i].size;
        slyz_arr_set(vm, &row, 2, item);

        slyz_arr_set(vm, ret, i, row);
    }
    return 0;
}

static int cfg_get_value(const char *key, char *out, int cap) {
    out[0] = '\0';

    if (slz_strlen(key) == 9 && key[0] == 'b' && key[1] == 'o') {
        const char *v = system_cfg_boot_gui() ? "gui" : "tty";
        slz_copy(out, v, cap);
        return 1;
    }
    if (slz_strlen(key) == 4 && key[0] == 'l') {
        const char *v = system_cfg_lang() ? "zh" : "en";
        slz_copy(out, v, cap);
        return 1;
    }
    if (slz_strlen(key) == 8 && key[0] == 't') {
        int tz = system_cfg_timezone();
        int n = 0;
        char tmp[8];
        int neg = 0;
        if (tz < 0) { neg = 1; tz = -tz; }
        if (tz == 0) tmp[n++] = '0';
        while (tz > 0) {
            tmp[n++] = (char)('0' + tz % 10);
            tz /= 10;
        }
        int o = 0;
        if (neg && o < cap - 1) out[o++] = '-';
        while (n > 0 && o < cap - 1) out[o++] = tmp[--n];
        out[o] = '\0';
        return 1;
    }
    if (slz_strlen(key) == 9 && key[0] == 'w') {
        slz_copy(out, system_cfg_wallpaper(), cap);
        return 1;
    }
    if (slz_strlen(key) == 6 && key[0] == 'a') {
        slz_copy(out, system_cfg_avatar(), cap);
        return 1;
    }
    return 0;
}

static int cfg_set_value(const char *key, const char *value) {
    if (slz_strlen(key) == 9 && key[0] == 'b' && key[1] == 'o') {
        int gui = (value[0] == 'g') ? 1 : 0;
        return system_cfg_set_boot_gui(gui) ? 0 : -1;
    }
    if (slz_strlen(key) == 4 && key[0] == 'l') {
        int id = (value[0] == 'z') ? 1 : 0;
        return system_cfg_set_lang(id) ? 0 : -1;
    }
    if (slz_strlen(key) == 8 && key[0] == 't') {
        int v = 0;
        int neg = 0;
        int i = 0;
        if (value[0] == '-') { neg = 1; i = 1; }
        while (value[i] >= '0' && value[i] <= '9') {
            v = v * 10 + (value[i] - '0');
            i++;
        }
        return system_cfg_set_timezone(neg ? -v : v) ? 0 : -1;
    }
    if (slz_strlen(key) == 9 && key[0] == 'w') {
        if (!system_cfg_set_wallpaper(value)) return -1;
        /* 壁纸影响整个桌面，不能只重绘设置窗口 */
        gui_request_redraw_all();
        return 0;
    }
    if (slz_strlen(key) == 6 && key[0] == 'a') {
        if (!system_cfg_set_avatar(value)) return -1;
        gui_request_redraw_all();
        return 0;
    }
    return -1;
}

static int gui_sys(struct SlyzVm *vm, uint16_t id,
                   struct SlyzVal *args, uint16_t argc,
                   struct SlyzVal *ret) {
    struct GuiSlyzApp *a = app_for_vm(vm);
    if (!a) {
        slyz_ret_int(ret, -1);
        return 0;
    }

    switch (id) {
        case SLE_SYS_PRINT:
        case SLE_SYS_PRINTLN:
        case SLE_SYS_EPRINT:
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_WIN_OPEN:
            slyz_ret_int(ret, (int64_t)a->win);
            return 0;

        case SLE_SYS_WIN_CLOSE:
            a->close_requested = 1;
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_WIN_SIZE:
            slyz_ret_int(ret, (int64_t)a->content.w);
            return 0;

        case SLE_SYS_WIN_HEIGHT:
            slyz_ret_int(ret, (int64_t)a->content.h);
            return 0;

        case SLE_SYS_WIN_TITLE: {
            int len = 0;
            const char *title = arg_s(args, argc, 1, &len);
            (void)len;
            slz_copy(a->title, title ? title : "", (int)sizeof(a->title));
            slyz_ret_int(ret, 0);
            return 0;
        }

        case SLE_SYS_GFX_CLEAR:
            gfx_fill(a, 0, 0, a->content.w, a->content.h,
                     (uint32_t)arg_i(args, argc, 0));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_RECT:
            gfx_fill(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                     (int)arg_i(args, argc, 2), (int)arg_i(args, argc, 3),
                     (uint32_t)arg_i(args, argc, 4));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_FRAME: {
            int x = (int)arg_i(args, argc, 0);
            int y = (int)arg_i(args, argc, 1);
            int w = (int)arg_i(args, argc, 2);
            int h = (int)arg_i(args, argc, 3);
            uint32_t color = (uint32_t)arg_i(args, argc, 4);
            int t = (int)arg_i(args, argc, 5);
            if (t < 1) t = 1;
            if (t > 8) t = 8;
            gfx_fill(a, x, y, w, t, color);
            gfx_fill(a, x, y + h - t, w, t, color);
            gfx_fill(a, x, y, t, h, color);
            gfx_fill(a, x + w - t, y, t, h, color);
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_GFX_TEXT: {
            int len = 0;
            const char *s = arg_s(args, argc, 2, &len);
            (void)len;
            gfx_text(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                     s, (uint32_t)arg_i(args, argc, 3));
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_GFX_TWIDTH: {
            int len = 0;
            const char *s = arg_s(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)ui_text_width(s));
            return 0;
        }

        case SLE_SYS_GFX_LINE:
            gfx_line(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                     (int)arg_i(args, argc, 2), (int)arg_i(args, argc, 3),
                     (uint32_t)arg_i(args, argc, 4));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_CIRCLE:
            gfx_circle(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                       (int)arg_i(args, argc, 2), (uint32_t)arg_i(args, argc, 3));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_FLUSH:
            /* 一帧绘制完成：让出 CPU，桌面继续处理输入与上屏 */
            slyzvm_request_yield(vm);
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EVT_POLL:
        case SLE_SYS_EVT_WAIT: {
            struct SlyzEvent e;
            if (!evt_pop(a, &e)) {
                slyz_ret_nil(ret);
                return 0;
            }

            if (!slyz_ret_arr(vm, ret, 4)) return -1;
            struct SlyzVal item;

            item.type = SLYZ_T_INT;
            item.aux = 0;
            item.u.i = e.type;
            slyz_arr_set(vm, ret, 0, item);

            item.type = SLYZ_T_INT;
            item.aux = 0;
            item.u.i = e.a;
            slyz_arr_set(vm, ret, 1, item);

            item.type = SLYZ_T_INT;
            item.aux = 0;
            item.u.i = e.b;
            slyz_arr_set(vm, ret, 2, item);

            item.type = SLYZ_T_INT;
            item.aux = 0;
            item.u.i = e.c;
            slyz_arr_set(vm, ret, 3, item);
            return 0;
        }

        case SLE_SYS_FS_READ: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            return fs_read_all(path, ret, vm);
        }

        case SLE_SYS_FS_WRITE: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            int tlen = 0;
            const char *text = arg_s(args, argc, 1, &tlen);
            slyz_ret_int(ret, (int64_t)fs_write_all(path, text, tlen));
            return 0;
        }

        case SLE_SYS_FS_EXISTS: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            struct GuiSlyzDirEntry ent;
            slyz_ret_int(ret, slyzfs_stat(path, &ent) ? 1 : 0);
            return 0;
        }

        case SLE_SYS_FS_LIST: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            return fs_list_dir(vm, path, ret);
        }

        case SLE_SYS_FS_ENTRIES: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            return fs_entries(vm, path, ret);
        }

        case SLE_SYS_CFG_GET: {
            int len = 0;
            const char *key = arg_s(args, argc, 0, &len);
            char value[64];
            if (!cfg_get_value(key, value, (int)sizeof(value))) {
                if (!slyz_ret_str(vm, ret, "", 0)) return -1;
            } else {
                if (!slyz_ret_str(vm, ret, value, slz_strlen(value))) return -1;
            }
            return 0;
        }

        case SLE_SYS_CFG_SET: {
            int len = 0;
            const char *key = arg_s(args, argc, 0, &len);
            const char *value = arg_s(args, argc, 1, 0);
            slyz_ret_int(ret, (int64_t)cfg_set_value(key, value));
            return 0;
        }

        case SLE_SYS_CFG_KEYS: {
            static const char *keys[5] = {
                "boot.mode", "lang", "timezone", "wallpaper", "avatar"
            };
            if (!slyz_ret_arr(vm, ret, 5)) return -1;
            for (int i = 0; i < 5; i++) {
                struct SlyzVal item;
                if (!slyz_ret_str(vm, &item, keys[i], slz_strlen(keys[i]))) return -1;
                slyz_arr_set(vm, ret, i, item);
            }
            return 0;
        }

        case SLE_SYS_TIME_NOW:
            slyz_ret_int(ret, (int64_t)(net_now_ms() / 1000u));
            return 0;

        case SLE_SYS_TIME_MS:
            slyz_ret_int(ret, (int64_t)net_now_ms());
            return 0;

        case SLE_SYS_TIME_TEXT: {
            uint32_t ms = net_now_ms();
            char buf[12];
            int sec = (int)(ms / 1000u) % 86400;
            int h = sec / 3600;
            int m = (sec / 60) % 60;
            int s = sec % 60;
            buf[0] = (char)('0' + (h / 10) % 10);
            buf[1] = (char)('0' + h % 10);
            buf[2] = ':';
            buf[3] = (char)('0' + (m / 10) % 10);
            buf[4] = (char)('0' + m % 10);
            buf[5] = ':';
            buf[6] = (char)('0' + (s / 10) % 10);
            buf[7] = (char)('0' + s % 10);
            buf[8] = '\0';
            if (!slyz_ret_str(vm, ret, buf, 8)) return -1;
            return 0;
        }

        case SLE_SYS_DATE_TEXT: {
            if (!slyz_ret_str(vm, ret, "", 0)) return -1;
            return 0;
        }

        case SLE_SYS_SLEEP:
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_INFO: {
            int key = (int)arg_i(args, argc, 0);
            uint32_t total = 0, used = 0, peak = 0;
            uint32_t allocs = 0, frees = 0, failed = 0;
            mem_stats(&total, &used, &peak, &allocs, &frees, &failed);

            int64_t v = -1;
            if (key == SLE_INFO_MEM_TOTAL) v = (int64_t)total;
            else if (key == SLE_INFO_MEM_USED) v = (int64_t)used;
            else if (key == SLE_INFO_MEM_PEAK) v = (int64_t)peak;
            else if (key == SLE_INFO_MEM_FREE) v = (int64_t)mem_free_bytes();
            else if (key == SLE_INFO_TASKS_ALIVE) v = (int64_t)task_count_alive();
            else if (key == SLE_INFO_TASK_MEM) v = (int64_t)task_total_used();
            else if (key == SLE_INFO_DISK_WRITTEN) v = (int64_t)task_total_disk_written();
            else if (key == SLE_INFO_UPTIME_MS) v = (int64_t)net_now_ms();
            else if (key == SLE_INFO_CPU_PERMILLE) v = (int64_t)gui_system_cpu_permille();
            else if (key == SLE_INFO_TASK_SLOTS) v = (int64_t)TASK_MAX;

            slyz_ret_int(ret, v);
            return 0;
        }

        case SLE_SYS_PROC: {
            int idx = (int)arg_i(args, argc, 0);
            int field = (int)arg_i(args, argc, 1);
            struct Task *t = (idx >= 0 && idx < TASK_MAX) ? task_by_id(idx) : 0;
            if (!t) {
                slyz_ret_int(ret, -1);
                return 0;
            }

            int64_t v = -1;
            if (field == SLE_PROC_ID) v = t->id;
            else if (field == SLE_PROC_STATE) v = t->state;
            else if (field == SLE_PROC_STEPS) v = t->steps;
            else if (field == SLE_PROC_QUOTA) v = (int64_t)t->quota;
            else if (field == SLE_PROC_USED) v = (int64_t)t->used;
            else if (field == SLE_PROC_EXIT) v = t->exit_code;

            slyz_ret_int(ret, v);
            return 0;
        }

        case SLE_SYS_PROC_NAME: {
            int idx = (int)arg_i(args, argc, 0);
            struct Task *t = (idx >= 0 && idx < TASK_MAX) ? task_by_id(idx) : 0;
            if (!t) {
                if (!slyz_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            if (!slyz_ret_str(vm, ret, t->name, slz_strlen(t->name))) return -1;
            return 0;
        }

        case SLE_SYS_IMG_LOAD: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)image_load(path));
            return 0;
        }

        case SLE_SYS_IMG_WIDTH:
            slyz_ret_int(ret, (int64_t)image_width((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_IMG_HEIGHT:
            slyz_ret_int(ret, (int64_t)image_height((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_IMG_DRAW: {
            int h = (int)arg_i(args, argc, 0);
            int x = a->content.x + (int)arg_i(args, argc, 1);
            int y = a->content.y + (int)arg_i(args, argc, 2);
            vbe_push_clip(a->content.x, a->content.y, a->content.w, a->content.h);
            image_draw(h, x, y);
            vbe_pop_clip();
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_IMG_DRAW_SCALED: {
            int h = (int)arg_i(args, argc, 0);
            int x = a->content.x + (int)arg_i(args, argc, 1);
            int y = a->content.y + (int)arg_i(args, argc, 2);
            int w = (int)arg_i(args, argc, 3);
            int hh = (int)arg_i(args, argc, 4);
            vbe_push_clip(a->content.x, a->content.y, a->content.w, a->content.h);
            image_draw_scaled(h, x, y, w, hh);
            vbe_pop_clip();
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_IMG_DRAW_CIRCLE: {
            int h = (int)arg_i(args, argc, 0);
            int cx = a->content.x + (int)arg_i(args, argc, 1);
            int cy = a->content.y + (int)arg_i(args, argc, 2);
            int r = (int)arg_i(args, argc, 3);
            vbe_push_clip(a->content.x, a->content.y, a->content.w, a->content.h);
            image_draw_circle(h, cx, cy, r);
            vbe_pop_clip();
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_IMG_FREE:
            image_free((int)arg_i(args, argc, 0));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_OPEN_UI: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)gui_open_file_path(path));
            return 0;
        }

        case SLE_SYS_MEDIA_REQ: {
            char path[160];
            audio_take_source(path, (int)sizeof(path));
            if (!slyz_ret_str(vm, ret, path, slz_strlen(path))) return -1;
            return 0;
        }

        case SLE_SYS_AUDIO_PLAY: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, &len);
            (void)len;
            slyz_ret_int(ret, (int64_t)audio_play(path));
            return 0;
        }

        case SLE_SYS_AUDIO_STOP:
            audio_stop();
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_AUDIO_PAUSE:
            audio_pause((int)arg_i(args, argc, 0));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_AUDIO_STATE:
            slyz_ret_int(ret, (int64_t)audio_state());
            return 0;

        case SLE_SYS_AUDIO_POS:
            slyz_ret_int(ret, (int64_t)audio_position_ms());
            return 0;

        case SLE_SYS_AUDIO_DUR:
            slyz_ret_int(ret, (int64_t)audio_duration_ms());
            return 0;

        case SLE_SYS_AUDIO_VOL:
            audio_set_volume((int)arg_i(args, argc, 0));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_AUDIO_GETVOL:
            slyz_ret_int(ret, (int64_t)audio_volume());
            return 0;

        case SLE_SYS_BEEP:
            audio_beep((uint32_t)arg_i(args, argc, 0),
                       (uint32_t)arg_i(args, argc, 1));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EXIT:
            slyzvm_request_exit(vm, (int)arg_i(args, argc, 0));
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_FAULT_TRIGGER:
            slyz_ret_int(ret, (int64_t)faultlab_trigger((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_FAULT_DUMP:
            slyz_ret_int(ret, (int64_t)faultlab_dump_now());
            return 0;

        case SLE_SYS_FAULT_LEAK:
            slyz_ret_int(ret, (int64_t)faultlab_leak((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_FAULT_LEAK_FREE:
            slyz_ret_int(ret, (int64_t)faultlab_leak_free());
            return 0;

        case SLE_SYS_FAULT_SET: {
            int len = 0;
            const char *key = arg_s(args, argc, 0, &len);
            (void)len;
            const char *value = arg_s(args, argc, 1, 0);
            slyz_ret_int(ret, (int64_t)faultlab_set(key, value));
            return 0;
        }

        case SLE_SYS_FAULT_GET: {
            int len = 0;
            const char *key = arg_s(args, argc, 0, &len);
            (void)len;
            char value[32];
            if (faultlab_get(key, value, (int)sizeof(value)) != 0) {
                if (!slyz_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            if (!slyz_ret_str(vm, ret, value, slz_strlen(value))) return -1;
            return 0;
        }

        case SLE_SYS_INPUT:
            /* 图形应用没有可用的行输入：返回空字符串而不是阻塞。
             * 终端版 SylFault 才有真实输入（shell.c 实现）。 */
            if (!slyz_ret_str(vm, ret, "", 0)) return -1;
            return 0;

        default:
            slyz_ret_int(ret, -1);
            return 0;
    }
}

/* ============================================================
 * SLYZ v3 宿主
 * ------------------------------------------------------------
 * 与 v2 使用同一组 SLE_SYS_* 编号；返回值改走 slyz3_ret_*。
 * 声明式界面走 ui.* 系统调用，由 slyzui.c 负责布局与绘制。
 * ============================================================ */

static void v3_print_val(struct SlyzVal *v, int newline) {
    if (!v) {
        kserial_puts("nil");
    } else if (v->type == SLYZ_T_INT) {
        kserial_dec32((uint32_t)v->u.i);
    } else if (v->type == SLYZ_T_STR && v->u.obj) {
        const char *s = (const char *)v->u.obj->data;
        uint32_t n = v->u.obj->len;
        for (uint32_t i = 0; i < n; i++) kserial_putchar(s[i]);
    } else if (v->type == SLYZ_T_NIL) {
        kserial_puts("nil");
    } else {
        kserial_puts("[object]");
    }
    if (newline) kserial_putchar('\n');
}

static int v3_fs_read(struct Slyz3Vm *vm, const char *path, struct SlyzVal *ret) {
    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        slyz3_ret_nil(ret);
        return 0;
    }
    int total = 0;
    while (total < (int)sizeof(g_slyz_text) - 1) {
        int n = slyzfs_read(fh, g_slyz_text + total,
                            (int)sizeof(g_slyz_text) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);
    g_slyz_text[total] = '\0';
    if (!slyz3_ret_str(vm, ret, (const char *)g_slyz_text, total)) return -1;
    return 0;
}

static int v3_fs_list(struct Slyz3Vm *vm, const char *path, struct SlyzVal *ret) {
    static struct GuiSlyzDirEntry entries[64];
    int n = slyzfs_list_dir(path, entries, 64);
    if (n < 0) n = 0;
    if (!slyz3_ret_arr(vm, ret, n)) return -1;

    for (int i = 0; i < n; i++) {
        char name[53];
        int k = 0;
        while (k < 52 && entries[i].name[k]) {
            name[k] = (char)entries[i].name[k];
            k++;
        }
        name[k] = '\0';
        struct SlyzVal item;
        if (!slyz3_ret_str(vm, &item, name, k)) return -1;
        slyz3_arr_set(vm, ret, i, item);
    }
    return 0;
}

static int v3_fs_entries(struct Slyz3Vm *vm, const char *path, struct SlyzVal *ret) {
    static struct GuiSlyzDirEntry entries[64];
    int n = slyzfs_list_dir(path, entries, 64);
    if (n < 0) n = 0;
    if (!slyz3_ret_arr(vm, ret, n)) return -1;

    for (int i = 0; i < n; i++) {
        char name[53];
        int k = 0;
        while (k < 52 && entries[i].name[k]) {
            name[k] = (char)entries[i].name[k];
            k++;
        }
        name[k] = '\0';

        struct SlyzVal row;
        if (!slyz3_ret_arr(vm, &row, 3)) return -1;

        struct SlyzVal item;
        if (!slyz3_ret_str(vm, &item, name, k)) return -1;
        slyz3_arr_set(vm, &row, 0, item);

        item.type = SLYZ_T_INT;
        item.aux = 0;
        item.u.i = entries[i].type;
        slyz3_arr_set(vm, &row, 1, item);

        item.type = SLYZ_T_INT;
        item.aux = 0;
        item.u.i = entries[i].size;
        slyz3_arr_set(vm, &row, 2, item);

        slyz3_arr_set(vm, ret, i, row);
    }
    return 0;
}

static int v3_time_text(struct Slyz3Vm *vm, struct SlyzVal *ret, int date) {
    uint32_t ms = net_now_ms();
    char buf[24];
    int n = 0;

    if (date) {
        /* 日期与 v2 一致：以开机时间换算，系统尚无 RTC 日历时保持稳定 */
        uint32_t day = ms / 86400000u;
        buf[n++] = 'D';
        buf[n++] = (char)('0' + (day / 100) % 10);
        buf[n++] = (char)('0' + (day / 10) % 10);
        buf[n++] = (char)('0' + day % 10);
    } else {
        int sec = (int)(ms / 1000u) % 86400;
        int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
        buf[n++] = (char)('0' + (h / 10) % 10);
        buf[n++] = (char)('0' + h % 10);
        buf[n++] = ':';
        buf[n++] = (char)('0' + (m / 10) % 10);
        buf[n++] = (char)('0' + m % 10);
        buf[n++] = ':';
        buf[n++] = (char)('0' + (s / 10) % 10);
        buf[n++] = (char)('0' + s % 10);
    }
    if (!slyz3_ret_str(vm, ret, buf, n)) return -1;
    return 0;
}

static int slyz3_gui_sys(struct Slyz3Vm *vm, uint16_t id,
                         struct SlyzVal *args, uint16_t argc,
                         struct SlyzVal *ret) {
    struct GuiSlyzApp *a = app_for_vm3(vm);
    if (!a) {
        slyz3_ret_int(ret, -1);
        return 0;
    }

    switch (id) {
        case SLE_SYS_PRINT:
            if (argc >= 1) v3_print_val(&args[0], 0);
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_PRINTLN:
            if (argc >= 1) v3_print_val(&args[0], 1);
            else kserial_putchar('\n');
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_EPRINT:
            if (argc >= 1) v3_print_val(&args[0], 1);
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_FS_READ:
            return v3_fs_read(vm, arg_s(args, argc, 0, 0), ret);

        case SLE_SYS_FS_WRITE: {
            int len = 0;
            const char *path = arg_s(args, argc, 0, 0);
            const char *text = arg_s(args, argc, 1, &len);
            slyz3_ret_int(ret, (int64_t)fs_write_all(path, text, len));
            return 0;
        }

        case SLE_SYS_FS_EXISTS: {
            struct GuiSlyzDirEntry ent;
            slyz3_ret_int(ret, slyzfs_stat(arg_s(args, argc, 0, 0), &ent) ? 1 : 0);
            return 0;
        }

        case SLE_SYS_FS_LIST:
            return v3_fs_list(vm, arg_s(args, argc, 0, 0), ret);

        case SLE_SYS_FS_ENTRIES:
            return v3_fs_entries(vm, arg_s(args, argc, 0, 0), ret);

        case SLE_SYS_FS_REMOVE:
            slyz3_ret_int(ret, slyzfs_remove(arg_s(args, argc, 0, 0)) ? 0 : -1);
            return 0;

        case SLE_SYS_FS_MKDIR:
            slyz3_ret_int(ret, slyzfs_mkdir(arg_s(args, argc, 0, 0)) ? 0 : -1);
            return 0;

        case SLE_SYS_CFG_GET: {
            char value[128];
            if (!cfg_get_value(arg_s(args, argc, 0, 0), value, (int)sizeof(value))) {
                if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
            } else {
                if (!slyz3_ret_str(vm, ret, value, slz_strlen(value))) return -1;
            }
            return 0;
        }

        case SLE_SYS_CFG_SET:
            slyz3_ret_int(ret, (int64_t)cfg_set_value(arg_s(args, argc, 0, 0),
                                                      arg_s(args, argc, 1, 0)));
            return 0;

        case SLE_SYS_CFG_KEYS: {
            static const char *keys[5] = {
                "boot.mode", "lang", "timezone", "wallpaper", "avatar"
            };
            if (!slyz3_ret_arr(vm, ret, 5)) return -1;
            for (int i = 0; i < 5; i++) {
                struct SlyzVal item;
                if (!slyz3_ret_str(vm, &item, keys[i], slz_strlen(keys[i]))) return -1;
                slyz3_arr_set(vm, ret, i, item);
            }
            return 0;
        }

        case SLE_SYS_TIME_NOW:
            slyz3_ret_int(ret, (int64_t)(net_now_ms() / 1000u));
            return 0;

        case SLE_SYS_TIME_MS:
            slyz3_ret_int(ret, (int64_t)net_now_ms());
            return 0;

        case SLE_SYS_TIME_TEXT:
            return v3_time_text(vm, ret, 0);

        case SLE_SYS_DATE_TEXT:
            return v3_time_text(vm, ret, 1);

        case SLE_SYS_SLEEP: {
            int64_t ms = arg_i(args, argc, 0);
            uint32_t start = net_now_ms();
            if (ms < 0) ms = 0;
            while ((uint32_t)(net_now_ms() - start) < (uint32_t)ms) {
                task_yield();
            }
            slyz3_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_BEEP:
            audio_beep((uint32_t)arg_i(args, argc, 0),
                       (uint32_t)arg_i(args, argc, 1));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_EXEC: {
            char cmd[160];
            int len = 0;
            const char *src = arg_s(args, argc, 0, &len);
            slz_copy(cmd, src, (int)sizeof(cmd));
            (void)len;
            slyz3_ret_int(ret, (int64_t)shell_exec_line(cmd));
            return 0;
        }

        case SLE_SYS_EXIT:
            slyz3vm_request_exit(vm, (int)arg_i(args, argc, 0));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_WIN_OPEN:
            slyz3_ret_int(ret, (int64_t)a->win);
            return 0;

        case SLE_SYS_WIN_CLOSE:
            a->close_requested = 1;
            if (a->ui) slyzui_request_close(a->ui);
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_WIN_SIZE:
            slyz3_ret_int(ret, (int64_t)a->content.w);
            return 0;

        case SLE_SYS_WIN_HEIGHT:
            slyz3_ret_int(ret, (int64_t)a->content.h);
            return 0;

        case SLE_SYS_WIN_TITLE: {
            int len = 0;
            const char *title = arg_s(args, argc, 1, &len);
            (void)len;
            slz_copy(a->title, title ? title : "", (int)sizeof(a->title));
            slyz3_ret_int(ret, 0);
            return 0;
        }

        case SLE_SYS_GFX_CLEAR:
            gfx_fill(a, 0, 0, a->content.w, a->content.h,
                     (uint32_t)arg_i(args, argc, 0));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_RECT:
            gfx_fill(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                     (int)arg_i(args, argc, 2), (int)arg_i(args, argc, 3),
                     (uint32_t)arg_i(args, argc, 4));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_FRAME: {
            int x = (int)arg_i(args, argc, 0);
            int y = (int)arg_i(args, argc, 1);
            int w = (int)arg_i(args, argc, 2);
            int h = (int)arg_i(args, argc, 3);
            uint32_t color = (uint32_t)arg_i(args, argc, 4);
            int t = (int)arg_i(args, argc, 5);
            if (t < 1) t = 1;
            if (t > 8) t = 8;
            gfx_fill(a, x, y, w, t, color);
            gfx_fill(a, x, y + h - t, w, t, color);
            gfx_fill(a, x, y, t, h, color);
            gfx_fill(a, x + w - t, y, t, h, color);
            slyz3_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_GFX_TEXT:
            gfx_text(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                     arg_s(args, argc, 2, 0), (uint32_t)arg_i(args, argc, 3));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_TWIDTH:
            slyz3_ret_int(ret, (int64_t)ui_text_width(arg_s(args, argc, 0, 0)));
            return 0;

        case SLE_SYS_GFX_LINE:
            gfx_line(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                     (int)arg_i(args, argc, 2), (int)arg_i(args, argc, 3),
                     (uint32_t)arg_i(args, argc, 4));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_CIRCLE:
            gfx_circle(a, (int)arg_i(args, argc, 0), (int)arg_i(args, argc, 1),
                       (int)arg_i(args, argc, 2), (uint32_t)arg_i(args, argc, 3));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_GFX_FLUSH:
            slyz3vm_request_yield(vm);
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_EVT_POLL:
        case SLE_SYS_EVT_WAIT:
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_INFO: {
            int key = (int)arg_i(args, argc, 0);
            uint32_t total = 0, used = 0, peak = 0;
            uint32_t allocs = 0, frees = 0, failed = 0;
            mem_stats(&total, &used, &peak, &allocs, &frees, &failed);

            int64_t v = -1;
            if (key == SLE_INFO_MEM_TOTAL) v = (int64_t)total;
            else if (key == SLE_INFO_MEM_USED) v = (int64_t)used;
            else if (key == SLE_INFO_MEM_PEAK) v = (int64_t)peak;
            else if (key == SLE_INFO_MEM_FREE) v = (int64_t)mem_free_bytes();
            else if (key == SLE_INFO_TASKS_ALIVE) v = (int64_t)task_count_alive();
            else if (key == SLE_INFO_TASK_MEM) v = (int64_t)task_total_used();
            else if (key == SLE_INFO_DISK_WRITTEN) v = (int64_t)task_total_disk_written();
            else if (key == SLE_INFO_UPTIME_MS) v = (int64_t)net_now_ms();
            else if (key == SLE_INFO_CPU_PERMILLE) v = (int64_t)gui_system_cpu_permille();
            else if (key == SLE_INFO_TASK_SLOTS) v = (int64_t)TASK_MAX;

            slyz3_ret_int(ret, v);
            return 0;
        }

        case SLE_SYS_PROC: {
            int idx = (int)arg_i(args, argc, 0);
            int field = (int)arg_i(args, argc, 1);
            struct Task *t = (idx >= 0 && idx < TASK_MAX) ? task_by_id(idx) : 0;
            if (!t) {
                slyz3_ret_int(ret, -1);
                return 0;
            }
            int64_t v = -1;
            if (field == SLE_PROC_ID) v = t->id;
            else if (field == SLE_PROC_STATE) v = t->state;
            else if (field == SLE_PROC_STEPS) v = t->steps;
            else if (field == SLE_PROC_QUOTA) v = (int64_t)t->quota;
            else if (field == SLE_PROC_USED) v = (int64_t)t->used;
            else if (field == SLE_PROC_EXIT) v = t->exit_code;
            slyz3_ret_int(ret, v);
            return 0;
        }

        case SLE_SYS_PROC_NAME: {
            int idx = (int)arg_i(args, argc, 0);
            struct Task *t = (idx >= 0 && idx < TASK_MAX) ? task_by_id(idx) : 0;
            const char *name = t ? t->name : "";
            if (!slyz3_ret_str(vm, ret, name, slz_strlen(name))) return -1;
            return 0;
        }

        case SLE_SYS_IMG_LOAD:
            slyz3_ret_int(ret, (int64_t)image_load(arg_s(args, argc, 0, 0)));
            return 0;

        case SLE_SYS_IMG_WIDTH:
            slyz3_ret_int(ret, (int64_t)image_width((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_IMG_HEIGHT:
            slyz3_ret_int(ret, (int64_t)image_height((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_IMG_DRAW: {
            int h = (int)arg_i(args, argc, 0);
            int x = a->content.x + (int)arg_i(args, argc, 1);
            int y = a->content.y + (int)arg_i(args, argc, 2);
            vbe_push_clip(a->content.x, a->content.y, a->content.w, a->content.h);
            image_draw(h, x, y);
            vbe_pop_clip();
            slyz3_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_IMG_DRAW_SCALED: {
            int h = (int)arg_i(args, argc, 0);
            int x = a->content.x + (int)arg_i(args, argc, 1);
            int y = a->content.y + (int)arg_i(args, argc, 2);
            int w = (int)arg_i(args, argc, 3);
            int hh = (int)arg_i(args, argc, 4);
            vbe_push_clip(a->content.x, a->content.y, a->content.w, a->content.h);
            image_draw_scaled(h, x, y, w, hh);
            vbe_pop_clip();
            slyz3_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_IMG_DRAW_CIRCLE: {
            int h = (int)arg_i(args, argc, 0);
            int cx = a->content.x + (int)arg_i(args, argc, 1);
            int cy = a->content.y + (int)arg_i(args, argc, 2);
            int r = (int)arg_i(args, argc, 3);
            vbe_push_clip(a->content.x, a->content.y, a->content.w, a->content.h);
            image_draw_circle(h, cx, cy, r);
            vbe_pop_clip();
            slyz3_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_IMG_FREE:
            image_free((int)arg_i(args, argc, 0));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_OPEN_UI:
            slyz3_ret_int(ret, (int64_t)gui_open_file_path(arg_s(args, argc, 0, 0)));
            return 0;

        case SLE_SYS_MEDIA_REQ: {
            char path[160];
            audio_take_source(path, (int)sizeof(path));
            if (!slyz3_ret_str(vm, ret, path, slz_strlen(path))) return -1;
            return 0;
        }

        case SLE_SYS_AUDIO_PLAY:
            slyz3_ret_int(ret, (int64_t)audio_play(arg_s(args, argc, 0, 0)));
            return 0;

        case SLE_SYS_AUDIO_STOP:
            audio_stop();
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_AUDIO_PAUSE:
            audio_pause((int)arg_i(args, argc, 0));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_AUDIO_STATE:
            slyz3_ret_int(ret, (int64_t)audio_state());
            return 0;

        case SLE_SYS_AUDIO_POS:
            slyz3_ret_int(ret, (int64_t)audio_position_ms());
            return 0;

        case SLE_SYS_AUDIO_DUR:
            slyz3_ret_int(ret, (int64_t)audio_duration_ms());
            return 0;

        case SLE_SYS_AUDIO_VOL:
            audio_set_volume((int)arg_i(args, argc, 0));
            slyz3_ret_nil(ret);
            return 0;

        case SLE_SYS_AUDIO_GETVOL:
            slyz3_ret_int(ret, (int64_t)audio_volume());
            return 0;

        case SLE_SYS_FAULT_TRIGGER:
            slyz3_ret_int(ret, (int64_t)faultlab_trigger((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_FAULT_DUMP:
            slyz3_ret_int(ret, (int64_t)faultlab_dump_now());
            return 0;

        case SLE_SYS_FAULT_LEAK:
            slyz3_ret_int(ret, (int64_t)faultlab_leak((int)arg_i(args, argc, 0)));
            return 0;

        case SLE_SYS_FAULT_LEAK_FREE:
            slyz3_ret_int(ret, (int64_t)faultlab_leak_free());
            return 0;

        case SLE_SYS_FAULT_SET: {
            int len = 0;
            const char *key = arg_s(args, argc, 0, &len);
            (void)len;
            slyz3_ret_int(ret, (int64_t)faultlab_set(key, arg_s(args, argc, 1, 0)));
            return 0;
        }

        case SLE_SYS_FAULT_GET: {
            char value[32];
            if (faultlab_get(arg_s(args, argc, 0, 0), value, (int)sizeof(value)) != 0) {
                if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            if (!slyz3_ret_str(vm, ret, value, slz_strlen(value))) return -1;
            return 0;
        }

        case SLE_SYS_UI_MARKUP: {
            int len = 0;
            const char *text = arg_s(args, argc, 0, &len);
            int rc = a->ui ? slyzui_set_markup(a->ui, text, len) : -1;
            if (a->ui) {
                slyzui_set_view(a->ui, a->content.x, a->content.y,
                                a->content.w, a->content.h);
            }
            slyz3_ret_int(ret, rc);
            return 0;
        }

        case SLE_SYS_UI_STYLE: {
            int len = 0;
            const char *css = arg_s(args, argc, 0, &len);
            slyz3_ret_int(ret, a->ui ? slyzui_set_style(a->ui, css, len) : -1);
            return 0;
        }

        case SLE_SYS_UI_PRESENT: {
            struct SlyzUiEvent ev;
            int has = a->ui ? slyzui_present(a->ui, &ev) : 0;
            if (!has) {
                slyz3_ret_nil(ret);
            } else {
                if (!slyz3_ret_arr(vm, ret, 4)) return -1;
                struct SlyzVal item;
                item.type = SLYZ_T_INT;
                item.aux = 0;
                item.u.i = ev.type;
                slyz3_arr_set(vm, ret, 0, item);

                if (ev.type == SLYZUI_EV_CLICK || ev.type == SLYZUI_EV_CHANGE) {
                    if (!slyz3_ret_str(vm, &item, ev.id, slz_strlen(ev.id))) return -1;
                } else if (ev.type == SLYZUI_EV_KEY) {
                    item.type = SLYZ_T_INT;
                    item.aux = 0;
                    item.u.i = ev.code;
                } else {
                    item.type = SLYZ_T_INT;
                    item.aux = 0;
                    item.u.i = 0;
                }
                slyz3_arr_set(vm, ret, 1, item);

                item.type = SLYZ_T_INT;
                item.aux = 0;
                item.u.i = 0;
                slyz3_arr_set(vm, ret, 2, item);
                slyz3_arr_set(vm, ret, 3, item);
            }
            /* 一帧一次：绘制完成后让出，桌面继续处理输入 */
            slyz3vm_request_yield(vm);
            return 0;
        }

        case SLE_SYS_UI_SET: {
            int len = 0;
            const char *id = arg_s(args, argc, 0, &len);
            (void)len;
            const char *text = arg_s(args, argc, 1, &len);
            slyz3_ret_int(ret, a->ui ? slyzui_set_text(a->ui, id, text, len) : -1);
            return 0;
        }

        case SLE_SYS_UI_GET: {
            char value[SLYZUI_TEXT_MAX];
            if (!a->ui || slyzui_get_text(a->ui, arg_s(args, argc, 0, 0),
                                          value, (int)sizeof(value)) != 0) {
                if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
                return 0;
            }
            if (!slyz3_ret_str(vm, ret, value, slz_strlen(value))) return -1;
            return 0;
        }

        case SLE_SYS_INPUT:
            if (!slyz3_ret_str(vm, ret, "", 0)) return -1;
            return 0;

        default:
            slyz3_ret_int(ret, -1);
            return 0;
    }
}

static void *slyz3_gui_alloc(uint32_t bytes) {
    return kmalloc(bytes);
}

static void slyz3_gui_free(void *ptr) {
    kfree(ptr);
}

static void slyz3_gui_log(const char *text) {
    (void)text;
}

static const struct Slyz3Ops g_slyz3_gui_ops = {
    slyz3_gui_sys,
    slyz3_gui_alloc,
    slyz3_gui_free,
    0,
    slyz3_gui_log
};

static const struct SlyzVmOps g_slyz_gui_ops = {
    gui_sys,
    slyz_gui_alloc,
    slyz_gui_free,
    0,
    slyz_gui_log
};

/* ============================================================
 * 生命周期
 * ============================================================ */

void gui_app_init(void) {
    for (int i = 0; i < GUI_SLYZ_MAX; i++) {
        g_slyz_apps[i].used = 0;
        g_slyz_apps[i].vm = 0;
        g_slyz_apps[i].vm3 = 0;
        g_slyz_apps[i].ui = 0;
        g_slyz_apps[i].is_v3 = 0;
        g_slyz_apps[i].image = 0;
        g_slyz_apps[i].state = 0;
        g_slyz_apps[i].title[0] = '\0';
    }
    g_slyz_epoch = 1;
    g_slyz_current = 0;
}

void gui_app_begin_frame(void) {
    g_slyz_epoch++;
}

void gui_app_stop(int win) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a) return;

    if (a->vm) {
        slyzvm_destroy(a->vm);
        a->vm = 0;
    }
    if (a->vm3) {
        slyz3vm_destroy(a->vm3);
        a->vm3 = 0;
    }
    if (a->ui) {
        slyzui_destroy(a->ui);
        a->ui = 0;
    }
    if (a->image) {
        kfree(a->image);
        a->image = 0;
    }
    a->used = 0;
    a->state = 0;
    a->win = -1;
    a->is_v3 = 0;
    a->close_requested = 0;
    a->evt_head = 0;
    a->evt_tail = 0;
    a->error[0] = '\0';
}

int gui_app_active(int win) {
    return app_for_win(win) != 0;
}

int gui_app_start(int win, const char *path) {
    if (!path || !path[0]) return 0;

    gui_app_stop(win);

    struct GuiSlyzApp *a = 0;
    for (int i = 0; i < GUI_SLYZ_MAX; i++) {
        if (!g_slyz_apps[i].used) {
            a = &g_slyz_apps[i];
            break;
        }
    }
    if (!a) return 0;

    struct GuiSlyzDirEntry ent;
    if (!slyzfs_stat(path, &ent) || ent.type == GUI_SLYZ_DIR_DIR || ent.size == 0) {
        return 0;
    }
    if (ent.size > SLYZ_MAX_FILE) {
        return 0;
    }

    uint8_t *image = (uint8_t *)kmalloc(ent.size + 1);
    if (!image) return 0;

    void *fh = slyzfs_open(path, "r");
    if (!fh) {
        kfree(image);
        return 0;
    }

    int total = 0;
    while (total < (int)ent.size) {
        int n = slyzfs_read(fh, image + total, (int)ent.size - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);
    image[total] = 0;

    if (total <= 0) {
        kfree(image);
        return 0;
    }

    a->used = 1;
    a->win = win;
    a->image = image;
    a->image_size = total;
    slz_copy(a->path, path, (int)sizeof(a->path));
    a->evt_head = 0;
    a->evt_tail = 0;
    a->close_requested = 0;
    a->epoch = 0;
    a->error[0] = '\0';
    a->title[0] = '\0';
    a->is_v3 = slyz3_is_binary(image, total) ? 1 : 0;

    if (a->is_v3) {
        a->ui = slyzui_create(0, 0, 640, 554);
        a->vm3 = slyz3vm_create(image, total, &g_slyz3_gui_ops, a->path);
        if (!a->ui || !a->vm3) {
            if (a->ui) { slyzui_destroy(a->ui); a->ui = 0; }
            if (a->vm3) { slyz3vm_destroy(a->vm3); a->vm3 = 0; }
            a->state = 2;
            slz_copy(a->error, "cannot load v3 image (format or memory)",
                     (int)sizeof(a->error));
            return 0;
        }
        a->state = 1;
        return 1;
    }

    a->vm = slyzvm_create(image, total, &g_slyz_gui_ops, a->path);
    if (!a->vm) {
        a->state = 2;
        slz_copy(a->error, "cannot load image (format, version or checksum)",
                 (int)sizeof(a->error));
        return 0;
    }

    a->state = 1;
    return 1;
}

int gui_app_should_close(int win) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a) return 0;
    if (a->is_v3) {
        if (a->ui && slyzui_closed(a->ui)) return 1;
        if (a->vm3 && slyz3vm_state(a->vm3) == 1) return 1;
    }
    return a->close_requested || a->state == 0;
}

const char *gui_app_status(int win) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a) return 0;
    return a->state == 2 ? a->error : 0;
}

const char *gui_app_title(int win) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a || !a->title[0]) return 0;
    return a->title;
}

/* ============================================================
 * 事件投递
 * ============================================================ */

void gui_app_click(int win, int x, int y, int button) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a) return;
    if (a->is_v3) {
        if (a->ui) slyzui_mouse(a->ui, x, y, 1);
        return;
    }
    evt_push(a, SLE_EVT_CLICK, x, y, button);
}

void gui_app_mouse(int win, int x, int y, int down) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a || !a->is_v3 || !a->ui) return;
    slyzui_mouse(a->ui, x, y, down);
}

void gui_app_key(int win, int ch) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a) return;
    if (a->is_v3) {
        if (a->ui) slyzui_key(a->ui, ch);
        return;
    }
    evt_push(a, SLE_EVT_KEY, ch, 0, 0);
}

void gui_app_wheel(int win, int delta) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a) return;
    if (a->is_v3) {
        if (a->ui) slyzui_wheel(a->ui, delta);
        return;
    }
    evt_push(a, SLE_EVT_WHEEL, delta, 0, 0);
}

/* ============================================================
 * 渲染
 * ============================================================ */

static void draw_crash(struct GuiSlyzApp *a) {
    ui_rect c = a->content;

    vbe_push_clip(c.x, c.y, c.w, c.h);
    vbe_fill_rect((uint16_t)c.x, (uint16_t)c.y, (uint16_t)c.w, (uint16_t)c.h, 0x1A0F13);
    ui_text_transparent(c.x + 24, c.y + 24, "SLYZ application stopped", 0xF87171);
    ui_text_transparent(c.x + 24, c.y + 68, a->error, 0xE9EFF7);
    ui_text_transparent(c.x + 24, c.y + 112, "close the window to dismiss", 0x8EA0B4);
    ui_text_transparent(c.x + 24, c.y + 156, a->path, 0x5A6B7E);
    vbe_pop_clip();
}

int gui_app_render(int win, ui_rect content) {
    struct GuiSlyzApp *a = app_for_win(win);
    if (!a || !a->used) return 0;

    if (content.w <= 0 || content.h <= 0) return 1;
    a->content = content;

    if (a->state == 2) {
        draw_crash(a);
        return 1;
    }
    if (a->state != 1) {
        /* 程序已结束但窗口尚未关闭：保持最后一帧，桌面会关闭它 */
        return 1;
    }

    if (a->is_v3) {
        if (a->ui) {
            slyzui_set_view(a->ui, content.x, content.y, content.w, content.h);
        }
        if (a->vm3) {
            int st = slyz3vm_step(a->vm3, 6000);
            if (st < 0) {
                a->state = 2;
                const char *err = slyz3vm_error(a->vm3);
                slz_copy(a->error, err ? err : "runtime error", (int)sizeof(a->error));
                draw_crash(a);
                return 1;
            }
            if (st == 0) {
                a->state = 0;
                a->close_requested = 1;
            }
        }
        return 1;
    }

    /* 同一帧只推进一次：脏矩形重绘会多次调用这里，
     * 应用绘制时已经把裁剪替换为完整内容区，内容不会缺失。 */
    if (a->epoch == g_slyz_epoch) return 1;
    a->epoch = g_slyz_epoch;

    g_slyz_current = a;
    vbe_push_clip(content.x, content.y, content.w, content.h);
    int st = slyzvm_step(a->vm, 120000);
    vbe_pop_clip();
    g_slyz_current = 0;

    if (st < 0) {
        a->state = 2;
        const char *err = slyzvm_error(a->vm);
        slz_copy(a->error, err ? err : "runtime error", (int)sizeof(a->error));
        draw_crash(a);
        return 1;
    }

    if (st == 0) {
        a->state = 0;
        a->close_requested = 1;
    }

    return 1;
}
