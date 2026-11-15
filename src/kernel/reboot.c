/* ============================================================
 * reboot.c - SaylterOS Real Hardware Reboot
 * ============================================================ */

#include "reboot.h"
#include "io.h"

void reboot(void) {
    /* 1. 关中断 —— 防止 reboot 过程中被中断打断 */
    __asm__ volatile ("cli");

    /* 2. 等待 8042 输入缓冲空 (status register bit 1 = IBF) */
    /*    IBF=1 表示控制器还没准备好接收新命令/数据 */
    volatile int wait = 100000;
    while (wait-- > 0) {
        if (!(inb(0x64) & 0x02)) {
            break;
        }
        __asm__ volatile ("nop");
    }

    /* 3. 发送 Reset Command (0xFE) */
    /*    8042 收到 0xFE 后立即拉 CPU RESET# 引脚，整机硬复位 */
    outb(0x64, 0xFE);

    /* 4. 给硬件留一点时间复位 —— 正常情况下 CPU 已经被拉复位了 */
    /*    如果执行到这里，说明 8042 reset 没成功（极少数硬件） */
    wait = 1000000;
    while (wait-- > 0) {
        __asm__ volatile ("nop");
    }

    /* 5. 终极兜底 —— CLI;HLT 等外部复位键或电源重置 */
    __asm__ volatile ("hlt");
}
