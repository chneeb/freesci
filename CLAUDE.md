# FreeSCI — Claude Code Notes

FreeSCI is a Sierra SCI game interpreter (circa 2007), ported to SDL2 with a CMake build system. The original codebase used SDL1 and Autotools.

## Build

```bash
cmake -B build -DPLATFORM=desktop
cmake --build build -j$(nproc)
# Binary: build/src/freesci
```

Requires: `libsdl2-dev`

## Run

```bash
./build/src/freesci --gamedir ~/Downloads/sq3 --graphics sdl
```

## Platform variable

`-DPLATFORM=desktop` (default) builds with SDL2. `-DPLATFORM=pico` stubs in a pico-sdk path for a future Raspberry Pi Pico port (not yet functional).

## Key CMake decisions

- `HAVE_CONFIG_H=1` must be set as a **compiler flag** (not just inside `config.h`) because `scitypes.h` guards its include with `#ifdef HAVE_CONFIG_H` before config.h is ever included — a chicken-and-egg problem.
- `X_DISPLAY_MISSING=1` excludes the Xlib driver from `gfx_drivers.c`.
- `-fgnu89-inline` is required: the old code uses non-static `inline` (GNU C89 style) that modern GCC treats as C99 inline without external linkage, causing linker errors.
- `-w` silences the many old-code warnings.
- `src/config.c`, `src/engine/savegame.c`, and `src/engine/said.c` are pre-generated (flex/bison output) — no flex/bison dependency needed.
- `src/gfx/alpha_mvi_crossblit.c` is compiled twice as OBJECT libraries with different `FUNCT_NAME`/`PRIORITY` defines (see `src/gfx/CMakeLists.txt`).
- `src/config/libsciconfig.a` is intentionally NOT linked into the `freesci` binary.

## SDL2 port — critical notes

### Alpha convention (`GFX_MODE_FLAG_REVERSE_ALPHA`)
The FreeSCI gfx pipeline internally treats alpha=0 as opaque and alpha=255 as transparent. SDL2 BLEND mode is the opposite. The fix is `GFX_MODE_FLAG_REVERSE_ALPHA` in the `gfx_new_mode()` call in `sdl_init_specific()` (`src/gfx/drivers/sdl_driver.c`). Without this flag, all pixmap pixels have alpha=0 and are invisible under SDL2 BLEND mode — backgrounds render black, sprites are transparent, only direct `SDL_FillRect` draws (dialog box fills) appear.

### Rendering pipeline
`S->visual[0..2]` are software `SDL_Surface*` compositing buffers. On each front-buffer update, `visual[0]` is blitted to `S->primary`, then uploaded to `S->screen_texture` via `SDL_UpdateTexture`, and presented with `SDL_RenderCopy` + `SDL_RenderPresent`.

### SDL1 → SDL2 changes made
- `SDL_SetVideoMode` → `SDL_CreateWindow` + `SDL_CreateRenderer` + `SDL_CreateTexture`
- `SDL_UpdateRect` → `SDL_UpdateTexture` + `SDL_RenderCopy` + `SDL_RenderPresent`
- `SDL_SetColors` → `SDL_SetPaletteColors`
- `SDL_SetAlpha(SDL_SRCALPHA)` → `SDL_SetSurfaceBlendMode(SDL_BLENDMODE_BLEND)`
- `SDL_WM_SetCaption` → `SDL_SetWindowTitle`
- `SDL_EnableUNICODE` → `SDL_StartTextInput()`
- `SDL_EnableKeyRepeat` → removed (SDL2 auto-repeats)
- `SDL_VIDEOEXPOSE` → `SDL_WINDOWEVENT`
- `SDL_EVENTMASK(SDL_MOUSEMOTION)` → `SDL_PeepEvents(..., SDL_MOUSEMOTION, SDL_MOUSEMOTION)`
- `SDLKey` → `SDL_Keycode`, `SDLK_KP0-9` → `SDLK_KP_0-9`, `SDLK_LMETA/RMETA` → `SDLK_LGUI/RGUI`, `SDLK_SCROLLOCK` → `SDLK_SCROLLLOCK`, `SDLK_NUMLOCK` → `SDLK_NUMLOCKCLEAR`
- `keysym.unicode` → `(skey > 0 && skey < 128) ? skey : 0`
- `SDL_INIT_NOPARACHUTE` → removed

## Known pre-existing engine warnings (not regressions)
- `kNOP: Kernel function 0x71 invoked: unmapped` — SCI0 quirk
- `Could not map 'vol'/'pri'/etc. to any selector` — selector mapping for this game version
- `VM: Attempt to use invalid param variable` — SCI script issue in SQ3
- `Looking up song handle failed` — SCI sound engine edge case
