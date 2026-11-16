/* ============================================================
 * html.h - 极简 HTML + CSS 渲染器（无 JavaScript）
 * ------------------------------------------------------------
 * 面向 GUI 浏览器窗口的流式排版引擎：
 *   - 解析块级/行内标签，按可用宽度自动换行；
 *   - 支持 <style> 中的简单规则与内联 style；
 *   - 记录 <a href> 的屏幕矩形，供点击命中；
 *   - 两遍执行：先测量总高度，再按 scroll 偏移绘制。
 * 不支持的：脚本、框架、外部样式表、图片解码、表格列宽算法。
 * ============================================================ */

#ifndef HTML_H
#define HTML_H

/* 渲染一段 HTML。
 *   src/len   : 原始 HTML 文本
 *   x/y/w     : 内容区左上角与可用宽度（像素）
 *   scroll    : 向上滚动的像素数
 *   content_h : 输出排版后的总高度
 * 返回总高度。 */
int html_render(const char *src, int len, int x, int y, int w, int scroll,
                int *content_h);

/* 命中链接：px/py 为屏幕坐标。命中返回 1 并把 href 写进 out。 */
int html_link_at(int px, int py, char *out, int cap);

/* 清空链接表（每次重新渲染前自动调用，也可手动调） */
void html_reset(void);

#endif /* HTML_H */
