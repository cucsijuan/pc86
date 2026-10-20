/*
 * fdc86 - the diskette BIOS of the adapter's option ROM, serviced in C.
 * See fdc_int13() in include/fdc.h.
 */
#include "fdc_internal.h"

#include <string.h>

/* BIOS data area */
#define BDA_EQUIPMENT  0x410u
#define BDA_RECAL      0x43Eu     /* bits 0-3 recalibrated, bit 7 interrupt flag */
#define BDA_MOTOR      0x43Fu     /* bits 0-3 motor on */
#define BDA_MOTOR_TIME 0x440u     /* ticks until the motors go off (INT 08h) */
#define BDA_STATUS     0x441u     /* last INT 13h status */
#define BDA_NEC        0x442u     /* the controller's last result bytes */
#define BDA_RATE       0x48Bu
#define BDA_DRIVE_INFO 0x48Fu
#define BDA_MEDIA      0x490u     /* media state, one byte per drive */
#define BDA_CYLINDER   0x494u     /* current cylinder, one byte per drive */

/* INT 13h status codes */
#define ST_OK          0x00
#define ST_BAD_COMMAND 0x01
#define ST_WRITE_PROT  0x03
#define ST_NOT_FOUND   0x04
#define ST_CHANGED     0x06
#define ST_BOUNDARY    0x09
#define ST_MEDIA_TYPE  0x0C
#define ST_TIMEOUT     0x80

static uint8_t *bda(fdc_t *c, uint32_t a) { return c->mem + a; }

static int done(fdc_t *c, fdc_bios_regs_t *r, uint8_t status, uint8_t sectors)
{
    r->ax = (uint16_t)(status << 8 | sectors);
    if (status) r->flags |= 1; else r->flags &= (uint16_t)~1;
    *bda(c, BDA_STATUS) = status;
    return FDC_BIOS_DONE;
}

/* The diskette parameter table, wherever INT 1Eh points (DOS replaces it). */
static uint16_t table_seg(fdc_t *c) { return (uint16_t)(c->mem[0x7A] | c->mem[0x7B] << 8); }
static uint16_t table_off(fdc_t *c) { return (uint16_t)(c->mem[0x78] | c->mem[0x79] << 8); }

static uint8_t table_byte(fdc_t *c, int i, uint8_t fallback)
{
    if (!table_seg(c) && !table_off(c)) return fallback;
    return c->mem[(((uint32_t)table_seg(c) << 4) + table_off(c) + (uint32_t)i) & 0xFFFFFu];
}

static int drive_count(const fdc_t *c)
{
    return c->drive[1] ? 2 : c->drive[0] ? 1 : 0;
}

/* Motor on and drive selected, as the BIOS leaves the DOR; INT 08h turns it
 * off again when the count at 0040:0040 runs out. */
static void motor_on(fdc_t *c, int d)
{
    fdc_write_dor(c, (uint8_t)((c->s.dor & 0xF0) | 0x0C | d | (0x10 << d)));
    *bda(c, BDA_MOTOR) = (uint8_t)((*bda(c, BDA_MOTOR) & 0xF0) | (c->s.dor >> 4));
    *bda(c, BDA_MOTOR_TIME) = table_byte(c, 2, 0x25);
}

static void seek(fdc_t *c, int d, fdd_t *f, int cyl)
{
    fdd_step_to(f, cyl);
    c->s.pcn[d] = (uint8_t)cyl;
    *bda(c, BDA_CYLINDER + (uint32_t)d) = (uint8_t)cyl;
}

/* The diskette is there and has not changed; establishes its media type. */
static uint8_t media_check(fdc_t *c, int d, fdd_t *f)
{
    const disk_geometry_t *g;
    if (fdd_disk_changed(f)) {
        int t = fdd_track(f);
        fdd_step_to(f, t ? t - 1 : 1);                          /* a step resets the change line... */
        seek(c, d, f, t);
        *bda(c, BDA_MEDIA + (uint32_t)d) = 0;
        return fdd_disk_changed(f) ? ST_TIMEOUT : ST_CHANGED;   /* ...unless there is no diskette */
    }
    if (!fdd_disk(f)) return ST_TIMEOUT;
    g = disk_geometry(fdd_disk(f));
    c->s.ccr = g->rate;
    *bda(c, BDA_RATE) = (uint8_t)(g->rate << 6);
    *bda(c, BDA_MEDIA + (uint32_t)d) = (uint8_t)(g->rate << 6 | 0x17);    /* established, rate */
    return ST_OK;
}

static void nec_result(fdc_t *c, int d, int cyl, int head, int sec, uint8_t st0, uint8_t st1)
{
    uint8_t *p = bda(c, BDA_NEC);
    p[0] = (uint8_t)(st0 | head << 2 | d); p[1] = st1; p[2] = 0;
    p[3] = (uint8_t)cyl; p[4] = (uint8_t)head; p[5] = (uint8_t)sec; p[6] = 2;
}

void fdc_bios_init(fdc_t *c)
{
    int n = drive_count(c), d;
    uint16_t eq;

    fdc_write_dor(c, 0x00);                                     /* reset the controller... */
    fdc_write_dor(c, 0x0C);
    c->s.reset_sense = 0;                                       /* ...and take its four interrupts */
    c->s.irq_line = 0;
    c->s.irq_edge = 0;
    c->s.irq_out = 0;
    c->s.srt = (uint8_t)(table_byte(c, 0, 0xDF) >> 4);
    for (d = 0; d < FDC_DRIVES; d++) {
        if (c->drive[d]) seek(c, d, c->drive[d], 0);            /* recalibrate */
        *bda(c, BDA_MEDIA + (uint32_t)d) = 0;
    }
    eq = (uint16_t)(c->mem[BDA_EQUIPMENT] | c->mem[BDA_EQUIPMENT + 1] << 8);
    eq &= (uint16_t)~0x00C1;
    if (n) eq |= (uint16_t)(0x01 | (n - 1) << 6);
    c->mem[BDA_EQUIPMENT] = (uint8_t)eq;
    c->mem[BDA_EQUIPMENT + 1] = (uint8_t)(eq >> 8);
    *bda(c, BDA_RECAL) = (uint8_t)(c->drive[0] ? 1 : 0) | (c->drive[1] ? 2 : 0);
    *bda(c, BDA_MOTOR) = 0;
    *bda(c, BDA_MOTOR_TIME) = 0;
    *bda(c, BDA_STATUS) = 0;
    *bda(c, BDA_RATE) = 0;
    *bda(c, BDA_DRIVE_INFO) = (uint8_t)((c->drive[0] ? 0x07 : 0) | (c->drive[1] ? 0x70 : 0));
}

/* AH = 02h read, 03h write, 04h verify */
static int transfer(fdc_t *c, fdc_bios_regs_t *r, int d, fdd_t *f, int op)
{
    int count = r->ax & 0xFF, cyl = (r->cx >> 8) | ((r->cx & 0xC0) << 2);
    int sec = r->cx & 0x3F, head = (r->dx >> 8) & 0xFF, i;
    uint32_t lin = ((uint32_t)r->es << 4) + r->bx;
    const disk_geometry_t *g;
    disk_t *disk;
    uint8_t st;

    if (count == 0) return done(c, r, ST_BAD_COMMAND, 0);
    if (op != 4 && (lin & 0xFFFFu) + (uint32_t)count * SECTOR_SIZE > 0x10000u)
        return done(c, r, ST_BOUNDARY, 0);                      /* the DMA address would wrap */
    motor_on(c, d);
    if ((st = media_check(c, d, f)) != ST_OK) return done(c, r, st, 0);
    disk = fdd_disk(f);
    g = disk_geometry(disk);
    seek(c, d, f, cyl);
    if (op == 3 && disk_write_protected(disk)) {
        nec_result(c, d, cyl, head, sec, 0x40, 0x02);
        return done(c, r, ST_WRITE_PROT, 0);
    }
    for (i = 0; i < count; i++) {
        int s = sec + i, h = head, k;
        uint8_t *p;
        if (s > g->sectors && head == 0) { s -= g->sectors; h = 1; }   /* multi-track */
        p = disk_sector(disk, cyl, h, s);
        if (!p) {
            nec_result(c, d, cyl, h, s, 0x40, 0x04);
            return done(c, r, ST_NOT_FOUND, (uint8_t)i);
        }
        if (op == 2)
            for (k = 0; k < (int)SECTOR_SIZE; k++) c->mem[(lin + (uint32_t)(i * SECTOR_SIZE + k)) & 0xFFFFFu] = p[k];
        else if (op == 3) {
            for (k = 0; k < (int)SECTOR_SIZE; k++) p[k] = c->mem[(lin + (uint32_t)(i * SECTOR_SIZE + k)) & 0xFFFFFu];
            disk->modified = 1;
        }
    }
    nec_result(c, d, cyl, head, sec + count - 1, 0x00, 0x00);
    return done(c, r, ST_OK, (uint8_t)count);
}

/* AH = 05h: ES:BX points at C, H, R, N for each sector. */
static int format(fdc_t *c, fdc_bios_regs_t *r, int d, fdd_t *f)
{
    int count = r->ax & 0xFF, cyl = r->cx >> 8, head = (r->dx >> 8) & 0xFF, i;
    uint32_t lin = ((uint32_t)r->es << 4) + r->bx;
    const disk_geometry_t *g;
    disk_t *disk;
    uint8_t st, fill = table_byte(c, 8, 0xF6);

    motor_on(c, d);
    if ((st = media_check(c, d, f)) != ST_OK) return done(c, r, st, 0);
    disk = fdd_disk(f);
    g = disk_geometry(disk);
    if (count != g->sectors) return FDC_BIOS_UNSUPPORTED;
    for (i = 0; i < count; i++)
        if (c->mem[(lin + (uint32_t)i * 4 + 3) & 0xFFFFFu] != 2) return FDC_BIOS_UNSUPPORTED;   /* N = 512 bytes */
    seek(c, d, f, cyl);
    if (disk_write_protected(disk)) return done(c, r, ST_WRITE_PROT, 0);
    if (cyl >= g->tracks || head >= g->heads) return done(c, r, ST_NOT_FOUND, 0);
    for (i = 1; i <= g->sectors; i++) memset(disk_sector(disk, cyl, head, i), fill, SECTOR_SIZE);
    disk->modified = 1;
    return done(c, r, ST_OK, 0);
}

int fdc_int13(fdc_t *c, fdc_bios_regs_t *r)
{
    int ah = r->ax >> 8, d = r->dx & 0xFF;
    fdd_t *f = d < 0x80 ? fdc_drive_n(c, d) : NULL;

    if (d >= 0x80) {                                            /* no hard disks on this adapter */
        r->ax = (uint16_t)(ST_BAD_COMMAND << 8 | (r->ax & 0xFF));
        r->flags |= 1;
        return FDC_BIOS_DONE;
    }
    switch (ah) {
    case 0x00:
        *bda(c, BDA_RECAL) &= 0xF0;
        return done(c, r, ST_OK, (uint8_t)r->ax);
    case 0x01: {
        uint8_t st = *bda(c, BDA_STATUS);
        r->ax = (uint16_t)(st << 8 | st);
        if (st) r->flags |= 1; else r->flags &= (uint16_t)~1;
        return FDC_BIOS_DONE;
    }
    case 0x02: case 0x03: case 0x04:
        if (!f) return done(c, r, ST_TIMEOUT, 0);
        return transfer(c, r, d, f, ah);
    case 0x05:
        if (!f) return done(c, r, ST_TIMEOUT, 0);
        return format(c, r, d, f);
    case 0x08:
        r->ax = 0;
        r->flags &= (uint16_t)~1;
        if (!f) { r->bx = 0; r->cx = 0; r->dx = (uint16_t)drive_count(c); return FDC_BIOS_DONE; }
        r->bx = fdd_model(f)->bios_type;
        r->cx = (uint16_t)((fdd_model(f)->tracks - 1) << 8 | 18);
        r->dx = (uint16_t)(0x0100 | drive_count(c));
        r->es = table_seg(c);
        r->di = table_off(c);
        *bda(c, BDA_STATUS) = 0;
        return FDC_BIOS_DONE;
    case 0x15:
        r->ax = (uint16_t)((f ? 0x02 : 0x00) << 8 | (r->ax & 0xFF));   /* 02h: diskette with change line */
        r->flags &= (uint16_t)~1;
        return FDC_BIOS_DONE;
    case 0x16:
        if (!f) return done(c, r, ST_TIMEOUT, 0);
        motor_on(c, d);
        return done(c, r, fdd_disk_changed(f) ? ST_CHANGED : ST_OK, (uint8_t)r->ax);
    case 0x17:
        if (!f) return done(c, r, ST_TIMEOUT, 0);
        return done(c, r, ST_OK, (uint8_t)r->ax);
    case 0x18: {
        int tracks = (r->cx >> 8) | ((r->cx & 0xC0) << 2), spt = r->cx & 0x3F;
        if (!f) return done(c, r, ST_TIMEOUT, 0);
        if (!fdd_disk(f)) return done(c, r, ST_TIMEOUT, 0);
        if (tracks != 79 || (spt != 18 && spt != 9)) return done(c, r, ST_MEDIA_TYPE, 0);
        r->es = table_seg(c);
        r->di = table_off(c);
        return done(c, r, ST_OK, 0);
    }
    }
    return FDC_BIOS_UNSUPPORTED;
}
