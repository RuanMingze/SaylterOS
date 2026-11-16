/* ============================================================
 * audio.h - AC'97 音频输出与 MP3 媒体播放
 * ------------------------------------------------------------
 * 负责三件事：
 *   1. 在 PCI 上发现 Intel 82801AA AC'97 控制器，建立 PCM
 *      输出 DMA（8 个描述符循环，48 kHz / 16 位 / 立体声）；
 *   2. 用 MP3 解码器把文件解成 PCM，重采样到 48 kHz，按软件
 *      音量混合后写入 DMA 缓冲；
 *   3. 向 SLYZ 应用提供播放/暂停/停止/进度/音量接口。
 *
 * 所有函数都在 GUI 线程调用。audio_pump() 需要桌面每帧调用
 * 一次，负责补充解码数据并回收已经播放完的 DMA 描述符。
 * ============================================================ */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* 播放状态 */
#define AUDIO_STOPPED 0
#define AUDIO_PLAYING 1
#define AUDIO_PAUSED  2

/* 初始化 AC'97。没有声卡时安全降级：播放接口仍然可用，
 * 只是不会产生声音；beep 走 PC 扬声器。 */
void audio_init(void);

/* GUI 模式下创建后台音频任务：驱动在自己的任务里解码并
 * 填充 DMA，GUI 主线程不再参与解码。TTY 自检不要调用。 */
void audio_start_task(void);

/* 每帧调用：解码 MP3 数据并填充 DMA 环。 */
void audio_pump(void);

/* AC'97 是否可用 */
int audio_available(void);

/* 开始播放一个 MP3 文件。返回 0 成功。-1 表示打开/解析失败。 */
int audio_play(const char *path);

/* 停止并释放当前文件；位置归零。 */
void audio_stop(void);

/* 暂停/继续。 */
void audio_pause(int paused);

int audio_state(void);

/* 当前播放位置与总时长（毫秒）。 */
uint32_t audio_position_ms(void);
uint32_t audio_duration_ms(void);

/* 软件音量 0..100。 */
void audio_set_volume(int volume);
int audio_volume(void);

/* 双击 MP3 时由桌面写入待播放路径，媒体播放器启动后取走。 */
void audio_set_source(const char *path);
int audio_take_source(char *out, int cap);

/* PC 扬声器蜂鸣（阻塞 ms 毫秒）。 */
void audio_beep(uint32_t freq, uint32_t ms);

#endif /* AUDIO_H */
