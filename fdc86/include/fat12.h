/*
 * fdc86 - FAT12 file system on a diskette, for the host.
 *
 * Lets the engine prepare diskettes for the machine and read back what the
 * machine wrote: format a diskette the way DOS does, put files in its root
 * directory, read and delete them, and install a boot sector. The geometry
 * comes from the diskette's own BPB, so images formatted elsewhere work too.
 *
 * Only the root directory is handled (no subdirectories, no long names).
 * Names are 8.3, given as "NAME.EXT" in any case.
 */
#ifndef FAT12_H
#define FAT12_H

#include "fdd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fat12_entry {
    char name[13];                /* "NAME.EXT" */
    uint8_t attr;                 /* 10h directory, 08h volume label, 01h read-only... */
    uint32_t size;
} fat12_entry_t;

/*
 * Writes a boot sector with the BPB for the diskette's format, two empty FATs
 * and an empty root directory. The boot code says the diskette is not a
 * system diskette, waits for a key and calls INT 19h. label may be NULL.
 */
FDC86_API int fat12_format(disk_t *d, const char *label);

/* 1 if sector 0 holds a FAT12 BPB this code understands. */
FDC86_API int fat12_valid(const disk_t *d);

/*
 * Boot sector: copies code (at most 510 bytes, assembled for 0000:7C00) to
 * sector 0, keeping the diskette's BPB (bytes 03h-3Dh) and writing the 55AAh
 * signature. The code must start with a jump over the BPB, e.g.
 *     jmp short start / nop / times 59 db 0
 * 0, or -1 (too long, not FAT12).
 */
FDC86_API int fat12_install_boot(disk_t *d, const uint8_t *code, size_t len);

/* Files in the root directory. 0 / -1 (bad name, no space, not found...). */
FDC86_API int  fat12_write_file(disk_t *d, const char *name, const uint8_t *data, size_t len);  /* replaces */
FDC86_API long fat12_file_size(const disk_t *d, const char *name);                                /* -1: none */
FDC86_API long fat12_read_file(const disk_t *d, const char *name, uint8_t *buf, size_t cap);      /* bytes read */
FDC86_API int  fat12_delete_file(disk_t *d, const char *name);

/* Up to max entries of the root directory (files, directories, label). */
FDC86_API int  fat12_list(const disk_t *d, fat12_entry_t *out, int max);
FDC86_API long fat12_free_bytes(const disk_t *d);

#ifdef __cplusplus
}
#endif
#endif
