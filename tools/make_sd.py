#!/usr/bin/env python3
"""Fills SD_CARD/ with what goes on the card: the NRO, your copy of the game, and openQ4's own pack.

    python tools/make_sd.py "<folder with q4base>" [--nro build/quake4.nro] [--pak0 pak0.pk4]

Without --pak0, the pack is made from a clone of openQ4 in .upstream/openQ4 (tools/fetch_pak0.py makes one, of only what
this needs). Your files are only read. Large files are linked instead of copied where the disk allows it, so
filling SD_CARD/ takes no room on the PC.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

CARD_ROOT = "switch/quake4"
# Windows game code: the port has its own
NOT_USED_PREFIX = "game"


def copy_or_link(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if target.stat().st_size == source.stat().st_size:
            return
        target.unlink()
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("install", type=pathlib.Path, help="the folder that holds q4base")
    parser.add_argument("--nro", type=pathlib.Path, default=ROOT / "build" / "quake4.nro")
    parser.add_argument("--pak0", type=pathlib.Path, help="baseoq4/pak0.pk4, already made")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "SD_CARD")
    args = parser.parse_args()

    base = args.install / "q4base"
    if not base.is_dir():
        print("ERROR: %s does not exist" % base)
        return 1

    check = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "check_assets.py"), str(args.install)], capture_output=True, text=True)
    if check.returncode != 0:
        print(check.stdout)
        print("ERROR: the game's files did not pass tools/check_assets.py")
        return 1

    card = args.out / CARD_ROOT
    card.mkdir(parents=True, exist_ok=True)

    if args.nro.is_file():
        shutil.copyfile(args.nro, card / "quake4.nro")
        print("quake4.nro")
    else:
        print("WARNING: no %s: build it, or take it from a release" % args.nro)

    copied = 0
    size = 0
    for source in sorted(base.iterdir()):
        name = source.name.lower()
        if source.suffix.lower() != ".pk4" or (name.startswith(NOT_USED_PREFIX) and name[len(NOT_USED_PREFIX):len(NOT_USED_PREFIX) + 1].isdigit()):
            continue
        copy_or_link(source, card / "q4base" / name)
        copied += 1
        size += source.stat().st_size
    print("q4base: %d packs, %.2f GB" % (copied, size / 1e9))

    target = card / "baseoq4" / "pak0.pk4"
    target.parent.mkdir(parents=True, exist_ok=True)
    if args.pak0:
        shutil.copyfile(args.pak0, target)
        shutil.copyfile(args.pak0.parent / "mod.json", target.parent / "mod.json")
    else:
        from fetch_pak0 import build_pak0, write_manifest
        if not target.exists():
            build_pak0(target)
        write_manifest(target.parent / "mod.json")
    print("baseoq4/pak0.pk4: %.1f MB" % (target.stat().st_size / 1e6))

    print()
    print("Copy what is inside %s to the root of your SD card." % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
