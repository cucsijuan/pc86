# workbench — the pc86 machine in a web page

`index.html` is a single self-contained page: the complete [pc86](../pc86) machine compiled to WebAssembly, an assembler editor, and the examples of every component. Open it in a browser; nothing is installed and nothing is sent anywhere.

What it shows:

- **Front panel**: power switch, reset, run/stop, CPU speed, speaker sound, power/run/error/drive lights and the keyboard's lock lights.
- **Back of the machine**: the VGA card in slot 1 and the diskette adapter in slot 2 (removable with the power off), the monitor on the card (monochrome VGA, color VGA, multisync; the CGA monitor's digital plug does not fit) with its own power switch, and the keyboard on the board (84, 101, 102 or 104 keys). Peripherals can be changed while a program runs.
- **Program**: NASM-syntax source assembled with `asm86` and loaded as a .COM program. A program at `org 7C00h` is a boot sector: Run writes it to the diskette and boots from it. Assembly errors, and errors the machine reports while running (an unsupported BIOS call, port or instruction), point at their source line.
- **Diskette**: one diskette, in drive A: or out of it: a new formatted 1.44 MB one, or a raw `.img` image opened from the computer. Copy the program onto it as a file, make it the boot sector, boot from A:, write-protect it, and save the image back. The page lists the diskette's files as the machine changes them. The diskette is not kept when the page is closed: save the image.

A tiny DOS is provided, as in `pcrun`: `INT 20h` and `INT 21h AH=02h, 09h, 4Ch`.

## Building

```
make WASI_SDK=/path/to/wasi-sdk     # build/workbench.wasm, then index.html
```

[wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) is the only requirement (plus Python 3 to embed the result into the page). `shell.html` is the page without the machine; `tools/embed.py` adds the WebAssembly binary and the example programs to make `index.html`, which is committed so the page works straight from a checkout.

## Files

```
shell.html            the page: layout, editor, screen, connections, speaker
src/web.c             the machine as the page sees it (web_* exports)
tools/embed.py        builds index.html
examples/bounce.asm   a ball bouncing in mode 13h, synchronized with the retrace
                      (the page also embeds the other components' examples, fdc86/examples/boot.asm among them)
index.html            the built page
```
