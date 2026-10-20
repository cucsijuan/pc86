# pc86 — a virtual 8086 PC built from independent parts

A suite of small, dependency-free C99 components that together simulate an
early PC. Each part is its own project (own Makefile, CMake, tests) and does not
depend on the others; `pc86` wires them into a machine. The long-term goal is to
embed the whole machine in a game made with Godot (GDExtension), linking
`libpc86`.

**Try it in the browser: [cucsijuan.github.io/pc86](https://cucsijuan.github.io/pc86/)** — the whole machine, with an assembler editor and examples.

| Component | What it is |
|---|---|
| [`vm86/`](vm86) | 8086 CPU core + NASM-compatible assembler (`asm86`), plus `run86` |
| [`pit86/`](pit86) | 8253/8254 programmable interval timer (system tick, speaker) |
| [`dma86/`](dma86) | 8237A DMA controller and page registers |
| [`kbd86/`](kbd86) | Keyboard (peripheral, several models), 8042 controller + keyboard BIOS, 8259 PIC |
| [`vga86/`](vga86) | VGA card (text mode 03h, mode 13h, INT 10h) and monitors (mono, color, CGA, multisync) |
| [`fdc86/`](fdc86) | uPD765A floppy controller + diskette BIOS (INT 13h), 3.5" drives, 1.44 MB / 720 KB diskettes, FAT12, `fdimage` |
| [`pc86/`](pc86) | The motherboard: puts everything together, with ISA slots and hot-pluggable peripherals |
| [`workbench/`](workbench) | The whole machine in a web page (WebAssembly): editor, examples, power, cards, peripherals and diskettes, and a DOS86 system diskette to boot |

## How a machine is put together

Like a real PC:

- **Motherboard** (fixed): CPU, RAM, PIC, PIT, DMA controller, keyboard controller, port 61h, BIOS ROM.
- **Expansion slots**: cards, changed with the power off (a VGA card and a diskette adapter today).
- **Peripherals**: keyboard, monitor and floppy drives, plugged and unplugged at any time, and diskettes in and out of the drives; different models have different capabilities.

Every device is its own project and does not depend on the others or on the CPU; `pc86` is the only one that knows them all. New devices (serial port and mouse, hard disk, sound...) go in their own top-level folder following the same layout: `include/`, `src/`, `tests/`, `examples/`, `Makefile`, `CMakeLists.txt`, `README.md`.

## Build and test everything

```sh
make          # build all libraries and tools
make test     # unit tests + integration tests against vm86
make demo     # demos, screenshots in vga86/build and pc86/build
```

`vm86`'s assembler comparison test needs `nasm` installed (skipped otherwise);
`make -C vm86 test-cpu` validates the CPU against SingleStepTests (~700 MB download).

Each component can also be built on its own; see its README.

The workbench needs [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases):
`make -C workbench WASI_SDK=/path/to/wasi-sdk`. GitHub Actions runs `make test`, tests
DOS86 from [sw86](https://github.com/cucsijuan/sw86) on the machine, and rebuilds the workbench
with it on every push (`.github/workflows/build.yml`).
