/*
 * pc86 - the motherboard of an in-game virtual PC.
 *
 * Puts the independent components together into a machine:
 *
 *   on the board (fixed)   8086 CPU (vm86), 640 KB of RAM, 8259 PIC, 8253 PIT,
 *                          8237 DMA controller, 8042 keyboard controller,
 *                          port 61h, system BIOS ROM
 *   expansion slots (ISA)  cards (pc_card_t): video (pc_vga.h), diskette
 *                          adapter (pc_fdc.h), and later sound, serial...
 *                          Cards are inserted and removed with the power off.
 *   connectors             keyboard (kbd86), on the board; the monitor plugs
 *                          into the video card (pc_vga.h). Peripherals can be
 *                          plugged and unplugged while the machine runs.
 *
 * Time: pc_run() advances the machine by an amount of emulated time. The CPU
 * runs in 40 us slices; between slices every device's clock advances and
 * interrupts are delivered, so programs see a consistent machine (timer at
 * 18.2 Hz, VGA retrace at 70 Hz, keyboard typematic...) whatever the frame
 * rate of the engine.
 *
 * BIOS: the ROM at F000:0000 holds real 8086 code where a program can observe
 * it (the timer interrupt, the bootstrap, the diskette interrupt, the reset
 * entry, the unexpected-IRQ handler) and entry points serviced in C for the
 * rest (INT 09h, 11h, 12h, 16h, 1Ah). A program that installs its own handler
 * changes the vector and gets the real behavior. Cards provide their own ROM
 * services (the VGA card: INT 10h; the diskette adapter: INT 13h).
 *
 * Boot: INT 19h reads the boot sector of drive A: to 0000:7C00 through INT
 * 13h and jumps to it with DL = 0 if it ends in 55AAh.
 *
 * Ports nobody decodes read as FFh and ignore writes, like an empty bus.
 * Ports a device decodes but does not emulate stop the machine with an error
 * (pc_error()), never behaving differently from the real hardware.
 *
 * There is no DOS: pc_set_int_hook() lets the host service software
 * interrupts that have no handler in the machine (e.g. INT 20h/21h).
 */
#ifndef PC86_H
#define PC86_H

#include <stddef.h>
#include <stdint.h>

#include "kbd.h"
#include "vm86.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(PC86_BUILD_DLL)
#  define PC86_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PC86_USE_DLL)
#  define PC86_API __declspec(dllimport)
#else
#  define PC86_API
#endif

#define PC_SLOTS        6
#define PC_SLICE_US     40u          /* CPU slice; below the 63 us VGA retrace pulse */
#define PC_DEFAULT_IPS  400000u      /* roughly a 4.77 MHz 8088 */
#define PC_COM_SEGMENT  0x1000u      /* where pc_load_com() puts a program */

typedef struct pc pc_t;

typedef enum pc_status {
    PC_OFF = 0,
    PC_RUNNING,                      /* executing */
    PC_HALTED,                       /* CPU in HLT, waiting for an interrupt */
    PC_STOPPED,                      /* the host stopped it (pc_stop, or an int hook) */
    PC_ERROR                         /* something not emulated was reached; see pc_error() */
} pc_status_t;

/* ---------- expansion cards ---------- */

enum {
    PC_PORT_NOT_MINE    = 0,
    PC_PORT_HANDLED     = 1,
    PC_PORT_UNSUPPORTED = -1
};

enum {
    PC_BIOS_UNSUPPORTED = 0,
    PC_BIOS_DONE        = 1,
    PC_BIOS_WAIT        = 2          /* re-execute the INT instruction and let time pass */
};

/* What the board's DMA controller did with a card's request. */
enum {
    PC_DMA_DONE   = 0,               /* every byte moved */
    PC_DMA_TC     = 1,               /* terminal count reached: the card ends its transfer */
    PC_DMA_NO_ACK = 2                /* channel masked (or not emulated: the machine stops) */
};

/*
 * An ISA card. Every callback may be NULL. The card's option ROM services
 * the interrupts listed in rom_ints: the POST points those vectors at the
 * card's ROM, and bios() is called while they still do.
 *
 * dma and bus are filled in by the machine when the card is inserted: a
 * card asserting DREQ on a channel calls dma(bus, channel, ...) and the
 * board's 8237 moves the bytes, in the direction the program set up.
 */
typedef struct pc_card {
    const char *name;
    void *user;
    void (*attach)(void *user, uint8_t *memory);       /* inserted: the machine's 1 MB bus */
    void (*power)(void *user, int on);                 /* power on (reset) / off */
    int  (*port_in)(void *user, uint16_t port, uint8_t *value);    /* PC_PORT_* */
    int  (*port_out)(void *user, uint16_t port, uint8_t value);
    void (*advance)(void *user, uint32_t microseconds);
    int  (*take_irq)(void *user);                      /* IRQ line 2-7 to raise now, or -1 */
    uint8_t rom_ints[4];
    uint8_t rom_int_count;
    int  (*bios)(void *user, uint8_t number, vm86_regs_t *regs);   /* PC_BIOS_* */
    size_t (*state_size)(void *user);
    size_t (*save_state)(void *user, void *buf, size_t len);
    int    (*load_state)(void *user, const void *buf, size_t len);
    int  (*dma)(void *bus, int channel, uint8_t *buf, size_t len, size_t *moved);   /* PC_DMA_*, set by the machine */
    void *bus;
} pc_card_t;

/* ---------- machine ---------- */

PC86_API pc_t *pc_create(void);
PC86_API void  pc_destroy(pc_t *pc);           /* does not destroy cards or peripherals */

PC86_API vm86_t  *pc_cpu(pc_t *pc);
PC86_API uint8_t *pc_memory(pc_t *pc);          /* the 1 MB address space */

/* Cards: only with the power off. The card is not copied: it must outlive
 * its time in the slot. Return 0, or -1 (powered on, bad slot, slot busy). */
PC86_API int        pc_insert_card(pc_t *pc, int slot, pc_card_t *card);
PC86_API pc_card_t *pc_remove_card(pc_t *pc, int slot);     /* NULL if powered on or empty */
PC86_API pc_card_t *pc_card(pc_t *pc, int slot);

/* Keyboard connector: any time. NULL unplugs. The keyboard gets power from
 * the machine: it runs its self test when the machine is on. */
PC86_API void   pc_connect_keyboard(pc_t *pc, kbd_t *keyboard);
PC86_API kbd_t *pc_keyboard(pc_t *pc);

/* Power switch. Power on clears memory, runs the POST and boots from drive
 * A:; with no bootable diskette the BIOS says so and waits. */
PC86_API void        pc_power_on(pc_t *pc);
PC86_API void        pc_power_off(pc_t *pc);
PC86_API void        pc_reset(pc_t *pc);                     /* reset button: power cycle */
PC86_API pc_status_t pc_status(const pc_t *pc);
PC86_API const char *pc_error(const pc_t *pc);               /* "" when there is none */
PC86_API void        pc_stop(pc_t *pc);                      /* e.g. from an int hook */

/* Emulated instructions per second (default PC_DEFAULT_IPS). */
PC86_API void     pc_set_speed(pc_t *pc, uint32_t ips);
PC86_API uint32_t pc_speed(const pc_t *pc);

/* Runs the machine for dt_seconds of emulated time. Fractions of a slice are
 * carried over to the next call. */
PC86_API void     pc_run(pc_t *pc, double dt_seconds);
PC86_API uint64_t pc_time_us(const pc_t *pc);                /* emulated time since power on */

/* Loads a .COM program at PC_COM_SEGMENT the way DOS does (see
 * vm86_load_com) and jumps to it. The machine must be on. 0 or -1. */
PC86_API int pc_load_com(pc_t *pc, const uint8_t *data, size_t len);

/*
 * Called for a software interrupt whose vector has no handler in the machine
 * (it is 0000:0000 or the BIOS's dummy IRET). Return 1 if the host serviced
 * it, 0 to let the CPU go through the vector.
 */
typedef int (*pc_int_hook_t)(void *user, pc_t *pc, uint8_t number);
PC86_API void pc_set_int_hook(pc_t *pc, pc_int_hook_t hook, void *user);

/* PC speaker: frequency of the tone being played, or 0 when silent. */
PC86_API double pc_speaker_hz(const pc_t *pc);

/* Machine state (CPU and memory, board chips, cards). Peripherals keep their
 * own state (kbd_save_state...). Raw: same build, same platform, same cards. */
PC86_API size_t pc_state_size(const pc_t *pc);
PC86_API size_t pc_save_state(const pc_t *pc, void *buf, size_t len);
PC86_API int    pc_load_state(pc_t *pc, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
