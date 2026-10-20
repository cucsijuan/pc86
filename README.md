# pc86 — a virtual 8086 PC built from independent parts

A suite of small, dependency-free C99 components that together simulate an
early PC. Each part is its own project (own Makefile, CMake, tests) and does not
depend on the others; the host wires them together. The long-term goal is to
embed the whole machine in a game made with Godot (GDExtension).

| Component | What it is |
|---|---|
| [`vm86/`](vm86) | 8086 CPU core + NASM-compatible assembler (`asm86`), plus `run86` |
| [`vga86/`](vga86) | Monochrome VGA display device (text mode 03h, mode 13h, INT 10h) |
| [`kbd86/`](kbd86) | AT keyboard + 8042 controller + keyboard BIOS (INT 09h/16h) and 8259 PIC |
| [`workbench/`](workbench) | Browser workbench (`index.html`) to play with the machine |

New devices (timer/PIT, disk, sound...) go in their own top-level folder
following the same layout: `include/`, `src/`, `tests/`, `examples/`,
`Makefile`, `CMakeLists.txt`, `README.md`.

## Build and test everything

```sh
make          # build all libraries and tools
make test     # unit tests + integration tests against vm86
make demo     # vga86 demos, screenshots in vga86/build/*.pgm
```

`vm86`'s assembler comparison test needs `nasm` installed (skipped otherwise);
`make -C vm86 test-cpu` validates the CPU against SingleStepTests (~700 MB download).

Each component can also be built on its own; see its README.
