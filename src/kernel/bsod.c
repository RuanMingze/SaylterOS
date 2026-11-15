/* ============================================================
 * bsod.c - 内核崩溃蓝屏
 * ------------------------------------------------------------
 * 进入顺序经过刻意设计：
 *   1. cli + 屏蔽 8259 全部 IRQ —— 不再响应键盘、时钟、串口
 *   2. 关闭 VGA 硬件光标 —— 画面冻结
 *   3. 串口输出一行崩溃摘要 —— 保留可追溯性
 *   4. 整屏绘制蓝底亮白并写入诊断信息
 *   5. cli; hlt 永久循环
 * 整个过程不调用文件系统，因此不会在崩溃后产生半写数据。
 * ============================================================ */

#include "bsod.h"
#include "io.h"
#include "task.h"
#include "mem.h"

#define BSOD_COLS 80
#define BSOD_ROWS 25
#define BSOD_ATTR 0x1F          /* 蓝底亮白 */

extern void kserial_puts(const char *s);
extern void kserial_hex32(uint32_t v);

static uint16_t *const bsod_vga = (uint16_t *)0xB8000;

static int bsod_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int bsod_append(char *dst, int pos, const char *src) {
    while (*src) dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

static int bsod_append_dec(char *dst, int pos, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) dst[pos++] = tmp[--n];
    dst[pos] = '\0';
    return pos;
}

static int bsod_append_hex8(char *dst, int pos, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        dst[pos++] = hex[(v >> shift) & 0xFu];
    }
    dst[pos] = '\0';
    return pos;
}

static void bsod_text(int row, int col, const char *text) {
    if (row < 0 || row >= BSOD_ROWS) return;
    int i = 0;
    while (text[i] && col + i < BSOD_COLS) {
        bsod_vga[row * BSOD_COLS + col + i] =
            (uint16_t)(((uint16_t)BSOD_ATTR << 8) | (uint8_t)text[i]);
        i++;
    }
}

void bsod(uint32_t stop_code, const char *reason) {
    /* 1. 冻结中断源 */
    __asm__ volatile ("cli");
    outb(0x21, 0xFF);           /* 主 PIC 全部屏蔽 */
    outb(0xA1, 0xFF);           /* 从 PIC 全部屏蔽 */

    /* 2. 关闭硬件光标 */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);

    /* 3. 串口留下现场（只写一次，不逐字符刷屏） */
    kserial_puts("\r\n[BSOD] stop=0x");
    kserial_hex32(stop_code);
    kserial_puts(" reason=");
    kserial_puts(reason ? reason : "(none)");
    kserial_puts("\r\n");

    /* 4. 整屏蓝底 */
    for (int i = 0; i < BSOD_COLS * BSOD_ROWS; i++) {
        bsod_vga[i] = (uint16_t)(((uint16_t)BSOD_ATTR << 8) | ' ');
    }

    char line[BSOD_COLS + 1];

    int pos = 0;
    pos = bsod_append(line, pos, "SAYLTER OS  v0.3");
    bsod_text(3, 3, line);

    pos = 0;
    pos = bsod_append(line, pos, "A fatal exception has occurred. The system has been halted.");
    bsod_text(5, 3, line);

    pos = 0;
    pos = bsod_append(line, pos, "*** STOP: 0x");
    pos = bsod_append_hex8(line, pos, stop_code);
    bsod_text(7, 3, line);

    pos = 0;
    pos = bsod_append(line, pos, "Reason: ");
    pos = bsod_append(line, pos, reason ? reason : "(unknown)");
    bsod_text(9, 3, line);

    bsod_text(11, 3, "Hardware state protection:");

    pos = 0;
    pos = bsod_append(line, pos, "  - interrupts disabled, 8259 fully masked");
    bsod_text(12, 3, line);

    pos = 0;
    pos = bsod_append(line, pos, "  - all tasks frozen, no further scheduling");
    bsod_text(13, 3, line);

    pos = 0;
    pos = bsod_append(line, pos, "  - no disk writes performed after this point");
    bsod_text(14, 3, line);

    uint32_t total = 0, used = 0;
    mem_stats(&total, &used, 0, 0, 0, 0);

    pos = 0;
    pos = bsod_append(line, pos, "Heap: ");
    pos = bsod_append_dec(line, pos, used);
    pos = bsod_append(line, pos, " / ");
    pos = bsod_append_dec(line, pos, total);
    pos = bsod_append(line, pos, " bytes used, tasks alive: ");
    pos = bsod_append_dec(line, pos, (uint32_t)task_count_alive());
    bsod_text(16, 3, line);

    bsod_text(18, 3, "Press the power button or reset the machine to restart.");
    bsod_text(20, 3, "If this persists, check the serial log for the crash reason.");

    /* 5. 冻结 CPU —— 只能靠电源键或复位离开 */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
