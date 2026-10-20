/* Unit tests for dma86. They drive the device directly (no CPU). */
#include "dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static uint8_t mem[0x100000];

static void out(dma_t *d, uint16_t port, uint8_t v) { dma_port_out(d, port, v); }
static uint8_t in(dma_t *d, uint16_t port) { uint8_t v = 0; dma_port_in(d, port, &v); return v; }

/* Programs channel 2 the way the floppy BIOS does: mask, mode, flip-flop,
 * address, page, count, unmask. */
static void program(dma_t *d, uint8_t mode, uint32_t addr, uint16_t count)
{
    out(d, 0x0A, 0x06);
    out(d, 0x0B, mode);
    out(d, 0x0C, 0);
    out(d, 0x04, (uint8_t)addr);
    out(d, 0x04, (uint8_t)(addr >> 8));
    out(d, 0x81, (uint8_t)(addr >> 16));
    out(d, 0x05, (uint8_t)count);
    out(d, 0x05, (uint8_t)(count >> 8));
    out(d, 0x0A, 0x02);
}

int main(void)
{
    dma_t *d = dma_create(mem);
    uint8_t buf[1024], st[256];
    size_t moved, i;
    int r, ok;

    CHECK(d != NULL, "created");
    for (i = 0; i < DMA_CHANNELS; i++) CHECK(dma_masked(d, (int)i), "power on: channel masked");
    CHECK(dma_transfer(d, 2, buf, 1, &moved) == DMA_XFER_MASKED && moved == 0, "a masked channel moves nothing");

    /* ---------- ports ---------- */
    {
        uint8_t v;
        CHECK(dma_port_in(d, 0x10, &v) == DMA_PORT_NOT_MINE && dma_port_out(d, 0x60, 0) == DMA_PORT_NOT_MINE,
              "ports outside 00h-0Fh and 80h-8Fh are not mine");
    }
    out(d, 0x0C, 0);
    out(d, 0x02, 0x34); out(d, 0x02, 0x12);
    out(d, 0x0C, 0);
    CHECK(in(d, 0x02) == 0x34 && in(d, 0x02) == 0x12, "address register written and read through the flip-flop");
    out(d, 0x83, 0x05);
    CHECK(in(d, 0x83) == 0x05 && dma_address(d, 1) == 0x51234, "page register 83h is channel 1's");

    /* ---------- device to memory (floppy read) ---------- */
    program(d, 0x46, 0x12345, 511);                     /* single, write, channel 2 */
    CHECK(!dma_masked(d, 2) && dma_mode_type(d, 2) == DMA_WRITE, "channel 2 programmed for a write transfer");
    for (i = 0; i < 512; i++) buf[i] = (uint8_t)(i * 7);
    r = dma_transfer(d, 2, buf, 256, &moved);
    CHECK(r == DMA_XFER_DONE && moved == 256 && dma_address(d, 2) == 0x12445 && dma_count(d, 2) == 255,
          "half a sector: address and count advance");
    r = dma_transfer(d, 2, buf + 256, 300, &moved);
    CHECK(r == DMA_XFER_TC && moved == 256, "terminal count stops at the programmed length");
    ok = 1;
    for (i = 0; i < 512; i++) ok &= mem[0x12345 + i] == (uint8_t)(i * 7);
    CHECK(ok, "the bytes are in memory");
    CHECK(dma_masked(d, 2), "terminal count masks the channel");
    CHECK((in(d, 0x08) & 0x04) && !(in(d, 0x08) & 0x04), "status shows TC once; reading it clears it");

    /* ---------- memory to device (floppy write) ---------- */
    program(d, 0x4A, 0x12345, 9);
    memset(buf, 0, sizeof(buf));
    r = dma_transfer(d, 2, buf, 10, &moved);
    CHECK(r == DMA_XFER_TC && moved == 10 && buf[3] == 21, "a read transfer fills the device's buffer");

    /* ---------- verify, decrement, auto-init, page wrap ---------- */
    memset(mem + 0x20000, 0xAA, 16);
    program(d, 0x42, 0x20000, 15);
    r = dma_transfer(d, 2, buf, 16, &moved);
    CHECK(r == DMA_XFER_TC && mem[0x20000] == 0xAA, "verify only counts");
    program(d, 0x66, 0x30010, 3);                       /* decrement */
    buf[0] = 1; buf[1] = 2; buf[2] = 3; buf[3] = 4;
    dma_transfer(d, 2, buf, 4, &moved);
    CHECK(mem[0x30010] == 1 && mem[0x3000D] == 4, "address decrement");
    program(d, 0x56, 0x40000, 1);                       /* auto-init */
    r = dma_transfer(d, 2, buf, 2, &moved);
    CHECK(r == DMA_XFER_TC && !dma_masked(d, 2) && dma_address(d, 2) == 0x40000 && dma_count(d, 2) == 1,
          "auto-initialization reloads and stays unmasked");
    program(d, 0x46, 0x5FFFF, 1);
    buf[0] = 0x11; buf[1] = 0x22;
    dma_transfer(d, 2, buf, 2, &moved);
    CHECK(mem[0x5FFFF] == 0x11 && mem[0x50000] == 0x22 && mem[0x60000] != 0x22, "the address wraps inside its 64 KB page");

    /* ---------- masks, disable, master clear ---------- */
    program(d, 0x46, 0x1000, 10);
    out(d, 0x0F, 0x0F);
    CHECK(dma_masked(d, 2), "all-channel mask register");
    out(d, 0x0E, 0);
    CHECK(!dma_masked(d, 0) && !dma_masked(d, 2), "clear mask register");
    out(d, 0x08, 0x04);
    CHECK(dma_transfer(d, 2, buf, 1, &moved) == DMA_XFER_MASKED, "controller disabled by the command register");
    out(d, 0x0D, 0);
    CHECK(dma_masked(d, 2) && dma_transfer(d, 2, buf, 1, &moved) == DMA_XFER_MASKED, "master clear masks every channel");

    /* ---------- not emulated ---------- */
    CHECK(dma_port_out(d, 0x09, 0x06) == DMA_PORT_UNSUPPORTED, "software DMA request is unsupported");
    program(d, 0xC6, 0x1000, 10);
    CHECK(dma_transfer(d, 2, buf, 1, &moved) == DMA_XFER_UNSUPPORTED, "cascade mode is unsupported");
    program(d, 0x4E, 0x1000, 10);
    CHECK(dma_transfer(d, 2, buf, 1, &moved) == DMA_XFER_UNSUPPORTED, "transfer type 3 is unsupported");

    /* ---------- state ---------- */
    program(d, 0x46, 0x7000, 99);
    CHECK(dma_save_state(d, st, sizeof(st)) == dma_state_size() && dma_state_size() <= sizeof(st), "state saved");
    dma_reset(d);
    CHECK(dma_load_state(d, st, dma_state_size()) == 0 && dma_address(d, 2) == 0x7000 && dma_count(d, 2) == 99 &&
          !dma_masked(d, 2), "state restored");

    dma_destroy(d);
    if (fails) printf("%d FAILED\n", fails); else printf("All passed (0 failures)\n");
    return fails ? 1 : 0;
}
