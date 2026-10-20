# dma86 — 8237A DMA controller for an in-game virtual PC

A C99 device with no dependencies: the chip that moves bytes between a device and memory without the CPU. On a PC the floppy controller uses channel 2.

| Channel | On a PC/XT | Page register |
|---|---|---|
| 0 | DRAM refresh | `87h` |
| 1 | free (sound cards) | `83h` |
| 2 | floppy controller | `81h` |
| 3 | hard disk (XT) | `82h` |

The 16-bit address of a channel plus its 4-bit page register make the 20-bit address. As on the real chip the address wraps inside its 64 KB page, which is why the floppy BIOS refuses a buffer that crosses a 64 KB boundary.

Like the other devices it does not depend on any CPU core: the host routes ports `00h`–`0Fh` and `80h`–`8Fh` to it, and a device calls `dma_transfer()` when it raises its DREQ line. [pc86](../pc86) does both: the controller is on the motherboard and the floppy controller card asks it for channel 2.

## Status

| Check | Result |
|---|---|
| Unit tests (registers and flip-flop, page registers, write/read/verify transfers, terminal count, decrement, auto-init, 64 KB wrap, masks, master clear, unsupported modes, save state) | 29/29 |
| Machine tests in pc86 (floppy BIOS and a program driving the floppy controller itself) | pass |

## Usage

```c
dma_t *dma = dma_create(memory);           /* every channel masked, as at power on */

/* port callbacks of the machine: */
int r = dma_port_in(dma, port, &value);    /* or dma_port_out; DMA_PORT_NOT_MINE / HANDLED / UNSUPPORTED */

/* a device with DREQ asserted on channel 2: */
size_t moved;
int res = dma_transfer(dma, 2, sector, 512, &moved);
/* DMA_XFER_DONE, DMA_XFER_TC (terminal count: the device ends its command),
 * DMA_XFER_MASKED (no DACK: nothing moved) */
```

The direction is the one the program set in the mode register: a *write* transfer copies the device's buffer to memory, a *read* transfer fills the buffer from memory, *verify* only counts.

## What is emulated

- Ports `00h`–`07h` (base and current address and count, through the byte flip-flop), `08h` command / status, `0Ah` single mask, `0Bh` mode, `0Ch` clear flip-flop, `0Dh` master clear / temporary register, `0Eh` clear mask, `0Fh` all-channel mask.
- Page registers `80h`–`8Fh` (all readable; `81h`, `82h`, `83h`, `87h` drive channels 2, 3, 1, 0).
- Single, block and demand modes (they only differ in bus timing, which is not modeled); write, read and verify transfers; address increment and decrement; auto-initialization.
- Terminal count: status bits 0–3 (cleared by reading the status), and the channel masks itself unless auto-initializing.
- Controller disable (command bit 2).

### Not emulated

Reported as unsupported:

- Memory-to-memory transfers (command bit 0).
- Cascade mode.
- Software DMA requests (port `09h` with bit 2 set).
- Transfer type 3.

## Building

```
make          # build/libdma86.a, build/libdma86.so
make test     # unit tests
```

With CMake: `cmake -S . -B build`, then `cmake --build build`.

## Files

```
include/dma.h       API
src/dma.c           device
tests/dma_test.c    unit tests
```
