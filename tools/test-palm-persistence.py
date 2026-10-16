#!/usr/bin/env python3
"""Optional real-ROM integration check. ROM stays local and is never copied.

Usage: python3 tools/test-palm-persistence.py build-host/dragonfruit /path/to/palm.rom
Checks SD-style database installation, cold-process RAM restore, keeping edited
databases, installing new files after restore, and Palm's reported RAM size.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def database(path, name, text):
    header = bytearray(88)
    encoded = name.encode("ascii")
    header[:len(encoded)] = encoded
    struct.pack_into(">HH", header, 32, 0, 1)
    header[60:68] = b"DATADFst"
    struct.pack_into(">H", header, 76, 1)
    struct.pack_into(">I", header, 78, 88)
    header[85] = 1
    path.write_bytes(header + text)


def payload(path):
    raw = path.read_bytes()
    return raw[struct.unpack_from(">I", raw, 78)[0]:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emulator", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--ram-mb", type=int, choices=(2, 4), default=4)
    parser.add_argument("--card", action="store_true", help="also enable virtual storage (OS 4 ROM)")
    args = parser.parse_args()
    exe, rom = args.emulator.resolve(), args.rom.resolve()
    with tempfile.TemporaryDirectory(prefix="dragonfruit-state-") as temp:
        root = Path(temp)
        source = root / "PersistTest.pdb"
        database(source, "PersistTest", b"Saved user data\0")

        def run(label, installs):
            cmd = [str(exe), "-r", str(rom), "-c", "400000000",
                   "--state", "checkpoint", "-b", label]
            if args.card:
                cmd += ["--card", str(root / "card")]
            for item in installs:
                cmd += ["-i", str(item)]
            result = subprocess.run(cmd, cwd=root, capture_output=True, text=True, timeout=45)
            log = result.stdout + result.stderr
            if result.returncode or "CRASH GUARD" in log or "timed out" in log:
                raise RuntimeError(log)
            expected = f"Palm memory: RAM={args.ram_mb * 1024 * 1024} "
            assert expected in log, log
            assert "RAM checkpoint: saved/restored" in log, log
            if args.card:
                assert "SD Card mounted as Palm VFS volume 1" in log, log
            return log

        first = run("before", [source])
        assert "RAM restore: no saved RAM" in first
        assert payload(root / "before/PersistTest.pdb") == b"Saved user data\0"
        database(source, "PersistTest", b"Original SD file changed\0")
        extra = root / "NewFromSD.pdb"
        database(extra, "NewFromSD", b"New install after restore\0")
        second = run("after", [source, extra])
        assert "RAM restore: saved/restored" in second
        assert "already present (kept)" in second
        assert payload(root / "after/PersistTest.pdb") == b"Saved user data\0"
        assert payload(root / "after/NewFromSD.pdb") == b"New install after restore\0"
        # Two committed generations, newest intentionally truncated: older must load.
        (root / "checkpoint.1").write_bytes(b"interrupted write")
        third = run("fallback", [])
        assert "RAM restore: saved/restored" in third
        assert payload(root / "fallback/PersistTest.pdb") == b"Saved user data\0"
        assert not (root / "fallback/NewFromSD.pdb").exists()
    print(f"PASS {rom.name}: {args.ram_mb} MB RAM, install/restore/keep/new install/corrupt-save fallback")


if __name__ == "__main__":
    main()
