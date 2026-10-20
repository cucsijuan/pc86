#!/usr/bin/env python3
"""
Checks that unsupported or non-8086 code produces a clear error on the right
line. Usage: python3 tests/asm_errors.py [--asm build/asm86]
"""
import argparse, os, subprocess, sys, tempfile

# (source, expected line, text that must appear in the message)
CASES = [
    ("org 100h\ndaa", 2, "not supported on this machine"),
    ("xlat", 1, "not supported"),
    ("lds si, [bx]", 1, "not supported"),
    ("retf", 1, "not supported"),
    ("pusha", 1, "later CPU"),
    ("movzx ax, bl", 1, "later CPU"),
    ("push 5", 1, "80186"),
    ("imul ax, bx", 1, "80186"),
    ("shl ax, 2", 1, "by 1 or by CL"),
    ("mov es, 5", 1, "immediate into a segment register"),
    ("mov cs, ax", 1, "CS"),
    ("pop cs", 1, "POP CS"),
    ("mov ds, es", 1, "between segment registers"),
    ("mov ax, [ax]", 1, "BX, BP, SI and DI"),
    ("mov ax, [bx+bp]", 1, "register combination"),
    ("mov ax, [si+di]", 1, "register combination"),
    ("mov ax, [bx-si]", 1, "subtracted"),
    ("mov [bx], 5", 1, "size not specified"),
    ("inc [si]", 1, "size not specified"),
    ("push [si]", 1, "size not specified"),
    ("mov ax, bl", 1, "sizes do not match"),
    ("mov [bx], [si]", 1, "two memory operands"),
    ("mov al, 300", 1, "does not fit in a byte"),
    ("mov ax, 70000", 1, "does not fit in a word"),
    ("in al, 300", 1, "port"),
    ("out 60h, bl", 1, "AL or AX"),
    ("rep add ax, bx", 1, "REP can only"),
    ("jmp far 0:0", 1, "FAR"),
    ("mov eax, 1", 1, "32-bit"),
    ("mov ax, word ptr [bx]", 1, "PTR"),
    ("dd 1", 1, "db/dw"),
    ("section .text", 1, "SECTION"),
    ("bits 32", 1, "16-bit"),
    ("mov ax, es:[bx]", 1, "inside the brackets"),
    ("mov ax, dword [bx]", 1, "BYTE and WORD"),
    ("mvo ax, bx", 1, "unknown instruction: mvo"),
    ("nop\njmp nowhere", 2, "undefined symbol: 'nowhere'"),
    ("a: nop\na: nop", 2, "already defined"),
    ("mov ax, 1/0", 1, "division by zero"),
    ("db 'abc", 1, "unterminated string"),
    ("mov ax, 12g", 1, "invalid number"),
    ("lea ax, bx", 1, "LEA"),
    ("int 300", 1, "0..255"),
    ("jz far_away\ntimes 200 nop\nfar_away:", 1, "out of range"),
    ("jz near x\nx:", 1, "NEAR"),
    ("loop x\ntimes 130 nop\nx:", 1, "out of range"),
    ("times -1 nop", 1, "negative"),
    ("nop\nnop\nnop\nmov ax, [cx]", 4, "BX, BP, SI and DI"),
    ("x equ\n", 1, "missing value"),
    ("equ 5", 1, "EQU needs a name"),
    ("org 100h\nnop\norg 200h", 3, "ORG"),
    ("mov ax, bx, cx", 1, "operand"),
    ("add", 1, "takes 2 operands"),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--asm", default=os.path.join(here, "..", "build", "asm86"))
    args = ap.parse_args()
    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, "e.asm")
    bad = 0
    for src, line, text in CASES:
        open(path, "w").write(src + "\n")
        p = subprocess.run([args.asm, path, "-o", path + ".com"], capture_output=True, text=True)
        want = f":{line}: error:"
        if p.returncode == 0 or want not in p.stderr or text not in p.stderr:
            bad += 1
            print(f"FAIL: {src!r}\n   expected line {line} containing '{text}'\n   got: {p.stderr.strip() or '(no error)'}")
    print(f"Error cases: {len(CASES) - bad}/{len(CASES)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
