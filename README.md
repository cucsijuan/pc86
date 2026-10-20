# pc86 — a virtual 8086 PC built from independent parts

A suite of small, dependency-free C99 components that together simulate an
early PC. Each part is its own project (own Makefile, CMake, tests) and does not
depend on the others; `pc86` wires them into a machine. The long-term goal is to
embed the whole machine in a game made with Godot (GDExtension), linking
`libpc86`.

| Component | What it is |
|---|---|
| [`vm86/`](vm86) | 8086 CPU core + NASM-compatible assembler (`asm86`), plus `run86` |
| [`pit86/`](pit86) | 8253/8254 programmable interval timer (system tick, speaker) |
| [`kbd86/`](kbd86) | Keyboard (peripheral, several models), 8042 controller + keyboard BIOS, 8259 PIC |
| [`vga86/`](vga86) | VGA card (text mode 03h, mode 13h, INT 10h) and monitors (mono, color, CGA, multisync) |
| [`pc86/`](pc86) | The motherboard: puts everything together, with ISA slots and hot-pluggable peripherals |
| [`workbench/`](workbench) | The whole machine in a web page (WebAssembly): editor, examples, power, cards and peripherals |

## How a machine is put together

Like a real PC:

- **Motherboard** (fixed): CPU, RAM, PIC, PIT, keyboard controller, port 61h, BIOS ROM.
- **Expansion slots**: cards, changed with the power off (a VGA card today).
- **Peripherals**: keyboard and monitor, plugged and unplugged at any time; different models have different capabilities.

Every device is its own project and does not depend on the others or on the CPU; `pc86` is the only one that knows them all. New devices (serial port and mouse, storage, sound...) go in their own top-level folder following the same layout: `include/`, `src/`, `tests/`, `examples/`, `Makefile`, `CMakeLists.txt`, `README.md`.

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
`make -C workbench WASI_SDK=/path/to/wasi-sdk`. GitHub Actions runs `make test` and
rebuilds it on every push (`.github/workflows/build.yml`).
