# pc86 — the motherboard of an in-game virtual PC

Puts the independent components together into a machine with expansion slots and connectors, the way a real PC is put together:

```
                    ┌──────────────────── motherboard (fixed) ────────────────────┐
                    │  8086 CPU (vm86)   640 KB RAM   system BIOS ROM             │
 keyboard (kbd86) ──┤  8042 keyboard controller (kbd86)   8259 PIC   8253 PIT     │
                    │  8237 DMA (dma86)   port 61h (speaker, refresh)             │
                    │  ISA slots: [VGA card]──── monitor (vga86)                  │
                    │             [diskette adapter]──── drives A:, B: (fdc86)    │
                    │             [ ] [ ] [ ] [ ]                                 │
                    └─────────────────────────────────────────────────────────────┘
```

- **On the board** (not interchangeable): CPU, RAM, PIC, PIT, DMA controller, keyboard controller, port 61h, BIOS.
- **Expansion slots**: cards (`pc_card_t`) are inserted and removed with the power off. A VGA card (`pc_vga.h`) and a diskette adapter (`pc_fdc.h`) are included; future cards (sound, serial...) implement the same interface.
- **Peripherals**: the keyboard plugs into the board, the monitor into the video card, the floppy drives into the diskette adapter. They can be plugged and unplugged while the machine runs, diskettes go in and out of the drives at any time, and different models behave differently (see [kbd86](../kbd86), [vga86](../vga86) and [fdc86](../fdc86)).

## Status

| Check | Result |
|---|---|
| Machine tests (POST, boot message, BIOS timer, INT 1Ch and own INT 08h, BIOS services, monitor sense and hot-swap, keyboard hot-plug, port 61h, open bus, error reporting, save state, booting diskettes, a boot sector driving the floppy and DMA controllers, motor timeout, diskette hot-swap, INT 13h, machine without video) | 55/55 |
| Same tests under AddressSanitizer + UBSan | 0 errors |
| `examples/clock.asm` with `pcrun` | screenshots checked |
| `examples/piano.asm`, `examples/music.asm` in the [workbench](../workbench) | tone frequencies checked |

Compiles without warnings as C99 and as C++17.

## Usage

```c
#include "pc.h"
#include "pc_fdc.h"
#include "pc_vga.h"

pc_t *pc = pc_create();
pc_vga_card_t *vga = pc_vga_card_create();
pc_fdc_card_t *fdc = pc_fdc_card_create();
monitor_t *mon = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_MONO));
kbd_t *kbd = kbd_create(kbd_builtin_model(KBD_MODEL_ENHANCED101));
fdd_t *a = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD));
disk_t *disk = disk_create_image(img, img_size); /* or disk_create(DISK_1440K) + fat12_format() */

pc_insert_card(pc, 0, pc_vga_card_slot(vga));   /* power off only */
pc_insert_card(pc, 1, pc_fdc_card_slot(fdc));
pc_vga_card_connect_monitor(vga, mon);           /* any time; -1 if the connector does not fit */
pc_connect_keyboard(pc, kbd);                    /* any time; NULL unplugs */
pc_fdc_card_connect_drive(fdc, 0, a);            /* drive A: */
fdd_insert(a, disk);                             /* any time; fdd_eject(a) */
pc_power_on(pc);                                 /* POST, then boots the diskette in A: */
pc_load_com(pc, program, size);                  /* or loads a .COM straight into memory */

/* every engine frame: */
pc_run(pc, delta_seconds);
pc_vga_card_display(vga, delta_seconds, picture); /* 640x400 x monitor_channels(mon) */
double tone = pc_speaker_hz(pc);                  /* 0 = silent */

/* input: */
kbd_key(kbd, kbd_key_from_code("KeyA"), pressed);
```

`tools/pcrun.c` is a complete example that also saves screenshots, boots diskette images and can type on the keyboard, which makes it a test driver for software on diskettes:

```
make demo                       # assembles examples/clock.asm and runs it for 3 s
build/pcrun program.com [--seconds S] [--every N] [--out prefix] [--ips N] [--color]
build/pcrun --a system.img --b data.img --type "dir b:\n" --until "A:\>" --text
```

| Option | |
|---|---|
| `program.com` | loaded after the POST, as DOS would; without it the machine boots from drive A: |
| `--a` / `--b image` | the diskette adapter in slot 1 with two drives, and these images (1.44 MB or 720 KB) in them |
| `--a2` / `--b2 image` | a second diskette, put in the drive instead of the first one when `--type` reaches `\1` (A:) or `\2` (B:) |
| `--protect a\|b` | the write-protect tab of that diskette |
| `--save` | writes the images back if the machine changed them |
| `--type TEXT` | types TEXT, a key each time the BIOS keyboard buffer is empty: `\n` Enter, `\b` Backspace, `\e` Escape, `\t` Tab, `\c` Ctrl+C, `\\` a backslash, `\{Name}` any other key by its name (`\{F2}`, `\{ArrowUp}`, `\{Ctrl+Home}`) |
| `--until TEXT` | stops when everything has been typed and TEXT is on the cursor's row (exit code 3 if that never happens) |
| `--text` | prints the text screen at the end |

### Time

`pc_run()` takes emulated time, not instructions. The CPU runs in 40 µs slices (`PC_SLICE_US`); after each slice every clock advances (PIT, keyboard, cards) and pending interrupts are delivered through the PIC. 40 µs is below the 63 µs VGA retrace pulse, so programs that poll port `3DAh` never miss it. The speed is set in instructions per second (`pc_set_speed`, default 400,000, about a 4.77 MHz 8088); it changes how much a program gets done, not how fast time passes for it.

### BIOS

| Vector | Service |
|---|---|
| `08h` | timer tick: real 8086 code at `F000:FEA5` — counts ticks at `0040:006C`, sets the midnight flag, calls `INT 1Ch`, sends EOI |
| `09h` | keyboard interrupt (kbd86's BIOS) |
| `08h` (also) | counts down the diskette motor timeout at `0040:0040` and turns the motors off (`OUT 3F2h, 0Ch`) |
| `0Eh` | diskette interrupt (IRQ 6): real 8086 code at `F000:EF57` — sets bit 7 of `0040:003E`, sends EOI |
| `0Ah`–`0Dh`, `0Fh` | unexpected hardware interrupt: EOI and return |
| `10h` | the video card's ROM (none without a card) |
| `11h` / `12h` | equipment list (diskette drives counted by the adapter's ROM) / memory size (640 KB) |
| `13h` | the diskette adapter's ROM (none without the card; see [fdc86](../fdc86)) |
| `16h` | keyboard services (kbd86's BIOS) |
| `19h` | bootstrap: real 8086 code at `F000:E6F2` — reads sector 1 of drive A: to `0000:7C00` through INT 13h (four tries), checks the `55AAh` signature and jumps to it with DL = 0. Otherwise it prints "No boot device available." or "The diskette in drive A: is not bootable." and the CPU idles in `HLT` with interrupts on |
| `1Eh` | pointer to the diskette parameter table at `F000:EFC7` (1.44 MB values) |
| `1Ah` | `AH=00h`/`01h` read/set the tick count; the rest (real-time clock) is reported as unsupported |
| others below `20h` | `IRET` at `F000:FF53` |

Services are provided in C while the vector still points at the BIOS address; a program that installs its own handler gets the real behavior, as on a real PC. The POST leaves IRQ 0, 1 and 6 enabled in the PIC.

The bootstrap enters the boot sector at `0000:7C00` with an `IRET`. Boot sectors and loaders can use far jumps, calls and returns (`jmp 0000h:7C00h`, `call far [bx]`, `retf`) and `LDS`/`LES`, for example to read the diskette parameter table through the `INT 1Eh` vector (see [vm86](../vm86)).

### Ports

| Port | Device |
|---|---|
| `00h`–`0Fh`, `80h`–`8Fh` | DMA controller and page registers |
| `20h`–`21h` | PIC |
| `40h`–`43h` | PIT |
| `60h`, `64h` | keyboard controller |
| `61h` | bit 0 speaker gate (PIT channel 2), bit 1 speaker data, bit 4 refresh toggle, bit 5 PIT channel 2 output |
| others | the cards in the slots; nobody: reads `FFh`, writes ignored (empty bus) |

A port a device decodes but does not emulate stops the machine with `PC_ERROR` and a message in `pc_error()`, like an unsupported instruction or BIOS call.

### DOS

There is none: a program loaded with `pc_load_com()` that calls `INT 20h` or `INT 21h` reaches whatever the host provides through `pc_set_int_hook()`, which is called for software interrupts whose vector has no handler. `pcrun` provides a tiny one (`INT 20h`, `INT 21h AH=02h, 09h, 4Ch`). A DOS booted from a diskette installs its own vectors, and from then on the hook is no longer called for them.

### Writing a card

Fill a `pc_card_t`: port handlers (return `PC_PORT_NOT_MINE` for ports that are not yours), `advance` for its clock, `take_irq` for its IRQ line (2–7), and `rom_ints` + `bios` for option ROM services. When the card is inserted the machine fills `dma` and `bus`: a card asserting DREQ calls `card->dma(card->bus, channel, buf, len, &moved)` and the board's 8237 moves the bytes. `src/pc_vga.c` and `src/pc_fdc.c` are the references.

## Building

```
make          # build/libpc86.a and build/libpc86.so (the whole machine), build/pcrun
make test     # machine tests
make demo     # examples/clock.asm on pcrun, screenshots in build/
```

The other components are expected next to this folder (`VM86_DIR`, `PIT86_DIR`, `DMA86_DIR`, `KBD86_DIR`, `VGA86_DIR`, `FDC86_DIR` override it). With CMake: `cmake -S . -B build`.

## Files

```
include/pc.h          the machine and the card interface
include/pc_vga.h      the VGA card
include/pc_fdc.h      the diskette adapter
src/pc.c              motherboard, BIOS, scheduling
src/pc_vga.c          the VGA card
src/pc_fdc.c          the diskette adapter
tools/pcrun.c         runs a .COM or boots diskettes on a full machine, types, saves screenshots
examples/clock.asm    timer-driven stopwatch
examples/piano.asm    notes on the PC speaker (timer channel 2, port 61h)
examples/music.asm    a tune from a note table, sequenced by the timer interrupt
tests/pc_test.c       machine tests
```
