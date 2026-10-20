/*
 * pic86 - 8259A programmable interrupt controller (single, master PIC of a PC).
 *
 * Emulated: initialization sequence (ICW1-ICW4), interrupt mask (OCW1),
 * non-specific and specific EOI (OCW2), reading IRR/ISR (OCW3), fixed
 * priority (IRQ 0 highest), edge-triggered requests. After pic_reset() it is
 * programmed like the PC BIOS leaves it: vectors 08h-0Fh.
 *
 * Not emulated (reported as PIC_PORT_UNSUPPORTED): automatic EOI, rotating
 * priority, special mask mode, poll mode, level mode. Cascade mode can be
 * programmed (as on an AT) but there is no slave PIC, so IRQs 8-15 never occur.
 *
 * Delivering interrupts to the CPU: between CPU slices, if the CPU has IF=1,
 * call pic_acknowledge(); if it returns a vector, request that interrupt on
 * the CPU (e.g. vm86_request_interrupt).
 */
#ifndef PIC86_H
#define PIC86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KBD86_API
#  define KBD86_API
#endif

enum {
    PIC_PORT_NOT_MINE    = 0,
    PIC_PORT_HANDLED     = 1,
    PIC_PORT_UNSUPPORTED = -1
};

typedef struct pic {
    uint8_t irr;        /* requests waiting */
    uint8_t isr;        /* requests being serviced */
    uint8_t imr;        /* masked lines */
    uint8_t vector_base;
    uint8_t init_step;  /* 0 = ready, 1 = expecting ICW2, 2 = ICW3, 3 = ICW4 */
    uint8_t icw1;
    uint8_t read_isr;   /* OCW3: port 20h reads ISR instead of IRR */
} pic_t;

KBD86_API void pic_reset(pic_t *p);                   /* as left by the PC BIOS */
KBD86_API void pic_raise(pic_t *p, int irq);          /* edge on an IRQ line, 0-7 */
KBD86_API int  pic_pending(const pic_t *p);           /* 1 if an interrupt would be delivered */
KBD86_API int  pic_acknowledge(pic_t *p);             /* vector of the delivered interrupt, or -1 */
KBD86_API void pic_eoi(pic_t *p);                     /* non-specific EOI, e.g. from a BIOS handler in C */
KBD86_API int  pic_port_in(pic_t *p, uint16_t port, uint8_t *value);
KBD86_API int  pic_port_out(pic_t *p, uint16_t port, uint8_t value);

#ifdef __cplusplus
}
#endif
#endif
