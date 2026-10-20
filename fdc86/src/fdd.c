/*
 * fdc86 - diskettes and drives. See include/fdd.h.
 */
#include "fdc_internal.h"

#include <stdlib.h>
#include <string.h>

static const disk_geometry_t GEOMETRY[DISK_FORMAT_COUNT] = {
    {"1.44 MB", 80, 2, 18, FDC_RATE_500K, 80u * 2 * 18 * SECTOR_SIZE},
    {"720 KB",  80, 2,  9, FDC_RATE_250K, 80u * 2 *  9 * SECTOR_SIZE},
};

static const fdd_model_t MODELS[FDD_MODEL_COUNT] = {
    {"3.5\" 1.44 MB", 80, 4, (1u << DISK_1440K) | (1u << DISK_720K)},
};

/* ---------- diskettes ---------- */

const disk_geometry_t *disk_format_geometry(disk_format_t format)
{
    return (unsigned)format < DISK_FORMAT_COUNT ? &GEOMETRY[format] : NULL;
}

disk_t *disk_create(disk_format_t format)
{
    disk_t *d;
    if ((unsigned)format >= DISK_FORMAT_COUNT) return NULL;
    d = (disk_t *)calloc(1, sizeof(disk_t));
    if (!d) return NULL;
    d->geo = &GEOMETRY[format];
    d->data = (uint8_t *)malloc(d->geo->size);
    if (!d->data) { free(d); return NULL; }
    memset(d->data, 0xF6, d->geo->size);
    return d;
}

disk_t *disk_create_image(const uint8_t *data, size_t len)
{
    int f;
    disk_t *d;
    for (f = 0; f < DISK_FORMAT_COUNT; f++)
        if (GEOMETRY[f].size == len) break;
    if (f == DISK_FORMAT_COUNT || !(d = disk_create((disk_format_t)f))) return NULL;
    memcpy(d->data, data, len);
    return d;
}

void disk_destroy(disk_t *d)
{
    if (!d) return;
    free(d->data);
    free(d);
}

const disk_geometry_t *disk_geometry(const disk_t *d) { return d->geo; }
uint8_t *disk_data(disk_t *d) { return d->data; }
size_t disk_size(const disk_t *d) { return d->geo->size; }
void disk_set_write_protect(disk_t *d, int on) { d->wp = (uint8_t)(on != 0); }
int disk_write_protected(const disk_t *d) { return d->wp; }
int disk_modified(const disk_t *d) { return d->modified; }
void disk_clear_modified(disk_t *d) { d->modified = 0; }

uint8_t *disk_sector(disk_t *d, int c, int h, int r)
{
    const disk_geometry_t *g = d->geo;
    if (c < 0 || c >= g->tracks || h < 0 || h >= g->heads || r < 1 || r > g->sectors) return NULL;
    return d->data + (((size_t)c * g->heads + (size_t)h) * g->sectors + (size_t)(r - 1)) * SECTOR_SIZE;
}

/* ---------- drives ---------- */

const fdd_model_t *fdd_builtin_model(int id) { return id >= 0 && id < FDD_MODEL_COUNT ? &MODELS[id] : NULL; }

fdd_t *fdd_create(const fdd_model_t *model)
{
    fdd_t *f;
    if (!model) return NULL;
    f = (fdd_t *)calloc(1, sizeof(fdd_t));
    if (!f) return NULL;
    f->model = model;
    f->s.changed = 1;             /* DSKCHG is active from power on until a step */
    return f;
}

void fdd_destroy(fdd_t *f) { free(f); }
const fdd_model_t *fdd_model(const fdd_t *f) { return f->model; }

int fdd_insert(fdd_t *f, disk_t *disk)
{
    int format;
    if (!disk || f->disk) return -1;
    format = (int)(disk->geo - GEOMETRY);
    if (!(f->model->formats & (1u << format))) return -1;
    f->disk = disk;
    return 0;
}

disk_t *fdd_eject(fdd_t *f)
{
    disk_t *d = f->disk;
    f->disk = NULL;
    if (d) f->s.changed = 1;
    return d;
}

disk_t *fdd_disk(const fdd_t *f) { return f->disk; }
int fdd_motor(const fdd_t *f) { return f->s.motor; }
int fdd_track(const fdd_t *f) { return f->s.track; }
int fdd_disk_changed(const fdd_t *f) { return f->s.changed; }

int fdd_ready(const fdd_t *f) { return f->disk && f->s.motor && f->s.spin_us >= SPINUP_US; }

void fdd_set_motor(fdd_t *f, int on)
{
    if (on && !f->s.motor) f->s.spin_us = 0;
    f->s.motor = (uint8_t)(on != 0);
}

void fdd_spin(fdd_t *f, uint32_t us)
{
    if (f->s.motor && f->s.spin_us < SPINUP_US) f->s.spin_us += us;
}

int fdd_step_to(fdd_t *f, int track)
{
    int max = f->model->tracks + 3, steps;          /* the head goes a little past the last track */
    if (track < 0) track = 0;
    if (track > max) track = max;
    steps = track > f->s.track ? track - f->s.track : f->s.track - track;
    f->s.track = (uint8_t)track;
    if (steps && f->disk) f->s.changed = 0;          /* a step pulse with a diskette in clears DSKCHG */
    return steps;
}

size_t fdd_state_size(void) { return sizeof(fdd_state_t); }

size_t fdd_save_state(const fdd_t *f, void *buf, size_t len)
{
    if (len < sizeof(fdd_state_t)) return 0;
    memcpy(buf, &f->s, sizeof(fdd_state_t));
    return sizeof(fdd_state_t);
}

int fdd_load_state(fdd_t *f, const void *buf, size_t len)
{
    if (len != sizeof(fdd_state_t)) return -1;
    memcpy(&f->s, buf, sizeof(fdd_state_t));
    return 0;
}
