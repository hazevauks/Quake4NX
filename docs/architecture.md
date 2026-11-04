# How the port is put together

## Layout

```
src/                 the engine and both games, from openQ4 (upstream 2422873)
  renderer/deko3d/   the native renderer
  sys/switch/        the platform layer: files, threads, input, audio, network, logs
shaders/             the renderer's GLSL, compiled into the NRO with uam
third_party/         RmlUi, JsonCpp and libtess2, with upstream's patches applied
tools/               on the PC: checks, the pack, the SD card
cmake/               build helpers
SD_CARD/             test build, ready to copy to the card (not in git)
SD_LOG/              logs copied back from the card (not in git)
```

## What is on the card

| Folder | Holds | From |
| --- | --- | --- |
| `q4base/` | The retail `.pk4` files, without the Windows game code (`game*.pk4`) | Your copy of Quake 4 |
| `baseoq4/pak0.pk4` | openQ4's menus, fonts, strings and configs | openQ4's repository, made by `tools/fetch_pak0.py` |
| `baseoq4/mod.json` | The runtime files' version, which the engine checks at start | `tools/fetch_pak0.py` |
| `baseoq4/` | Your config and saves | The game |
| `SD_LOG/` | Logs and crash reports | The game |

openQ4 has a second pack, `pak1.pk4`, of about 800 MB. It holds light grids and cube maps for the modern renderer, tiles for its loading screens, and one extra map. The native renderer does not use the first two, and the engine falls back to the retail loading screens, so the port leaves it out. Packs are not hashed at each start either: on the SD card, hashing 800 MB would take many seconds.

## The game modules

Quake 4 has two game modules, one for single player and one for multiplayer, with different types under the same names, and each expects to be a library of its own with its own copies of idlib and of the engine's interface globals.

Homebrew cannot load a library. So each module's objects are linked into one relocatable object, with COMDAT groups dissolved, its `GetGameAPI` gets the module's own name (`GetGameAPI_SP`, `GetGameAPI_MP`) and every other symbol is made local (`cmake/Q4GameObject.cmake`). `Sys_DLL_Load` hands out the one the engine asks for.

## The renderer

There is no OpenGL in the build, and no Mesa. openQ4 already separates its shared front end from its back ends, to run on Vulkan, so the deko3D back end takes the same places in the shared code (`OPENQ4_RENDERER_DK`).

| File | Replaces | Does |
| --- | --- | --- |
| `dk_device.cpp` | `glimp`, driver | device, memory blocks, swapchain, command buffers, uploads, frame pacing |
| `dk_state.cpp` | `GL_State`, parts of `tr_backend.cpp` | tracks state, writes only what changed before a draw |
| `dk_draw.cpp` | `draw_common`, `draw_arb2`, `tr_render` | the view's passes |
| `dk_image.cpp` | `gl_Image`, parts of `Image_load` | image storage, uploads, screen copies, cinematics |
| `dk_programs.cpp` | ARB and GLSL program loading | shader modules by the name materials use |
| `dk_backend.cpp` | `tr_backend`, `gl_RenderTexture` | the frame's commands, render targets, what the front end links against |
| `dk_GLStubs.cpp` | OpenGL itself | the GL calls left in shared files, which draw nothing |
| `VertexCache.cpp` | same file | static and per-frame GPU memory |

### Straight to the screen

On PC, Quake 4's game code draws the scene to a render texture and resolves it through post-process materials. On the Switch `idGameLocal::RenderScene` draws straight to the screen, which saves several full-screen passes a frame, and the targets are not made (`game/Game_render.cpp`). Features that would need them (bloom, SSAO, HDR, SMAA, shadow maps, temporal AA and so on) stay off, whatever a config says (`Dk_ForceSettings`).

### Conventions

Clip space has y up and depth -1 to 1, as on OpenGL, so matrices and winding work unchanged. The screen's first row is its top, which differs from OpenGL in three places, all of them converted: viewports and scissors (`Dk_SetViewport`, `Dk_SetScissor`), a fragment's position (the heat haze shaders), and screen copies, which flip so programs read `_currentRender` as on OpenGL (`idImage::CopyFramebuffer`). A render target keeps OpenGL's rows, with a negative viewport height.

### Uniforms

Programs read registers, as ARB programs do. `program.env`, `program.local` and the matrices are uniform blocks, pushed into the command stream when they change (`Dk_FlushForDraw`):

| Vertex binding | Fragment binding | Holds |
| --- | --- | --- |
| 0 | | `mvp`, `modelView`, `projection`, `texture`, as rows |
| 1 | 0 | `program.env` |
| 2 | 1 | `program.local` |

### Vertex attributes

`0` position, `1` texture coordinates, `2` normal, `3` tangent, `4` bitangent, `5` color.

### Shaders

Everything is in `shaders/` and goes into the NRO's romfs, so there is nothing to compile on a PC at install, and nothing made from the game's data.

Retail materials name six ARB programs: `heatHaze` (and its variants with a mask), `bumpyEnvironment` and `monochrome`. Each has a shader here, written from the behavior upstream documents in its own ports of them. Every material stage that uses another program, such as the openQ4 GLSL ones (`displacement`, `multiplyBlend`, `medlabs` and so on, for special effects), is skipped and named once in the log.

### Vertex colors

Quake 4's `SVC_INVERSE_MODULATE` inverts only the color, not the alpha. The `_to_smolder` materials write a mask through it, and inverting the alpha would remove their overlay. So the stage shaders take four terms: color modulate and add, alpha modulate and add.

### Images

Quake 4 ships most of its textures compressed in `dds/`, and `progimg/` for the results of image programs. openQ4's loader reads them as they are: no decoding on the console, a quarter of the memory, and the GPU's own BC1 and BC3 decoding. Normal maps are `RXGB`, with X in alpha, as the programs expect. A `.tga` with no `.dds` takes the slow path.

### Stencil shadows

Each shadow volume is one draw with two-sided stencil, where OpenGL's single-sided path needs two to four. The engine's front faces are the GPU's back faces, which `Dk_SetShadowStencilOps` accounts for.

### What deko3D 0.5.0 needs

Learned by the ports that run deko3D on hardware, and followed here:

- Its release library ends the process silently on an error. Test builds link the debug library (`Q4NX_DEKO3D_DEBUG`), whose callback writes the error to the log.
- Images need `Dk_FixTileSize`, or BC textures 24 to 32 pixels tall corrupt themselves.
- A command can write past what it reserved, so each piece of command memory keeps a gap.
- Clears respect the scissor. Coarse depth culling (ZCULL) stays off until tested on the console: `r_dkZcull 1`.
- Depth-only draws bind an empty fragment program instead of none.

## Threads and cores

| Core | Runs |
| --- | --- |
| 0 | the game and the render front end and back end |
| 1 | free, for the back end's own thread |
| 2 | async tick, sound, every library thread |
| 3 | the system |

`pthread_create` is wrapped so threads started inside libraries land on their core too.

## Memory

| What | Budget |
| --- | --- |
| Images | up to 1.5 GB of GPU memory |
| Static vertexes | up to 256 MB, oldest purged first |
| Frame vertexes | 24 MB per frame in flight |
| Everything else | what is left of about 3.2 GB, through newlib's `malloc` |

The album's applet mode leaves far too little, so the game says so at start and exits. It must start from a forwarder or a title.

## Audio

devkitPro's OpenAL Soft calls seven of SDL2's audio functions. SDL2 itself would pull in its video driver and all of Mesa, so `switch_audio.cpp` implements those seven over the system's `audout` service. Input goes straight to libnx's HID too, so SDL2 and SDL3 are not in the build.

## Input

The Switch's buttons become the same keys openQ4 uses for any gamepad, by position: B is the south button, A the east one. So its bindings, weapon wheel, quick chat, menu navigation and prompts work as they are. With the Nintendo prompt family, menus confirm with A and go back with B.
