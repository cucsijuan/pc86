#!/usr/bin/env python3
"""
Assembles the programs in examples/, runs them with run86 and checks their
output. If nasm is installed, also checks that the binary is identical.
"""
import os, shutil, subprocess, sys, tempfile

# name -> (expected stdout, expected exit code)
EXPECTED = {
    "hello": ("Hello from the 8086!\r\n", 0),
    "numbers": (" ".join(str(i) for i in range(1, 21)) + " \r\n", 0),
    "sort": ("-300 -7 -1 0 5 8 13 42 77 1000 \r\n", 0),
    "factorial": ("".join(f"{v}\r\n" for v in [1, 1, 2, 6, 24, 120, 720, 5040, 40320]), 0),
    "strings": ("The copy is identical\r\nThe 8086 has string instructions\r\n**********\r\n", 0),
    "interrupts": ("INT 60h handled by the program\r\n" * 3 +
                   "Dividing by zero...\r\nCaught the divide error exception (INT 0)\r\n", 1),
}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(here, "..")
    asm = os.path.join(root, "build", "asm86")
    run = os.path.join(root, "build", "run86")
    tmp = tempfile.mkdtemp()
    have_nasm = shutil.which("nasm") is not None
    bad = 0
    for name, (out_expected, code_expected) in EXPECTED.items():
        src = os.path.join(root, "examples", name + ".asm")
        com = os.path.join(tmp, name + ".com")
        p = subprocess.run([asm, src, "-o", com], capture_output=True, text=True)
        if p.returncode:
            print(f"FAIL {name}: does not assemble\n{p.stderr}")
            bad += 1
            continue
        if have_nasm:
            subprocess.run(["nasm", "-f", "bin", src, "-o", com + ".nasm"], check=True)
            if open(com, "rb").read() != open(com + ".nasm", "rb").read():
                print(f"FAIL {name}: binary differs from NASM")
                bad += 1
        r = subprocess.run([run, com], capture_output=True)
        out = r.stdout.decode("latin-1")
        if out != out_expected or r.returncode != code_expected:
            print(f"FAIL {name}: output {out!r} (exit code {r.returncode})")
            bad += 1
        else:
            print(f"ok   {name}")
    print(f"Programs: {len(EXPECTED) - bad}/{len(EXPECTED)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
