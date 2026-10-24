# vm86 — 8086 core and NASM-style assembler for an in-game virtual PC

Two dependency-free C99 components, designed to be embedded in any engine:

- **`vm86`**: an 8086 CPU that behaves like the real one in everything it implements.
- **`asm86`**: an assembler for a subset of NASM that produces **the same bytes as `nasm -f bin`**.

Both also compile as C++, so they can be dropped into an Unreal module as-is.

## Status

| Check | Result |
|---|---|
| CPU vs [SingleStepTests/8088](https://github.com/SingleStepTests/8088) (tests captured from a physical 8088) | 271 opcodes and groups, ~1.4 million cases, 0 failures |
| Assembler vs NASM 2.16 (random programs) | 6,000+ programs (~275,000 lines), byte-for-byte identical output; also agrees on what to reject |
| Error messages | 60 cases checked (text and line number) |
| API tests (REP across frames, save state, IRQs, HLT, ports, far transfers...) | 28/28 |
| Example programs (assemble + run + compare output) | 6/6 |
| Robustness under AddressSanitizer + UBSan (6,000 broken or mutated sources) | 0 memory errors or undefined behavior |

## Layout

```
include/vm86.h       CPU API
include/asm86.h      assembler API
src/cpu.c            8086 CPU
src/asm.c            assembler
tools/asm86_cli.c    command-line assembler (cross-development)
tools/run86.c        runs a .COM file with a minimal test mini-DOS (testing only)
examples/*.asm       example programs
tests/               tests (see below)
Makefile, CMakeLists.txt
```

## Building

Linux / macOS / MinGW:

```
make            # build/libvm86.a, build/libvm86.so, build/asm86, build/run86
make test       # quick tests (NASM optional, used for the comparison)
make test-cpu   # CPU vs the real 8088 (downloads ~700 MB of tests the first time)
```

Windows with Visual Studio, or any platform with CMake:

```
cmake -S . -B build
cmake --build build --config Release
```

To embed in an engine, just compile `src/cpu.c` and `src/asm.c` with your code.

Trying it from the terminal:

```
build/asm86 examples/hello.asm -o hello.com -l hello.lst
build/run86 hello.com
```

## Using the CPU

```c
#include "vm86.h"

vm86_t *vm = vm86_create();

vm86_host_t host = {0};
host.user      = my_machine;      /* your state: video, keyboard, disk... */
host.in8       = my_in;           /* IN  al, port */
host.out8      = my_out;          /* OUT port, al */
host.interrupt = my_services;     /* INT n: return 1 if you handled it in C */
vm86_set_host(vm, &host);

vm86_load_com(vm, program, size, 0x1000);   /* like DOS: CS=DS=ES=SS, IP=100h */

/* in the engine's Tick: */
vm86_run(vm, 8000);               /* instructions for this frame */
```

Key points:

- **The engine sets the pace.** `vm86_run(vm, n)` executes at most `n` steps and returns. There are no threads and no internal clock, so you can pause, speed up (a "turbo button") or run several machines at once. A 4.77 MHz 8088 ran roughly 0.3–0.5 million instructions per second, about 5,000–8,000 per frame at 60 Hz.
- **`REP` never freezes a frame.** Each iteration of a string instruction counts as one step; when the budget runs out it continues on the next call, just like real hardware servicing an interrupt in the middle of a `REP`.
- **Interrupts.** `host.interrupt` is called on every `INT n` (and on the divide error, `INT 0`). Return 1 if you handled it in C, which is useful for the machine's BIOS/DOS services. Return 0 and the CPU does the real thing: it jumps through the vector table at `0000:0000`, so players can install their own handlers.
- **Hardware interrupts.** `vm86_request_interrupt(vm, 8)` queues an IRQ (timer, keyboard...). It is serviced between instructions when IF=1 and wakes the CPU from `HLT`.
- **Status.** `vm86_status()` returns `RUNNING`, `HALTED` (executed `HLT`), `STOPPED` (you called `vm86_stop()`, e.g. from `INT 20h`) or `ERR_UNSUPPORTED`. In the last case `vm86_error()` gives the address and opcode, and the CPU is left just before that instruction, with no side effects.
- **Save games.** `vm86_save_state()` / `vm86_load_state()` copy the CPU and memory (a little over 1 MB). The block is the raw internal structure: valid for the same build on the same platform.
- **Direct access.** `vm86_memory()` (1 MB) and `vm86_regs()` let you read and write memory and registers, e.g. to copy the framebuffer or build a debugger.

## Using the assembler

```c
#include "asm86.h"

asm86_result_t r;
if (asm86_assemble(player_source, &r) == 0) {
    vm86_load_com(vm, r.code, r.size, 0x1000);
} else {
    for (int i = 0; i < r.error_count; i++)
        show_error(r.errors[i].line, r.errors[i].message);   /* UTF-8 */
}
asm86_free(&r);   /* always, even when there were errors */
```

The result also includes:

- `symbols`: every label and constant with its value, for the debugger.
- `lines`: the line → address and size mapping, to highlight the line being executed.

Errors are reported **all at once, sorted by line**, with hints on how to fix them, for example:

```
3: the 8086 cannot load an immediate into a segment register; go through a register (mov ax, value / mov es, ax)
4: the 8086 can only shift by 1 or by CL (e.g. mov cl, 4 / shl ax, cl)
7: conditional jump out of range: target is 300 bytes away and the limit is -128..127. To go further, write NEAR (je near label)
8: instruction not supported on this machine: daa
```

## What is supported

### CPU

The whole 8086 except the following, which stop the CPU with `VM86_ERR_UNSUPPORTED` instead of behaving differently:

- BCD: `DAA DAS AAA AAS AAM AAD`
- `XLAT`, `LOCK`, `WAIT`, `ESC` (FPU), `INTO`
- `POP CS`, `MOV CS, ...`
- `LEA`, `LDS`, `LES` and far `CALL`/`JMP` through memory (`FF /3`, `FF /5`) with a register operand, which have no defined behavior on the 8086
- Aliases and undocumented opcodes (60–6F, 82, C0, C1, C8, C9, D6, F1, F6/F7 /1, D0–D3 /6...)

Far jumps, calls and returns (`JMP`/`CALL` to `segment:offset` or through a far pointer in memory, `RETF`, `RETF n`) and `LDS`/`LES` are included.

It does include the real 8086 quirks verified against hardware: `PUSH SP` pushes the already decremented value, FLAGS bits 12–15 always read as 1, `IDIV` does not allow a quotient of -128/-32768, and `REP` before `IDIV` negates the quotient.

Cycle timing, the prefetch queue and the single-step trap (TF) are not emulated.

### Assembler

- Labels with or without `:`, local labels (`.loop`, attached to the previous normal label) and `;` comments. As in NASM, labels may contain accented letters.
- Numbers `123`, `0x1F`, `1Fh`, `$1F`, `0b101`, `101b`, `17q`, `0o17`, and characters `'A'`, `'AB'`.
- Expressions `+ - * / // % %% << >> & | ^ ~ ! ( )`, with `$` (start of the line) and `$$` (start of the section).
- Directives `org`, `db`, `dw`, `resb`, `resw`, `times`, `equ`, `bits 16`, `cpu 8086`.
- Sections as in NASM's flat binary output: `section .text`, `.data` and `.bss` (or `segment`), in any order and as many times as wanted. The output has `.text`, then `.data` from the next multiple of 4 (with zeros in between), and `.bss` after it takes addresses but no room in the file: only `resb`/`resw` go there.
- Operands: registers, immediates, `[bx+si+disp]`, `[es:di]`, `byte`/`word`, `short`/`near` on jumps.
- Far jumps and calls, as in NASM: `jmp 0000h:7C00h`, `call 1234h:label`, `call far [bx]`, `jmp far [si]`; also `retf`, `retf 4`, `lds si, [bx]`, `les di, [label]`.
- Prefixes `rep`, `repe`, `repz`, `repne`, `repnz`.
- Every instruction the CPU supports, always choosing the same encoding as NASM: short jumps when they fit, sign-extended 8-bit immediates when they fit, 16-bit displacements when a label is involved, and so on.
- `je near label` and the other conditional jumps with `near`, which NASM refuses for the 8086 (its conditional jumps are only short): the short jump when the target is within -128..127, otherwise the opposite condition over a near `jmp` (`jne $+5` / `jmp near label`, 5 bytes). Compilers use it for jumps whose distance they do not know.

Not included: macros and the preprocessor (`%define`, `%macro`, `%include`), other sections and section attributes (`align=`, `vstart=`...), `dd`/`dq`, backquote strings and `strict`. All of these give a clear error. Object files (`-f obj`) and a linker are not included: sw86's C compiler puts programs of several files together as assembler source, and writes the header of an .EXE itself.

Like NASM, it requires a size when it cannot be inferred from a register (`inc word [si]`, `push word [bx]`, `mov byte [di], 5`).

## Character set

The machine uses **code page 437**, the original IBM PC character set: one byte per character, including accented letters, `ñ` (`A4h`) and box-drawing characters. This will be implemented in step 3, together with text mode and the font.

The assembler treats source text as bytes, like NASM: string contents (`db "..."`) are copied unchanged into the program. Files written by the in-machine editor will be CP437 natively. For sources written outside the machine, save them as CP437 (or convert them) so that non-ASCII characters in strings display correctly.

Assembler error messages are always returned as valid UTF-8, even if the source contains bytes that are not. In Unreal, convert them with `UTF8_TO_TCHAR`.

## Tests

| File | What it checks | Requirements |
|---|---|---|
| `tests/api_test.c` | CPU and assembler API | none |
| `tests/asm_vs_nasm.py` | fuzzing: same output as NASM, same rejections | Python 3, `nasm` (skipped if missing) |
| `tests/asm_errors.py` | error messages and line numbers | Python 3 |
| `tests/programs.py` | assembles and runs `examples/` and checks the output | Python 3 |
| `tests/cpu_singlestep.py` | CPU vs the real 8088; downloads the tests to `tests/sst/` | Python 3, internet access |

`asm_vs_nasm.py` accepts `--programs` and `--seed`, and `cpu_singlestep.py` accepts `--limit` and `--only`, for longer or targeted runs.

## Unreal integration (summary)

1. Copy `include/` and `src/` into the module (or into `ThirdParty/vm86`) and add the `include` path in the `.Build.cs`. The `.c` files compile as C or as C++.
2. In the monitor actor, create the VM in `BeginPlay` and call `vm86_run` in `Tick`.
3. Video, keyboard and sound are the next step: implement them with `in8`/`out8`/`interrupt` and by reading video memory through `vm86_memory()`.

## Next step

Step 3 is the machine itself: 80x25 text mode with a CP437 font and mode 13h with luminance output, keyboard (`INT 16h` and port `60h`), vertical retrace at 70 Hz on `3DAh`, PC speaker, the `INT 21h` subset and the virtual disk.
