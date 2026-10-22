#!/usr/bin/env python3
"""Group a dragonfruit --profile dump by Palm OS ROM function.

Palm OS ROMs keep MacsBug symbol names right after each function's final
RTS/RTE/JMP (A0): a length byte with bit 7 set, then the name. A profiled PC
belongs to the first named function that ends after it.

usage: palm_profile.py ROM PROFILE [--top N] [--by ops|cycles] [--pcs FUNC]
"""
import argparse, bisect, collections

ROM_BASE = 0x10C08000

def rom_symbols(rom):
    syms = []   # (end offset, name)
    for i in range(2, len(rom) - 2, 2):
        w = (rom[i - 2] << 8) | rom[i - 1]
        if w not in (0x4E75, 0x4E73, 0x4ED0):
            continue
        b = rom[i]
        if b == 0x80:
            n, start = rom[i + 1], i + 2
        elif 0x81 <= b <= 0x9F:
            n, start = b & 0x1F, i + 1
        else:
            continue
        name = rom[start:start + n]
        if n < 2 or not all(48 <= c < 123 and (chr(c).isalnum() or c == 95) for c in name):
            continue
        # After the name: pad to even, then a word giving the length of the
        # function's literal pool, then the pool. The next function follows.
        nxt = (start + n + 1) & ~1
        if nxt + 2 <= len(rom):
            nxt += 2 + ((rom[nxt] << 8) | rom[nxt + 1])
        syms.append((i, name.decode(), nxt))
    return syms

def func_ranges(syms, name):
    r = [((syms[k - 1][2] if k else 0), s[0]) for k, s in enumerate(syms) if s[1] == name]
    if not r:
        raise SystemExit(f"no function {name}")
    return r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom"); ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--by", choices=("ops", "cycles"), default="cycles")
    ap.add_argument("--pcs", help="list the hot PCs inside this function")
    ap.add_argument("--disasm", help="disassemble this function with per-PC counts")
    a = ap.parse_args()
    rom = open(a.rom, "rb").read()
    syms = rom_symbols(rom)
    if a.disasm:
        import subprocess
        counts = {}
        for line in open(a.profile):
            addr, o, c = line.split()
            counts[int(addr, 16)] = (int(o), int(c))
        # Some names occur more than once (stubs); take the busiest.
        lo, hi = max(func_ranges(syms, a.disasm),
                     key=lambda r: sum(v[0] for k, v in counts.items()
                                       if ROM_BASE + r[0] <= k < ROM_BASE + r[1]))
        out = subprocess.run(["m68k-linux-gnu-objdump", "-D", "-b", "binary", "-m", "m68k:68000",
                              f"--adjust-vma={ROM_BASE:#x}", f"--start-address={ROM_BASE + lo:#x}",
                              f"--stop-address={ROM_BASE + hi:#x}", a.rom],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split(":", 1)
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            o, c = counts.get(addr, (0, 0))
            print(f"{o:9d} {line.strip()}")
        return
    ends = [s[0] for s in syms]
    def func(addr):
        if addr < ROM_BASE:
            return "(RAM)"
        k = bisect.bisect_right(ends, addr - ROM_BASE)
        return syms[k][1] if k < len(syms) else "?"
    ops = collections.Counter(); cyc = collections.Counter(); pcs = collections.Counter()
    tot_ops = tot_cyc = 0
    for line in open(a.profile):
        addr, o, c = line.split()
        addr, o, c = int(addr, 16), int(o), int(c)
        f = func(addr)
        ops[f] += o; cyc[f] += c; tot_ops += o; tot_cyc += c
        if a.pcs and f == a.pcs:
            pcs[addr] = c if a.by == "cycles" else o
    if a.pcs:
        for addr, v in sorted(pcs.items()):
            print(f"{addr:08X} {v}")
        return
    key = cyc if a.by == "cycles" else ops
    print(f"total {tot_ops} instructions, {tot_cyc} cycles")
    print(f"{'cycles%':>8} {'ops%':>6} {'ops':>11}  function")
    for f, _ in key.most_common(a.top):
        print(f"{100*cyc[f]/tot_cyc:8.2f} {100*ops[f]/tot_ops:6.2f} {ops[f]:11d}  {f}")

if __name__ == "__main__":
    main()
