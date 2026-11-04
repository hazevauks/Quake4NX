# Testing

## On a PC

[tools/hostcheck](../tools/hostcheck/README.md) runs the port's own code on your game files, with no console. It is the first thing to run after a change: it shows most mistakes in minutes, without copying anything to a card. It cannot show what the console's GPU does, or how fast the game runs.

## On the console

1. Build, then fill `SD_CARD/`:

   ```bash
   ./build.sh
   python tools/make_sd.py "<folder with q4base>"
   ```

2. Copy what is in `SD_CARD/` to the card once. After that only `switch/quake4/quake4.nro` changes.
3. Copy `sd:/switch/quake4/SD_LOG/` back to `SD_LOG/` after each run.

For faster rounds, without taking the card out:

```bash
nxlink -s -a <console address> build/quake4.nro
```

That prints the log in the terminal as it runs. Start the homebrew menu's NetLoader first (Y).

## Reading a crash

`quake4_crash.txt` gives addresses in the executable as `elf offset`:

```bash
aarch64-none-elf-addr2line -f -C -e build/quake4.elf <offset> <offset>
```

The build keeps frame pointers, so the call stack is reliable.

## Measuring

| Cvar | Use |
| --- | --- |
| `com_showFPS 1` | fps on screen |
| `r_dkLogPerf 1` | back end time, draws and memory in the log, once a second |
| `com_speeds 1` | game, front end and back end time, on screen |
| `r_shadows 0` | stencil shadows off, to see their cost |
| `r_dkLogDraws 1` | each material stage as it draws, in the log. Set it for a frame or two: it writes a lot |

## What to check before calling a stage done

| Stage | Check |
| --- | --- |
| Data | the log lists all the `.pk4` files, and no `Couldn't load image` at the main menu |
| Boot | the main menu draws, and the buttons move through it |
| Level | a new game reaches the first map |
| Programs | `reportShaderPrograms` in the console, and `has a stage with a program there is no shader for` in the log, for what is skipped |
| Quake 4's mechanics | the hover tank and walker sections drive and fire, marines follow, the stroggification sequence plays |
| Multiplayer | a local server starts, and a second console joins it |
| Cinematics | the intro and the `.roq` videos play with sound |
| Memory | no `Out of memory` after three map changes |
| Speed | `r_dkLogPerf`, at the same places each time |
