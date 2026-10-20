/*
 * pit86 - 8253/8254 programmable interval timer for an in-game virtual PC.
 *
 * Three 16-bit counters clocked at 1.193182 MHz, as wired on a PC:
 *   - channel 0: system timer; its output is IRQ 0 (18.2 Hz after reset)
 *   - channel 1: DRAM refresh (port 61h bit 4 toggles with it)
 *   - channel 2: PC speaker; its gate is port 61h bit 0, its output is
 *     port 61h bit 5 and, through bit 1, drives the speaker
 *
 * Emulated: ports 40h-43h, modes 0, 2, 3 and 4, LSB/MSB/both access, the
 * counter latch command and the 8254 read-back command (count and status),
 * gate input of each channel. After pit_reset() the counters are programmed
 * the way the PC BIOS leaves them.
 *
 * Not emulated (reported as PIT_PORT_UNSUPPORTED): modes 1 and 5 (hardware
 * triggered by a gate edge) and BCD counting.
 *
 * Like the other devices it does not depend on any CPU core: the host routes
 * ports 40h-43h to it, advances its clock and, when pit_take_irq() returns 1,
 * raises IRQ 0 on the PIC.
 */
#ifndef PIT86_H
#define PIT86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(PIT86_BUILD_DLL)
#  define PIT86_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PIT86_USE_DLL)
#  define PIT86_API __declspec(dllimport)
#else
#  define PIT86_API
#endif

#define PIT_CLOCK_HZ 1193182u

enum {
    PIT_PORT_NOT_MINE    = 0,
    PIT_PORT_HANDLED     = 1,
    PIT_PORT_UNSUPPORTED = -1
};

typedef struct pit pit_t;

PIT86_API pit_t *pit_create(void);
PIT86_API void   pit_destroy(pit_t *p);
PIT86_API void   pit_reset(pit_t *p);            /* power-on, programmed as left by the PC BIOS */

/* Ports 40h-42h (counters) and 43h (control word). */
PIT86_API int    pit_port_in(pit_t *p, uint16_t port, uint8_t *value);
PIT86_API int    pit_port_out(pit_t *p, uint16_t port, uint8_t value);

/* Advances the clock. Exact over time: no drift between 1 us and 1.193182 MHz. */
PIT86_API void   pit_advance(pit_t *p, uint32_t microseconds);

/* Returns 1 once if channel 0's output had a rising edge since the last call
 * (several edges in one slice are one request, as the PIC is edge triggered). */
PIT86_API int    pit_take_irq(pit_t *p);

/* Gate input of a channel (0-2). On a PC only channel 2's is not tied high. */
PIT86_API void     pit_set_gate(pit_t *p, int channel, int level);
PIT86_API int      pit_output(const pit_t *p, int channel);    /* current output level */
PIT86_API uint32_t pit_edges(const pit_t *p, int channel);     /* rising edges since reset */

/* Frequency of the square wave on a channel in mode 3 with its gate high,
 * or 0. Lets the host synthesize the speaker tone. */
PIT86_API double   pit_square_wave_hz(const pit_t *p, int channel);

/* Raw state: same build, same platform. */
PIT86_API size_t pit_state_size(void);
PIT86_API size_t pit_save_state(const pit_t *p, void *buf, size_t len);
PIT86_API int    pit_load_state(pit_t *p, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
