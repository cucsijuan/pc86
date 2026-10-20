/*
 * pic86 - 8259A PIC. See include/pic.h.
 */
#include "pic.h"

#include <string.h>

void pic_reset(pic_t *p)
{
    memset(p, 0, sizeof(*p));
    p->vector_base = 0x08;
    p->imr = 0xFC;          /* the BIOS enables IRQ 0 (timer) and IRQ 1 (keyboard) */
}

void pic_raise(pic_t *p, int irq)
{
    if (irq >= 0 && irq < 8) p->irr |= (uint8_t)(1u << irq);
}

/* Highest-priority request that may be delivered now, or -1. */
static int best_request(const pic_t *p)
{
    uint8_t req = (uint8_t)(p->irr & ~p->imr);
    int i;
    if (p->init_step) return -1;
    for (i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (p->isr & bit) return -1;     /* a higher or equal priority one is in service */
        if (req & bit) return i;
    }
    return -1;
}

int pic_pending(const pic_t *p) { return best_request(p) >= 0; }

int pic_acknowledge(pic_t *p)
{
    int irq = best_request(p);
    if (irq < 0) return -1;
    p->irr &= (uint8_t)~(1u << irq);
    p->isr |= (uint8_t)(1u << irq);
    return p->vector_base + irq;
}

void pic_eoi(pic_t *p)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (p->isr & (1u << i)) { p->isr &= (uint8_t)~(1u << i); return; }
    }
}

int pic_port_in(pic_t *p, uint16_t port, uint8_t *value)
{
    if (port == 0x20) { *value = p->read_isr ? p->isr : p->irr; return PIC_PORT_HANDLED; }
    if (port == 0x21) { *value = p->imr; return PIC_PORT_HANDLED; }
    return PIC_PORT_NOT_MINE;
}

int pic_port_out(pic_t *p, uint16_t port, uint8_t value)
{
    if (port == 0x20) {
        if (value & 0x10) {                         /* ICW1: start initialization */
            if (value & 0x08) return PIC_PORT_UNSUPPORTED;      /* level triggered */
            p->icw1 = value;
            p->init_step = 1;
            p->imr = 0;
            p->isr = 0;
            p->irr = 0;
            p->read_isr = 0;
            return PIC_PORT_HANDLED;
        }
        if ((value & 0x18) == 0x00) {               /* OCW2 */
            uint8_t cmd = (uint8_t)(value & 0xE0);
            if (cmd == 0x20) { pic_eoi(p); return PIC_PORT_HANDLED; }              /* non-specific EOI */
            if (cmd == 0x60) { p->isr &= (uint8_t)~(1u << (value & 7)); return PIC_PORT_HANDLED; } /* specific EOI */
            return PIC_PORT_UNSUPPORTED;            /* rotation commands */
        }
        if ((value & 0x18) == 0x08) {               /* OCW3 */
            if (value & 0x64) return PIC_PORT_UNSUPPORTED;   /* poll or special mask mode */
            if (value & 0x02) p->read_isr = value & 0x01;
            return PIC_PORT_HANDLED;
        }
        return PIC_PORT_UNSUPPORTED;
    }
    if (port == 0x21) {
        switch (p->init_step) {
        case 0: p->imr = value; return PIC_PORT_HANDLED;               /* OCW1 */
        case 1:                                                         /* ICW2 */
            p->vector_base = (uint8_t)(value & 0xF8);
            /* cascade mode (AT, ICW1 = 11h) expects ICW3; no slave is attached */
            p->init_step = !(p->icw1 & 0x02) ? 2 : (p->icw1 & 0x01) ? 3 : 0;
            return PIC_PORT_HANDLED;
        case 2:                                                         /* ICW3 */
            p->init_step = (p->icw1 & 0x01) ? 3 : 0;
            return PIC_PORT_HANDLED;
        case 3:                                                         /* ICW4 */
            if (value & 0x02) return PIC_PORT_UNSUPPORTED;              /* automatic EOI */
            p->init_step = 0;
            return PIC_PORT_HANDLED;
        default:
            p->init_step = 0;
            return PIC_PORT_UNSUPPORTED;
        }
    }
    return PIC_PORT_NOT_MINE;
}
