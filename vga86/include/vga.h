/*
 * vga86 - VGA card for an in-game virtual PC (monitors: monitor.h).
 *
 * Emulates the parts of a VGA card that programs see:
 *   - text mode 03h (80x25, 8x16 CP437 font, attributes, blink, cursor)
 *   - mode 13h (320x200, 256 colors, linear framebuffer at A000:0000)
 *   - DAC palette ports 3C7h/3C8h/3C9h, CRTC 3D4h/3D5h, input status 3DAh
 *     (vertical retrace at 70 Hz, horizontal blanking)
 *   - a subset of the video BIOS (INT 10h) and its BIOS data area variables
 *
 * The CPU writes video memory directly (B800:0000 in text mode, A000:0000 in
 * mode 13h); this device reads it from the machine's 1 MB memory when it
 * renders. It does not depend on any particular CPU core: the host wires
 * ports and INT 10h to it (see examples/vm86_glue.c).
 *
 * Output: the card's video signal, always 640x400. vga_render_rgb() gives it as
 * 8-bit RGB (what goes down the cable); a monitor (monitor.h) turns it into the
 * picture. vga_render() gives it directly as one byte of luminance per pixel,
 * as seen on a monochrome VGA monitor (30% R + 59% G + 11% B). Text mode is
 * 80x25 cells of 8x16 pixels; mode 13h is double-scanned to 2x2 pixels, like
 * real VGA.
 *
 * Anything a program can reach that is not supported is reported, never
 * silently emulated differently: vga_port_in/out return VGA_PORT_UNSUPPORTED
 * and vga_int10 returns 0, so the host can stop the machine with a clear error.
 */
#ifndef VGA86_H
#define VGA86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(VGA86_BUILD_DLL)
#  define VGA86_API __declspec(dllexport)
#elif defined(_WIN32) && defined(VGA86_USE_DLL)
#  define VGA86_API __declspec(dllimport)
#else
#  define VGA86_API
#endif

#define VGA_OUT_WIDTH  640
#define VGA_OUT_HEIGHT 400

/* Timing of the 70 Hz VGA modes (both 03h and 13h). */
#define VGA_FRAME_US   14268u     /* 1 / 70.086 Hz, in microseconds */
#define VGA_LINES      449u       /* total scanlines per frame */

typedef struct vga vga_t;

/* Timing of the signal on the video connector, for the monitor. */
typedef struct vga_signal {
    uint16_t width, height;       /* pixels delivered by vga_render*() */
    uint32_t hfreq_hz;            /* horizontal sync */
    uint32_t vfreq_mhz;           /* vertical sync, in millihertz */
} vga_signal_t;

/* What the monitor ID pins of the connector report (INT 10h AX=1A00h). */
enum {
    VGA_SENSE_NONE  = 0,          /* nothing plugged in: the BIOS assumes color */
    VGA_SENSE_MONO  = 1,
    VGA_SENSE_COLOR = 2
};

/* CPU registers used by INT 10h. The host copies them in and out. */
typedef struct vga_bios_regs {
    uint16_t ax, bx, cx, dx;
    uint16_t es, bp;
} vga_bios_regs_t;

enum {
    VGA_PORT_NOT_MINE   = 0,      /* not a video port: route it elsewhere */
    VGA_PORT_HANDLED    = 1,
    VGA_PORT_UNSUPPORTED = -1     /* a VGA port this device does not emulate */
};

/* memory: the machine's 1 MB address space (e.g. vm86_memory()). Not copied. */
VGA86_API vga_t *vga_create(uint8_t *memory);
VGA86_API void   vga_destroy(vga_t *v);

VGA86_API void   vga_set_memory(vga_t *v, uint8_t *memory);   /* moved to another machine */

/* Power-on state: mode 03h, cleared screen, BIOS data area initialized. */
VGA86_API void   vga_reset(vga_t *v);

/* I/O ports. Return one of the VGA_PORT_* values. */
VGA86_API int    vga_port_in(vga_t *v, uint16_t port, uint8_t *value);
VGA86_API int    vga_port_out(vga_t *v, uint16_t port, uint8_t value);

/* Video BIOS. Returns 1 if the function (AH, and AL where relevant) is
 * supported and was performed, 0 if it is not supported (registers untouched). */
VGA86_API int    vga_int10(vga_t *v, vga_bios_regs_t *regs);

/* Advances the device clock (retrace, blinking). Programs that poll the
 * retrace bit of port 3DAh only see the 63 us pulse if the CPU runs in slices
 * of at most ~60 us of emulated time between calls; 40 us is recommended
 * (e.g. 16 steps at 400,000 instructions/s). See examples/vm86_glue.c. */
VGA86_API void     vga_advance(vga_t *v, uint32_t microseconds);
VGA86_API uint32_t vga_frame_count(const vga_t *v);   /* 70 Hz frames elapsed */

/* Draws the current screen: vga_render() as luminance (VGA_OUT_WIDTH *
 * VGA_OUT_HEIGHT bytes), vga_render_rgb() as RGB (3 bytes per pixel). */
VGA86_API void     vga_render(vga_t *v, uint8_t *out);
VGA86_API void     vga_render_rgb(vga_t *v, uint8_t *out);
VGA86_API void     vga_signal(const vga_t *v, vga_signal_t *sig);

/* Set by the host when a monitor is plugged in or out (VGA_SENSE_*). */
VGA86_API void     vga_set_monitor_sense(vga_t *v, int sense);

VGA86_API uint8_t  vga_mode(const vga_t *v);          /* 03h or 13h */

/* Device state (registers, palette, clock; not video memory, which lives in
 * the machine's memory). Raw structure: same build, same platform. */
VGA86_API size_t vga_state_size(void);
VGA86_API size_t vga_save_state(const vga_t *v, void *buf, size_t len);
VGA86_API int    vga_load_state(vga_t *v, const void *buf, size_t len);

/*
 * Optional phosphor persistence, independent of the device: each output pixel
 * is max(new, previous * decay), where decay follows the given half-life.
 * A green P1 phosphor looks right at roughly 20-40 ms.
 */
typedef struct vga_phosphor vga_phosphor_t;
VGA86_API vga_phosphor_t *vga_phosphor_create(float half_life_ms);
VGA86_API void vga_phosphor_destroy(vga_phosphor_t *p);
VGA86_API void vga_phosphor_apply(vga_phosphor_t *p, const uint8_t *in, float dt_seconds, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
