/* ============================================================
 * mp3.h - MPEG-1 Layer III 解码器
 * ------------------------------------------------------------
 * 只依赖 <stdint.h>，不使用 libc / libm，便于内核直接链接。
 * 解码算法为自研实现：帧头/边信息解析、比例因子、Huffman 解码、
 * 立体声处理、混叠抑制、IMDCT 与多相合成滤波全部在本项目内完成；
 * mp3_tables.h 仅包含 ISO/IEC 11172-3 规定的静态常量数据。
 *
 * 使用方式：
 *   decoder = kmalloc(mp3_decoder_size());
 *   mp3_decoder_init(decoder);
 *   while (...) {
 *       int ch, hz;
 *       int n = mp3_decode_frame(decoder, frame, frame_bytes, pcm, &ch, &hz);
 *   }
 *
 * pcm 需要能容纳 1152 * 2 个 int16（立体声）。
 * 返回值是每声道样本数（1152），失败返回 0。
 * ============================================================ */

#ifndef MP3_H
#define MP3_H

#include <stdint.h>

/* 单帧最大 PCM 样本数（每声道） */
#define MP3_MAX_SAMPLES 1152

/* 帧头解析结果 */
struct Mp3FrameInfo {
    int frame_bytes;   /* 含 4 字节帧头与 padding 的总字节数 */
    int channels;      /* 1 或 2 */
    int hz;            /* 采样率：32000 / 44100 / 48000 */
    int bitrate_kbps;
};

/* 解码器状态尺寸，便于调用方用 kmalloc 分配。 */
uint32_t mp3_decoder_size(void);

/* 初始化（清零全部状态）。返回 1 成功。 */
int mp3_decoder_init(void *state);

/* 检查 4 字节帧头是否是本解码器支持的 MPEG-1 Layer III。 */
int mp3_header_valid(const uint8_t *hdr);

/* 解析帧头；失败返回 0。 */
int mp3_parse_header(const uint8_t *hdr, struct Mp3FrameInfo *info);

/* 解码一帧。frame 必须包含完整帧（含帧头）。
 * 返回每声道样本数（1152），失败返回 0。 */
int mp3_decode_frame(void *state, const uint8_t *frame, int frame_bytes,
                     int16_t *pcm, int *out_channels, int *out_hz);

#endif /* MP3_H */
