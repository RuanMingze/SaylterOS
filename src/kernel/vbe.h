#ifndef VBE_H
#define VBE_H

#include <stdint.h>

/* VBE 结果由 bootloader (pm.asm) 存在 0x7DF0 */
#define VBE_INFO_PTR   ((vbe_info_t *)0x7DF0)
#define VBE_MAGIC_OK   0xA1B2

typedef struct __attribute__((packed)) {
    uint16_t magic;          /* 0xA1B2 = VBE OK */
    uint32_t framebuffer;    /* 物理地址 */
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint16_t pitch;          /* BytesPerScanLine */
} vbe_info_t;   /* packed: 必须与 pm.asm 的紧凑写入布局一致 (pitch 在奇地址 0xB) */

extern vbe_info_t vbe_current;

/* 读 bootloader 留下的 VBE 信息，填全局 vbe_current */
void vbe_init(void);

/* 全局填充 */
void vbe_fill_color(uint32_t color);
void vbe_fill_sky_blue(void);   /* 0x00BFE8FF */

/* 双缓冲：把 backbuffer 一次性 blit 到 LFB */
void vbe_swap(void);

/* 脏矩形：只 blit (x,y) 起 w×h 区域到 LFB */
void vbe_swap_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* 全局裁剪：设置后所有绘制原语只写该矩形内的像素。
 * GUI 局部重绘用它渲染整棵场景树，再配合 vbe_swap_rect 上屏。 */
void vbe_set_clip(int x, int y, int w, int h);
void vbe_clear_clip(void);
int  vbe_clip_active(void);
void vbe_get_clip(int *x, int *y, int *w, int *h);

/* 可嵌套裁剪：与当前裁剪求交，pop 时恢复上一层。
 * 用于"脏矩形重绘 → 窗口 → 内容区"这种多层裁剪。 */
void vbe_push_clip(int x, int y, int w, int h);
void vbe_pop_clip(void);

/* 单像素 */
void vbe_put_pixel(uint16_t x, uint16_t y, uint32_t color);

/* 读 backbuffer 像素（UI 的圆角阴影 / 高光需要做 alpha 混合） */
uint32_t vbe_get_pixel(uint16_t x, uint16_t y);

/* 把 color 以 alpha（0..255）混合到 backbuffer 矩形上 */
void vbe_blend_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint32_t color, uint8_t alpha);

/* 实心矩形 */
void vbe_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);

/* 按行区间填充 backbuffer，供多核并行渲染使用。
 * 只写 [y0, y1) 行、x..x+w 的区域；调用方必须保证多个工作线程的
 * 行区间互不重叠（每个像素只被一个核写）。 */
void vbe_fill_rect_rows(uint16_t x, uint16_t w, uint16_t y0, uint16_t y1,
                        uint32_t color);

/* 光标双缓冲：保存/恢复矩形像素到内存 buf */
void vbe_save_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t *buf);
void vbe_restore_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint32_t *buf);

/* 16×32 字符绘制 */
void vbe_draw_char(uint16_t cx, uint16_t cy, char c, uint32_t fg, uint32_t bg);
void vbe_draw_string(const char *s, uint16_t cx, uint16_t cy, uint32_t fg, uint32_t bg);

/* 像素坐标文本（GUI 用）：每个字形 16×32 像素，不返回边界外 */
void vbe_draw_text_px(uint16_t x, uint16_t y, const char *s, uint32_t fg, uint32_t bg);

/* ---- 文本终端专用：直写帧缓冲（单缓冲，不经过 backbuffer） ----
 * 终端字符量大、颜色固定，双缓冲会让每个字符多一次 2KB 的 LFB 复制；
 * 直接写 LFB 可以省掉这一半的显存流量。GUI 仍使用 backbuffer + swap。 */
void vbe_fb_clear(uint32_t color);
void vbe_fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);
void vbe_fb_draw_char(uint16_t cx, uint16_t cy, char c, uint32_t fg, uint32_t bg);
void vbe_fb_scroll_up(uint16_t pixel_rows, uint32_t fill_color);

/* 状态 */
uint8_t vbe_is_enabled(void);

/* 辅助：命中检测 */
static inline uint8_t point_in_rect(int16_t px, int16_t py,
                                    uint16_t rx, uint16_t ry,
                                    uint16_t rw, uint16_t rh) {
    return (px >= (int16_t)rx && px < (int16_t)(rx + rw) &&
            py >= (int16_t)ry && py < (int16_t)(ry + rh));
}

#endif /* VBE_H */
