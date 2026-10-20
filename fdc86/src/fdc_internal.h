/*
 * fdc86 - what the controller, its BIOS and the drives share.
 */
#ifndef FDC86_INTERNAL_H
#define FDC86_INTERNAL_H

#include "fdc.h"

#define SECTOR_SIZE   512u
#define REV_US        200000u     /* 300 rpm */
#define SPINUP_US     250000u     /* motor on until the drive gives index pulses */

struct disk {
    const disk_geometry_t *geo;
    uint8_t *data;
    uint8_t wp;
    uint8_t modified;
};

typedef struct fdd_state {
    uint8_t track;
    uint8_t changed;              /* DSKCHG */
    uint8_t motor;
    uint32_t spin_us;             /* time since the motor came on, capped */
} fdd_state_t;

struct fdd {
    const fdd_model_t *model;
    disk_t *disk;
    fdd_state_t s;
};

/* ---------- the controller ---------- */

enum { PH_CMD = 0, PH_EXEC, PH_RESULT };
enum { OP_NONE = 0, OP_READ, OP_WRITE, OP_READ_ID, OP_FORMAT };

typedef struct fdc_state {
    uint8_t dor;                  /* 3F2h */
    uint8_t ccr;                  /* data rate, 3F7h */
    uint8_t phase, op;
    uint8_t cmd[9], cmd_len, cmd_need;
    uint8_t res[7], res_len, res_pos;
    uint8_t srt, hut, hlt;        /* SPECIFY */
    uint8_t pcn[4];               /* present cylinder of each drive, as the controller counts it */
    uint8_t seeking;              /* bit per drive: SEEK/RECALIBRATE in progress */
    uint8_t seek_pcn[4], seek_track[4], seek_head[4], seek_ec[4];
    uint32_t seek_us[4];
    uint8_t int_pending;          /* bit per drive: seek ended, SENSE INTERRUPT STATUS owed */
    uint8_t int_st0[4];
    uint8_t reset_sense;          /* bit per drive: SENSE INTERRUPT STATUS owed after a reset */
    uint8_t irq_line, irq_out, irq_edge;
    /* the command executing */
    uint8_t x_drive, x_head, x_mt, x_mfm;
    uint8_t x_c, x_h, x_r, x_n, x_eot;
    uint8_t x_sc, x_fill, x_count;
    uint8_t x_sync;               /* 0: looking for the sector under the head */
    uint8_t x_fail;               /* the search fails when the timer runs out */
    uint8_t st1, st2;
    int32_t timer_us;
    uint64_t time_us;
} fdc_state_t;

struct fdc {
    uint8_t *mem;
    fdd_t *drive[FDC_DRIVES];
    fdc_dma_fn dma;
    void *dma_user;
    fdc_state_t s;
};

fdd_t *fdc_drive_n(fdc_t *c, int n);              /* NULL past FDC_DRIVES */
void   fdc_write_dor(fdc_t *c, uint8_t value);

/* Used by the controller and its BIOS */
int  fdd_ready(const fdd_t *f);                   /* diskette in, spinning */
void fdd_set_motor(fdd_t *f, int on);
void fdd_spin(fdd_t *f, uint32_t us);
int  fdd_step_to(fdd_t *f, int track);            /* moves the head; returns the steps taken */
/* Sector at cylinder c, head h, sector r (1-based) of the diskette, or NULL. */
uint8_t *disk_sector(disk_t *d, int c, int h, int r);

#endif
