/* ============================================================
 * mouse.c - PS/2 指针设备驱动（8042 控制器第二端口）
 * ------------------------------------------------------------
 * 支持三类设备，全部走非阻塞轮询：
 *   1. Synaptics 触控板：切成绝对模式（6 字节包），在内核里完成
 *      单指移动、轻点左键、双指轻点右键、三指轻点中键、双指滚动、
 *      轻点拖拽（tap-drag）、双击、手掌误触抑制、指针加速。
 *   2. IntelliMouse 滚轮鼠标（4 字节包）。
 *   3. 标准 3 字节 PS/2 鼠标。
 *
 * 对外接口不变：ps2_mouse_poll_ex() 每次返回一个事件
 * （相对位移 + 按键位图 + 滚轮格），上层 GUI 完全不用改。
 * 触控板产生的合成点击同样以按键事件的形式排队发出。
 * ============================================================ */

#include <stdint.h>
#include "io.h"
#include "mouse.h"
#include "i2c_tp.h"
#include "net.h"
#include "tty.h"

#define STATUS_PORT  0x64
#define DATA_PORT    0x60

/* ---------------- 触控板手势参数 ---------------- */
#define TP_TOUCH_Z        25      /* 压力 >= 该值视为手指接触 */
#define TP_PALM_WIDTH     10      /* W >= 该值视为手掌，整段接触忽略 */
#define TP_TAP_MS         260u    /* 轻点：按下时长上限（ms） */
#define TP_TAP_SLOP       140     /* 轻点：允许的最大移动（触控板单位，约 2mm） */
#define TP_DBL_MS         380u    /* 两次轻点的双击窗口（ms） */
#define TP_DBL_DIST       700     /* 双击两点最大距离（触控板单位，约 11mm） */
#define TP_SCROLL_STEP    200     /* 双指滚动：每 200 单位（约 3mm）发一格滚轮 */
#define TP_GAIN_SLOW      90      /* Q8 增益：0.35 px/单位（慢速） */
#define TP_GAIN_MID       120     /* 0.47 px/单位（中速） */
#define TP_GAIN_FAST      170     /* 0.66 px/单位（快速） */
#define TP_MAX_STEP       120     /* 单事件最大位移，受 int8 输出限制 */

/* ---------------- 事件队列 ---------------- */
typedef struct {
    int8_t  dx;
    int8_t  dy;
    int8_t  wheel;
    uint8_t buttons;      /* bit0 左 / bit1 右 / bit2 中 */
} MOUSE_EVT;

#define MOUSE_Q_SIZE  64
static MOUSE_EVT g_q[MOUSE_Q_SIZE];
static int g_q_head = 0;      /* 写入位置 */
static int g_q_tail = 0;      /* 读取位置 */

static void ev_push(int dx, int dy, int wheel, uint8_t buttons) {
    int next = (g_q_head + 1) & (MOUSE_Q_SIZE - 1);

    if (next == g_q_tail) return;      /* 队列满：丢弃新事件，避免破坏按键顺序 */

    if (dx > TP_MAX_STEP) dx = TP_MAX_STEP;
    if (dx < -TP_MAX_STEP) dx = -TP_MAX_STEP;
    if (dy > TP_MAX_STEP) dy = TP_MAX_STEP;
    if (dy < -TP_MAX_STEP) dy = -TP_MAX_STEP;
    if (wheel > 127) wheel = 127;
    if (wheel < -127) wheel = -127;

    g_q[g_q_head].dx = (int8_t)dx;
    g_q[g_q_head].dy = (int8_t)dy;
    g_q[g_q_head].wheel = (int8_t)wheel;
    g_q[g_q_head].buttons = buttons;
    g_q_head = next;
}

/* ---------------- 触控板状态 ---------------- */
typedef struct {
    int      active;             /* 当前有手指接触 */
    int      fingers_prev;       /* 上一包的手指数量 */
    int      max_fingers;        /* 本次接触出现过的最大手指数 */
    int      have_last;          /* last_x/last_y 有效 */
    int      last_x, last_y;     /* 上一包坐标 */
    int      moved;              /* 本次接触累计移动量（单位） */
    int      tap_ok;             /* 本次接触是否仍可能产生轻点 */
    int      tap2;               /* 第二次按下：可能是双击或拖拽 */
    int      drag;               /* 正在合成左键拖拽 */
    int      discovered_phys;    /* 本次接触期间出现过物理按键 */
    int      palm;               /* 本次接触被判定为手掌 */
    int      scroll_ref_y;       /* 双指滚动参考点（上一包 y） */
    int      scroll_accum;       /* 双指滚动余量（单位） */
    int      notches;            /* 本次接触已发出的滚动格数 */
    int      acc_x, acc_y;       /* 光标子像素余量（Q8） */
    int      last_tap_valid;
    int      last_tap_x, last_tap_y;
    uint32_t down_ms;            /* 接触开始时间 */
    uint32_t last_tap_ms;        /* 上一次轻点完成时间 */
    uint8_t  last_out_buttons;   /* 最近一次发出的按键位图 */
} TP_STATE;

static TP_STATE g_tp;
static uint8_t g_is_touchpad = 0;
static uint8_t g_pkt_size = 3;          /* 3 / 4 / 6 字节包 */
static uint8_t g_legacy_pkt_size = 3;   /* 相对模式下的包长（6 字节模式失败时回退用） */
static uint8_t g_abs_mismatch = 0;      /* 绝对模式下连续收到的非包首字节数 */
static uint8_t g_mouse_pkt[6];
static uint8_t g_mouse_phase = 0;

static void tp_state_reset(void) {
    g_tp.active = 0;
    g_tp.fingers_prev = 0;
    g_tp.max_fingers = 0;
    g_tp.have_last = 0;
    g_tp.moved = 0;
    g_tp.tap_ok = 0;
    g_tp.tap2 = 0;
    g_tp.drag = 0;
    g_tp.discovered_phys = 0;
    g_tp.palm = 0;
    g_tp.scroll_accum = 0;
    g_tp.notches = 0;
    g_tp.acc_x = 0;
    g_tp.acc_y = 0;
    g_tp.last_tap_valid = 0;
    g_tp.down_ms = 0;
    g_tp.last_tap_ms = 0;
    g_tp.last_out_buttons = 0;
    g_mouse_phase = 0;
    g_abs_mismatch = 0;
    g_q_head = 0;
    g_q_tail = 0;
}

/* ============================================================
 * 8042 基础访问
 * ============================================================ */

/* 等 8042 input buffer 空（可以写 command/data 了） */
static void wait_input_empty(void) {
    /* status bit 1 = input buffer full → 等它清 0 */
    while (inb(STATUS_PORT) & 0x02) {
        __asm__ volatile ("nop");
    }
}

/* 等 output buffer 有数据 → 读走返回 */
static uint8_t wait_output_byte(void) {
    /* status bit 0 = output buffer full → 等它变 1。
     * 8042 的响应在真实硬件上是微秒级；两万次端口读在 QEMU 下
     * 约 20ms，既能覆盖慢速设备，也不会让一次无响应卡住近一秒。 */
    uint32_t tries = 20000;
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
    uint32_t tries = 20000;
    while (!(inb(STATUS_PORT) & 0x01)) {
        if (--tries == 0) return 0;
        __asm__ volatile ("nop");
    }
    *out = inb(DATA_PORT);
    return 1;
}

/* 连续读取最多 max 个字节，遇到超时停止；返回实际读到的个数。 */
static int read_output_bytes(uint8_t *buf, int max) {
    int n = 0;
    while (n < max) {
        uint8_t v;
        if (!read_output_byte(&v)) break;
        buf[n++] = v;
    }
    return n;
}

/* 发命令到鼠标本身：先 0x64 = 0xD4（下一个 byte 送第二端口），
 * 再 0x60 = mouse_cmd；鼠标返回 0xFA = ACK。
 * 0xFE 表示要求重发，按 PS/2 规范最多重试 4 次。 */
static uint8_t mouse_command(uint8_t mouse_cmd) {
    int retry;
    for (retry = 0; retry < 4; retry++) {
        uint8_t ack = 0;
        ctrl_command(0xD4);            /* next byte → mouse */
        ctrl_write_data(mouse_cmd);    /* send cmd */
        if (!read_output_byte(&ack)) return 0;
        if (ack == 0xFA) return 1;
        if (ack != 0xFE) return 0;
    }
    return 0;
}

/* 带一个参数字节的鼠标命令（如 0xE8 分辨率、0xF3 采样率）。
 * 命令字节和参数字节各自会收到一个 ACK。 */
static uint8_t mouse_command_arg(uint8_t cmd, uint8_t arg) {
    return (uint8_t)(mouse_command(cmd) && mouse_command(arg));
}

/* ---------------- 键盘扫描码集同步（原有逻辑） ---------------- */

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

/* ============================================================
 * Synaptics 触控板识别与绝对模式
 * ------------------------------------------------------------
 * 识别：连续 4 次“设置分辨率 0”作为魔法敲击，再取设备 ID
 *       （0xF2）。普通鼠标只回 1 字节；触控板回 3 字节，其中
 *       第 2 字节是 0x47。
 * 绝对模式：Sliced Command（0xE7 入口 + 4 个 0xE8 两比特切片）
 *       写入模式字节，再发采样率 0x14 完成 Mode Byte 2 握手。
 *       模式位：0x80 绝对 / 0x40 80Hz / 0x04 关板载手势 /
 *               0x02 关板载轻点 / 0x01 W 模式。
 * ============================================================ */

static int syn_sliced(uint8_t value) {
    int i;

    /* Set Scaling 2:1 是切片命令入口 */
    if (!mouse_command(0xE7)) return 0;
    for (i = 6; i >= 0; i -= 2) {
        if (!mouse_command_arg(0xE8, (uint8_t)((value >> i) & 0x03)))
            return 0;
    }
    return 1;
}

static int syn_set_mode(uint8_t mode) {
    if (!syn_sliced(mode)) return 0;
    /* Mode Byte 2：采样率 20（0x14）是固定的结束握手 */
    return mouse_command_arg(0xF3, 0x14);
}

/* 返回 1 表示已切到绝对模式（g_pkt_size = 6） */
static int synaptics_probe(void) {
    uint8_t buf[3];
    int i, n;

    /* 探测期间关闭上报，避免数据包混进命令应答 */
    (void)mouse_command(0xF5);

    /* 魔法敲击：4 次 Set Resolution 0 */
    for (i = 0; i < 4; i++) {
        if (!mouse_command_arg(0xE8, 0x00)) return 0;
    }
    if (!mouse_command(0xF2)) return 0;

    n = read_output_bytes(buf, 3);
    if (!((n >= 1 && buf[0] == 0x47) ||
          (n >= 2 && buf[1] == 0x47))) {
        return 0;
    }

    /* 绝对 + 80Hz + 关板载手势 + 关板载轻点 + W 模式 */
    if (!syn_set_mode(0xC7)) {
        (void)syn_set_mode(0x00);      /* 尽量恢复相对模式 */
        (void)mouse_command_arg(0xE8, 0x02);   /* 恢复默认分辨率 */
        return 0;
    }

    g_is_touchpad = 1;
    g_pkt_size = 6;
    printk("[MOUSE] Synaptics touchpad: absolute mode\n");
    return 1;
}

/* IntelliMouse 4 字节（滚轮）协议探测。返回 1 = 成功。 */
static int intellimouse_probe(void) {
    if (mouse_command(0xF3) && mouse_command(200) &&
        mouse_command(0xF3) && mouse_command(100) &&
        mouse_command(0xF3) && mouse_command(80) &&
        mouse_command(0xF2)) {
        uint8_t id = 0;
        if (read_output_byte(&id) && id == 0x03) return 1;
    }
    return 0;
}

/* ============================================================
 * 触控板手势
 * ============================================================ */

/* 只发按键变化（无位移）。 */
static void tp_emit_buttons(uint8_t buttons) {
    if (buttons != g_tp.last_out_buttons) {
        ev_push(0, 0, 0, buttons);
        g_tp.last_out_buttons = buttons;
    }
}

/* 单指移动 → 屏幕相对位移（Q8 子像素累计 + 三段加速）。 */
static void tp_move(int adx, int ady, uint8_t buttons) {
    int ax = adx < 0 ? -adx : adx;
    int ay = ady < 0 ? -ady : ady;
    int gx = ax > 120 ? TP_GAIN_FAST : (ax > 45 ? TP_GAIN_MID : TP_GAIN_SLOW);
    int gy = ay > 120 ? TP_GAIN_FAST : (ay > 45 ? TP_GAIN_MID : TP_GAIN_SLOW);
    int dx, dy;

    g_tp.acc_x += adx * gx;
    g_tp.acc_y += ady * gy;

    dx = g_tp.acc_x >> 8;
    dy = g_tp.acc_y >> 8;
    g_tp.acc_x -= dx << 8;
    g_tp.acc_y -= dy << 8;

    if (dx == 0 && dy == 0) {
        tp_emit_buttons(buttons);
        return;
    }
    ev_push(dx, dy, 0, buttons);
    g_tp.last_out_buttons = buttons;
}

/* 统一的触控板原始数据入口：PS/2 绝对包与 I2C HID 上报都归一化到这里。
 *   x/y：触控板坐标（I2C 侧会先归一到 0..6143）
 *   z  ：压力（<=0 表示无接触）
 *   w  ：宽度/手指数编码（2=单指，0=双指，1=三指，>=10 视为手掌）
 *   phys：物理按键位图（bit0 左 / bit1 右 / bit2 中）
 *   now ：毫秒时间戳（用于轻点、双击判定） */
void tp_input_packet(int x, int y, int z, int w, int phys, uint32_t now) {
    int touch = (z >= TP_TOUCH_Z);
    int palm = (w >= TP_PALM_WIDTH);
    int fingers = 0;
    uint8_t buttons;

    /* W 编码（W 模式）：0 = 两指，1 = 三指，其余 = 单指（2 为常规单指） */
    if (touch && !palm) {
        if (w == 0) fingers = 2;
        else if (w == 1) fingers = 3;
        else fingers = 1;
    }

    /* 手掌落到已有接触上时，本次接触立即作废，避免误点击 */
    if (palm) {
        g_tp.tap_ok = 0;
        g_tp.tap2 = 0;
    }

    /* ClickPad 把主按键编码在“中键位”：接触期间按中键视为左键 */
    if (touch && (phys & 0x04)) {
        phys = (phys & ~0x04) | 0x01;
    }

    buttons = (uint8_t)phys;
    if (g_tp.drag) buttons |= 0x01;

    /* ---------------- 接触开始 ---------------- */
    if (fingers > 0 && !g_tp.active) {
        int ddx = x - g_tp.last_tap_x;
        int ddy = y - g_tp.last_tap_y;

        g_tp.active = 1;
        g_tp.palm = palm;
        g_tp.down_ms = now;
        g_tp.last_x = x;
        g_tp.last_y = y;
        g_tp.have_last = 1;
        g_tp.moved = 0;
        g_tp.tap_ok = !palm;
        g_tp.discovered_phys = 0;
        g_tp.scroll_accum = 0;
        g_tp.notches = 0;
        g_tp.max_fingers = fingers;
        g_tp.fingers_prev = fingers;
        g_tp.acc_x = 0;
        g_tp.acc_y = 0;
        g_tp.scroll_ref_y = y;

        /* 上一次轻点之后的快速再按：双击或拖拽。要求位置接近。 */
        g_tp.tap2 = 0;
        if (!palm && g_tp.last_tap_valid &&
            (uint32_t)(now - g_tp.last_tap_ms) <= TP_DBL_MS &&
            (ddx < 0 ? -ddx : ddx) + (ddy < 0 ? -ddy : ddy) <= TP_DBL_DIST) {
            g_tp.tap2 = 1;
            g_tp.tap_ok = 0;
        }

        if (fingers >= 2) {
            /* 双指静止轻点 = 右键：保留 tap_ok，滚动后由 notches 否决 */
            g_tp.scroll_ref_y = y;
        }
        return;
    }

    /* ---------------- 接触结束 ---------------- */
    if (fingers == 0 && g_tp.active) {
        uint32_t dur = now - g_tp.down_ms;
        /* tap_move：本次接触“静止且短促”，用于双击第二次点击；
         * tap 额外要求 tap_ok（滚动/手掌后已作废）。 */
        int tap_move = (!g_tp.palm &&
                        g_tp.discovered_phys == 0 &&
                        g_tp.moved < TP_TAP_SLOP &&
                        dur <= TP_TAP_MS);
        int tap = (g_tp.tap_ok && tap_move);

        if (g_tp.drag) {
            /* 拖拽结束：释放合成左键 */
            g_tp.drag = 0;
            tp_emit_buttons((uint8_t)phys);
        } else if (g_tp.tap2 && tap_move && g_tp.notches == 0) {
            /* 第二次轻点：完成双击 */
            ev_push(0, 0, 0, (uint8_t)(phys | 0x01));
            ev_push(0, 0, 0, (uint8_t)(phys & ~0x01));
            g_tp.last_out_buttons = (uint8_t)(phys & ~0x01);
            g_tp.last_tap_ms = now;
            g_tp.last_tap_x = x;
            g_tp.last_tap_y = y;
        } else if (tap && g_tp.notches == 0) {
            /* 单指轻点 = 左键；双指轻点 = 右键；三指轻点 = 中键 */
            int btn = (g_tp.max_fingers >= 3) ? 4 :
                      (g_tp.max_fingers == 2 ? 2 : 1);
            ev_push(0, 0, 0, (uint8_t)(phys | btn));
            ev_push(0, 0, 0, (uint8_t)(phys & ~btn));
            g_tp.last_out_buttons = (uint8_t)(phys & ~btn);
            if (btn == 1) {
                g_tp.last_tap_ms = now;
                g_tp.last_tap_valid = 1;
                g_tp.last_tap_x = x;
                g_tp.last_tap_y = y;
            }
        }

        g_tp.active = 0;
        g_tp.tap2 = 0;
        g_tp.drag = 0;
        g_tp.fingers_prev = 0;
        g_tp.max_fingers = 0;
        g_tp.have_last = 0;
        tp_emit_buttons((uint8_t)phys);
        return;
    }

    /* ---------------- 没有接触：只同步物理按键 ---------------- */
    if (!g_tp.active) {
        tp_emit_buttons((uint8_t)phys);
        return;
    }

    /* ---------------- 手掌：整段忽略 ---------------- */
    if (palm) {
        g_tp.discovered_phys |= (uint8_t)phys;
        tp_emit_buttons((uint8_t)phys);
        return;
    }

    g_tp.discovered_phys |= (uint8_t)phys;
    if (fingers > g_tp.max_fingers) g_tp.max_fingers = fingers;

    /* ---------------- 手指数量变化 ---------------- */
    if (fingers != g_tp.fingers_prev) {
        if (fingers >= 2) {
            g_tp.tap2 = 0;
            g_tp.scroll_ref_y = y;
            g_tp.scroll_accum = 0;
        } else if (fingers == 1) {
            /* 从双指回到单指：不产生点击，重新建立移动基准 */
            g_tp.tap_ok = 0;
            g_tp.have_last = 0;
        }
        g_tp.fingers_prev = fingers;
    }

    /* ---------------- 单指：移动 / 拖拽 / 双击判定 ---------------- */
    if (fingers == 1) {
        int adx = 0, ady = 0;

        if (g_tp.have_last) {
            adx = x - g_tp.last_x;
            ady = y - g_tp.last_y;
        }
        g_tp.last_x = x;
        g_tp.last_y = y;
        g_tp.have_last = 1;

        if (adx != 0 || ady != 0) {
            g_tp.moved += (adx < 0 ? -adx : adx) + (ady < 0 ? -ady : ady);
        }

        if (g_tp.tap2 && !g_tp.drag) {
            if (g_tp.moved > TP_TAP_SLOP) {
                /* 第二指点下后移动 → 转为拖拽，不动光标避免跳变 */
                g_tp.drag = 1;
                g_tp.tap2 = 0;
                adx = 0;
                ady = 0;
                tp_emit_buttons((uint8_t)(phys | 0x01));
            } else {
                return;      /* 双击判定窗口内保持光标不动 */
            }
        }

        if (adx != 0 || ady != 0) {
            tp_move(adx, ady, g_tp.drag ? (uint8_t)(phys | 0x01)
                                        : (uint8_t)phys);
        } else {
            tp_emit_buttons(buttons);
        }
        return;
    }

    /* ---------------- 双指 / 三指：滚动 ---------------- */
    if (fingers >= 2) {
        int dy = y - g_tp.scroll_ref_y;
        int notch = 0;

        g_tp.scroll_ref_y = y;
        g_tp.scroll_accum += dy;
        if (dy >= 0) g_tp.moved += dy; else g_tp.moved -= dy;

        /* 手指下移 → 滚轮向下（负值），与桌面滚轮语义一致 */
        while (g_tp.scroll_accum >= TP_SCROLL_STEP) {
            g_tp.scroll_accum -= TP_SCROLL_STEP;
            notch--;
        }
        while (g_tp.scroll_accum <= -TP_SCROLL_STEP) {
            g_tp.scroll_accum += TP_SCROLL_STEP;
            notch++;
        }

        if (notch != 0) {
            g_tp.notches += (notch < 0 ? -notch : notch);
            ev_push(0, 0, notch, buttons);
            g_tp.last_out_buttons = buttons;
        } else {
            tp_emit_buttons(buttons);
        }
    }
}

/* PS/2 Synaptics 6 字节绝对包 → 统一触控板入口 */
static void tp_process(const uint8_t *p, uint32_t now) {
    int x = ((p[3] & 0x10) << 8) | ((p[1] & 0x0F) << 8) | p[4];
    int y = ((p[3] & 0x20) << 7) | ((p[1] & 0xF0) << 4) | p[5];
    int z = p[2];
    int w = ((p[0] & 0x30) >> 2) | ((p[0] & 0x04) >> 1) | ((p[3] & 0x04) >> 2);
    int phys = 0;

    /* 按键位：b0 bit0=左，b0 bit1=右，中键是 b0/b3 的 bit0 异或
     * （b0 的 bit2 属于 W 宽度，不能当按键用） */
    if (p[0] & 0x01) phys |= 0x01;
    if (p[0] & 0x02) phys |= 0x02;
    if ((p[0] ^ p[3]) & 0x01) phys |= 0x04;

    tp_input_packet(x, y, z, w, phys, now);
}

/* 供 I2C HID 触控板驱动投递归一化事件 */
void ps2_mouse_post_event(int dx, int dy, int wheel, uint8_t buttons) {
    ev_push(dx, dy, wheel, buttons);
}

/* ============================================================
 * 包分发
 * ============================================================ */

/* 3/4 字节相对包（标准鼠标 / IntelliMouse）。 */
static void legacy_packet(void) {
    int dx, dy;
    int wheel = 0;

    /* 包首字节 bit3 必须为 1，bit6/7 是 X/Y 溢出 */
    if ((g_mouse_pkt[0] & 0x08) == 0) return;
    if (g_mouse_pkt[0] & 0xC0) return;

    dx = (int8_t)g_mouse_pkt[1];
    dy = -(int8_t)g_mouse_pkt[2];      /* PS/2 向上为负，屏幕向下为正 */

    if (g_pkt_size == 4) {
        uint8_t w = (uint8_t)(g_mouse_pkt[3] & 0x0F);
        if (w & 0x08) w = (uint8_t)(w | 0xF0);   /* 4 位有符号扩展 */
        wheel = (int8_t)w;
    }

    ev_push(dx, dy, wheel, (uint8_t)(g_mouse_pkt[0] & 0x07));
}

/* 把 8042 输出缓冲里所有鼠标字节吃干净，产出 0..N 个事件。 */
static void mouse_drain_hw(void) {
    for (;;) {
        uint8_t status = inb(STATUS_PORT);
        uint8_t b;

        if ((status & 0x21) != 0x21) return;   /* OBF=0 或 AUX=0 */
        b = inb(DATA_PORT);

        if (g_pkt_size == 6) {
            /* 绝对模式包首：bit7=1、bit6=0（其余位都不是包首标志），
             * 只在相位 0 处校验，避免把数据字节误当成包首。 */
            if (g_mouse_phase == 0 && (b & 0xC0) != 0x80) {
                /* 若设备其实没进绝对模式（例如切片命令不被接受），
                 * 连续一屏的相对包都是这种字节：自动退回相对模式，
                 * 宁可没有手势，也不能让光标完全不动。 */
                if (++g_abs_mismatch >= 12) {
                    g_abs_mismatch = 0;
                    g_is_touchpad = 0;
                    g_pkt_size = g_legacy_pkt_size;
                    printk("[MOUSE] touchpad absolute mode rejected, fallback\n");
                }
                continue;
            }
            g_abs_mismatch = 0;
            g_mouse_pkt[g_mouse_phase++] = b;
            if (g_mouse_phase == 6) {
                g_mouse_phase = 0;
                tp_process(g_mouse_pkt, net_now_ms());
            }
            continue;
        }

        g_mouse_pkt[g_mouse_phase++] = b;
        if (g_mouse_phase < g_pkt_size) continue;
        g_mouse_phase = 0;
        legacy_packet();
    }
}

/* ============================================================
 * 对外接口
 * ============================================================ */

uint8_t ps2_mouse_init(void) {
    static int initialized = 0;

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

    /* 4. 打开辅助端口 */
    ctrl_command(0xA8);
    flush_output();

    /* 5. 设备识别：触控板优先，其次 IntelliMouse，最后标准鼠标 */
    tp_state_reset();
    g_is_touchpad = 0;
    g_pkt_size = 3;

    if (!synaptics_probe()) {
        /* 魔法敲击会把普通鼠标的分辨率改成 0，恢复成 PS/2 默认值 2 */
        (void)mouse_command_arg(0xE8, 0x02);
        flush_output();
        if (intellimouse_probe()) {
            g_pkt_size = 4;
        }
        g_legacy_pkt_size = g_pkt_size;
    }
    flush_output();

    initialized = 1;
    if (!mouse_command(0xF4)) return 0;
    return 1;
}

void ps2_mouse_disable(void) {
    /* 先清掉输出缓冲再发命令：里面可能还躺着鼠标包或键盘扫描码，
     * 否则等待 ACK 时会读到旧数据并误判失败。 */
    flush_output();
    mouse_command(0xF5);           /* Disable data reporting */
    flush_output();

    /* 退出图形会话时清空手势状态，避免拖拽的合成左键残留到下一次会话 */
    tp_state_reset();
}

/* 非阻塞轮询：先取队列事件，队列空时再读 8042。
 * 返回 1 表示拿到一个事件；0 表示当前没有任何新数据。 */
uint8_t ps2_mouse_poll_ex(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons,
                          int8_t *out_wheel) {
    if (g_q_tail == g_q_head) {
        i2c_tp_poll();          /* I2C HID 触控板（笔记本内置） */
    }
    if (g_q_tail == g_q_head) {
        mouse_drain_hw();
    }
    if (g_q_tail == g_q_head) {
        return 0;
    }

    *out_dx = g_q[g_q_tail].dx;
    *out_dy = g_q[g_q_tail].dy;
    *out_buttons = g_q[g_q_tail].buttons;
    if (out_wheel) *out_wheel = g_q[g_q_tail].wheel;
    g_q_tail = (g_q_tail + 1) & (MOUSE_Q_SIZE - 1);
    return 1;
}

uint8_t ps2_mouse_poll(int8_t *out_dx, int8_t *out_dy, uint8_t *out_buttons) {
    int8_t wheel = 0;
    return ps2_mouse_poll_ex(out_dx, out_dy, out_buttons, &wheel);
}
