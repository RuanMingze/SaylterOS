/* ============================================================
 * gui_slyz.h - 桌面侧的 SLYZ 应用宿主
 * ------------------------------------------------------------
 * 每个 SLYZ 桌面应用拥有独立的虚拟机实例与独立窗口：应用在
 * 自己的 VM 里执行，崩溃只终止该 VM，内核与其他应用继续运行。
 *
 * 渲染模型：
 *   桌面每帧给每个活动应用一次渲染机会。VM 执行到 gfx.flush 时
 *   暂停，宿主把内容区标脏；下一帧继续处理事件并绘制下一帧。
 *   应用绘制时临时把裁剪矩形设为自己的内容区，所以越界绘制
 *   不会污染标题栏或其他窗口。
 *
 * 事件模型：
 *   鼠标点击/键盘在内容区内的输入先进入应用的环形队列，应用通过
 *   event.poll() 取出；没有事件时返回 nil，应用继续绘制下一帧。
 * ============================================================ */

#ifndef GUI_SLYZ_H
#define GUI_SLYZ_H

#include "ui.h"

/* 同时运行的 SLYZ 应用数量。窗口总数上限仍是 GUI_WIN_MAX。 */
#define GUI_SLYZ_MAX 4

void gui_app_init(void);

/* 每帧开头调用一次，使渲染去重生效 */
void gui_app_begin_frame(void);

/* 启动/停止某个窗口上的 SLYZ 程序。返回 1 成功。 */
int  gui_app_start(int win, const char *path);
void gui_app_stop(int win);

/* 该窗口是否由 SLYZ 应用接管 */
int  gui_app_active(int win);

/* 渲染一帧。返回 1 表示该窗口由 SLYZ 接管（无论成功或崩溃），
 * 返回 0 表示没有绑定应用，调用方应回退到内置实现。 */
int  gui_app_render(int win, ui_rect content);

/* 事件投递：坐标为内容区局部坐标 */
void gui_app_click(int win, int x, int y, int button);
void gui_app_key(int win, int ch);
void gui_app_wheel(int win, int delta);

/* 应用已结束或请求关闭时返回 1，桌面据此关闭窗口 */
int  gui_app_should_close(int win);

/* 调试/状态文本（崩溃原因等），无应用时返回 0 */
const char *gui_app_status(int win);

#endif /* GUI_SLYZ_H */
