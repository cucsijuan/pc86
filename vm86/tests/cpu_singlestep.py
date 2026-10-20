#!/usr/bin/env python3
"""
Validates the vm86 core against the public SingleStepTests/8088 (v2) suite,
generated from a real 8088: https://github.com/SingleStepTests/8088

Usage:
    python3 tests/cpu_singlestep.py [--lib build/libvm86.so] [--limit 2000] [--cache tests/sst]
                                    [--only 00,01,F6.6]

Downloads each opcode file the first time (about 2-3 MB each) and keeps it
in --cache. Only tests the opcodes vm86 supports.
"""
import argparse, ctypes, gzip, json, os, sys, urllib.request

BASE = "https://raw.githubusercontent.com/SingleStepTests/8088/main/v2/"

# Supported opcodes (non-group) and groups with their supported /reg values.
PLAIN = (
    [f"{o:02X}" for o in range(0x00, 0x40) if (o & 7) < 6 and o not in (0x26, 0x2E, 0x36, 0x3E)]
    + ["06", "07", "0E", "16", "17", "1E", "1F"]
    + [f"{o:02X}" for o in range(0x40, 0x60)]
    + [f"{o:02X}" for o in range(0x70, 0x80)]
    + ["84", "85", "86", "87", "88", "89", "8A", "8B", "8C", "8D", "8E"]
    + [f"{o:02X}" for o in range(0x90, 0x9B)]
    + ["9C", "9D", "9E", "9F"]
    + [f"{o:02X}" for o in range(0xA0, 0xC0)]
    + ["C2", "C3", "C4", "C5", "CA", "CB", "CC", "CD", "CF"]
    + ["E0", "E1", "E2", "E3", "E4", "E5", "E6", "E7", "E8", "E9", "EA", "EB", "EC", "ED", "EE", "EF"]
    + ["F5", "F8", "F9", "FA", "FB", "FC", "FD"]
)
GROUPS = {
    "80": range(8), "81": range(8), "83": range(8),
    "8F": [None], "C6": [None], "C7": [None],
    "D0": [0, 1, 2, 3, 4, 5, 7], "D1": [0, 1, 2, 3, 4, 5, 7],
    "D2": [0, 1, 2, 3, 4, 5, 7], "D3": [0, 1, 2, 3, 4, 5, 7],
    "F6": [0, 2, 3, 4, 5, 6, 7], "F7": [0, 2, 3, 4, 5, 6, 7],
    "FE": [0, 1], "FF": [0, 1, 2, 3, 4, 5, 6],
}
STRING_OPS = {0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF}
PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3}


class Regs(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint16) for n in
                ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
                 "cs", "ds", "es", "ss", "ip", "flags")]


def load_lib(path):
    lib = ctypes.CDLL(path)
    lib.vm86_create.restype = ctypes.c_void_p
    lib.vm86_reset.argtypes = [ctypes.c_void_p]
    lib.vm86_memory.restype = ctypes.POINTER(ctypes.c_uint8)
    lib.vm86_memory.argtypes = [ctypes.c_void_p]
    lib.vm86_regs.restype = ctypes.POINTER(Regs)
    lib.vm86_regs.argtypes = [ctypes.c_void_p]
    lib.vm86_run.restype = ctypes.c_uint32
    lib.vm86_run.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.vm86_status.restype = ctypes.c_int
    lib.vm86_status.argtypes = [ctypes.c_void_p]
    return lib


def fetch(cache, name):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, name + ".json.gz")
    if not os.path.exists(path):
        urllib.request.urlretrieve(BASE + name + ".json.gz", path)
    with gzip.open(path) as f:
        return json.load(f)


def modrm(bytes_):
    i = 0
    while bytes_[i] in PREFIXES:
        i += 1
    return bytes_[i + 1]


def modrm_reg(bytes_):
    return (modrm(bytes_) >> 3) & 7


def reg_operand(bytes_):
    return modrm(bytes_) >> 6 == 3


# Cases vm86 deliberately excludes within supported opcodes.
SKIP = {
    "8C": lambda b: modrm_reg(b) > 3,                       # segment register aliases
    "8E": lambda b: modrm_reg(b) > 3 or modrm_reg(b) == 1,  # aliases and MOV CS
    "8F": lambda b: modrm_reg(b) != 0,                      # only /0 is POP
    "C6": lambda b: modrm_reg(b) != 0,                      # only /0 is MOV
    "C7": lambda b: modrm_reg(b) != 0,
    "C4": reg_operand,                                      # LES/LDS/far CALL/JMP with a register:
    "C5": reg_operand,                                      # undefined on the 8086
    "FF.3": reg_operand,
    "FF.5": reg_operand,
}


def is_rep_string(bytes_):
    i, rep = 0, False
    while i < len(bytes_) and bytes_[i] in PREFIXES:
        rep = rep or bytes_[i] in (0xF2, 0xF3)
        i += 1
    return rep and i < len(bytes_) and bytes_[i] in STRING_OPS


def run_file(lib, vm, tests, mask, limit, skip=None):
    mem = lib.vm86_memory(vm)
    regs = lib.vm86_regs(vm).contents
    fails = []
    for t in tests[:limit]:
        if skip and skip(t["bytes"]):
            continue
        lib.vm86_reset(vm)
        for k, v in t["initial"]["regs"].items():
            setattr(regs, k, v)
        for a, v in t["initial"]["ram"]:
            mem[a] = v
        start = (regs.cs, regs.ip)
        rep = is_rep_string(t["bytes"])
        for _ in range(70000):
            lib.vm86_run(vm, 1)
            if lib.vm86_status(vm) != 0:
                break
            if not (rep and (regs.cs, regs.ip) == start):
                break
        if lib.vm86_status(vm) not in (0, 1):
            fails.append((t["name"], "status %d" % lib.vm86_status(vm)))
            continue
        exp = dict(t["initial"]["regs"]); exp.update(t["final"]["regs"])
        bad = []
        for k, v in exp.items():
            got = getattr(regs, k)
            if k == "flags":
                if (got & mask) != (v & mask):
                    bad.append(f"flags {got:04X}!={v:04X} (mask {mask:04X})")
            elif got != v:
                bad.append(f"{k} {got:04X}!={v:04X}")
        # If an exception was taken, the undefined FLAGS bits are also masked
        # in the pushed flags word (SS:SP+4).
        fl = set()
        if exp["sp"] == (t["initial"]["regs"]["sp"] - 6) & 0xFFFF and rep is False:
            base = (exp["ss"] << 4) + ((exp["sp"] + 4) & 0xFFFF)
            fl = {base & 0xFFFFF: mask & 0xFF, (base + 1) & 0xFFFFF: mask >> 8}
        for a, v in t["final"]["ram"]:
            if a in fl:
                if (mem[a] & fl[a]) != (v & fl[a]):
                    bad.append(f"ram[{a:05X}] {mem[a]:02X}!={v:02X} (pushed flags)")
                continue
            if mem[a] != v:
                bad.append(f"ram[{a:05X}] {mem[a]:02X}!={v:02X}")
        if bad:
            fails.append((t["name"], ", ".join(bad[:4])))
    return fails


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", default=os.path.join(here, "..", "build", "libvm86.so"))
    ap.add_argument("--limit", type=int, default=2000)
    ap.add_argument("--cache", default=os.path.join(here, "sst"))
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    lib = load_lib(args.lib)
    vm = lib.vm86_create()
    meta = fetch_meta(args.cache)

    names = PLAIN + [op if r is None else f"{op}.{r}" for op, regs in GROUPS.items() for r in regs]
    if args.only:
        wanted = set(args.only.upper().split(","))
        names = [n for n in names if n in wanted]

    total_fail = 0
    for name in names:
        op, _, sub = name.partition(".")
        m = meta["opcodes"].get(op, {})
        if sub:
            m = m.get("reg", {}).get(sub, {})
        mask = m.get("flags-mask", 0xFFFF)
        try:
            tests = fetch(args.cache, name)
        except Exception as e:
            print(f"{name:6s} NO DATA ({e})")
            continue
        fails = run_file(lib, vm, tests, mask, args.limit, SKIP.get(name))
        n = min(len(tests), args.limit)
        status = "OK" if not fails else f"FAIL {len(fails)}/{n}"
        print(f"{name:6s} {status}")
        for f in fails[:3]:
            print("        ", f[0], "->", f[1])
        total_fail += bool(fails)
    print("\nOpcodes with failures:", total_fail, "of", len(names))
    sys.exit(1 if total_fail else 0)


def fetch_meta(cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, "metadata.json")
    if not os.path.exists(path):
        urllib.request.urlretrieve(BASE + "metadata.json", path)
    with open(path) as f:
        return json.load(f)


if __name__ == "__main__":
    main()
