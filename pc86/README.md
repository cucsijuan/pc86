# pc86 — the motherboard of an in-game virtual PC

Puts the independent components together into a machine with expansion slots and connectors, the way a real PC is put together:

```
                    ┌──────────────────── motherboard (fixed) ────────────────────┐
                    │  8086 CPU (vm86)   640 KB RAM   system BIOS ROM             │
 keyboard (kbd86) ──┤  8042 keyboard controller (kbd86)   8259 PIC   8253 PIT     │
                    │  port 61h (speaker, refresh)                                │
                    │  ISA slots: [VGA card]──── monitor (vga86)  [ ] [ ] [ ] ...  │
                    └─────────────────────────────────────────────────────────────┘
```

- **On the board** (not interchangeable): CPU, RAM, PIC, PIT, keyboard controller, port 61h, BIOS.
- **Expansion slots**: cards (`pc_card_t`) are inserted and removed with the power off. A VGA card is included (`pc_vga.h`); future cards (sound, disk controller, serial...) implement the same interface.
- **Peripherals**: the keyboard plugs into the board, the monitor into the video card. Both can be plugged and unplugged while the machine runs, and different models behave differently (see [kbd86](../kbd86) and [vga86](../vga86)).

## Status

| Check | Result |
|---|---|
| Machine tests (POST, boot message, BIOS timer, INT 1Ch and own INT 08h, BIOS services, monitor sense and hot-swap, keyboard hot-plug, port 61h, open bus, error reporting, save state, machine without video) | 35/35 |
| Same tests under AddressSanitizer + UBSan | 0 errors |
| `examples/clock.asm` with `pcrun` | screenshots checked |

Compiles without warnings as C99 and as C++17.

## Usage

```c
#include "pc.h"
#include "pc_vga.h"

pc_t *pc = pc_create();
pc_vga_card_t *vga = pc_vga_card_create();
monitor_t *mon = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_MONO));
kbd_t *kbd = kbd_create(kbd_builtin_model(KBD_MODEL_ENHANCED101));

pc_insert_card(pc, 0, pc_vga_card_slot(vga));   /* power off only */
pc_vga_card_connect_monitor(vga, mon);           /* any time; -1 if the connector does not fit */
pc_connect_keyboard(pc, kbd);                    /* any time; NULL unplugs */
pc_power_on(pc);                                 /* POST; "No boot device available." */
pc_load_com(pc, program, size);                  /* until there is storage */

/* every engine frame: */
pc_run(pc, delta_seconds);
pc_vga_card_display(vga, delta_seconds, picture); /* 640x400 x monitor_channels(mon) */
double tone = pc_speaker_hz(pc);                  /* 0 = silent */

/* input: */
kbd_key(kbd, kbd_key_from_code("KeyA"), pressed);
```

`tools/pcrun.c` is a complete example that also saves screenshots:

```
make demo                       # assembles examples/clock.asm and runs it for 3 s
build/pcrun program.com [--seconds S] [--every N] [--out prefix] [--ips N] [--color]
```

### Time

`pc_run()` takes emulated time, not instructions. The CPU runs in 40 µs slices (`PC_SLICE_US`); after each slice every clock advances (PIT, keyboard, cards) and pending interrupts are delivered through the PIC. 40 µs is below the 63 µs VGA retrace pulse, so programs that poll port `3DAh` never miss it. The speed is set in instructions per second (`pc_set_speed`, default 400,000, about a 4.77 MHz 8088); it changes how much a program gets done, not how fast time passes for it.

### BIOS

| Vector | Service |
|---|---|
| `08h` | timer tick: real 8086 code at `F000:FEA5` — counts ticks at `0040:006C`, sets the midnight flag, calls `INT 1Ch`, sends EOI |
| `09h` | keyboard interrupt (kbd86's BIOS) |
| `0Ah`–`0Fh` | unexpected hardware interrupt: EOI and return |
| `10h` | the video card's ROM (none without a card) |
| `11h` / `12h` | equipment list / memory size (640 KB) |
| `16h` | keyboard services (kbd86's BIOS) |
| `19h` | bootstrap: no storage yet, so it prints "No boot device available." and the CPU idles in `HLT` with interrupts on |
| `1Ah` | `AH=00h`/`01h` read/set the tick count; the rest (real-time clock) is reported as unsupported |
| others below `20h` | `IRET` at `F000:FF53` |

Services are provided in C while the vector still points at the BIOS address; a program that installs its own handler gets the real behavior, as on a real PC.

### Ports

| Port | Device |
|---|---|
| `20h`–`21h` | PIC |
| `40h`–`43h` | PIT |
| `60h`, `64h` | keyboard controller |
| `61h` | bit 0 speaker gate (PIT channel 2), bit 1 speaker data, bit 4 refresh toggle, bit 5 PIT channel 2 output |
| others | the cards in the slots; nobody: reads `FFh`, writes ignored (empty bus) |

A port a device decodes but does not emulate stops the machine with `PC_ERROR` and a message in `pc_error()`, like an unsupported instruction or BIOS call.

### DOS

There is none: a program loaded with `pc_load_com()` that calls `INT 20h` or `INT 21h` reaches whatever the host provides through `pc_set_int_hook()`, which is called for software interrupts whose vector has no handler. `pcrun` provides a tiny one (`INT 20h`, `INT 21h AH=02h, 09h, 4Ch`).

### Writing a card

Fill a `pc_card_t`: port handlers (return `PC_PORT_NOT_MINE` for ports that are not yours), `advance` for its clock, `take_irq` for its IRQ line (2–7), and `rom_ints` + `bios` for option ROM services. `src/pc_vga.c` is the reference.

## Building

```
make          # build/libpc86.a and build/libpc86.so (the whole machine), build/pcrun
make test     # machine tests
make demo     # examples/clock.asm on pcrun, screenshots in build/
```

The other components are expected next to this folder (`VM86_DIR`, `PIT86_DIR`, `KBD86_DIR`, `VGA86_DIR` override it). With CMake: `cmake -S . -B build`.

## Files

```
include/pc.h          the machine and the card interface
include/pc_vga.h      the VGA card
src/pc.c              motherboard, BIOS, scheduling
src/pc_vga.c          the VGA card
tools/pcrun.c         runs a .COM on a full machine, saves screenshots
examples/clock.asm    timer-driven stopwatch
tests/pc_test.c       machine tests
```
