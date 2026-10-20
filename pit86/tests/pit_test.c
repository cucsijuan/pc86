/* Unit tests for pit86. They drive the device directly (no CPU). */
#include "pit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static void out(pit_t *p, uint16_t port, uint8_t v) { pit_port_out(p, port, v); }
static uint8_t in(pit_t *p, uint16_t port) { uint8_t v = 0; pit_port_in(p, port, &v); return v; }

/* Programs a channel with LSB+MSB access. */
static void program(pit_t *p, int ch, int mode, uint16_t count)
{
    out(p, 0x43, (uint8_t)((ch << 6) | 0x30 | (mode << 1)));
    out(p, (uint16_t)(0x40 + ch), (uint8_t)count);
    out(p, (uint16_t)(0x40 + ch), (uint8_t)(count >> 8));
}

static uint16_t read_latched(pit_t *p, int ch)
{
    uint8_t lo, hi;
    out(p, 0x43, (uint8_t)(ch << 6));                   /* counter latch */
    lo = in(p, (uint16_t)(0x40 + ch));
    hi = in(p, (uint16_t)(0x40 + ch));
    return (uint16_t)(lo | hi << 8);
}

/* Runs ms milliseconds in 40 us slices and counts IRQ requests. */
static int run_irqs(pit_t *p, int ms)
{
    int i, n = 0;
    for (i = 0; i < ms * 25; i++) { pit_advance(p, 40); n += pit_take_irq(p); }
    return n;
}

int main(void)
{
    pit_t *p = pit_create();
    uint8_t st[1024];
    size_t len;
    uint16_t a, b;
    int n, i;

    /* ---------- as left by the BIOS ---------- */
    n = run_irqs(p, 1000);
    CHECK(n == 18 || n == 19, "after reset channel 0 interrupts at 18.2 Hz");
    CHECK(pit_square_wave_hz(p, 2) == 0.0, "the speaker channel is gated off");
    {
        uint32_t e0 = pit_edges(p, 1);
        pit_advance(p, 1000);
        CHECK(pit_edges(p, 1) - e0 >= 65 && pit_edges(p, 1) - e0 <= 67, "channel 1 (refresh) pulses every 15 us");
    }

    /* ---------- mode 2: rate generator ---------- */
    program(p, 0, 2, 1193);                              /* ~1000 Hz */
    pit_take_irq(p);
    {
        uint32_t e0 = pit_edges(p, 0);
        for (i = 0; i < 25000; i++) pit_advance(p, 40);
        CHECK(pit_edges(p, 0) - e0 == 1000, "mode 2 with 1193 gives 1000 edges per second");
    }
    a = read_latched(p, 0);
    pit_advance(p, 100);
    b = read_latched(p, 0);
    CHECK(a >= 1 && a <= 1193 && (uint16_t)(a - b + 1193) % 1193 == 119, "the latched count goes down 119 in 100 us");

    /* ---------- no drift over a long run with odd slice sizes ---------- */
    pit_reset(p);
    program(p, 0, 2, 0);                                 /* 65536 */
    {
        uint32_t e0 = pit_edges(p, 0);
        uint64_t total = 0;
        for (i = 0; i < 100000; i++) { pit_advance(p, 37 + (i % 7) * 13); total += 37 + (i % 7) * 13; }
        CHECK(pit_edges(p, 0) - e0 == total * PIT_CLOCK_HZ / 1000000u / 65536u,
              "7.6 s in irregular slices: exact number of 65536-count periods");
    }

    /* ---------- mode 3: square wave ---------- */
    program(p, 2, 3, 1193);
    pit_set_gate(p, 2, 1);
    CHECK(pit_square_wave_hz(p, 2) > 1000.0 && pit_square_wave_hz(p, 2) < 1000.2, "mode 3 on channel 2 reports ~1000 Hz");
    {
        int high = 0;
        for (i = 0; i < 1000; i++) { pit_advance(p, 1); high += pit_output(p, 2); }
        CHECK(high >= 495 && high <= 505, "the square wave is high half of the time");
    }
    pit_set_gate(p, 2, 0);
    CHECK(pit_output(p, 2) == 1 && pit_square_wave_hz(p, 2) == 0.0, "gate low stops it with the output high");
    {
        uint32_t e0 = pit_edges(p, 2);
        pit_advance(p, 10000);
        CHECK(pit_edges(p, 2) == e0, "no edges while the gate is low");
    }

    /* ---------- mode 0: interrupt on terminal count ---------- */
    pit_take_irq(p);
    out(p, 0x43, 0x30);                                  /* channel 0, LSB+MSB, mode 0 */
    CHECK(pit_output(p, 0) == 0, "mode 0: the control word sets the output low");
    out(p, 0x40, 0xA9); out(p, 0x40, 0x04);              /* 1193 = 1 ms */
    pit_advance(p, 900);
    CHECK(pit_output(p, 0) == 0 && !pit_take_irq(p), "still low before the count ends");
    pit_advance(p, 200);
    CHECK(pit_output(p, 0) == 1 && pit_take_irq(p), "output rises at terminal count: one IRQ");
    pit_advance(p, 200000);
    CHECK(pit_output(p, 0) == 1 && !pit_take_irq(p), "mode 0 fires only once");

    /* ---------- mode 4: software strobe ---------- */
    out(p, 0x43, 0x38);
    CHECK(pit_output(p, 0) == 1, "mode 4: output starts high");
    out(p, 0x40, 0x10); out(p, 0x40, 0x00);
    pit_advance(p, 20);
    CHECK(pit_take_irq(p) && pit_output(p, 0) == 1, "mode 4 strobes once at terminal count");

    /* ---------- access modes and read-back ---------- */
    out(p, 0x43, 0x94);                                  /* channel 2, LSB only, mode 2 */
    out(p, 0x42, 100);
    pit_set_gate(p, 2, 1);
    CHECK(in(p, 0x42) <= 100, "LSB-only reads return the low byte");
    out(p, 0x43, 0xA4);                                  /* channel 2, MSB only, mode 2 */
    out(p, 0x42, 0x02);                                  /* 0200h */
    CHECK(in(p, 0x42) <= 0x02, "MSB-only reads return the high byte");
    program(p, 0, 3, 0x1234);
    out(p, 0x43, 0xE2);                                  /* read-back: count and status, channel 0 */
    {
        uint8_t s = in(p, 0x40);
        uint8_t lo = in(p, 0x40), hi = in(p, 0x40);
        CHECK((s & 0x3F) == 0x36, "read-back status: LSB+MSB access, mode 3");
        CHECK((uint16_t)(lo | hi << 8) <= 0x1234 && !(lo & 1), "then the latched count (even in mode 3)");
    }
    len = pit_save_state(p, st, sizeof(st));
    a = read_latched(p, 0);                              /* value at this moment */
    pit_load_state(p, st, len);
    out(p, 0x43, 0x00);
    pit_advance(p, 500);
    b = in(p, 0x40);
    b = (uint16_t)(b | in(p, 0x40) << 8);
    CHECK(a == b && read_latched(p, 0) != a, "the latch holds the value until both bytes are read");

    /* ---------- unsupported ---------- */
    CHECK(pit_port_out(p, 0x43, 0x32) == PIT_PORT_UNSUPPORTED, "mode 1 is reported as unsupported");
    CHECK(pit_port_out(p, 0x43, 0x3B) == PIT_PORT_UNSUPPORTED, "mode 5 is reported as unsupported");
    CHECK(pit_port_out(p, 0x43, 0x35) == PIT_PORT_UNSUPPORTED, "BCD counting is reported as unsupported");
    CHECK(pit_port_out(p, 0x61, 0) == PIT_PORT_NOT_MINE && pit_port_in(p, 0x20, st) == PIT_PORT_NOT_MINE,
          "other ports are left alone");

    /* ---------- state ---------- */
    pit_reset(p);
    program(p, 0, 2, 1000);
    pit_advance(p, 300);
    len = pit_save_state(p, st, sizeof(st));
    a = read_latched(p, 0);
    pit_advance(p, 5000);
    CHECK(len == pit_state_size() && pit_load_state(p, st, len) == 0 && read_latched(p, 0) == a, "state saved and loaded");

    pit_destroy(p);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
