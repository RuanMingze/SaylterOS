/* ============================================================
 * vc_app.h - VizCormin 应用外壳
 * ------------------------------------------------------------
 * 负责窗口内部布局与输入：
 *   - 标签栏、编辑区、查找栏、状态栏；
 *   - 键盘编辑、光标移动、撤销/重做、保存、新建、关闭；
 *   - 鼠标点击定位与标签切换；
 *   - 通过 VcSurface 渲染，不依赖具体窗口系统。
 * 窗口宿主只需提供 surface 与事件，把本层接进 GUI 即可。
 * ============================================================ */

#ifndef VC_APP_H
#define VC_APP_H

#include <stdint.h>
#include "../core/vc_session.h"
#include "../core/vc_render.h"
#include "../core/vc_command.h"

/* 特殊键码（>= 0x100，避免与 Unicode 冲突） */
enum {
    VC_KEY_LEFT = 0x100,
    VC_KEY_RIGHT,
    VC_KEY_UP,
    VC_KEY_DOWN,
    VC_KEY_HOME,
    VC_KEY_END,
    VC_KEY_PGUP,
    VC_KEY_PGDN,
    VC_KEY_DELETE,
    VC_KEY_ESC
};

typedef struct VcApp VcApp;

VcApp *vc_app_create(const VcFileOps *ops, uint32_t width, uint32_t height,
                     uint32_t cell_w, uint32_t cell_h);
void   vc_app_destroy(VcApp *a);

void   vc_app_set_size(VcApp *a, uint32_t width, uint32_t height);
int    vc_app_open(VcApp *a, const char *path);

/* 键盘：key 为 ASCII/特殊键码，mods 为 VC_MOD_*。 */
void   vc_app_key(VcApp *a, uint32_t key, uint32_t mods);
/* 文本输入（输入法/字符）。 */
void   vc_app_text(VcApp *a, uint32_t cp);
/* 鼠标：窗口内坐标。 */
void   vc_app_mouse(VcApp *a, int x, int y, int button);

VcRenderStats vc_app_render(VcApp *a, const VcSurface *surface);

int    vc_app_find_active(const VcApp *a);

/* 宿主与测试访问会话（标签页、文档）。 */
VcSession *vc_app_session(VcApp *a);

#endif /* VC_APP_H */
