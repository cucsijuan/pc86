/*
 * pc86 - diskette adapter for the expansion slots.
 *
 * An ISA card with the floppy controller (fdc86) and the connector for two
 * drives, A: and B:. It answers ports 3F2h-3F5h and 3F7h, interrupts on IRQ 6,
 * moves data on DMA channel 2 of the motherboard's 8237, and its option ROM
 * provides the diskette BIOS (INT 13h), the way the high-density adapters
 * for XT-class machines did.
 *
 * Drives connect at any time; diskettes go in and out of the drives at any
 * time (fdd_insert/fdd_eject). The card goes in a slot with the power off.
 */
#ifndef PC86_FDC_H
#define PC86_FDC_H

#include "fdc.h"
#include "pc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PC_FDC_IRQ 6
#define PC_FDC_DMA 2

typedef struct pc_fdc_card pc_fdc_card_t;

PC86_API pc_fdc_card_t *pc_fdc_card_create(void);
PC86_API void           pc_fdc_card_destroy(pc_fdc_card_t *c);   /* not the drives */

PC86_API pc_card_t *pc_fdc_card_slot(pc_fdc_card_t *c);          /* for pc_insert_card */
PC86_API fdc_t     *pc_fdc_card_fdc(pc_fdc_card_t *c);

/* Drive cable: n = 0 (A:) or 1 (B:); NULL unplugs. 0, or -1 for a bad n.
 * The BIOS counts the drives at power on. */
PC86_API int    pc_fdc_card_connect_drive(pc_fdc_card_t *c, int n, fdd_t *drive);
PC86_API fdd_t *pc_fdc_card_drive(pc_fdc_card_t *c, int n);

#ifdef __cplusplus
}
#endif
#endif
