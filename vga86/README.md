# vga86 — monochrome VGA display device for an in-game virtual PC

A C99 device with no dependencies (apart from libm) that emulates what a program sees of a VGA card connected to a **monochrome VGA monitor**, and produces the image that monitor would show.

It is a separate device: it does not depend on any CPU core. It reads video memory from the machine's 1 MB address space and the host routes I/O ports and `INT 10h` to it. `examples/vm86_glue.c` shows the wiring for [vm86](../vm86).

## Output

```
640 x 400 pixels, 1 byte per pixel, luminance 0-255
```

The size is the same in every mode, so the engine needs a single `G8` texture:

- **Text mode 03h:** 80x25 cells of 8x16 pixels = 640x400.
- **Mode 13h:** 320x200, each pixel doubled to 2x2, like the real VGA double scan.

Colors are converted to gray the way a monochrome VGA monitor shows them: 30% R + 59% G + 11% B of the DAC value. The engine tints the result (green phosphor) and adds screen effects. An optional, engine-independent phosphor persistence filter is included (`vga_phosphor_*`).

## Status

| Check | Result |
|---|---|
| Unit tests (modes, BIOS, ports, palette, blink, cursor, retrace timing, save state, phosphor, font) | 64/64 |
| Integration with vm86 (real programs; retrace polling at 100k–4.77M instructions/s) | 9/9 |
| Robustness: 3 million random port/BIOS calls under AddressSanitizer + UBSan | 0 errors |
| Demos (`examples/*.asm`), assembled with asm86 | identical to NASM, screenshots checked |

Compiles without warnings as C99 and as C++17.

## Usage

```c
#include "vga.h"

vga_t *vga = vga_create(vm86_memory(cpu));   /* the machine's 1 MB */

/* port callbacks of the machine: */
int r = vga_port_in(vga, port, &value);      /* or vga_port_out */
/* VGA_PORT_NOT_MINE    -> route to other devices (keyboard, timer...)
   VGA_PORT_HANDLED     -> done
   VGA_PORT_UNSUPPORTED -> a VGA port this device does not emulate: stop with an error */

/* INT 10h: */
vga_bios_regs_t regs = { ax, bx, cx, dx, es, bp };
if (!vga_int10(vga, &regs)) { /* unsupported BIOS function: stop with an error */ }
/* copy ax, bx, cx, dx back to the CPU */

/* every engine frame: */
run_cpu_in_40us_slices_calling_vga_advance();
vga_render(vga, pixels);                     /* 640*400 bytes -> texture */
```

### Timing: why the CPU must run in short slices

Programs synchronize with the screen by polling bit 3 of port `3DAh` (vertical retrace), which is set for only **63 µs** of each 14.27 ms frame. The device clock only moves when the host calls `vga_advance()`, so the CPU must run in slices of **at most ~60 µs of emulated time**, with `vga_advance()` after each one. 40 µs is recommended: about 420 calls per frame at any CPU speed, a negligible cost.

Measured with a program that waits for 70 retraces (should take exactly 70 frames), at 400,000 instructions/s:

| Slice | Frames taken |
|---|---|
| 16 steps (40 µs) | 70 ✓ |
| 24 steps (60 µs) | 70 ✓ |
| 32 steps (80 µs) | 95 ✗ |
| 256 steps (640 µs) | 749 ✗ |
| one engine frame | never finishes ✗ |

`examples/vm86_glue.c` computes the slice from the CPU speed (`ips / 25000`).

## What is emulated

### Modes

| Mode | Description |
|---|---|
| `03h` (and `02h`) | text 80x25, 16 colors as grays, 8x16 CP437 font, B800:0000 |
| `13h` | 320x200, 256 colors as grays, linear framebuffer at A000:0000 |

Any other mode set returns "not supported" and leaves the mode unchanged.

### Text mode

- Character + attribute per cell. Foreground in bits 0–3, background in bits 4–6, bit 7 blinks (32-frame cycle) or, after `INT 10h AX=1003h BL=0`, gives a bright background.
- Hardware cursor from CRTC registers `0Ah`/`0Bh` (shape, bit 5 = off) and `0Eh`/`0Fh` (position), blinking on a 16-frame cycle.
- CRTC start address (`0Ch`/`0Dh`) for hardware scrolling and pages.
- Colors go through the attribute controller palette (default EGA mapping) into DAC entries 0–63 (the 64 EGA colors), exactly like the real card.

### Mode 13h

- One byte per pixel, through the DAC. Mode set loads the standard VGA default palette (16 EGA colors, 16 grays, 216 hue/saturation/intensity entries, 8 black).

### Ports

| Port | Function |
|---|---|
| `3C7h` | DAC read index (write), DAC state (read) |
| `3C8h` | DAC write index |
| `3C9h` | DAC data, R, G, B with auto-increment |
| `3CCh` | miscellaneous output (read): color I/O at 3Dxh |
| `3D4h` / `3D5h` | CRTC index / data (registers 00h–18h stored; 0Ah, 0Bh, 0Ch, 0Dh, 0Eh, 0Fh used) |
| `3DAh` | input status: bit 3 vertical retrace (lines 412–413), bit 0 display disabled (horizontal or vertical blanking) |

Other ports in 3B0h–3DFh (sequencer, graphics controller, attribute controller...) return `VGA_PORT_UNSUPPORTED`. Programs that use unchained "mode X" therefore get a clear error instead of a wrong picture.

### Video BIOS (`INT 10h`)

| AH | Function |
|---|---|
| `00h` | set mode (03h, 02h, 13h; bit 7 = do not clear) |
| `01h` | cursor shape |
| `02h` / `03h` | set / get cursor position (page 0) |
| `05h` | select page (page 0 only) |
| `06h` / `07h` | scroll window up / down |
| `08h` | read character and attribute (text mode) |
| `09h` / `0Ah` | write character (and attribute) CX times |
| `0Eh` | teletype output (BEL, BS, LF, CR, wrap, scroll) |
| `0Fh` | get mode |
| `10h` | palette: AL = 00h, 01h, 02h, 03h, 07h, 08h, 09h, 10h, 12h, 15h, 17h, 1Bh (gray summing) |
| `12h` | BL=10h: EGA/VGA information |
| `13h` | write string (AL = 0–3) |
| `1Ah` | AL=00h: display combination, reports **07h = VGA with analog monochrome display** |

In mode 13h, BIOS text uses the 8x8 font (40x25 cells) in color BL. The BIOS data area at 0040:0000 is kept up to date (mode, columns, cursor position and shape, rows, character height), as programs read it directly.

### Not emulated

- Display pages other than 0 in the BIOS (the CRTC start address does work).
- Planar 16-color modes, mode X, the sequencer, graphics and attribute controller ports.
- Changing the font (`INT 10h AH=11h`).
- The 9th pixel column of real VGA text mode (characters are 8 pixels wide, so the output is 640 rather than 720 pixels).
- The overscan border, which has no area in the output. Its value is stored and can be read back.
- Changes made in the middle of a frame (for example, palette changes mid-screen). `vga_render` draws the whole screen at once.

All of these are reported as unsupported when a program uses them, except the last three, which are output limitations that programs cannot detect.

## Character set and font

Code page 437. The font is generated from **unscii** by viznut ([github.com/viznut/unscii](https://github.com/viznut/unscii)), released into the public domain / CC0. Only `unscii-16.hex` and `unscii-8.hex` are used; the `-full` variants include GPL Unifont glyphs and must not be used.

Four CP437 characters are missing from unscii and are handled by `tools/gen_font.py`: `0Fh` (sun) and `7Fh` (house) are drawn by hand, `A9h` is the mirror of `¬`, and `F9h` uses the middle dot. To regenerate the font:

```
git clone https://github.com/viznut/unscii
make font UNSCII_DIR=unscii/fontfiles
```

## Building

```
make                                     # build/libvga86.a, build/libvga86.so
make test                                # unit tests, no other package needed
make test-integration VM86_DIR=../vm86   # tests with the CPU
make demo VM86_DIR=../vm86               # runs the demos, writes build/*.pgm screenshots
```

With CMake: `cmake -S . -B build [-DVM86_DIR=../vm86]`, then `cmake --build build`.

For an engine, compile `src/vga.c` and `src/font_cp437.c`. Both also compile as C++.

## Unreal (summary)

1. Add `include/` and `src/` to the module. On Linux and macOS, libm is already linked.
2. Create a transient `UTexture2D` of 640x400 in `PF_G8` with nearest filtering.
3. In the monitor actor's `Tick`: run the CPU in 40 µs slices with `vga_advance`, call `vga_render` (optionally `vga_phosphor_apply` with the frame's `DeltaTime`), and upload the buffer with `UpdateTextureRegions`.
4. The phosphor material multiplies the luminance by the phosphor color and adds scanlines, curvature and bloom.

## Files

```
include/vga.h            API
src/vga.c                device
src/font_cp437.c/.h      generated fonts (8x16, 8x8)
tools/gen_font.py        font generator
examples/vm86_glue.c     wiring with vm86 + "vgarun" screenshot tool
examples/text_demo.asm   text mode demo (CP437, attributes, blink, cursor)
examples/gfx_demo.asm    mode 13h demo (DAC ports, retrace-synced palette animation)
tests/vga_test.c         unit tests
tests/integration_test.c tests with vm86
```
