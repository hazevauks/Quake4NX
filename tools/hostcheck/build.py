#!/usr/bin/env python3
"""Builds the port for the PC it runs on, with tools/hostcheck/shim/ standing in for libnx.

    python tools/hostcheck/build.py --game-dir <folder>

The game folder is laid out as on the SD card: q4base/*.pk4 and baseoq4/pak0.pk4. It is compiled in as
the base path, like the console's, and written to (settings, saves, logs), so use a copy.
Needs a GCC whose `long` has 64 bits, as on the console: MSYS2's own "gcc" package (/usr/bin/gcc, not
the mingw64 one), or any Linux; and zlib's headers. See README.md.
"""

import argparse
import concurrent.futures
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"
THIRD = ROOT / "third_party"
OUT = ROOT / "build_host"
HOST = ROOT / "tools" / "hostcheck"
GENERATED = SRC / "sys" / "switch" / "generated"

# the seam to libnx's sound: OpenAL has nothing to play through here
NOT_ON_HOST = {"switch_audio.cpp"}


def posix(path):
    """A path as the compiler takes it. GCC on MSYS2 sees C:/ as relative where it looks for headers."""
    text = pathlib.PurePath(path).as_posix()
    return "/%s%s" % (text[0].lower(), text[2:]) if text[1:2] == ":" else text


def native(path):
    text = str(path)
    if len(text) > 2 and text[0] == "/" and text[2] == "/" and os.name == "nt":
        return pathlib.Path("%s:%s" % (text[1].upper(), text[2:]))
    return pathlib.Path(text)


def source_lists(cmake, env):
    """The lists of cmake/Q4Sources.cmake, from CMake itself."""
    build = OUT / "list_sources"
    output = OUT / "sources.txt"
    subprocess.run([cmake, "-S", posix(HOST / "list_sources"), "-B", posix(build), "-DOUTPUT=%s" % posix(output)],
                   check=True, capture_output=True, env=env)
    lists = {}
    for line in output.read_text().splitlines():
        name, path = line.split(" ", 1)
        lists.setdefault(name, []).append(native(path))
    return lists


class Group:
    """Files that share flags, and so a precompiled header."""

    def __init__(self, name, sources, flags, std="gnu++20", pch=True):
        self.name = name
        self.sources = sources
        self.flags = flags
        self.std = std
        self.pch = pch
        self.objects = []


def compile_one(job):
    command, source, obj = job
    if obj.exists() and obj.stat().st_mtime >= source.stat().st_mtime:
        return None
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(command + ["-c", posix(source), "-o", posix(obj)], capture_output=True, text=True, env=compile_one.env)
    if result.returncode:
        return "%s:\n%s" % (source, result.stderr[-3500:])
    return None


def run_jobs(jobs, workers):
    done = 0
    errors = []
    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for error in pool.map(compile_one, jobs):
            done += 1
            if error:
                errors.append(error)
            if done % 50 == 0:
                print("%d/%d, %d s" % (done, len(jobs), time.time() - started), flush=True)
    for error in errors[:12]:
        print(error)
    if errors:
        sys.exit("%d files failed" % len(errors))


def check(command, env):
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    if result.returncode:
        print(" ".join(str(c) for c in command)[:2000])
        print(result.stdout[-6000:], result.stderr[-12000:])
        sys.exit(1)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-dir", required=True, help="the folder laid out as on the card, written to")
    parser.add_argument("--msys", default="C:/msys64", help="MSYS2's folder, for its gcc and libraries")
    parser.add_argument("--devkitpro", required=True, help="devkitPro's folder, as this program sees it: for deko3d.h, OpenAL's headers and CMake")
    parser.add_argument("--cmake", default=None)
    parser.add_argument("--opt", default="-O1", help="for example -O0, or -O1 -fsanitize=alignment")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--no-pch", action="store_true")
    args = parser.parse_args()

    msys_bin = pathlib.Path(args.msys) / "usr" / "bin"
    gcc, gxx = str(msys_bin / "gcc.exe"), str(msys_bin / "g++.exe")
    env = dict(os.environ)
    env["PATH"] = str(msys_bin) + os.pathsep + env["PATH"]
    compile_one.env = env

    # headers from devkitPro: only those that do not bring the console's C library along
    include = OUT / "include"
    (include / "AL").mkdir(parents=True, exist_ok=True)
    shutil.copy(pathlib.Path(args.devkitpro) / "libnx" / "include" / "deko3d.h", include / "deko3d.h")
    for header in (pathlib.Path(args.devkitpro) / "portlibs" / "switch" / "include" / "AL").glob("*.h"):
        shutil.copy(header, include / "AL" / header.name)

    cmake = args.cmake or str(pathlib.Path(args.devkitpro) / "msys2" / "usr" / "bin" / "cmake.exe")
    lists = source_lists(cmake, env)

    # in a header: quotes do not survive a command line between Windows and MSYS2
    config = OUT / "host_config.h"
    game_dir = pathlib.Path(args.game_dir).resolve().as_posix()
    config_text = ('#define SWITCH_BASE_PATH "%s"\n#define DEFAULT_BASE_PATH "%s"\n#define Q4NX_ROMFS_SHADERS "%s/"\n#define RMLUI_VERSION "6.3"\n'
                   % (game_dir, game_dir, (OUT / "romfs").as_posix()))
    if not config.exists() or config.read_text() != config_text:
        config.write_text(config_text)

    # the compiled shaders: here each is just its name (fake_deko3d.cpp)
    romfs = OUT / "romfs"
    romfs.mkdir(exist_ok=True)
    for shader in list((ROOT / "shaders").glob("*.vert")) + list((ROOT / "shaders").glob("*.frag")):
        (romfs / (shader.name + ".dksh")).write_bytes(shader.name.encode() + b"\0")

    base = ["-fsigned-char", "-fno-strict-aliasing", "-fno-omit-frame-pointer", "-w", "-g1"] + args.opt.split()
    defines = ["-D__SWITCH__", "-DQ4NX_HOSTCHECK", "-DUSE_OPENAL", "-DGLEW_NO_GLU", "-include", posix(GENERATED / "switch_compat.h"), "-include", posix(config)]
    cxx_extra = ["-fpermissive", "-fexceptions", "-frtti", "-Wa,-mbig-obj"]
    host_includes = [HOST / "shim", HOST, include]

    def includes(*paths):
        return ["-I%s" % posix(p) for p in list(paths) + host_includes]

    retained_inc = includes(ROOT, THIRD / "rmlui/Include", THIRD / "jsoncpp/include", THIRD / "libtess2/Include", GENERATED)
    retained_def = ["-DRMLUI_STATIC_LIB", "-DRMLUI_NO_THIRDPARTY_CONTAINERS"]

    game_inc = [ROOT, SRC, SRC / "idlib", SRC / "renderer", SRC / "ui", SRC / "sys", SRC / "bse", SRC / "mayaimport", GENERATED]
    engine_sources = [s for s in lists["Q4_ENGINE_SOURCES"] if s.suffix != ".c"]
    engine_c = [s for s in lists["Q4_ENGINE_SOURCES"] if s.suffix == ".c"]
    platform = [s for s in lists["Q4_PLATFORM_SOURCES"] if s.name not in NOT_ON_HOST]
    platform += [SRC / "sys" / "stub" / "stub_openal.cpp", HOST / "host_commands.cpp"]

    groups = [
        # the stand-ins know nothing of the engine: idlib's headers forbid the C library's snprintf
        Group("host", [HOST / "fake_deko3d.cpp", HOST / "shim" / "switch_shim.cpp"], includes(), std="gnu++17", pch=False),
        Group("retained",lists["Q4_RETAINED_SOURCES"] + lists["Q4_RMLUI_SOURCES"] + lists["Q4_JSONCPP_SOURCES"], retained_inc + retained_def, pch=False),
        Group("retained_c", lists["Q4_LIBTESS2_SOURCES"], retained_inc + retained_def, std=None, pch=False),
        Group("game_sp", lists["Q4_GAME_SP_SOURCES"] + lists["Q4_IDLIB_SOURCES"], includes(*game_inc, SRC / "game") + ["-DGAME_DLL", "-DGetGameAPI=GetGameAPI_SP"], std="gnu++17"),
        Group("game_mp", lists["Q4_GAME_MP_SOURCES"] + lists["Q4_IDLIB_SOURCES"], includes(*game_inc, SRC / "mpgame", SRC / "game") + ["-DGAME_DLL", "-DGAME_MPAPI", "-DGetGameAPI=GetGameAPI_MP"], std="gnu++17"),
        Group("bse", lists["Q4_BSE_SOURCES"], includes(ROOT, SRC, SRC / "bse", SRC / "game", GENERATED)),
        Group("engine_c", engine_c + [SRC / "external/glew/glew.c"], includes(SRC) + ["-DOPENQ4_GLEW_SDL3_LOADER", "-DGLAPI=extern"], std=None, pch=False),
        Group("engine", engine_sources + lists["Q4_RENDERER_SOURCES"] + lists["Q4_IDLIB_SOURCES"] + platform,
              includes(ROOT, SRC, SRC / "game", GENERATED, SRC / "external/stb_vorbis", THIRD / "rmlui/Include", THIRD / "jsoncpp/include", THIRD / "libtess2/Include")
              + ["-D__DOOM_DLL__", "-DOPENQ4_RENDERER_DK", "-DRMLUI_STATIC_LIB", "-DRMLUI_NO_THIRDPARTY_CONTAINERS"]),
    ]

    jobs = []
    pch_jobs = []
    for group in groups:
        compiler = [gxx, "-std=%s" % group.std] + cxx_extra if group.std else [gcc, "-std=gnu11"]
        command = compiler + base + defines + group.flags
        if group.pch and not args.no_pch:
            folder = OUT / "pch" / group.name / "idlib"
            folder.mkdir(parents=True, exist_ok=True)
            header = folder / "precompiled.h"
            wrapper = '#include "%s"\n' % posix(SRC / "idlib" / "precompiled.h")
            if not header.exists() or header.read_text() != wrapper:
                header.write_text(wrapper)
            gch = folder / "precompiled.h.gch"
            pch_jobs.append((command + ["-x", "c++-header"], header, gch))
            command = command + ["-include", posix(header)]
        elif group.pch:
            command = command + ["-include", posix(SRC / "idlib" / "precompiled.h")]
        for source in group.sources:
            rel = source.resolve().relative_to(ROOT).as_posix().replace("/", "_")
            obj = OUT / "obj" / group.name / (rel + ".o")
            this = list(command)
            if source.name == "switch_main.cpp":
                this.append("-Dmain=Switch_Main")
            jobs.append((this, source, obj))
            group.objects.append(obj)

    if pch_jobs:
        print("precompiling %d headers" % len(pch_jobs), flush=True)
        run_jobs(pch_jobs, args.jobs)
    print("compiling %d files with %d jobs" % (len(jobs), args.jobs), flush=True)
    run_jobs(jobs, args.jobs)

    by_name = {g.name: g for g in groups}
    print("linking", flush=True)

    # The game modules are real libraries here: on the PC's executable format the console's way
    # of keeping them apart (cmake/Q4GameObject.cmake) does not stop COMDATs from being shared.
    libraries = []
    for name, entry in (("game_sp", "GetGameAPI_SP"), ("game_mp", "GetGameAPI_MP")):
        definition = OUT / ("%s.def" % name)
        definition.write_text("EXPORTS\n    %s\n" % entry)
        response = OUT / ("%s.rsp" % name)
        response.write_text("\n".join(posix(o) for o in by_name[name].objects) + "\n")
        library = OUT / ("q4_%s.dll" % name)
        implib = OUT / ("libq4_%s.dll.a" % name)
        check([gxx, "-shared", "-g", "@%s" % posix(response), posix(definition), "-Wl,--out-implib,%s" % posix(implib), "-o", posix(library), "-lm"] + args.opt.split(), env)
        libraries.append(implib)

    exe = OUT / "quake4_host.exe"
    host_main = OUT / "obj" / "host_main.o"
    check([gxx, "-std=gnu++17", "-g1", "-w", "-I%s" % posix(HOST), "-c", posix(HOST / "host_main.cpp"), "-o", posix(host_main)], env)
    objects = by_name["host"].objects + by_name["engine"].objects + by_name["engine_c"].objects + by_name["bse"].objects + by_name["retained"].objects + by_name["retained_c"].objects + [host_main]
    response = OUT / "link.rsp"
    response.write_text("\n".join(posix(o) for o in objects) + "\n")
    check([gxx, "@%s" % posix(response), "-g", "-Wl,--wrap=pthread_create", "-Wl,--stack,16777216", "-o", posix(exe)] + [posix(l) for l in libraries] + ["-lz", "-lm", "-lpthread"] + args.opt.split(), env)
    print("built", exe)


if __name__ == "__main__":
    main()
