/*
 * pc86 - diskette adapter for the expansion slots. See include/pc_fdc.h.
 */
#include "pc_fdc.h"

#include <stdlib.h>

struct pc_fdc_card {
    pc_card_t card;
    fdc_t *fdc;
    int on;
};

/* The controller asserts DREQ 2: the motherboard's DMA controller answers. */
static int c_dma(void *user, uint8_t *buf, size_t len, size_t *moved)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    *moved = 0;
    if (!c->card.dma) return FDC_DMA_NO_ACK;
    switch (c->card.dma(c->card.bus, PC_FDC_DMA, buf, len, moved)) {
    case PC_DMA_DONE: return FDC_DMA_DONE;
    case PC_DMA_TC: return FDC_DMA_TC;
    }
    return FDC_DMA_NO_ACK;
}

static void c_attach(void *user, uint8_t *memory)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    fdc_set_memory(c->fdc, memory);
}

static void c_power(void *user, int on)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    c->on = on;
    fdc_reset(c->fdc);                                          /* off: the motors stop */
    if (on) fdc_bios_init(c->fdc);                              /* the option ROM's part of the POST */
}

static int c_port_in(void *user, uint16_t port, uint8_t *value)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    return c->on ? fdc_port_in(c->fdc, port, value) : PC_PORT_NOT_MINE;
}

static int c_port_out(void *user, uint16_t port, uint8_t value)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    return c->on ? fdc_port_out(c->fdc, port, value) : PC_PORT_NOT_MINE;
}

static void c_advance(void *user, uint32_t microseconds)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    if (c->on) fdc_advance(c->fdc, microseconds);
}

static int c_take_irq(void *user)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    return c->on && fdc_take_irq(c->fdc) ? PC_FDC_IRQ : -1;
}

static int c_bios(void *user, uint8_t number, vm86_regs_t *r)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)user;
    fdc_bios_regs_t b;
    if (!c->on || number != 0x13) return PC_BIOS_UNSUPPORTED;
    b.ax = r->ax; b.bx = r->bx; b.cx = r->cx; b.dx = r->dx; b.es = r->es; b.di = r->di; b.flags = r->flags;
    if (fdc_int13(c->fdc, &b) != FDC_BIOS_DONE) return PC_BIOS_UNSUPPORTED;
    r->ax = b.ax; r->bx = b.bx; r->cx = b.cx; r->dx = b.dx; r->es = b.es; r->di = b.di;
    r->flags = (uint16_t)((r->flags & ~1u) | (b.flags & 1u));
    return PC_BIOS_DONE;
}

static size_t c_state_size(void *user) { (void)user; return fdc_state_size(); }

static size_t c_save_state(void *user, void *buf, size_t len)
{
    return fdc_save_state(((pc_fdc_card_t *)user)->fdc, buf, len);
}

static int c_load_state(void *user, const void *buf, size_t len)
{
    return fdc_load_state(((pc_fdc_card_t *)user)->fdc, buf, len);
}

pc_fdc_card_t *pc_fdc_card_create(void)
{
    pc_fdc_card_t *c = (pc_fdc_card_t *)calloc(1, sizeof(pc_fdc_card_t));
    if (!c) return NULL;
    c->fdc = fdc_create(NULL);
    if (!c->fdc) { free(c); return NULL; }
    fdc_set_dma(c->fdc, c_dma, c);
    c->card.name = "Diskette adapter";
    c->card.user = c;
    c->card.attach = c_attach;
    c->card.power = c_power;
    c->card.port_in = c_port_in;
    c->card.port_out = c_port_out;
    c->card.advance = c_advance;
    c->card.take_irq = c_take_irq;
    c->card.rom_ints[0] = 0x13;
    c->card.rom_int_count = 1;
    c->card.bios = c_bios;
    c->card.state_size = c_state_size;
    c->card.save_state = c_save_state;
    c->card.load_state = c_load_state;
    return c;
}

void pc_fdc_card_destroy(pc_fdc_card_t *c)
{
    if (!c) return;
    fdc_destroy(c->fdc);
    free(c);
}

pc_card_t *pc_fdc_card_slot(pc_fdc_card_t *c) { return &c->card; }
fdc_t *pc_fdc_card_fdc(pc_fdc_card_t *c) { return c->fdc; }
int pc_fdc_card_connect_drive(pc_fdc_card_t *c, int n, fdd_t *drive) { return fdc_connect_drive(c->fdc, n, drive); }
fdd_t *pc_fdc_card_drive(pc_fdc_card_t *c, int n) { return fdc_drive(c->fdc, n); }
