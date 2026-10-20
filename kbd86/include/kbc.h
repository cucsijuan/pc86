/*
 * kbd86 - keyboard controller (8042) and keyboard BIOS for an in-game virtual PC.
 *
 * This is the motherboard side of the keyboard:
 *   - port 60h: data (bytes from the keyboard; bytes written go to the keyboard)
 *   - port 64h: controller status and a few controller commands
 *   - the keyboard BIOS: the default INT 09h handler (scan code -> ASCII with
 *     Shift, Ctrl, Alt, Caps/Num/Scroll Lock, 16-key buffer in the BIOS data
 *     area) and the INT 16h services. Like a real BIOS, it sends EDh to the
 *     keyboard to update its LEDs and F3h to change the typematic rate.
 *
 * The keyboard (kbd.h, or anything else that speaks bytes) is plugged in with
 * kbc_connect(). With nothing plugged in, bytes written to port 60h time out:
 * the controller answers FEh and sets the time-out status bit.
 *
 * Interrupts go through the 8259 PIC (pic.h): kbc_take_irq() moves the next
 * byte from the keyboard into the output buffer when it is free and returns 1
 * when the host must raise IRQ 1. Call it every CPU slice.
 *
 * It uses the machine's 1 MB memory only for the BIOS data area
 * (0040:0017-003D, 0096-0097). Anything a program can reach that is not
 * supported is reported (KBC_PORT_UNSUPPORTED, KBC_BIOS_UNSUPPORTED), never
 * emulated differently.
 */
#ifndef KBC86_H
#define KBC86_H

#include <stddef.h>
#include <stdint.h>

#include "kbd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kbc kbc_t;

enum {
    KBC_PORT_NOT_MINE    = 0,     /* not a keyboard controller port */
    KBC_PORT_HANDLED     = 1,
    KBC_PORT_UNSUPPORTED = -1     /* port/command not emulated */
};

enum {
    KBC_BIOS_UNSUPPORTED = 0,     /* function not supported, registers untouched */
    KBC_BIOS_DONE        = 1,
    KBC_BIOS_WAIT        = 2      /* no key yet: re-execute the INT 16h instruction
                                     (rewind IP by 2) and let time pass */
};

/* CPU registers used by INT 16h. flags: only ZF (bit 6) is read/written. */
typedef struct kbc_bios_regs {
    uint16_t ax, bx, cx, dx;
    uint16_t flags;
} kbc_bios_regs_t;

/* What is plugged into the keyboard connector. */
typedef struct kbc_device {
    void *user;
    int (*read)(void *user, uint8_t *byte);    /* next byte for the controller: 1, or 0 if none */
    int (*write)(void *user, uint8_t byte);    /* byte from the controller: KBD_WIRE_* */
} kbc_device_t;

/* Wires a kbd_t keyboard as a kbc_device_t. */
KBD86_API kbc_device_t kbc_keyboard_device(kbd_t *keyboard);

KBD86_API kbc_t *kbc_create(uint8_t *memory);   /* the machine's 1 MB */
KBD86_API void   kbc_destroy(kbc_t *c);
KBD86_API void   kbc_set_memory(kbc_t *c, uint8_t *memory);

/* Power-on / POST: resets the controller, initializes the BIOS data area,
 * sets the keyboard LEDs from it and discards what the keyboard had sent. */
KBD86_API void   kbc_reset(kbc_t *c);

/* Plugs a device in (copied), or unplugs with NULL. Works while running. */
KBD86_API void   kbc_connect(kbc_t *c, const kbc_device_t *device);
KBD86_API int    kbc_connected(const kbc_t *c);

KBD86_API int    kbc_take_irq(kbc_t *c);

/* Ports 60h and 64h. */
KBD86_API int    kbc_port_in(kbc_t *c, uint16_t port, uint8_t *value);
KBD86_API int    kbc_port_out(kbc_t *c, uint16_t port, uint8_t value);

/*
 * BIOS. kbc_bios_irq() is the default INT 09h handler: it reads port 60h and
 * updates the BIOS data area. The host calls it when IRQ 1 is delivered while
 * the INT 09h vector still points at the BIOS, and then sends EOI to the PIC.
 * kbc_int16() implements INT 16h AH = 00h, 01h, 02h, 03h, 05h, 10h, 11h, 12h.
 */
KBD86_API void   kbc_bios_irq(kbc_t *c);
KBD86_API int    kbc_int16(kbc_t *c, kbc_bios_regs_t *regs);

/* Controller state (not the BIOS data area, which lives in the machine's memory). */
KBD86_API size_t kbc_state_size(void);
KBD86_API size_t kbc_save_state(const kbc_t *c, void *buf, size_t len);
KBD86_API int    kbc_load_state(kbc_t *c, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
