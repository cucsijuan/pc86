/*
 * pc86 - VGA card for the expansion slots, built on vga86.
 *
 * The card decodes ports 3B0h-3DFh, uses the machine's memory as its video
 * memory window and services INT 10h from its option ROM. A monitor
 * (vga86's monitor.h) plugs into its connector at any time.
 */
#ifndef PC86_VGA_H
#define PC86_VGA_H

#include "monitor.h"
#include "pc.h"
#include "vga.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pc_vga_card pc_vga_card_t;

PC86_API pc_vga_card_t *pc_vga_card_create(void);
PC86_API void           pc_vga_card_destroy(pc_vga_card_t *card);
PC86_API pc_card_t     *pc_vga_card_slot(pc_vga_card_t *card);   /* what goes into pc_insert_card() */
PC86_API vga_t         *pc_vga_card_vga(pc_vga_card_t *card);    /* NULL until inserted once */

/* Plugs a monitor in (NULL unplugs). Returns -1 if its connector does not fit. */
PC86_API int        pc_vga_card_connect_monitor(pc_vga_card_t *card, monitor_t *monitor);
PC86_API monitor_t *pc_vga_card_monitor(pc_vga_card_t *card);

/*
 * What the connected monitor shows this engine frame: renders the card's
 * signal (none while the machine is off) and passes it through the monitor.
 * out: 640 x 400 x monitor_channels() bytes. Returns MONITOR_*, or -1 if no
 * monitor is connected.
 */
PC86_API int pc_vga_card_display(pc_vga_card_t *card, float dt_seconds, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
