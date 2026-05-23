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

`-DPLATFORM=desktop` (default) builds with SDL2.  
`-DPLATFORM=pico` targets PicoCalc (RP2350 + ILI9488 TFT + I2C keyboard).

### Pico build

```bash
cmake -B build-pico \
  -DPLATFORM=pico \
  -DPICO_SDK_PATH=~/Source/pico-sdk \
  -DPICO_BOARD=pico2
cmake --build build-pico -j$(nproc)
# Flash build-pico/src/freesci.uf2 to the PicoCalc
```

Requires: `pico-sdk`, PicoCalc hardware library (`i2ckbd` + `lcdspi`), FatFS SD SPI driver.

### Pico architecture

| File | Purpose |
|------|---------|
| `src/gfx/drivers/pico_driver.c` | GFX driver: 8bpp palette → ILI9488 SPI push |
| `src/platform/pico/pico_main.c` | Entry point: HW init → PSRAM init → SD chooser → FreeSCI |
| `src/platform/pico/pico_time.c` | `sci_gettime()` via `time_us_64()` |
| `src/platform/pico/pico_io.c` | POSIX `_open/_read/_write/_lseek/_close` over FatFS |
| `src/platform/pico/pico_sdcard.c` | SD init + `show_dir_chooser()` scanning `0:/freesci/` |
| `src/platform/pico/audio/pwm_synth.c` | PWM audio (from tiny_agi) |
| `src/platform/pico/psram/psram_spi.{c,h,pio}` | Ian Scott's rp2040-psram PIO SPI driver (vendored) |
| `src/platform/pico/psram_alloc.{h,c}` | PSRAM bump allocator (`psram_alloc/reset/store/load`) |

### Pico driver design
- `gfx_driver_pico` uses 8bpp palette mode (bytespp=1, xfact=1, yfact=1)
- `visual[0]`: one 320×200 uint8_t buffer (64KB) — back and front combined
- On `GFX_BUFFER_FRONT` update: only the dirty rect is pushed to ILI9488 via `define_region_spi` + `hw_send_spi`
- Keyboard events come from `kbd_read()` (I2C); mapped to `sci_event_t` in an 8-entry ring buffer
- No mouse support (PicoCalc has no pointing device)
- `src/main.c:main()` is renamed `freesci_main()` under `HAVE_PICO`; `pico_main.c` provides the real entry

### PSRAM — hardware

The PicoCalc has 8MB PSRAM on PIO1 (not memory-mapped). Access is ~4MB/s via DMA.

| Signal | GPIO |
|--------|------|
| CS     | 20   |
| SCK    | 21   |
| MOSI   | 2    |
| MISO   | 3    |

Init: `g_psram = psram_spi_init_clkdiv(pio1, -1, 1.0f, true)` in `pico_main.c`.  
A smoke test (write 8 bytes, read back) runs on boot; prints `[psram] OK` or halts with a display message.

### PSRAM — memory strategy

RP2350 heap is ~388KB. The SCI0 pic decoder peaks at ~320KB for one room (4×64KB maps + 64KB struct).
PSRAM is used to offload inactive bitmap data after decode, freeing SRAM for the VM.

**Implemented (on master):**

| What | Where | Saves |
|------|-------|-------|
| Ordering fix: free old room's pics before decoding new room | `gfxop_new_pic` → `gfxr_free_all_pics()` | 192KB peak |
| `visual_map->index_data` → PSRAM after decode | `sci_resmgr.c` end of `gfxr_interpreter_calculate_pic` | 64KB SRAM |
| `priority_map->index_data` → PSRAM after `_gfxop_set_pic` | `gfxop_new_pic` in `operations.c` | 64KB SRAM |
| `control_map->index_data` freed after decode | same | 64KB SRAM |
| `undithered_buffer` freed after decode | same | 64KB SRAM |
| `state->control_map = NULL` (null-guarded everywhere) | `_gfxop_init_common` | 64KB SRAM |
| `state->static_priority_map` aliased to `priority_map` | `_gfxop_init_common` | 64KB SRAM |

`pico_blit_indexed` reads `visual_map` row-by-row from PSRAM when `pxm->psram_valid == 1`.  
`gfx_pixmap_t` has `psram_addr` + `psram_valid` fields under `#ifdef HAVE_PICO`.

**Still OOM — critical missing piece:**  
`gfxr_pic_t::aux_map` is a 64KB array **embedded in the struct** (`gfx_resource.h:82`), so
`sci_malloc(sizeof(gfxr_pic_t))` alone allocates 64KB. Peak during decode is still ~448KB > 388KB.
Fix: change `byte aux_map[64000]` → `byte *aux_map`, allocate within `gfxr_draw_pic01`, free on return.
This change is already on the `pico-mem-opts` branch and must be cherry-picked to master.

**Remaining PSRAM candidates (not yet implemented), in priority order:**

1. **`aux_map` pointer fix** — must land first; without it room transitions still OOM
2. **Skip `control_map->index_data` allocation** in `gfxr_alloc_pic` (not just free after decode) — saves 64KB peak
3. **View `index_data` → PSRAM** — `pico_blit_indexed` already handles it; extend to view decode in `gfxr_draw_view0` — saves ~30–80KB of sprite data that turns over every room
4. **Resource data (scripts) → PSRAM** — ~100–200KB, requires a read cache in the VM; largest headroom gain, most invasive

### SD card game selection
Games must be in subdirectories under `0:/freesci/` on the SD card (e.g. `0:/freesci/sq3/`).
The chooser scans that directory, presents a scrollable list via the ILI9488 display,
and navigates with the I2C keyboard (UP/DOWN/ENTER/ESC). Behaviour is identical to tiny_agi's
`show_dir_chooser()` — only the root path changed from `0:/agi` to `0:/freesci`.

### Sound on Pico (TODO)
Sound is currently disabled (`--no-sound` in `pico_main.c`). To enable:
- Wire FreeSCI's OPL2 softsynth (fmopl.c) output into a PCM callback feeding `pwm_synth`
- Add a Pico PCM device driver under `src/sfx/pcm_device/pico_pwm.c`
- Remove `--no-sound` from `pico_main.c`'s argv

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
