/*
 * vga86 - monitors (the peripheral) for an in-game virtual PC.
 *
 * A monitor turns the signal that comes down the cable from a video card
 * (vga_render_rgb() + vga_signal()) into the picture the player sees. Models
 * differ in what they can show and how:
 *   - connector: an analog VGA monitor (DE-15) cannot be plugged into a
 *     digital CGA/EGA/MDA card (DE-9), and the other way around
 *   - horizontal frequency range: a monitor that cannot lock to the signal
 *     shows nothing (MONITOR_OUT_OF_RANGE)
 *   - color or monochrome: monochrome monitors show luminance
 *     (30% R + 59% G + 11% B), 1 byte per pixel; the engine tints it with the
 *     phosphor color. Color monitors give RGB, 3 bytes per pixel.
 *   - phosphor persistence: each pixel fades with the given half-life
 *
 * The monitor knows nothing about the machine; the host connects it to a card
 * and tells the card what the monitor ID pins say (vga_set_monitor_sense).
 */
#ifndef MONITOR86_H
#define MONITOR86_H

#include <stddef.h>
#include <stdint.h>

#include "vga.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MONITOR_CONN_VGA     = 0,     /* analog, DE-15 */
    MONITOR_CONN_DIGITAL = 1      /* TTL, DE-9 (MDA, CGA, EGA) */
};

typedef struct monitor_model {
    const char *name;
    uint8_t connector;            /* MONITOR_CONN_* */
    uint8_t color;                /* 0 monochrome (1 byte per pixel), 1 color (3 bytes per pixel) */
    uint32_t hfreq_min_hz, hfreq_max_hz;   /* horizontal sync it can lock to */
    float persistence_ms;         /* phosphor half-life; 0 = none */
} monitor_model_t;

/* Built-in models */
enum {
    MONITOR_MODEL_VGA_MONO = 0,   /* IBM 8503-style monochrome VGA, long persistence */
    MONITOR_MODEL_VGA_COLOR,      /* IBM 8512-style color VGA */
    MONITOR_MODEL_CGA_COLOR,      /* IBM 5153-style CGA (digital, 15.7 kHz) */
    MONITOR_MODEL_MULTISYNC,      /* analog multisync, 15-38 kHz */
    MONITOR_MODEL_COUNT
};
VGA86_API const monitor_model_t *monitor_builtin_model(int id);   /* NULL if out of range */

/* What monitor_display() showed */
enum {
    MONITOR_SHOWING      = 0,
    MONITOR_OFF          = 1,     /* power switch off */
    MONITOR_NO_SIGNAL    = 2,     /* no cable or nothing on it */
    MONITOR_OUT_OF_RANGE = 3      /* signal it cannot lock to */
};

typedef struct monitor monitor_t;

/* model is copied; NULL means MONITOR_MODEL_VGA_MONO. Created switched on. */
VGA86_API monitor_t *monitor_create(const monitor_model_t *model);
VGA86_API void       monitor_destroy(monitor_t *m);
VGA86_API const monitor_model_t *monitor_model(const monitor_t *m);

VGA86_API void monitor_set_power(monitor_t *m, int on);
VGA86_API int  monitor_power(const monitor_t *m);

/* VGA_SENSE_* this monitor's ID pins report to a card. */
VGA86_API int  monitor_sense(const monitor_t *m);
VGA86_API int  monitor_channels(const monitor_t *m);     /* 1 or 3 bytes per output pixel */

/* 1 if it can lock to this signal. */
VGA86_API int  monitor_accepts(const monitor_t *m, const vga_signal_t *sig);

/*
 * Shows one engine frame. sig and rgb (sig->width * sig->height * 3 bytes) are
 * the signal; pass NULL for both when nothing is connected. dt_seconds drives
 * the phosphor decay. out receives sig->width * sig->height * channels bytes
 * (640x400 when sig is NULL). Returns MONITOR_*.
 */
VGA86_API int  monitor_display(monitor_t *m, const vga_signal_t *sig, const uint8_t *rgb,
                               float dt_seconds, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
