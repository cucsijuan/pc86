# pit86 — 8253/8254 timer for an in-game virtual PC

A C99 device with no dependencies: the programmable interval timer of the PC, the chip that gives programs a sense of time.

| Channel | On a PC | After `pit_reset()` |
|---|---|---|
| 0 | system timer, output is IRQ 0 | mode 3, count 65536: **18.2 Hz** |
| 1 | DRAM refresh; port 61h bit 4 toggles with it | mode 2, count 18: every 15 µs |
| 2 | PC speaker; gate = port 61h bit 0, output = port 61h bit 5 | mode 3, count 0533h (896 Hz), gate low |

Like the other devices it does not depend on any CPU core: the host routes ports `40h`–`43h` to it, advances its clock between CPU slices and raises IRQ 0 when `pit_take_irq()` says so. [pc86](../pc86) does all of that, plus port 61h.

## Status

| Check | Result |
|---|---|
| Unit tests (BIOS programming, modes 0/2/3/4, access modes, latch, read-back, gates, no drift, save state) | 26/26 |
| Machine tests in pc86 (BIOS tick count, INT 1Ch hook, own INT 08h at 1000 Hz, speaker, refresh bit) | pass |
| Unit tests under AddressSanitizer + UBSan; 3 million random operations | 0 errors |

Compiles without warnings as C99 and as C++17.

## Usage

```c
pit_t *pit = pit_create();                 /* already programmed like the BIOS leaves it */

/* port callbacks of the machine: */
int r = pit_port_in(pit, port, &value);    /* or pit_port_out; PIT_PORT_NOT_MINE / HANDLED / UNSUPPORTED */

/* between CPU slices: */
pit_advance(pit, us);
if (pit_take_irq(pit)) pic_raise(&pic, 0);

/* port 61h (on the motherboard): */
pit_set_gate(pit, 2, value & 1);
bit5 = pit_output(pit, 2);
bit4 = pit_edges(pit, 1) & 1;

/* the speaker, for the engine's audio: */
double hz = speaker_enabled ? pit_square_wave_hz(pit, 2) : 0.0;
```

Time is exact over any run: the clock converts microseconds to 1.193182 MHz pulses from the total elapsed time, so irregular slices do not drift. Counters are advanced arithmetically, so a long slice costs the same as a short one.

## What is emulated

- Ports `40h`–`42h` (counters) and `43h` (control word).
- Modes 0 (interrupt on terminal count), 2 (rate generator), 3 (square wave) and 4 (software strobe); 6 and 7 as aliases of 2 and 3.
- LSB only, MSB only, and LSB-then-MSB access, with separate read and write flip-flops.
- Counter latch command, and the 8254 read-back command (count and status, several channels at once).
- Gate input per channel: low pauses modes 0 and 4; in modes 2 and 3 it forces the output high, and a rising edge restarts the period.
- A new count written while a channel runs in mode 2 or 3 takes effect at the end of the current period.

### Not emulated

Reported as `PIT_PORT_UNSUPPORTED`:

- Modes 1 and 5 (started by a gate edge).
- BCD counting.

Output limitations programs cannot detect: in mode 3 a new count takes effect at the end of the whole period rather than the half period, and the one-clock delay between writing a count and the counter loading it is not modeled.

## Building

```
make          # build/libpit86.a, build/libpit86.so
make test     # unit tests
```

With CMake: `cmake -S . -B build`, then `cmake --build build`.

## Files

```
include/pit.h       API
src/pit.c           device
tests/pit_test.c    unit tests
```
