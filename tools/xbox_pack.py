#!/usr/bin/env python3
"""Pack a game root into one file for the Xbox build.

The Xbox apps carry only the program; the game lives in each app's own
storage (LocalState), and Device Portal uploads one file at a time. So a
whole game root - the base game, or a mod's root as `tools/dlc.py stage`
builds it - goes over as a single .ojpk file, which the app unpacks into
LocalState\\game on first launch (src/uwp_platform.cpp).

The NBA JAM 2020s and Legends apps take the mod's root:

    python tools/dlc.py stage
    python tools/xbox_pack.py "<staged>/mods/<2020s mod>" 2020s.ojpk

then upload 2020s.ojpk into the NBA JAM 2020s app's LocalState.

The file is EA's game data from your own copy plus the mod: keep it to your
own console.

Format, all integers little-endian, nothing compressed:

    "OJPK1\\0\\0\\0"
    u32 count
    count times:
        u16 path length, path (UTF-8, '/' separated, relative)
        u64 size, then that many bytes
"""

import argparse
import os
import struct
import sys
from pathlib import Path

MAGIC = b"OJPK1\0\0\0"

# What a staged root holds that is not the game: saves and logs belong to
# the PC folder, mods.list and saves.path point at things the console does
# not have, and ui.original is the manager's backup of the front end.
SKIP_DIRS = {"saves", "logs", "mods", "ui.original"}
SKIP_FILES = {"mods.list", "saves.path", "unpacking.txt", "crash_stack.txt"}
SKIP_SUFFIXES = (".exe", ".dll", ".pdb", ".log")


def files_of(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = Path(dirpath).relative_to(root)
        if rel_dir == Path("."):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        dirnames.sort()
        for name in sorted(filenames):
            if rel_dir == Path(".") and (name in SKIP_FILES or name.endswith(SKIP_SUFFIXES)):
                continue
            out.append(Path(dirpath) / name)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="the game root to pack (it has default.xex in it)")
    ap.add_argument("out", help="the .ojpk file to write")
    args = ap.parse_args(argv)

    root = Path(args.root).resolve()
    if not (root / "default.xex").is_file():
        print("{} has no default.xex - point this at a game root".format(root))
        return 1
    files = files_of(root)
    total = sum(f.stat().st_size for f in files)
    print("packing {} file(s), {:.0f} MB, from {}".format(len(files), total / 2**20, root))

    with open(args.out, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<I", len(files)))
        done = 0
        for i, f in enumerate(files):
            rel = f.relative_to(root).as_posix().encode("utf-8")
            size = f.stat().st_size
            out.write(struct.pack("<H", len(rel)))
            out.write(rel)
            out.write(struct.pack("<Q", size))
            # Hard links, as a staged mod's root is made of, read as the file
            # they point at.
            with open(f, "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
            done += size
            if i % 200 == 0 or i == len(files) - 1:
                print("  {:3d}%  {}".format(int(done * 100 / max(total, 1)), rel.decode()),
                      flush=True)
    print("wrote {} ({:.0f} MB)".format(args.out, os.path.getsize(args.out) / 2**20))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
