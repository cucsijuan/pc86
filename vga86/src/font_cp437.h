#ifndef VGA_FONT_CP437_H
#define VGA_FONT_CP437_H
#include <stdint.h>
/* Bit 7 of each byte is the leftmost pixel. */
extern const uint8_t vga_font_8x16[256][16];
extern const uint8_t vga_font_8x8[256][8];
#endif
