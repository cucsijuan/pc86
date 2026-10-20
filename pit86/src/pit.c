/*
 * pit86 - 8253/8254 PIT. See include/pit.h.
 *
 * Counters are advanced arithmetically (not tick by tick), so a long slice
 * costs the same as a short one.
 */
#include "pit.h"

#include <stdlib.h>
#include <string.h>

typedef struct pit_channel {
    uint8_t mode;          /* 0, 2, 3 or 4 */
    uint8_t rw;            /* 1 LSB only, 2 MSB only, 3 LSB then MSB */
    uint8_t write_msb;     /* rw 3: the next write is the MSB */
    uint8_t read_msb;      /* rw 3: the next read is the MSB */
    uint8_t low_byte;      /* rw 3: LSB written, waiting for the MSB */
    uint8_t latch_bytes;   /* bytes left to read from the count latch */
    uint16_t latch;
    uint8_t status_latched;
    uint8_t status;
    uint8_t gate, out;
    uint8_t armed;         /* a count was loaded and the counter runs */
    uint8_t null_count;    /* a count was written but not loaded yet */
    uint8_t fired;         /* modes 0 and 4: terminal count already reached */
    uint8_t pending;       /* modes 2 and 3: new count waiting for the period end */
    uint32_t reload;       /* 1..65536 */
    uint32_t next_reload;
    uint32_t ce;           /* modes 0, 4: counting element; mode 2: 1..reload */
    uint32_t pos;          /* mode 3: position inside the period */
    uint32_t edges;        /* rising edges of the output since reset */
} pit_channel_t;

typedef struct pit_state {
    pit_channel_t ch[3];
    uint64_t time_us;
    uint64_t ticks;        /* clock pulses already applied */
    uint32_t irq_seen;     /* channel 0 edges already reported */
} pit_state_t;

struct pit {
    pit_state_t st;
};

/* ---------- counting ---------- */

static void run_channel(pit_channel_t *c, uint32_t d)
{
    if (!c->armed || d == 0) return;
    switch (c->mode) {
    case 0: case 4:
        if (!c->gate) return;                          /* gate low pauses counting */
        if (!c->fired && d >= c->ce) {
            c->fired = 1;
            c->edges++;                                /* mode 0 rises; mode 4 pulses low for one clock */
            c->out = 1;
        }
        c->ce = (c->ce - d) & 0xFFFFu;                 /* keeps wrapping after terminal count */
        return;
    case 2:
        if (!c->gate) return;
        if (d < c->ce) { c->ce -= d; }
        else {
            uint32_t rem = d - c->ce;
            c->edges++;
            if (c->pending) { c->reload = c->next_reload; c->pending = 0; c->null_count = 0; }
            c->edges += rem / c->reload;
            c->ce = c->reload - rem % c->reload;
        }
        c->out = c->ce != 1;                           /* low during the last count */
        return;
    case 3: {
        uint64_t t;
        if (!c->gate) return;
        t = (uint64_t)c->pos + d;
        if (t >= c->reload) {
            t -= c->reload;
            c->edges++;
            if (c->pending) { c->reload = c->next_reload; c->pending = 0; c->null_count = 0; }
            c->edges += (uint32_t)(t / c->reload);
            t %= c->reload;
        }
        c->pos = (uint32_t)t;
        c->out = c->pos < (c->reload + 1) / 2;         /* high for the first (longer) half */
        return;
    }
    default:
        return;
    }
}

static uint16_t current_count(const pit_channel_t *c)
{
    if (c->armed && c->mode == 3) {
        uint32_t half = (c->reload + 1) / 2;
        uint32_t p = c->pos < half ? c->pos : c->pos - half;
        return (uint16_t)(((c->reload & ~1u) - 2 * p) & 0xFFFFu);   /* decrements by 2 */
    }
    return (uint16_t)(c->ce & 0xFFFFu);
}

static void load_count(pit_channel_t *c, uint32_t n)
{
    if (n == 0) n = 65536;
    switch (c->mode) {
    case 0: case 4:
        c->ce = n;
        c->out = c->mode == 4;
        c->fired = 0;
        c->armed = 1;
        c->null_count = 0;
        break;
    default:                                           /* 2, 3 */
        if (!c->armed) {
            c->reload = n;
            c->ce = n;
            c->pos = 0;
            c->out = 1;
            c->armed = 1;
            c->null_count = 0;
        } else {
            c->next_reload = n;
            c->pending = 1;
        }
        break;
    }
}

/* ---------- ports ---------- */

static uint8_t status_byte(const pit_channel_t *c)
{
    return (uint8_t)((c->out ? 0x80 : 0) | (c->null_count ? 0x40 : 0) | (c->rw << 4) | (c->mode << 1));
}

static void latch_count(pit_channel_t *c)
{
    if (c->latch_bytes) return;                        /* a latch not read yet is kept */
    c->latch = current_count(c);
    c->latch_bytes = c->rw == 3 ? 2 : 1;
}

static uint8_t read_part(pit_channel_t *c, uint16_t v)
{
    uint8_t b;
    switch (c->rw) {
    case 1: return (uint8_t)v;
    case 2: return (uint8_t)(v >> 8);
    default:
        b = c->read_msb ? (uint8_t)(v >> 8) : (uint8_t)v;
        c->read_msb ^= 1;
        return b;
    }
}

int pit_port_in(pit_t *p, uint16_t port, uint8_t *value)
{
    pit_channel_t *c;
    if (port < 0x40 || port > 0x43) return PIT_PORT_NOT_MINE;
    if (port == 0x43) { *value = 0xFF; return PIT_PORT_HANDLED; }   /* the control register is write only */
    c = &p->st.ch[port - 0x40];
    if (c->status_latched) {
        *value = c->status;
        c->status_latched = 0;
        return PIT_PORT_HANDLED;
    }
    if (c->latch_bytes) {
        *value = read_part(c, c->latch);
        c->latch_bytes--;
        return PIT_PORT_HANDLED;
    }
    *value = read_part(c, current_count(c));
    return PIT_PORT_HANDLED;
}

static int control_word(pit_t *p, uint8_t v)
{
    int sc = v >> 6, rw = (v >> 4) & 3, mode = (v >> 1) & 7;
    pit_channel_t *c;

    if (sc == 3) {                                     /* 8254 read-back */
        int i;
        for (i = 0; i < 3; i++) {
            if (!(v & (2 << i))) continue;
            c = &p->st.ch[i];
            if (!(v & 0x20)) latch_count(c);
            if (!(v & 0x10) && !c->status_latched) { c->status = status_byte(c); c->status_latched = 1; }
        }
        return PIT_PORT_HANDLED;
    }
    c = &p->st.ch[sc];
    if (rw == 0) { latch_count(c); return PIT_PORT_HANDLED; }
    if (v & 1) return PIT_PORT_UNSUPPORTED;           /* BCD counting */
    if (mode > 5) mode -= 4;                           /* 6 and 7 are aliases of 2 and 3 */
    if (mode == 1 || mode == 5) return PIT_PORT_UNSUPPORTED;
    c->mode = (uint8_t)mode;
    c->rw = (uint8_t)rw;
    c->write_msb = c->read_msb = c->low_byte = 0;
    c->latch_bytes = 0;
    c->status_latched = 0;
    c->armed = 0;
    c->pending = 0;
    c->fired = 0;
    c->null_count = 1;
    c->out = mode != 0;
    return PIT_PORT_HANDLED;
}

int pit_port_out(pit_t *p, uint16_t port, uint8_t value)
{
    pit_channel_t *c;
    if (port < 0x40 || port > 0x43) return PIT_PORT_NOT_MINE;
    if (port == 0x43) return control_word(p, value);
    c = &p->st.ch[port - 0x40];
    switch (c->rw) {
    case 1: load_count(c, value); break;
    case 2: load_count(c, (uint32_t)value << 8); break;
    case 3:
        if (!c->write_msb) {
            c->low_byte = value;
            c->write_msb = 1;
            if (c->mode == 0) c->armed = 0;            /* the first byte stops counting in mode 0 */
        } else {
            c->write_msb = 0;
            load_count(c, (uint32_t)c->low_byte | ((uint32_t)value << 8));
        }
        break;
    default:
        return PIT_PORT_UNSUPPORTED;                   /* channel never programmed */
    }
    return PIT_PORT_HANDLED;
}

/* ---------- clock and outputs ---------- */

void pit_advance(pit_t *p, uint32_t microseconds)
{
    pit_state_t *s = &p->st;
    uint64_t target;
    uint32_t d;
    int i;
    s->time_us += microseconds;
    target = s->time_us * PIT_CLOCK_HZ / 1000000u;
    d = (uint32_t)(target - s->ticks);
    s->ticks = target;
    for (i = 0; i < 3; i++) run_channel(&s->ch[i], d);
}

int pit_take_irq(pit_t *p)
{
    if (p->st.ch[0].edges == p->st.irq_seen) return 0;
    p->st.irq_seen = p->st.ch[0].edges;
    return 1;
}

void pit_set_gate(pit_t *p, int channel, int level)
{
    pit_channel_t *c;
    if (channel < 0 || channel > 2) return;
    c = &p->st.ch[channel];
    level = level != 0;
    if (level == c->gate) return;
    c->gate = (uint8_t)level;
    if (c->mode == 2 || c->mode == 3) {
        if (!level) c->out = 1;                        /* gate low forces the output high */
        else if (c->armed) { c->ce = c->reload; c->pos = 0; }   /* rising gate restarts the period */
    }
}

int pit_output(const pit_t *p, int channel)
{
    return channel >= 0 && channel <= 2 ? p->st.ch[channel].out : 0;
}

uint32_t pit_edges(const pit_t *p, int channel)
{
    return channel >= 0 && channel <= 2 ? p->st.ch[channel].edges : 0;
}

double pit_square_wave_hz(const pit_t *p, int channel)
{
    const pit_channel_t *c;
    if (channel < 0 || channel > 2) return 0.0;
    c = &p->st.ch[channel];
    if (!c->armed || c->mode != 3 || !c->gate) return 0.0;
    return (double)PIT_CLOCK_HZ / (double)c->reload;
}

/* ---------- lifecycle ---------- */

static void bios_program(pit_t *p, int ch, uint8_t mode, uint32_t count)
{
    uint8_t cw = (uint8_t)((ch << 6) | 0x30 | (mode << 1));
    control_word(p, cw);
    pit_port_out(p, (uint16_t)(0x40 + ch), (uint8_t)count);
    pit_port_out(p, (uint16_t)(0x40 + ch), (uint8_t)(count >> 8));
}

pit_t *pit_create(void)
{
    pit_t *p = (pit_t *)calloc(1, sizeof(pit_t));
    if (p) pit_reset(p);
    return p;
}

void pit_destroy(pit_t *p) { free(p); }

void pit_reset(pit_t *p)
{
    memset(&p->st, 0, sizeof(p->st));
    p->st.ch[0].gate = 1;
    p->st.ch[1].gate = 1;
    p->st.ch[2].gate = 0;                              /* port 61h bit 0 is clear at boot */
    bios_program(p, 0, 3, 0);                          /* 65536: 18.2 Hz system timer */
    bios_program(p, 1, 2, 18);                         /* DRAM refresh every 15 us */
    bios_program(p, 2, 3, 0x0533);                     /* 896 Hz beep tone, speaker off */
}

size_t pit_state_size(void) { return sizeof(pit_state_t); }

size_t pit_save_state(const pit_t *p, void *buf, size_t len)
{
    if (len < sizeof(pit_state_t)) return 0;
    memcpy(buf, &p->st, sizeof(pit_state_t));
    return sizeof(pit_state_t);
}

int pit_load_state(pit_t *p, const void *buf, size_t len)
{
    if (len != sizeof(pit_state_t)) return -1;
    memcpy(&p->st, buf, sizeof(pit_state_t));
    return 0;
}
