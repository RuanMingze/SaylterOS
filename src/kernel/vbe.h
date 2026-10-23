#ifndef VBE_H
#define VBE_H

#include <stdint.h>

#define VBE_WIDTH  1024
#define VBE_HEIGHT 768
#define VBE_BPP    32
#define VBE_FB     0xE00000u

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint32_t pitch;
    uint32_t framebuffer;
    uint8_t  enabled;
} vbe_mode_info_t;

extern uint8_t vbe_bios_switch_mode(void);

void vbe_init_basic(void);
uint8_t vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp);
void vbe_fill_color(uint32_t color);
void vbe_fill_sky_blue(void);

#endif /* VBE_H */
