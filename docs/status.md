# Status

What has been checked, and how. Nothing here counts as working until a console has shown it.

## Stages

"On the PC" means the host check in [tools/hostcheck](../tools/hostcheck/README.md): the port's own code on the real game files, with stand-ins for libnx and the GPU.

| Stage | State | Checked by |
| --- | --- | --- |
| Source, asset layout, packs | Done | `tools/check_assets.py`, on the real game files |
| File system: paths, saves, settings | Runs on the PC, not on a card | Saving and loading a game in the host check |
| Renderer and threads separated from OpenGL and SDL | Done | The link has no OpenGL driver, Mesa or SDL |
| Build with devkitA64 and CMake | Done | Local build. GitHub Actions: see the repository's Actions page |
| POSIX layer, memory, threads | Runs on the PC, not on a console | Starting, playing and quitting in the host check |
| Controller mapping | Runs on the PC, feel not tested | Buttons and sticks from a scenario |
| Audio | Built, not run | OpenAL is a stub in the host check |
| Boot on the console, logs | **Next: waiting on the first test** | |
| Asset loading in the engine: packs, materials, images | Runs on the PC | Menus, two single player maps, one multiplayer map |
| Native deko3D renderer | Runs on a stand-in GPU, never on a real one | Pictures checked by eye, no `FAKE DEKO3D ERROR`, shaders compile with `uam` |
| Performance and tuning | **Waiting for a console** | |

## What the checks found

On the GOG copy of the game:

- All 22 required packs and all 11 optional ones match the official checksums.
- 32,070 files are visible to the engine, and no two names in one pack differ only by case.
- The longest name in a pack is 197 characters, well under the card's limit.
- The largest file is 44.7 MB, so FAT32 is fine.
- 8,454 images are compressed already: DXT1 4,980, RXGB 1,869, DXT5 1,568, DXT3 19, and 18 uncompressed.
- 2.82 GB go on the card, without the Windows game code.

In the host check:

- `sp_game.txt` goes through the main menu, the HUD, firing, moving, a save, a load and the pause menu, and quits cleanly.
- `mp_smoke.txt` starts a local server in the multiplayer module, and reaches its join screen.
- `sp_smoke.txt` plays the first map's opening cinematic.
- The heaviest of these took about 2.5 GB of the 3.2 GB an application gets, measured on the PC.

It found five things that would have stopped the game on a console, all fixed: a missing `baseoq4/mod.json`, settings and saves that refused a path starting with `sdmc:/`, saves stamped with build 0, and a thread that kept every exit waiting forever.

## Not checked yet

Everything that needs the console:

- The boot, and whether the real GPU draws what the stand-in draws
- Frame rate, load times and the real memory limit
- Sound
- How the controls feel, rumble and the gyro
- Reading from a real SD card
- Vehicles, the stroggification sequence, and joining another console in multiplayer

## Known gaps

| Gap | Note |
| --- | --- |
| Material programs | Six retail ARB programs draw, through this renderer's own shaders. The stages of openQ4's GLSL programs (`displacement`, `multiplyBlend`, `medlabs`, `blur` and others, for some effects) are skipped, and named in the log |
| Post-processing | Bloom, SSAO, HDR, SMAA, shadow maps and temporal AA are off: the scene draws straight to the screen, for the frame budget |
| Depth textures | Materials that sample the depth buffer, such as soft particles, are not drawn |
| Docked resolution | Always 1280x720, scaled by the system |
| Loading screens | The retail ones: openQ4's tiles are in `pak1.pk4`, which is left out |
| Touch screen | Drives the menus as a relative pointer: the cursor moves by the drag, it does not jump to the finger |
| Switching between single player and multiplayer | Going back to the one already used starts the NRO again, which takes a start's wait |
| Render thread | The back end runs on the game's core by default. `r_dkRenderThread 1` moves it to core 1, and that is the first thing to try if the CPU is what holds 60 fps back. It has not been run |
| Main menu prompts | Keyboard prompts and a mouse cursor show until the first button press, then the controller's |
| The opening cinematic | A controller button did not skip it in the host check, which is why saving there is refused. Not looked into |
| Voice chat | Not available |
