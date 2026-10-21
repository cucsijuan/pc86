#!/usr/bin/env python3
"""
Generates random programs using every supported form and checks that asm86
produces exactly the same bytes as NASM ("nasm -f bin", cpu 8086), and that
it rejects exactly the programs NASM rejects.

Usage: python3 tests/asm_vs_nasm.py [--programs 300] [--seed 1] [--asm build/asm86]
Requires nasm on the PATH (the test is skipped otherwise).
"""
import argparse, os, random, shutil, subprocess, sys, tempfile

R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
SREG = ["es", "cs", "ss", "ds"]
ALU = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"]
SHIFTS = ["rol", "ror", "rcl", "rcr", "shl", "sal", "shr", "sar"]
GRP3 = ["not", "neg", "mul", "imul", "div", "idiv"]
JCC = ["jo", "jno", "jb", "jc", "jnae", "jae", "jnb", "jnc", "je", "jz", "jne", "jnz",
       "jbe", "jna", "ja", "jnbe", "js", "jns", "jp", "jpe", "jnp", "jpo", "jl", "jnge",
       "jge", "jnl", "jle", "jng", "jg", "jnle"]
LOOPS = ["loop", "loope", "loopz", "loopne", "loopnz", "jcxz"]
SIMPLE = ["movsb", "movsw", "cmpsb", "cmpsw", "stosb", "stosw", "lodsb", "lodsw",
          "scasb", "scasw", "nop", "hlt", "cbw", "cwd", "pushf", "popf", "sahf", "lahf",
          "clc", "stc", "cmc", "cli", "sti", "cld", "std", "iret", "ret", "int3"]
STRINGS = ["movsb", "movsw", "cmpsb", "cmpsw", "stosb", "stosw", "lodsb", "lodsw", "scasb", "scasw"]
DISPS = ["0", "1", "-1", "5", "127", "128", "-128", "-129", "300", "-300", "0x7FFF", "0FFFFh",
         "KA", "KB", "KA*2", "(KB-KA)", "dat", "dat+2", "data_end-dat", "FWDK", "'A'"]
BASES = ["bx", "bp", "si", "di", "bx+si", "bx+di", "bp+si", "bp+di", "si+bx", "di+bp"]


class Gen:
    def __init__(self, rnd):
        self.r = rnd

    def c(self, xs):
        return self.r.choice(xs)

    def mem(self, size=None):
        r = self.r
        kind = r.random()
        seg = (self.c(SREG) + ":") if r.random() < 0.15 else ""
        if kind < 0.15:
            inner = seg + self.c(["dat", "dat+4", "0x10", "1234h", "KA", "data_end"])
        else:
            base = self.c(BASES)
            if r.random() < 0.6:
                d = self.c(DISPS)
                sign = "-" if d[0] != "-" and r.random() < 0.2 and d[0].isdigit() else "+"
                if d.startswith("-"):
                    sign = ""
                inner = seg + (f"{base}{sign}{d}" if r.random() < 0.7 else f"{d}+{base}")
            else:
                inner = seg + base
        pre = {None: "", 1: "byte ", 2: "word "}[size]
        return f"{pre}[{inner}]"

    def imm8(self):
        return self.c(["0", "1", "5", "-1", "127", "-128", "255", "0FFh", "'A'", "KA", "FWDK",
                       "3*4", "1<<3", "~0 & 0xF", "-KA", "(KA+1)"])

    def imm16(self):
        return self.c(["0", "1", "-1", "127", "128", "-128", "-129", "255", "256", "1000",
                       "0x7FFF", "0FFFFh", "65535", "-32768", "0FF80h", "KA", "KB", "FWDK",
                       "dat", "data_end", "data_end-dat", "$", "$-$$", "'AB'", "KB/3", "KB % 7"])

    def rm8(self):
        return self.c(R8) if self.r.random() < 0.5 else self.mem(1)

    def rm16(self):
        return self.c(R16) if self.r.random() < 0.5 else self.mem(2)

    def instr(self, local_labels, global_labels):
        r = self.r
        k = r.randrange(27)
        if k == 0:
            op = self.c(ALU)
            f = r.randrange(6)
            if f == 0: return f"{op} {self.rm8()}, {self.c(R8)}"
            if f == 1: return f"{op} {self.rm16()}, {self.c(R16)}"
            if f == 2: return f"{op} {self.c(R8)}, {self.mem()}"
            if f == 3: return f"{op} {self.c(R16)}, {self.mem()}"
            if f == 4: return f"{op} {self.rm8() if r.random()<.5 else 'al'}, {self.imm8()}"
            return f"{op} {self.rm16() if r.random()<.5 else 'ax'}, {self.imm16()}"
        if k == 1:
            f = r.randrange(9)
            if f == 0: return f"mov {self.rm8()}, {self.c(R8)}"
            if f == 1: return f"mov {self.rm16()}, {self.c(R16)}"
            if f == 2: return f"mov {self.c(R8)}, {self.mem()}"
            if f == 3: return f"mov {self.c(R16)}, {self.mem()}"
            if f == 4: return f"mov {self.c(R8)}, {self.imm8()}"
            if f == 5: return f"mov {self.c(R16)}, {self.imm16()}"
            if f == 6: return f"mov {self.mem(1)}, {self.imm8()}"
            if f == 7: return f"mov {self.mem(2)}, {self.imm16()}"
            return self.c([f"mov al, {self.mem()}", f"mov ax, {self.mem()}",
                           f"mov {self.mem()}, al", f"mov {self.mem()}, ax"])
        if k == 2:
            s = self.c(["es", "ss", "ds"])
            return self.c([f"mov {s}, {self.c(R16)}", f"mov {s}, {self.mem()}",
                           f"mov {self.c(R16)}, {self.c(SREG)}", f"mov {self.mem()}, {self.c(SREG)}"])
        if k == 3:
            return self.c([f"xchg {self.c(R16)}, {self.c(R16)}", f"xchg {self.c(R8)}, {self.c(R8)}",
                           f"xchg {self.c(R16)}, {self.mem()}", f"xchg {self.mem()}, {self.c(R16)}",
                           f"xchg {self.c(R8)}, {self.mem()}", f"xchg {self.mem()}, {self.c(R8)}"])
        if k == 4:
            return self.c([f"test {self.rm8()}, {self.c(R8)}", f"test {self.rm16()}, {self.c(R16)}",
                           f"test {self.c(R8)}, {self.mem()}", f"test {self.c(R16)}, {self.mem()}",
                           f"test {self.rm8()}, {self.imm8()}", f"test {self.rm16()}, {self.imm16()}",
                           f"test al, {self.imm8()}", f"test ax, {self.imm16()}"])
        if k == 5:
            return f"lea {self.c(R16)}, {self.mem()}"
        if k == 6:
            op = self.c(["inc", "dec"])
            return self.c([f"{op} {self.c(R16)}", f"{op} {self.c(R8)}", f"{op} {self.mem(1)}", f"{op} {self.mem(2)}"])
        if k == 7:
            op = self.c(GRP3)
            return self.c([f"{op} {self.rm8()}", f"{op} {self.rm16()}"])
        if k == 8:
            op = self.c(SHIFTS)
            cnt = self.c(["1", "cl"])
            return self.c([f"{op} {self.rm8()}, {cnt}", f"{op} {self.rm16()}, {cnt}"])
        if k == 9:
            op = self.c(["push", "pop"])
            regs = SREG if op == "push" else ["es", "ss", "ds"]
            return self.c([f"{op} {self.c(R16)}", f"{op} {self.c(regs)}", f"{op} {self.mem(2)}",
                           f"{op} {self.mem()}"])
        if k == 10:
            if local_labels:
                return f"{self.c(JCC)} {self.c(local_labels)}"
            return "nop"
        if k == 11:
            if local_labels:
                return f"{self.c(LOOPS)} {self.c(local_labels)}"
            return "nop"
        if k == 12:
            t = self.c(global_labels + local_labels)
            return self.c([f"jmp {t}", f"call {t}", f"jmp near {t}", f"jmp {self.c(R16)}",
                           f"call {self.c(R16)}", f"jmp {self.mem()}", f"call {self.mem(2)}",
                           f"jmp word {self.mem()[0:0]}[bx]"])
        if k == 13:
            if local_labels:
                return f"jmp short {self.c(local_labels)}"
            return "nop"
        if k == 14:
            return self.c(SIMPLE)
        if k == 15:
            return f"{self.c(['rep', 'repe', 'repz', 'repne', 'repnz'])} {self.c(STRINGS)}"
        if k == 16:
            return self.c([f"int {self.c(['0', '3', '10h', '21h', '0FFh', 'KA'])}", f"ret {self.imm16()}",
                           f"retn"])
        if k == 17:
            return self.c(["in al, 60h", "in ax, 40h", "in al, dx", "in ax, dx",
                           "out 61h, al", "out 40h, ax", "out dx, al", "out dx, ax", f"in al, {self.c(['0', 'KA', '0FFh'])}"])
        if k == 18:
            return self.c([f"db {self.imm8()}, {self.imm8()}", "db 'hola', 13, 10, '$'",
                           f'db "x", {self.imm8()}', f"dw {self.imm16()}, {self.imm16()}",
                           "dw 'AB', 'C'", "db 0", "dw dat, data_end"])
        if k == 19:
            return self.c(["times 3 nop", "times 2 db 1, 2", "times KA db 0", "times 2 inc ax",
                           f"times 2 dw {self.imm16()}"])
        if k == 20:
            return self.c(["cbw", "cwd", "lahf", "sahf"])
        if k == 21:
            op = self.c(ALU)
            return f"{op} word {self.mem()[0:0]}[{self.c(BASES)}], {self.c(['5', '-1', '200', 'KB', 'byte 5', 'word 5'])}"
        if k == 22:
            return self.c(["resb 3", "resw 2"])
        if k == 23:
            return f"mov {self.c(R16)}, {self.c(['dat', 'data_end', '$', 'café_block0', 'data_end-dat', '$$'])}"
        if k == 24:
            return f"mov {self.mem()}, word {self.imm16()}"
        if k == 25:
            seg = self.c(["0", "0F000h", "0FFFFh", "1234h", "KA", "FWDK", "KB*2"])
            t = self.c(global_labels + local_labels + ["7C00h", "0", "dat"])
            if r.random() < 0.05:
                return f"jmp far {seg}:{t}"  # rejected by both: already far
            return self.c([f"jmp {seg}:{t}", f"call {seg}:{t}", f"jmp {seg}:{self.imm16()}",
                           f"call far {self.mem()}", f"jmp far {self.mem()}",
                           f"call far word [{self.c(BASES)}]", "retf", f"retf {self.imm16()}"])
        if k == 26:
            return f"{self.c(['lds', 'les'])} {self.c(R16)}, {self.mem(2 if r.random() < 0.05 else None)}"
        return f"{self.c(['add', 'sub', 'cmp'])} {self.c(R16)}, {self.c(['KA', 'KB', 'FWDK', 'dat', 'byte 3'])}"

    def sections(self, b):
        """Some data in .data and .bss, then back to .text and code using it."""
        r, out, used = self.r, [], []
        if r.random() < 0.7:
            out += ["section .data", f"dvar{b}:"]
            out += ["    " + self.c([f"db {self.imm8()}", "db 'abc', 0", f"dw {self.imm16()}", "dw $ - $$",
                                     f"dw dvar{b}", f"times {self.c(['1', '3', '7'])} db 9", "db 1, 2, 3, 4, 5"])
                    for _ in range(r.randrange(1, 4))]
            used.append(f"dvar{b}")
        if r.random() < 0.6:
            out += ["section .bss", f"bvar{b}: {self.c(['resb', 'resw'])} {self.c(['1', '2', '3', '100', 'KA'])}"]
            used.append(f"bvar{b}")
        out.append(self.c(["section .text", "segment .text"]))
        for v in used:
            out.append("    " + self.c([f"mov ax, {v}", f"mov [{v}], al", f"mov bx, [{v}+1]", f"inc word [{v}]",
                                        f"mov si, {v} - $$", f"dw {v}"]))
        return out

    def program(self):
        r = self.r
        lines = ["cpu 8086", "org 100h" if r.random() < 0.8 else f"org {self.c(['0', '7C00h', '200h'])}",
                 "KA equ 10", "KB equ 300"]
        nblocks = r.randrange(2, 8)
        gl = [f"café_block{i}" for i in range(nblocks)]  # non-ASCII labels on purpose
        for b in range(nblocks):
            lines.append(f"café_block{b}:")
            nlocal = r.randrange(1, 3)
            locs = [f".naïve{j}" if j % 2 else f".l{j}" for j in range(nlocal)]
            body = [self.instr(locs, gl) for _ in range(r.randrange(2, 7))]
            for j, l in enumerate(locs):
                body.insert(r.randrange(len(body) + 1), f"{l}:")
            lines += ["    " + x for x in body]
            if r.random() < 0.2:
                lines.append(f"    times {self.c(['100', '200', '130'])} nop")
            if r.random() < 0.3:
                lines += self.sections(b)
        lines += ["dat: dw 1, 2, 3", "    db 'texto$'", "data_end:", "FWDK equ 7"]
        return "\n".join(lines) + "\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--asm", default=os.path.join(here, "..", "build", "asm86"))
    args = ap.parse_args()

    if not shutil.which("nasm"):
        print("nasm is not installed: skipping the comparison")
        return 0
    rnd = random.Random(args.seed)
    gen = Gen(rnd)
    tmp = tempfile.mkdtemp()
    ok = skipped = 0
    lines_total = 0
    fails = []
    for i in range(args.programs):
        src = gen.program()
        path = os.path.join(tmp, "p.asm")
        open(path, "w", encoding="utf-8").write(src)
        n = subprocess.run(["nasm", "-w-all", "-f", "bin", path, "-o", path + ".nasm"], capture_output=True, text=True)
        m = subprocess.run([args.asm, path, "-o", path + ".mine"], capture_output=True, text=True)
        lines_total += src.count("\n")
        if n.returncode != 0:
            # NASM rejects it: asm86 must reject it too
            if m.returncode == 0:
                fails.append((src, "NASM fails but asm86 accepted it:\n" + n.stderr))
            else:
                skipped += 1
            continue
        if m.returncode != 0:
            fails.append((src, "asm86 failed:\n" + m.stderr))
            continue
        a = open(path + ".nasm", "rb").read()
        b = open(path + ".mine", "rb").read()
        if a != b:
            off = next((j for j in range(min(len(a), len(b))) if a[j] != b[j]), min(len(a), len(b)))
            fails.append((src, f"bytes differ at offset {off:#x}: nasm={a[off:off+6].hex()} asm86={b[off:off+6].hex()} (sizes {len(a)}/{len(b)})"))
            continue
        ok += 1
    print(f"Programs identical to NASM: {ok}, different: {len(fails)}, rejected by both: {skipped}, lines: {lines_total}")
    for src, msg in fails[:3]:
        print("-" * 60)
        print(msg)
        print(src)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
