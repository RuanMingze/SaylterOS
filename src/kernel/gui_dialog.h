/* ============================================================
 * gui_dialog.h - 桌面模态对话框（截图保存 / 文件选择）
 * ------------------------------------------------------------
 * 对话框独立于窗口系统：它总是画在所有窗口与菜单之上，打开期间
 * 独占鼠标与键盘。当前提供 PrtSc 截图保存对话框，文件浏览器与
 * 后续的打开/保存对话框共用同一套目录浏览逻辑。
 * ============================================================ */

#ifndef GUI_DIALOG_H
#define GUI_DIALOG_H

/* PrtSc：抓取当前屏幕并弹出保存位置选择。重复按下会先丢弃上一次
 * 尚未保存的截图，避免内存累积。 */
void gui_dialog_open_screenshot(void);

int  gui_dialog_active(void);

/* 模态绘制/事件入口；活动期间由桌面主循环优先调用。 */
void gui_dialog_render(void);
void gui_dialog_click(int x, int y);
void gui_dialog_key(char key);
void gui_dialog_wheel(int delta);

/* 桌面底部短暂提示（保存成功/失败）。 */
void gui_toast(const char *text, uint32_t ms);
void gui_toast_render(void);
int  gui_toast_active(void);

#endif /* GUI_DIALOG_H */
