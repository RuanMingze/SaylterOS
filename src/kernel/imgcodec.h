/* ============================================================
 * imgcodec.h - 图片解码公共接口
 * ------------------------------------------------------------
 * 解码器只处理内存中的完整文件，输出统一的 0xAARRGGBB 像素。
 * 内存申请走 img_alloc/img_free：内核映射到 kmalloc/kfree，
 * Windows 端测试工具映射到 malloc/free，解码逻辑只有一份。
 * ============================================================ */

#ifndef IMGCODEC_H
#define IMGCODEC_H

#include <stdint.h>

/* 单张解码结果上限。默认壁纸 8256x5504 会按比例缩放后再解码，
 * 因此正常路径不会接近这个值；它只是防御异常图片的硬闸。 */
#define IMG_MAX_BYTES (24u * 1024u * 1024u)

typedef struct {
    uint32_t *px;      /* 0xAARRGGBB，自上而下 */
    int       w;
    int       h;
} ImgBitmap;

void *img_alloc(uint32_t bytes);
void  img_free(void *ptr);

int img_inflate(const uint8_t *src, int src_len,
                uint8_t *dst, int dst_cap, int *dst_len);

/* PNG：支持 8/16 位、灰度/RGB/调色板/灰度+Alpha/RGBA、tRNS。 */
int img_png_decode(const uint8_t *data, int size, ImgBitmap *out);

/* JPEG：baseline Huffman；大图自动按 1/8 DCT 缩放解码。 */
int img_jpeg_decode(const uint8_t *data, int size, ImgBitmap *out);

/* SVG：矩形/圆/椭圆/折线/路径（M L H V C S Q T A Z）填充。
 * max_dim 限制光栅尺寸（默认头像 128）。 */
int img_svg_decode(const uint8_t *data, int size, int max_dim,
                   uint32_t color, ImgBitmap *out);

#endif /* IMGCODEC_H */
