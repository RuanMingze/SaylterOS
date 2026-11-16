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

/* 读走 8042 输出缓冲里所有残留字节。
 * 读配置字节（0x20）之前必须调用：输出缓冲里若有键盘扫描码，
 * 会被当成配置字节读走，随后 0x60 写回时把键盘中断/时钟位关掉，
 * 表现为“进入 GUI 后键盘完全失效”。 */
static void flush_output(void) {
    uint32_t guard = 100000;
    while ((inb(STATUS_PORT) & 0x01) && guard--) {
        (void)inb(DATA_PORT);
    }
}

/* 等输出缓冲有数据并读走，成功返回 1；超时返回 0。
 * 与 wait_output_byte 的区别是能把"超时"和"读到 0"区分开。 */
static int read_output_byte(uint8_t *out) {
    uint32_t tries = 1000000;
    while (!(inb(STATUS_PORT) & 0x01)) {
        if (--tries == 0) return 0;
        __asm__ volatile ("nop");
    }
    *out = inb(DATA_PORT);
    return 1;
}

/* 向第一端口（键盘）发一个命令字节并等 ACK(0xFA)。
 * 0xD4 是"下一个字节给第二端口"，键盘命令直接写 0x60 即可。 */
static int kbd_cmd(uint8_t cmd) {
    wait_input_empty();
    outb(DATA_PORT, cmd);

    uint8_t ack = 0;
    if (!read_output_byte(&ack)) return 0;
    return ack == 0xFA;
}

/* 查询键盘当前扫描码集（0xF0 0x00）。
 * 返回 -1 无响应，0 键盘报错(0xFF)，1/2/3 为扫描码集号。 */
static int kbd_scanset(void) {
    if (!kbd_cmd(0xF0)) return -1;
    if (!kbd_cmd(0x00)) return -1;

    uint8_t set = 0;
    if (!read_output_byte(&set)) return -1;
    if (set == 0xFF) return 0;
    return (int)set;
}

/* 设置键盘扫描码集（0xF0 <set>） */
static int kbd_set_scanset(uint8_t set) {
    if (!kbd_cmd(0xF0)) return 0;
    return kbd_cmd(set);
}

/* IntelliMouse 4 字节协议开关：init 中根据设备 ID 决定 */
static int g_mouse_wheel_packet = 0;

uint8_t ps2_mouse_init(void) {
    static int initialized = 0;
    static int wheel_ready = 0;

    if (initialized) {
        /* 曾经退出过 GUI 会调用 ps2_mouse_disable 关闭上报，
         * 再次进入图形会话必须重新打开，否则鼠标完全不动。 */
        flush_output();
        mouse_command(0xF4);
        flush_output();
        return 1;
    }

    /* 0. 先关掉两个端口。
     *    8042 的输出缓冲只有一个字节，读配置字节（0x20）期间若键盘
     *    或鼠标往缓冲里写数据，读到的就不是配置字节而是扫描码/鼠标
     *    包，写回之后 translation（bit6）与键盘时钟（bit4）会被随机
     *    改写——表现正好是"进 GUI 以后键盘乱码"：控制器关闭了扫描码
     *    翻译，键盘原生 Set 2 码直通，而内核的解码表是按 Set 1 写的，
     *    于是 e(Set2 0x24) 变成 j，且松键的 0xF0 0x24 再出一个 j。 */
    ctrl_command(0xAD);            /* 关闭键盘端口 */
    ctrl_command(0xA7);            /* 关闭辅助端口 */
    flush_output();

    /* 1. 读控制器配置字节 */
    ctrl_command(0x20);
    uint8_t cfg = ctrl_read_data();

    /* 2. 固定语义，不依赖固件留下的随机值：
     *    bit0 = 1  键盘中断使能（TTY 依赖 IRQ1）
     *    bit1 = 0  鼠标中断关闭（鼠标走轮询）
     *    bit4 = 0  键盘时钟使能
     *    bit6 = 1  扫描码翻译开启：键盘原生 Set 2 由控制器翻成 Set 1，
     *              内核的 normal_map / shift_map 全部按 Set 1 编写 */
    cfg |= 0x01;
    cfg |= 0x40;
    cfg &= (uint8_t)~0x02;
    cfg &= (uint8_t)~0x10;

    ctrl_command(0x60);
    ctrl_write_data(cfg);
    flush_output();

    /* 3. 打开键盘端口，核对键盘真实扫描码集并保持一致 */
    ctrl_command(0xAE);
    flush_output();

    int set = kbd_scanset();
    if (set == 1) {
        /* 键盘自身已是 Set 1：必须关闭翻译，否则会被二次翻译成乱码 */
        cfg &= (uint8_t)~0x40;
        ctrl_command(0x60);
        ctrl_write_data(cfg);
    } else if (set == 2) {
        /* 键盘 Set 2 + 控制器翻译 = guest 收 Set 1，正是需要的组合 */
    } else {
        /* 无响应或报错：主动设成 Set 2，失败也不阻塞后续流程 */
        (void)kbd_set_scanset(0x02);
    }
    flush_output();

    /* 4. 打开辅助端口，启用鼠标数据上报 */
    ctrl_command(0xA8);
    flush_output();

    /* 5. 尝试启用 IntelliMouse 扩展（滚轮）。
     *    标准 PS/2 鼠标只有 3 字节包；连续设置采样率 200/100/80
     *    之后设备会切到 4 字节包，第 4 字节低 4 位是有符号滚轮值。
     *    设备 ID 读到 0x03 才代表切换成功，否则保持 3 字节协议。 */
    if (mouse_command(0xF3) && mouse_command(200) &&
        mouse_command(0xF3) && mouse_command(100) &&
        mouse_command(0xF3) && mouse_command(80)) {
        if (mouse_command(0xF2)) {
            uint8_t id = 0;
            if (read_output_byte(&id) && id == 0x03) {
                wheel_ready = 1;
            }
        }
    }
    flush_output();

    initialized = 1;
    g_mouse_wheel_packet = wheel_ready;
    if (!mouse_command(0xF4)) return 0;
    return 1;
}

void ps2_mouse_disable(void) {
    /* 先清掉输出缓冲再发命令：里面可能还躺着鼠标包或键盘扫描码，
     * 否则等待 ACK 时会读到旧数据并误判失败。 */
    flush_output();
    mouse_command(0xF5);           /* Disable data reporting */
    flush_output();
}

/* ------------------------------------------------------------
 * 非阻塞 poll：状态寄存器 bit0(OBF) + bit5(AUX=mouse)
 * 两个都为 1 才说明 8042 输出缓冲区里的是鼠标数据。
 *
 * 这里用"跨调用状态机"组包，而不是一次 inb 三个字节：
 *   - 鼠标包可能分几帧到达，第二/三字节没就绪时不能拿 0 填充，
 *     否则后续包会整体错位；
 *   - 键盘中断也读 0x60，但它现在会先检查 AUX 位，不会抢鼠标字节；
 *   - 包首字节 bit3 恒为 1，用它做自同步，错位时丢弃重来。
 * ------------------------------------------------------------ */
static uint8_t g_mouse_pkt[4];
static uint8_t g_mouse_phase = 0;

/* 带滚轮的轮询入口。out_wheel 返回 PS/2 第 4 字节的低 4 位符号扩展值：
 * 正数 = 向上滚（查看更早内容），负数 = 向下滚。3 字节设备恒为 0。 */
uint8_t ps2_mouse_poll_ex(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons,
                          int8_t *out_wheel) {
    int packet = g_mouse_wheel_packet ? 4 : 3;

    for (;;) {
        uint8_t status = inb(STATUS_PORT);
        if ((status & 0x21) != 0x21) {
            return 0;
        }

        uint8_t b = inb(DATA_PORT);
        g_mouse_pkt[g_mouse_phase++] = b;

        if (g_mouse_phase < packet) {
            continue;
        }
        g_mouse_phase = 0;

        /* 包首字节 bit3 必须为 1，bit6/7 是 X/Y 溢出 */
        if ((g_mouse_pkt[0] & 0x08) == 0) {
            continue;
        }
        if (g_mouse_pkt[0] & 0xC0) {
            continue;
        }

        int32_t dx_raw = (int8_t)g_mouse_pkt[1];
        int32_t dy_raw = (int8_t)g_mouse_pkt[2];

        int8_t wheel = 0;
        if (packet == 4) {
            uint8_t w = (uint8_t)(g_mouse_pkt[3] & 0x0F);
            if (w & 0x08) w = (uint8_t)(w | 0xF0);   /* 4 位有符号扩展 */
            wheel = (int8_t)w;
        }

        /* PS/2 是"向上移动 Y 为负"，屏幕坐标"向下为正"，翻转 Y */
        *out_dx = (int8_t)dx_raw;
        *out_dy = (int8_t)(-dy_raw);
        *out_buttons = g_mouse_pkt[0] & 0x07;
        if (out_wheel) *out_wheel = wheel;
        return 1;
    }
}

uint8_t ps2_mouse_poll(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons) {
    int8_t wheel = 0;
    return ps2_mouse_poll_ex(out_dx, out_dy, out_buttons, &wheel);
}
