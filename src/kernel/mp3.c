/* ============================================================
 * mp3.c - MPEG-1 Layer III 解码器（自研实现）
 * ------------------------------------------------------------
 * 处理流程与 ISO/IEC 11172-3 一致：
 *   帧头 -> 边信息 -> 比特池重组 -> 比例因子 -> Huffman
 *   -> 反量化 -> 立体声 -> 短块重排 -> 混叠抑制 -> IMDCT
 *   -> 多相合成滤波 -> 16 位 PCM
 *
 * 约束：
 *   - 只接受 MPEG-1 Layer III（32/44.1/48 kHz）；
 *   - 不使用 libc/libm，内存全部由调用方分配的 Mp3Decoder 持有；
 *   - 浮点使用 x87（内核未启用 SSE，构建参数已禁用 SSE 指令）。
 *
 * 常量表（Huffman 码表、窗函数、多相滤波系数）见 mp3_tables.h，
 * 它们是标准规定的数据；解码逻辑、状态机与内存布局均在本文件
 * 独立实现。
 * ============================================================ */

#include <stdint.h>

#include "mp3.h"
#include "mp3_tables.h"

/* ============================================================
 * 基础常量
 * ============================================================ */

#define MP3_MAX_MAIN   (2048 + 511)
#define MP3_MAX_FRAME  1441

#define MP3_SFB_MAX    40
#define MP3_SCF_MAX    40

#define MP3_BLOCK_SHORT 2
#define MP3_BLOCK_STOP  3

#define MP3_MODE_MONO   3
#define MP3_MODE_JOINT  1

/* 反量化增益常数：MAX_SCF = 255 + (-1)*4 - 210 = 41，
 * MAX_SCFI 向上对齐到 44。 */
#define MP3_MAX_SCF    41
#define MP3_MAX_SCFI   44

/* 边信息结构（MPEG-1） */
struct Mp3Granule {
    uint16_t part_23_length;
    uint16_t big_values;
    uint8_t  global_gain;
    uint8_t  scalefac_compress;
    uint8_t  block_type;
    uint8_t  mixed_block;
    uint8_t  table_select[3];
    uint8_t  region_count[3];
    uint8_t  subblock_gain[3];
    uint8_t  preflag;
    uint8_t  scalefac_scale;
    uint8_t  count1_table;
    uint8_t  scfsi;
    uint8_t  n_long_sfb;
    uint8_t  n_short_sfb;
    const uint8_t *sfb_width;
};

/* 解码器全部状态。没有静态全局变量，同一时刻可以并存多个实例。 */
struct Mp3Decoder {
    float    overlap[2][9 * 32];
    float    qmf_state[15 * 64];
    uint8_t  reservoir[511];
    int      reservoir_len;
    uint8_t  header[4];

    uint8_t  maindata[MP3_MAX_MAIN + 8];
    struct Mp3Granule gr[4];

    float    grbuf[2][576];
    float    scf[MP3_SCF_MAX];
    float    syn[33 * 64];
    uint8_t  ist_pos[2][MP3_SFB_MAX];

    int      sample_rate_index;
    int      channels;
};

/* ============================================================
 * 内存与数学小工具
 * ============================================================ */

static void mp3_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void mp3_memset(void *dst, int value, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)value;
}

static int mp3_clip16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

/* 把浮点样本转成 int16，按四舍五入远离零。 */
static int16_t mp3_scale_pcm(float sample) {
    if (sample >= 32766.5f) return 32767;
    if (sample <= -32767.5f) return -32768;

    int v = (int)(sample + 0.5f);
    if (v < 0) v--;
    return (int16_t)mp3_clip16(v);
}

/* 2 的 exp_q2/4 次幂乘以 y，整数指数拆分，避免 libm。 */
static float mp3_exp_q2(float y, int exp_q2) {
    while (exp_q2 > 0) {
        int e = exp_q2 > 120 ? 120 : exp_q2;
        y *= mp3_exp_frac[e & 3] * (float)(1 << 30 >> (e >> 2));
        exp_q2 -= e;
    }
    return y;
}

/* 正数表查询：mp3_pow43 的后半段是 0..128 的 |x|^(4/3)。 */
static float mp3_pow43_pos(int x) {
    if (x < 0) x = 0;
    if (x > 128) x = 128;
    return mp3_pow43[16 + x];
}

/* |x|^(4/3)，x 为 0..8191 的整数。小值查表，大值用二次修正。 */
static float mp3_pow43_calc(int x) {
    if (x < 129) return mp3_pow43_pos(x);

    int mult = 256;
    if (x < 1024) {
        mult = 16;
        x <<= 3;
    }

    int sign = (2 * x) & 64;
    float frac = (float)((x & 63) - sign) / (float)((x & ~63) + sign);
    float base = mp3_pow43_pos((x + sign) >> 6);
    return base * (1.0f + frac * (1.33333333f + frac * 0.22222222f)) * (float)mult;
}

/* ============================================================
 * 比特读取
 * ------------------------------------------------------------
 * 每次 peek 最多取 25 位：从当前比特位置读出 5 个字节拼成
 * 64 位窗口，再移位截取。比逐位循环快一个数量级，且边界安全。
 * ============================================================ */

struct Mp3Bits {
    const uint8_t *data;
    int bytes;
    int pos;
};

static void bits_init(struct Mp3Bits *bs, const uint8_t *data, int bytes) {
    bs->data = data;
    bs->bytes = bytes;
    bs->pos = 0;
}

static uint32_t bits_peek(const struct Mp3Bits *bs, int n) {
    if (n <= 0) return 0;
    int byte = bs->pos >> 3;
    int off = bs->pos & 7;

    uint64_t win = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t b = (byte + i < bs->bytes) ? bs->data[byte + i] : 0;
        win = (win << 8) | b;
    }

    /* 5 字节窗口先对齐到高 40 位，再跳过比特偏移。 */
    win <<= 24;
    win <<= off;
    return (uint32_t)(win >> (64 - n));
}

static void bits_skip(struct Mp3Bits *bs, int n) {
    bs->pos += n;
    if (bs->pos > bs->bytes * 8) bs->pos = bs->bytes * 8;
}

/* ============================================================
 * 帧头
 * ============================================================ */

static const uint16_t g_bitrate_mpeg1_l3[15] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
};

static const int g_sample_rate[3] = { 44100, 48000, 32000 };

static int hdr_layer(const uint8_t *h) {
    return (h[1] >> 1) & 3;
}

static int hdr_version(const uint8_t *h) {
    return (h[1] >> 3) & 3;
}

static int hdr_bitrate_index(const uint8_t *h) {
    return (h[2] >> 4) & 15;
}

static int hdr_rate_index(const uint8_t *h) {
    return (h[2] >> 2) & 3;
}

static int hdr_padding(const uint8_t *h) {
    return (h[2] >> 1) & 1;
}

static int hdr_mode(const uint8_t *h) {
    return (h[3] >> 6) & 3;
}

static int hdr_mode_ext(const uint8_t *h) {
    return (h[3] >> 4) & 3;
}

int mp3_header_valid(const uint8_t *hdr) {
    if (!hdr) return 0;
    if (hdr[0] != 0xFF) return 0;
    if ((hdr[1] & 0xE0) != 0xE0) return 0;
    if (hdr_version(hdr) != 3) return 0;          /* MPEG-1 */
    if (hdr_layer(hdr) != 1) return 0;            /* Layer III */
    if (hdr_bitrate_index(hdr) == 0) return 0;    /* free format 不支持 */
    if (hdr_bitrate_index(hdr) == 15) return 0;
    if (hdr_rate_index(hdr) == 3) return 0;
    return 1;
}

int mp3_parse_header(const uint8_t *hdr, struct Mp3FrameInfo *info) {
    if (!mp3_header_valid(hdr) || !info) return 0;

    int br = g_bitrate_mpeg1_l3[hdr_bitrate_index(hdr)];
    int hz = g_sample_rate[hdr_rate_index(hdr)];
    int pad = hdr_padding(hdr);

    info->frame_bytes = 144000 * br / hz + pad;
    info->channels = (hdr_mode(hdr) == MP3_MODE_MONO) ? 1 : 2;
    info->hz = hz;
    info->bitrate_kbps = br;
    return info->frame_bytes >= 4;
}

uint32_t mp3_decoder_size(void) {
    return (uint32_t)sizeof(struct Mp3Decoder);
}

int mp3_decoder_init(void *state) {
    if (!state) return 0;
    mp3_memset(state, 0, (uint32_t)sizeof(struct Mp3Decoder));
    return 1;
}

/* ============================================================
 * 边信息
 * ============================================================ */

static int side_info_size(int channels) {
    return channels == 1 ? 17 : 32;
}

/* 解析 MPEG-1 边信息，返回 main_data_begin；失败返回 -1。 */
static int read_side_info(struct Mp3Bits *bs, struct Mp3Decoder *d,
                          int channels, int rate_index) {
    /* MPEG-1 的比例因子频带表位于第 5..7 行：参考实现用
     * (rate_index + 6) - 1 得到行号，0..2 行属于 MPEG-2/2.5。 */
    int sfb_row = rate_index + 5;
    if (sfb_row > 7) sfb_row = 7;

    int main_data_begin = (int)bits_peek(bs, 9);
    bits_skip(bs, 9);

    int gr_count = channels == 1 ? 2 : 4;
    unsigned scfsi = bits_peek(bs, 7 + gr_count);
    bits_skip(bs, 7 + gr_count);

    int part_sum = 0;

    for (int i = 0; i < gr_count; i++) {
        struct Mp3Granule *g = &d->gr[i];

        if (channels == 1) scfsi <<= 4;

        g->part_23_length = (uint16_t)bits_peek(bs, 12);
        bits_skip(bs, 12);
        part_sum += g->part_23_length;

        g->big_values = (uint16_t)bits_peek(bs, 9);
        bits_skip(bs, 9);
        if (g->big_values > 288) return -1;

        g->global_gain = (uint8_t)bits_peek(bs, 8);
        bits_skip(bs, 8);

        g->scalefac_compress = (uint8_t)bits_peek(bs, 4);
        bits_skip(bs, 4);

        g->sfb_width = mp3_scf_long[sfb_row];
        g->n_long_sfb = 22;
        g->n_short_sfb = 0;

        int window_switch = (int)bits_peek(bs, 1);
        bits_skip(bs, 1);

        if (window_switch) {
            g->block_type = (uint8_t)bits_peek(bs, 2);
            bits_skip(bs, 2);
            if (g->block_type == 0) return -1;

            g->mixed_block = (uint8_t)bits_peek(bs, 1);
            bits_skip(bs, 1);

            g->region_count[0] = 7;
            g->region_count[1] = 255;
            g->region_count[2] = 255;

            if (g->block_type == MP3_BLOCK_SHORT) {
                scfsi &= 0x0F0F;
                if (!g->mixed_block) {
                    g->region_count[0] = 8;
                    g->sfb_width = mp3_scf_short[sfb_row];
                    g->n_long_sfb = 0;
                    g->n_short_sfb = 39;
                } else {
                    g->sfb_width = mp3_scf_mixed[sfb_row];
                    g->n_long_sfb = 8;
                    g->n_short_sfb = 30;
                }
            }

            unsigned tables = bits_peek(bs, 10);
            bits_skip(bs, 10);
            tables <<= 5;
            g->subblock_gain[0] = (uint8_t)bits_peek(bs, 3);
            bits_skip(bs, 3);
            g->subblock_gain[1] = (uint8_t)bits_peek(bs, 3);
            bits_skip(bs, 3);
            g->subblock_gain[2] = (uint8_t)bits_peek(bs, 3);
            bits_skip(bs, 3);

            g->table_select[0] = (uint8_t)(tables >> 10);
            g->table_select[1] = (uint8_t)((tables >> 5) & 31);
            g->table_select[2] = (uint8_t)(tables & 31);
        } else {
            g->block_type = 0;
            g->mixed_block = 0;

            unsigned tables = bits_peek(bs, 15);
            bits_skip(bs, 15);

            g->region_count[0] = (uint8_t)bits_peek(bs, 4);
            bits_skip(bs, 4);
            g->region_count[1] = (uint8_t)bits_peek(bs, 3);
            bits_skip(bs, 3);
            g->region_count[2] = 255;

            g->table_select[0] = (uint8_t)(tables >> 10);
            g->table_select[1] = (uint8_t)((tables >> 5) & 31);
            g->table_select[2] = (uint8_t)(tables & 31);
        }

        g->preflag = (uint8_t)bits_peek(bs, 1);
        bits_skip(bs, 1);
        g->scalefac_scale = (uint8_t)bits_peek(bs, 1);
        bits_skip(bs, 1);
        g->count1_table = (uint8_t)bits_peek(bs, 1);
        bits_skip(bs, 1);

        g->scfsi = (uint8_t)((scfsi >> 12) & 15);
        scfsi <<= 4;
    }

    if (part_sum + bs->pos > bs->bytes * 8 + main_data_begin * 8) {
        return -1;
    }
    return main_data_begin;
}

/* ============================================================
 * 比例因子
 * ============================================================ */

static void read_scalefactors(uint8_t *scf, uint8_t *prev, const uint8_t *size,
                              const uint8_t *count, struct Mp3Bits *bs,
                              unsigned scfsi) {
    for (int i = 0; i < 4 && count[i]; i++, scfsi *= 2) {
        int n = count[i];

        if (scfsi & 8) {
            for (int k = 0; k < n; k++) scf[k] = prev[k];
        } else {
            int b = size[i];
            if (b == 0) {
                for (int k = 0; k < n; k++) {
                    scf[k] = 0;
                    prev[k] = 0;
                }
            } else {
                for (int k = 0; k < n; k++) {
                    int v = (int)bits_peek(bs, b);
                    bits_skip(bs, b);
                    scf[k] = (uint8_t)v;
                    prev[k] = (uint8_t)v;
                }
            }
        }

        prev += n;
        scf += n;
    }

    scf[0] = 0;
    scf[1] = 0;
    scf[2] = 0;
}

static void decode_scalefactors(struct Mp3Decoder *d, struct Mp3Granule *g,
                                int ch, struct Mp3Bits *bs) {
    const uint8_t *partition = mp3_scf_partitions[
        (!!g->n_short_sfb) + (!g->n_long_sfb)];

    uint8_t size[4];
    uint8_t iscf[MP3_SFB_MAX];

    int part = mp3_scfc_decode[g->scalefac_compress];
    size[0] = size[1] = (uint8_t)(part >> 2);
    size[2] = size[3] = (uint8_t)(part & 3);

    read_scalefactors(iscf, d->ist_pos[ch], size, partition, bs, g->scfsi);

    int shift = g->scalefac_scale + 1;

    if (g->n_short_sfb) {
        int add = 3 - shift;
        for (int i = 0; i < g->n_short_sfb; i += 3) {
            iscf[g->n_long_sfb + i + 0] += (uint8_t)(g->subblock_gain[0] << add);
            iscf[g->n_long_sfb + i + 1] += (uint8_t)(g->subblock_gain[1] << add);
            iscf[g->n_long_sfb + i + 2] += (uint8_t)(g->subblock_gain[2] << add);
        }
    } else if (g->preflag) {
        for (int i = 0; i < 10; i++) {
            iscf[11 + i] += mp3_preamp[i];
        }
    }

    int stereo_adjust = (hdr_mode(d->header) == MP3_MODE_JOINT &&
                         hdr_mode_ext(d->header) & 2) ? 2 : 0;

    int gain_exp = (int)g->global_gain - 4 - 210 - stereo_adjust;
    float gain = mp3_exp_q2(2048.0f, MP3_MAX_SCFI - gain_exp);

    int n = g->n_long_sfb + g->n_short_sfb;
    for (int i = 0; i < n; i++) {
        d->scf[i] = mp3_exp_q2(gain, (int)iscf[i] << shift);
    }
}

/* ============================================================
 * Huffman 解码
 * ============================================================ */

/* 取一个 Huffman 值对应的 |x|^(4/3)，并按符号施加。 */
static float huff_value(int v, int sign) {
    if (v == 0) return 0.0f;
    float m = (v < 129) ? mp3_pow43_pos(v) : mp3_pow43_calc(v);
    return sign ? -m : m;
}

/* 解码一对（两个值），写入 dst[0..1]。 */
static int huff_pair(struct Mp3Bits *bs, int table, int linbits,
                     float scale, float *dst) {
    const int16_t *codebook = mp3_huff_packed + mp3_huff_index[table];

    int leaf = codebook[bits_peek(bs, 5)];
    int width = 5;

    while (leaf < 0) {
        bits_skip(bs, width);
        width = leaf & 7;
        int idx = (int)bits_peek(bs, width) - (leaf >> 3);
        leaf = codebook[idx];
    }

    bits_skip(bs, leaf >> 8);

    for (int j = 0; j < 2; j++) {
        int lsb = leaf & 15;

        if (linbits && lsb == 15) {
            int extra = (int)bits_peek(bs, linbits);
            bits_skip(bs, linbits);
            int sign = (int)bits_peek(bs, 1);
            bits_skip(bs, 1);
            dst[j] = huff_value(15 + extra, sign) * scale;
        } else {
            int sign = lsb ? (int)bits_peek(bs, 1) : 0;
            if (lsb) bits_skip(bs, 1);

            if (lsb == 0) {
                dst[j] = 0.0f;
            } else if (sign) {
                dst[j] = -mp3_pow43_pos(lsb) * scale;
            } else {
                dst[j] = mp3_pow43_pos(lsb) * scale;
            }
        }

        leaf >>= 4;
    }
    return 1;
}

/* 解码 big_values 段与 count1 段。limit 是本 granule 的比特上界。 */
static void decode_huffman(struct Mp3Decoder *d, struct Mp3Granule *g,
                           int ch, struct Mp3Bits *bs, int limit) {
    float *dst = d->grbuf[ch];
    const uint8_t *sfb = g->sfb_width;
    const float *scf = d->scf;

    int remaining = (int)g->big_values;
    int region = 0;
    float one = 0.0f;

    while (remaining > 0 && region < 3) {
        int table = g->table_select[region];
        int region_sfb = g->region_count[region];
        int linbits = mp3_linbits[table];
        region++;

        for (;;) {
            int np = *sfb++ / 2;
            if (np <= 0) break;

            int pairs = np < remaining ? np : remaining;
            one = *scf++;

            for (int p = 0; p < pairs; p++) {
                huff_pair(bs, table, linbits, one, dst);
                dst += 2;
            }

            remaining -= np;
            if (remaining <= 0) break;
            if (--region_sfb < 0) break;
        }
    }

    /* count1 段：每组 4 个 -1/0/1 值，使用固定的 A/B 表。
     * 参考实现用 1-big_val_cnt 处理 big_values 在最后一个频带
     * 中途结束的情况；这里保留同样的语义，避免比例因子错位。 */
    const uint8_t *ctab = g->count1_table ? mp3_huff_count_b
                                          : mp3_huff_count_a;
    int np = 1 - remaining;
    int done = 0;

    for (;;) {
        int leaf = ctab[bits_peek(bs, 4)];

        if (!(leaf & 8)) {
            int bits = leaf & 3;
            int extra = (int)bits_peek(bs, 4 + bits) & ((1 << bits) - 1);
            leaf = ctab[(leaf >> 3) + extra];
        }

        bits_skip(bs, leaf & 7);

        if (bs->pos > limit) break;

        for (int half = 0; half < 2; half++) {
            if (--np == 0) {
                np = *sfb++ / 2;
                if (np <= 0) {
                    done = 1;
                    break;
                }
                one = *scf++;
            }

            for (int s = half * 2; s < half * 2 + 2; s++) {
                if (leaf & (128 >> s)) {
                    int sign = (int)bits_peek(bs, 1);
                    bits_skip(bs, 1);
                    dst[s] = sign ? -one : one;
                } else {
                    dst[s] = 0.0f;
                }
            }
        }

        if (done) break;
        dst += 4;
    }
}

/* ============================================================
 * 立体声
 * ============================================================ */

static void midside_band(float *left, int n) {
    float *right = left + 576;
    for (int i = 0; i < n; i++) {
        float a = left[i];
        float b = right[i];
        left[i] = a + b;
        right[i] = a - b;
    }
}

static void intensity_band(float *left, int n, float kl, float kr) {
    for (int i = 0; i < n; i++) {
        left[576 + i] = left[i] * kr;
        left[i] = left[i] * kl;
    }
}

static void stereo_top_band(const float *right, const uint8_t *sfb,
                            int nbands, int max_band[3]) {
    max_band[0] = max_band[1] = max_band[2] = -1;

    for (int i = 0; i < nbands; i++) {
        int n = sfb[i];
        for (int k = 0; k < n; k += 2) {
            if (right[k] != 0.0f || right[k + 1] != 0.0f) {
                max_band[i % 3] = i;
                break;
            }
        }
        right += n;
    }
}

static void stereo_process(float *left, const uint8_t *ist_pos,
                           const uint8_t *sfb, const uint8_t *hdr,
                           int max_band[3]) {
    int ms = (hdr_mode(hdr) == MP3_MODE_JOINT) && (hdr_mode_ext(hdr) & 2);

    for (int i = 0; sfb[i]; i++) {
        int ipos = ist_pos[i];
        if (i > max_band[i % 3] && ipos < 7) {
            float kl = mp3_pan[2 * ipos];
            float kr = mp3_pan[2 * ipos + 1];
            float s = ms ? 1.41421356f : 1.0f;
            intensity_band(left, sfb[i], kl * s, kr * s);
        } else if (ms) {
            midside_band(left, sfb[i]);
        }
        left += sfb[i];
    }
}

static void intensity_stereo(struct Mp3Decoder *d, struct Mp3Granule *g) {
    int max_band[3];
    int n_sfb = g->n_long_sfb + g->n_short_sfb;

    stereo_top_band(d->grbuf[1], g->sfb_width, n_sfb, max_band);

    if (g->n_long_sfb) {
        int m = max_band[0];
        if (max_band[1] > m) m = max_band[1];
        if (max_band[2] > m) m = max_band[2];
        max_band[0] = max_band[1] = max_band[2] = m;
    }

    int max_blocks = g->n_short_sfb ? 3 : 1;
    for (int i = 0; i < max_blocks; i++) {
        int itop = n_sfb - max_blocks + i;
        int prev = itop - max_blocks;
        if (itop < 0 || itop >= MP3_SFB_MAX) continue;
        if (prev >= 0 && prev < MP3_SFB_MAX) {
            d->ist_pos[1][itop] = (max_band[i] >= prev)
                                  ? 3 : d->ist_pos[1][prev];
        } else {
            d->ist_pos[1][itop] = 3;
        }
    }

    stereo_process(d->grbuf[0], d->ist_pos[1], g->sfb_width, d->header, max_band);
}

/* ============================================================
 * 频域后处理
 * ============================================================ */

static void reorder_short(float *dst, float *scratch, const uint8_t *sfb) {
    float *src = dst;
    float *out = scratch;
    int len;

    while ((len = *sfb) != 0) {
        sfb += 3;
        for (int i = 0; i < len; i++) {
            out[0] = src[i];
            out[1] = src[i + len];
            out[2] = src[i + 2 * len];
            out += 3;
        }
        src += 3 * len;
    }

    int count = (int)(out - scratch);
    for (int i = 0; i < count; i++) dst[i] = scratch[i];
}

static void alias_reduce(float *grbuf, int bands) {
    for (int b = 0; b < bands; b++, grbuf += 18) {
        for (int i = 0; i < 8; i++) {
            float u = grbuf[18 + i];
            float v = grbuf[17 - i];
            grbuf[18 + i] = u * mp3_aa[0][i] - v * mp3_aa[1][i];
            grbuf[17 - i] = u * mp3_aa[1][i] + v * mp3_aa[0][i];
        }
    }
}

/* 9 点 DCT-III，就地变换。 */
static void dct9(float *y) {
    float s0 = y[0], s2 = y[2], s4 = y[4], s6 = y[6], s8 = y[8];
    float t0 = s0 + s6 * 0.5f;
    s0 -= s6;
    float t4 = (s4 + s2) * 0.93969262f;
    float t2 = (s8 + s2) * 0.76604444f;
    s6 = (s4 - s8) * 0.17364818f;
    s4 += s8 - s2;

    s2 = s0 - s4 * 0.5f;
    y[4] = s4 + s0;
    s8 = t0 - t2 + s6;
    s0 = t0 - t4 + t2;
    s4 = t0 + t4 - s6;

    float s1 = y[1], s3 = y[3], s5 = y[5], s7 = y[7];

    s3 *= 0.86602540f;
    t0 = (s5 + s1) * 0.98480775f;
    t4 = (s5 - s7) * 0.34202014f;
    t2 = (s1 + s7) * 0.64278761f;
    s1 = (s1 - s5 - s7) * 0.86602540f;

    s5 = t0 - s3 - t2;
    s7 = t4 - s3 - t0;
    s3 = t4 + s3 - t2;

    y[0] = s4 - s7;
    y[1] = s2 + s1;
    y[2] = s0 - s3;
    y[3] = s8 + s5;
    y[5] = s8 - s5;
    y[6] = s0 + s3;
    y[7] = s2 - s1;
    y[8] = s4 + s7;
}

static void imdct36(float *grbuf, float *overlap, const float *window,
                    int bands) {
    for (int b = 0; b < bands; b++, grbuf += 18, overlap += 9) {
        float co[9];
        float si[9];

        co[0] = -grbuf[0];
        si[0] = grbuf[17];

        for (int i = 0; i < 4; i++) {
            si[8 - 2 * i] = grbuf[4 * i + 1] - grbuf[4 * i + 2];
            co[1 + 2 * i] = grbuf[4 * i + 1] + grbuf[4 * i + 2];
            si[7 - 2 * i] = grbuf[4 * i + 4] - grbuf[4 * i + 3];
            co[2 + 2 * i] = -(grbuf[4 * i + 3] + grbuf[4 * i + 4]);
        }

        dct9(co);
        dct9(si);

        si[1] = -si[1];
        si[3] = -si[3];
        si[5] = -si[5];
        si[7] = -si[7];

        for (int i = 0; i < 9; i++) {
            float ovl = overlap[i];
            float sum = co[i] * mp3_twid18[9 + i] + si[i] * mp3_twid18[i];

            overlap[i] = co[i] * mp3_twid18[i] - si[i] * mp3_twid18[9 + i];
            grbuf[i] = ovl * window[i] - sum * window[9 + i];
            grbuf[17 - i] = ovl * window[9 + i] + sum * window[i];
        }
    }
}

static void idct3(float x0, float x1, float x2, float *dst) {
    float m1 = x1 * 0.86602540f;
    float a1 = x0 - x2 * 0.5f;
    dst[1] = x0 + x2;
    dst[0] = a1 + m1;
    dst[2] = a1 - m1;
}

static void imdct12(const float *x, float *dst, float *overlap) {
    float co[3];
    float si[3];

    idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
    idct3(x[15], x[12] - x[9], x[6] - x[3], si);
    si[1] = -si[1];

    for (int i = 0; i < 3; i++) {
        float ovl = overlap[i];
        float sum = co[i] * mp3_twid6[3 + i] + si[i] * mp3_twid6[i];

        overlap[i] = co[i] * mp3_twid6[i] - si[i] * mp3_twid6[3 + i];
        dst[i] = ovl * mp3_twid6[2 - i] - sum * mp3_twid6[5 - i];
        dst[5 - i] = ovl * mp3_twid6[5 - i] + sum * mp3_twid6[2 - i];
    }
}

static void imdct_short(float *grbuf, float *overlap, int bands) {
    for (; bands > 0; bands--, overlap += 9, grbuf += 18) {
        float tmp[18];
        for (int i = 0; i < 18; i++) tmp[i] = grbuf[i];
        for (int i = 0; i < 6; i++) grbuf[i] = overlap[i];

        imdct12(tmp, grbuf + 6, overlap + 6);
        imdct12(tmp + 1, grbuf + 12, overlap + 6);
        imdct12(tmp + 2, overlap, overlap + 6);
    }
}

static void change_sign(float *grbuf) {
    for (int b = 0, i; b < 32; b += 2) {
        grbuf += 18;
        for (i = 1; i < 18; i += 2) grbuf[i] = -grbuf[i];
        grbuf += 18;
    }
}

static void imdct_granule(float *grbuf, float *overlap, int block_type,
                          int n_long_bands) {
    if (n_long_bands) {
        imdct36(grbuf, overlap, mp3_mdct_window[0], n_long_bands);
        grbuf += 18 * n_long_bands;
        overlap += 9 * n_long_bands;
    }

    if (block_type == MP3_BLOCK_SHORT) {
        imdct_short(grbuf, overlap, 32 - n_long_bands);
    } else {
        imdct36(grbuf, overlap, mp3_mdct_window[block_type == MP3_BLOCK_STOP],
                32 - n_long_bands);
    }
}

/* ============================================================
 * 多相合成滤波
 * ============================================================ */

static void synth_pair(int16_t *pcm, int nch, const float *z) {
    float a = (z[14 * 64] - z[0]) * 29.0f;
    a += (z[1 * 64] + z[13 * 64]) * 213.0f;
    a += (z[12 * 64] - z[2 * 64]) * 459.0f;
    a += (z[3 * 64] + z[11 * 64]) * 2037.0f;
    a += (z[10 * 64] - z[4 * 64]) * 5153.0f;
    a += (z[5 * 64] + z[9 * 64]) * 6574.0f;
    a += (z[8 * 64] - z[6 * 64]) * 37489.0f;
    a += z[7 * 64] * 75038.0f;
    pcm[0] = mp3_scale_pcm(a);

    z += 2;
    a = z[14 * 64] * 104.0f;
    a += z[12 * 64] * 1567.0f;
    a += z[10 * 64] * 9727.0f;
    a += z[8 * 64] * 64019.0f;
    a += z[6 * 64] * -9975.0f;
    a += z[4 * 64] * -45.0f;
    a += z[2 * 64] * 146.0f;
    a += z[0 * 64] * -5.0f;
    pcm[16 * nch] = mp3_scale_pcm(a);
}

static void synth_slot(float *xl, int16_t *pcm, int nch, float *lins) {
    float *xr = xl + 576 * (nch - 1);
    int16_t *dst_r = pcm + (nch - 1);
    float *zlin = lins + 15 * 64;
    const float *w = mp3_synth_win;

    zlin[4 * 15 + 0] = xl[18 * 16];
    zlin[4 * 15 + 1] = xr[18 * 16];
    zlin[4 * 15 + 2] = xl[0];
    zlin[4 * 15 + 3] = xr[0];

    zlin[4 * 31 + 0] = xl[1 + 18 * 16];
    zlin[4 * 31 + 1] = xr[1 + 18 * 16];
    zlin[4 * 31 + 2] = xl[1];
    zlin[4 * 31 + 3] = xr[1];

    synth_pair(dst_r, nch, lins + 4 * 15 + 1);
    synth_pair(dst_r + 32 * nch, nch, lins + 4 * 15 + 64 + 1);
    synth_pair(pcm, nch, lins + 4 * 15);
    synth_pair(pcm + 32 * nch, nch, lins + 4 * 15 + 64);

    for (int i = 14; i >= 0; i--) {
        float a[4];
        float b[4];

        zlin[4 * i + 0] = xl[18 * (31 - i)];
        zlin[4 * i + 1] = xr[18 * (31 - i)];
        zlin[4 * i + 2] = xl[1 + 18 * (31 - i)];
        zlin[4 * i + 3] = xr[1 + 18 * (31 - i)];

        zlin[4 * (i + 16) + 0] = xl[1 + 18 * (1 + i)];
        zlin[4 * (i + 16) + 1] = xr[1 + 18 * (1 + i)];
        zlin[4 * (i - 16) + 2] = xl[18 * (1 + i)];
        zlin[4 * (i - 16) + 3] = xr[18 * (1 + i)];

        for (int j = 0; j < 4; j++) {
            a[j] = 0.0f;
            b[j] = 0.0f;
        }

        for (int k = 0; k < 8; k++) {
            float w0 = *w++;
            float w1 = *w++;
            float *vz = &zlin[4 * i - k * 64];
            float *vy = &zlin[4 * i - (15 - k) * 64];

            if (k == 0) {
                for (int j = 0; j < 4; j++) {
                    b[j] = vz[j] * w1 + vy[j] * w0;
                    a[j] = vz[j] * w0 - vy[j] * w1;
                }
            } else if (k & 1) {
                for (int j = 0; j < 4; j++) {
                    b[j] += vz[j] * w1 + vy[j] * w0;
                    a[j] += vy[j] * w1 - vz[j] * w0;
                }
            } else {
                for (int j = 0; j < 4; j++) {
                    b[j] += vz[j] * w1 + vy[j] * w0;
                    a[j] += vz[j] * w0 - vy[j] * w1;
                }
            }
        }

        dst_r[(15 - i) * nch] = mp3_scale_pcm(a[1]);
        dst_r[(17 + i) * nch] = mp3_scale_pcm(b[1]);
        pcm[(15 - i) * nch] = mp3_scale_pcm(a[0]);
        pcm[(17 + i) * nch] = mp3_scale_pcm(b[0]);
        dst_r[(47 - i) * nch] = mp3_scale_pcm(a[3]);
        dst_r[(49 + i) * nch] = mp3_scale_pcm(b[3]);
        pcm[(47 - i) * nch] = mp3_scale_pcm(a[2]);
        pcm[(49 + i) * nch] = mp3_scale_pcm(b[2]);
    }
}

static void dct32(float *grbuf, int slots) {
    for (int s = 0; s < slots; s++) {
        float t[4][8];
        float *y = grbuf + s;

        for (int i = 0; i < 8; i++) {
            float x0 = y[i * 18];
            float x1 = y[(15 - i) * 18];
            float x2 = y[(16 + i) * 18];
            float x3 = y[(31 - i) * 18];

            float t0 = x0 + x3;
            float t1 = x1 + x2;
            float t2 = (x1 - x2) * mp3_dct_sec[3 * i + 0];
            float t3 = (x0 - x3) * mp3_dct_sec[3 * i + 1];

            t[0][i] = t0 + t1;
            t[1][i] = (t0 - t1) * mp3_dct_sec[3 * i + 2];
            t[2][i] = t3 + t2;
            t[3][i] = (t3 - t2) * mp3_dct_sec[3 * i + 2];
        }

        for (int blk = 0; blk < 4; blk++) {
            float *x = t[blk];
            float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
            float x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7];
            float xt;

            xt = x0 - x7;
            x0 += x7;
            x7 = x1 - x6;
            x1 += x6;
            x6 = x2 - x5;
            x2 += x5;
            x5 = x3 - x4;
            x3 += x4;
            x4 = x0 - x3;
            x0 += x3;
            x3 = x1 - x2;
            x1 += x2;

            x[0] = x0 + x1;
            x[4] = (x0 - x1) * 0.70710677f;
            x5 += x6;
            x6 = (x6 + x7) * 0.70710677f;
            x7 += xt;
            x3 = (x3 + x4) * 0.70710677f;

            x5 -= x7 * 0.198912367f;
            x7 += x5 * 0.382683432f;
            x5 -= x7 * 0.198912367f;

            x0 = xt - x6;
            xt += x6;

            x[1] = (xt + x7) * 0.50979561f;
            x[2] = (x4 + x3) * 0.54119611f;
            x[3] = (x0 - x5) * 0.60134488f;
            x[5] = (x0 + x5) * 0.89997619f;
            x[6] = (x4 - x3) * 1.30656302f;
            x[7] = (xt - x7) * 2.56291556f;
        }

        for (int i = 0; i < 7; i++) {
            int o = 4 * i * 18;
            y[o + 0 * 18] = t[0][i];
            y[o + 1 * 18] = t[2][i] + t[3][i] + t[3][i + 1];
            y[o + 2 * 18] = t[1][i] + t[1][i + 1];
            y[o + 3 * 18] = t[2][i + 1] + t[3][i] + t[3][i + 1];
        }

        y[28 * 18] = t[0][7];
        y[29 * 18] = t[2][7] + t[3][7];
        y[30 * 18] = t[1][7];
        y[31 * 18] = t[3][7];
    }
}

static void synth_granule(struct Mp3Decoder *d, float *grbuf, int nch,
                          int16_t *pcm, float *lins) {
    for (int ch = 0; ch < nch; ch++) {
        dct32(grbuf + 576 * ch, 18);
    }

    for (int i = 0; i < 15 * 64; i++) lins[i] = d->qmf_state[i];
    for (int i = 0; i < 64 * 18; i++) lins[15 * 64 + i] = 0.0f;

    for (int i = 0; i < 18; i += 2) {
        synth_slot(grbuf + i, pcm + 32 * nch * i, nch, lins + i * 64);
    }

    if (nch == 1) {
        for (int i = 0; i < 15 * 64; i += 2) {
            d->qmf_state[i] = lins[18 * 64 + i];
        }
    } else {
        for (int i = 0; i < 15 * 64; i++) {
            d->qmf_state[i] = lins[18 * 64 + i];
        }
    }
}

/* ============================================================
 * 帧解码
 * ============================================================ */

static void save_reservoir(struct Mp3Decoder *d, struct Mp3Bits *bs) {
    int pos = (bs->pos + 7) / 8;
    int remain = bs->bytes - pos;

    if (remain < 0) remain = 0;
    if (remain > 511) {
        pos += remain - 511;
        remain = 511;
    }

    if (remain > 0) {
        for (int i = 0; i < remain; i++) d->reservoir[i] = d->maindata[pos + i];
    }
    d->reservoir_len = remain;
}

int mp3_decode_frame(void *state, const uint8_t *frame, int frame_bytes,
                     int16_t *pcm, int *out_channels, int *out_hz) {
    struct Mp3Decoder *d = (struct Mp3Decoder *)state;
    if (!d || !frame || !pcm || frame_bytes < 4) return 0;

    struct Mp3FrameInfo info;
    if (!mp3_parse_header(frame, &info)) return 0;
    if (frame_bytes < info.frame_bytes) return 0;

    int channels = info.channels;
    int frame_len = info.frame_bytes;
    if (out_channels) *out_channels = channels;
    if (out_hz) *out_hz = info.hz;

    for (int i = 0; i < 4; i++) d->header[i] = frame[i];

    int side = side_info_size(channels);
    int header_len = 4 + ((frame[1] & 1) ? 0 : 2);  /* protection_bit=0 时存在 CRC */
    int main_off = header_len + side;
    if (main_off > frame_len) return 0;

    struct Mp3Bits bs;
    bits_init(&bs, frame + header_len, frame_len - header_len);

    int main_data_begin = read_side_info(&bs, d, channels,
                                        hdr_rate_index(frame));
    if (main_data_begin < 0) return 0;

    /* ---- 比特池重组 ---- */
    int have = d->reservoir_len < main_data_begin ? d->reservoir_len
                                                  : main_data_begin;
    int main_bytes = have + (frame_len - main_off);

    if (main_bytes > MP3_MAX_MAIN) return 0;

    if (have > 0) {
        int start = d->reservoir_len - have;
        mp3_memcpy(d->maindata, d->reservoir + start, (uint32_t)have);
    }
    mp3_memcpy(d->maindata + have, frame + main_off,
               (uint32_t)(frame_len - main_off));
    for (int i = 0; i < 8; i++) d->maindata[main_bytes + i] = 0;

    struct Mp3Bits mb;
    bits_init(&mb, d->maindata, main_bytes);

    int reservoir_ok = d->reservoir_len >= main_data_begin;

    if (!reservoir_ok) {
        /* 第一帧或损坏帧：输出静音，但仍按规则更新池。 */
        for (int i = 0; i < MP3_MAX_SAMPLES * 2; i++) pcm[i] = 0;
        save_reservoir(d, &mb);
        return MP3_MAX_SAMPLES;
    }

    for (int gr = 0; gr < 2; gr++) {
        int base = gr * channels;

        /* 每个 granule 的频谱缓冲必须清零：Huffman 只写入实际
         * 编码的系数，残留上一帧数据会持续累积并导致爆音。 */
        for (int ch = 0; ch < channels; ch++) {
            for (int i = 0; i < 576; i++) d->grbuf[ch][i] = 0.0f;
        }

        for (int ch = 0; ch < channels; ch++) {
            struct Mp3Granule *g = &d->gr[base + ch];
            int limit = mb.pos + (int)g->part_23_length;
            if (limit > main_bytes * 8) limit = main_bytes * 8;

            decode_scalefactors(d, g, ch, &mb);
            decode_huffman(d, g, ch, &mb, limit);
            mb.pos = limit;
        }

        if (channels == 2) {
            if (hdr_mode(d->header) == MP3_MODE_JOINT &&
                (hdr_mode_ext(d->header) & 1)) {
                intensity_stereo(d, &d->gr[base]);
            } else if (hdr_mode(d->header) == MP3_MODE_JOINT &&
                       (hdr_mode_ext(d->header) & 2)) {
                midside_band(d->grbuf[0], 576);
            }
        }

        for (int ch = 0; ch < channels; ch++) {
            struct Mp3Granule *g = &d->gr[base + ch];
            int n_long_bands = g->mixed_block ? 2 : 0;
            /* MPEG-1 不存在 MPEG-2 的 32 kHz 短窗对齐特例。 */

            int aa_bands = 31;
            if (g->n_short_sfb) {
                aa_bands = n_long_bands - 1;
                reorder_short(d->grbuf[ch] + n_long_bands * 18, d->syn,
                              g->sfb_width + g->n_long_sfb);
            }

            alias_reduce(d->grbuf[ch], aa_bands);
            imdct_granule(d->grbuf[ch], d->overlap[ch], g->block_type,
                          n_long_bands);
            change_sign(d->grbuf[ch]);
        }

        synth_granule(d, d->grbuf[0], channels,
                      pcm + gr * 576 * channels, d->syn);
    }

    save_reservoir(d, &mb);
    return MP3_MAX_SAMPLES;
}
