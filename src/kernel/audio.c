/* ============================================================
 * audio.c - 内核音频影子层
 * ------------------------------------------------------------
 * 真正的 AC'97 驱动、MP3 解码与重采样已经移到
 * system\drivers\audio.slys。本文件只负责：
 *   1. 启动时加载驱动并取得导出表；
 *   2. 把 audio.* 接口转发给驱动；
 *   3. 维护"双击 MP3 的待播放路径"（纯内核状态）；
 *   4. 在 GUI 模式下创建后台音频任务，避免 GUI 主线程解码。
 *
 * 驱动缺失时所有接口安全降级为静音/停止，不会影响桌面启动。
 * ============================================================ */

#include <stdint.h>

#include "audio.h"
#include "slys.h"
#include "io.h"
#include "net.h"
#include "task.h"

#define AUDIO_DRIVER_PATH "system\\drivers\\audio.slys"

extern void kserial_puts(const char *s);
extern void kserial_dec32(uint32_t v);

static const struct SlysAudioDriver *g_drv = 0;
static int g_task_started = 0;

static char g_source[160];
static int  g_source_pending = 0;

static void copy_str(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void audio_init(void) {
    if (g_drv) return;

    void *entry = 0;
    char name[24];
    if (!slys_load(AUDIO_DRIVER_PATH, &entry, name, (int)sizeof(name))) {
        return;
    }

    SlysEntryFn fn = (SlysEntryFn)entry;
    const struct SlysAudioDriver *drv = fn(slys_kernel_api());
    if (!drv || drv->version != SLYS_AUDIO_ABI) {
        kserial_puts("[SLYS] audio ABI mismatch\r\n");
        return;
    }

    g_drv = drv;
}

void audio_start_task(void) {
    if (!g_drv || g_task_started) return;

    int id = task_spawn("audio.slys", g_drv->run, TASK_QUOTA_UNLIMITED);
    if (id < 0) return;

    g_task_started = 1;
    kserial_puts("[SLYS] audio task id=");
    kserial_dec32((uint32_t)id);
    kserial_puts("\r\n");
}

int audio_available(void) {
    if (!g_drv) return 0;
    slys_lock();
    int ok = g_drv->available();
    slys_unlock();
    return ok;
}

int audio_play(const char *path) {
    if (!g_drv || !path) return -1;
    slys_lock();
    int rc = g_drv->play(path);
    slys_unlock();
    return rc;
}

void audio_stop(void) {
    if (!g_drv) return;
    slys_lock();
    g_drv->stop();
    slys_unlock();
}

void audio_pause(int paused) {
    if (!g_drv) return;
    slys_lock();
    g_drv->pause(paused);
    slys_unlock();
}

int audio_state(void) {
    if (!g_drv) return AUDIO_STOPPED;
    slys_lock();
    int st = g_drv->state();
    slys_unlock();
    return st;
}

uint32_t audio_position_ms(void) {
    if (!g_drv) return 0;
    slys_lock();
    uint32_t ms = g_drv->position_ms();
    slys_unlock();
    return ms;
}

uint32_t audio_duration_ms(void) {
    if (!g_drv) return 0;
    slys_lock();
    uint32_t ms = g_drv->duration_ms();
    slys_unlock();
    return ms;
}

void audio_set_volume(int volume) {
    if (!g_drv) return;
    slys_lock();
    g_drv->set_volume(volume);
    slys_unlock();
}

int audio_volume(void) {
    if (!g_drv) return 70;
    slys_lock();
    int v = g_drv->volume();
    slys_unlock();
    return v;
}

void audio_pump(void) {
    /* GUI 模式由后台任务负责泵；TTY 自检/驱动未启动任务时才同步泵。 */
    if (!g_drv || g_task_started) return;
    slys_lock();
    g_drv->pump();
    slys_unlock();
}

void audio_set_source(const char *path) {
    copy_str(g_source, path ? path : "", (int)sizeof(g_source));
    g_source_pending = 1;
}

int audio_take_source(char *out, int cap) {
    if (!g_source_pending) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    copy_str(out, g_source, cap);
    g_source[0] = '\0';
    g_source_pending = 0;
    return 1;
}

/* PC 扬声器不依赖音频驱动，驱动缺失时仍然可用。 */
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

    uint32_t start = net_now_ms();
    while (net_now_ms() - start < ms) {
        __asm__ volatile ("pause");
    }

    outb(0x61, (uint8_t)(inb(0x61) & 0xFC));
}
