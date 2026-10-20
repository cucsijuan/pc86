# fdc86 — floppy controller, drives and diskettes for an in-game virtual PC

A C99 device with no dependencies: the diskette side of a PC, in three parts that plug together like the real ones.

| Part | Header | What it is |
|---|---|---|
| Diskette | `fdd.h` (`disk_t`) | The medium: a sector image, 1.44 MB (80×2×18, 500 kbps) or 720 KB (80×2×9, 250 kbps), with a write-protect tab |
| Drive | `fdd.h` (`fdd_t`) | 3.5" high density: reads and writes both formats. Diskettes go in and out at any time; the disk change line tells the controller |
| Controller | `fdc.h` (`fdc_t`) | uPD765A with the AT adapter's registers (DOR, CCR, DIR), two drives on its cable, and the diskette BIOS (INT 13h) |
| File system | `fat12.h` | For the host: format like DOS, put files in and take them out, install a boot sector |

Like the other devices it does not depend on any CPU core or on the DMA chip: data moves through a DMA callback. [pc86](../pc86) puts the controller on an ISA card (`pc_fdc.h`) that uses channel 2 of the board's 8237 ([dma86](../dma86)) and IRQ 6.

## Status

| Check | Result |
|---|---|
| Unit tests (diskettes and drives, reset and SENSE INTERRUPT, seek timing and change line, READ/WRITE DATA with terminal count, multi-track, end of cylinder, wrong cylinder, missing sector, wrong data rate, overrun, motor off and no diskette, READ ID, FORMAT TRACK, 720 KB in drive B:, BIOS INT 13h, FAT12, save state) | 87/87 |
| Machine tests in pc86 (booting, a boot sector driving the controller and the 8237 itself, motor timeout, hot-swapping, INT 13h read/write) | pass |
| `examples/boot.asm` booted from 1.44 MB and 720 KB diskettes | lists the files |

## Usage

```c
disk_t *disk = disk_create(DISK_1440K);         /* or disk_create_image(bytes, len) */
fat12_format(disk, "GAME");
fat12_write_file(disk, "GAME.COM", code, size);

fdd_t *a = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD));
fdd_insert(a, disk);                            /* any time; fdd_eject(a) */

fdc_t *fdc = fdc_create(memory);
fdc_connect_drive(fdc, 0, a);
fdc_set_dma(fdc, my_dma, my_user);              /* DREQ: move bytes, FDC_DMA_DONE / TC / NO_ACK */

/* port callbacks of the machine: ports 3F2h, 3F4h, 3F5h, 3F7h */
fdc_port_in(fdc, port, &value);                 /* FDC_PORT_NOT_MINE / HANDLED / UNSUPPORTED */

/* between CPU slices: */
fdc_advance(fdc, us);
if (fdc_take_irq(fdc)) pic_raise(&pic, 6);

/* the adapter's option ROM: */
fdc_bios_init(fdc);                             /* at POST */
fdc_int13(fdc, &regs);                          /* INT 13h, DL < 80h */

if (disk_modified(disk)) save(disk_data(disk), disk_size(disk));
```

`tools/fdimage.c` makes and edits images from the command line:

```
fdimage new  disk.img [--720k] [--label NAME]   formatted FAT12 diskette
fdimage ls   disk.img
fdimage add  disk.img file [NAME.EXT]
fdimage get  disk.img NAME.EXT [file]
fdimage rm   disk.img NAME.EXT
fdimage boot disk.img boot.bin                  boot sector (BPB kept)
```

### Time

The diskette turns at 300 rpm: a sector passes under the head every 11.1 ms (1.44 MB) or 22.2 ms (720 KB), and a read waits for its sector to come round. Seeks take the step rate set by SPECIFY per track. A drive gives no index pulses with its motor off, with no diskette, or for the first 250 ms after the motor starts; a command waits for them, as the chip does.

## What is emulated

### Controller

- Ports `3F2h` DOR (write: drive select, reset, DMA/IRQ enable, motors), `3F4h` main status, `3F5h` data, `3F7h` DIR (read: disk change, bit 7) and CCR (write: data rate). `3F0h`, `3F1h` and reads of `3F2h` are not decoded on an AT adapter (open bus).
- Reset through the DOR: an interrupt, then four SENSE INTERRUPT STATUS (ST0 `C0h`–`C3h`).
- SPECIFY, SENSE DRIVE STATUS, RECALIBRATE (77 steps at most: equipment check if track 0 is not reached), SEEK (overlapping on both drives), SENSE INTERRUPT STATUS.
- READ DATA and WRITE DATA with MT, MFM and SK; terminal count from DMA; end of cylinder; no data and wrong cylinder; missing address mark (wrong data rate or FM); not writable; overrun (DMA channel masked, or DOR bit 3 off).
- READ ID, FORMAT TRACK (fills the sectors with the fill byte).
- Opcodes the 765A does not have (`VERSION`, `CONFIGURE`, `DUMPREG`, relative seeks...) get the invalid-command result, ST0 = `80h`, as on the chip.

### Drive and diskette

- Head position (it can go 3 tracks past the last one), motor, 250 ms spin-up, disk change line: active from power on and after an eject until a step pulse with a diskette in.
- A drive refuses a diskette when it already has one.

### Diskette BIOS (`fdc_int13`)

| AH | Service |
|---|---|
| `00h` | reset |
| `01h` | status of the last operation |
| `02h` / `03h` / `04h` | read / write / verify sectors (multi-track to side 1); DMA boundary error `09h` for a buffer across 64 KB |
| `05h` | format a track |
| `08h` | drive parameters (type 4, 80×2×18, ES:DI = INT 1Eh table) |
| `15h` | drive type: diskette with change line |
| `16h` | change line status |
| `17h` / `18h` | set the type for a format |

As on an AT BIOS, the first access after a diskette change returns `06h` (media changed) and resets the line; with no diskette it times out (`80h`). The BIOS turns the drive's motor on and leaves the motor-off count at `0040:0040` for the timer interrupt. The parameter table is the one `INT 1Eh` points at. `DL >= 80h` (hard disks) returns `01h`.

### Not emulated

Reported as unsupported:

- READ TRACK, READ/WRITE DELETED DATA, SCAN.
- Non-DMA mode (SPECIFY with ND set).
- FORMAT TRACK with a sector size other than 512 bytes or a sector count other than the diskette's; INT 13h AH=05h likewise.
- Other INT 13h functions.

FORMAT TRACK does not keep the ID fields it is given (interleave, non-standard IDs): the track always reads back with IDs C, H, 1..n, 512 bytes. CRC errors, deleted data marks and copy-protection formats do not exist on these diskettes.

## Building

```
make          # build/libfdc86.a, build/libfdc86.so, build/fdimage
make test     # unit tests
```

With CMake: `cmake -S . -B build`, then `cmake --build build`.

## Files

```
include/fdd.h        diskettes and drives
include/fdc.h        controller and diskette BIOS
include/fat12.h      FAT12 for the host
src/fdd.c            diskettes and drives
src/fdc.c            controller
src/fdc_bios.c       INT 13h
src/fat12.c          file system
src/fdc_internal.h   shared between them
tools/fdimage.c      image tool
examples/boot.asm    a boot sector that lists its diskette's files
tests/fdc_test.c     unit tests
```
