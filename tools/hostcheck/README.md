# Host check

Runs the port on a PC, on the game's real files, with no console and no emulator.

`shim/` stands in for libnx, so `src/sys/switch/` compiles unchanged for the PC. `fake_deko3d.cpp` stands in for deko3D, so `src/renderer/deko3d/` does too: it records commands and runs them when they are submitted, keeps state across frames as a GPU does, and prints a line starting with `FAKE DEKO3D ERROR` for what it would refuse. For the frames a scenario asks a picture of, it also draws what the commands ask.

The game plays a scenario instead of a player, on a virtual clock. The game modules are real libraries here, because the PC's executable format would merge what the console's build keeps apart.

## What you need

- MSYS2's own `gcc` and `zlib-devel` packages (`/usr/bin/gcc`, whose `long` has 64 bits like the console's, not the mingw64 one), or any Linux.
- devkitPro, for `deko3d.h`, OpenAL's headers and CMake.
- A folder laid out like the card. The game writes settings, saves and logs there, so don't use your install. `tools/make_sd.py` makes one with hard links, which costs no disk space:

```bash
python tools/make_sd.py "/path/to/Quake 4" --out build_host/card
```

## Running it

```bash
python tools/hostcheck/build.py --game-dir build_host/card/switch/quake4 --devkitpro "/path/to/devkitPro"
```

```bash
mkdir -p build_host/out
```

```bash
build_host/quake4_host.exe tools/hostcheck/scenarios/sp_smoke.txt build_host/out
```

On Windows, run it from an MSYS2 shell, or with `C:\msys64\usr\bin` first in `PATH`.

`build.py` compares each object with its source file only, so after changing a header, delete `build_host/obj` and `build_host/pch`.

Pictures go to the output folder. The log goes to `SD_LOG` in the game folder, as on the console. The format of a scenario is at the top of `shim/switch_shim.cpp`.

## Reading the result

A `FAKE DEKO3D ERROR` or `HOST CHECK ERROR` line is a failed run, whatever the pictures look like.

| It can show | It cannot show |
| --- | --- |
| The port's code boots, loads the real files and reaches the game | Anything about the console's GPU, or that the compiled shaders work |
| What the renderer asks for makes the right picture | Frame rate or load times |
| The GPU interface is used correctly, as far as the fake checks | Sound: OpenAL is stubbed |
| How much memory a map takes, roughly | Input feel, rumble, gyro, the real SD card |

| Scenario | What it does |
| --- | --- |
| `sp_game.txt` | The main menu, the HUD, firing, moving, a save, a load and the pause menu |
| `sp_smoke.txt` | The first map's opening cinematic |
| `mp_smoke.txt` | A local multiplayer server, up to its join screen |

To find out what drew a pixel, add `probe x y` to a scenario: each write to it in the pictures that follow goes to the log. In the fake GPU, memory that was never written reads as 0xCD, which shows up as a color of 0.804.

The shaders are written out a second time in C++ at the end of `fake_deko3d.cpp`, so a change to one in `shaders/` has to be made there too.
