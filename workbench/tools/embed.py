#!/usr/bin/env python3
"""Builds the single-file workbench page: shell.html with the WebAssembly
machine and the example programs embedded.

    embed.py shell.html workbench.wasm <suite root> index.html [--sw86 DIR]

With --sw86, the sources of COS (DIR/dos) go in too, and the page can make
a COS system diskette.
"""
import base64
import hashlib
import json
import os
import subprocess
import sys

# key, file (relative to the suite root), title, whether it reads the keyboard,
# whether it plays sound
EXAMPLES = [
    ("text_demo", "vga86/examples/text_demo.asm", "Text mode demo", False, False),
    ("typewriter", "kbd86/examples/typewriter.asm", "Typewriter (keyboard through the BIOS)", True, False),
    ("keys_game", "kbd86/examples/keys_game.asm", "Move a square (own keyboard handler)", True, False),
    ("clock", "pc86/examples/clock.asm", "Stopwatch (timer interrupt)", True, False),
    ("piano", "pc86/examples/piano.asm", "Piano (timer and speaker)", True, True),
    ("music", "pc86/examples/music.asm", "Music: Greensleeves (speaker)", True, True),
    ("bounce", "workbench/examples/bounce.asm", "Bouncing ball (mode 13h)", False, False),
    ("gfx_demo", "vga86/examples/gfx_demo.asm", "Palette animation (mode 13h)", False, False),
    ("boot", "fdc86/examples/boot.asm", "Boot sector: list the diskette's files", False, False),
    ("hello", "vm86/examples/hello.asm", "Hello", False, False),
    ("numbers", "vm86/examples/numbers.asm", "Numbers 1 to 20", False, False),
    ("factorial", "vm86/examples/factorial.asm", "Recursive factorial", False, False),
    ("sort", "vm86/examples/sort.asm", "Bubble sort", False, False),
    ("strings", "vm86/examples/strings.asm", "String instructions", False, False),
    ("interrupts", "vm86/examples/interrupts.asm", "Interrupt handlers", False, False),
]


def old_versions(root, path, current):
    """SHA-256 of every committed version of an example but the current one."""
    try:
        log = subprocess.run(["git", "-C", root, "log", "--format=%H", "--", path],
                             capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return set()
    out = set()
    for commit in log:
        r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (commit, path)], capture_output=True)
        if r.returncode == 0:
            text = r.stdout.decode("utf-8").replace("\r\n", "\n")
            if text != current:
                out.add(hashlib.sha256(text.encode("utf-8")).hexdigest())
    return out


# COS (from sw86): the parts of a system diskette
COS = [("boot", "dos/boot.asm"), ("kernel", "dos/cos.asm"), ("command", "dos/command.asm"),
       ("asm", "dos/asm.asm"), ("edit", "dos/edit.asm"), ("hello", "programs/hello/hello.asm")]


def main():
    shell, wasm, root, out = sys.argv[1:5]
    sw86 = sys.argv[sys.argv.index("--sw86") + 1] if "--sw86" in sys.argv[5:] else None
    cos = None
    if sw86:
        cos = {}
        for key, path in COS:
            with open(os.path.join(sw86, path), encoding="utf-8") as f:
                cos[key] = f.read().replace("\r\n", "\n")
    with open(shell, encoding="utf-8") as f:
        page = f.read()
    with open(wasm, "rb") as f:
        b64 = base64.b64encode(f.read()).decode("ascii")
    examples = {}
    old = set()
    for key, path, title, keys, sound in EXAMPLES:
        with open(os.path.join(root, path), encoding="utf-8") as f:
            source = f.read()
        examples[key] = {"title": title, "source": source, "keys": keys, "sound": sound}
        old |= old_versions(root, path, source)
    # "</" cannot appear inside a <script> element
    js = json.dumps(examples, ensure_ascii=False).replace("</", "<\\/")
    for marker in ("@WASM@", "@EXAMPLES@", "@OLD@", "@COS@"):
        if page.count(marker) != 1:
            sys.exit("embed.py: %s must appear once in %s" % (marker, shell))
    page = page.replace("@WASM@", b64).replace("@EXAMPLES@", js)
    page = page.replace("@COS@", json.dumps(cos, ensure_ascii=False).replace("</", "<\\/"))
    page = page.replace("@OLD@", json.dumps(sorted(old)))
    with open(out, "w", encoding="utf-8") as f:
        f.write(page)


if __name__ == "__main__":
    main()
