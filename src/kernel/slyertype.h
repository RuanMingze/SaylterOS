/* ============================================================
 * slyertype.h - 内核 TrueType 平滑字体引擎
 * ------------------------------------------------------------
 * SlyerType 负责把系统字体目录里的 TrueType 轮廓字体渲染成
 * 抗锯齿字形。设计目标与 FreeType 同类：解析 sfnt/glyf 轮廓、
 * 展开二次贝塞尔曲线、按扫描线计算覆盖率、alpha 混合上屏。
 *
 * 使用边界：
 *   - 只在 VBE 图形模式（vbe_is_enabled()）下初始化；
 *   - 80x25 文本模式（tty_gfx == 0）永远不启用，绘制请求直接
 *     返回 0，由调用方回退到原有 16x32 / 32x32 点阵，
 *     宽字符继续显示为两个 '?'；
 *   - 字体文件缺失、表损坏或字形超出缓存上限时逐字回退点阵，
 *     不会阻塞启动，也不会画出错误像素。
 *
 * 坐标约定：所有公开绘制接口都使用"单元格"语义——调用方给出
 * 左上角像素与单元格宽高，SlyerType 在格内居中排版字形。这样
 * 终端与 GUI 既有的 16px/32px 栅格布局不需要改动。
 * ============================================================ */

#ifndef SLYERTYPE_H
#define SLYERTYPE_H

#include <stdint.h>

/* 初始化：加载 system\fonts 下的 TrueType 字体并建立字形缓存。
 * 幂等；未启用 VBE 时直接返回 0 且不分配任何内存。 */
int slyertype_init(void);

/* 调试模式专用：从内核内嵌字体建立最小可用面（拉丁），
 * 供 SlyzFS 尚未挂载的早期调试控制台使用平滑字形。 */
int slyertype_init_early(void);

/* 引擎是否可用（字体已加载 + 图形模式）。 */
int slyertype_available(void);

/* 已加载的字体面数量。 */
int slyertype_face_count(void);

/* 指定码位是否由已加载字体覆盖。 */
int slyertype_covers(uint32_t cp);

/* 人类可读的状态描述，用于启动日志。 */
const char *slyertype_status(void);

/* 在 backbuffer 上绘制一个码位（透明背景，只混合字形像素）。
 * x,y,cw,ch 是单元格像素矩形；cp 是 Unicode 码位。
 * 返回 1 表示已处理（含空格），0 表示字体不覆盖或引擎不可用，
 * 调用方应回退到点阵字形。 */
int slyertype_draw_cell_buf(int x, int y, int cw, int ch,
                            uint32_t cp, uint32_t fg);

/* 在 backbuffer 上绘制一个码位并先把单元格填成 bg（不透明文本）。 */
int slyertype_draw_cell_buf_opaque(int x, int y, int cw, int ch,
                                   uint32_t cp, uint32_t fg, uint32_t bg);

/* 在 VBE 线性帧缓冲上绘制（终端直写路径）。
 * 终端已知底色，因此按 bg 计算混合结果，不回读显存。 */
int slyertype_draw_cell_fb(int x, int y, int cw, int ch,
                           uint32_t cp, uint32_t fg, uint32_t bg);

/* SlyzOS 专属字形：位于 Supplementary Private Use Area-A，
 * 不占用任何真实 Unicode 文字区，也不会与系统字体冲突。 */
#define SLYERTYPE_LOGO_CP 0xF0001u

#endif /* SLYERTYPE_H */
