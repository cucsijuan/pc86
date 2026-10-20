/*
 * fdc86 - floppy disk controller for an in-game virtual PC.
 *
 * The controller of an AT-class diskette adapter: a NEC uPD765A with the
 * digital output register (motors, drive select, reset, DMA/IRQ enable), the
 * configuration control register (data rate) and the digital input register
 * (disk change line). Up to two drives (fdd.h) on its cable. Data moves by
 * DMA through a callback, so the controller does not depend on any DMA chip
 * (on a PC: channel 2 of the 8237, dma86) nor on any CPU core.
 *
 * Ports: 3F2h DOR, 3F4h main status, 3F5h data, 3F7h DIR (read) / CCR
 * (write). Interrupts on IRQ 6 on a PC.
 *
 * Emulated commands: SPECIFY, SENSE DRIVE STATUS, RECALIBRATE, SEEK, SENSE
 * INTERRUPT STATUS, READ DATA, WRITE DATA, READ ID, FORMAT TRACK; opcodes the
 * 765A does not have get the invalid-command result (ST0 = 80h), as on the
 * chip. Timing follows the medium: 300 rpm, sectors passing under the head,
 * step rate from SPECIFY. A command on a drive with no diskette or with its
 * motor off waits, as the chip does with no index pulses.
 *
 * Not emulated (reported as unsupported): READ TRACK, READ/WRITE DELETED
 * DATA, SCAN, non-DMA mode, FORMAT TRACK with a sector size or count other
 * than the diskette's. FORMAT TRACK fills the sectors but does not keep the
 * ID fields it is given: the track reads back with IDs C, H, 1..n, 2.
 *
 * It also carries the diskette BIOS (fdc_int13), serviced in C, for the
 * adapter's option ROM.
 */
#ifndef FDC86_H
#define FDC86_H

#include "fdd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FDC_DRIVES 2

enum {
    FDC_PORT_NOT_MINE    = 0,
    FDC_PORT_HANDLED     = 1,
    FDC_PORT_UNSUPPORTED = -1
};

/* What the DMA channel did with a request: as dma86's DMA_XFER_DONE/TC/MASKED */
enum {
    FDC_DMA_DONE   = 0,           /* every byte moved */
    FDC_DMA_TC     = 1,           /* terminal count: the command ends after this sector */
    FDC_DMA_NO_ACK = 2            /* no DACK (channel masked): nothing moved, the controller overruns */
};

/*
 * The controller asserts DREQ: move len bytes. For a read from the diskette
 * buf holds the data for memory; for a write or a format the channel fills it.
 * *moved gets the number of bytes. Returns FDC_DMA_*.
 */
typedef int (*fdc_dma_fn)(void *user, uint8_t *buf, size_t len, size_t *moved);

typedef struct fdc fdc_t;

FDC86_API fdc_t *fdc_create(uint8_t *memory);     /* the 1 MB address space, for the BIOS */
FDC86_API void   fdc_destroy(fdc_t *c);           /* does not destroy the drives */
FDC86_API void   fdc_set_memory(fdc_t *c, uint8_t *memory);
FDC86_API void   fdc_set_dma(fdc_t *c, fdc_dma_fn dma, void *user);
FDC86_API void   fdc_reset(fdc_t *c);             /* power on: DOR 00h, controller held in reset */

/* Drive cable: n = 0 (A:) or 1 (B:), NULL unplugs. -1 for a bad n. */
FDC86_API int    fdc_connect_drive(fdc_t *c, int n, fdd_t *drive);
FDC86_API fdd_t *fdc_drive(const fdc_t *c, int n);

/* Ports 3F0h-3F5h and 3F7h. FDC_PORT_* */
FDC86_API int fdc_port_in(fdc_t *c, uint16_t port, uint8_t *value);
FDC86_API int fdc_port_out(fdc_t *c, uint16_t port, uint8_t value);

/* Clock: seeks, the diskettes turning, transfers. */
FDC86_API void fdc_advance(fdc_t *c, uint32_t microseconds);
/* 1 once for each rising edge of the interrupt line (IRQ 6 on a PC). */
FDC86_API int  fdc_take_irq(fdc_t *c);

/* ---------- diskette BIOS ---------- */

typedef struct fdc_bios_regs {
    uint16_t ax, bx, cx, dx, es, di;
    uint16_t flags;               /* in and out; bit 0 = CF */
} fdc_bios_regs_t;

enum {
    FDC_BIOS_UNSUPPORTED = 0,
    FDC_BIOS_DONE        = 1
};

/*
 * POST part of the adapter's ROM: resets the controller, recalibrates the
 * drives and fills the BIOS data area (equipment bits 0 and 6-7, diskette
 * status bytes at 0040:003E-0048 and 0040:008B-0091). The diskette
 * parameter table is the one INT 1Eh points at (the system BIOS sets it;
 * without one the BIOS uses the 1.44 MB values).
 */
FDC86_API void fdc_bios_init(fdc_t *c);

/*
 * INT 13h for diskettes (DL < 80h): AH = 00h reset, 01h status, 02h read,
 * 03h write, 04h verify, 05h format track, 08h drive parameters, 15h drive
 * type, 16h change line, 17h/18h set type for format. Errors come back in AH
 * with CF, as from the real BIOS (06h media changed, 80h no diskette...). DL
 * >= 80h (hard disks: none on this adapter) gets AH = 01h, CF set.
 * Returns FDC_BIOS_UNSUPPORTED for other functions.
 */
FDC86_API int fdc_int13(fdc_t *c, fdc_bios_regs_t *r);

/* For tests and debuggers */
FDC86_API uint8_t fdc_dor(const fdc_t *c);
FDC86_API int     fdc_busy(const fdc_t *c);       /* a command is executing */

FDC86_API size_t fdc_state_size(void);
FDC86_API size_t fdc_save_state(const fdc_t *c, void *buf, size_t len);
FDC86_API int    fdc_load_state(fdc_t *c, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
