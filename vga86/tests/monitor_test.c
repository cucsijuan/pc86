/* Unit tests for the monitors (no CPU, no card: the signal is made up). */
#include "monitor.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

#define PIXELS (VGA_OUT_WIDTH * VGA_OUT_HEIGHT)

static uint8_t rgb[PIXELS * 3], out[PIXELS * 3];

int main(void)
{
    vga_signal_t vga = {640, 400, 31469, 70086};
    monitor_t *mono = monitor_create(NULL);
    monitor_t *color = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_COLOR));
    monitor_t *cga = monitor_create(monitor_builtin_model(MONITOR_MODEL_CGA_COLOR));
    monitor_t *multi = monitor_create(monitor_builtin_model(MONITOR_MODEL_MULTISYNC));
    int i;

    memset(rgb, 0, sizeof(rgb));
    rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;           /* red */
    rgb[3] = 255; rgb[4] = 255; rgb[5] = 255;       /* white */

    CHECK(monitor_sense(mono) == VGA_SENSE_MONO && monitor_sense(color) == VGA_SENSE_COLOR, "ID pins: mono and color");
    CHECK(monitor_channels(mono) == 1 && monitor_channels(color) == 3, "mono gives 1 byte per pixel, color 3");

    CHECK(monitor_display(mono, &vga, rgb, 0.016f, out) == MONITOR_SHOWING && out[0] == 77 && out[1] == 255,
          "mono shows luminance: red -> 77, white -> 255");
    CHECK(monitor_display(color, &vga, rgb, 0.016f, out) == MONITOR_SHOWING && out[0] == 255 && out[1] == 0 && out[5] == 255,
          "color shows RGB");

    CHECK(monitor_display(cga, &vga, rgb, 0.016f, out) == MONITOR_OUT_OF_RANGE && out[0] == 0,
          "a 15 kHz CGA monitor cannot lock to the 31.5 kHz VGA signal");
    CHECK(monitor_model(cga)->connector == MONITOR_CONN_DIGITAL, "and it has the digital DE-9 connector");
    CHECK(monitor_accepts(multi, &vga), "a multisync monitor locks to VGA");

    /* phosphor persistence on the mono monitor (30 ms half-life) */
    monitor_display(mono, &vga, rgb, 0.016f, out);
    monitor_display(mono, NULL, NULL, 0.030f, out);
    CHECK(out[1] >= 126 && out[1] <= 129, "unplugged: the picture fades to half in one half-life");
    for (i = 0; i < 30; i++) monitor_display(mono, NULL, NULL, 0.030f, out);
    CHECK(out[1] == 0, "and then goes dark");

    monitor_set_power(color, 0);
    CHECK(monitor_display(color, &vga, rgb, 0.5f, out) == MONITOR_OFF && out[0] == 0, "switched off: nothing on screen");
    monitor_set_power(color, 1);
    CHECK(monitor_display(color, NULL, NULL, 0.016f, out) == MONITOR_NO_SIGNAL, "on but no cable: no signal");

    monitor_destroy(mono); monitor_destroy(color); monitor_destroy(cga); monitor_destroy(multi);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
