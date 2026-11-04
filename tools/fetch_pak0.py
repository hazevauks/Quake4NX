#!/usr/bin/env python3
"""Makes baseoq4/pak0.pk4, openQ4's own runtime content: its menus, fonts, strings and configs.

It lives in openQ4's repository, not this one, because of its size. This fetches just that folder, at the version the
port's source was imported from, and packs it the way the engine reads it.

    python tools/fetch_pak0.py <output pak0.pk4>
"""

import json
import pathlib
import re
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
UPSTREAM_URL = "https://github.com/themuffinator/openQ4.git"
UPSTREAM_COMMIT = "2422873b9852c7115dedd943df0162604e9f06ab"
CONTENT = "content/baseoq4/pak0"

# file types the engine stores uncompressed in a pack, so reading them does not inflate twice
STORED_SUFFIXES = {".jpg", ".ogg", ".roq"}


def run(*command, cwd=None):
    subprocess.run(command, cwd=cwd, check=True)


def fetch_source():
    clone = ROOT / ".upstream" / "openQ4"
    if (clone / CONTENT).is_dir():
        return clone / CONTENT

    clone = ROOT / ".upstream" / "openQ4-pak0"
    if not (clone / ".git").is_dir():
        clone.mkdir(parents=True, exist_ok=True)
        run("git", "init", "-q", cwd=clone)
        run("git", "remote", "add", "origin", UPSTREAM_URL, cwd=clone)
        run("git", "config", "core.sparseCheckout", "true", cwd=clone)
        run("git", "sparse-checkout", "set", CONTENT, cwd=clone)
    run("git", "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", UPSTREAM_COMMIT, cwd=clone)
    run("git", "checkout", "-q", "FETCH_HEAD", cwd=clone)
    return clone / CONTENT


def build_pak0(output):
    source = fetch_source()
    output = pathlib.Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    files = sorted(path for path in source.rglob("*") if path.is_file())
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            info = zipfile.ZipInfo(path.relative_to(source).as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if path.suffix.lower() in STORED_SUFFIXES else zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    print("%d files into %s" % (len(files), output))


def write_manifest(output):
    """baseoq4/mod.json, which the engine refuses to start without. Its version must be the engine's own."""
    header = (ROOT / "src" / "sys" / "switch" / "generated" / "openq4_version_generated.h").read_text()
    version = re.search(r'#define OPENQ4_VERSION_BASE "([^"]+)"', header).group(1)
    manifest = {
        "name": "openQ4",
        "version": version,
        "releaseDate": "2026-04-14",
        "website": "https://www.darkmatter-quake.com",
        "author": "DarkMatter Studios",
        "requiredopenQ4Version": version,
    }
    pathlib.Path(output).write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    build_pak0(sys.argv[1])
    write_manifest(pathlib.Path(sys.argv[1]).parent / "mod.json")
