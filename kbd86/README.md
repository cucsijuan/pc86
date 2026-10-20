# kbd86 — keyboard, keyboard controller and interrupt controller for an in-game virtual PC

Three small C99 devices with no dependencies:

- **`kbd`** (`kbd.h`): the keyboard itself, the peripheral at the end of the cable: key matrix, internal buffer, typematic repeat, LEDs and the keyboard commands. Several models (84-key AT, 101/102-key enhanced, 104-key) with different keys.
- **`kbc`** (`kbc.h`): the 8042 keyboard controller on the motherboard (ports `60h`/`64h`) plus the keyboard BIOS (the default `INT 09h` handler and `INT 16h`). The keyboard plugs into it and can be unplugged at any time.
- **`pic`** (`pic.h`): the 8259 interrupt controller, which delivers the keyboard interrupt (IRQ 1) and the timer (IRQ 0).

They don't depend on any CPU core: the controller reads and writes the machine's 1 MB memory (only the BIOS data area) and the host routes ports and interrupts to them. `tests/integration_test.c` is the reference wiring with vm86; [pc86](../pc86) wires them into a whole machine.

## Status

| Check | Result |
|---|---|
| Unit tests (scan codes, typematic, keyboard and controller commands, BIOS translation, INT 16h, LEDs, models, plugging and unplugging, PIC) | 89/89 |
| Integration with vm86 (BIOS blocking and polling reads, own INT 09h handler with HLT, LED commands) | 11/11 |
| Unit and integration tests under AddressSanitizer + UBSan | 0 errors |
| Robustness: 3 million random operations (ports, keys, plugging, power, BIOS calls, a corrupted BIOS data area) under AddressSanitizer + UBSan | 0 errors |
| Examples (`examples/*.asm`) | identical to NASM, run in the web workbench |

Compiles without warnings as C99 and as C++17.

## Keyboard and controller: two pieces, one wire

On a real PC the keyboard and the 8042 talk over a serial cable, one byte at a time in each direction. Here it is the same:

- The keyboard queues bytes (scan codes, `FAh` acknowledgements, `AAh` after its self test) and the controller pulls them with `kbd_wire_read()` when its output buffer is free.
- Bytes a program writes to port `60h` go to the keyboard through `kbd_wire_write()`.
- The BIOS talks to the keyboard the same way a real one does: Caps/Num/Scroll Lock send `EDh` + LED byte, `INT 16h AX=0305h` sends `F3h` + rate. The `FAh` replies come back through IRQ 1 and `INT 09h` ignores them.

`kbc_device_t` is the connector: a keyboard is plugged in with `kbc_keyboard_device()`, but anything that speaks bytes can be.

### Models

| Model | Keys | `F2h` identify |
|---|---|---|
| `KBD_MODEL_AT84` | 01h–53h, SysRq 54h; arrows only on the keypad | `FA` only |
| `KBD_MODEL_ENHANCED101` | adds gray keys (`E0`), right Ctrl/Alt, F11, F12 | `FA AB 83` |
| `KBD_MODEL_ENHANCED102` | adds the ISO key (56h) | `FA AB 83` |
| `KBD_MODEL_WINDOWS104` | adds the Windows and Menu keys | `FA AB 83` |

Keys a model does not have are ignored. Custom models are a `kbd_model_t` passed to `kbd_create()`.

### Plugging and unplugging

- `kbd_power(k, 1)` when the keyboard gets power (plugged into a running machine, or the machine is switched on): it resets and sends `AAh`. `kbd_power(k, 0)` when it loses it: keys go nowhere and its LEDs go dark.
- With nothing plugged in, a byte written to port `60h` times out: the controller answers `FEh` and sets status bit 6. The interface test (`64h` ← `ABh`) reports the clock line stuck low.
- Like on a real PC, a keyboard plugged in while running has its LEDs off until the BIOS next updates them, and a key held when it was unplugged stays "held" in the BIOS flags until it is pressed and released again.

## Both ways programs read the keyboard

**Through the BIOS** (`INT 16h`), typical in tools and text programs: a key goes from the keyboard → IRQ 1 → the BIOS `INT 09h` handler, which translates it to ASCII and stores it in the 16-key buffer of the BIOS data area → `INT 16h` returns it.

**Directly** (games): the program installs its own `INT 09h` handler, reads raw scan codes from port `60h`, and ends with `OUT 20h, 20h` (EOI to the PIC). `examples/keys_game.asm` does this.

Both work because the BIOS handlers are only used while the vector table still points at the BIOS addresses (`F000:E987` for `INT 09h`, `F000:E82E` for `INT 16h`, as on a real PC). A program that installs its own handler changes the vector and its code runs instead.

## Wiring (host side)

```c
kbd_t *kbd = kbd_create(kbd_builtin_model(KBD_MODEL_ENHANCED101));
kbc_t *kbc = kbc_create(vm86_memory(cpu));
kbc_device_t plug = kbc_keyboard_device(kbd);
pic_t pic;  pic_reset(&pic);

kbc_connect(kbc, &plug);       /* plug in (NULL unplugs) */
kbd_power(kbd, 1);             /* the machine is on */
kbc_reset(kbc);                /* POST: BIOS data area, LEDs, discard the AAh */
/* IVT: INT 09h -> F000:E987, INT 16h -> F000:E82E, with an IRET byte at each address */

/* ports: try kbc_port_in/out (60h, 64h), then pic_port_in/out (20h, 21h) */

/* INT 09h, while its vector points at the BIOS: */
kbc_bios_irq(kbc);  pic_eoi(&pic);

/* INT 16h, while its vector points at the BIOS: */
switch (kbc_int16(kbc, &regs)) {
case KBC_BIOS_DONE:        /* copy AX and ZF back */ break;
case KBC_BIOS_WAIT:        cpu_ip -= 2; cpu_flags |= IF; break;   /* run INT 16h again */
case KBC_BIOS_UNSUPPORTED: /* stop the machine with an error */ break;
}

/* between CPU slices (the same 40 us slices the monitor needs): */
kbd_advance(kbd, us);
if (kbc_take_irq(kbc)) pic_raise(&pic, 1);
if (cpu_flags & IF) { int v = pic_acknowledge(&pic); if (v >= 0) vm86_request_interrupt(cpu, v); }

/* from the engine's input: */
kbd_key(kbd, kbd_key_from_code("ArrowUp"), pressed);   /* W3C KeyboardEvent.code names */
kbd_release_all(kbd);                                   /* when the machine loses focus */
```

Two details that matter:

- **`KBC_BIOS_WAIT` sets IF.** The real BIOS waits for a key with interrupts enabled (`STI`). Without that, a program that calls `INT 16h` with interrupts disabled would wait forever.
- **Send physical keys, one press and one release per stroke.** The keyboard generates its own repeats (typematic), so ignore the host's auto-repeat events (`e.repeat` in a browser). Keys are physical positions, so the machine sees a **US layout**, as DOS did before loading `KEYB`.

## What is emulated

### Keyboard and controller (ports 60h, 64h)

- Scan code set 1, with `E0` prefixes for extended keys (arrows, gray navigation keys, right Ctrl/Alt, keypad Enter and /, Windows keys). Each byte raises IRQ 1.
- Typematic repeat generated by the device: 500 ms delay and 10.9 characters per second by default, configurable with command `F3h` or `INT 16h AX=0305h`.
- Keyboard commands: `EDh` LEDs, `EEh` echo, `F2h` identify (per model), `F3h` typematic, `F4h`/`F5h` enable/disable, `F6h` defaults, `FFh` reset (`FA AA`). Each is acknowledged with `FAh`, as on real hardware.
- Controller: status port (output buffer full, system flag, command/data, inhibit, time-out), commands `20h`/`60h` (command byte), `AAh` self-test (`55h`), `ABh` interface test, `ADh`/`AEh` (disable/enable the keyboard).

### Keyboard BIOS

- `INT 09h`: Shift, Ctrl, Alt (left and right), Caps/Num/Scroll Lock with their LEDs, Insert mode, the standard AT translation table (ASCII, function keys, Alt and Ctrl combinations, keypad with and without Num Lock, gray keys with `E0h`), and the 16-key buffer at 0040:001E. Num Lock is on at boot.
- `INT 16h`: `00h`/`10h` read, `01h`/`11h` check (ZF), `02h`/`12h` shift flags, `03h` typematic rate, `05h` store a key. The standard functions (`00h`, `01h`) convert or skip keys of the 101-key keyboard (F11, F12, gray-key variants) like the real BIOS, and the enhanced functions (`10h`, `11h`) return them.

### PIC (ports 20h, 21h)

Initialization (ICW1–ICW4, including the AT cascade setup), mask register, non-specific and specific EOI, reading IRR and ISR, fixed priority. After reset it is programmed like the PC BIOS leaves it: vectors `08h`–`0Fh`, IRQ 0 and 1 enabled.

### Not emulated

All of these are reported as unsupported rather than emulated differently:

- Keyboard: scan code sets 2 and 3 (`F0h`), and the controller's output port, A20 gate, CPU reset and mouse port.
- BIOS: Ctrl+Break (`INT 1Bh`), Ctrl+Alt+Del, Pause, Print Screen/SysRq, Alt+keypad character entry, and the other `INT 16h` functions.
- PIC: automatic EOI, rotating priority, special mask mode, poll mode and level triggering. There is no slave PIC (IRQs 8–15).

Port `61h` (system control: speaker gate and data, refresh toggle) is on the motherboard: [pc86](../pc86) emulates it with the timer. The web workbench stores its value so that classic XT-style handlers work.

## Files

```
include/kbd.h, src/kbd.c       keyboard (the peripheral) and its models
include/kbc.h, src/kbc.c       8042 controller and keyboard BIOS
include/pic.h, src/pic.c       8259 PIC
examples/typewriter.asm        BIOS input: echoes keys and shows their codes
examples/keys_game.asm         own INT 09h handler: moves a square with arrows or WASD
tests/kbd_test.c               unit tests
tests/integration_test.c       tests with vm86; also the reference wiring
```

## Building

```
make                                     # build/libkbd86.a, build/libkbd86.so
make test                                # unit tests
make test-integration VM86_DIR=../vm86   # tests with the CPU
```

With CMake: `cmake -S . -B build [-DVM86_DIR=../vm86]`.
