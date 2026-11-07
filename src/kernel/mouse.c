/* ============================================================
 * mouse.c - PS/2 鼠标 (8042 controller, second port)
 * ------------------------------------------------------------
 * 纯轮询、非阻塞：ps2_mouse_poll() 返回 1 代表收齐一个 3 字节包
 * 协议：状态 | dx | dy (带符号扩展)
 * ============================================================ */

#include <stdint.h>
#include "io.h"
#include "mouse.h"

#define STATUS_PORT  0x64
#define DATA_PORT    0x60

/* 等 8042 input buffer 空（可以写 command/data 了） */
static void wait_input_empty(void) {
    /* status bit 1 = input buffer full → 等它清 0 */
    while (inb(STATUS_PORT) & 0x02) {
        __asm__ volatile ("nop");
    }
}

/* 等 output buffer 有数据 → 读走返回 */
static uint8_t wait_output_byte(void) {
    /* status bit 0 = output buffer full → 等它变 1 */
    uint32_t tries = 1000000;
    while (!(inb(STATUS_PORT) & 0x01)) {
        if (--tries == 0) return 0;   /* 超时返回 0 垃圾值 */
        __asm__ volatile ("nop");
    }
    return inb(DATA_PORT);
}

/* 发 command byte 到 8042 自身（不是到鼠标） */
static void ctrl_command(uint8_t cmd) {
    wait_input_empty();
    outb(STATUS_PORT, cmd);
}

/* 发 data byte 到 8042（上一个 command 指定了读哪个 config / 写字段） */
static void ctrl_write_data(uint8_t data) {
    wait_input_empty();
    outb(DATA_PORT, data);
}

/* 读 8042 上次 command 的返回值 */
static uint8_t ctrl_read_data(void) {
    return wait_output_byte();
}

/* 发命令到鼠标本身：先 0x64 = 0xD4（下一个 byte 送第二端口），
 * 再 0x60 = mouse_cmd；鼠标返回 0xFA = ACK */
static uint8_t mouse_command(uint8_t mouse_cmd) {
    ctrl_command(0xD4);            /* next byte → mouse */
    ctrl_write_data(mouse_cmd);    /* send cmd */
    uint8_t ack = wait_output_byte();
    return ack == 0xFA;
}

uint8_t ps2_mouse_init(void) {
    /* 1. Enable 8042 second port */
    ctrl_command(0xA8);

    /* 2. Read controller config byte, clear bit1 (disable mouse IRQ for polling) */
    ctrl_command(0x20);
    uint8_t cfg = ctrl_read_data();
    cfg &= ~0x02;                  /* bit1 = mouse IRQ; 清掉走 polling */
    ctrl_command(0x60);
    ctrl_write_data(cfg);

    /* 3. Enable data reporting (mouse 0xF4 → ACK 0xFA) */
    if (!mouse_command(0xF4)) return 0;

    return 1;
}

void ps2_mouse_disable(void) {
    mouse_command(0xF5);           /* Disable data reporting */
}

/* ------------------------------------------------------------
 * 非阻塞 poll：状态寄存器 bit0(OBF) + bit5(AUX=mouse)
 * 两个都为 1 才说明 8042 输出缓冲区里的是鼠标数据
 * 3 个 byte 必须连续读走，中间不能插其他 inb
 * 返回 1 = 成功拿到一个完整包；0 = 现在没数据
 * ------------------------------------------------------------ */
uint8_t ps2_mouse_poll(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons) {
    uint8_t status = inb(STATUS_PORT);
    if ((status & 0x21) != 0x21) {
        /* OBF=0 或 AUX=0：现在没鼠标数据 */
        return 0;
    }

    /* 三个 byte 连续读（中间不能插别的 inb，否则数据错位） */
    uint8_t b0 = inb(DATA_PORT);
    /* 这里不等——如果第二个 byte 还没好，下一次 poll 的时候缓冲区里肯定有了 */
    uint8_t s1 = inb(STATUS_PORT);
    uint8_t b1 = (s1 & 0x01) ? inb(DATA_PORT) : 0;
    uint8_t s2 = inb(STATUS_PORT);
    uint8_t b2 = (s2 & 0x01) ? inb(DATA_PORT) : 0;

    /* 符号扩展 */
    int32_t dx_raw = (int8_t)b1;   /* 直接 cast，高 bit 自动扩展 */
    int32_t dy_raw = (int8_t)b2;

    /* PS/2 是"向上移动 Y 为负"，屏幕坐标"向下为正"，翻转 Y */
    *out_dx = (int8_t)dx_raw;
    *out_dy = (int8_t)(-dy_raw);

    /* buttons: bit0=left, bit1=right, bit2=middle */
    *out_buttons = b0 & 0x07;
    return 1;
}
