#!/usr/bin/env python3
"""Make the SD card image for the emulated Cybiko MP3 cartridge.

    cybiko_card.py FOLDER mp3card.img [SIZE_MB]

On the board, a folder works too and needs no image: files in /Cybiko/card
are on the card (see docs/cybiko-mp3-cartridge.md). Copies FOLDER (subfolders too) into a new disk image with one FAT16
partition, long file names included, the way an SD card from a PC looks to
the cartridge. SIZE_MB defaults to the content plus 16 MB, at least 16 and at
most 1024 (CyOS cannot read the 32 KB clusters a bigger FAT16 disk needs).
Long names are folded to ASCII (accents dropped, Cyrillic transliterated),
as CyOS does not list files with other characters in their names. Put the image in /Cybiko on the board's SD card; CyOS sees the
card as "Removable Disk" and the MP3 player lists the .mp3 files on it.

To change the files later, mount the image on a PC (it is an ordinary disk
image) or make a new one. Python 3, no extra packages.
"""
import os
import struct
import sys
import time
import unicodedata

SECTOR = 512
PART_START = 63                 # first partition sector, as on most cards
ROOT_ENTRIES = 512
BAD_CHARS = set('"*+,./:;<=>?[\\]|')


class Node:
    def __init__(self, name, path, is_dir):
        self.name, self.path, self.is_dir = name, path, is_dir
        self.children = []
        self.size = 0 if is_dir else os.path.getsize(path)
        self.mtime = os.path.getmtime(path)
        self.cluster = 0
        self.short = None


def scan(path, name=""):
    node = Node(name, path, True)
    for entry in sorted(os.listdir(path), key=str.lower):
        if entry.startswith("."):
            continue
        p = os.path.join(path, entry)
        if os.path.isdir(p):
            node.children.append(scan(p, entry))
        elif os.path.isfile(p):
            node.children.append(Node(entry, p, False))
    return node


# ---- names ------------------------------------------------------------------

def short_part(s, n):
    out = ""
    for ch in s.upper():
        if ch == " " or ch == ".":
            continue
        out += "_" if (ch in BAD_CHARS or ord(ch) < 0x20 or ord(ch) > 0x7e) else ch
    return out[:n]


def is_plain_83(name):
    base, dot, ext = name.partition(".")
    return (name == name.upper() and 1 <= len(base) <= 8 and len(ext) <= 3 and "." not in ext
            and short_part(base, 8) == base and short_part(ext, 3) == ext)


def make_short_names(children):
    used = set()
    for c in children:
        base, _, ext = c.name.partition(".")
        plain = base.ljust(8) + ext.ljust(3)
        if is_plain_83(c.name) and plain not in used:
            c.short, c.lfn = plain.encode("ascii"), False
        else:
            base, _, ext = c.name.rpartition(".") if "." in c.name[1:] else (c.name, "", "")
            b, e = short_part(base, 8) or "_", short_part(ext, 3)
            for i in range(1, 100000):
                tail = "~%d" % i
                cand = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
                if cand not in used:
                    break
            c.short, c.lfn = cand.encode("ascii"), True
        used.add(c.short.decode("ascii"))


CYRILLIC = ["a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
            "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "sch", "", "y", "", "e", "yu", "ya"]


def ascii_name(name):
    """The long name as CyOS can show it (the same folding as cy_vfat.c)."""
    out = ""
    for ch in name:
        c = ord(ch)
        if c < 0x80:
            out += ch
        elif 0x410 <= c <= 0x42f:
            out += CYRILLIC[c - 0x410].capitalize()
        elif 0x430 <= c <= 0x44f:
            out += CYRILLIC[c - 0x430]
        elif c in (0x401, 0x451):
            out += "E" if c == 0x401 else "e"
        else:
            base = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode()
            out += base or {"\u00c6": "AE", "\u00e6": "ae", "\u00df": "ss", "\u00d8": "O",
                            "\u00f8": "o", "\u00de": "Th", "\u00fe": "th", "\u00d0": "D",
                            "\u00f0": "d", "\u00d7": "x"}.get(ch, "_")
    return out


def lfn_checksum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xff
    return s


def lfn_entries(name, short):
    units = list(ascii_name(name).encode("utf-16-le"))
    chars = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
    chars.append(0)
    while len(chars) % 13:
        chars.append(0xffff)
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    if len(parts) > 1 and all(c == 0xffff for c in parts[-1]):
        parts.pop()
    csum = lfn_checksum(short)
    out = []
    for i in range(len(parts) - 1, -1, -1):
        p = parts[i]
        seq = (i + 1) | (0x40 if i == len(parts) - 1 else 0)
        e = struct.pack("<B5HBBB6HH2H", seq, *p[0:5], 0x0f, 0, csum, *p[5:11], 0, *p[11:13])
        out.append(e)
    return out


def dos_time(t):
    tm = time.localtime(t)
    year = max(tm.tm_year, 1980)
    d = (year - 1980) << 9 | tm.tm_mon << 5 | tm.tm_mday
    tt = tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec // 2
    return tt, d


def dir_entry(short, attr, cluster, size, mtime):
    tt, d = dos_time(mtime)
    return struct.pack("<11sBBBHHHHHHHI", short, attr, 0, 0, tt, d, d, 0, tt, d, cluster, size)


def entry_count(node):
    n = 0 if node.name == "" else 2            # . and ..
    for c in node.children:
        n += 1 + (len(lfn_entries(c.name, c.short)) if c.lfn else 0)
    return n


# ---- layout -----------------------------------------------------------------

def build(src, out, size_mb=None):
    root = scan(src)
    nodes = []

    def walk(n):
        make_short_names(n.children)
        for c in n.children:
            nodes.append(c)
            if c.is_dir:
                walk(c)
    walk(root)
    content = sum(n.size for n in nodes)
    if size_mb is None:
        size_mb = max(16, (content >> 20) + 16)
    size_mb = max(8, min(1024, size_mb))
    total = size_mb * 2048
    part = total - PART_START

    # Cluster size: the smallest that keeps FAT16 under 65525 clusters.
    spc = 1
    while part // spc > 65524:
        spc *= 2
    cluster = spc * SECTOR
    reserved = 1
    root_secs = ROOT_ENTRIES * 32 // SECTOR
    clusters = (part - reserved - root_secs) // spc
    fat_secs = (2 * (clusters + 2) + SECTOR - 1) // SECTOR
    clusters = (part - reserved - root_secs - 2 * fat_secs) // spc
    if clusters < 4085:
        raise SystemExit("image too small for FAT16")

    if entry_count(root) > ROOT_ENTRIES:
        raise SystemExit("too many files at the top level (%d entries, %d fit)" % (entry_count(root), ROOT_ENTRIES))

    fat = [0] * (clusters + 2)
    fat[0], fat[1] = 0xfff8, 0xffff
    nxt = 2

    def alloc(nbytes):
        nonlocal nxt
        n = max(1, (nbytes + cluster - 1) // cluster)
        if nxt + n > clusters + 2:
            raise SystemExit("the files do not fit: give a bigger SIZE_MB")
        first = nxt
        for i in range(n - 1):
            fat[first + i] = first + i + 1
        fat[first + n - 1] = 0xffff
        nxt += n
        return first

    for n in nodes:
        if n.is_dir:
            n.cluster = alloc(entry_count(n) * 32)
        elif n.size:
            n.cluster = alloc(n.size)

    data_start = reserved + 2 * fat_secs + root_secs     # in the partition

    def dir_bytes(n, parent_cluster):
        ents = []
        if n.name:
            ents.append(dir_entry(b".          ", 0x10, n.cluster, 0, n.mtime))
            ents.append(dir_entry(b"..         ", 0x10, parent_cluster, 0, n.mtime))
        for c in n.children:
            if c.lfn:
                ents += lfn_entries(c.name, c.short)
            ents.append(dir_entry(c.short, 0x10 if c.is_dir else 0x20, c.cluster, c.size, c.mtime))
        return b"".join(ents)

    with open(out, "wb") as f:
        f.truncate(total * SECTOR)
        # MBR: one partition, type 4 (FAT16 under 32 MB) or 6.
        mbr = bytearray(SECTOR)
        ptype = 0x04 if part < 65536 else 0x06
        struct.pack_into("<B3sB3sII", mbr, 446, 0x00, b"\x01\x01\x00", ptype, b"\xfe\xff\xff", PART_START, part)
        mbr[510:512] = b"\x55\xaa"
        f.write(mbr)
        # Boot sector
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xeb\x3c\x90"
        bs[3:11] = b"MSDOS5.0"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, spc, reserved, 2, ROOT_ENTRIES,
                         part if part < 65536 else 0, 0xf8, fat_secs, 63, 255, PART_START,
                         part if part >= 65536 else 0)
        struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29, int(time.time()) & 0xffffffff,
                         b"CYBIKO     ", b"FAT16   ")
        bs[510:512] = b"\x55\xaa"
        base = PART_START * SECTOR
        f.seek(base)
        f.write(bs)
        fat_bytes = struct.pack("<%dH" % len(fat), *fat)
        for i in range(2):
            f.seek(base + (reserved + i * fat_secs) * SECTOR)
            f.write(fat_bytes)
        # Root directory with the volume label
        f.seek(base + (reserved + 2 * fat_secs) * SECTOR)
        f.write(dir_entry(b"CYBIKO     ", 0x08, 0, 0, time.time()) + dir_bytes(root, 0))

        def at(cl):
            return base + (data_start + (cl - 2) * spc) * SECTOR

        def write_tree(n, parent_cluster):
            for c in n.children:
                if c.is_dir:
                    f.seek(at(c.cluster))
                    f.write(dir_bytes(c, parent_cluster))
                    write_tree(c, c.cluster)
                elif c.size:
                    f.seek(at(c.cluster))
                    with open(c.path, "rb") as s:
                        while True:
                            chunk = s.read(1 << 20)
                            if not chunk:
                                break
                            f.write(chunk)
        write_tree(root, 0)
    files = sum(1 for n in nodes if not n.is_dir)
    print("%s: %d MB, %d file(s), %d KB used" % (out, size_mb, files, (content + 1023) // 1024))


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    build(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) == 4 else None)
