/* ============================================================
 * image.h - 内核图片解码与绘制
 * ------------------------------------------------------------
 * 当前支持 BMP（Windows BITMAPINFOHEADER 及以上）：
 *   8 位调色板、16 位 5-6-5 / BI_BITFIELDS、24 位 BGR、
 *   32 位 BGRA，自底向上与自顶向下两种行序都处理。
 *
 * 解码结果统一为 32 位 0xAARRGGBB 像素缓冲，放在内核堆里并按
 * 路径缓存：桌面背景每帧都会请求同一张壁纸，缓存命中时只做
 * 一次线性查找，不会重复解码。
 *
 * 所有绘制函数都遵守 vbe 当前裁剪矩形；坐标是屏幕绝对坐标。
 * 绘制使用最近邻采样与整数运算，不依赖浮点或 SSE。
 * ============================================================ */

#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

/* 失败返回值。合法句柄从 0 开始。 */
#define IMAGE_NONE (-1)

/* 加载图片并返回缓存句柄；文件不存在、格式不支持或内存不足返回
 * IMAGE_NONE。同一路径重复调用返回同一个句柄。 */
int image_load(const char *path);

/* 句柄信息；非法句柄返回 0。 */
int image_width(int handle);
int image_height(int handle);

/* 从缓存中移除一张图片并释放像素。非法句柄无副作用。 */
void image_free(int handle);

/* 清空整个缓存（切换用户/卸载文件系统前调用） */
void image_cache_clear(void);

/* 缓存当前占用字节数（诊断用） */
uint32_t image_cache_bytes(void);

/* 原尺寸绘制，左上角在 (x, y) */
void image_draw(int handle, int x, int y);

/* 缩放到 w×h 后绘制，最近邻采样 */
void image_draw_scaled(int handle, int x, int y, int w, int h);

/* 缩放到直径 2*radius 并以 (cx, cy) 为圆心裁剪绘制。
 * 用于用户头像：圆外像素不会被写入。 */
void image_draw_circle(int handle, int cx, int cy, int radius);

/* 把整张图缩放进调用方提供的 32 位像素缓冲（dw×dh，行距 = dw）。
 * 桌面壁纸用它把"缩放结果"缓存起来：之后每帧只按脏矩形拷贝像素，
 * 不再重复做 1600×900 次采样。透明像素与 bg 合成。 */
int image_scale_to_buffer(int handle, uint32_t *dst, int dw, int dh, uint32_t bg);

/* 大块像素缓冲：优先从 VM 物理帧池分配，失败再回落内核堆。 */
void *image_alloc_buffer(uint32_t bytes);
void image_free_buffer(void *ptr);

#endif /* IMAGE_H */
