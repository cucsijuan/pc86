/* Unit tests for fdc86. They drive the controller through its ports with a
 * stand-in DMA channel (no CPU, no 8237). */
#include "fat12.h"
#include "fdc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static uint8_t mem[0x100000];

/* ---------- a DMA channel ---------- */

static struct {
    int to_memory;                /* 1: device to memory (floppy read) */
    int masked;
    uint32_t addr;
    long left;                    /* bytes until terminal count */
} ch;

static int fake_dma(void *user, uint8_t *buf, size_t len, size_t *moved)
{
    size_t i;
    (void)user;
    *moved = 0;
    if (ch.masked || ch.left <= 0) return FDC_DMA_NO_ACK;
    for (i = 0; i < len && ch.left > 0; i++, ch.left--) {
        if (ch.to_memory) mem[ch.addr++] = buf[i];
        else buf[i] = mem[ch.addr++];
    }
    *moved = i;
    if (ch.left == 0) { ch.masked = 1; return FDC_DMA_TC; }
    return FDC_DMA_DONE;
}

static void dma_setup(int to_memory, uint32_t addr, long len)
{
    ch.to_memory = to_memory; ch.addr = addr; ch.left = len; ch.masked = 0;
}

/* ---------- controller helpers ---------- */

static fdc_t *c;
static int irqs;

static uint8_t msr(void) { uint8_t v = 0; fdc_port_in(c, 0x3F4, &v); return v; }

static void run_us(long us)
{
    for (; us > 0; us -= 40) { fdc_advance(c, 40); irqs += fdc_take_irq(c); }
}

static int send(const uint8_t *bytes, int n)
{
    int i, r = FDC_PORT_HANDLED;
    for (i = 0; i < n; i++) {
        if ((msr() & 0xC0) != 0x80) return -2;                  /* not ready for a command byte */
        r = fdc_port_out(c, 0x3F5, bytes[i]);
    }
    return r;
}

static int results(uint8_t *out)
{
    int n = 0;
    while ((msr() & 0xD0) == 0xD0 && n < 16) fdc_port_in(c, 0x3F5, &out[n++]);
    return n;
}

/* Runs until the command ends (at most ms milliseconds); its results. */
static int wait_results(uint8_t *out, int ms)
{
    int i;
    for (i = 0; i < ms * 25 && fdc_busy(c); i++) { fdc_advance(c, 40); irqs += fdc_take_irq(c); }
    return results(out);
}

static int sense_int(uint8_t *st0, uint8_t *pcn)
{
    uint8_t b = 0x08, r[4];
    int n;
    send(&b, 1);
    n = results(r);
    *st0 = r[0];
    *pcn = n > 1 ? r[1] : 0xFF;
    return n;
}

static void seek(int drive, int cyl)
{
    uint8_t cmd[3], st0, pcn;
    cmd[0] = 0x0F; cmd[1] = (uint8_t)drive; cmd[2] = (uint8_t)cyl;
    send(cmd, 3);
    run_us(400000);
    sense_int(&st0, &pcn);
}

static void rw_cmd(uint8_t op, int drive, int cyl, int head, int sec, int eot)
{
    uint8_t cmd[9];
    cmd[0] = op; cmd[1] = (uint8_t)(head << 2 | drive); cmd[2] = (uint8_t)cyl; cmd[3] = (uint8_t)head;
    cmd[4] = (uint8_t)sec; cmd[5] = 2; cmd[6] = (uint8_t)eot; cmd[7] = 0x1B; cmd[8] = 0xFF;
    send(cmd, 9);
}

/* ---------- BIOS helpers ---------- */

static fdc_bios_regs_t int13(uint16_t ax, uint16_t cx, uint16_t dx, uint16_t es, uint16_t bx)
{
    fdc_bios_regs_t r;
    memset(&r, 0, sizeof(r));
    r.ax = ax; r.bx = bx; r.cx = cx; r.dx = dx; r.es = es;
    fdc_int13(c, &r);
    return r;
}

static void fill_pattern(disk_t *d)
{
    size_t i;
    uint8_t *p = disk_data(d);
    for (i = 0; i < disk_size(d); i++) p[i] = (uint8_t)(i / 512 + i);
}

int main(void)
{
    disk_t *hd = disk_create(DISK_1440K), *dd = disk_create(DISK_720K), *x;
    fdd_t *a = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD)), *b = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD));
    uint8_t res[16], st0, pcn, st[2048], *img;
    int n, ok, i;

    /* ---------- diskettes and drives ---------- */
    CHECK(hd && dd && a && b, "diskettes and drives created");
    CHECK(disk_size(hd) == 1474560 && disk_geometry(hd)->sectors == 18 && disk_size(dd) == 737280, "geometries");
    CHECK(disk_data(hd)[1000] == 0xF6, "a new diskette is formatted with F6h");
    img = (uint8_t *)calloc(1, 737280);
    x = disk_create_image(img, 737280);
    CHECK(x && disk_geometry(x)->sectors == 9, "an image's size says its format");
    CHECK(disk_create_image(img, 1000) == NULL, "an image of unknown size is refused");
    disk_destroy(x);
    CHECK(fdd_disk_changed(a), "a drive powers up with its change line active");
    CHECK(fdd_insert(a, hd) == 0 && fdd_insert(a, dd) == -1, "one diskette per drive");
    CHECK(fdd_eject(a) == hd && fdd_eject(a) == NULL && fdd_disk_changed(a), "eject gives the diskette back");
    fill_pattern(hd);
    fdd_insert(a, hd);
    fdd_insert(b, dd);

    /* ---------- controller reset ---------- */
    c = fdc_create(mem);
    fdc_set_dma(c, fake_dma, NULL);
    fdc_connect_drive(c, 0, a);
    fdc_connect_drive(c, 1, b);
    CHECK(fdc_connect_drive(c, 2, a) == -1, "two drive connectors");
    CHECK(msr() == 0x00, "power on: held in reset");
    {
        uint8_t v;
        CHECK(fdc_port_in(c, 0x3F2, &v) == FDC_PORT_NOT_MINE && fdc_port_in(c, 0x3F6, &v) == FDC_PORT_NOT_MINE &&
              fdc_port_in(c, 0x3F0, &v) == FDC_PORT_NOT_MINE, "DOR is write-only; 3F0h, 3F1h and 3F6h are not decoded");
    }
    fdc_port_out(c, 0x3F2, 0x1C);                               /* motor A, DMA/IRQ, out of reset */
    CHECK(fdc_take_irq(c) == 1 && msr() == 0x80, "releasing reset interrupts; ready for a command");
    ok = 1;
    for (i = 0; i < 4; i++) ok &= sense_int(&st0, &pcn) == 2 && st0 == (0xC0 | i);
    CHECK(ok, "four SENSE INTERRUPT STATUS after a reset (ST0 C0h-C3h)");
    CHECK(sense_int(&st0, &pcn) == 1 && st0 == 0x80, "and then none pending: ST0 80h");
    CHECK(fdd_motor(a) && !fdd_motor(b), "DOR bit 4: motor A");

    /* ---------- invalid and unsupported ---------- */
    res[0] = 0x10;
    CHECK(send(res, 1) == FDC_PORT_HANDLED && results(res) == 1 && res[0] == 0x80, "VERSION is not a 765A command: invalid");
    res[0] = 0x8F;
    CHECK(send(res, 1) == FDC_PORT_HANDLED && results(res) == 1 && res[0] == 0x80, "RELATIVE SEEK neither");
    res[0] = 0x03; res[1] = 0xDF; res[2] = 0x03;
    CHECK(send(res, 3) == FDC_PORT_UNSUPPORTED, "SPECIFY with non-DMA mode is unsupported");
    fdc_port_out(c, 0x3F2, 0x18); fdc_port_out(c, 0x3F2, 0x1C);
    for (i = 0; i < 4; i++) sense_int(&st0, &pcn);
    res[0] = 0x42; for (i = 1; i < 9; i++) res[i] = 0;
    CHECK(send(res, 9) == FDC_PORT_UNSUPPORTED, "READ TRACK is unsupported");
    fdc_port_out(c, 0x3F2, 0x18); fdc_port_out(c, 0x3F2, 0x1C);
    for (i = 0; i < 4; i++) sense_int(&st0, &pcn);
    res[0] = 0x03; res[1] = 0xDF; res[2] = 0x02;                /* 3 ms steps, DMA */
    CHECK(send(res, 3) == FDC_PORT_HANDLED && msr() == 0x80, "SPECIFY: no result phase");

    /* ---------- seeks ---------- */
    fdc_port_out(c, 0x3F7, 0);
    fdc_take_irq(c);
    res[0] = 0x07; res[1] = 0;
    send(res, 2);
    CHECK((msr() & 0x01) && fdc_take_irq(c) == 0, "RECALIBRATE: drive 0 busy seeking");
    run_us(1000);
    CHECK(irqs == 1 && sense_int(&st0, &pcn) == 2 && st0 == 0x20 && pcn == 0, "at track 0 already: seek end, PCN 0");
    {
        uint8_t v;
        fdc_port_in(c, 0x3F7, &v);
        CHECK(v & 0x80, "no step yet: DIR bit 7 shows the diskette change");
    }
    irqs = 0;
    res[0] = 0x0F; res[1] = 0; res[2] = 40;
    send(res, 3);
    run_us(100000);
    CHECK(irqs == 0 && (msr() & 1), "40 steps at 3 ms take more than 100 ms");
    run_us(30000);
    CHECK(irqs == 1 && sense_int(&st0, &pcn) == 2 && st0 == 0x20 && pcn == 40 && fdd_track(a) == 40, "SEEK to 40");
    {
        uint8_t v;
        fdc_port_in(c, 0x3F7, &v);
        CHECK(!(v & 0x80) && !fdd_disk_changed(a), "the step reset the change line");
    }
    res[0] = 0x04; res[1] = 0;
    send(res, 2);
    CHECK(results(res) == 1 && res[0] == 0x28, "SENSE DRIVE STATUS: ready, two-sided, not at track 0");

    /* ---------- read data ---------- */
    seek(0, 10);
    memset(mem, 0, 0x30000);
    dma_setup(1, 0x10000, 1024);
    irqs = 0;
    rw_cmd(0xE6, 0, 10, 0, 1, 18);
    CHECK(msr() == 0x10, "READ DATA executing: busy, no request for master");
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x00 && res[1] == 0 && res[3] == 10 && res[4] == 0 && res[5] == 3 && res[6] == 2,
          "two sectors, terminal count: normal end, next sector 3");
    CHECK(!memcmp(mem + 0x10000, disk_data(hd) + (10 * 2 * 18) * 512, 1024), "the data is in memory");
    CHECK(irqs == 1, "one interrupt at the end");

    dma_setup(1, 0x10000, 2 * 512);
    rw_cmd(0xC6, 0, 10, 0, 18, 18);                             /* multi-track: 18 on side 0, then 1 on side 1 */
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x04 && res[4] == 1 && res[5] == 2 &&
          !memcmp(mem + 0x10200, disk_data(hd) + (10 * 2 * 18 + 18) * 512, 512), "multi-track goes on to side 1");

    dma_setup(1, 0x10000, 4096);
    rw_cmd(0x46, 0, 10, 0, 17, 18);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x40 && res[1] == 0x80 && res[3] == 11 && res[5] == 1, "past EOT without TC: end of cylinder");

    dma_setup(1, 0x10000, 512);
    rw_cmd(0x46, 0, 12, 0, 1, 18);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x40 && res[1] == 0x04 && res[2] == 0x10, "wrong cylinder: no data, ST2 wrong cylinder");

    rw_cmd(0x46, 0, 10, 0, 19, 19);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x40 && res[1] == 0x04, "sector 19 does not exist: no data");

    fdc_port_out(c, 0x3F7, FDC_RATE_250K);
    dma_setup(1, 0x10000, 512);
    rw_cmd(0x46, 0, 10, 0, 1, 18);
    run_us(300000);
    CHECK(fdc_busy(c), "wrong data rate: still searching after one revolution");
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x40 && res[1] == 0x01, "then missing address mark");
    fdc_port_out(c, 0x3F7, FDC_RATE_500K);

    ch.masked = 1;
    rw_cmd(0x46, 0, 10, 0, 1, 18);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x40 && res[1] == 0x10, "DMA channel masked: overrun");

    /* timing */
    {
        int t;
        dma_setup(1, 0x10000, 18 * 512);
        rw_cmd(0x46, 0, 10, 0, 1, 18);
        for (t = 0; t < 1000 * 25 && fdc_busy(c); t++) fdc_advance(c, 40);
        results(res);
        CHECK(t * 40 >= 200000 && t * 40 <= 400000 && res[0] == 0, "a whole track takes between one and two revolutions");
    }

    /* motor and diskette */
    fdc_port_out(c, 0x3F2, 0x0C);                               /* motor off */
    dma_setup(1, 0x10000, 512);
    rw_cmd(0x46, 0, 10, 0, 1, 18);
    run_us(2000000);
    CHECK(fdc_busy(c), "motor off: no index pulses, the command waits");
    fdc_port_out(c, 0x3F2, 0x1C);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0, "motor on: it completes");
    fdd_eject(a);
    rw_cmd(0x46, 0, 10, 0, 1, 18);
    run_us(1000000);
    CHECK(fdc_busy(c), "no diskette: the command waits");
    fdd_insert(a, hd);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[1] == 0x10, "diskette back: it goes on (and overruns: the channel was not set up)");

    /* ---------- write data ---------- */
    for (i = 0; i < 512; i++) mem[0x20000 + i] = (uint8_t)(0xA0 ^ i);
    dma_setup(0, 0x20000, 512);
    disk_clear_modified(hd);
    rw_cmd(0xC5, 0, 10, 1, 5, 18);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x04 && !memcmp(disk_data(hd) + (10 * 2 * 18 + 18 + 4) * 512, mem + 0x20000, 512) &&
          disk_modified(hd), "WRITE DATA on side 1");
    disk_set_write_protect(hd, 1);
    dma_setup(0, 0x20000, 512);
    rw_cmd(0xC5, 0, 10, 0, 1, 18);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x40 && res[1] == 0x02, "write-protected: not writable");
    disk_set_write_protect(hd, 0);

    /* ---------- read ID, format ---------- */
    res[0] = 0x4A; res[1] = 0x04;
    send(res, 2);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x04 && res[3] == 10 && res[4] == 1 && res[5] >= 1 && res[5] <= 18 && res[6] == 2, "READ ID");
    for (i = 0; i < 18; i++) { mem[0x30000 + i * 4] = 10; mem[0x30001 + i * 4] = 0; mem[0x30002 + i * 4] = (uint8_t)(i + 1); mem[0x30003 + i * 4] = 2; }
    dma_setup(0, 0x30000, 18 * 4);
    res[0] = 0x4D; res[1] = 0; res[2] = 2; res[3] = 18; res[4] = 0x54; res[5] = 0xE5;
    CHECK(send(res, 6) == FDC_PORT_HANDLED, "FORMAT TRACK accepted");
    n = wait_results(res, 1000);
    ok = n == 7 && res[0] == 0;
    for (i = 0; i < 18 * 512; i++) ok &= disk_data(hd)[(10 * 2 * 18) * 512 + i] == 0xE5;
    CHECK(ok, "the track is filled with E5h");
    res[0] = 0x4D; res[1] = 0; res[2] = 3; res[3] = 9; res[4] = 0x54; res[5] = 0xE5;
    CHECK(send(res, 6) == FDC_PORT_UNSUPPORTED, "FORMAT with 1 KB sectors is unsupported");
    fdc_port_out(c, 0x3F2, 0x18); fdc_port_out(c, 0x3F2, 0x1C);
    for (i = 0; i < 4; i++) sense_int(&st0, &pcn);

    /* ---------- DD diskette in drive B ---------- */
    fdc_port_out(c, 0x3F2, 0x2D);                               /* motor B, select B */
    fdc_port_out(c, 0x3F7, FDC_RATE_250K);
    fill_pattern(dd);
    seek(1, 3);
    dma_setup(1, 0x10000, 512);
    rw_cmd(0x46, 1, 3, 0, 9, 9);
    n = wait_results(res, 1000);
    CHECK(n == 7 && res[0] == 0x01 && !memcmp(mem + 0x10000, disk_data(dd) + (3 * 2 * 9 + 8) * 512, 512),
          "720 KB diskette at 250 kbps in drive B");

    /* ---------- state ---------- */
    CHECK(fdc_save_state(c, st, sizeof(st)) == fdc_state_size(), "state saved");
    fdc_reset(c);
    CHECK(fdc_load_state(c, st, fdc_state_size()) == 0 && fdc_dor(c) == 0x2D && fdd_motor(b), "state restored");

    /* ---------- BIOS ---------- */
    {
        fdc_bios_regs_t r;
        fdd_t *empty;
        memset(mem, 0, 0x500);
        fdc_reset(c);
        fdc_bios_init(c);
        CHECK((mem[0x410] & 0xC1) == 0x41 && fdc_dor(c) == 0x0C, "POST: two drives in the equipment word, controller ready");
        fill_pattern(hd);
        fdd_eject(a); fdd_insert(a, hd);
        r = int13(0x0201, 0x0001, 0x0000, 0x0000, 0x7C00);
        CHECK((r.flags & 1) && (r.ax >> 8) == 0x06 && mem[0x441] == 0x06, "first read after a diskette change: AH=06h");
        r = int13(0x0201, 0x0001, 0x0000, 0x0000, 0x7C00);
        CHECK(!(r.flags & 1) && r.ax == 0x0001 && !memcmp(mem + 0x7C00, disk_data(hd), 512), "then the boot sector reads");
        CHECK((mem[0x43F] & 1) && mem[0x440] == 0x25 && (fdc_dor(c) & 0x10), "motor on, timeout count set");
        r = int13(0x0203, 0x0A11, 0x0100, 0x2000, 0x0000);      /* cyl 10, sector 17, head 1, 3 sectors */
        CHECK(r.ax == 0x0402 && (r.flags & 1) && !memcmp(mem + 0x20000, disk_data(hd) + (10 * 36 + 18 + 16) * 512, 1024),
              "reading past side 1 of the cylinder: two sectors, then sector not found");
        r = int13(0x0202, 0x0001, 0x0000, 0x1000, 0xFF00);
        CHECK(r.ax == 0x0900 && (r.flags & 1), "a buffer across a 64 KB boundary: DMA boundary error");
        for (i = 0; i < 1024; i++) mem[0x30000 + i] = (uint8_t)i;
        r = int13(0x0302, 0x4F01, 0x0100, 0x3000, 0x0000);
        CHECK(r.ax == 0x0002 && !memcmp(disk_data(hd) + (79 * 36 + 18) * 512, mem + 0x30000, 1024), "write two sectors at cylinder 79");
        disk_set_write_protect(hd, 1);
        r = int13(0x0301, 0x0001, 0x0000, 0x3000, 0x0000);
        CHECK(r.ax >> 8 == 0x03 && (r.flags & 1), "write-protected: AH=03h");
        disk_set_write_protect(hd, 0);
        r = int13(0x0100, 0, 0, 0, 0);
        CHECK(r.ax >> 8 == 0x03 && (r.flags & 1), "AH=01h returns the last status");
        r = int13(0x0800, 0, 0x0000, 0, 0);
        CHECK(r.ax == 0 && (r.bx & 0xFF) == 4 && r.cx == 0x4F12 && r.dx == 0x0102, "AH=08h: 1.44 MB drive, 80x2x18, two drives");
        r = int13(0x1500, 0, 0x0001, 0, 0);
        CHECK(r.ax >> 8 == 0x02, "AH=15h: diskette drive with change line");
        r = int13(0x1500, 0, 0x0002, 0, 0);
        CHECK(r.ax >> 8 == 0x00, "AH=15h: no drive C");
        r = int13(0x1600, 0, 0x0000, 0, 0);
        CHECK(r.ax >> 8 == 0x00 && !(r.flags & 1), "AH=16h: not changed");
        empty = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD));
        fdc_connect_drive(c, 1, empty);
        r = int13(0x0201, 0x0001, 0x0001, 0x0000, 0x7C00);
        CHECK(r.ax >> 8 == 0x80 && (r.flags & 1), "empty drive: time out (80h)");
        r = int13(0x1600, 0, 0x0001, 0, 0);
        CHECK(r.ax >> 8 == 0x06 && (r.flags & 1), "AH=16h on an empty drive: changed");
        fdc_connect_drive(c, 1, b);
        fdd_destroy(empty);
        for (i = 0; i < 18; i++) { mem[0x30000 + i * 4] = 5; mem[0x30001 + i * 4] = 1; mem[0x30002 + i * 4] = (uint8_t)(i + 1); mem[0x30003 + i * 4] = 2; }
        mem[0x522] = 0;
        r = int13(0x0512, 0x0500, 0x0100, 0x3000, 0x0000);
        CHECK(r.ax >> 8 == 0 && disk_data(hd)[(5 * 36 + 18) * 512 + 100] == 0xF6, "AH=05h formats a track");
        r = int13(0x0201, 0x0001, 0x0080, 0x0000, 0x7C00);
        CHECK(r.ax >> 8 == 0x01 && (r.flags & 1), "DL=80h: no hard disk");
        r.ax = 0x4100; r.dx = 0;
        CHECK(fdc_int13(c, &r) == FDC_BIOS_UNSUPPORTED, "other functions are unsupported");
    }

    /* ---------- FAT12 ---------- */
    {
        static uint8_t big[200000], back[200000];
        fat12_entry_t list[8];
        long freeb;
        for (i = 0; i < (int)sizeof(big); i++) big[i] = (uint8_t)(i * 13 + i / 300);
        CHECK(!fat12_valid(hd) || 1, "fat12_valid on random data does not crash");
        CHECK(fat12_format(hd, "work disk") == 0 && fat12_valid(hd), "format 1.44 MB");
        CHECK(disk_data(hd)[510] == 0x55 && disk_data(hd)[511] == 0xAA && disk_data(hd)[0x15] == 0xF0, "boot signature, media F0h");
        freeb = fat12_free_bytes(hd);
        CHECK(freeb == 2847L * 512, "1.44 MB: 2847 free clusters of 512 bytes");
        CHECK(fat12_write_file(hd, "hello.com", (const uint8_t *)"\xB4\x4C\xCD\x21", 4) == 0, "write HELLO.COM");
        CHECK(fat12_write_file(hd, "big.dat", big, sizeof(big)) == 0, "write a 200 KB file");
        CHECK(fat12_file_size(hd, "BIG.DAT") == (long)sizeof(big) &&
              fat12_read_file(hd, "Big.Dat", back, sizeof(back)) == (long)sizeof(big) && !memcmp(big, back, sizeof(big)),
              "read it back, any case");
        n = fat12_list(hd, list, 8);
        CHECK(n == 3 && !strcmp(list[0].name, "WORK DISK") && list[0].attr == 0x08 && !strcmp(list[1].name, "HELLO.COM") &&
              list[1].size == 4 && !strcmp(list[2].name, "BIG.DAT"), "list: label and two files");
        CHECK(fat12_write_file(hd, "big.dat", big, 1000) == 0 && fat12_file_size(hd, "big.dat") == 1000 &&
              fat12_free_bytes(hd) == freeb - 512 - 1024, "replacing a file frees its old clusters");
        CHECK(fat12_delete_file(hd, "hello.com") == 0 && fat12_file_size(hd, "hello.com") == -1 && fat12_free_bytes(hd) == freeb - 1024,
              "delete");
        CHECK(fat12_write_file(hd, "toolongname.com", big, 1) == -1 && fat12_write_file(hd, "a.comx", big, 1) == -1 &&
              fat12_write_file(hd, "a?.com", big, 1) == -1 && fat12_write_file(hd, "my file.com", big, 1) == -1,
              "bad 8.3 names are refused");
        CHECK(fat12_write_file(hd, "huge.dat", big, 0) == 0 && fat12_file_size(hd, "huge.dat") == 0, "an empty file");
        {
            uint8_t boot[64];
            memset(boot, 0x90, sizeof(boot));
            boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
            boot[0x3E] = 0xF4;
            CHECK(fat12_install_boot(hd, boot, sizeof(boot)) == 0 && disk_data(hd)[0x3E] == 0xF4 && disk_data(hd)[0x40] == 0 &&
                  fat12_valid(hd) && fat12_file_size(hd, "big.dat") == 1000, "install a boot sector, keeping the BPB and the files");
            CHECK(fat12_install_boot(hd, big, 511) == -1, "a boot sector has room for 510 bytes");
        }
        CHECK(fat12_format(dd, NULL) == 0 && fat12_free_bytes(dd) == 713L * 1024 && fat12_list(dd, list, 8) == 0,
              "format 720 KB: 713 clusters of 1 KB, no label");
        CHECK(fat12_write_file(dd, "big.dat", big, sizeof(big)) == 0 &&
              fat12_read_file(dd, "big.dat", back, sizeof(back)) == (long)sizeof(big) && !memcmp(big, back, sizeof(big)),
              "files on a 720 KB diskette");
        {
            int k = 0;
            while (fat12_write_file(dd, "big.dat", big, sizeof(big)) == 0 && k < 10) {
                char nm[13];
                sprintf(nm, "F%d.DAT", k++);
                if (fat12_write_file(dd, nm, big, sizeof(big)) != 0) break;
            }
            CHECK(fat12_free_bytes(dd) < (long)sizeof(big), "the diskette fills up and further writes fail");
        }
    }

    fdc_destroy(c);
    fdd_destroy(a); fdd_destroy(b);
    disk_destroy(hd); disk_destroy(dd);
    free(img);
    if (fails) printf("%d FAILED\n", fails); else printf("All passed (0 failures)\n");
    return fails ? 1 : 0;
}
