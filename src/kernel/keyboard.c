/* ============================================================
 * keyboard.c - C 层键盘接口（环形缓冲）
 * ------------------------------------------------------------
 * pm.asm 的 IRQ1 ISR 把扫描码写入 0xA010 起的 64 字节环形缓冲：
 *   head @ 0xA000（ISR 写）、tail @ 0xA002（本层写）。
 * 早期单字节 mailbox 在连续扫描码（Ctrl+S 会一次产生 4 个事件）
 * 时会互相覆盖导致修饰键错乱，因此改为环形缓冲。
 * C kernel 从这里轮询。
 * ============================================================ */

#include <stdint.h>
#include "io.h"
#include "usb_msc.h"
#include "net.h"

#define KB_HEAD      (*(volatile uint16_t *)0xA000)
#define KB_TAIL      (*(volatile uint16_t *)0xA002)
#define KB_BUF_BASE  0xA010
#define KB_BUF_MASK  63

#define KB_LED_CAPS   0x04
#define KB_LED_NUM    0x02
#define KB_LED_SCROLL 0x01

static uint8_t shift_pressed = 0;
static uint8_t ctrl_pressed = 0;
static uint8_t caps_lock = 0;
static uint8_t num_lock = 0;
static uint8_t scroll_lock = 0;
static uint8_t kb_led_state = 0;
static uint8_t ext_prefix = 0;

static void kb_wait_input_ready(void) {
    while (inb(0x64) & 0x02) {
        __asm__ volatile ("nop");
    }
}

static void kb_wait_output_ready(void) {
    while (!(inb(0x64) & 0x01)) {
        __asm__ volatile ("nop");
    }
}

static void kb_flush_input_buffer(void) {
    while (inb(0x64) & 0x01) {
        (void)inb(0x60);
    }
}

void kb_set_leds(uint8_t mask) {
    /* 先清掉任何旧的扫描码/ACK，避免上一轮状态切换影响下一轮输入。 */
    kb_flush_input_buffer();

    kb_wait_input_ready();
    outb(0x60, 0xED);
    kb_wait_output_ready();
    if (inb(0x60) != 0xFA) {
        return;
    }

    kb_wait_input_ready();
    outb(0x60, mask & 0x07);
    kb_wait_output_ready();
    if (inb(0x60) == 0xFA) {
        kb_led_state = mask & 0x07;
    }
}

void kb_set_led_state(uint8_t caps_lock_state, uint8_t num_lock_state, uint8_t scroll_lock_state) {
    uint8_t mask = (caps_lock_state ? KB_LED_CAPS : 0) |
                   (num_lock_state ? KB_LED_NUM : 0) |
                   (scroll_lock_state ? KB_LED_SCROLL : 0);
    kb_set_leds(mask);
}

uint8_t kb_caps_locked(void) {
    return caps_lock;
}

uint8_t kb_num_locked(void) {
    return num_lock;
}

void kb_flush(void) {
    /* 关中断再同步 head/tail，避免 ISR 在赋值中间写入新扫描码 */
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile ("cli");

    KB_TAIL = KB_HEAD;
    shift_pressed = 0;
    ctrl_pressed = 0;
    ext_prefix = 0;

    if (flags & 0x200u) {
        __asm__ volatile ("sti");
    }
}

/* ============================================================
 * USB HID 启动键盘 → PS/2 Set 1 扫描码注入
 * ------------------------------------------------------------
 * 2.4G 无线键盘的 USB 接收器按 HID 启动协议上报 8 字节报告。
 * 这里把 HID 键码翻译成 PS/2 Set 1 扫描码写进同一个环形缓冲，
 * 上层的 kb_read_char / 编辑器 / shell 不需要任何改动。
 * ============================================================ */

static void kb_ring_push(uint8_t sc) {
    uint64_t flags;
    uint16_t head;
    uint16_t tail;
    uint16_t next;

    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile ("cli");

    head = KB_HEAD;
    tail = KB_TAIL;
    next = (uint16_t)((head + 1) & KB_BUF_MASK);
    if (next != tail) {
        *(volatile uint8_t *)(uintptr_t)(KB_BUF_BASE + (head & KB_BUF_MASK)) = sc;
        KB_HEAD = next;
    }

    if (flags & 0x200u) {
        __asm__ volatile ("sti");
    }
}

/* HID Usage ID（Keyboard page 0x07）→ PS/2 Set 1 键码。
 * 返回 0 表示忽略；*ext = 1 表示需要先发 0xE0 前缀。 */
static uint8_t hid_usage_to_sc(uint8_t usage, uint8_t *ext) {
    static const uint8_t letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
        0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
        0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
    };
    static const uint8_t digits[10] = {
        0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A
    };
    static const uint8_t funcs[12] = {
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
        0x41, 0x42, 0x43, 0x44, 0x57, 0x58
    };

    *ext = 0;

    if (usage >= 0x04 && usage <= 0x1D) return letters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x27) return digits[usage - 0x1E];
    if (usage >= 0x3A && usage <= 0x45) return funcs[usage - 0x3A];

    switch (usage) {
    case 0x28: return 0x1C;             /* Enter */
    case 0x29: return 0x01;             /* Esc */
    case 0x2A: return 0x0E;             /* Backspace */
    case 0x2B: return 0x0F;             /* Tab */
    case 0x2C: return 0x39;             /* Space */
    case 0x2D: return 0x0C;             /* - */
    case 0x2E: return 0x0D;             /* = */
    case 0x2F: return 0x1A;             /* [ */
    case 0x30: return 0x1B;             /* ] */
    case 0x31: return 0x2B;             /* \ */
    case 0x32: return 0x2B;             /* 非美式 \| */
    case 0x33: return 0x27;             /* ; */
    case 0x34: return 0x28;             /* ' */
    case 0x35: return 0x29;             /* ` */
    case 0x36: return 0x33;             /* , */
    case 0x37: return 0x34;             /* . */
    case 0x38: return 0x35;             /* / */
    case 0x39: return 0x3A;             /* Caps Lock */
    case 0x46: *ext = 1; return 0x37;   /* Print Screen */
    case 0x49: *ext = 1; return 0x52;   /* Insert */
    case 0x4A: *ext = 1; return 0x47;   /* Home */
    case 0x4B: *ext = 1; return 0x49;   /* Page Up */
    case 0x4C: *ext = 1; return 0x53;   /* Delete */
    case 0x4D: *ext = 1; return 0x4F;   /* End */
    case 0x4E: *ext = 1; return 0x51;   /* Page Down */
    case 0x4F: *ext = 1; return 0x4D;   /* Right */
    case 0x50: *ext = 1; return 0x4B;   /* Left */
    case 0x51: *ext = 1; return 0x50;   /* Down */
    case 0x52: *ext = 1; return 0x48;   /* Up */
    case 0x53: return 0x45;             /* Num Lock */
    case 0x54: *ext = 1; return 0x35;   /* Keypad / */
    case 0x55: return 0x37;             /* Keypad * */
    case 0x56: return 0x4A;             /* Keypad - */
    case 0x57: return 0x4E;             /* Keypad + */
    case 0x58: *ext = 1; return 0x1C;   /* Keypad Enter */
    case 0x59: return 0x4F;             /* Keypad 1 */
    case 0x5A: return 0x50;             /* Keypad 2 */
    case 0x5B: return 0x51;             /* Keypad 3 */
    case 0x5C: return 0x4B;             /* Keypad 4 */
    case 0x5D: return 0x4C;             /* Keypad 5 */
    case 0x5E: return 0x4D;             /* Keypad 6 */
    case 0x5F: return 0x47;             /* Keypad 7 */
    case 0x60: return 0x48;             /* Keypad 8 */
    case 0x61: return 0x49;             /* Keypad 9 */
    case 0x62: return 0x52;             /* Keypad 0 */
    case 0x63: return 0x53;             /* Keypad . */
    case 0x64: return 0x56;             /* 非美式 <> */
    default: return 0;
    }
}

static void usb_emit_key(uint8_t usage, int down) {
    uint8_t ext = 0;
    uint8_t sc = hid_usage_to_sc(usage, &ext);

    if (!sc) return;
    if (ext) kb_ring_push(0xE0);
    kb_ring_push(down ? sc : (uint8_t)(sc | 0x80u));
}

static uint8_t usb_prev[8];
static uint8_t usb_shift;
static uint8_t usb_ctrl;
static uint8_t usb_alt;
static uint32_t usb_last_ms;
static uint32_t usb_repeat_ms;
static uint8_t usb_repeat_usage;
static uint8_t usb_repeat_on;

static void usb_emit_mods(uint8_t mods) {
    uint8_t shift = (mods & 0x22u) ? 1u : 0u;
    uint8_t ctrl = (mods & 0x11u) ? 1u : 0u;
    uint8_t alt = (mods & 0x44u) ? 1u : 0u;

    if (shift != usb_shift) {
        kb_ring_push(shift ? 0x2A : 0xAA);
        usb_shift = shift;
    }
    if (ctrl != usb_ctrl) {
        kb_ring_push(ctrl ? 0x1D : 0x9D);
        usb_ctrl = ctrl;
    }
    if (alt != usb_alt) {
        kb_ring_push(alt ? 0x38 : 0xB8);
        usb_alt = alt;
    }
}

static void usb_process_report(const uint8_t *rep) {
    uint8_t keys[6];
    int nk = 0;
    int i, j;
    int changed = 0;
    uint32_t now;

    for (i = 0; i < 6; i++) {
        if (rep[2 + i]) keys[nk++] = rep[2 + i];
    }

    usb_emit_mods(rep[0]);

    /* 先释放旧键，再按下新键 */
    for (i = 0; i < 6; i++) {
        uint8_t k = usb_prev[2 + i];
        int found = 0;

        if (!k) continue;
        for (j = 0; j < nk; j++) {
            if (keys[j] == k) { found = 1; break; }
        }
        if (!found) {
            usb_emit_key(k, 0);
            changed = 1;
        }
    }

    for (j = 0; j < nk; j++) {
        uint8_t k = keys[j];
        int found = 0;

        for (i = 0; i < 6; i++) {
            if (usb_prev[2 + i] == k) { found = 1; break; }
        }
        if (!found) {
            usb_emit_key(k, 1);
            changed = 1;
            if (k != 0x39 && k != 0x53) {   /* Caps/Num Lock 不做重复 */
                usb_repeat_usage = k;
                usb_repeat_on = 1;
            }
        }
    }

    for (i = 0; i < 8; i++) usb_prev[i] = rep[i];

    /* 重复键必须仍在按下的键里；否则换一个仍按住的键 */
    {
        int still = 0;
        for (j = 0; j < nk; j++) {
            if (keys[j] == usb_repeat_usage) still = 1;
        }
        if (!still) {
            usb_repeat_usage = 0;
            for (j = nk - 1; j >= 0; j--) {
                if (keys[j] != 0x39 && keys[j] != 0x53) {
                    usb_repeat_usage = keys[j];
                    break;
                }
            }
            usb_repeat_on = (usb_repeat_usage != 0);
        }
    }

    if (changed || nk == 0) {
        now = net_now_ms();
        usb_last_ms = now;
        usb_repeat_ms = now;
        if (nk == 0) usb_repeat_on = 0;
    }
}

/* 软件重复：启动协议只在状态变化时上报，按住键需要自己补发 */
static void usb_repeat_tick(void) {
    uint32_t now;

    if (!usb_repeat_on || !usb_repeat_usage) return;
    now = net_now_ms();
    if ((uint32_t)(now - usb_last_ms) < 500u) return;
    if ((uint32_t)(now - usb_repeat_ms) < 40u) return;
    usb_repeat_ms = now;
    usb_emit_key(usb_repeat_usage, 1);
}

/* 把 USB 键盘的待处理报告搬进环形缓冲 */
static void kb_usb_pump(void) {
    uint8_t rep[8];
    int n = 0;

    while (n++ < 4 && usb_kbd_poll_report(rep)) {
        usb_process_report(rep);
    }
    usb_repeat_tick();
}

static int is_alpha_code(uint8_t code) {    return (code >= 0x10 && code <= 0x19) ||
           (code >= 0x1E && code <= 0x26) ||
           (code >= 0x2C && code <= 0x32);
}

static char apply_caps_state(uint8_t code, char lower) {
    if (!is_alpha_code(code)) return lower;
    if (caps_lock ^ shift_pressed) {
        return (char)(lower - 32);
    }
    return lower;
}

static char apply_num_lock_state(uint8_t code) {
    if (!num_lock) {
        return 0;
    }

    switch (code) {
        case 0x47: return '7';
        case 0x48: return '8';
        case 0x49: return '9';
        case 0x4A: return '-';
        case 0x4B: return '4';
        case 0x4C: return '5';
        case 0x4D: return '6';
        case 0x4E: return '+';
        case 0x4F: return '1';
        case 0x50: return '2';
        case 0x51: return '3';
        case 0x52: return '0';
        case 0x53: return '.';
        default: return 0;
    }
}

static const char normal_map[128] = {
    [0x01] = 0, [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-',
    [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o',
    [0x19] = 'p', [0x1A] = '[', [0x1B] = ']', [0x1C] = '\r', [0x1D] = 0, [0x1E] = 'a',
    [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2A] = 0,
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x36] = 0,
    [0x37] = 0, [0x38] = 0, [0x39] = ' ', [0x3A] = 0, [0x3B] = 0, [0x3C] = 0, [0x3D] = 0,
    [0x3E] = 0, [0x3F] = 0, [0x40] = 0, [0x41] = 0, [0x42] = 0, [0x43] = 0, [0x44] = 0,
    [0x45] = 0, [0x46] = 0, [0x47] = 0, [0x48] = 0, [0x49] = 0, [0x4A] = 0, [0x4B] = 0,
    [0x4C] = 0, [0x4D] = 0, [0x4E] = 0, [0x4F] = 0, [0x50] = 0, [0x51] = 0, [0x52] = 0,
    [0x53] = 0, [0x54] = 0, [0x55] = 0, [0x56] = 0, [0x57] = 0, [0x58] = 0, [0x59] = 0,
    [0x5A] = 0, [0x5B] = 0, [0x5C] = 0, [0x5D] = 0, [0x5E] = 0, [0x5F] = 0
};

static const char shift_map[128] = {
    [0x01] = 0, [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')', [0x0C] = '_',
    [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O',
    [0x19] = 'P', [0x1A] = '{', [0x1B] = '}', [0x1C] = '\r', [0x1D] = 0, [0x1E] = 'A',
    [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H', [0x24] = 'J',
    [0x25] = 'K', [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2A] = 0,
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x36] = 0,
    [0x37] = 0, [0x38] = 0, [0x39] = ' ', [0x3A] = 0, [0x3B] = 0, [0x3C] = 0, [0x3D] = 0,
    [0x3E] = 0, [0x3F] = 0, [0x40] = 0, [0x41] = 0, [0x42] = 0, [0x43] = 0, [0x44] = 0,
    [0x45] = 0, [0x46] = 0, [0x47] = 0, [0x48] = 0, [0x49] = 0, [0x4A] = 0, [0x4B] = 0,
    [0x4C] = 0, [0x4D] = 0, [0x4E] = 0, [0x4F] = 0, [0x50] = 0, [0x51] = 0, [0x52] = 0,
    [0x53] = 0, [0x54] = 0, [0x55] = 0, [0x56] = 0, [0x57] = 0, [0x58] = 0, [0x59] = 0,
    [0x5A] = 0, [0x5B] = 0, [0x5C] = 0, [0x5D] = 0, [0x5E] = 0, [0x5F] = 0
};

char kb_read_char(void) {
    uint16_t tail = KB_TAIL;

    if (tail == KB_HEAD) {
        /* 环形缓冲为空时顺带轮询 USB 键盘，把 HID 报告转成扫描码 */
        kb_usb_pump();
        tail = KB_TAIL;
        if (tail == KB_HEAD) return 0;
    }

    uint8_t sc = *(volatile uint8_t *)(uintptr_t)(KB_BUF_BASE + (tail & KB_BUF_MASK));
    KB_TAIL = (uint16_t)((tail + 1) & KB_BUF_MASK);

    uint8_t code = sc & 0x7F;

    if (sc == 0xE0) {
        ext_prefix = 1;
        return 0;
    }

    if (ext_prefix) {
        ext_prefix = 0;
        if (sc & 0x80) return 0;

        switch (code) {
            case 0x48: return (char)0xF6; /* up arrow */
            case 0x4B: return (char)0xF1; /* left arrow */
            case 0x4D: return (char)0xF2; /* right arrow */
            case 0x50: return (char)0xF7; /* down arrow */
            case 0x52: return (char)0xF0; /* insert */
            case 0x53: return (char)0xF5; /* delete */
            case 0x47: return (char)0xF3; /* home */
            case 0x4F: return (char)0xF4; /* end */
            case 0x49: return (char)0xF8; /* page up */
            case 0x51: return (char)0xF9; /* page down */
            case 0x37: return (char)0xFB; /* Print Screen (E0 37) */
            default:   return 0;
        }
    }

    if (sc & 0x80) {
        if (code == 0x1D) {
            ctrl_pressed = 0;
        } else if (code == 0x2A || code == 0x36) {
            shift_pressed = 0;
        }
        return 0;
    }

    if (code == 0x1D) {
        ctrl_pressed = 1;
        return 0;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = 1;
        return 0;
    }

    /* Ctrl+Space 是输入法的标准切换键；空格不是字母，无法走下面的
     * Ctrl+字母映射，这里单独给出一个专用键码 0xFE。 */
    if (ctrl_pressed && code == 0x39) {
        return (char)0xFE;
    }

    /* Ctrl + 字母 → ASCII 控制码（Ctrl+A=1 ... Ctrl+Z=26）。
     * 编辑器依赖这套通用映射实现 Ctrl+S 保存 / Ctrl+X 退出等快捷键；
     * shell 的 readline 会把除 Ctrl+C 之外的控制字符当作不可打印输入忽略，
     * 因此扩充映射不会改变现有命令行行为。 */
    if (ctrl_pressed) {
        char base = normal_map[code];
        if (base >= 'a' && base <= 'z') {
            return (char)(base - 'a' + 1);
        }
    }

    if (code == 0x3A) {
        caps_lock = !caps_lock;
        kb_set_led_state(caps_lock, num_lock, scroll_lock);
        return 0;
    }

    if (code == 0x45) {
        num_lock = !num_lock;
        kb_set_led_state(caps_lock, num_lock, scroll_lock);
        return 0;
    }

    if (code >= 0x47 && code <= 0x53) {
        if (!num_lock) {
            switch (code) {
                case 0x47: return (char)0xF3; /* home */
                case 0x48: return (char)0xF6; /* up */
                case 0x4B: return (char)0xF1; /* left */
                case 0x4D: return (char)0xF2; /* right */
                case 0x4F: return (char)0xF4; /* end */
                case 0x50: return (char)0xF7; /* down */
                case 0x52: return (char)0xF0; /* insert */
                case 0x53: return (char)0xF5; /* delete */
                default:   return 0;
            }
        }

        char num_char = apply_num_lock_state(code);
        if (num_char != 0) {
            return num_char;
        }
    }

    if (code > 0x5F) return 0;

    if (is_alpha_code(code)) {
        char lower = normal_map[code];
        return apply_caps_state(code, lower);
    }

    if (shift_pressed) {
        return shift_map[code];
    }
    return normal_map[code];
}

char scancode_to_char(uint8_t sc) {
    uint8_t code = sc & 0x7F;

    if (sc == 0xE0) {
        ext_prefix = 1;
        return 0;
    }

    if (ext_prefix) {
        ext_prefix = 0;
        if (sc & 0x80) return 0;

        switch (code) {
            case 0x48: return (char)0xF6;
            case 0x4B: return (char)0xF1;
            case 0x4D: return (char)0xF2;
            case 0x50: return (char)0xF7;
            case 0x52: return (char)0xF0;
            case 0x53: return (char)0xF5;
            case 0x47: return (char)0xF3;
            case 0x4F: return (char)0xF4;
            case 0x49: return (char)0xF8;
            case 0x51: return (char)0xF9;
            case 0x37: return (char)0xFB; /* Print Screen */
            default:   return 0;
        }
    }

    if (code > 0x5F) return 0;

    if (sc & 0x80) {
        if (code == 0x1D) {
            ctrl_pressed = 0;
        } else if (code == 0x2A || code == 0x36) {
            shift_pressed = 0;
        }
        return 0;
    }

    if (code == 0x1D) {
        ctrl_pressed = 1;
        return 0;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = 1;
        return 0;
    }

    /* Ctrl+Space 是输入法的标准切换键；空格不是字母，无法走下面的
     * Ctrl+字母映射，这里单独给出一个专用键码 0xFE。 */
    if (ctrl_pressed && code == 0x39) {
        return (char)0xFE;
    }

    /* Ctrl + 字母 → ASCII 控制码（Ctrl+A=1 ... Ctrl+Z=26）。
     * 编辑器依赖这套通用映射实现 Ctrl+S 保存 / Ctrl+X 退出等快捷键；
     * shell 的 readline 会把除 Ctrl+C 之外的控制字符当作不可打印输入忽略，
     * 因此扩充映射不会改变现有命令行行为。 */
    if (ctrl_pressed) {
        char base = normal_map[code];
        if (base >= 'a' && base <= 'z') {
            return (char)(base - 'a' + 1);
        }
    }

    if (code == 0x3A) {
        caps_lock = !caps_lock;
        kb_set_led_state(caps_lock, num_lock, scroll_lock);
        return 0;
    }

    if (code == 0x45) {
        num_lock = !num_lock;
        kb_set_led_state(caps_lock, num_lock, scroll_lock);
        return 0;
    }

    if (code >= 0x47 && code <= 0x53) {
        if (!num_lock) {
            switch (code) {
                case 0x47: return (char)0xF3;
                case 0x48: return (char)0xF6;
                case 0x4B: return (char)0xF1;
                case 0x4D: return (char)0xF2;
                case 0x4F: return (char)0xF4;
                case 0x50: return (char)0xF7;
                case 0x52: return (char)0xF0;
                case 0x53: return (char)0xF5;
                default:   return 0;
            }
        }

        char num_char = apply_num_lock_state(code);
        if (num_char != 0) {
            return num_char;
        }
    }

    if (is_alpha_code(code)) {
        char lower = normal_map[code];
        return apply_caps_state(code, lower);
    }

    if (shift_pressed) {
        return shift_map[code];
    }
    return normal_map[code];
}
