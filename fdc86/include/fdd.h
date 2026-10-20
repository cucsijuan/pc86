/*
 * fdc86 - diskettes and floppy drives for an in-game virtual PC.
 *
 * A diskette (disk_t) is the medium: a sector image with the geometry of a
 * standard format and a write-protect tab. A drive (fdd_t) is the peripheral
 * that plugs into the floppy controller's cable (fdc.h); diskettes go in and
 * out of it at any time, as on a real machine, and the drive's disk change
 * line tells the controller.
 *
 * Drives: 3.5" high density (1.44 MB), which also reads and writes 720 KB
 * double density diskettes, as the real ones do.
 */
#ifndef FDD86_H
#define FDD86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(FDC86_BUILD_DLL)
#  define FDC86_API __declspec(dllexport)
#elif defined(_WIN32) && defined(FDC86_USE_DLL)
#  define FDC86_API __declspec(dllimport)
#else
#  define FDC86_API
#endif

/* Data rates, as selected in the controller's CCR (port 3F7h) */
enum { FDC_RATE_500K = 0, FDC_RATE_300K = 1, FDC_RATE_250K = 2, FDC_RATE_1M = 3 };

/* ---------- diskettes ---------- */

typedef enum disk_format {
    DISK_1440K = 0,               /* 3.5" HD: 80 tracks, 2 sides, 18 sectors, 500 kbps */
    DISK_720K,                    /* 3.5" DD: 80 tracks, 2 sides, 9 sectors, 250 kbps */
    DISK_FORMAT_COUNT
} disk_format_t;

typedef struct disk_geometry {
    const char *name;
    uint8_t tracks, heads, sectors;   /* sectors per track, numbered from 1 */
    uint8_t rate;                     /* FDC_RATE_* the medium is recorded at */
    uint32_t size;                    /* bytes: tracks * heads * sectors * 512 */
} disk_geometry_t;

typedef struct disk disk_t;

FDC86_API const disk_geometry_t *disk_format_geometry(disk_format_t format);

/* A new diskette, low-level formatted, every byte F6h (what FORMAT leaves). */
FDC86_API disk_t *disk_create(disk_format_t format);
/* A diskette from a raw image (.img); the size says the format. NULL if it is
 * not a known size. The data is copied. */
FDC86_API disk_t *disk_create_image(const uint8_t *data, size_t len);
FDC86_API void    disk_destroy(disk_t *d);

FDC86_API const disk_geometry_t *disk_geometry(const disk_t *d);
FDC86_API uint8_t *disk_data(disk_t *d);           /* the image, cylinder-head-sector order */
FDC86_API size_t   disk_size(const disk_t *d);
FDC86_API void     disk_set_write_protect(disk_t *d, int on);
FDC86_API int      disk_write_protected(const disk_t *d);
/* Set whenever the machine writes or formats; the host clears it after saving. */
FDC86_API int      disk_modified(const disk_t *d);
FDC86_API void     disk_clear_modified(disk_t *d);

/* ---------- drives ---------- */

typedef enum fdd_model_id {
    FDD_MODEL_35_HD = 0,          /* 3.5" 1.44 MB */
    FDD_MODEL_COUNT
} fdd_model_id_t;

typedef struct fdd_model {
    const char *name;
    uint8_t tracks;               /* the head moves over tracks 0..tracks-1 */
    uint8_t bios_type;            /* INT 13h AH=08h drive type (CMOS type) */
    uint8_t formats;              /* bit per disk_format_t it reads and writes */
} fdd_model_t;

typedef struct fdd fdd_t;

FDC86_API const fdd_model_t *fdd_builtin_model(int id);
FDC86_API fdd_t *fdd_create(const fdd_model_t *model);
FDC86_API void   fdd_destroy(fdd_t *f);           /* does not destroy the diskette in it */
FDC86_API const fdd_model_t *fdd_model(const fdd_t *f);

/* Any time. Insert returns -1 if there is a diskette in the drive already or
 * the drive does not take that format. Eject returns the diskette, or NULL. */
FDC86_API int     fdd_insert(fdd_t *f, disk_t *disk);
FDC86_API disk_t *fdd_eject(fdd_t *f);
FDC86_API disk_t *fdd_disk(const fdd_t *f);

/* For the host's drive light, and for tests. */
FDC86_API int fdd_motor(const fdd_t *f);           /* spindle motor on */
FDC86_API int fdd_track(const fdd_t *f);           /* where the head is */
FDC86_API int fdd_disk_changed(const fdd_t *f);    /* DSKCHG line: diskette out since the last step */

/* Head position and change line (the diskette is not part of it). */
FDC86_API size_t fdd_state_size(void);
FDC86_API size_t fdd_save_state(const fdd_t *f, void *buf, size_t len);
FDC86_API int    fdd_load_state(fdd_t *f, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
