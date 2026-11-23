/* ============================================================
 * gui.h - 图形会话入口
 * ============================================================ */

#ifndef GUI_H
#define GUI_H

#include <stdint.h>

/* 进入完整图形会话：启动画面 -> 图形登录 -> 桌面。
 * 返回表示用户从桌面选择了 "Exit to Terminal"；
 * 重启/关机会直接复位或断电，不会返回。 */
void gui_main(void);

/* 请求下一帧整屏重绘。SLYZ 应用修改壁纸/头像后调用，
 * 否则局部脏矩形重绘只会刷新设置窗口，桌面背景保持旧图。 */
void gui_request_redraw_all(void);

/* SLYZ 的 win.open(title, w, h)：按应用请求的内容区尺寸调整窗口，
 * 自动居中并夹在可用桌面内。窗口尚未打开时同时完成打开与聚焦。 */
void gui_slyz_resize_window(int win, int w, int h);

/* 按扩展名打开文件：.mp3 -> 媒体播放器，SLYZ 程序 -> 应用窗口，
 * 其他文本 -> 编辑器。返回占用的窗口 id，失败返回 -1。
 * 供 SLYZ 的 os.open() 与桌面/文件管理器共用。 */
int gui_open_file_path(const char *path);

/* GUI 独占键盘（IRQ1 屏蔽）期间，预装模块必须用这两个接口读键：
 * owned 返回当前是否由 GUI 独占，poll 直接轮询 8042 取一个字符。 */
int  gui_keyboard_owned(void);
char gui_keyboard_poll(void);

/* ============================================================
 * 外部窗口宿主
 * ------------------------------------------------------------
 * 原生预装模块（.slys）不直接改写全屏，而是注册为一个普通窗口：
 * GUI 负责标题栏、拖动、缩放、层级与脏区，模块只画自己的内容区。
 * 回调由应用提供，GUI 在对应时机调用。
 * ============================================================ */
typedef struct GuiExternalHost {
    /* 内容区坐标已由 GUI 提供；模块用相对坐标绘制。 */
    void (*render)(int x, int y, int w, int h);
    void (*key)(int key);              /* 原始键码（含 0xF1.. 特殊键） */
    void (*mouse)(int x, int y, int button);
    void (*close)(void);
    void (*resize)(int w, int h);
} GuiExternalHost;

/* 打开/关闭唯一的原生应用窗口。w/h 是建议的内容区尺寸。 */
int  gui_external_open(const char *title, int w, int h,
                       const GuiExternalHost *host);
void gui_external_close(void);
int  gui_external_is_open(void);

/* 供 SlysKernelApi 转发：坐标相对窗口内容区，自动裁剪。 */
void     gui_ext_fill_rect(int x, int y, int w, int h, uint32_t color);
int      gui_ext_draw_cell(int x, int y, int cw, int ch,
                           uint32_t cp, uint32_t fg);
uint32_t gui_ext_width(void);
uint32_t gui_ext_height(void);
void     gui_ext_invalidate(void);
void     gui_ext_request_close(void);

#endif /* GUI_H */
