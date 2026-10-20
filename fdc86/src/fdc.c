/*
 * fdc86 - the uPD765A floppy controller of an AT-class adapter. See include/fdc.h.
 */
#include "fdc_internal.h"

#include <stdlib.h>
#include <string.h>

/* Commands, low five bits of the first byte */
#define CMD_READ_TRACK   0x02
#define CMD_SPECIFY      0x03
#define CMD_SENSE_DRIVE  0x04
#define CMD_WRITE        0x05
#define CMD_READ         0x06
#define CMD_RECALIBRATE  0x07
#define CMD_SENSE_INT    0x08
#define CMD_WRITE_DEL    0x09
#define CMD_READ_ID      0x0A
#define CMD_READ_DEL     0x0C
#define CMD_FORMAT       0x0D
#define CMD_SEEK         0x0F
#define CMD_SCAN_EQ      0x11
#define CMD_SCAN_LE      0x19
#define CMD_SCAN_HE      0x1D

#define ST0_ABNORMAL 0x40
#define ST0_INVALID  0x80
#define ST0_SEEK_END 0x20
#define ST0_EQUIP    0x10
#define ST1_EN 0x80                   /* end of cylinder */
#define ST1_OR 0x10                   /* overrun */
#define ST1_ND 0x04                   /* no data */
#define ST1_NW 0x02                   /* not writable */
#define ST1_MA 0x01                   /* missing address mark */
#define ST2_WC 0x10                   /* wrong cylinder */

#define RECAL_STEPS 77

fdc_t *fdc_create(uint8_t *memory)
{
    fdc_t *c = (fdc_t *)calloc(1, sizeof(fdc_t));
    if (!c) return NULL;
    c->mem = memory;
    fdc_reset(c);
    return c;
}

void fdc_destroy(fdc_t *c) { free(c); }
void fdc_set_memory(fdc_t *c, uint8_t *memory) { c->mem = memory; }
void fdc_set_dma(fdc_t *c, fdc_dma_fn dma, void *user) { c->dma = dma; c->dma_user = user; }

fdd_t *fdc_drive_n(fdc_t *c, int n) { return n >= 0 && n < FDC_DRIVES ? c->drive[n] : NULL; }
fdd_t *fdc_drive(const fdc_t *c, int n) { return n >= 0 && n < FDC_DRIVES ? c->drive[n] : NULL; }

static void motors(fdc_t *c)
{
    int n;
    for (n = 0; n < FDC_DRIVES; n++)
        if (c->drive[n]) fdd_set_motor(c->drive[n], (c->s.dor >> (4 + n)) & 1);
}

int fdc_connect_drive(fdc_t *c, int n, fdd_t *drive)
{
    if (n < 0 || n >= FDC_DRIVES) return -1;
    if (c->drive[n]) fdd_set_motor(c->drive[n], 0);
    c->drive[n] = drive;
    motors(c);
    return 0;
}

/* The interrupt line reaches the bus only with DOR bit 3 (DMA and IRQ enable). */
static void update_irq(fdc_t *c)
{
    uint8_t out = (uint8_t)(c->s.irq_line && (c->s.dor & 0x08));
    if (out && !c->s.irq_out) c->s.irq_edge = 1;
    c->s.irq_out = out;
}

static void raise_irq(fdc_t *c) { c->s.irq_line = 1; update_irq(c); }
static void lower_irq(fdc_t *c) { c->s.irq_line = 0; update_irq(c); }

/* What a reset leaves; SPECIFY values and present cylinders are kept. */
static void controller_reset(fdc_t *c)
{
    c->s.phase = PH_CMD;
    c->s.op = OP_NONE;
    c->s.cmd_len = 0;
    c->s.res_len = c->s.res_pos = 0;
    c->s.seeking = 0;
    c->s.int_pending = 0;
    c->s.reset_sense = 0;
    c->s.irq_line = 0;
}

void fdc_reset(fdc_t *c)
{
    memset(&c->s, 0, sizeof(c->s));
    controller_reset(c);
    c->s.srt = 0x0D;              /* until SPECIFY: 3 ms steps */
    motors(c);
}

void fdc_write_dor(fdc_t *c, uint8_t value)
{
    uint8_t old = c->s.dor;
    c->s.dor = value;
    motors(c);
    if (!(value & 0x04)) controller_reset(c);                  /* held in reset */
    else if (!(old & 0x04)) {                                   /* reset released: one interrupt, */
        controller_reset(c);                                    /* then a SENSE INTERRUPT per drive */
        c->s.reset_sense = 0x0F;
        c->s.irq_line = 1;
    }
    update_irq(c);
}

/* ---------- medium timing ---------- */

static uint32_t sector_us(const disk_geometry_t *g) { return REV_US / g->sectors; }
static uint32_t rotation(const fdc_t *c) { return (uint32_t)(c->s.time_us % REV_US); }

/* Time until sector r (1-based) starts passing under the head. */
static uint32_t wait_sector(const fdc_t *c, const disk_geometry_t *g, int r)
{
    uint32_t at = (uint32_t)(r - 1) * sector_us(g);
    return (at + REV_US - rotation(c)) % REV_US;
}

static uint32_t step_us(const fdc_t *c)
{
    uint32_t us = (16u - c->s.srt) * 1000u;                     /* at 500 kbps */
    switch (c->s.ccr) {
    case FDC_RATE_250K: return us * 2;
    case FDC_RATE_300K: return us * 5 / 3;
    case FDC_RATE_1M: return us / 2;
    }
    return us;
}

/* ---------- results ---------- */

static void result(fdc_t *c, const uint8_t *bytes, int n)
{
    memcpy(c->s.res, bytes, (size_t)n);
    c->s.res_len = (uint8_t)n;
    c->s.res_pos = 0;
    c->s.phase = PH_RESULT;
}

/* End of READ DATA, WRITE DATA, READ ID or FORMAT TRACK: seven result bytes
 * and an interrupt. advance: the sector in x_r was the last one transferred. */
static void finish(fdc_t *c, uint8_t st0, int advance)
{
    uint8_t r[7];
    uint8_t cy = c->s.x_c, h = c->s.x_h, s = c->s.x_r;
    if (advance) {
        if (s < c->s.x_eot) s++;
        else {
            s = 1;
            if (c->s.x_mt && !(h & 1)) h ^= 1;
            else { cy++; if (c->s.x_mt) h ^= 1; }
        }
    }
    r[0] = (uint8_t)(st0 | c->s.x_head << 2 | c->s.x_drive);
    r[1] = c->s.st1;
    r[2] = c->s.st2;
    r[3] = cy; r[4] = h; r[5] = s; r[6] = c->s.x_n;
    c->s.op = OP_NONE;
    result(c, r, 7);
    raise_irq(c);
}

/* ---------- seeks ---------- */

static void start_seek(fdc_t *c, int recal)
{
    int d = c->s.cmd[1] & 3, steps, track;
    fdd_t *f = fdc_drive_n(c, d);
    c->s.seek_head[d] = (uint8_t)((c->s.cmd[1] >> 2) & 1);
    c->s.seek_ec[d] = 0;
    if (recal) {
        c->s.seek_pcn[d] = 0;
        if (f) {
            steps = fdd_track(f) < RECAL_STEPS ? fdd_track(f) : RECAL_STEPS;
            track = fdd_track(f) - steps;
            c->s.seek_ec[d] = track != 0;                       /* no TRK0 after 77 steps */
        } else {
            steps = RECAL_STEPS;
            track = 0;
            c->s.seek_ec[d] = 1;                                /* nothing on the cable */
        }
    } else {
        int ncn = c->s.cmd[2], pcn = c->s.pcn[d];
        steps = ncn > pcn ? ncn - pcn : pcn - ncn;
        track = f ? fdd_track(f) + (ncn > pcn ? steps : -steps) : 0;
        c->s.seek_pcn[d] = (uint8_t)ncn;
    }
    c->s.seek_track[d] = (uint8_t)(track < 0 ? 0 : track > 255 ? 255 : track);
    c->s.seek_us[d] = (uint32_t)steps * step_us(c);
    c->s.seeking |= (uint8_t)(1u << d);
    c->s.phase = PH_CMD;
}

static void end_seek(fdc_t *c, int d)
{
    fdd_t *f = fdc_drive_n(c, d);
    c->s.seeking &= (uint8_t)~(1u << d);
    c->s.pcn[d] = c->s.seek_pcn[d];
    if (f) fdd_step_to(f, c->s.seek_track[d]);
    c->s.int_st0[d] = (uint8_t)(ST0_SEEK_END | (c->s.seek_ec[d] ? ST0_ABNORMAL | ST0_EQUIP : 0) |
                                c->s.seek_head[d] << 2 | d);
    c->s.int_pending |= (uint8_t)(1u << d);
    raise_irq(c);
}

/* ---------- commands ---------- */

static int command_length(uint8_t b)
{
    uint8_t op = b & 0x1F, extra = b & 0xE0, allowed;
    int len;
    switch (op) {
    case CMD_READ: case CMD_WRITE: case CMD_READ_TRACK: case CMD_READ_DEL: case CMD_WRITE_DEL:
    case CMD_SCAN_EQ: case CMD_SCAN_LE: case CMD_SCAN_HE:
        len = 9; allowed = op == CMD_READ_TRACK ? 0x60 : (op == CMD_WRITE || op == CMD_WRITE_DEL) ? 0xC0 : 0xE0; break;
    case CMD_READ_ID: len = 2; allowed = 0x40; break;
    case CMD_FORMAT: len = 6; allowed = 0x40; break;
    case CMD_SPECIFY: len = 3; allowed = 0; break;
    case CMD_SENSE_DRIVE: len = 2; allowed = 0; break;
    case CMD_RECALIBRATE: len = 2; allowed = 0; break;
    case CMD_SENSE_INT: len = 1; allowed = 0; break;
    case CMD_SEEK: len = 3; allowed = 0; break;
    default: return 0;
    }
    return extra & ~allowed ? 0 : len;
}

static void start_transfer(fdc_t *c, int op)
{
    const uint8_t *b = c->s.cmd;
    c->s.op = (uint8_t)op;
    c->s.x_drive = b[1] & 3;
    c->s.x_head = (b[1] >> 2) & 1;
    c->s.x_mt = (uint8_t)(op != OP_FORMAT && op != OP_READ_ID && (b[0] & 0x80));
    c->s.x_mfm = (uint8_t)((b[0] & 0x40) != 0);
    c->s.st1 = c->s.st2 = 0;
    c->s.x_sync = 0;
    c->s.x_fail = 0;
    if (op == OP_READ || op == OP_WRITE) {
        c->s.x_c = b[2]; c->s.x_h = b[3]; c->s.x_r = b[4]; c->s.x_n = b[5]; c->s.x_eot = b[6];
    } else if (op == OP_FORMAT) {
        c->s.x_n = b[2]; c->s.x_sc = b[3]; c->s.x_fill = b[5]; c->s.x_count = 0;
        c->s.x_c = c->s.pcn[c->s.x_drive]; c->s.x_h = c->s.x_head; c->s.x_r = 1; c->s.x_eot = b[3];
    } else {
        c->s.x_c = c->s.x_h = c->s.x_r = 0; c->s.x_n = 2; c->s.x_eot = 0xFF;
    }
    c->s.phase = PH_EXEC;
}

static int execute(fdc_t *c)
{
    const uint8_t *b = c->s.cmd;
    uint8_t r[2];
    int d = b[1] & 3;
    fdd_t *f = fdc_drive_n(c, d);

    switch (b[0] & 0x1F) {
    case CMD_SPECIFY:
        if (b[2] & 1) return FDC_PORT_UNSUPPORTED;              /* non-DMA mode */
        c->s.srt = b[1] >> 4;
        c->s.hut = b[1] & 0x0F;
        c->s.hlt = b[2] >> 1;
        c->s.phase = PH_CMD;
        return FDC_PORT_HANDLED;
    case CMD_SENSE_DRIVE:
        r[0] = (uint8_t)(0x20 | (b[1] & 7));                    /* ready: tied high on a PC */
        if (f) {
            r[0] |= 0x08;                                       /* two-sided */
            if (fdd_track(f) == 0) r[0] |= 0x10;
            if (fdd_disk(f) && disk_write_protected(fdd_disk(f))) r[0] |= 0x40;
        }
        result(c, r, 1);
        return FDC_PORT_HANDLED;
    case CMD_RECALIBRATE: start_seek(c, 1); return FDC_PORT_HANDLED;
    case CMD_SEEK: start_seek(c, 0); return FDC_PORT_HANDLED;
    case CMD_SENSE_INT: {
        int n;
        lower_irq(c);
        for (n = 0; n < 4; n++) {
            if (c->s.reset_sense & (1u << n)) {
                c->s.reset_sense &= (uint8_t)~(1u << n);
                r[0] = (uint8_t)(0xC0 | n); r[1] = c->s.pcn[n];
                result(c, r, 2);
                return FDC_PORT_HANDLED;
            }
        }
        for (n = 0; n < 4; n++) {
            if (c->s.int_pending & (1u << n)) {
                c->s.int_pending &= (uint8_t)~(1u << n);
                r[0] = c->s.int_st0[n]; r[1] = c->s.pcn[n];
                result(c, r, 2);
                return FDC_PORT_HANDLED;
            }
        }
        r[0] = ST0_INVALID;
        result(c, r, 1);
        return FDC_PORT_HANDLED;
    }
    case CMD_READ: start_transfer(c, OP_READ); return FDC_PORT_HANDLED;
    case CMD_WRITE: start_transfer(c, OP_WRITE); return FDC_PORT_HANDLED;
    case CMD_READ_ID: start_transfer(c, OP_READ_ID); return FDC_PORT_HANDLED;
    case CMD_FORMAT: {
        const disk_geometry_t *g = f && fdd_disk(f) ? disk_geometry(fdd_disk(f)) : NULL;
        if (b[2] != 2 || (g ? b[3] != g->sectors : (b[3] != 9 && b[3] != 18))) return FDC_PORT_UNSUPPORTED;
        start_transfer(c, OP_FORMAT);
        return FDC_PORT_HANDLED;
    }
    }
    return FDC_PORT_UNSUPPORTED;                                /* READ TRACK, deleted data, SCAN */
}

static int data_write(fdc_t *c, uint8_t value)
{
    int len;
    if (c->s.phase != PH_CMD) return FDC_PORT_HANDLED;          /* not expecting a byte: ignored */
    if (c->s.cmd_len == 0) {
        len = command_length(value);
        if (!len) {                                             /* invalid opcode */
            uint8_t st0 = ST0_INVALID;
            result(c, &st0, 1);
            return FDC_PORT_HANDLED;
        }
        c->s.cmd_need = (uint8_t)len;
    }
    c->s.cmd[c->s.cmd_len++] = value;
    if (c->s.cmd_len < c->s.cmd_need) return FDC_PORT_HANDLED;
    c->s.cmd_len = 0;
    return execute(c);
}

static uint8_t data_read(fdc_t *c)
{
    uint8_t v;
    if (c->s.phase != PH_RESULT) return 0xFF;
    v = c->s.res[c->s.res_pos++];
    if (c->s.res_pos == 1 && c->s.res_len == 7) lower_irq(c);  /* reading ST0 clears the interrupt */
    if (c->s.res_pos >= c->s.res_len) c->s.phase = PH_CMD;
    return v;
}

static uint8_t main_status(const fdc_t *c)
{
    uint8_t v;
    if (!(c->s.dor & 0x04)) return 0x00;                        /* in reset */
    switch (c->s.phase) {
    case PH_EXEC: v = 0x10; break;
    case PH_RESULT: v = 0xD0; break;
    default: v = (uint8_t)(0x80 | (c->s.cmd_len ? 0x10 : 0)); break;
    }
    return (uint8_t)(v | (c->s.seeking & 0x0F));
}

/* ---------- ports ---------- */

int fdc_port_in(fdc_t *c, uint16_t port, uint8_t *value)
{
    switch (port) {
    case 0x3F4: *value = main_status(c); return FDC_PORT_HANDLED;
    case 0x3F5: *value = (c->s.dor & 0x04) ? data_read(c) : 0xFF; return FDC_PORT_HANDLED;
    case 0x3F7: {
        int sel = c->s.dor & 3;
        fdd_t *f = fdc_drive_n(c, sel);
        int chg = f && ((c->s.dor >> (4 + sel)) & 1) && fdd_disk_changed(f);
        *value = (uint8_t)(chg << 7 | 0x7F);                    /* bits 0-6 belong to a hard disk adapter */
        return FDC_PORT_HANDLED;
    }
    }
    return FDC_PORT_NOT_MINE;                                   /* the DOR is write-only on an AT adapter */
}

int fdc_port_out(fdc_t *c, uint16_t port, uint8_t value)
{
    switch (port) {
    case 0x3F2: fdc_write_dor(c, value); return FDC_PORT_HANDLED;
    case 0x3F4: return FDC_PORT_HANDLED;                        /* read-only on an AT adapter */
    case 0x3F5: return (c->s.dor & 0x04) ? data_write(c, value) : FDC_PORT_HANDLED;
    case 0x3F7: c->s.ccr = value & 3; return FDC_PORT_HANDLED;
    }
    return FDC_PORT_NOT_MINE;
}

/* ---------- execution ---------- */

static int dma(fdc_t *c, uint8_t *buf, size_t len, size_t *moved)
{
    *moved = 0;
    if (!(c->s.dor & 0x08) || !c->dma) return FDC_DMA_NO_ACK;  /* DRQ gated off */
    return c->dma(c->dma_user, buf, len, moved);
}

/* The head finds its place on the turning diskette: sets the timer for the
 * next event of the command, or ends it. 0 while the drive cannot read. */
static int sync(fdc_t *c)
{
    fdd_t *f = fdc_drive_n(c, c->s.x_drive);
    disk_t *disk;
    const disk_geometry_t *g;
    int track, readable;

    if (!f || !fdd_ready(f)) return 0;                          /* no index pulses: keep waiting */
    disk = fdd_disk(f);
    g = disk_geometry(disk);
    track = fdd_track(f);
    readable = c->s.x_mfm && c->s.ccr == g->rate && track < g->tracks;
    c->s.x_sync = 1;
    c->s.x_fail = 0;

    if ((c->s.op == OP_WRITE || c->s.op == OP_FORMAT) && disk_write_protected(disk)) {
        c->s.st1 = ST1_NW;
        finish(c, ST0_ABNORMAL, 0);
        return 1;
    }
    if (!readable) {                                            /* no address marks in two revolutions */
        c->s.st1 = ST1_MA;
        c->s.x_fail = 1;
        c->s.timer_us = (int32_t)(2 * REV_US);
        return 1;
    }
    switch (c->s.op) {
    case OP_READ:
    case OP_WRITE:
        if (c->s.x_c != track || c->s.x_h != c->s.x_head || c->s.x_n != 2 || c->s.x_r < 1 || c->s.x_r > g->sectors) {
            c->s.st1 = ST1_ND;
            if (c->s.x_c != track) c->s.st2 = ST2_WC;
            c->s.x_fail = 1;
            c->s.timer_us = (int32_t)(2 * REV_US);
            return 1;
        }
        c->s.timer_us = (int32_t)(wait_sector(c, g, c->s.x_r) + sector_us(g));
        return 1;
    case OP_READ_ID: {
        uint32_t s = (rotation(c) + sector_us(g) - 1) / sector_us(g) % g->sectors;
        c->s.x_c = (uint8_t)track; c->s.x_h = c->s.x_head; c->s.x_r = (uint8_t)(s + 1);
        c->s.timer_us = (int32_t)(wait_sector(c, g, (int)s + 1) + sector_us(g) / 16);
        return 1;
    }
    case OP_FORMAT:
        c->s.x_c = (uint8_t)track;
        c->s.timer_us = (int32_t)((REV_US - rotation(c)) % REV_US + sector_us(g));   /* from the index hole */
        return 1;
    }
    return 1;
}

/* The timer ran out: one sector (or ID field) has passed under the head. */
static void event(fdc_t *c)
{
    fdd_t *f = fdc_drive_n(c, c->s.x_drive);
    disk_t *disk = fdd_disk(f);
    const disk_geometry_t *g = disk_geometry(disk);
    uint8_t buf[SECTOR_SIZE];
    size_t moved;
    int r;

    if (c->s.x_fail) { finish(c, ST0_ABNORMAL, 0); return; }

    switch (c->s.op) {
    case OP_READ_ID:
        finish(c, 0, 0);
        return;
    case OP_FORMAT: {
        uint8_t *p;
        r = dma(c, buf, 4, &moved);                             /* C, H, R, N of the sector */
        if (r == FDC_DMA_NO_ACK) { c->s.st1 = ST1_OR; finish(c, ST0_ABNORMAL, 0); return; }
        p = disk_sector(disk, c->s.x_c, c->s.x_head, c->s.x_count + 1);
        if (p) memset(p, c->s.x_fill, SECTOR_SIZE);
        disk->modified = 1;
        c->s.x_r = (uint8_t)(c->s.x_count + 1);
        if (++c->s.x_count >= c->s.x_sc || r == FDC_DMA_TC) { finish(c, 0, 0); return; }
        c->s.timer_us += (int32_t)sector_us(g);
        return;
    }
    }

    /* READ DATA / WRITE DATA */
    {
        uint8_t *sector = disk_sector(disk, c->s.x_c, c->s.x_h, c->s.x_r);
        if (c->s.op == OP_READ) {
            memcpy(buf, sector, SECTOR_SIZE);
            r = dma(c, buf, SECTOR_SIZE, &moved);
        } else {
            memset(buf, 0, SECTOR_SIZE);                        /* after TC the rest is written as 00h */
            r = dma(c, buf, SECTOR_SIZE, &moved);
            if (r != FDC_DMA_NO_ACK) { memcpy(sector, buf, SECTOR_SIZE); disk->modified = 1; }
        }
    }
    if (r == FDC_DMA_NO_ACK) { c->s.st1 = ST1_OR; finish(c, ST0_ABNORMAL, 0); return; }
    if (r == FDC_DMA_TC) { finish(c, 0, 1); return; }
    if (c->s.x_r < c->s.x_eot) {
        c->s.x_r++;
        if (c->s.x_r > g->sectors) { c->s.x_sync = 0; return; } /* not on this track: the search fails */
        c->s.timer_us += (int32_t)sector_us(g);
        return;
    }
    if (c->s.x_mt && c->s.x_head == 0) {                        /* multi-track: on to side 1 */
        c->s.x_head = 1; c->s.x_h = 1; c->s.x_r = 1;
        c->s.x_sync = 0;
        return;
    }
    c->s.st1 = ST1_EN;                                          /* past EOT without terminal count */
    c->s.x_mt = 0;
    finish(c, ST0_ABNORMAL, 1);
}

void fdc_advance(fdc_t *c, uint32_t microseconds)
{
    int n;
    c->s.time_us += microseconds;
    for (n = 0; n < FDC_DRIVES; n++)
        if (c->drive[n]) fdd_spin(c->drive[n], microseconds);

    for (n = 0; n < 4; n++) {
        if (!(c->s.seeking & (1u << n))) continue;
        if (c->s.seek_us[n] <= microseconds) end_seek(c, n);
        else c->s.seek_us[n] -= microseconds;
    }

    if (c->s.phase == PH_EXEC && (c->s.dor & 0x04)) {
        fdd_t *f = fdc_drive_n(c, c->s.x_drive);
        if (c->s.x_sync && !(f && fdd_ready(f))) c->s.x_sync = 0;   /* diskette out or motor off: lost */
        if (!c->s.x_sync) sync(c);
        else {
            c->s.timer_us -= (int32_t)microseconds;
            while (c->s.phase == PH_EXEC && c->s.x_sync && c->s.timer_us <= 0) event(c);
        }
    }
    update_irq(c);
}

int fdc_take_irq(fdc_t *c)
{
    int e = c->s.irq_edge;
    c->s.irq_edge = 0;
    return e;
}

uint8_t fdc_dor(const fdc_t *c) { return c->s.dor; }
int fdc_busy(const fdc_t *c) { return c->s.phase == PH_EXEC; }

size_t fdc_state_size(void) { return sizeof(fdc_state_t); }

size_t fdc_save_state(const fdc_t *c, void *buf, size_t len)
{
    if (len < sizeof(fdc_state_t)) return 0;
    memcpy(buf, &c->s, sizeof(fdc_state_t));
    return sizeof(fdc_state_t);
}

int fdc_load_state(fdc_t *c, const void *buf, size_t len)
{
    if (len != sizeof(fdc_state_t)) return -1;
    memcpy(&c->s, buf, sizeof(fdc_state_t));
    motors(c);
    return 0;
}
