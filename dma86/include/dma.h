/*
 * dma86 - 8237A DMA controller for an in-game virtual PC.
 *
 * Moves bytes between devices and memory without the CPU, as on a PC/XT:
 * four channels, 16-bit address and count registers, and the page registers
 * (ports 81h, 82h, 83h, 87h) that give the upper 4 bits of the 20-bit address.
 * On a PC the floppy controller uses channel 2.
 *
 * Emulated: ports 00h-0Fh (address and count registers, command, status,
 * single and all-channel masks, mode, flip-flop, master clear) and 80h-8Fh
 * (page registers). Single, block and demand modes, read/write/verify
 * transfers, address increment and decrement, auto-initialization, terminal
 * count (status bits, and the channel masks itself unless auto-initializing).
 * As on the real chip the address wraps inside its 64 KB page.
 *
 * Not emulated (reported as unsupported): memory-to-memory transfers,
 * cascade mode and software DMA requests (port 09h).
 *
 * It does not depend on any CPU core: the host routes the ports to it and a
 * device calls dma_transfer() when it raises its DREQ line.
 */
#ifndef DMA86_H
#define DMA86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(DMA86_BUILD_DLL)
#  define DMA86_API __declspec(dllexport)
#elif defined(_WIN32) && defined(DMA86_USE_DLL)
#  define DMA86_API __declspec(dllimport)
#else
#  define DMA86_API
#endif

#define DMA_CHANNELS 4

enum {
    DMA_PORT_NOT_MINE    = 0,
    DMA_PORT_HANDLED     = 1,
    DMA_PORT_UNSUPPORTED = -1
};

/* What dma_transfer() did. */
enum {
    DMA_XFER_DONE        = 0,     /* every byte asked for was moved */
    DMA_XFER_TC          = 1,     /* stopped at terminal count (the last byte moved was the last one programmed) */
    DMA_XFER_MASKED      = 2,     /* channel masked or controller disabled: nothing moved */
    DMA_XFER_UNSUPPORTED = -1     /* programmed for something not emulated (see above) */
};

/* Transfer types, mode register bits 2-3 */
enum { DMA_VERIFY = 0, DMA_WRITE = 1, DMA_READ = 2 };   /* write = device to memory */

typedef struct dma dma_t;

DMA86_API dma_t *dma_create(uint8_t *memory);          /* the machine's 1 MB address space */
DMA86_API void   dma_destroy(dma_t *d);
DMA86_API void   dma_set_memory(dma_t *d, uint8_t *memory);
DMA86_API void   dma_reset(dma_t *d);                  /* power-on: every channel masked */

/* Ports 00h-0Fh and 80h-8Fh. DMA_PORT_* */
DMA86_API int dma_port_in(dma_t *d, uint16_t port, uint8_t *value);
DMA86_API int dma_port_out(dma_t *d, uint16_t port, uint8_t value);

/*
 * A device on channel ch requests up to len bytes. The direction is what the
 * channel was programmed for: a write transfer copies buf to memory, a read
 * transfer fills buf from memory, a verify transfer only counts. *moved gets
 * the number of bytes transferred. Returns DMA_XFER_*.
 */
DMA86_API int dma_transfer(dma_t *d, int ch, uint8_t *buf, size_t len, size_t *moved);

/* For tests and debuggers: the channel's programmed transfer type (DMA_VERIFY/
 * WRITE/READ), its current 20-bit address and its remaining count - 1. */
DMA86_API int      dma_mode_type(const dma_t *d, int ch);
DMA86_API uint32_t dma_address(const dma_t *d, int ch);
DMA86_API uint16_t dma_count(const dma_t *d, int ch);
DMA86_API int      dma_masked(const dma_t *d, int ch);

DMA86_API size_t dma_state_size(void);
DMA86_API size_t dma_save_state(const dma_t *d, void *buf, size_t len);
DMA86_API int    dma_load_state(dma_t *d, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
