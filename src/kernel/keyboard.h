#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void kb_set_leds(uint8_t mask);
void kb_set_led_state(uint8_t caps_lock, uint8_t num_lock, uint8_t scroll_lock);
char kb_read_char(void);
char scancode_to_char(uint8_t sc);
uint8_t kb_caps_locked(void);
uint8_t kb_num_locked(void);

/* 丢弃环形缓冲里的全部待处理扫描码，并复位 Shift/Ctrl/扩展前缀。
 * GUI 会话接管键盘前调用，避免 TTY 阶段残留的按键在登录页重复出现。 */
void kb_flush(void);

#endif
