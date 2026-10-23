#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void kb_set_leds(uint8_t mask);
void kb_set_led_state(uint8_t caps_lock, uint8_t num_lock, uint8_t scroll_lock);
char kb_read_char(void);
char scancode_to_char(uint8_t sc);
uint8_t kb_caps_locked(void);
uint8_t kb_num_locked(void);

#endif
