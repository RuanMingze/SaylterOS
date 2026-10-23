/* ============================================================
 * keyboard.c - C 层键盘接口 (mailbox 模式)
 * ------------------------------------------------------------
 * pm.bin ISR 把扫描码写到 0xA000, 0xA001 设 has_data
 * C kernel 从这里轮询
 * ============================================================ */

#include <stdint.h>
#include "io.h"
#define KB_MAILBOX_SCANCODE (*(volatile uint8_t *)0xA000)
#define KB_MAILBOX_FLAG     (*(volatile uint8_t *)0xA001)

#define KB_LED_CAPS   0x04
#define KB_LED_NUM    0x02
#define KB_LED_SCROLL 0x01

static uint8_t shift_pressed = 0;
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

static int is_alpha_code(uint8_t code) {
    return (code >= 0x10 && code <= 0x19) ||
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
    if (KB_MAILBOX_FLAG == 0) return 0;
    KB_MAILBOX_FLAG = 0;

    uint8_t sc = KB_MAILBOX_SCANCODE;
    uint8_t code = sc & 0x7F;

    if (sc == 0xE0) {
        ext_prefix = 1;
        return 0;
    }

    if (ext_prefix) {
        ext_prefix = 0;
        if (sc & 0x80) return 0;

        switch (code) {
            case 0x4B: return (char)0xF1; /* left arrow */
            case 0x4D: return (char)0xF2; /* right arrow */
            case 0x52: return (char)0xF0; /* insert */
            case 0x53: return (char)0xF5; /* delete */
            case 0x47: return (char)0xF3; /* home */
            case 0x4F: return (char)0xF4; /* end */
            default:   return 0;
        }
    }

    if (sc & 0x80) {
        if (code == 0x2A || code == 0x36) {
            shift_pressed = 0;
        }
        return 0;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = 1;
        return 0;
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
            case 0x4B: return (char)0xF1;
            case 0x4D: return (char)0xF2;
            case 0x52: return (char)0xF0;
            case 0x53: return (char)0xF5;
            case 0x47: return (char)0xF3;
            case 0x4F: return (char)0xF4;
            default:   return 0;
        }
    }

    if (code > 0x5F) return 0;

    if (sc & 0x80) {
        if (code == 0x2A || code == 0x36) shift_pressed = 0;
        return 0;
    }

    if (code == 0x2A || code == 0x36) {
        shift_pressed = 1;
        return 0;
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
