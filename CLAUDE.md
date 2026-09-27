# FreeSCI — Claude Code Notes

FreeSCI is a Sierra SCI game interpreter (circa 2007), ported to SDL2 with a CMake build system. The original codebase used SDL1 and Autotools.

## ⚠️ Two Pico targets — check which one you are reasoning about

All Pico work is on **`master`** (the old `pico-wip-render-debug` and `pico-pimoroni-mapped-psram` branches are
merged and deleted). `origin` = upstream `wjp/freesci-archive`, `fork` = `chneeb/freesci`.

- **PicoCalc / PIO PSRAM** (the default Pico build): **SRAM is the binding constraint.** The heap ceiling is
  478,288 B raw (2026-09-27: +16,352 from moving const tables out of `.data`, −4,608 for streaming all
  decompression methods; `size` reports `.data` as text, so the `.bss` baseline does not show it), the failure mode is *fragmentation* (contiguity, not total free bytes), and every OOM/arena/scratch
  lesson in `docs/history/pico-memory-oom.md` applies here.
- **Pimoroni Pico Plus 2 / memory-MAPPED PSRAM** (`-DPICO_PSRAM_MAPPED=ON`): engine allocations default to a
  6 MB PSRAM heap, SRAM sits ~163 KB with `chunks=1`, and the ceiling/fragmentation problems **do not apply**.
  It has a DIFFERENT memory rule and the desktop render model — see "Pimoroni target" below **before** applying
  any PIO memory or render conclusion to it.

Every mapped-only change is behind `PICO_PSRAM_MAPPED`; the PIO build must stay unchanged by mapped work —
a standing requirement, not a nicety.

`pico-4bpp-packing` (pushed to `fork`) holds the abandoned 4bpp visual-buffer experiment; master keeps
`PICO_DITHER_D16` and the offline harnesses (`tests/picodiff`, `tests/picodiff/visdiff`, `tests/drvdiff`,
`tests/decompdiff`, `tests/d16check.c`, `tests/ditherpreview.c`, `tests/viewdump.c`, `tests/celblit.c`,
`tests/picbg.c`).

## History files

This file holds the current state and the standing rules. Investigation logs, retracted theories and reverted
experiments were moved **verbatim** to `docs/history/` — read the relevant one before re-opening a topic:

| file | covers |
|---|---|
| `docs/history/pico-memory-oom.md` | PSRAM offload strategy, every OOM/restore/arena-ratchet/fragmentation investigation, B-1/B-1.2/B-1.3 scratches, leaks, "aspb" corruption, `.bss` recovery, malloc_trim / GC-on-OOM / GC-pool dead ends |
| `docs/history/pico-render.md` | PIO single-buffer render model: picviews, `color_key`, Pestulon overlay, static-view priority bake, composed surface (phases 1–2c), dialog bleed, Two Guys panels, Colonel's fingerprints, KQ4 Rosella fix, line tracer, pic-open flash, `old_screen` |
| `docs/history/pico-sound.md` | PWM/OPL2 sound: poll-starvation fix, feep (idle duty), song cap, PSRAM songs, buffer sizing, KQ4 limit, XIP-RAM, sound shed, streaming decompress |
| `docs/history/pico-parser-vocab.md` | Text parser re-enable, visual-borrow during parse, GNF heap floor, vocab packing, the `said.y` wordset fix |
| `docs/history/pico-engine-fixes.md` | Engine correctness fixes (VM stack, selector/kernel names, GC interval, cursor path, OOM report), SCI1/VGA rejection |
| `docs/history/pico-pimoroni-mapped.md` | Mapped-PSRAM bring-up, memory model, measurements, render model, options |
| `docs/history/pico-clock.md` | 396 MHz bring-up (flash timing, vreg, PSRAM phase, clk_peri), 252 MHz failures, SD speed, 16-bit LCD |
| `docs/history/pico-build-flags.md` | Default PIO build rationale, flag cleanup, chooser toggles, log-noise gating |
| `docs/history/pico-4bpp-attempt.md` | The abandoned 4bpp/D16 packing work and the harness techniques it produced |
| `docs/history/pico-misc.md` | Old branch/merge status, LCD loading-progress idea, RP2040 note, parked next-steps list |

`PICO_SQ3_SRAM_CEILING_ASSESSMENT.md` (repo root) is the Codex SRAM-ceiling assessment referenced in the memory history.

**Forward plan:** `docs/pico-memory-model-plan.md` — PIO memory-model refactor (fixed pools, Sierra-style
purgeable hunk, VM accessor) derived from the pico-286 comparison. Read it before starting new OOM/fragmentation work.

## Build

```bash
cmake -B build -DPLATFORM=desktop
cmake --build build -j$(nproc)
# Binary: build/src/freesci
```

Requires: `libsdl2-dev`

## Run

```bash
./build/src/freesci --gamedir ~/Downloads/quest/sq3 --graphics sdl
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

### Pico build — Pimoroni Pico Plus 2 (memory-mapped PSRAM)

The second target (see "Pimoroni target" below). Same PicoCalc hardware; only the
MCU board differs, and it drops into the same socket.

```bash
cmake -B build-pimoroni \
  -DPLATFORM=pico \
  -DPICO_SDK_PATH=~/Source/pico-sdk \
  -DPICO_PSRAM_MAPPED=ON
cmake --build build-pimoroni -j$(nproc)
# Flash build-pimoroni/src/freesci.uf2 to the Pimoroni board in the PicoCalc
```

`PICO_BOARD` stays `pico2` (set automatically) even though the board is RP2350B — see `docs/history/pico-pimoroni-mapped.md`.
Everything else defaults correctly; the mapped-only options (`PICO_PSRAM_SCRIPTS`, `PICO_STATIC_VISUAL`,
`PICO_WORKING_PRIORITY`) are all ON and each is an A/B switch.

**After ANY shared-file change, rebuild the PIO target and check its `.bss` is unchanged** — that is the
guarantee that the PicoCalc build is untouched. Current default PIO baseline: **`.bss` 29,952** (sound ON, `PICO_STREAM_METHODS=7`,
measured 2026-09-27; it was 25,344 with methods=1; `-DPICO_PWM_AUDIO=OFF` gives 17,608). Older figures in `docs/history/` (17,280 / 17,284 /
17,608) are from earlier configs, not regressions:
```bash
cmake --build build-pico -j$(nproc) && arm-none-eabi-size build-pico/src/freesci.elf
```

## Diagnostic probe toggles

The leftover instrumentation from the SQ3 bring-up is kept in the tree for debugging OTHER games,
gated behind compile-time CMake `option()`s so default builds are clean. They are **top-level**
(not Pico-scoped) so the desktop mirror probes compile on desktop too; Pico-only probes also require
`HAVE_PICO` in their own `#if`. Default OFF for clean builds, **except `FSCI_PROBE_STR`** (default ON,
kept as cheap standing insurance — it was the canary for the now-CLOSED "aspb" heap-corruption family
(2026-06-23); left ON only to catch any recurrence, no longer guarding an open bug).

| Option (default) | Define | Probes gated | Where |
|---|---|---|---|
| `FSCI_PROBE_STR` (**ON**) | `FSCI_PROBE_STR` | `[strprobe]` — SCI string kernels writing past the dest buffer's real size; `kFormat` overflow check in `CHECK_OVERFLOW1` | `kstring.c` |
| `FSCI_PROBE_GFX` (OFF) | `FSCI_PROBE_GFX` | `[pcol]`/`[ctl]` (priority/control decode), `[ovl]` (overlay base-restore/composite: `restore_base`, base PSRAM `vaddr`, visual-sum `delta`), `[oc]` (onControl scans), `[pblit]` (occlusion), `[pstat]`/`[pbuf]`/`[pupd]` (driver static-buffer swap / cel-buffer target / flush+BACK-restore rects) + desktop mirrors `[dpcol]`/`[dpblit]` (env `FREESCI_PRIPROBE=1`) | `sci_resmgr.c`, `kgraphics.c`, `pico_driver.c`, `operations.c`, `gfx_support.c` |
| `FSCI_PROBE_MEM` (OFF) | `FSCI_PROBE_MEM` | `[mem] BREAKDOWN`/`PXM`/`room enter`/`room ready` lines; desktop `desktop_mem_probe` (env `FREESCI_MEMPROBE=1`) | `kgraphics.c`, `operations.c` |
| `FSCI_PROBE_MEM_CENSUS` (OFF) | `FSCI_PROBE_MEM_CENSUS` | `[mem] CENSUS`/`SITES` + the `--wrap` malloc histogram & call-site tagger (~27.6KB `.bss`). **Implies `FSCI_PROBE_MEM`** (the dump prints inside the breakdown). | `kgraphics.c`, `pico_mem_census.c` |
| `FSCI_PROBE_PARSER` (OFF) | `FSCI_PROBE_PARSER` | `[gnf]` per-command GNF-rebuild transient byte size | `kstring.c` |
| `FSCI_PROBE_ARENA` (OFF) | `FSCI_PROBE_ARENA` | `[arenagrow]` — names the allocation that forces an sbrk grow (`sci_*` sites plus the raw decode/restore sites via `PICO_ARENA_PROBE_RAW`) | `sci_memory.c`, `sci_resmgr.c`, `operations.c`, `savegame.c` |
| `FSCI_PROBE_PERF` (OFF) | `FSCI_PROBE_PERF` | `[perf]` per-room pic-decode time. Built for the XIP-RAM comparison; kept as a reusable decode timer | `operations.c`, `pico_time.c` |
| `FSCI_PROBE_SND` (OFF) | `FSCI_PROBE_SND` | `[snd] span/polls/produced/consumed/underrun/ring \| seq` — audio production vs consumption per interval. **This is what localised the starved sound poll**; for a timing bug, measure the two rates before reasoning about the code | `pico_pwm.c`, `pwm_synth.c`, `polled.c` |
**CMAKE CACHE TRAP -- a changed `option()` DEFAULT never reaches an existing build dir.** Found the hard
way (2026-09-12): `build-pico` still had `PICO_STATIC_COMPOSED=OFF` long after that option became default-ON,
because the cache was created before the change. Flashing it would have silently lost every PQ2 fix
(dialogs, glovebox, card pickup, overlay) with no build-time signal -- the same class as the leftover-probe
flags below, but harder to spot because nothing is printed. **Before trusting any build dir, check the
options you care about, not just the probes:**

```bash
grep -E '^(PICO_|FSCI_)[A-Z_]*:BOOL' build-pico/CMakeCache.txt | sort
```

and when a default changes, `rm -rf` the dir and re-configure rather than rebuilding in place. The canonical
PIO shipping config (fresh configure, 2026-09-26): `PICO_STATIC_COMPOSED`, `PICO_STATIC_VIEW_PRIORITY`,
`PICO_CONTROL_MAP`, `PICO_PACK_VOCAB`, `PICO_REBOOT_BETWEEN_GAMES` ON; sound cluster ON (`PICO_PWM_AUDIO`,
`PICO_SND_RATE=11025`, `PICO_PSRAM_SONGS`, `PICO_STREAM_DECOMPRESS` with `PICO_STREAM_METHODS=7`, `PICO_SONG_MAX_BYTES=65536`); 133 MHz
(`PICO_SYS_CLOCK_MHZ=396` is opt-in — it works but costs battery; it drags `PICO_PSRAM_SM_MHZ` to 198 by
itself); SD 30000, LCD 25000; probes off except `FSCI_PROBE_STR`; `.bss` 29,952.

The game chooser offers per-launch toggles, so many A/Bs need no rebuild: **`[S]`** sound (off = `-q`),
**`[C]`** composed surface, **`[V]`** static-view priority (PIO only; Colonel's Bequest needs `[V]` off to show
its copy-protection fingerprints). The launch log records the combination.

**Always-on Pico timing (NOT probes -- one line each, negligible cost, no flag needed):**

| line | where | what it tells you |
|---|---|---|
| `[clk] sys_clk = N Hz (requested M MHz)` | `pico_main.c`, at the LAUNCH point (after the chooser, so it is reachable with uf2loader attached) | the **achieved** clock, and flags a silent `set_sys_clock_khz` fallback explicitly. **Trust this, not CMakeCache** -- see the build-system trap under "Overclocking". |
| `[clk] SD SPI = N kHz` | same | which SD rate was compiled in |
| `[perf] resource load: N ms (M resources)` | `main.c`, after the resmgr comes up | the startup SD scan + resource-map parse. **This is the part of loading the CORE CLOCK CANNOT speed up** (it is SD-clock bound), so it is the number to watch when changing `PICO_SD_SPI_KHZ`. Pairs with `[perf] pic N decode` (`FSCI_PROBE_PERF`), which is the CPU/PSRAM-bound half. |

| `FSCI_PROBE_FPS` (OFF) | `FSCI_PROBE_FPS` | `[fps]` frames + worst frame gap per second. Works WITHOUT sound, so a target's frame rate (== the sound poll rate, which sets the minimum `PICO_SND_BUF_FRAMES`) can be measured before deciding whether audio fits | `pico_driver.c` |

Notes:
- **Census file is always compiled**: `pico_mem_census.c` owns the `__wrap_*` symbols (top-level CMake
  defines `pico_malloc` + `-Wl,--wrap=*`), so even with the census OFF it provides thin pass-through
  wrappers (preserving the `<fn> N failed` OOM log) plus no-op `census_site_register`/`census_dump_sites`
  stubs — only the 27.6KB of bookkeeping arrays drop. Turning census OFF is what gives a true SRAM
  headroom reading.
- `PICO_VOCAB_PROBE` (the throwaway boot-time `[vocab]` vocab-cost measurement, `game.c`/`grammar.c`) is
  **separate** — it has its own `option()` in the Pico block and is not folded into `FSCI_PROBE_PARSER`.
- **Memory-test build** (per-room breakdown + leak histogram, e.g. for save/restore headroom checks):
  `cmake -B build-pico -DPLATFORM=pico -DPICO_SDK_PATH=~/Source/pico-sdk -DPICO_BOARD=pico2 -DFSCI_PROBE_MEM_CENSUS=ON`
  (census auto-enables mem). Watch the `[mem] SITES256:` line for `kscripts.c:212` (the known
  clone-`variables` leak) climbing across same-room restores.

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

Init: `psram_spi_init_clkdiv(pio1, ...)` in `pico_main.c`; the clkdiv is derived from the ACHIEVED `clk_sys`
and `PICO_PSRAM_SM_MHZ` and printed as `[psram] PIO clkdiv ...`.  
A smoke test (write 8 bytes, read back) runs on boot; prints `[psram] OK` or halts with a display message.


### PSRAM — how PIO uses it (summary; details in `docs/history/pico-memory-oom.md`)

- Decoded pic **visual / priority / control** maps and **view cel** `index_data` are offloaded to PSRAM right
  after decode; `pico_blit_indexed` and the bitmask scans read them back row-by-row. Priority and control maps
  are **nibble-packed**.
- `psram_alloc` is a **bump allocator rewound to 0 by `psram_reset()` on every room change**
  (`gfxr_free_all_pics`). Anything that must survive a room change needs a **fixed slot outside the arena**
  (the `PICO_PARSE_SCRATCH_ADDR` 0x700000 pattern; PSRAM songs use slots at 0x710000).
- `psram_alloc` never signals failure and **offset 0 is a valid address** — never test `if (!addr)`.
- `script_t.buf` can NOT go to PSRAM on PIO: it is hot read-write VM memory (bytecode + object vars).
- Permanent SRAM decode scratches, allocated once from the pristine boot heap and never freed:
  `g_pico_priority_scratch` (32 KB, also borrowed for view-cel decode) and `g_pico_decompress_scratch` (16 KB,
  pic/view only). They exist because a *contiguous* per-decode malloc fails on a fragmented heap.
- `visual[0]` stays resident across restore; the chooser reboots between games (`PICO_REBOOT_BETWEEN_GAMES`)
  so each game starts on a cold heap.

### Pimoroni target — summary (details in `docs/history/pico-pimoroni-mapped.md`)

RP2350B with 8 MB PSRAM mapped at `0x11000000` via QMI CS1 (init = the frank-snes sequence: `AUTO_CS1N`,
`NOPUSH`, no ID read). Memory model is **inverted**: `sci_malloc` defaults to the PSRAM heap
(`psram_heap.c`, `[2MB,8MB)`; `[0,2MB)` stays the per-room bump arena) and hot buffers opt back into SRAM with
`sci_malloc_sram()`. Freed SRAM buys the desktop render model: `PICO_WORKING_PRIORITY` (real per-frame priority
maps) and `PICO_STATIC_VISUAL` (a `visual[2]` analogue) — this is why PQ2's dialog bleed, door over-occlusion and
glovebox items are all correct here. Options `PICO_PSRAM_SCRIPTS`/`PICO_STATIC_VISUAL`/`PICO_WORKING_PRIORITY`
default ON, each an A/B. `PICO_STATIC_COMPOSED` and the `[C]`/`[V]` toggles do not apply on this target. Runs at
252 MHz by default (the QMI PSRAM divisor re-derives from `clk_sys`). Sound works at 22050 Hz. The memory
invariants are listed under "standing invariants" below.

### SD card game selection
Games must be in subdirectories under `0:/freesci/` on the SD card (e.g. `0:/freesci/sq3/`).
The chooser scans that directory, presents a scrollable list via the ILI9488 display,
and navigates with the I2C keyboard (UP/DOWN/ENTER/ESC). Behaviour is identical to tiny_agi's
`show_dir_chooser()` — only the root path changed from `0:/agi` to `0:/freesci`.


### Debugging Pico offline (disassembler + desktop repro)

Two fast ways to debug Pico issues without the slow flash cycle:

1. **Build FreeSCI's script disassembler** (`src/tools/scidisasm.c`) against the desktop
   static libs and run it in a game dir to read SCI0 bytecode at the exact `off=` values
   the on-device `[pc]`/`[kcall]` probes report:
   ```bash
   LIBS=$(find build -name '*.a' | tr '\n' ' ')
   gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
     -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
     -o /tmp/scidisasm src/tools/scidisasm.c \
     -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
   cd ~/Downloads/quest/sq3 && /tmp/scidisasm   # disassembles all scripts (segfaults mid-batch)
   ```
   The stock tool segfaults partway through a full batch; to reliably get one script, patch
   `main()` to call `disassemble_script(&d, N, 1/2)` for a single N and run each in its own
   process. SQ3 landmarks: script 994 = `Game` (`play` @0x150, main loop 0x182–0x198),
   script 0 = `SQ3` (`doit` @0x277, per-frame `HaveMouse` @0x294 = cursor logic, harmless),
   script 996 = `User` (`doit` @0x4e calls `GetEvent` @0x77 only when `global_55==0`).

2. **Reproduce the no-mouse path on desktop** with `--disable-mouse` (sets
   `have_mouse_flag=0`, so `kHaveMouse` returns 0 just like Pico):
   ```bash
   ./build/src/freesci --gamedir ~/Downloads/quest/sq3 --graphics sdl --disable-mouse --run
   ```

3. **Enable engine debug flags on the *device* via a config file — no firmware change.** Drop a
   file named `freesci.cfg` at the **SD card root** (`0:/freesci.cfg`, NOT inside `0:/freesci/`)
   with one line, e.g. the parser/Said trace:
   ```
   debug_mode = pS
   ```
   It works because on Pico `sci_get_homedir()` is NULL (`tools.c`) and the hardcoded argv has no
   `-f`, so `config_init` (`config.l`) falls back to the relative DOS path `freesci.cfg`; FatFS
   mounts drive 0 and never chdirs, so cwd at config-init is the SD root → resolves to
   `0:/freesci.cfg`. A header-less option lands in `conf[0]`, which is exactly what `active_conf`
   falls back to (game_name is NULL on Pico → `find_config` returns 0, `main.c`), and
   `set_debug_mode(gamestate, 1, "pS")` fires at `main.c`. Confirm it loaded: pico.log shows
   `Reading configuration...` (vs `No configuration file found; using defaults.`). `p` = bit 10
   (`SCIkPARSER`), `S` = bit 12 (Said specs); areas table at `scriptdebug.c`. The trace comes over
   serial: `kSaid` prints `Said block: <spec>` per call and `Match.` when one matches (`kstring.c`).
   **Desktop equivalent:** same line in `~/.freesci/config`.

   The disassembler (item 1) is now persisted as `src/tools/scidisasm_safe.c` — a hardened copy
   that NEVER writes the game dir (output goes to `$DISASM_OUT`, default `/tmp/sq3_disasm`; env
   `DISASM_SCRIPT=N` limits to one script). Build/run instructions are in its header comment.


## Pico — standing invariants (do NOT undo; the "why" is in the history files)

Engine / VM (`pico-engine-fixes.md`):
- `VM_STACK_SIZE` stays **0x1000**. Shrinking it (0x400) overflowed on SQ3 room 2 recursion → HardFault.
- `selector_names` and `kernel_names` stay **resident** (freeing them silently drops `PUT_SEL32` writes /
  breaks `has_kernel_function`).
- `GC_INTERVAL` is **2048** on Pico (clone-table growth OOM at larger values; 4096 tried, unattributable).
- **Never run `run_gc` from inside an allocation** — `pico_reclaim_heap` is LRU-flush-only (it HardFaulted
  walking a half-restored state).
- `said.y` and the generated `said.c` are edited **in lockstep** (also true for the lazy `said_tree` alloc).
- A missing song must behave like NOSOUND in `ksound.c` — a NULL iterator reaching `sfx_add_song` opens the SCI
  console. Shared code, not Pico-gated.
- SCI1/VGA is **rejected with a legible LCD halt** in `gfxop_new_pic`; Pico is SCI0-only.

Memory (`pico-memory-oom.md`):
- OOMs on Pico must be **legible** (`pico_oom_report` → LCD), never a HardFault. `sci_malloc` halts; raw
  `malloc` is used deliberately where a caller has a real recovery path (control map, deferred visual, song
  data via `pico_sram_alloc_soft`).
- Judge viability on a **clean build** — diagnostic probes cost ~26 KB of heap ceiling and manufacture OOMs.
- `malloc_trim` only releases the **top** chunk; the arena floor is set by the highest-addressed survivor.
- Before freeing anything on the restore path, confirm which function a `savegame.c:NNNN` line really sits in
  (the CFSML file has many near-identical `buf` readers; `script_t.buf` is never serialized).
- Any Pico packed-buffer drawing primitive must trace the **same pixels** as its desktop counterpart (the
  `ctl_draw_line` Bresenham-vs-midpoint-DDA divergence opened a flood-fill leak).
- No stack frame may approach the **8 KB** main stack (`decrypt1`'s 16 KB frame collided with the heap).
- Adding a field to a FreeSCI widget struct: `_gfxw_new_widget` does **not** memset — initialise it by hand.
- When adding a path that frees a shared buffer, copy the guards of the existing free sites
  (`priority_is_scratch`, `PICO_IS_DECOMPRESS_SCRATCH`).

Pimoroni / mapped (`pico-pimoroni-mapped.md`):
- `free`/`realloc` route by **ownership** (`psram_heap_owns`) at the single `--wrap` chokepoint in
  `pico_mem_census.c`, never at call sites — lots of code frees `sci_malloc` memory with raw `free()`.
- **`malloc_usable_size` must never see a PSRAM pointer.**
- Per-pixel buffers (`visual[0]`, static visual, priority maps, pixmap data, `drv->state`) and DMA targets
  (compressed-input buffers) stay in SRAM via `sci_malloc_sram()`.
- The `psram_heap.c` next-fit rover must always point at a valid block start (re-point it on every absorb).

## Pico — ruled out, do not retry cold

| idea | why (see history file) |
|---|---|
| Read-only PSRAM script cache | `script_t.buf` is hot RW VM memory (memory-oom) |
| `malloc_trim` per room / padded | returns bytes the next peak needs, or is a no-op (memory-oom) |
| GC on OOM | faults walking inconsistent state (memory-oom) |
| Persistent GC hashmap / dirty-rect / worklist pools | net-negative: raises the restore peak (memory-oom) |
| Compressed-INPUT permanent scratch | +20 KB arena, net-negative (memory-oom) |
| Codex "free CFSML script bufs" | premise false, HardFaulted (memory-oom) |
| XIP cache as RAM | disables flash icache: decode 6.5–11× slower (sound) |
| Port-clip / ambient-clip the static draw | fixes PQ2, breaks SQ3 — static draw is often the only one painting (render) |
| Persisting static-view **colour** (`PICO_STATIC_VIEW_BAKE`, deleted) | four ghost/overdraw regressions (render) |
| Save-unders in SRAM | `old_screen` alone is 60,800 B per transition → OOM (pimoroni-mapped) |
| 4bpp visual buffer | control-pass aux needs 8 bpp; peak gets worse (4bpp-attempt) |
| Heuristic sound shedding | no free-heap floor separates "about to die" from normal; use `[S]` (sound) |
| PWM carrier-frequency theory of the feep | disproved; it was the idle duty cycle (sound) |
| 252 MHz by rescaling only the PSRAM divisor | wrong knob — flash timing/vreg/clk_peri were missing (clock) |

## Pico — open issues

- **PIO memory model** — the fragmentation wall behind most OOMs below is structural (general `malloc`, pinned
  big blocks). Plan and status: `docs/pico-memory-model-plan.md`; step 0 (census: movable vs pinned share) not run. (memory)
- **PIO dialog bleed / SQ3 door not closing** — composed surface (2c) fixed PQ2; the SQ3 door is baked once and
  never redrawn, which neither invalidation rule distinguishes from stale. Next idea: mirror save-under
  restores into composed. (render)
- **SQ3 intro Two Guys panels persist as bands** — accepted; caused by `PICO_STATIC_VIEW_PRIORITY`, real fix is a
  transient working priority map (no SRAM for it on PIO). (render)
- **SQ3 "Pirates of Pestulon" intermittently missing** — overlay path fragility; capture `[ovl]` on a failing
  run. Untried fix: give `gfxop_add_to_pic` the `visual[0]` borrow. (render)
- **Colonel's Bequest** — dialog fills transparent + sticky corners; fingerprints need `[V]` off; hits true
  exhaustion (use `[S]` off). (render)
- **`old_screen` transition garbage** — needs a fixed PSRAM slot outside the bump arena. (render)
- **KQ4 with sound** now runs (2026-09-27, one clean-build session after the 16 KB const-table move: no
  allocation failures, music played) -- margin unknown, re-check if anything grows the heap. **SQ3 savegame
  load with sound ON** OOMs (not re-tested since the const move). (sound)
- **After a failed game, no further game starts** until power cycle — undiagnosed. (sound)
- **Dropped notes** — upstream has no OPL voice stealing (`opl2.c`). (sound)
- **Runtime actor-to-actor control writes** are a no-op (`state->control_map` NULL). (misc)
- **396 MHz on PIO**: soak it before trusting it long-term; it is opt-in (`-DPICO_SYS_CLOCK_MHZ=396`). (clock)

## Pico — method lessons (these cost real device cycles)

- **For a timing bug, measure the two rates before reasoning about the code** (`FSCI_PROBE_SND` found the
  starved poll in one run after four reasoned hypotheses failed).
- **A symptom present before the suspected subsystem can run eliminates that subsystem** ("the feep starts at the
  chooser").
- **Bisect a suspected flag instead of reasoning** — one flash beat three rounds of code reading.
- **Classify mismatches, don't count them** (neighbour-pixel / parity classification found hidden writers).
- **A harness only proves the paths its scene walks**; when plumbing traces identical but output differs,
  suspect the harness model first.
- **Ask which resources actually load in the shipping config before building anything.**
- **Check for the error line before building a theory on an allocation failure** — recovery paths are silent.
- **Measure coverage, not excess** — an "excess pixels" metric cannot tell harmful overdraw from the only draw.
- **CMake: a dependent default must come after what it depends on**, keep `set()` and its
  `add_compile_definitions` adjacent, and after any default change `rm -rf` the build dir and read
  `CMakeCache.txt` — "it built" was true in every broken case.

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
