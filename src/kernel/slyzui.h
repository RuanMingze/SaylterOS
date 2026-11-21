/* ============================================================
 * slyzui.h - SLYZ v3 声明式界面引擎
 * ------------------------------------------------------------
 * 负责 screen 标记与 CSS 样式表的解析、自动布局、绘制、
 * 命中测试、滚动、hover/pressed/focus 状态与过渡动画。
 *
 * 设计边界：
 *   - 标记是行导向的 XAML 风格（一个元素一行，花括号嵌套）；
 *   - CSS 支持 type/.class/#id 组合、后代与子选择器、:hover/
 *     :pressed/:focus/:disabled 伪类、属性级层叠与继承；
 *   - 布局默认撑开：文本自动换行，容器按内容长高，根视图装不下
 *     时滚动；只有显式 clip/overflow:hidden 才裁剪。
 *
 * 引擎只依赖 ui/vbe/unicode/mem，可被 GUI 宿主独立使用。
 * ============================================================ */

#ifndef SLYZUI_H
#define SLYZUI_H

#include <stdint.h>

#define SLYZUI_MAX_NODES   192
#define SLYZUI_MAX_RULES   96
#define SLYZUI_ID_MAX      32
#define SLYZUI_TEXT_MAX    192
#define SLYZUI_CLASS_MAX   64
#define SLYZUI_ERR_MAX     96

enum {
    SLYZUI_EV_NONE = 0,
    SLYZUI_EV_CLICK = 3,
    SLYZUI_EV_KEY = 1,
    SLYZUI_EV_CLOSE = 5,
    SLYZUI_EV_CHANGE = 8
};

struct SlyzUiEvent {
    int  type;
    int  code;
    char id[SLYZUI_ID_MAX];
};

struct SlyzUi;

struct SlyzUi *slyzui_create(int x, int y, int w, int h);
void slyzui_destroy(struct SlyzUi *ui);

/* 窗口内容区变化（拖动/最大化/恢复）时同步。 */
void slyzui_set_view(struct SlyzUi *ui, int x, int y, int w, int h);

/* 设置标记与样式表；两者都会触发重新解析与布局。
 * 返回 0 成功，-1 表示解析失败（错误文本可用 slyzui_error 读取）。 */
int  slyzui_set_markup(struct SlyzUi *ui, const char *text, int len);
int  slyzui_set_style(struct SlyzUi *ui, const char *css, int len);
const char *slyzui_error(struct SlyzUi *ui);

/* 控件文本更新/读取。id 为空或不存在时返回 -1。 */
int  slyzui_set_text(struct SlyzUi *ui, const char *id, const char *text, int len);
int  slyzui_get_text(struct SlyzUi *ui, const char *id, char *out, int cap);

/* 输入转发：坐标是窗口内容区局部坐标。 */
void slyzui_mouse(struct SlyzUi *ui, int x, int y, int down);
void slyzui_wheel(struct SlyzUi *ui, int delta);
void slyzui_key(struct SlyzUi *ui, int ch);

/* 布局 + 绘制 + 推进动画。返回 1 表示 out 中有一个事件。 */
int  slyzui_present(struct SlyzUi *ui, struct SlyzUiEvent *out);

int  slyzui_closed(struct SlyzUi *ui);
void slyzui_request_close(struct SlyzUi *ui);

#endif /* SLYZUI_H */
