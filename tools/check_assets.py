#!/usr/bin/env python3
"""Checks a Quake 4 install before it goes on the SD card, without changing it.

Recomputes each .pk4's checksum as the engine does, compares it with the official list, and reports
what the Switch's file system and renderer will face: path lengths, names that differ only by case,
image formats, the game's own GPU programs, and how much room the game takes.

    python tools/check_assets.py "<folder with q4base>"          # central directories only, fast
    python tools/check_assets.py "<folder with q4base>" --deep   # also reads every file back
"""

import argparse
import collections
import pathlib
import struct
import sys
import zipfile

# The engine refuses to start without these (src/framework/FileSystem.cpp).
REQUIRED = {
    "pak001.pk4": 0xf2cbc998, "pak002.pk4": 0x7f8d80d1, "pak003.pk4": 0x1b57b207, "pak004.pk4": 0x385aa578,
    "pak005.pk4": 0x60d50a1d, "pak006.pk4": 0x9099ed11, "pak007.pk4": 0xaf301fff, "pak008.pk4": 0x4ac6f6d9,
    "pak009.pk4": 0x36030c7d, "pak010.pk4": 0x4b80fbda, "pak011.pk4": 0x8acf4cfa, "pak012.pk4": 0xbe4120b0,
    "pak013.pk4": 0x6ad67f40, "pak014.pk4": 0xee51cd59, "pak015.pk4": 0xf5bf4e0c, "pak016.pk4": 0x2196f58c,
    "pak017.pk4": 0x91118a35, "pak018.pk4": 0x98a14f03, "pak019.pk4": 0xbc82ac79, "pak020.pk4": 0xce74cda5,
    "pak021.pk4": 0x2ba6e70c, "pak022.pk4": 0x4e390eec,
}
OPTIONAL = {
    "pak023.pk4": 0x7c1fd3a5, "pak024.pk4": 0x5546d551, "pak025.pk4": 0xcaeec1fd,
    "q4cmp_pak001.pk4": 0xd0813943, "zpak_english.pk4": 0x5868f530, "zpak_english_01.pk4": 0xd9f04b8b,
    "zpak_english_02.pk4": 0x9dbd91fd, "zpak_english_03.pk4": 0x02eb6ad8, "zpak_english_04.pk4": 0xd3fefaa1,
    "zpak_english_05.pk4": 0x8596af60, "zpak_spanish.pk4": 0xb706e2b8,
}
# Windows game code in .pk4 form. The port has its own game code, so these are not used.
IGNORED_PREFIX = "game"

# Horizon's limit is 0x301 bytes for a whole path. The engine's folder takes about 30 of them.
HORIZON_PATH_LIMIT = 769 - 30


def md4(message):
    """MD4, which hashlib no longer offers on many systems."""
    def rol(value, shift):
        value &= 0xffffffff
        return ((value << shift) | (value >> (32 - shift))) & 0xffffffff

    words = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    length = len(message) * 8
    message = message + b"\x80" + b"\0" * ((55 - len(message)) % 64) + struct.pack("<Q", length)
    for offset in range(0, len(message), 64):
        x = struct.unpack("<16I", message[offset:offset + 64])
        a, b, c, d = words
        for i in (0, 4, 8, 12):
            a = rol(a + ((b & c) | (~b & d)) + x[i], 3)
            d = rol(d + ((a & b) | (~a & c)) + x[i + 1], 7)
            c = rol(c + ((d & a) | (~d & b)) + x[i + 2], 11)
            b = rol(b + ((c & d) | (~c & a)) + x[i + 3], 19)
        for i in (0, 1, 2, 3):
            a = rol(a + ((b & c) | (b & d) | (c & d)) + x[i] + 0x5a827999, 3)
            d = rol(d + ((a & b) | (a & c) | (b & c)) + x[i + 4] + 0x5a827999, 5)
            c = rol(c + ((d & a) | (d & b) | (a & b)) + x[i + 8] + 0x5a827999, 9)
            b = rol(b + ((c & d) | (c & a) | (d & a)) + x[i + 12] + 0x5a827999, 13)
        for i in (0, 2, 1, 3):
            a = rol(a + (b ^ c ^ d) + x[i] + 0x6ed9eba1, 3)
            d = rol(d + (a ^ b ^ c) + x[i + 8] + 0x6ed9eba1, 9)
            c = rol(c + (d ^ a ^ b) + x[i + 4] + 0x6ed9eba1, 11)
            b = rol(b + (c ^ d ^ a) + x[i + 12] + 0x6ed9eba1, 15)
        words = [(w + v) & 0xffffffff for w, v in zip(words, (a, b, c, d))]
    return words


def pk4_checksum(archive):
    """As LoadZipFile: MD4 over the CRCs of the non-empty files, in directory order, folded to 32 bits."""
    crcs = [info.CRC for info in archive.infolist() if info.file_size > 0]
    words = md4(struct.pack("<%dI" % len(crcs), *crcs))
    return words[0] ^ words[1] ^ words[2] ^ words[3]


def dds_format(archive, info):
    """The image format and size of a .dds, from its first bytes."""
    with archive.open(info) as stream:
        header = stream.read(128)
    if len(header) < 128 or header[:4] != b"DDS ":
        return "not a DDS", 0, 0
    height, width = struct.unpack("<II", header[12:20])
    flags = struct.unpack("<I", header[80:84])[0]
    if flags & 4:
        return header[84:88].decode("latin-1").strip("\0 ") or "fourcc?", width, height
    bits = struct.unpack("<I", header[88:92])[0]
    return "uncompressed %d bits" % bits, width, height


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("install", type=pathlib.Path, help="the folder that holds q4base")
    parser.add_argument("--deep", action="store_true", help="read every file back and test its CRC")
    args = parser.parse_args()

    base = args.install / "q4base"
    if not base.is_dir():
        print("ERROR: %s does not exist" % base)
        return 1

    problems = []
    names = {}
    extensions = collections.Counter()
    bytes_by_extension = collections.Counter()
    dds_formats = collections.Counter()
    dds_not_block_aligned = []
    programs = set()
    maps = collections.defaultdict(set)
    longest = ""
    biggest = ("", 0)
    used_bytes = 0
    packs = sorted(path for path in base.iterdir() if path.suffix.lower() == ".pk4")

    print("%-26s %11s %8s  %-10s %s" % ("pack", "bytes", "files", "checksum", "state"))
    for path in packs:
        key = path.name.lower()
        ignored = key.startswith(IGNORED_PREFIX) and key[len(IGNORED_PREFIX):len(IGNORED_PREFIX) + 1].isdigit()
        try:
            archive = zipfile.ZipFile(path)
        except zipfile.BadZipFile as error:
            problems.append("%s: not a valid archive (%s)" % (path.name, error))
            continue
        with archive:
            checksum = pk4_checksum(archive)
            expected = REQUIRED.get(key, OPTIONAL.get(key))
            if ignored:
                state = "not used: Windows game code"
            elif expected is None:
                state = "not on the official list: mounted anyway"
            elif expected == checksum:
                state = "ok"
            elif key in REQUIRED:
                state = "CHANGED"
                problems.append("%s: checksum 0x%08x, expected 0x%08x. The engine would refuse it." % (path.name, checksum, expected))
            else:
                state = "changed, not required"
            print("%-26s %11d %8d  0x%08x %s" % (path.name, path.stat().st_size, len(archive.infolist()), checksum, state))
            if ignored:
                continue
            used_bytes += path.stat().st_size

            if args.deep:
                broken = archive.testzip()
                if broken:
                    problems.append("%s: %s fails its CRC" % (path.name, broken))

            for info in archive.infolist():
                if info.is_dir():
                    continue
                name = info.filename.replace("\\", "/")
                lower = name.lower()
                # The engine lowercases names in a .pk4, so packs can differ by case. Inside one pack it is a clash.
                previous = names.get(lower)
                if previous and previous[0] == path.name and previous[1] != name:
                    problems.append("%s: %s and %s differ only by case" % (path.name, previous[1], name))
                names[lower] = (path.name, name)
                if len(name) > len(longest):
                    longest = name
                if info.file_size > biggest[1]:
                    biggest = ("%s:%s" % (path.name, name), info.file_size)
                extension = pathlib.PurePosixPath(lower).suffix or "(none)"
                extensions[extension] += 1
                bytes_by_extension[extension] += info.file_size
                if extension == ".dds":
                    fmt, width, height = dds_format(archive, info)
                    dds_formats[fmt] += 1
                    if fmt in ("DXT1", "DXT3", "DXT5", "RXGB") and (width % 4 or height % 4) and width > 4 and height > 4:
                        dds_not_block_aligned.append(lower)
                elif extension in (".vfp", ".vp", ".fp"):
                    programs.add(lower)
                elif lower.startswith("maps/") and extension in (".map", ".proc", ".cm") or extension.startswith(".aas"):
                    maps[extension].add(lower)

    missing = sorted(set(REQUIRED) - {path.name.lower() for path in packs})
    for name in missing:
        problems.append("%s is missing. The engine refuses to start without it." % name)
    if not any(path.name.lower().startswith("zpak_") and path.name.lower().count("_") == 1 for path in packs):
        problems.append("no zpak_<language>.pk4: characters would have no voices")

    print()
    print("Room on the SD card, without the Windows game code: %.2f GB" % (used_bytes / 1e9))
    print("Files visible to the engine: %d" % len(names))
    print("Longest name inside a pack: %d characters (limit on the card %d), %s" % (len(longest), HORIZON_PATH_LIMIT, longest))
    print("Biggest file: %s, %.1f MB" % (biggest[0], biggest[1] / 1e6))
    if len(longest) > HORIZON_PATH_LIMIT:
        problems.append("%s is too long for the SD card" % longest)

    print()
    print("By type:")
    for extension, count in sorted(bytes_by_extension.items(), key=lambda item: -item[1])[:16]:
        print("  %-14s %7d files %9.1f MB" % (extension, extensions[extension], count / 1e6))

    print()
    print("Compressed images (.dds), by format:")
    for fmt, count in dds_formats.most_common():
        print("  %-22s %7d" % (fmt, count))
    if dds_not_block_aligned:
        print("  %d have a size that is not a multiple of 4, which the GPU's block formats must be padded for" % len(dds_not_block_aligned))

    print()
    print("The game's own GPU programs (they must be turned into the Switch's format): %d" % len(programs))
    for extension, found in sorted(maps.items()):
        print("Level files %-8s %d" % (extension, len(found)))

    print()
    for problem in problems:
        print("ERROR: " + problem)
    print("%d problems" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
