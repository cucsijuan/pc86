/*
 * dma86 - 8237A DMA controller. See include/dma.h.
 */
#include "dma.h"

#include <stdlib.h>
#include <string.h>

#define MEM_MASK 0xFFFFFu

typedef struct dma_channel {
    uint16_t base_addr, base_count;
    uint16_t addr, count;
    uint8_t mode;
    uint8_t page;
} dma_channel_t;

typedef struct dma_state {
    dma_channel_t ch[DMA_CHANNELS];
    uint8_t command;
    uint8_t status;               /* bits 0-3: terminal count reached */
    uint8_t mask;                 /* bits 0-3 */
    uint8_t flipflop;             /* 0 = low byte next */
    uint8_t temp;
    uint8_t pages[16];            /* ports 80h-8Fh */
} dma_state_t;

struct dma {
    uint8_t *mem;
    dma_state_t s;
};

/* page register port of each channel */
static const uint8_t PAGE_PORT[DMA_CHANNELS] = {0x87, 0x83, 0x81, 0x82};

dma_t *dma_create(uint8_t *memory)
{
    dma_t *d = (dma_t *)calloc(1, sizeof(dma_t));
    if (!d) return NULL;
    d->mem = memory;
    dma_reset(d);
    return d;
}

void dma_destroy(dma_t *d) { free(d); }
void dma_set_memory(dma_t *d, uint8_t *memory) { d->mem = memory; }

static void master_clear(dma_t *d)
{
    d->s.command = 0;
    d->s.status = 0;
    d->s.mask = 0x0F;
    d->s.flipflop = 0;
    d->s.temp = 0;
}

void dma_reset(dma_t *d)
{
    memset(&d->s, 0, sizeof(d->s));
    master_clear(d);
}

static uint8_t byte_of(dma_t *d, uint16_t v)
{
    uint8_t b = d->s.flipflop ? (uint8_t)(v >> 8) : (uint8_t)v;
    d->s.flipflop ^= 1;
    return b;
}

static void set_byte(dma_t *d, uint16_t *v, uint8_t b)
{
    if (d->s.flipflop) *v = (uint16_t)((*v & 0x00FF) | b << 8);
    else *v = (uint16_t)((*v & 0xFF00) | b);
    d->s.flipflop ^= 1;
}

int dma_port_in(dma_t *d, uint16_t port, uint8_t *value)
{
    if (port >= 0x80 && port <= 0x8F) { *value = d->s.pages[port - 0x80]; return DMA_PORT_HANDLED; }
    if (port > 0x0F) return DMA_PORT_NOT_MINE;
    if (port < 8) {
        dma_channel_t *c = &d->s.ch[port >> 1];
        *value = byte_of(d, (port & 1) ? c->count : c->addr);
        return DMA_PORT_HANDLED;
    }
    switch (port) {
    case 0x08: *value = d->s.status; d->s.status &= 0xF0; return DMA_PORT_HANDLED;   /* reading clears TC */
    case 0x0D: *value = d->s.temp; return DMA_PORT_HANDLED;
    default: *value = 0xFF; return DMA_PORT_HANDLED;                                   /* write-only registers */
    }
}

int dma_port_out(dma_t *d, uint16_t port, uint8_t value)
{
    int i;
    if (port >= 0x80 && port <= 0x8F) {
        d->s.pages[port - 0x80] = value;
        for (i = 0; i < DMA_CHANNELS; i++)
            if (PAGE_PORT[i] == port) d->s.ch[i].page = (uint8_t)(value & 0x0F);
        return DMA_PORT_HANDLED;
    }
    if (port > 0x0F) return DMA_PORT_NOT_MINE;
    if (port < 8) {
        dma_channel_t *c = &d->s.ch[port >> 1];
        /* the base and current registers are written together */
        if (port & 1) { set_byte(d, &c->base_count, value); c->count = c->base_count; }
        else { set_byte(d, &c->base_addr, value); c->addr = c->base_addr; }
        return DMA_PORT_HANDLED;
    }
    switch (port) {
    case 0x08: d->s.command = value; return DMA_PORT_HANDLED;
    case 0x09:
        if (value & 0x04) return DMA_PORT_UNSUPPORTED;                              /* software DREQ */
        return DMA_PORT_HANDLED;
    case 0x0A:
        if (value & 0x04) d->s.mask |= (uint8_t)(1u << (value & 3));
        else d->s.mask &= (uint8_t)~(1u << (value & 3));
        return DMA_PORT_HANDLED;
    case 0x0B: d->s.ch[value & 3].mode = value; return DMA_PORT_HANDLED;
    case 0x0C: d->s.flipflop = 0; return DMA_PORT_HANDLED;
    case 0x0D: master_clear(d); return DMA_PORT_HANDLED;
    case 0x0E: d->s.mask = 0; return DMA_PORT_HANDLED;
    case 0x0F: d->s.mask = (uint8_t)(value & 0x0F); return DMA_PORT_HANDLED;
    }
    return DMA_PORT_HANDLED;
}

int dma_transfer(dma_t *d, int ch, uint8_t *buf, size_t len, size_t *moved)
{
    dma_channel_t *c;
    int type, down, autoinit;
    size_t n = 0;

    if (moved) *moved = 0;
    if (ch < 0 || ch >= DMA_CHANNELS) return DMA_XFER_MASKED;
    if ((d->s.command & 0x04) || (d->s.mask & (1u << ch))) return DMA_XFER_MASKED;
    c = &d->s.ch[ch];
    if ((d->s.command & 0x01) || (c->mode & 0xC0) == 0xC0) return DMA_XFER_UNSUPPORTED;   /* mem-to-mem, cascade */
    type = (c->mode >> 2) & 3;
    if (type == 3) return DMA_XFER_UNSUPPORTED;                                         /* illegal on the 8237 */
    down = (c->mode & 0x20) != 0;
    autoinit = (c->mode & 0x10) != 0;

    while (n < len) {
        uint32_t a = ((uint32_t)c->page << 16 | c->addr) & MEM_MASK;
        if (type == DMA_WRITE) { if (d->mem) d->mem[a] = buf[n]; }
        else if (type == DMA_READ) buf[n] = d->mem ? d->mem[a] : 0xFF;
        n++;
        c->addr = (uint16_t)(down ? c->addr - 1 : c->addr + 1);
        if (c->count-- == 0) {                                                          /* terminal count */
            d->s.status |= (uint8_t)(1u << ch);
            if (autoinit) { c->addr = c->base_addr; c->count = c->base_count; }
            else d->s.mask |= (uint8_t)(1u << ch);
            if (moved) *moved = n;
            return DMA_XFER_TC;
        }
    }
    if (moved) *moved = n;
    return DMA_XFER_DONE;
}

int dma_mode_type(const dma_t *d, int ch) { return ch >= 0 && ch < DMA_CHANNELS ? (d->s.ch[ch].mode >> 2) & 3 : 0; }
uint32_t dma_address(const dma_t *d, int ch)
{
    return ch >= 0 && ch < DMA_CHANNELS ? (((uint32_t)d->s.ch[ch].page << 16) | d->s.ch[ch].addr) & MEM_MASK : 0;
}
uint16_t dma_count(const dma_t *d, int ch) { return ch >= 0 && ch < DMA_CHANNELS ? d->s.ch[ch].count : 0; }
int dma_masked(const dma_t *d, int ch) { return ch >= 0 && ch < DMA_CHANNELS ? (d->s.mask >> ch) & 1 : 1; }

size_t dma_state_size(void) { return sizeof(dma_state_t); }

size_t dma_save_state(const dma_t *d, void *buf, size_t len)
{
    if (len < sizeof(dma_state_t)) return 0;
    memcpy(buf, &d->s, sizeof(dma_state_t));
    return sizeof(dma_state_t);
}

int dma_load_state(dma_t *d, const void *buf, size_t len)
{
    if (len != sizeof(dma_state_t)) return -1;
    memcpy(&d->s, buf, sizeof(dma_state_t));
    return 0;
}
