/* ============================================================
 * audio_drv.c - 可加载音频驱动（.slys）
 * ------------------------------------------------------------
 * 由内核加载到固定地址后，在独立任务里循环解码 MP3 并填充
 * AC'97 DMA，GUI 只通过导出表查询状态与控制播放，不再在自
 * 己的帧循环里做解码与重采样。
 *
 * 驱动不链接任何内核符号：内存、时间、日志、锁、任务让出全
 * 部通过 SlysKernelApi 提供。MP3 文件一次性读入驱动内的
 * 8 MB 缓冲，播放期间不再访问 SlyzFS，避免与 GUI 的文件操作
 * 产生并发问题。
 * ============================================================ */

#include <stdint.h>

#include "slys.h"
#include "mp3.h"
#include "io.h"

#define AUDIO_MUSIC_MAX    (8u * 1024u * 1024u)

static const struct SlysKernelApi *g_kin = 0;

uint32_t audio_position_ms(void);

static uint64_t g_stat_sum = 0;
static uint32_t g_stat_count = 0;
static uint32_t g_stat_peak = 0;
static uint32_t g_underruns = 0;

static uint32_t au_isqrt(uint32_t v) {
    if (v == 0) return 0;
    uint32_t x = v;
    uint32_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

static void drv_log_u32(uint32_t v) {
    if (g_kin) g_kin->log_u32(v);
}

static void audio_report_stats(void) {
    if (g_stat_count < 12000u) return;

    uint32_t rms = au_isqrt((uint32_t)(g_stat_sum / g_stat_count));

    if (!g_kin) return;
    g_kin->log("[AUDIO] frames=");
    drv_log_u32(g_stat_count);
    g_kin->log(" rms=");
    drv_log_u32(rms);
    g_kin->log(" peak=");
    drv_log_u32(g_stat_peak);
    g_kin->log(" pos=");
    drv_log_u32(audio_position_ms());
    g_kin->log(" underrun=");
    drv_log_u32(g_underruns);
    g_kin->log(rms < 2 ? " SILENT\r\n" : "\r\n");

    g_stat_sum = 0;
    g_stat_count = 0;
    g_stat_peak = 0;
    g_underruns = 0;
}

/* ============================================================
 * AC'97 寄存器
 * ============================================================ */

#define AC97_NAM_RESET      0x00
#define AC97_NAM_MASTER_VOL 0x02
#define AC97_NAM_PCM_VOL    0x18
#define AC97_NAM_PCM_RATE   0x2C

#define AC97_NABM_PO_BASE   0x10
#define AC97_NABM_PO_CIV    0x14
#define AC97_NABM_PO_LVI    0x15
#define AC97_NABM_PO_SR     0x16
#define AC97_NABM_PO_PICB   0x18
#define AC97_NABM_PO_CR     0x1B
#define AC97_NABM_GLOB_CNT  0x2C
#define AC97_NABM_GLOB_STA  0x30

#define AUDIO_DESC_COUNT    16
#define AUDIO_DESC_FRAMES   4096          /* 每描述符的采样帧数 */
#define AUDIO_DESC_BYTES    (AUDIO_DESC_FRAMES * 4)
#define AUDIO_RATE          48000

struct AudioDesc {
    uint32_t addr;
    uint16_t frames;
    uint16_t flags;
};

/* PCI 配置空间 */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* ============================================================
 * 状态（全部静态驻留，驱动不依赖内核堆）
 * ============================================================ */

#define AUDIO_FIFO_FRAMES 32768

static int      g_ac97_found = 0;
static uint16_t g_nam = 0;
static uint16_t g_nabm = 0;

static struct AudioDesc g_bdl_mem[AUDIO_DESC_COUNT]
    __attribute__((aligned(64)));
static uint8_t g_desc_mem[AUDIO_DESC_COUNT * AUDIO_DESC_BYTES]
    __attribute__((aligned(64)));
static struct AudioDesc *g_bdl = 0;
static int       g_fill_desc = 0;
static int       g_fill_off = 0;

static int16_t  g_fifo_mem[AUDIO_FIFO_FRAMES * 2];
static int16_t *g_fifo = 0;
static uint32_t  g_fifo_head = 0;
static uint32_t  g_fifo_tail = 0;
static uint32_t  g_fifo_count = 0;
static uint32_t  g_src_phase = 0;
static int       g_src_rate = 44100;
static uint32_t  g_consumed = 0;
static uint32_t  g_duration_ms = 0;

static uint8_t g_decoder_mem[32768] __attribute__((aligned(16)));
static struct Mp3Decoder *g_decoder = 0;
static uint8_t  g_frame_mem[2048];
static uint8_t *g_frame = 0;
static int16_t  g_pcm[MP3_MAX_SAMPLES * 2];

static uint8_t  g_music[AUDIO_MUSIC_MAX];
static uint32_t g_music_len = 0;
static uint32_t g_music_pos = 0;

static uint32_t  g_bitrate = 128;
static int       g_state = 0;          /* 0 停止 1 播放 2 暂停 */
static int       g_volume = 70;
static int       g_eof = 0;

/* 播放进度使用独立软件时钟：宿主机没有可用音频后端时，
 * AC'97 描述符可能不前进，但用户仍需要看到播放进度且确认
 * 没有暂停。暂停时冻结，继续时从冻结值接着走。 */
static uint32_t  g_pos_base = 0;
static uint32_t  g_pos_tick = 0;
static int       g_pos_running = 0;

static void pos_start(void) {
    g_pos_base = 0;
    g_pos_tick = g_kin->now_ms();
    g_pos_running = 1;
}

static void pos_hold(void) {
    if (g_pos_running) {
        g_pos_base = audio_position_ms();
        g_pos_running = 0;
    }
}

static void pos_resume(void) {
    if (!g_pos_running) {
        g_pos_tick = g_kin->now_ms();
        g_pos_running = 1;
    }
}

static void pos_stop(void) {
    g_pos_base = 0;
    g_pos_running = 0;
}

/* ============================================================
 * 小工具
 * ============================================================ */

static void au_copy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void au_delay_ms(uint32_t ms) {
    uint32_t start = g_kin->now_ms();
    while (g_kin->now_ms() - start < ms) {
        __asm__ volatile ("pause");
    }
}

/* ============================================================
 * PCI
 * ============================================================ */

static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFCu);
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, off));
    return inw((uint16_t)(PCI_CONFIG_DATA + (off & 2)));
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                        uint16_t value) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, off));
    outw((uint16_t)(PCI_CONFIG_DATA + (off & 2)), value);
}

static int ac97_is_device(uint16_t vendor, uint16_t device) {
    if (vendor != 0x8086u) return 0;
    return device == 0x2415u || device == 0x2425u ||
           device == 0x2445u || device == 0x2485u;
}

static uint32_t g_bdl_phys = 0;
static uint32_t g_desc_phys = 0;

static int ac97_find(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            uint16_t device = (uint16_t)(id >> 16);
            if (!ac97_is_device(vendor, device)) continue;

            uint32_t header = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            int funcs = (header & 0x00800000u) ? 8 : 1;

            for (int func = 0; func < funcs; func++) {
                uint32_t cfg = pci_read32((uint8_t)bus, (uint8_t)slot,
                                          (uint8_t)func, 0x00);
                if (!ac97_is_device((uint16_t)(cfg & 0xFFFFu),
                                    (uint16_t)(cfg >> 16))) {
                    continue;
                }

                uint16_t cmd = pci_read16((uint8_t)bus, (uint8_t)slot,
                                          (uint8_t)func, 0x04);
                pci_write16((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04,
                            (uint16_t)(cmd | 0x0005u));

                uint32_t bar0 = pci_read32((uint8_t)bus, (uint8_t)slot,
                                           (uint8_t)func, 0x10);
                uint32_t bar1 = pci_read32((uint8_t)bus, (uint8_t)slot,
                                           (uint8_t)func, 0x14);
                if ((bar0 & 1u) == 0 || (bar1 & 1u) == 0) continue;

                g_nam = (uint16_t)(bar0 & 0xFFFEu);
                g_nabm = (uint16_t)(bar1 & 0xFFFEu);
                return 1;
            }
        }
    }
    return 0;
}

/* ============================================================
 * AC'97 初始化
 * ============================================================ */

static void ac97_write_nam(uint8_t reg, uint16_t value) {
    outw((uint16_t)(g_nam + reg), value);
    (void)inw((uint16_t)(g_nam + reg));
}

static void ac97_bdl_write(int index, uint32_t addr, uint16_t frames) {
    g_bdl[index].addr = addr;
    g_bdl[index].frames = frames;
    /* bit14 = BUP：欠载时输出静音而不是停机 */
    g_bdl[index].flags = 0x4000;
}

static void ac97_fill_silence(void) {
    for (int i = 0; i < AUDIO_DESC_COUNT; i++) {
        int16_t *buf = (int16_t *)(void *)(uintptr_t)(g_desc_phys + (uint32_t)i * AUDIO_DESC_BYTES);
        for (int k = 0; k < AUDIO_DESC_FRAMES * 2; k++) buf[k] = 0;
    }
}

static int ac97_start(void) {
    g_desc_phys = (uint32_t)(uintptr_t)g_desc_mem;

    g_bdl = g_bdl_mem;
    g_bdl_phys = (uint32_t)(uintptr_t)g_bdl_mem;

    for (int i = 0; i < AUDIO_DESC_COUNT; i++) {
        ac97_bdl_write(i, g_desc_phys + (uint32_t)i * AUDIO_DESC_BYTES,
                       AUDIO_DESC_FRAMES);
    }
    ac97_fill_silence();

    /* NAM：复位，主音量与 PCM 音量都开到 0 dB。 */
    ac97_write_nam(AC97_NAM_RESET, 0x0000);
    au_delay_ms(2);
    ac97_write_nam(AC97_NAM_MASTER_VOL, 0x0000);
    ac97_write_nam(AC97_NAM_PCM_VOL, 0x0000);
    /* 可变速率声卡会接受该寄存器；固定 48 kHz 的型号忽略它。 */
    ac97_write_nam(AC97_NAM_PCM_RATE, AUDIO_RATE);

    /* NABM：软复位，等待复位位清零。 */
    outl((uint16_t)(g_nabm + AC97_NABM_GLOB_CNT), 0x00000002u);
    for (int i = 0; i < 1000; i++) {
        if ((inl((uint16_t)(g_nabm + AC97_NABM_GLOB_CNT)) & 0x2u) == 0) break;
        au_delay_ms(1);
    }

    outl((uint16_t)(g_nabm + AC97_NABM_PO_BASE), g_bdl_phys);
    outb((uint16_t)(g_nabm + AC97_NABM_PO_LVI),
         (uint8_t)(AUDIO_DESC_COUNT - 1));
    outw((uint16_t)(g_nabm + AC97_NABM_PO_SR), 0x001Cu);
    outb((uint16_t)(g_nabm + AC97_NABM_PO_CR), 0x01u);

    g_fill_desc = 2;
    g_fill_off = 0;
    return 1;
}

int audio_init(const struct SlysKernelApi *api) {
    if (!api) return 0;
    g_kin = api;

    g_fifo = g_fifo_mem;
    g_decoder = (struct Mp3Decoder *)g_decoder_mem;
    if (mp3_decoder_size() > sizeof(g_decoder_mem)) return 0;
    mp3_decoder_init(g_decoder);
    g_frame = g_frame_mem;
    g_bdl = g_bdl_mem;

    g_ac97_found = 0;
    if (!ac97_find()) return 0;
    if (!ac97_start()) return 0;

    g_ac97_found = 1;

    g_kin->log("[AC97] codec=");
    drv_log_u32((uint32_t)inw((uint16_t)(g_nam + AC97_NAM_RESET)));
    g_kin->log(" master=");
    drv_log_u32((uint32_t)inw((uint16_t)(g_nam + AC97_NAM_MASTER_VOL)));
    g_kin->log(" pcm=");
    drv_log_u32((uint32_t)inw((uint16_t)(g_nam + AC97_NAM_PCM_VOL)));
    g_kin->log("\r\n");
    return 1;
}

int audio_available(void) {
    return g_ac97_found;
}

/* ============================================================
 * FIFO / 解码
 * ============================================================ */

static int fifo_free(void) {
    return (int)(AUDIO_FIFO_FRAMES - g_fifo_count);
}

static void fifo_clear(void) {
    g_fifo_head = 0;
    g_fifo_tail = 0;
    g_fifo_count = 0;
    g_src_phase = 0;
}

/* 在内存缓冲区中寻找第一个合法 MP3 帧头。 */
static int find_sync_mem(uint32_t *start) {
    if (g_music_len < 4) return 0;
    for (uint32_t i = 0; i + 4 <= g_music_len; i++) {
        if (mp3_header_valid(g_music + i)) {
            *start = i;
            return 1;
        }
    }
    return 0;
}

static int media_decode_one(void) {
    if (g_eof) return 0;
    if (g_music_pos + 4 > g_music_len) {
        g_eof = 1;
        return 0;
    }

    const uint8_t *frame = g_music + g_music_pos;
    struct Mp3FrameInfo info;

    if (!mp3_parse_header(frame, &info) ||
        info.frame_bytes < 4 || info.frame_bytes > 1441) {
        /* 损坏帧：在缓冲区里向后寻找下一个帧头。 */
        int found = 0;
        for (uint32_t i = g_music_pos + 1; i + 4 <= g_music_len; i++) {
            if (mp3_header_valid(g_music + i)) {
                g_music_pos = i;
                found = 1;
                break;
            }
        }
        if (!found) g_eof = 1;
        return 0;
    }

    if (g_music_pos + (uint32_t)info.frame_bytes > g_music_len) {
        g_eof = 1;
        return 0;
    }

    int ch = 0;
    int hz = 0;
    int n = mp3_decode_frame(g_decoder, frame, info.frame_bytes, g_pcm, &ch, &hz);
    g_music_pos += (uint32_t)info.frame_bytes;
    if (n <= 0) return 1;

    g_src_rate = (uint32_t)hz;

    if ((int)g_fifo_count + n > AUDIO_FIFO_FRAMES) return 1;

    for (int i = 0; i < n; i++) {
        int16_t l = g_pcm[i * ch];
        int16_t r = (ch == 2) ? g_pcm[i * 2 + 1] : l;
        g_fifo[g_fifo_tail * 2 + 0] = l;
        g_fifo[g_fifo_tail * 2 + 1] = r;
        g_fifo_tail = (g_fifo_tail + 1) % AUDIO_FIFO_FRAMES;
    }
    g_fifo_count += (uint32_t)n;
    return 1;
}

static void media_decode_more(void) {
    if (!g_music_len || g_state != 1) return;

    /* 后台任务会连续调用本函数；按 FIFO 水位解码，直到接近满
     * 或预算用完，避免 GUI 帧率低时供不上数据。 */
    uint32_t deadline = g_kin->now_ms() + 80u;
    int budget = 10;

    while (budget-- > 0 &&
           g_fifo_count < AUDIO_FIFO_FRAMES * 3u / 4u &&
           fifo_free() >= MP3_MAX_SAMPLES) {
        if (!media_decode_one()) break;
        if (g_kin->now_ms() >= deadline) break;
    }

    if (g_eof && g_fifo_count == 0) {
        g_state = 0;
        g_pos_base = g_duration_ms;
        g_pos_running = 0;
    }
}

/* ============================================================
 * 重采样与 DMA 填充
 * ============================================================ */

static int32_t apply_volume(int32_t v, int gain) {
    int32_t out = (v * gain) >> 8;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return out;
}

static int render_frames(int16_t *out, int want) {
    int gain = (g_volume * 256) / 100;
    int produced = 0;

    if (g_state != 1 || !g_fifo) {
        for (int i = 0; i < want * 2; i++) out[i] = 0;
        return want;
    }

    while (produced < want) {
        if (g_fifo_count == 0) {
            if (g_eof) {
                g_state = 0;
                g_pos_base = g_duration_ms;
                g_pos_running = 0;
                break;
            }
            g_underruns++;
            break;
        }

        int16_t l0 = g_fifo[g_fifo_head * 2 + 0];
        int16_t r0 = g_fifo[g_fifo_head * 2 + 1];
        int16_t l1 = l0;
        int16_t r1 = r0;
        if (g_fifo_count > 1) {
            uint32_t nx = (g_fifo_head + 1) % AUDIO_FIFO_FRAMES;
            l1 = g_fifo[nx * 2 + 0];
            r1 = g_fifo[nx * 2 + 1];
        }

        int32_t fl = (int32_t)l0 + ((int32_t)(l1 - l0) * (int32_t)g_src_phase) / AUDIO_RATE;
        int32_t fr = (int32_t)r0 + ((int32_t)(r1 - r0) * (int32_t)g_src_phase) / AUDIO_RATE;

        out[produced * 2 + 0] = (int16_t)apply_volume(fl, gain);
        out[produced * 2 + 1] = (int16_t)apply_volume(fr, gain);

        int32_t sl = out[produced * 2 + 0];
        int32_t sr = out[produced * 2 + 1];
        g_stat_sum += (uint64_t)(sl * sl) + (uint64_t)(sr * sr);
        g_stat_count += 2;

        uint32_t al = (uint32_t)(sl < 0 ? -sl : sl);
        uint32_t ar = (uint32_t)(sr < 0 ? -sr : sr);
        if (al > g_stat_peak) g_stat_peak = al;
        if (ar > g_stat_peak) g_stat_peak = ar;

        produced++;

        g_src_phase += g_src_rate;
        while (g_src_phase >= AUDIO_RATE && g_fifo_count > 0) {
            g_src_phase -= AUDIO_RATE;
            g_fifo_head = (g_fifo_head + 1) % AUDIO_FIFO_FRAMES;
            g_fifo_count--;
            g_consumed++;
        }
    }

    /* 数据不足时补静音，避免 DMA 描述符里残留上一轮音频。
     * 返回值固定为 want，让描述符始终完整轮转。 */
    for (int i = produced * 2; i < want * 2; i++) out[i] = 0;
    return want;
}

static void audio_hw_refill(void) {
    if (!g_ac97_found) return;

    static int last_civ = -1;
    static uint32_t last_change = 0;

    uint32_t now = g_kin->now_ms();
    int civ = inb((uint16_t)(g_nabm + AC97_NABM_PO_CIV)) % AUDIO_DESC_COUNT;

    if (civ != last_civ) {
        last_civ = civ;
        last_change = now;
    }

    int last_filled = -1;

    for (int guard = 0; guard < AUDIO_DESC_COUNT; guard++) {
        int idx = g_fill_desc;
        if (idx == civ || idx == ((civ + 1) % AUDIO_DESC_COUNT)) break;

        int16_t *dst = (int16_t *)(void *)(uintptr_t)(g_desc_phys +
                                                      (uint32_t)idx * AUDIO_DESC_BYTES);
        int room = AUDIO_DESC_FRAMES - g_fill_off;
        int made = render_frames(dst + g_fill_off * 2, room);
        g_fill_off += made;

        if (g_fill_off >= AUDIO_DESC_FRAMES) {
            g_fill_off = 0;
            g_fill_desc = (idx + 1) % AUDIO_DESC_COUNT;
            last_filled = idx;
        }

        if (made < room) break;
    }

    /* 只在本轮填完后滚动一次 LVI，避免每个描述符都写寄存器
     * 造成 AC'97 在中间停顿。 */
    if (last_filled >= 0) {
        outb((uint16_t)(g_nabm + AC97_NABM_PO_LVI), (uint8_t)last_filled);
    }

    /* 硬件停在 LVI 上超过 250ms 才重启，避免正常轮转时误触发。 */
    if (g_state == 1 && (now - last_change) > 250u) {
        outb((uint16_t)(g_nabm + AC97_NABM_PO_CR), 0x00);
        outb((uint16_t)(g_nabm + AC97_NABM_PO_CR), 0x01);
        int nciv = inb((uint16_t)(g_nabm + AC97_NABM_PO_CIV)) %
                   AUDIO_DESC_COUNT;
        g_fill_desc = (nciv + 2) % AUDIO_DESC_COUNT;
        g_fill_off = 0;
        last_change = now;
    }
}

void audio_pump(void) {
    media_decode_more();
    audio_hw_refill();
    audio_report_stats();
}

/* ============================================================
 * 媒体接口
 * ============================================================ */

static void release_track(void) {
    g_eof = 0;
    g_fifo_count = 0;
    g_fifo_head = 0;
    g_fifo_tail = 0;
    g_src_phase = 0;
    g_consumed = 0;
    g_duration_ms = 0;
    g_music_len = 0;
    g_music_pos = 0;
    g_state = 0;
    pos_stop();
}

void audio_stop(void) {
    release_track();
}

int audio_play(const char *path) {
    if (!path || !path[0] || !g_kin) return -1;

    release_track();

    int len = g_kin->read_file(path, g_music, (int)sizeof(g_music));
    if (len <= 4) return -1;

    g_music_len = (uint32_t)len;
    g_music_pos = 0;

    uint32_t start = 0;
    if (!find_sync_mem(&start)) {
        release_track();
        return -1;
    }
    g_music_pos = start;

    struct Mp3FrameInfo info;
    if (!mp3_parse_header(g_music + start, &info)) {
        release_track();
        return -1;
    }

    g_src_rate = (uint32_t)info.hz;
    g_bitrate = (uint32_t)info.bitrate_kbps;
    if (g_bitrate == 0) g_bitrate = 128;

    uint64_t bytes = (uint64_t)(g_music_len - start);
    g_duration_ms = (uint32_t)(bytes * 8u / g_bitrate);

    fifo_clear();
    g_state = 1;

    /* 预解码约 0.3 秒音频，后台任务接管后立刻有数据可送。 */
    for (int i = 0; i < 12; i++) {
        if (g_eof || fifo_free() < MP3_MAX_SAMPLES) break;
        if (!media_decode_one()) break;
    }

    pos_start();
    media_decode_more();
    return 0;
}

void audio_pause(int paused) {
    if (g_state == 0) return;

    if (paused) {
        pos_hold();
        g_state = 2;
    } else {
        g_state = 1;
        pos_resume();
    }
}

int audio_state(void) {
    return g_state;
}

uint32_t audio_position_ms(void) {
    uint32_t ms = g_pos_base;
    if (g_state == 1 && g_pos_running && g_kin) {
        ms += g_kin->now_ms() - g_pos_tick;
    }
    if (g_duration_ms && ms > g_duration_ms) ms = g_duration_ms;
    return ms;
}

uint32_t audio_duration_ms(void) {
    return g_duration_ms;
}

void audio_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_volume = volume;
}

int audio_volume(void) {
    return g_volume;
}

/* ============================================================
 * PC 扬声器
 * ============================================================ */

void audio_beep(uint32_t freq, uint32_t ms) {
    if (freq == 0) freq = 800;
    if (freq < 20) freq = 20;
    if (freq > 20000) freq = 20000;
    if (ms == 0) ms = 100;
    if (ms > 2000) ms = 2000;

    uint32_t divisor = 1193182u / freq;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;

    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)(divisor >> 8));

    uint8_t port = inb(0x61);
    outb(0x61, (uint8_t)(port | 0x03));

    au_delay_ms(ms);

    outb(0x61, (uint8_t)(inb(0x61) & 0xFC));
}

/* ============================================================
 * 驱动入口与后台任务
 * ============================================================ */

/* 一步 = 最多解码 2 帧 + 填一轮 DMA。
 *
 * 内核的 GUI 线程每帧都会查询播放状态与进度，这些接口要拿同一
 * 把锁。如果后台任务一次把 80ms 的解码预算全部做完再放锁，GUI
 * 就会在自旋锁上白白等 80ms，表现为拖动窗口掉帧。因此把预算
 * 拆成小步：一帧 MP3 约 26ms 音频、解码耗时在 1ms 量级，锁占用
 * 时间控制在毫秒级，解码吞吐仍远高于实时播放需求（2 帧 ≈ 52ms
 * 音频，每步只隔 2ms）。 */
static void audio_driver_step(void) {
    g_kin->lock();

    int budget = 2;
    while (budget-- > 0 && g_state == 1 &&
           g_fifo_count < AUDIO_FIFO_FRAMES * 3u / 4u &&
           fifo_free() >= MP3_MAX_SAMPLES) {
        if (!media_decode_one()) break;
    }

    if (g_state == 1 && g_eof && g_fifo_count == 0) {
        g_state = 0;
        g_pos_base = g_duration_ms;
        g_pos_running = 0;
    }

    audio_hw_refill();
    audio_report_stats();

    g_kin->unlock();

    /* 让出 CPU 给 GUI/应用任务，同时限制本任务的唤醒频率。 */
    g_kin->sleep_ms(2);
    g_kin->yield();
}

static void audio_driver_run(void) {
    for (;;) {
        audio_driver_step();
    }
}

static const struct SlysAudioDriver g_driver = {
    SLYS_AUDIO_ABI,
    audio_init,
    audio_available,
    audio_play,
    audio_stop,
    audio_pause,
    audio_state,
    audio_position_ms,
    audio_duration_ms,
    audio_set_volume,
    audio_volume,
    audio_pump,
    audio_beep,
    audio_driver_run
};

const struct SlysAudioDriver *slys_driver_entry(
    const struct SlysKernelApi *api) {
    if (!api) return 0;
    (void)audio_init(api);
    return &g_driver;
}
