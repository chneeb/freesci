# FreeSCI — Claude Code Notes

FreeSCI is a Sierra SCI game interpreter (circa 2007), ported to SDL2 with a CMake build system. The original codebase used SDL1 and Autotools.

## Branch / merge status

> ### ⚠️ THERE ARE TWO PICO TARGETS — check which one you are reasoning about
> - **PicoCalc / PIO PSRAM** (`pico-wip-render-debug`, the default build): SRAM is the binding constraint.
>   Everything in this file about the ~475 KB ceiling, the arena ratchet, fragmentation, permanent decode
>   scratches and graceful-degradation OOM paths applies **here**.
> - **Pimoroni Pico Plus 2 / memory-MAPPED PSRAM** (`pico-pimoroni-mapped-psram`, `-DPICO_PSRAM_MAPPED=ON`):
>   engine allocations default to an 8 MB PSRAM heap, SRAM sits ~163 KB with `chunks=1`, and the ceiling and
>   fragmentation problems **do not apply**. It also has a DIFFERENT memory rule (raw `free()` is
>   ownership-aware; `malloc_usable_size` must never see a PSRAM pointer) and the desktop render model
>   (per-frame priority maps). **See "Pimoroni Pico Plus 2" near the end of this file BEFORE applying any
>   memory or render conclusion from the sections in between.**
>
> Both targets are supported. The PIO build is byte-identical (`.bss` 17,280) — every mapped change is behind
> `PICO_PSRAM_MAPPED`, and that is a standing requirement, not a nicety.

### PicoCalc / PIO branch status (2026-06-23)

All Pico work lives on **`pico-wip-render-debug`**, currently **54 commits ahead of `master`, 0 behind** → a
clean **fast-forward** merge (no conflicts possible). Local `master` is itself 2 commits ahead of
`origin/master` (`origin` = upstream `wjp/freesci-archive`; `fork` = `chneeb/freesci`), so a merge would be
purely local; pushing is a separate decision. **Kept as a branch for now — not merged.**

**Merge-risk assessment (low on correctness, but it's a WIP debug branch):**
- **Desktop-live code is well-isolated.** The only changed code that runs on the desktop SDL build is the sound
  stack, properly gated: the OPL2 flash-table refactor is behind `FMOPL_FLASH_TABLES` (defined only on
  `-DPICO_PWM_AUDIO=ON`), so desktop falls through to the original `OPLBuildTables()` malloc path and stays
  stereo. Desktop audio behavior unchanged.
- **Shared engine changes are all net-positive bugfixes** (improve desktop too): `said.y`/`said.c` wordset-paren
  fix, `sci_view_0.c` mirrored view-RLE `yl` bound, `resource.c`/`tools.c` cwd-leak free, `seg_manager.c`
  clone-`variables` teardown free, `kernel.c` `kmem()` null guard, `hashmap.c` `sci_malloc`. Footnote:
  `FSCI_PROBE_STR` defaults ON → desktop gains `[strprobe]` diagnostic log lines (no behavior change).
- **Caveats are hygiene, not breakage:** merging enshrines diagnostic scaffolding in master
  (`pico_mem_census.c`, `scidisasm_safe.c`, `FSCI_PROBE_*` options, the large CLAUDE.md,
  `PICO_SQ3_SRAM_CEILING_ASSESSMENT.md`) and carries two still-open graphics bugs (PQ2 fade rectangle,
  Colonel's Bequest dialog boxes). Untracked clutter (`build-asan/`, `build-pico-clean/`, `asan.log`,
  `PICO_SRAM_PSRAM_REVIEW.md`) is **not** gitignored (only `build-pico` is) — won't be part of an FF merge, but
  worth a `.gitignore` line before any future commit.
- **Open verification gap:** no recent full **desktop** build + smoke-test is on record — that's the one thing
  to do before actually merging (the desktop-build check has not yet been completed this session).

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

### Pico build — Pimoroni Pico Plus 2 (memory-mapped PSRAM)

The second target (see "Pimoroni Pico Plus 2" near the end of this file). Same PicoCalc hardware; only the
MCU board differs, and it drops into the same socket.

```bash
cmake -B build-pimoroni \
  -DPLATFORM=pico \
  -DPICO_SDK_PATH=~/Source/pico-sdk \
  -DPICO_PSRAM_MAPPED=ON
cmake --build build-pimoroni -j$(nproc)
# Flash build-pimoroni/src/freesci.uf2 to the Pimoroni board in the PicoCalc
```

`PICO_BOARD` stays `pico2` (set automatically) even though the board is RP2350B — see the Pimoroni section.
Everything else defaults correctly; the mapped-only options (`PICO_PSRAM_SCRIPTS`, `PICO_STATIC_VISUAL`,
`PICO_WORKING_PRIORITY`) are all ON and each is an A/B switch.

**After ANY shared-file change, rebuild the PIO target and check its `.bss` is still 17,280** — that is the
guarantee that the PicoCalc build is untouched:
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
| `control_map->index_data` → PSRAM after decode (graceful OOM skip) | `sci_resmgr.c` Pass 2 | 32KB SRAM |
| View cel `index_data` → PSRAM after decode | `sci_resmgr.c` end of `gfxr_interpreter_get_view` | ~30–80KB/room |
| `undithered_buffer` freed after decode | same | 64KB SRAM |
| `state->control_map = NULL` (null-guarded everywhere) | `_gfxop_init_common` | 64KB SRAM |
| `state->static_priority_map` aliased to `priority_map` | `_gfxop_init_common` | 64KB SRAM |

`pico_blit_indexed` reads `visual_map` row-by-row from PSRAM when `pxm->psram_valid == 1`.  
`gfx_pixmap_t` has `psram_addr` + `psram_valid` fields under `#ifdef HAVE_PICO`.

**`aux_map` is already a heap pointer — DONE (no embedded-array bloat).**  
`gfxr_pic_t::aux_map` is `byte *aux_map` (`gfx_resource.h:82`), so `sizeof(gfxr_pic_t)` is ~100 bytes,
not 64KB. On Pico it's NULL at init (`sci_pic_0.c:258`) and **reuses the existing 64KB visual buffer**
as the flood-fill aux during the control pass (`sci_resmgr.c:233`), freed right after (`256-257`) — so
it adds zero extra peak. The old "embedded `byte aux_map[64000]`" note was stale; nothing to do here.

**Remaining PSRAM candidates (not yet implemented), in priority order:**

1. **Skip `control_map->index_data` allocation** in `gfxr_alloc_pic` (not just free after decode) — saves 64KB peak
2. ~~**View `index_data` → PSRAM**~~ — DONE (`sci_resmgr.c` `gfxr_interpreter_get_view`, all cels offloaded
   after decode; `pico_blit_indexed` reads them back row-by-row). Moved to the Implemented table above.
3. ~~**Resource data (scripts) → PSRAM**~~ — RULED OUT: `script_t.buf` is hot read-write VM working memory, not offloadable to a read-only cache (see roadmap ✗)

### SD card game selection
Games must be in subdirectories under `0:/freesci/` on the SD card (e.g. `0:/freesci/sq3/`).
The chooser scans that directory, presents a scrollable list via the ILI9488 display,
and navigates with the I2C keyboard (UP/DOWN/ENTER/ESC). Behaviour is identical to tiny_agi's
`show_dir_chooser()` — only the root path changed from `0:/agi` to `0:/freesci`.

### Sound on Pico

> ## ✅ RESOLVED on the MAPPED-PSRAM target (2026-09-05) — music plays at correct speed
>
> **The root cause was never memory and never the synth.** `pico_sfx_poll()` had exactly ONE call site,
> `pico_get_event()`, so audio production was gated on **how often the game asked for input** — which during
> an animation sequence is barely ever (measured **0.3–7 polls/sec**, and **50 seconds with zero** at startup).
> The mixer emits at most `buf_size` frames per call, so a starved poll rate starves the ring, and the PWM IRQ
> then holds `last_sample` — which **stretches AND chops** the audio at once. One cause, both recorded
> symptoms ("way too slow" + the "broken tractor" buzz).
>
> Fixed by also polling from the per-frame **front flush** (where `poll_keyboard` already lives for exactly
> this reason) and from inside **`usec_sleep`**, sliced. Device-measured: `produced` 6,144–14,336 → **~22,000**,
> `underrun` 7,000–19,700 → **0**.
>
> **This RETIRES the 2026-07-10 conclusion below** that "the bad audio is the PWM/mixer/OPL output path
> itself, which is untested/untuned" and that a future attempt must "start from tuning the PWM/mixer path".
> The path was fine — it was starved. Measured, all within budget: PWM IRQ **~1% CPU** (RAM-resident, fully
> inlined), OPL synth **33–47% CPU**, hardware consuming at exactly 22 kHz.
>
> **METHOD NOTE, because it cost several device cycles:** four successive hypotheses (buffer size, CPU-bound
> synth, XIP cache thrashing, IRQ overhead) were each reasoned from the code and each disproved by the first
> measurement. What localised it in one run was an `[snd]` probe printing **production vs consumption rates**
> (`FSCI_PROBE_SND`, default OFF). The tell was that every `produced` value was an exact multiple of
> `buf_size` — full batches, too few calls. **For a timing bug, measure the two rates before reasoning about
> the code.**
>
> **Still open even with sound working:**
> - **Dropped notes** — `ADLIB: All voices full`. Upstream FreeSCI has **no voice stealing** (literally
>   `XXX implement overflow code`, `opl2.c`), so a passage wanting >12 simultaneous voices loses one. Not
>   Pico-specific. Its `printf` is now rate-limited: it sits in the note-start path and goes over USB, so
>   unthrottled it could stall the very loop feeding the ring.
> - **SN76496 is NOT a cheaper drop-in** — it produces SILENCE on SQ3 (the resource almost certainly carries
>   no Tandy/PCjr track). Selectable via `PICO_SOFTSEQ` if ever revisited.
> - **The PicoCalc PIO target is NOT fixed by this** — see below.
>
> #### Would sound work on the PicoCalc PIO target? (analysed 2026-09-05, NOT device-tested)
>
> **The quality blocker is gone for free.** The poll fix is in shared code (`pico_driver.c` front flush +
> `usec_sleep`), so PIO inherits it. The 2026-07-10 "broken tractor" on PIO was almost certainly THIS bug,
> not an untuned DSP path — so if PIO sound can run at all, it should now run at the right speed.
>
> **The memory blocker is untouched.** The recorded PIO failure was `calloc 16384 failed` at
> `sm_allocate_stack` — the 16 KB VM value stack failing to find a **contiguous** block. That is the SRAM
> ceiling/fragmentation problem which ONLY the mapped target escaped.
>
> Measured static cost of a `-DPICO_PWM_AUDIO=ON -DPICO_SND_RATE=11025` PIO build:
> `.bss` 17,280 → 25,000, heap span **475,088 → 467,080 (−8 KB)**. Runtime resident on top is ~19 KB
> (OPL chip ~7 KB, compbuf 2×1002×4, feed + writebuf), so **~27 KB effective** against the documented
> **~26 KB clean-build margin**. It consumes essentially the whole margin, and the failure mode is
> contiguity, not total bytes.
>
> **Verdict: worth exactly one experiment, do not expect robustness.** 11025 halves the synth cost
> (33–47% → ~20% CPU) and every derived buffer, and `PICO_SND_RATE` makes it a build flag rather than a
> source edit. But do not expect it to survive long play or restore chains — the record's "sound + long
> restore chains + comfortable margin is out of reach" verdict still stands for PIO. Sound is a
> mapped-PSRAM feature.
>
> NB the buffer sizes were raised (`buf_size` 512→2048, ring 2048→8192 at 22 kHz) to survive rare polls
> BEFORE the poll fix existed. With polls now at 60 Hz a batch only needs ~367 frames, so if PIO memory is
> ever the deciding factor these can come back down — that is the cheapest ~24 KB available.

*Historical (the pre-2026-09-05 state — memory analysis still valid for the PIO target):*
Sound is currently disabled, in two layers:
- **FreeSCI engine sound:** `--no-sound` (`-q`) in `pico_main.c`'s argv → `SFX_STATE_FLAG_NOSOUND`.
- **PicoCalc PWM synth:** gated behind CMake `option(PICO_PWM_AUDIO)` (**default OFF**). When OFF,
  `audio/pwm_synth.c` is **not linked at all** (`src/platform/pico/CMakeLists.txt`) and
  `pwm_synth_init(26)` is `#ifdef PICO_PWM_AUDIO`-skipped (`pico_main.c`). This recovers the ~6.8KB
  SRAM its `strings[6924]` waveform table reserved (the synth otherwise ran an idle 22kHz IRQ
  outputting silence, since nothing ever drives a channel while engine sound is off) plus its flash +
  channel globals. `strings[]` is now `static const` (`audio/pwm_strings.h`) so even an ON build keeps
  it in flash (`.rodata`), not SRAM — it's read-only in the IRQ.

To bring sound back:
- Build with `-DPICO_PWM_AUDIO=ON` (re-links the synth + re-arms `pwm_synth_init`).
- Wire FreeSCI's OPL2 softsynth (fmopl.c) output into a PCM callback feeding `pwm_synth`
- Add a Pico PCM device driver under `src/sfx/pcm_device/pico_pwm.c`
- Remove `--no-sound` from `pico_main.c`'s argv

#### Sound stack now wired (default OFF) + the SQ3-intro sound OOM (graceful-skip DONE, music still doesn't fit)

The PCM/softseq plumbing above is now **implemented behind `PICO_PWM_AUDIO` (still default OFF)**: a Pico PCM
device (`src/sfx/pcm_device/pico_pwm.c`, polled at 60Hz from the main loop → lock-free ring → 22050Hz mono
8-bit PWM IRQ), the OPL2 softseq with **flash-resident `const` tables** (`fmopl_tables.{c,h}`,
`FMOPL_FLASH_TABLES`, ~136KB in `.rodata`, zero heap; mono = one ~7KB `FM_OPL` chip), and the CMake wiring.
**Default builds stay sound-off** — none of this links or runs unless `-DPICO_PWM_AUDIO=ON`.

**The blocker is unchanged: there is no SRAM headroom for music on SQ3.** With sound ON, the SQ3 intro OOM'd
**before the first note** at `decompress0.c:50` (`malloc 18996 failed`) — a **fragmentation OOM**: ~25KB free
but no 18996-contiguous run, arena 456416 of the ~469216 PWM-build physical ceiling (**~13KB growth room
left**). The resident sound stack (OPL chip + mixer compbuf + ring) ate the margin that the intro pic/view
decodes need.

**DONE — graceful skip for non-essential sound resources** (`decompress0.c`, `HAVE_PICO`). Per the directive
"assume it's a sound resource, don't touch pic/view": `pico_decompress_alloc` now routes `sci_sound` decodes
through **raw `malloc`** (not `sci_malloc`), so an OOM returns **NULL** instead of the fatal `pico_oom_report`
halt; a NULL-guard right after the alloc fails the decode cleanly (`SCI_STATUS_NOMALLOC` →
`SCI_ERROR_DECOMPRESSION_INSANE`), and `resource.c`'s existing decompressor-error path leaves `res->data=NULL`
so the song load returns empty and **the game keeps playing silently**. Every *other* resource type still
halts legibly (pic/view/script unchanged — they MUST fit). This makes a too-big song a non-event, but it does
NOT make music play — it just stops sound from crashing the intro.

**Levers assessed for actually fitting intro music — only one is viable, and it's unbuilt:**
*(SUPERSEDED 2026-07-10 — see the "RE-ANALYSIS" section below: three cheaper levers were missed here; the
vocab-borrow is now a last-mile helper, not the sole path. Kept for the rejection reasoning on PWM_BUF/lazy
chip/fixed buffer, which still stands.)*
- **`PICO_PWM_BUF_FRAMES` 512→256 — REJECTED.** Frees only ~2KB steady-state, and breaks audio: 256 frames =
  11.6ms produced/poll < the 16.6ms 60Hz drain → ring underrun/stutter. 512 (23.2ms/poll) is the floor.
- **Lazy OPL chip alloc — only shifts the peak.** Defers the ~7KB chip, but the intro is *where* music starts,
  so the chip is live exactly when the intro decode peaks. Helps only music-free rooms, not the intro OOM.
- **A fixed "intro-only" sound buffer — relocates the OOM, doesn't remove it.** It would have to be
  boot-allocated (you can't grab a contiguous block mid-intro on the fragmented heap — that's the OOM itself),
  so it's permanently resident; and it can't be a *reusable* scratch because song data is read every tick
  during playback (`iterator.c` reads `self->data` per tick, refcounted) — it must stay resident while
  playing. The intro is the peak, so reserving for it just moves the wall.
- **Share one buffer with vocab/grammar — fails on lifetime overlap.** Vocab is permanently resident; the GNF
  parse peak is already solved via the visual-borrow and overlaps *gameplay* music; every large buffer is
  busy during the intro.
- **Borrow the VOCAB blob to PSRAM during the intro — THE one viable path (NOT built).** The packed vocab blob
  (`g_pico_vocab_blob`, ~21419B, contiguous, > the 18996B song) is **idle during the intro** — its only
  gameplay reader is `vocab_tokenize_string` inside `kParse` (`kstring.c:325`), and you can't type a parser
  command during the intro. So mirror the `pico_borrow_visual`/`pico_return_visual` pattern: page the vocab
  blob to PSRAM for the duration of intro sound, freeing ~21KB SRAM for the song decode, then restore it
  before the first parse. **Mechanism must be borrow-to-PSRAM, NOT destroy-and-reload** (re-packing the blob
  needs a 21KB contiguous run → would re-OOM on the fragmented post-intro heap). **Catches (why it's a spike,
  not a quick edit):** the song must be **cut off / handed back before the first parse** (intro-only — it does
  nothing for in-room music, which overlaps the parser); it needs **sfx↔parser coordination** (who owns the
  blob when); it assumes a **single** large sound resource at a time; and the borrow/return `sci_malloc` on
  restore could itself halt if the heap fragmented (degrades loudly via `pico_oom_report`, never corrupts).
  Deferred — not worth the multi-file coordination for intro-only music while the port is at the ceiling.

**Bottom line: sound stays disabled.** The stack is ready to switch on (`-DPICO_PWM_AUDIO=ON`) and won't crash
the intro anymore (graceful skip), but real intro music needs the vocab-borrow spike above, which isn't built.

#### RE-ANALYSIS (2026-07-10, code-read — the "vocab-borrow is the ONLY path" conclusion above is SUPERSEDED)

A fresh read of the actual sound stack (not the CLAUDE.md summary) found **three levers the earlier analysis
missed**, two of them cheap and mechanical, which together change the recommended plan. The vocab-borrow
(above) is now **demoted from "the one viable path" to a small last-mile helper** — see lever 4. Nothing
built yet; this records the corrected plan. All line numbers verified in-tree this session.

**Corrected budget (the earlier "~10–13KB resident, ~19KB transient" was wrong on the transient):**
- **Resident with sound ON ≈ 13KB** (verified): OPL chip state ~7KB (`fmopl.c:1094-1105`, one `calloc` of
  `sizeof(FM_OPL)` + `sizeof(OPL_CH)*9`), mixer compbufs 2×512×4 = 4KB (`mixer/soft.c:95-96`) + feed buf
  ~1KB (`:180`) + writebuf ~0.5KB (`:311`), PCM ring `pcm_ring[2048]` = 2KB `.bss`
  (`audio/pwm_synth.c:15`), **plus the song data resident for the whole playback** (18,996B for the SQ3
  intro).
- **Transient at song START ≈ 38KB, NOT 19KB (finding #2 below):** the resource's decompressed copy AND the
  iterator's `memdup` copy are co-resident. The recorded intro OOM (`malloc 18996 failed`,
  `decompress0.c:50`) died at the *decompress* (step 1) before the doubling even happened — so the doubling
  was never in the earlier accounting. Contiguity, not total free, remains the binding ask.

**Finding #1 (CHEAP, verified) — ~4.7KB of the ~7KB OPL "chip state" can move to flash `const`.**
`FN_TABLE[1024]` (4096B) + `AR_TABLE[75]` + `DR_TABLE[75]` (600B) live *inside* `FM_OPL` (`fmopl.h:127-129`)
but are computed **only** in `OPL_initalize`/`init_timetables` (`fmopl.c:588-608`, `:764-768`) as pure
functions of `freqbase = ((double)clock/rate)/72` — both compile-time constants on Pico (fixed 22050Hz,
fixed clock). They are never rewritten after init (the per-slot `AR`/`DR` pointers just index into them, and
`FN_TABLE` is read-only in `OPLWriteReg`). So they can be precomputed `const` in `fmopl_tables.c` exactly
like the five big tables already are (`FMOPL_FLASH_TABLES`, `OPLOpenTable`, `fmopl.c:610-624`), dropping the
per-chip SRAM from ~7KB → **~2.3KB**. Mechanical extension of the existing flash-table pattern; low risk.
(Caveat: they'd have to move OUT of the `FM_OPL` struct to a shared const — the struct is `calloc`'d as one
block, so this is a small representation change like the `SIN_TABLE`→offsets one already done.)

**Finding #2 (CHEAP, verified) — the iterator DOUBLES every song; halve the song-start transient for free.**
`songit_new` does `it->data = sci_refcount_memdup(data, size)` (`iterator.c:1985`), and the caller
`ksound.c:116-121` (`_pick_song`/`kDoSound`) passes `song->data` from `scir_find_resource(..., lock=0)`
(`ksound.c:97,116`) — i.e. the resource copy is **still live in the LRU** when the memdup runs, so both are
resident. **Pico-gated fix:** steal `res->data` into the iterator and immediately evict the resource (the
exact evict-immediately pattern pic/view decode already uses), instead of memdup+leave-in-LRU. The iterator
is the sole reader after that point (loop-rewinds read `self->data`, never the resource;
`iterator.c:172,386,419`; teardown `sci_refcount_decref(self->data)` `:722`). Halves the ~38KB peak → ~19KB.

**Finding #3 (the NEW real lever, verified) — song playback reads are SEQUENTIAL → the song can live in
PSRAM, streamed through a small SRAM window.** Playback consumes the song strictly as
`cmd = self->data[channel->offset++]` (`iterator.c:172`) plus tiny `memcpy(buf+1, self->data+offset,
paramsleft)` of ≤ a few param bytes (`:209`) and `_parse_ticks(self->data+offset, ...)` (`:419`) — monotonic
per channel, single-digit bytes per 60Hz tick, with only occasional loop-point rewinds. On the ~4MB/s PSRAM
SPI link a 1–2KB SRAM window refilled on a boundary crossing costs ~0.5ms and rarely — imperceptible at
60Hz. So a song resource can sit in a **fixed PSRAM slot** (the `0x700000` visual-borrow scratch pattern,
`pico_driver.c:101`) with playback SRAM cost ≈ the window, not the whole song — this is what unlocks
**in-room** music (which overlaps the parser, so the vocab borrow can't help it). **Exclusion:** embedded-PCM
songs (`self->data[0] == 2`, `iterator.c:518,710`) read bulk digitized-sample data, not a byte stream — those
keep the current graceful-skip or an SRAM fallback.

**Revised ranked levers:**
| # | Lever | Effect | Risk | Effort |
|---|---|---|---|---|
| 1 | FN/AR/DR → flash `const` | −4.7KB resident/chip | low | small (`fmopl.c` + `fmopl_tables.{c,h}`) |
| 2 | Steal `res->data` + evict, drop the memdup | −~19KB song-start transient | med-low | small (`iterator.c`/`ksound.c`, `HAVE_PICO`) |
| 3 | Song → fixed PSRAM slot, streamed SRAM window | song size → ~1–2KB during playback; **enables in-room music** | medium (loop rewind across window, PCM-song fallback, refcount teardown from PSRAM) | the real spike (SCI0 `iterator.c` read path) |
| 4 | Vocab-borrow (above) — **demoted** | with #2+#3 the vocab hole is needed only for the ms of *decode*, not the whole intro → the recorded sfx↔parser-coordination problem largely evaporates | low once #3 exists | small |

**Do NOT** trade away the B-1.2 16KB decompress scratch to fund sound — it is what holds restore chains
together; sound must not regress restores.

**Recommended scope (honest, given the Codex "sound + long restore chains + comfortable margin = out of
reach" verdict):**
- **Phase A (cheap, ~1 session): levers 1+2.** Resident ~8.5KB, song-start transient ~19KB. Flash
  `-DPICO_PWM_AUDIO=ON`, measure with `[arenagrow]`/`[mem] BREAKDOWN` whether the intro song now decodes
  (graceful skip still catches misses). May already yield in-room music where the heap is calmer.
- **Phase B (the spike): lever 3 (+4 if needed).** Full music incl. intro; playback ~12KB total resident.
- Even A+B spends roughly **half** the ~26KB clean-build margin, so **sound remains best-effort alongside
  long restore chains** — declare that scope rather than chase "sound + robust arbitrary-length restores".

#### PHASE A BUILT + DEVICE-TESTED → REVERTED (2026-07-10) — sound stays disabled; two findings banked

Levers 1 and 2 were built (`FMOPL_FLASH_TABLES`/`HAVE_PICO`-gated), both builds clean, flashed with
`-DPICO_PWM_AUDIO=ON`, and **device-tested for the first time ever** (the sound stack had never actually
driven the PWM before). Result: **not viable — reverted (all code back to HEAD; sound stays OFF by default).**
Two concrete, load-bearing findings were captured before reverting:

- **FINDING (decisive) — the OPL synth output is garbled ("broken tractor"), and it is NOT the memory
  changes.** Lever 1 baked `FN_TABLE[1024]`+`AR_TABLE[75]`+`DR_TABLE[75]` into flash `.rodata` for the fixed
  Pico config (mono/22050Hz/clock 3579545), dropping ~4.7KB from the per-chip `FM_OPL` (ELF-verified: symbols
  at `0x100bxxxx`, sizes 0x1000/0x12c/0x12c = 4696B). **Those baked tables are byte-for-byte CORRECT** —
  proven by an independent re-derivation of `OPL_initalize`/`init_timetables` at 22050Hz diffed against
  `fmopl_tables.c` (`OK: … EXACTLY match the fmopl.c runtime formula`, not via the generator). So the bad
  audio is **the PWM/mixer/OPL output path itself, which is untested/untuned** (ring underrun at the 60Hz
  poll vs 22050Hz drain, sample-rate/format, or mixer immaturity) — a **separate, larger Phase-B-class job**,
  NOT lever 1. **A future sound attempt starts from "tune the PWM/mixer path," not "re-derive tables."**
- **FINDING — sound is fatal at the SRAM ceiling on game load (confirms the Codex verdict quantitatively).**
  With sound ON, a game load OOM'd: `calloc 16384 failed` at `sm_allocate_stack` (`seg_manager.c`) — the
  **VM value stack** (one of the three irreducible 16KB baselines) couldn't find a contiguous 16KB block
  (`free=80752` total but `arena=469216`, i.e. maxed + fragmented). The resident sound stack (~13KB + song
  data) ate the margin the load needs. **Phase A's ~5KB (lever 1) + evict (lever 2) cannot close this** — the
  arena still maxes and fragments during play. This is the documented "sound doesn't fit at the ceiling" wall,
  now reproduced with sound actually on.

- **Levers as built (reverted, but recorded so a Phase-B restart doesn't re-derive them):** *Lever 1* — flash
  FN/AR/DR via `tools/gen_fmopl_tables.c` `build_rate()` (mirrors the runtime math; regen per the file
  header), struct members `#ifndef FMOPL_FLASH_TABLES`'d out, init write-loops compiled out, 4 read sites
  through `OPL_{FN,AR,DR}_TABLE()` accessor macros. *Lever 2 (safe half)* — `build_iterator` (`ksound.c`,
  `HAVE_PICO`) evicts the sound resource via `scir_evict_resource_data` right after `songit_new` (guarded
  `lockers==0`), killing the steady-state duplicate copy; the `memdup` **2× transient** is unchanged (its
  removal needs the fragile refcount-ownership transfer — `res->data` is plain `sci_malloc`, tee-iterators
  incref/decref-share it, `iterator.c:616,1165,722` — do NOT attempt as a "steal the pointer" one-liner).
- **A `[pwm]` boot marker** was added to `pico_main.c` (also reverted) to confirm the sound firmware actually
  ran (`[pwm] sound-enabled firmware: launching freesci_main argc=5 (no -q)`) — it settled an initial
  wrong-uf2 upload. Keep in mind for the next attempt: the DEFAULT build (`build-pico`, no `PICO_PWM_AUDIO`)
  still passes `-q` and prints `[SFX] Sound disabled.`; only the PWM build drops `-q`.

**Bottom line unchanged:** sound stays disabled. Phase A's memory wins are real but small and only matter with
sound ON, so they were reverted with it. The two blockers for audible music are now sharply named: (1) the
PWM/mixer output path needs actual DSP tuning (garbled today), and (2) even tuned, the resident stack OOMs
game-load at the ceiling — needs Lever 3 (Phase B: PSRAM-streamed song data, the real unlock) AND likely a
"sound incompatible with restore chains" scope. Neither is a quick edit.

### Pico correctness fixes (do NOT re-apply the old RAM optimizations)

Earlier Pico work freed several vocab tables after use to save RAM. Two of those frees
were **wrong** and have been reverted — keep them resident:

- **`selector_names` must stay resident** (`game.c` `_init_vocabulary`). `write_selector()`
  bounds-checks selector ids against `selector_names_nr`; freeing them (nr=0) silently
  drops *every* kernel-side `PUT_SEL32` write (e.g. Bresenham dx/dy/b_di). ~2–3KB.
- **`kernel_names` must stay resident** (`game.c` `script_init_engine`). Freeing it left
  dangling `kfunct_table[].orig_name` (the "lati" garbage in kNOP) and made
  `has_kernel_function()` always return 0. ~1.5KB.

- **SCI0 cursor path for SQ3** (`kgraphics.c` `kSetCursor`). SQ3 (SCI0, 0.000.685) carries
  "MoveCursor" in its kernel table, so the `has_kernel_function(s,"MoveCursor")` heuristic
  wrongly flips it to the SCI1.1 cursor path, which misreads SQ3's `SetCursor(view,vis,x,y)`
  args and faults in gfxop pointer ops. Guarded out under `HAVE_PICO`.

- **Text rendering on Pico** (`operations.c` `gfxop_draw_text`, `pico_driver.c`
  `pico_blit_indexed`/`nearest_pal`). Pico leaves text `pxm->data` NULL so text routes
  through the indexed blit path (not `gfx_xlate_pixmap`, which maps via `global_index` = -1
  in SCI0 → invisible text). But `gfx_xlate_pixmap` is also what sets `pxm->xl/yl`, so they
  are set manually from index dims or `_gfxop_clip` discards every line. `pico_blit_indexed`
  uses `nearest_pal()` (RGB → closest palette slot) for non-256-color pixmaps so text gets
  its real color instead of assuming local index == EGA color.

- **Priority/control bitmask scans from PSRAM** (`operations.c` `_gfxop_scan_one_bitmask`,
  `gfxop_scan_bitmask`). Since priority/control `index_data` is offloaded to PSRAM on Pico,
  the clipped query zone is read back row-by-row. `state->control_map` is NULL on Pico, so
  the scan uses the pic's own `control_map` instead. **Caveat:** the pic's control map is only
  decoded when built with `-DPICO_CONTROL_MAP=ON` (default OFF — see roadmap #2); otherwise
  `pic->control_map->psram_valid` is 0 and the control scan returns 0 (no collision).

- **VM value stack must stay full-size** (`vm.h` `VM_STACK_SIZE 0x1000`). It was once shrunk
  to `0x400` on Pico to save SRAM; SQ3 room 2 (`Game::doit` → `eachElementDo(#check)` over the
  cast → nested `check()` sends → `Animate` → `motionCue`) shares one value stack across
  recursive `run_vm` and overflowed 1024 entries → `validate_stack_addr` NULL → PUSH trap →
  HardFault. Keep it at 0x1000; recover the 12KB via PSRAM, not by shrinking this.

- **GC runs far more often on Pico** (`vm.h` `GC_INTERVAL 2048`, desktop 32768). `kDisposeClone`
  only flags clones `OBJECT_FLAG_FREED`; their seg-manager table entries are reclaimed only by
  `run_gc()`. The clone/node/list tables (`heapmgr.h`) grow by realloc and **never shrink**, so
  a long interval let SQ3's clone table climb to ~428 slots (~18KB) until a realloc-grow could
  not find a contiguous block in the ~388KB heap → OOM in `alloc_clone_entry` (`seg_manager.c`).
  Same GC code the desktop runs; only the cadence changed. If gameplay stutters from GC, raise
  it; if it OOMs again under heavy animation, lower it. Single tunable knob.

- **OOM self-report to LCD** (`sci_memory.c` + `pico_main.c` `pico_oom_report`). The freesci
  target sets `PICO_MALLOC_PANIC=0` (`src/CMakeLists.txt`) so pico-sdk's malloc wrapper returns
  NULL on exhaustion instead of panic()ing with a USB-only message. `sci_malloc/calloc/realloc`
  then call `pico_oom_report`, which prints the failing allocation's size, free heap
  (`mallinfo.fordblks`), arena, source line, file basename, and function to the LCD (fault-safe
  SPI, same channel as the HardFault handler) and halts. This is what pinpointed the clone-table
  OOM above. Keep it — it makes any future OOM legible instead of a blind panic.

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
   cd ~/Downloads/sq3 && /tmp/scidisasm   # disassembles all scripts (segfaults mid-batch)
   ```
   The stock tool segfaults partway through a full batch; to reliably get one script, patch
   `main()` to call `disassemble_script(&d, N, 1/2)` for a single N and run each in its own
   process. SQ3 landmarks: script 994 = `Game` (`play` @0x150, main loop 0x182–0x198),
   script 0 = `SQ3` (`doit` @0x277, per-frame `HaveMouse` @0x294 = cursor logic, harmless),
   script 996 = `User` (`doit` @0x4e calls `GetEvent` @0x77 only when `global_55==0`).

2. **Reproduce the no-mouse path on desktop** with `--disable-mouse` (sets
   `have_mouse_flag=0`, so `kHaveMouse` returns 0 just like Pico):
   ```bash
   ./build/src/freesci --gamedir ~/Downloads/sq3 --graphics sdl --disable-mouse --run
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

### RESOLVED — SQ3 conveyor "jump" fails (bison `wordset` paren rule discarded the group)

The conveyor-shredder puzzle ("stand up" then "jump") was unwinnable: "stand up" worked, but "jump"
→ narrator "Check again" until Roger died in the shredder. **Not Pico-specific** — a shared-engine
FreeSCI bug. Diagnosed via the device parser trace (`debug_mode = pS`, see Debugging item 3) + the
rm10 disassembly (`/tmp/sq3_disasm/010.script`).

**Symptom mechanism:** bare "jump" spuriously **FULL-matched** rm10's **stand** Said-spec `0f2d`
(`(acquire<up),stand [<up] [/belt,conveyer]`, handler #9 of 13). Full match + no `>` marker →
`SAID_FULL_MATCH` → `PUT_SEL32V(parser_event, claimed, 1)` claimed the event, stopping the handler
chain before the **real** bare-jump handler `0f74` (`jump,leap[...] [/banister]`, handler #13 →
`setScript railJump`). Both "stand up" AND "jump" hit the identical
`augment_match_expression_p(): Empty condition → return 1` wildcard path, so the matcher could not
be fixed there (legit "stand" depends on it too) — the bug was upstream in the parser.

**Root cause — `said.y` / generated `said.c`, the `wordset` grammar production.** The rule
`wordset : YY_PARENO expr YY_PARENC` assigned **`$$ = $1`** (the open-paren *token's* `yylval`)
instead of `$$ = $2` (the parsed inner expression). Operator tokens never set `yylval` (`yylex`
only assigns it for `WGROUP` words, `said.y:267`), so `$1` was a **stale word value left over from a
prior parse** — a garbage tree index. This discarded the entire `(acquire<up)` group from the
required clause; with cross-parse `yylval` residue (belt = 0x924 from an earlier spec) it planted
the `(141 14f Error(0924))` degenerate node the device dump showed, emptying the required-word check
→ the empty-condition wildcard → "jump" matched.

**Fix (one rule, two files in lockstep):** `src/engine/said.y:201` `{ $$ = $1; }` → `{ $$ = $2; }`,
and the generated reduction `src/engine/said.c` case 19 `(yyvsp[(1) - (3)])` → `(yyvsp[(2) - (3)])`.
The required clause now correctly contains `acquire`/`stand`/`<up`; bare "jump" no longer matches
0f2d, "stand"/"stand up" still does. Because `$2` is a real node, the fix is immune to the
cross-parse residue that made the device case worse than a single isolated parse.

**Validated three ways:** (1) standalone harness feeding the exact 0f2d spec bytes through
`said_parse_spec` + `vocab_dump_parse_tree` — before/after trees confirm the required clause goes
from `Error(0924)` to `acquire/stand/<up`; (2) the same harness with case 19 reverted reproduces the
broken tree; (3) **device-confirmed (2026-06-07)** — pico.log `pS` trace shows "jump" now skips the
stand spec 0f2d (no "Match.") and the chain continues to the rail-jump spec, `railJump` fires, Roger
grabs the rail. **Blast radius:** affects every parenthesized word-group `(…)` in every SCI game's
Said specs (previously all silently dropped) — strictly more correct. The two analogous `<(…)` paren
rules (`said.y:234`, `248`, `wordrefset`/`recref`) share the same stale-operator pattern but were
left untouched (rarer, untested, not implicated here).

### RESOLVED — SQ3 plays on Pico (keyboard control + room-2 freeze)
SQ3 boots, reaches the first playable room, Roger walks, and gameplay runs without crashes.
The old "Roger won't walk / freeze on landing in room 2" was **never an input bug** — it was the
undersized VM value stack (`VM_STACK_SIZE 0x400`, see correctness fixes above). A second crash
(panic while walking / on text input) was the **clone-table OOM** fixed by `GC_INTERVAL 2048`.

### RESOLVED — Roger sinks under the floor in SQ3 room 3 (priority line-tracer mismatch)

On the merged/nibble-packed Pico decode, room 3 (pic 2051) painted a spurious priority band where
desktop had none — ego walked down, hit it, and got occluded by "floor" until he left the room.
Localized with the `[pcol]`/`[dpcol]` column probes: at x=82, rows 63-86 read **10 on Pico vs 0 on
desktop**; every other row matched. A priority-10 flood-fill was leaking into a pocket that is sealed
on desktop.

**Root cause — NOT a nibble-packing bug.** The packed writers (`ctl_get`/`ctl_set`/`ctl_fill`, the
clear, the fill core, `IS_BOUNDARY`/`BOUNDS_AT`) were all correct. The bug was the **line tracer**:
`ctl_draw_line` (used for the Pico packed priority AND control maps) was a generic integer Bresenham,
while the desktop path (`gfx_draw_line_pixmap_i` → `gfx_draw_line_buffer`, `gfx_line.c` `LINEMACRO`)
is a **midpoint DDA**. The two algorithms pick different pixels on **~32% of segments** (measured:
1275/4032 in a standalone diff). Where a priority *boundary* line shifts by one pixel, it opens a gap
that the 4-connected priority flood-fill leaks through → the spurious band. Desktop never hits this
because it only uses the midpoint tracer; Pico mixed the two.

**Fix (`sci_pic_0.c` `ctl_draw_line`).** Rewrote it to mirror the desktop midpoint DDA *exactly*
(major axis steps every iter, minor axis steps when the decision var goes negative, decision var
seeded at `major_delta - 1`), writing via `ctl_set` for nibble-packing. Verified **pixel-identical to
the desktop tracer across 853,760 sampled lines, 0 divergent**. Fixes both priority and control
boundary lines. Device-confirmed: room 3 `[pcol]` rows 63-86 now read 0, ego stays visible on the
floor. **Lesson:** any Pico packed-buffer drawing primitive must trace the *same pixels* as its
desktop byte-buffer counterpart, not merely be "a correct line/box" — a one-pixel divergence in a
boundary is enough to break a downstream flood-fill.

### RESOLVED — garbage rectangle during shadow/priority redraws
Cached views survived a room change with a stale `psram_addr`. `gfxr_free_all_pics`
(`src/gfx/resmgr.c`, called on every room change) freed the PIC tree and called `psram_reset()`
(rewinds the PSRAM bump arena to offset 0) but left the VIEW tree cached. Persistent views
(Roger's ego view, reused props like the trash lift) kept `psram_valid`/`psram_addr` pointing into
the arena the next room's offloads then overwrote → black box, then cycling memory garbage. Fix:
free the VIEW tree alongside the PIC tree before `psram_reset()`, so `gfxr_get_view` re-decodes
fresh. Trade-off: views re-decode per room change instead of staying cached — correct call on Pico,
and SQ3's per-room view set is small.

### RESOLVED (device-confirmed 2026-06-17) — dialogue box/text lingers on the background after dismiss (Pico)

After a text/dialogue box is dismissed in SQ3 (e.g. room 2's spacecraft narration), white+black box/text
remnants stayed on the background ("top of the spacecraft") until the room was re-entered. Desktop dismisses
cleanly. **Fixed by the static-picview bake-in** (`pico_bake_static_region`, `pico_driver.c`, commit
`4be87cb4`) — the SAME change that fixed the PQ2 missing-foreground-objects bug also clears this. The user
device-confirmed the SQ3 box/text now dismisses cleanly. The two earlier `kgraphics.c` attempts (below) were
the wrong layer: this is the one-buffer `static_bg`/BACK-restore consistency problem, fixed in the driver, not
a kernel-side flush gap. (Mechanism: the dismiss path's BACK restore now reproduces the correct background
over the dismissed-box region because `static_bg` and `visual[0]` are kept consistent through the static draw
path — the exact dismiss kernel sequence was not separately traced, but the same change set resolves it.)

*Historical (kept for the record — the two REVERTED kgraphics.c attempts that produced NO change on device,
confirming the fix belonged in the driver, not the kernel):*

**What was tried and ruled out (both reverted):**
1. **`graph_restore_box` FULL_REDRAW + forced flush** (`HAVE_PICO`). Hypothesis: SQ3's text boxes use the
   save-under/snapshot path (`kDisplay` save_under, kGraph `RESTORE_BOX`, menu/control save-unders all call
   `graph_restore_box`), which frees the box/text widgets via `gfxw_restore_snapshot` → dirtifies only
   `visual->dirty`, then returns to a **bare `gfxop_update()`** (`kgraphics.c:697`) that flushes only
   `state->dirty_rects` and never runs `s->visual->draw()` → on Pico's single `visual[0]` nothing
   BACK-restores/flushes the region. Tried: capture snapshot `area`, then `add_dirty_abs(area)` +
   `FULL_REDRAW()` + `gfxop_update_box(area)`. **No effect on device.**
2. **`kDisposeWindow` FULL_REDRAW + forced flush** (`HAVE_PICO`). Same remedy at the true
   `kNewWindow`/`kDisposeWindow` window-port dismiss site (bare `gfxop_update` → `add_dirty_abs(goner->bounds)`
   + `FULL_REDRAW()` + `gfxop_update_box(goner->bounds)`). **No effect on device either.**

**What "no change from either" implies:** the dismiss for this scene is NOT going through `graph_restore_box`
*or* `kDisposeWindow` (or a FULL_REDRAW *is* firing but something immediately re-draws the box). The flush-gap
theory may still be right for the *mechanism*, but we patched the wrong site(s). **Decisive next step: capture
a pico.log across the dismiss and identify the actual kernel call sequence** — look for `kDisposeWindow`'s
`Activating port %d after disposing window %d` line (confirms that path ran) and any `graph_restore_box` /
`kDisplay` / `kGraph` calls. With `debug_mode` set (SD-root `0:/freesci.cfg`, see the config-file note) the
graphics trace narrows which widget op draws and frees the box. Other possibilities to weigh once the path is
known: (a) the box pixels got composited into the PSRAM `static_bg` so a BACK-restore reproduces them; (b) the
artifact extent lies outside the dirtied rect (window shadow/title beyond `bounds`); (c) cleanup relies on a
per-frame `kAnimate` that this static scene never issues. Documented as open; not chased further until the log
names the path.

### FIXED (device-confirmed) — Pico render-path pic-open flash

Uncommitted, `HAVE_PICO`-guarded / Pico-only `pico_driver.c`, desktop-untouched.

1. **New room background flashes full, vanishes, then fades/curtains in** (SQ3 logo screen; room 2 "shown,
   disappears, curtain reveals"). **Cause:** `pico_render_background` (`pico_driver.c`) composited the
   freshly-decoded background into `visual[0]` AND **immediately flushed it to the LCD** (`flush_region`).
   It is called from `gfxop_new_pic` *inside* `kDrawPic`, **before** `kAnimate`'s open transition. So: the
   new pic snaps on ("show") → `animate_do_animation` redraws `s->old_screen` (the previous room, grabbed
   in kDrawPic before the composite) and flushes ("disappear") → the transition `switch` reveals
   `newscreen` ("fade/curtain in"). Desktop never flashes because `gfxop_new_pic` only stages the *static*
   buffer, never the front. **Fix:** removed the eager `flush_region` from `pico_render_background`; it now
   only **stages** the new background in `visual[0]` (matching the desktop model). The reveal comes solely
   from the normal pipeline (the open transition, `FULL_REDRAW`, or `_reset_graphics_input` on restore).
   `visual[0]` still holds the new pic, so `animate_do_animation`'s `newscreen` grab (reads `visual[0]`)
   is unchanged; `old_screen` is grabbed *before* the composite, so it's still the old room. Net traced
   sequence: old room stays on LCD → transition reveals new room, no flash. `flush_region` is still used
   by `pico_update`'s FRONT path (no dead code).

   **Residual risk (accepted):** a pic drawn with NO following `kAnimate`/update would stay invisible until
   the next flush — but `pic_not_valid=1` forces the first `kAnimate` after every `kDrawPic` into the
   `open_animation` path (which always flushes), identical to desktop, so any game that works on desktop is
   safe. All reported cases (logo, room 2) go through the fade.

**WHAT TO TEST when able to flash:**
- **Dialogue-dismiss (#1):** SQ3 room 2, trigger the first dialogue (the spacecraft narration), dismiss it.
  The white+black box/text must vanish cleanly and the spaceship background must be intact — no lingering
  artifacts on the top of the ship, no room re-entry needed. This scene uses the **`graph_restore_box`
  snapshot path** (the primary fix). Also exercise other message boxes (parser responses, inventory, "look"
  descriptions) AND any true `kNewWindow` windows: open → read → dismiss, confirm no stuck artifacts on
  either path.
- **Pic-open flash (#2):** Watch the **SQ3 intro logo** and **room 2 first entry**: the new screen must
  fade/curtain in directly from the *previous* screen — it must NOT snap on full, blank, then re-reveal.
  Walk between several rooms (2↔9↔10↔11) and confirm each room transition is a clean single fade with no
  pre-flash. Verify no room comes up *blank* and stuck (the residual-risk case) — every room should reveal
  via its transition.
- **Regression watch (both):** confirm normal gameplay rendering is unaffected — sprites, text, cursor,
  priority occlusion all still draw; no new garbage rects on window open/close or room change.

### RESOLVED — in-game savegame restore rebuilt on a coalesced heap (multi-restore device-confirmed 2026-06-07)

In-game `kRestoreGame` on Pico no longer rebuilds the new gamestate in place. The old in-place rebuild
peaked at (old state + new state) co-resident and shattered the ~388KB heap into sub-32KB fragments, so
the restored room's pic decode could not find a 32KB-contiguous block → fatal `kDrawPic` abort →
HardFault. The restore is now **deferred** to `_game_run` and run on a torn-down, coalesced heap:

- **`kRestoreGame`** (`kfile.c`, `HAVE_PICO`): stash the savedir name in `g_pico_restore_pending_name`,
  set `script_abort_flag = SCRIPT_ABORT_WITH_REPLAY`, unwind the exec stack — do NOT call
  `gamestate_restore` here.
- **`_game_run`** (`vm.c`, `HAVE_PICO`): on the pending name, tear the running game all the way down
  (`game_exit` → `script_free_engine` → `script_init_engine` → `game_init` → `sfx_reset_player`) so the
  heap coalesces, **reserve the 32KB priority decode buffer NOW** (`pico_reserve_restore_priority`,
  `operations.c`; consumed in `sci_resmgr.c` priority alloc) while a large contiguous run exists, THEN
  `gamestate_restore`. The reserved block survives the re-fragmentation `gamestate_restore` causes and
  feeds the first post-restore pic decode, so that decode can't fail.

**Resources stay resident (by design):** `game_exit` keeps gfx_state + resmgr alive, so there is NO slow
resource reload on restore. A full **relaunch** (quit `freesci_main`, re-enter cold) was tried and
**rejected** — it dumped the player to the chooser AND forced a full resource reload. Keep the in-engine
path.

**The restore #2 OOM fix — open the contiguous hole BEFORE deserialization, not after** (`gamestate_restore`,
`savegame.c`, `HAVE_PICO`). The earlier code freed the outgoing state's large buffers at the *bottom* of
`gamestate_restore` (just before `reconstruct_scripts`), but the OOM was `calloc 16384 failed` in
`read_mem_obj_t` *inside* `_cfsml_read_state_t` — the savegame **deserialization**, which runs *earlier*,
while the outgoing state `s` is still fully resident → 2× working set, no 16KB-contiguous run. Fix: relocate
the heap-relief to run right after the `state` file opens, *before* the CFSML read:
- flush the resmgr LRU resource cache (`scir_free_all_lru(s->resmgr)`, reloads on demand),
- free every outgoing script's bytecode `buf` (~30KB) and the 16KB VM value stack (one contiguous malloc →
  guaranteed 16KB hole), NULLing each (the `_sm_deallocate` cases are null-guarded under HAVE_PICO),
- **free `visual[0]`, the 64KB display back-buffer** (`pico_free_visual(s->gfx_state->driver)`), re-allocated
  right after `fclose` (`pico_alloc_visual`) before `_reset_graphics_input` repaints it.
This opens a ≥64KB contiguous hole exactly when `read_mem_obj_t` needs its 16KB calloc.

**Device status (log 67bbb55d, 2026-06-07) — THREE consecutive in-game restores all succeed.** Restores into
rooms 2 → 10 → 9, each `Restarting with replay() [clean-heap restore]` → room re-enters with `free` ≈
100–109KB after, no `[OOM]`, no `[FAULT]`. Clean teardown to the chooser at quit: `free=396856 used=26528`
(no leak across the whole multi-restore session). The undersized-hole OOM is gone.

**Residual (not a blocker, parked):** the arena still ratchets ~+32KB per restore
(349656 → 382424 → 415192 → 423384) and `chunks` climbs (17 → 204 → 241 → 282), because the in-engine
teardown can't reset the long-lived survivors (gfx_state/resmgr/vocab/driver) to the chooser baseline. With
~100KB free after each restore there is comfortable headroom for several restores, so this is **not** chased
now. If a *very* long restore chain eventually OOMs, the lever is the arena ratchet — likely the 32KB priority
reservation held across reconstruction forcing a fixed picolibc sbrk increment — not the deserialization hole
(now fixed). NOT a relaunch.

### FIXED (pending device retest) — restore HardFault was a control-map OOM, not corruption

Enabling the control map (roadmap #2, flag now ON by default) surfaced two HardFaults. They are
**NOT** the same root cause — an earlier guess that both were an unclipped brush overflow was wrong.
The console log decided it:

```
Restarting with replay()
malloc 32000 failed to allocate memory
GFX Error: ... gfxop_new_pic() L2260: Could not retreive background pic 2!
FSCI: ERROR in kDrawPic ... GFX subsystem fatal error ... aborting...
[FAULT]
```

- **Restore fault (PC in `gfxop_scan_bitmask`/`_gfxop_scan_one_bitmask`, BFAR `0x02027571`) = OOM.**
  On restore's `replay()` the heap is tighter than on first room entry, so the raw `malloc(32000)`
  for the packed control buffer (`sci_resmgr.c:157`) returns NULL. The old NULL branch did
  `return GFX_ERROR`; that propagates up so `gfxop_new_pic` returns `GFX_ERROR` with `state->pic`
  NULL, and `kDrawPic` (`kgraphics.c:1180`) escalates `GFX_ERROR` to a **FATAL VM error → "aborting"
  → HardFault**. The wild-pointer deref is the downstream cascade of the aborted/partial pic, not the
  primary bug; the *identical* `BFAR` across attempts fits a deterministic OOM state, not random
  corruption. ("malloc 32000 failed…" is the pico-sdk malloc wrapper under `PICO_MALLOC_PANIC=0`.)
- **Trash-elevator fault (unaligned UsageFault, CFSR `0x01000000`, in GC reg_t hashmap) = STILL
  OPEN.** GC is a canary (runs every 2048 allocs on Pico, walks every `reg_t`). Could be downstream
  of heap corruption OR an independent alignment bug — not yet explained. Retest after the OOM fix.

**Fix 1 — graceful control-map OOM (the real restore fix).** In `gfxr_interpreter_calculate_pic`
(`sci_resmgr.c`), when `malloc(32000)` for `control_buf` fails, **do not abort the decode**. Emit a
one-shot `GFXWARN` and skip Pass 2 (the control pass): leave `control_map->index_data` NULL /
`psram_valid` 0 so `gfxop_scan_bitmask` returns 0 (no collision) for that pic — exactly like
`-DPICO_CONTROL_MAP=OFF`, but **per-pic and recoverable** on the next decode with more free heap.
The whole Pass-2 block is wrapped `if (control_buf) { … } else { free(reuse_aux_buf); }` (the 64KB
visual buffer earmarked as the flood-fill aux_map is freed in the else so it doesn't leak). This is
the documented `sci_resmgr.c:157` TODO, taken one step further: the previous note said "return
GFX_ERROR" to stay recoverable, but `GFX_ERROR` is in fact fatal at the `kDrawPic` layer — so we
degrade instead of erroring.

**Fix 2 — defensive clip in `ctl_set` (kept, but NOT the restore cause).** `_gfxr_draw_pattern`
computes the control index from a 320x200 space with no per-pixel clip, so an edge-straddling brush
can exceed `[0,64000)`. Desktop tolerates it (full 64000-byte buffer + roomy heap → overhang lands in
slack — latent, not "handled"). On Pico's nibble-packed 32000-byte buffer + tight heap it could stomp
an adjacent allocation. `ctl_set` (`sci_pic_0.c`, the single Pico write chokepoint; `ctl_fill` calls
it, `ctl_draw_line` already clipped) now clips out-of-range indices and emits a one-shot `GFXWARN`
with the offending index — **diagnostic, not silent**, so it can't mask a benign edge overhang
(~64000–64500) vs a wild value. This is retained as defense-in-depth and as a probe for the still-open
trash-elevator fault: if that warning fires there, the OOB write is implicated; if not, look elsewhere.

### RESOLVED (elevator) + LOCALIZED (per-restore leak) — savegame-restore control map starved by a clone-variables leak

**Symptom:** SQ3's trash elevator (room 4) picked Roger up on a fresh boot but, after *several* savegame
restores, stopped — `kOnControl` returned bitmask **0** (no collision) instead of **3** (the scoop). The
user correctly guessed "a leak." The elevator gate is `rm004::doit` requiring `ego onControl == 3`;
`kOnControl` (`kgraphics.c`) returns `gfxop_scan_bitmask(...)` = `retval |= (1<<v)` per control colour
(bg colour 0 → bit 1; scoop → bits 0+1 = 3; 0 = control map absent/`psram_valid=0`).

**Root chain (confirmed from pico.log):** every restore runs `replay()`, which re-enters room 4 and
re-decodes control pic 2052 into a raw `malloc((GFXR_AUX_MAP_SIZE+1)>>1)` = 32000-byte nibble buffer
(`sci_resmgr.c`). A per-restore heap leak (below) ratcheted `uordblks` up until that `malloc(32000)`
returned NULL → graceful degrade (skip Pass 2, `psram_valid=0`, GFXWARN) → scan returns 0 → elevator
dead. So the **elevator code is correct** (the `[oc]`/`[ctl]` probes show `bitmask=3 cm=y valid=1
addr=76664` whenever the map decoded *and* Roger was on the spot, even after many restores); the failure
was purely the control decode being starved by accumulated leakage.

**Leak hunt (static audit + the `[32,128)` SITES tagger).** `game_exit` fully tears down the old
`state_t` each restore, so an *accumulating* leak must live in a structure that survives teardown or be
an orphaned sub-alloc. Static audit found every major restore/teardown path balanced (menubar,
sys_strings, song iterators, scripts/objects/code/obj_indices/locals, clones/lists/nodes, classtable,
file_handles, visual tree, CFSML refstructs, adopted parser/selector/kernel tables) **except** one
confirmed teardown miss — see fix below. To name the dominant leaker, the census call-site tagger
(`pico_mem_census.c`) was retargeted from the `[256,512)` bucket to **`[32,128)`** (where the measured
~1.2 KB/restore lived) and `CENSUS_NSITES` widened 96→192. The retarget worked: across one session the
`[mem] SITES256:` line showed exactly one site climbing monotonically while all others stayed flat —
**`kscripts.c:212`** (`kClone` allocating `clone_obj->variables`): live blocks **19 → 38 → 49 → 63 → 93
→ 165** over ~8 restores, while the live room only ever held `clones=11/88`. So ~150 clone-variable
blocks (~48 B each) were orphaned.

- **DONE — `game_version` teardown leak (small, certain).** `game_exit` (`game.c`) now does
  `free(s->game_version); s->game_version = NULL;`. It was *only* freed in `kSaveGame` (`kfile.c:937`),
  never on the restore-teardown path, where it is `sci_malloc`'d via `_cfsml_read_string`
  (`savegame.c:2065`). Plain `free()` (not `sci_free`) on purpose: `sci_free(NULL)` hits `BREAKPOINT()`,
  whereas `free`/`__wrap_free` guard NULL; initial state is `sci_calloc`'d so the field starts NULL.
  ~10 B/restore. (Note: freeing `sci_malloc`'d memory via raw `free` drifts the known-broken `scilive`
  counter but not `uordblks` — consistent with existing accepted behaviour.)

- **FIXED (pending device retest) — clone-`variables` leak on engine teardown (`seg_manager.c`
  `_sm_deallocate` MEM_OBJ_CLONES, the dominant ~77-block/restore leaker).** The earlier "GC apparently
  doesn't free variables" guess was WRONG: the GC clone-reclaim path `free_at_address_clones`
  (`seg_manager.c:1772-1796`) **does** `sci_free(victim_obj->variables)` before `sm_free_clone`, so
  disposed clones during play are reclaimed cleanly. The actual leak was the **teardown** path: at
  savegame-restore (`game_exit` → `script_free_engine` → `sm_destroy` → `_sm_deallocate`) the
  MEM_OBJ_CLONES case freed `mobj->data.clones.table` but never iterated to free each **still-live**
  clone's separately-`sci_malloc`'d `entry.variables`. SQ3's death scene holds ~77 live clones at
  restore time → ~77 variables blocks leaked per restore (the `kscripts.c:212` SITES climb 25→65 across
  restores), a per-restore heap-fragmentation driver feeding the post-restore decompress OOM
  (`decompress0.c:324`). **Fix:** `_sm_deallocate` MEM_OBJ_CLONES now walks the table
  (`ENTRY_IS_VALID`) and `sci_free`s each live `entry.variables` (NULLing it) before freeing the table.
  Safe vs the GC path (which NULLs `variables` before `sm_free_clone`, so a reclaimed slot is never
  re-walked). Both configs build clean. Process teardown was already clean at quit (`sm_destroy` frees
  the table; the leak only mattered *across* in-session restores where the same fragmented arena is
  reused).

**Diagnostic probes left in the tree (all `HAVE_PICO`-gated, strip when the clone leak is closed):**
`[oc]` (`kgraphics.c` `kOnControl`, logs control-mask scans on transition), `[ctl]` (`sci_resmgr.c`,
counts non-bg control nibbles post-decode so a blank map vs a stale-address read are distinguishable),
and the retargeted `[32,128)` census/SITES tagger (kept instrumentation — the regression watch for any
future per-restore growth; diff a SITES site's live_count across same-room restores to name a leaker).

### OPEN — corruption confirmed — branch-to-NULL HardFault (death-scene / 4ded2752)

The captured fault dump for the 4ded2752 session is a **jump to address 0**, NOT the GC
NULL-malloc data-deref earlier guessed for that log (that guess is **retracted** — see decode):

```
HardFault PC=0x00000000   ← branched to address 0, fetched an instruction there
LR  =0x20081…0            ← SRAM, near top-of-stack region
CFSR=0x00020000           ← UFSR bit1 = INVSTATE (UsageFault); BFSR/MMFSR both 0
HFSR=0x40000000           ← FORCED (the UsageFault escalated to HardFault)
MMFAR=0xe000ed34          ← = the MMFAR register's own address → MMARVALID clear → no fault addr
BFAR =0xe000ed38          ← = the BFAR register's own address  → BFARVALID clear → no fault addr
```

Decode: **PC=0 + INVSTATE + no valid BFAR/MMFAR = a control-flow transfer to NULL**, i.e. a call
through a **NULL/corrupted-to-zero function pointer** (addr 0 has bit[0]=0 → Thumb bit lost → INVSTATE)
or a `POP {PC}` returning through a zeroed stack slot. This is **not** a data write — a NULL-malloc
deref in `hashmap.c` would fault with PC in flash (`0x10xxxxxx`) + a small BFAR + BFSR set, which is
**not** what the dump shows. So the `calloc 2060` + `malloc 12` flood in the log tail was GC merely
*running* just before the fault; the fault itself is a jump-to-0. **This puts the 4ded2752 crash in the
corruption family below ("aspb"), not the OOM family.** The hashmap `sci_malloc`/`sci_calloc` hardening
stays (legible GC-time OOM) but does **not** fix this crash.

Likeliest concrete culprit given "fault before the Roger death scene": a **gfxw widget whose op
pointer (`draw`/`free`/`tag`/…) was overwritten with 0** — the death scene spins up animation widgets
and calls them via C function pointers (`widget->draw(...)`). Overflow source still points at the
message-window/text path (same as the dispose-time `free()` fault). → ASan target unchanged; add the
death-scene widget path to the things to hammer.

**Static-analysis finding — the SCI string kernels discard a known buffer size.** `kFormat`,
`kStrCat`, `kStrCpy` (`kstring.c`) deref their dest with `kernel_dereference_bulk_pointer(s, argv[0], 0)`
— the `0` is the bounds arg, so `_kernel_dereference_pointer` (`kernel.c:1097`) skips the
`entries > maxsize` check and **throws away the real size** `sm_dereference` computed (`seg_manager.c`:
`dynmem.size`, `sys_strings[].max_size`, script/locals/stack byte counts). They then write unbounded:
`kFormat` caps at a **fake `maxsize=4096`** (comment: "Arbitrary..."), `kStrCat` is a bare `strcat`,
`kStrCpy`'s argc==2 path a bare `strcpy`. A long formatted/concatenated dialog string (the message-
window path the logs correlate with) overflows a small dynmem/sys_string buffer → ASCII into the next
heap chunk = the "aspb"/PC=0 signature. Desktop survives on slack; Pico's tight heap puts a live
pointer right after.

**Probe in place (no behavior change, no clamp).** `str_overflow_probe` (`kstring.c`) + a one-shot
clause in `CHECK_OVERFLOW1` now log `[strprobe] <kernel> writes N bytes into an M-byte buffer …
(overflow by K)` whenever a write crosses the *real* dest size, **without truncating** — the write
proceeds exactly as before (`kFormat` still hard-stops at 4096). Captured via `sciprintf` → pico.log
over serial, so the line lands **before** the eventual branch-to-NULL crash and names the culprit
kernel + buffer. **Next device session: grep pico.log for `[strprobe]`** — if one fires just before the
`[FAULT]`, that kernel/buffer is the overflow source; then fix with a real bounds clamp (the size is
already in hand). If none fires, the corruptor is elsewhere (not these three string kernels).

### MITIGATED (legible halt) — `run_vm`-entry HardFault was a fragmentation-OOM cascade, NOT corruption (grabber/motivator/button, IMG_1682 + log 40355e01, 2026-06-07)

**Earlier theory RETRACTED.** The IMG_1682 LCD dump was first read as independent heap corruption (a
reg_t-shaped value smashing `state_t *s`). The follow-up session **captured a pico.log (40355e01)** of the
*identical* fault, and it proves the opposite: the garbage `s` is the **downstream symptom of a fatal
GFX-OOM longjmp into a dead frame**, not a bad write. Root trigger = **fragmentation OOM at a pic decode**.

The captured LCD dump (same as IMG_1682):
```
HardFault PC=0x1001e774   → run_vm, vm.c:754
LR  =0x1001e75e           → run_vm, vm.c:746
CFSR=0x00008200           → BFSR = 0x82 = BFARVALID | PRECISERR
HFSR=0x40000000           → FORCED
BFAR=0x0005021a  MMFAR=0x0005021a   → s=0x00050002, +0x218 (script_000) faults
```

**The cascade (log-confirmed).** Player restored (into room 12), walked 12→9→10→11→8. Entering room 8 the
background pic decode needed a 32 KB-contiguous priority buffer; heap was `free=75664` but fragmented
(arena pinned 415192, high chunk count) → **`malloc 32000 failed`**. The deferred/early-pin retry
(`operations.c` `gfxop_new_pic`) also couldn't find the block → `GFXERROR("Could not retreive background
pic 8")` → `return GFX_ERROR`. `kDrawPic`'s `GFX_ASSERT` (`kgraphics.c:1493`) treats `GFX_ERROR` as fatal
→ `vm_handle_fatal_error` (`vm.c:642`) → `longjmp(vm_error_address, 0)`. **The global `vm_error_address`
jmp_buf is stale across nested `run_vm` calls**, so the longjmp restores a *dead/returned* frame; back at
the `run_vm` prologue (`vm.c:754`) the reloaded `s` (`[sp,#44]`) is garbage (`0x00050002`) and
`s->script_000` derefs `0x0005021a` → HardFault. So `s` was never *written* — it's stale spilled-locals
from a frame that already returned. (`0x00050002` looking reg_t-shaped was a coincidence.)

**Classification: fragmentation OOM → fatal GFX abort → longjmp-into-dead-frame.** NOT the "aspb"
overflow family, NOT the clone-`variables` UAF. The `malloc 32000 failed` line in the log is the tell;
absence of an `[OOM]` LCD halt was only because the fatal-GFX path bypassed `pico_oom_report`.

**MITIGATION DONE (option A, legible halt).** `gfxop_new_pic` (`operations.c`, the `if (!state->pic ||
!state->pic_unscaled)` failure block, `HAVE_PICO`): instead of `return GFX_ERROR` — which on Pico
escalates to the unrecoverable longjmp-into-dead-frame HardFault — it now calls `pico_oom_report("pic
decode (no contiguous heap)", GFXR_AUX_MAP_SIZE, …)` and halts. A decode that genuinely can't find its
buffer now shows the failing decode + free heap on the LCD (same channel as every other OOM) instead of a
mystery HardFault. This does **not** keep the game running — it makes the failure *legible*. The
longjmp-into-dead-frame (a latent shared-engine bug: global jmp_buf clobbered by recursive `run_vm`) is
left as-is; on Pico the fatal GFX path is unrecoverable anyway.

**Still OPEN — the real "keep playing" fix is reducing the decode peak/fragmentation** so the 32 KB alloc
succeeds (the "transient peak + fragmentation OOM" roadmap work: per-cel decode scratch, control/priority
peak-shrink). Until then, hitting a room whose pic can't decode on a fragmented heap halts cleanly with an
`[OOM]` dump naming `operations.c` rather than HardFaulting.

**DEVICE-CONFIRMED working (log f3fa5b3f, 2026-06-07).** The mitigation behaves exactly as designed: after
a restore (into room 12) + walking 12→9→10→11→8→11, the *second* entry to room 11 hit
`malloc 32000 failed` and produced a clean `[OOM]` LCD dump (`pic decode (no contiguous heap)`,
`size=0xfa00`=64000 nominal, `free=0x15520`=87328, `operations.c`/`gfxop_new_pic`) instead of the prior
HardFault. (Cosmetic: the LCD `size` is the nominal 64 KB decode-buffer constant I hardcoded; the *actual*
failing alloc was the 32 KB priority buffer, correctly named by the serial `malloc 32000 failed` line.)

### DIAGNOSIS — the post-restore OOM is FRAGMENTATION + arena ratchet, NOT a leak or a high live-set (log f3fa5b3f)

The same f3fa5b3f session pins down *why* a 32 KB decode fails post-restore while fresh boot decodes every
room fine. The `[mem] BREAKDOWN` progression is decisive:

| phase | room | uord | ford | arena | chunks |
|---|---|---|---|---|---|
| fresh boot | 2 | 310192 | 72232 | 382424 | **29** |
| post-restore | 12 | 309152 | 106040 | **415192** | **192** |
| post-restore | 8 | 325736 | 89456 | 415192 | 126 |
| post-restore | 11 (2nd) | — | — | 415192 | → `malloc 32000 failed` |

- **`uord` (live bytes) is FLAT across the restore (~310→326 K).** The live set does NOT grow per revisit —
  this is **not** a leak. The (RESOLVED) ~35 KB/revisit cwd+console leak is confirmed still fixed: post-
  restore `untracked` holds at 237–248 K (oscillating ~10 K), not climbing 35 K/revisit.
- **Arena ratcheted +33 KB (382424 → 415192) and pinned.** picolibc `sbrk`'d during the restore's 2×
  working-set peak and never returns it. This quantitatively **confirms the parked arena-ratchet
  hypothesis** (restore note above): the prime suspect is `pico_reserve_restore_priority()`'s 32 KB held
  across `gamestate_restore` forcing a fixed sbrk increment.
- **Fragmentation exploded: chunks 29 → 192.** Post-restore the heap is shattered into 100+ free holes, so
  a 32 KB *contiguous* decode block can't be found even with ~90–100 K *total* free (`ford`). Contiguity,
  not total free bytes, is the limiting resource — same conclusion as the historical fragmentation note,
  now isolated to the **restore rebuild** as the fragmenting event (fresh-boot chunks stay ≤29).

**Baseline composition (CENSUS, room 2 fresh boot) — where the ~310 K actually lives:**
`32768: 1/64004` = **visual[0] 64 KB** · `16384: 3/71636` = the three-block lump, **now fully named** (below)
· `1024+2048: 38 blk/~65 KB` · `8: 1933/23196` = **23 KB across 1933 eight-byte blocks** (a fragmentation
source in itself).

**The ~71636-byte 16384-bucket lump is NAMED (SITES16K one-flash, log a71d552a) — three irreducible
resident costs, NOT a leak and NOT cheaply reclaimable:**

| Site | Bytes | What it is | Reclaim verdict |
|---|---|---|---|
| `resource_map.c:309` | 25404 | **Resource directory** — `sci_realloc(resources, sizeof(resource_t)*N)`, one `resource_t` per game resource (type/number/file/offset). Read-only after `_scir_read_resource_map`. | Only genuine PSRAM-offload candidate, but looked up on every resource load (random access on the hot load path) — see investigation below. |
| `vocab.c:206` | 29844 | **Packed vocab words** — the `vocab_pack_words` blob (offset table + packed records); the re-enabled parser's resident word list. | PSRAM-resident-words behind `psram_set_floor()` was already **ABANDONED** (roadmap #1): device measured packing saving only ~3 KB, real lever was the transient parse peak (solved via visual-borrow). Pulling to PSRAM adds per-`kParse` bsearch paging for a feature that already fits. Low value. |
| `seg_manager.c:1348` | 16388 | **VM value stack** — `sci_calloc(VM_STACK_SIZE=0x1000, sizeof(reg_t))` = 4096×4 + 4 hdr. | **OFF LIMITS.** Shrinking to 0x400 caused the SQ3 room-2 recursive-`run_vm` overflow HardFault (correctness fix above). Hot read-write (PUSH/POP every instruction) → can't go to PSRAM either. |

Sum = 71636, exact. The `SITES16K` line was **identical across all four rooms** (777/900/2/3) — stable
resident baseline, not per-room growth. **Conclusion: the baseline is "high" because it is three irreducible
costs (VM stack must stay, vocab already optimized, resource directory on the hot read path); none is a leak;
none moves cheaply.** This confirms the diagnosis — the post-restore OOM is fragmentation + the arena ratchet,
NOT a fat trimmable baseline. The census tagger has been reverted from `[16384,32768)`/SITES16K back to its
default `[32,128)`/SITES256 watch.

**Two levers, possibly one fix.** (1) The "keep playing" decode fix (fix B-1): a **permanent 32 KB priority
decode scratch** allocated once at boot from pristine heap, reused every decode, never freed — mirrors the
existing `visual_borrowed` skip-free pattern (the 64 KB visual already borrows resident visual[0], so the
32 KB priority is the *only* remaining fresh per-decode malloc). NB priority MUST be SRAM (drawn into with
random-access fills/lines in `gfxr_draw_pic01`) — it canNOT be decoded into PSRAM (SPI-only, not mapped);
the earlier "decode priority into PSRAM" idea is **retracted**. (2) The arena-ratchet fix: if B-1's
permanent scratch exists, the restore path no longer needs `pico_reserve_restore_priority()`, so the 32 KB
isn't held across reconstruction → the +33 KB sbrk ratchet may disappear. So **B-1 may fix both the decode
OOM and the post-restore baseline ratchet in one change** — at a cost of +32 KB always-resident SRAM
(decode *peak* ~unchanged since that 32 KB is live during every decode anyway; the *valley* between decodes
drops ~32 KB, e.g. room-8 free 89 K → ~57 K, still positive but tighter for heavy-clone scenes).

### TESTED — graceful retry+GC fired but did NOT recover; revealed TWO findings (log 63c9796a, 2026-06-08)

**The fix (as built):** every `sci_malloc`/`sci_calloc`/`sci_realloc` routes through
`_SCI_MALLOC`/`_SCI_CALLOC`/`_SCI_REALLOC` (`sci_memory.c`), where the Pico OOM-halt lives. On a NULL return
the allocator calls `pico_reclaim_heap()` (`game.c`, `HAVE_PICO`) **once**, then retries before falling to
`pico_oom_report`. `pico_reclaim_heap` flushes the resource LRU (`scir_free_all_lru(s->resmgr)`) then runs the
GC (`run_gc(s)`), reaching the live state via `g_pico_current_state` (published in `game_init`, cleared in
`game_exit`). A static `g_pico_in_reclaim` guard prevents recursion (run_gc + LRU reload both allocate). Both
configs build clean; desktop untouched (all `HAVE_PICO`-gated). **Uncommitted — held for the device test that
this note reports.**

**Device result: the room-13 restore OOM STILL HALTS.** Restore into room 13 now *succeeds* (room enters +
ready — the decrypt1 stack-collision crash is gone, consistent with the off-stack fix), but post-restore
gameplay still `[OOM]`s: `malloc 11127 failed` **printed twice** → `[OOM]` at `decompress0.c:324`
(`free=0x78b8`=30904, `arena=0x66bc4`=420804). The two identical "failed" lines confirm the **retry fired**
(attempt + post-reclaim retry, same 11127) — but reclaim freed nothing usable.

**FINDING 1 (implementation BUG — FIXED, pending device retest) — `g_pico_current_state` was NULL post-restore,
so reclaim was a no-op exactly when needed.** My earlier "why it targets the restore site correctly" reasoning
was WRONG. The restore path (`vm.c` `_game_run`, ~2313-2319) does `game_exit(s)` — which **clears the global to
NULL** — then swaps `s = rs` (the restored state from `gamestate_restore`) **without calling `game_init` on
`rs`**. So `g_pico_current_state` stayed NULL for the entire restored session; `pico_reclaim_heap()` saw NULL
and returned immediately. GC + LRU-flush never ran. (game_init *does* run earlier on the throwaway light state
at vm.c:2304, but that state is freed at 2318 — the running state `rs` was never published.) **Fix applied:**
republish `g_pico_current_state = s` right after `s = rs` (vm.c, `HAVE_PICO`, with an `extern` decl beside
`g_pico_restore_pending_name`); the `else` branch (rs==NULL, continuing the old game_init'd `s`) was already
correct. Both configs build clean. NB the OOM here is *gameplay* (post-restore, after a window dispose), NOT
deserialization — so the "GC walks a fresh consistent state" safety argument doesn't apply either; this is the
gameplay-time-GC-from-arbitrary-point path (the caveat below), now the primary path — so the next device run is
also the first real exercise of `run_gc` firing from inside an allocation.

**FINDING 2 (the deeper problem — reclaim is the WRONG LEVER for this OOM).** Even with Finding 1 fixed, this
specific OOM almost certainly won't recover, because the blocker is **fragmentation, not reclaimable bytes**:
- `reslru=0 reslock=615` — all 615 resources are LOCKED, so `scir_free_all_lru` frees **nothing** here.
- `chunks=219` at room-13-ready (fresh boot is 17-40) — the restore rebuild shattered the heap. 30904 B free
  total, but no 11127-contiguous run among 219 fragments. `run_gc` only frees small scattered clone/node
  blocks (~40 B each; `clonevar=3140 B/77 clones`) → cannot synthesize an 11127-contiguous hole.
So retry+GC is cheap insurance for OOMs where the LRU has evictable content or GC can free a *large* block, but
it does **not** address the post-restore fragmentation/arena-ratchet wall (the parked "lever 2"). The real
lever for the restore decompress OOM is reducing restore-time fragmentation (chunks 17→219) or a decompress
strategy that doesn't need a large contiguous block — NOT allocator self-reclaim.

**Status:** (a) Finding-1's republish is **DONE** (committed) so retry+GC now actually runs post-restore —
but the predicted "`run_gc` from inside an allocation HardFaults" risk **MATERIALIZED on device** (next note,
log dde62e0d): `run_gc` is now **removed** from `pico_reclaim_heap` (LRU-flush-only). (b) The fragmentation
lever for *this* decompress site is **still open** — reclaim can't conjure contiguity (LRU empty/all-locked, GC frees
only scattered ~40 B blocks vs the 219-chunk shatter), so it needs restore-time fragmentation reduction or a
decompress strategy that avoids a large contiguous block, NOT allocator self-reclaim. The clean `[OOM]` halt
(legible, no HardFault, no corruption) means the mitigation behaves; it just can't make space that isn't
contiguous. **The decrypt1 stack-collision fix is device-CONFIRMED by this log** (room 13 restore no longer
HardFaults at `decompress0.c:153`; it now reaches the clean fragmentation `[OOM]` underneath, exactly as the
3396e873 note predicted).

### DONE (device-confirmed, log dde62e0d + IMG_1686) — `run_gc` in reclaim HardFaults; reclaim is now LRU-flush-only

The Finding-1 republish (above) made `g_pico_current_state` non-NULL during the restored session, so the very
first device run that exercised `run_gc` *from inside a failed allocation* did exactly what the caveat warned:
it **HardFaulted**. Two consecutive in-game restores, then the second restore's re-init OOM'd at `malloc 7302
failed` (printed **once**, NOT the prior two-`failed`+clean-`[OOM]`) → `[FAULT]`. The single `failed`+`[FAULT]`
with no `[OOM]` is the tell: control diverged *between* the first malloc-fail and the retry — i.e. inside
`pico_reclaim_heap` → `run_gc`.

**LCD dump (IMG_1686), resolved against `build-pico/src/freesci.elf`:**
```
HardFault PC=0x1002e018  → worklist_push (gc.c:62) — the reg_t_hash_map_check_value(hashmap, reg, 1, &added) call
LR  =0x1002ac0c          → find_canonic_address_id (seg_manager.c:1652) — the GC segment walk
CFSR=0x00008200          → BFSR = BFARVALID | PRECISERR (precise data bus fault)
BFAR=MMFAR=0xffffffe4    → ≈ NULL−28: a struct-field deref through a bad/near-NULL pointer
```
**Root cause:** the 7302 alloc fails inside `gamestate_restore` → `_reset_graphics_input` (palette/decompress).
At that instant `g_pico_current_state` points at the **throwaway light state** `game_init` built at vm.c:2305 —
while `gamestate_restore` is mid-flight building the *real* target. So `run_gc` walks a half-reconstructed,
inconsistent seg_manager; `find_canonic_address_id` hands `worklist_push` a reg_t whose segment entry is bogus,
and the reg_t hashmap deref faults (BFAR ≈ NULL−28). This is the documented "GC from an arbitrary allocation
point" hazard, now proven on hardware.

**FIX (device-confirmed by the user): `run_gc(s)` is commented out in `pico_reclaim_heap` (`game.c`) — reclaim
is now LRU-flush-only.** The HardFault is gone. Nothing is lost for the restore case: `run_gc` couldn't help it
anyway (Finding 2 — fragmentation, all 615 resources LOCKED so even the LRU flush is a near-no-op there). The
LRU flush is retained because it *does* help true gameplay OOMs that have evictable (unlocked) resources.
Disposed clones/lists/nodes now wait for the normal `GC_INTERVAL` (2048) instead of an on-OOM sweep — acceptable.

**If GC-on-OOM is ever wanted back**, it must NOT fire from inside an arbitrary allocation. Move it to a *safe
sequence point* where the live state is consistent (e.g. before a room transition, or before `replay()` in the
restore path *after* the new state is fully built and published), gated so it never runs while
`gamestate_restore` is mid-flight. Not pursued now — the real OOM lever is restore-time fragmentation reduction,
not allocator self-reclaim.

### DONE (device-confirmed, log ce81217b) — B-1 permanent priority scratch fixes the pic-decode OOM

Fix B-1 is implemented and device-confirmed. `g_pico_priority_scratch` (32 KB nibble-packed) is `malloc`'d
**once** on the first pic decode from the still-pristine heap (`operations.c` `gfxop_new_pic` prologue) and
reused by every decode thereafter — never freed, mirroring the `visual_borrowed` skip-free pattern. The
priority alloc in `gfxr_interpreter_calculate_pic` (`sci_resmgr.c`) now points at the scratch instead of a
fresh per-decode `malloc`, and both priority free sites are guarded `if (!priority_is_scratch)`. The old
`pico_reserve_restore_priority()` reservation (and its `vm.c` `_game_run` call) is removed — no longer needed
since the scratch is permanent. **Result (log ce81217b):** after 6 post-restore room transitions with
chunks=196 (heavily fragmented), NO `malloc 32000 failed`, NO pic-decode `[OOM]`; player progressed (inserted
the motivator into the ship). The decode-OOM lever (lever 1) is closed.

- **Arena ratchet (lever 2) NOT fully eliminated** — it was a "may", and the log says no: arena still
  ratcheted 347100 → 379868 → 412636 → 424924 → 425948 across restores. Removing the priority reservation did
  not kill the +~33 KB/restore sbrk creep, so the ratchet's cause is elsewhere (still parked — not a blocker
  while ~100 K stays free after each restore).

### DONE (device-confirmed, log 20bb9822) — B-1.2 permanent pic/view decompress scratch closes the decompress0 OOM

Same permanent-scratch pattern as B-1, applied to the **decompress output buffer**. `g_pico_decompress_scratch`
(16 KB) is `malloc`'d **once** in the `gfxop_new_pic` prologue (`operations.c`, right after the priority
scratch) from the still-pristine boot heap, and `decompress0` reuses it for every **pic/view** decode instead
of a fresh per-decode `sci_malloc(result->size)` — so the fragmented post-restore heap never has to find a
contiguous block for it (the `decompress0.c:324` OOM, the wall the prior several notes kept hitting).

- **Touch points:** `sci_memory.h` (`PICO_DECOMPRESS_SCRATCH_SIZE 16384` + `extern g_pico_decompress_scratch`);
  `operations.c` (define global + prologue alloc); `decompress0.c` (`pico_decompress_alloc(type,size)` returns
  the scratch for `sci_pic`/`sci_view` when `size ≤ 16384`, else falls back to `sci_malloc`; the 1 alloc + 5
  error-path frees route through `DECOMPRESS_ALLOC_DATA`/`DECOMPRESS_FREE_DATA` macros, desktop path
  unchanged); `resource.c` (a `PICO_IS_DECOMPRESS_SCRATCH()` guard on **all four** `res->data` free sites —
  evict, LRU flush, LRU-age, teardown — so the shared scratch is never `sci_free`'d).
- **Why 16 KB / pic+view only:** measured device high-water (log 20bb9822 `[dcmp]` probe, since removed) was
  **pic 7506, view 10797** — both well under 16 KB. Scripts/vocab are **excluded by design** — their
  decompressed data stays resident (not evicted within the call), so they must not share a single reusable
  scratch; they keep using `sci_malloc`.
- **Safety:** all three pic/view load sites (`sci_resmgr.c:141`, `:467`, `operations.c:2285`) evict
  immediately and unconditionally, and pic/view are the *sole* consumers of those resource types — so the
  scratch is only ever live for one serial decode. The 4-site free guards are defense-in-depth.
- **Result (log 20bb9822):** restored into room 13, climbed the ladder, **room 15 loaded and rendered** (pic
  7506 + view 10797 both went through the scratch, NO `decompress0.c:324` OOM). The decode-output OOM lever is
  closed — it got the game *further* than any prior restore session.

### RESOLVED (device-confirmed 2026-06-21) — the two arena-ratchet OOMs are closed: restore keeps visual[0] resident + the chooser resets the arena between games

The arena ratchet manifested as **two distinct OOMs**, both now fixed and device-confirmed (multiple
in-game restores AND quit-SQ3→load-PQ2 all run with no crash):

1. **Post-restore OOM = the 64 KB `visual[0]` RE-ALLOCATION, not the ratchet itself** (`savegame.c`
   `gamestate_restore`, `HAVE_PICO`). The captured log decided it: after several restores the OOM was
   `pico_alloc_visual` (`pico_driver.c:72`, 64000 B) with **140 KB free but no 64 KB-contiguous run**, arena
   pinned at the physical ceiling. The restore relief was **freeing `visual[0]` before deserialization then
   re-allocating 64 KB after `fclose`** — trading the one 64 KB-contiguous block it already held for the
   deserializer's softer ~16 KB needs, but then unable to re-find 64 KB on the fragmented restore heap (the
   single largest contiguous requirement on the path). **Fix: do NOT free/re-alloc `visual[0]` on the restore
   path** — keep it resident throughout. The deserializer's ~16 KB holes still come from the retained LRU /
   script-buf / VM-value-stack frees (the VM stack free alone is a guaranteed 16 KB-contiguous hole). Removed
   both the `pico_free_visual` (was ~4828) and the post-`fclose` `pico_alloc_visual` (was ~4862). Note the
   per-room decode path already uses the visual[0]-REUSE pattern (decodes into the resident buffer, never
   frees it), so its `pico_alloc_visual` at `operations.c:2485` is a no-op while visual[0] is resident — the
   restore path was the *sole* remaining place still re-acquiring the 64 KB block.

2. **Cross-game-switch OOM = the next game inherits the prior game's maxed, fragmented arena** (`pico_main.c`
   chooser loop + `operations.c` `pico_reset_decode_scratches`). picolibc never returns sbrk'd memory, so
   after SQ3 ratcheted the break to the **physical ceiling** (475,100 B clean / 436,232 B diagnostic) on its
   first room decode, quitting to the chooser left the arena pinned there. Loading PQ2 (a heavier game: 1843
   vocab words vs SQ3's 1489) into that maxed/fragmented leftover OOM'd at `decompress0.c:50` (`malloc 8502
   failed`, 184 KB free but no contiguous run). **PQ2 from a cold boot loads fine** — proving it was purely
   the inherited arena, not PQ2 size. **Fix: reset the arena at the chooser, the one safe quiescent point**
   (unlike the restore path where `malloc_trim` was ruled out — see the RULED OUT note below — because it
   returned bytes the restore immediately needed). After `freesci_main` returns: free the two permanent decode
   scratches (B-1 32 KB priority + B-1.2 16 KB decompress — they otherwise pin the break high and block the
   trim), then `malloc_trim(0)` releases the now-free top of the heap via `sbrk`, so the next game grows from a
   low arena — a cold-boot heap without the power cycle. The next game re-allocates its scratches contiguously
   on its first pic decode. The new `[mem] post-trim` line shows the arena dropping off the ceiling.

This is the first safe win against the long-parked "lever 2": the ratchet *within* a single game session is
unchanged (the break still climbs to the ceiling during play and stays there until exit), but the two places
it became **fatal** — post-restore and cross-game — are now closed. The historical analysis below is kept for
the record (it correctly diagnosed the ratchet mechanism; the fix turned out to be removing the 64 KB churn
and resetting at the chooser, not driving the restore peak below the standing arena).

---

*Historical (the OPEN analysis that led here — arena-ratchet ceiling as the binding constraint, log 20bb9822):*

The decompress-scratch fix above unblocked room 15, which then hit the **next** OOM — and it is the parked
"lever 2" (arena ratchet), now fatal rather than a slow creep. Tail of log 20bb9822:
```
malloc 3926 failed to allocate memory   ← attempt
malloc 3926 failed to allocate memory   ← sci_malloc LRU-flush retry, no help (reslru=0 reslock=615)
ERROR in kScriptID L291: Script 0x3 does not have a dispatch table
... Attempt to send to non-object ... Address was 0000:0000   ← fatal VM abort
```
`kScriptID` (`kscripts.c:279`) called `script_get_segment(SCRIPT_GET_LOAD)` to instantiate script 3; a small
**3926-byte** alloc inside that load returned NULL (raw `malloc` → no `[OOM]` halt, game continued), so the
script came up with `exports_nr==0` (no dispatch table) and the subsequent VM `send` to it faulted fatally.

**Why a 3926-byte alloc fails: the arena is physically maxed.** Post-restore arena = **436200** (= 0x6A7E8).
Heap `__end__` 0x2001742c + 0x6A7E8 = heap top **0x20081C14**, only **~1004 bytes** below `__StackTop`
0x20082000 — i.e. the heap has grown to within ~1 KB of the absolute top of RAM and is **one sbrk increment
from the wall**. **A single restore did it:** room 2 arena 403432 → room 13 arena 436200 = **+32768** (exactly
one sbrk increment, never returned). With the arena maxed and `chunks=111+` in room 15, the shattered free
space can't yield 3926 contiguous bytes, and the LRU-flush retry frees nothing (all 615 resources locked).

**This is the same +32768/restore ratchet flagged (and parked) in the B-1 note above — now the active
blocker, not a creep.** Removing B-1's priority reservation did NOT kill it, so the cause is elsewhere: the
restore-time transient peak (2× working set during `gamestate_restore` deserialization) exceeds the standing
arena by a bit, triggering an sbrk grow of one 32 KB granule that picolibc never returns. **Lever:** drive the
restore peak *below* the standing arena so sbrk never grows. Investigation target = `savegame.c`
`gamestate_restore` + `vm.c` `_game_run` restore teardown/rebuild peak (NOT allocator self-reclaim — retry
can't conjure contiguity). **Tension to weigh:** the two permanent scratches (B-1 32 KB + B-1.2 16 KB = 48 KB
always-resident) raise the post-restore baseline; if the ratchet fix restores enough headroom that the 11 KB
decompress alloc succeeds normally, the 16 KB decompress scratch could be dropped to reclaim it. **Screenshot
(IMG_1693) confirms** room 15 background + hand cursor render cleanly (no garbage rect / corruption-family
artifact) — frame is merely incomplete because the VM aborted mid-load. Pure OOM, not a render bug.

**RE-CONFIRMED + EXACT CEILING from the ELF (FSCI_PROBE_ARENA build, pico.log 2026-06-12).** A restore into
the rats room (room 15) → climb ladder → enter next room reproduced this OOM byte-for-byte, and the
`[arenagrow]` probe pinned the numbers. The precise physical ceiling (from `arm-none-eabi-nm`): heap
`__end__` **0x200157f8** → `__HeapLimit/__StackLimit` **0x20080000** = **436,232 B max arena** (`__StackTop`
0x20082000, 8 KB main stack above). (Corrects the earlier ~436,748 / 436,200 estimates — the exact max is
**436,232**.) The session: pre-restore room 2 arena **403,464 / chunks 27** → post-restore room 15 arena
**436,232 / chunks 172**. So one restore ratcheted the arena **+32,768 to the byte-exact ceiling** (last
`[arenagrow]` line: `brk=20080000`, heap pinned against `__StackLimit`, zero growth room) AND fragmented it
**6×** (27→172). Climbing into the next room: `malloc 3926 failed` (script 3 load) → no 3926-contiguous run
among 172 fragments, arena can't grow → `kScriptID` no dispatch table → fatal send to 0000:0000. Same as
log 20bb9822.

**Telemetry finding — the dominant ratchet drivers are RAW mallocs the probe can't name directly.** The two
largest captured grows are mis-attributed to *tiny* `sci_*` allocs: `+32768 … req=256 … tools.c:720
sci_getcwd` and `+53248 … req=52 … sci_pic_0.c:282 gfxr_init_pic`. Per the probe's known limitation, the real
driver is the **raw `malloc` immediately before** each — i.e. the per-decode **32 KB control buffer**
(`sci_resmgr.c`) and **64 KB visual** / decompress buffers. The `sci_*`-routed grows the probe *can* see are a
minority; the arena climbed 387,080 → 403,464 across rooms 900/2 with **no** `[arenagrow]` lines at all
(all raw-malloc driven). **So to name the exact restore-time ratchet allocation, the probe must be extended to
the ~4 raw-malloc sites** (deferred visual `sci_resmgr.c:155`, early-pin `operations.c:2281`, 32 KB control,
decompress raw fallbacks) — otherwise it keeps attributing their grows to the next `sci_*` call. Lever is
unchanged (drive the restore peak below the standing 403,464 arena so the final +32,768 grow never fires);
the probe-extension is the cheap next diagnostic to name *which* restore alloc forces it.

### RULED OUT — `malloc_trim()` does NOT fix the arena ratchet (device-tested, reverted to HEAD baseline)

`malloc_trim(pad)` was tried as the un-ratchet lever and is a **dead end** for this OOM — do not re-attempt
cold. Two flavors, both failed:

- **Per-room `malloc_trim(0)` (operations.c, end of `gfxop_new_pic`)** — maximally aggressive (strips the top
  chunk to ~300 B via `_sbrk(negative)`). **Caused a regression:** the next restore's reconstruct then
  sbrk-grew from the stripped floor and `malloc 7302 failed` mid-reconstruct. It returned bytes the restore
  immediately needed back, fragmenting the rebuild.
- **Padded valley `malloc_trim(16384)`** — intended to un-ratchet "less aggressively." It was a **no-op**: at
  every room valley the top free chunk was smaller than the 16 KB cushion, so newlib released nothing (no
  `_sbrk(negative)` ever issued). Zero effect on the arena; the run still OOM'd on play-path-variance
  fragmentation, not the trim.

**Two facts established along the way (keep — they correct earlier guesses):**
- **The "trim poisons sbrk" theory is WRONG.** A post-lightinit `malloc_trim(0)` stripped to keep≈304, yet the
  following reconstruct sbrk-grew the arena 395276 → 432140 with no corruption. newlib `_sbrk` (pico-sdk
  `newlib_interface.c`) cleanly honors a later positive grow after a negative trim; the only guard is
  `next_heap_end > __StackLimit`. So an earlier-session regression blamed on "trim poisoning sbrk" was actually
  fragmentation/play-path variance.
- **True physical ceiling re-confirmed from the ELF:** heap `__end__` grows up to `__StackLimit/__HeapLimit`
  0x20080000; `__StackTop` 0x20082000. **Max arena ≈ 436,748 B (0x6A80C).**

**Conclusion:** `malloc_trim` (any pad) can't help — releasing the top chunk at a valley either returns memory
the next peak immediately re-sbrk's (ratchet unchanged) or fragments the very rebuild that needs contiguity.
The real lever stays **driving the restore-time transient peak below the standing arena** so sbrk never grows
(savegame.c `gamestate_restore` + vm.c `_game_run` teardown/rebuild peak), NOT allocator self-reclaim. All trim
experiments were reverted — vm.c/savegame.c/operations.c are back to **zero diff vs HEAD** (the room-15 build).

### RULED OUT — "free CFSML script bufs before reload" (Codex rescue rank-1) — HardFaulted, premise was false

A Codex `/codex:rescue` pass proposed, as its top-ranked un-ratchet fix, that the restore path **leaks the
old script bytecode buffers**: it claimed `_cfsml_read_script_t` re-allocates `script_t.buf` at
`savegame.c:1818`, duplicating each script's ~KBs across the deserialization → +32KB ratchet. The fix was to
`sci_free(scr->buf)` in `load_script` before the reload. **Implemented, then it HardFaulted immediately on the
first restore** (UNALIGNED UsageFault, CFSR=0x01000000, PC in `_malloc_usable_size_r`, LR in `_SCI_FREE`).

**The premise is FALSE — `script_t.buf` is never serialized.**
- `_cfsml_read_script_t` (savegame.c ~3411) reads `nr`/`buf_size`/`script_size`/`heap_size`/`obj_indices`/
  `exports_nr`/… but **not** `buf`. The `buf` reader/allocator at **line 1818 belongs to
  `_cfsml_read_dynmem_t`** (a different struct with its own `buf` field), NOT `_cfsml_read_script_t`. Codex
  mis-attributed the line.
- So at `load_script` entry `scr->buf` is **uninitialized garbage** (the seg-manager heap slot was just
  rebuilt); the original `scr->buf = malloc(scr->buf_size)` is a **first-time init**, not an overwrite of a
  retained pointer. There is **no leak and no duplicate peak** here.
- The HardFault was `_SCI_FREE` (sci_memory.c) calling `malloc_usable_size(garbage_ptr)` for its
  `g_sci_live_bytes` accounting *before* `free()` → faults inside `_malloc_usable_size_r` on the bogus
  pointer. Reverted — savegame.c back to zero diff vs HEAD.

**Lesson:** verify which function a cited `savegame.c:NNNN` line actually sits in (the CFSML file is one giant
generated file, many near-identical `buf` readers) before freeing anything on the restore path. The arena
ratchet is NOT a script-buf leak.

### TELEMETRY — `[arenagrow]` probe to NAME the alloc that triggers the sbrk grow (`FSCI_PROBE_ARENA`, default OFF)

Since the +32,768 B/restore ratchet root cause is still unexplained (genuine deserialization 2× working-set
peak is the prime suspect, but unproven), guessing sites is what produced the rank-1 dead-end above. Instead,
**name the allocation that actually grows the program break.** New top-level CMake `option(FSCI_PROBE_ARENA)`
(OFF; ON adds `-DFSCI_PROBE_ARENA=1`). When ON + `HAVE_PICO`, `sci_memory.c` defines `pico_arena_grow_probe`:
each `_SCI_MALLOC`/`_SCI_CALLOC`/`_SCI_REALLOC` reads `sbrk(0)` (O(1) program-break read, no heap walk) and,
when it moved up since the last sci_* alloc, prints
`[arenagrow] +<delta> brk=<addr> req=<size>  <file>:<line> <funct>`. Diff the `[arenagrow]` lines across a
restore to see exactly which sci_* call site forced each ~32KB sbrk granule.

- Build the diagnostic firmware: add `-DFSCI_PROBE_ARENA=ON` to the Pico configure (already wired).

**EXTENDED to the raw-malloc decode/restore sites (2026-06-12).** The first device run (pico.log, rats-room
restore → ladder → next-room `malloc 3926` crash) proved the `sci_*`-only probe's blind spot is the *whole
story* here: the arena climbed 387,080 → 403,464 across rooms with NO `[arenagrow]` lines (all raw-malloc
driven), and the two biggest captured grows were mis-attributed to tiny `sci_*` allocs (`+32768 req=256
sci_getcwd`, `+53248 req=52 gfxr_init_pic`) — the real drivers were the raw `malloc`s right before them. So
`pico_arena_grow_probe` was renamed `pico_arena_probe`, made non-static (declared in `sci_memory.h`), and its
program-break watermark (`pico_arena_last_brk`) is now **shared** with a raw-site macro `PICO_ARENA_PROBE_RAW(sz)`
(also in `sci_memory.h`; no-op unless `FSCI_PROBE_ARENA`, and a non-Pico fallback so unguarded call sites still
compile on desktop). Each raw decode/restore `malloc` calls it *immediately after* the alloc, so a grow lands on
the real culprit instead of the next `sci_*` call. Instrumented raw sites:
  - **Per-decode:** `sci_resmgr.c` visual 64KB deferred (`:162`) + priority 32KB fallback (`:190`);
    `operations.c` priority-scratch one-time (`:2297`), decompress-scratch one-time (`:2305`), early-pin visual
    64KB (`:2370`).
  - **Restore path (the ratchet suspects):** `savegame.c` `load_script` `scr->buf` per-script bytecode
    (`:4500`) + the CFSML `read_*_tp` raw struct allocs (`song_t` `:3890`, `int_hash_map_t` `:3917`,
    `int_hash_map_node_t` `:3967`).
  - **Already `sci_*`-routed (no raw site, covered by the macro probe):** `decompress0.c` decompress output
    (`:50` → `sci_malloc`), decrypt1 token buffers (`:113/:114`), script/vocab decompress (`:348`); visual[0]
    (`pico_init_specific` → `sci_malloc`); all seg-manager rebuild allocs.
- Both configs build clean; `pico_arena_probe` confirmed linked in the Pico ELF. **Next device run with this
  build should NAME the exact restore-time allocation forcing the +32,768 grow** (diff `[arenagrow]` across the
  restore — watch especially the `savegame.c:4500 load_script` lines during `reconstruct_scripts`).

### REVERTED — the compressed-INPUT decompress scratch was NET-NEGATIVE (device-measured, 2026-06-12)

A third permanent scratch (16KB, for `decompress0.c`'s compressed-INPUT read `buffer`, the `:348`/`sci_malloc(compressedLength)`
site) was added on top of B-1 (32KB priority) and B-1.2 (16KB decompress-OUTPUT), then **reverted** the same day.
It is a confirmed dead end — do not re-add it. The input buffer's lifetime DID make it scratch-safe (alias-free:
freed within `decompress0` before return, decode is core0-serial, exactly one live), so the idea was sound; the
problem is the **arena cost outweighs the benefit**.

**Device data (`[arenagrow]`, FSCI_PROBE_ARENA build):** at the boot's first pic decode the three permanent
scratches grow the break in lockstep —
```
+32768 req=32000  operations.c gfxop_new_pic  ← priority scratch (B-1)
+20480 req=16384  operations.c gfxop_new_pic  ← decompress-OUTPUT scratch (B-1.2)
+20480 req=16384  operations.c gfxop_new_pic  ← decompress-INPUT scratch (the reverted one)
```
i.e. **72KB of arena consumed at the very first decode**, of which my input scratch was a permanent **+20KB**
(16KB nominal, 20KB after the sbrk granule). The thing it was meant to fix (the `:348` input OOM) was no longer
the failure after it landed; the OUTPUT path (`decompress0.c:50`, script/vocab decompress, which canNOT share a
scratch — locked-resident, multiple live) OOM'd instead, and OOM'd *harder* because the input scratch had eaten
20KB of headroom. A transient buffer (one live at a time) is better served by general heap headroom than by a
permanent reservation. **Lesson: a permanent scratch only pays off for an allocation whose *contiguous* failure
is otherwise unavoidable on a fragmented heap (priority/decompress-output decode buffers); for a small transient
that `sci_malloc` can usually place, the always-resident cost is pure loss.**

**Device follow-up (post-revert, DIAGNOSTIC build, 2026-06-12):** climbing the ladder in the rats room (room 3)
OOM'd cleanly at exactly the reverted site — `[OOM] decompress0.c:348` (`line=0x15c`), `size=7725`,
`arena=0x6a808`=436232 (**the exact physical ceiling**), `free=40392` but fragmented. So the revert *did*
reintroduce the `:348` input OOM — **BUT only on the diagnostic firmware**, which sits ~26KB under the ceiling
(see the Codex assessment's clean-vs-diagnostic span). The user confirms this same OOM **does NOT occur on Codex's
probe-free clean build** — the ~26KB the probes cost is the whole margin here. Strongest evidence yet that (a) the
revert is correct (the input scratch's permanent 20KB was worse than the occasional `:348` miss), and (b)
**viability MUST be judged on a clean build** — the diagnostic build's 26KB overhead manufactures OOMs the shipping
configuration does not have. The legible `[OOM]` halt (vs a HardFault) is also reconfirmed.

### DONE (device-confirmed legible, this build) — `load_script` NULL-check turns a restore-OOM HardFault into a clean `[OOM]`

`load_script` (`savegame.c`, the `reconstruct_scripts` restore path) called `scir_find_resource(...sci_script...)`
then immediately `sm_mcpy_in_out(..., script->data, script->size, ...)` with **no NULL-check on `script`**. On the
tight post-restore heap the resource load can fail (`opendir`/`RESOURCE.NNN` small mallocs fail → `Resmgr: Failed
to read script.NNN` → `scir_find_resource` returns NULL), and the `memcpy` then read from a garbage source pointer
→ **HardFault** (IMG_1695: PC=`memcpy`, LR=`load_script` `savegame.c:4514`, BFAR=`0xf0000000`). `sm_mcpy_in_out`
already guards its *dest* (`scr->buf`), so only the *source* (`script->data`) was unguarded. Fix: bail with a
`sciprintf` + `pico_oom_report` (Pico) when `script` (or `heap` for SCI1.1) is NULL — converts the
memcpy-from-garbage fault into a legible `[OOM]` LCD dump naming `load_script`. Shared-engine fix (the NULL-deref
was always latent); the `pico_oom_report` halt is `HAVE_PICO`-gated, desktop just returns. **Device-confirmed:** the
next run produced a clean `[OOM]` (at `decompress0.c:50`, *before* reaching `load_script` this time) instead of a
HardFault — the legibility path works.

### KEY FINDING — the arena hits the PHYSICAL CEILING during room-2 GAMEPLAY, before any restore (log, 2026-06-12)

The decisive insight from the post-revert-era logs: **the restore is not where the arena maxes out — normal
gameplay is.** The four `[arenagrow]` lines immediately before the restore trigger (`Activating port 1 after
disposing window 4`) show the break reaching `0x20080000` (the absolute ceiling, max arena 436,232 B) during
**room-2 play**, driven by *tiny* allocations on a shattered heap:
```
+4096 brk=2007D000 req=1998  gfx_tools.c:307 gfx_pixmap_alloc_index_data
+4096 brk=2007E000 req=2060  reg_t_hashmap.c:42 new_reg_t_hash_map
+4096 brk=2007F000 req=12    reg_t_hashmap.c:42 reg_t_hash_map_check_value
+4096 brk=20080000 req=2060  reg_t_hashmap.c:42 new_reg_t_hash_map   ← CEILING, on a 2060-byte GC alloc
```
Each forces a fresh +4096 sbrk grow because the fragmented heap has no free chunk even for **12 bytes**. So by the
time a restore runs, the arena is already pinned at the ceiling; the restore rebuild then OOMs on an ordinary
7-12KB decompress (`decompress0.c:50`) for lack of a contiguous run (`free` ~15KB total but fragmented). **The
failure class has moved DOWN** — old walls were 64KB/32KB contiguous; now ordinary 7-12KB allocs fail post-restore.
The binding constraint is picolibc fragmentation denying small/medium contiguous runs near the ceiling, NOT total
free bytes and NOT PSRAM capacity (PSRAM is already used for everything offloadable; `script_t.buf` is hot RW VM
memory, ruled out).

### CURRENT STATUS (2026-06-22) — the "arena ratchet" is no longer a restore-chain problem; the remaining issue is gameplay-time FRAGMENTATION to the ceiling

Reframing after the 2026-06-21 fixes: **the arena ratchet does NOT have a restore-chain problem anymore.** The
two places it was *fatal* are closed and device-confirmed (post-restore visual[0] kept resident; the chooser
resets the arena between games — see the RESOLVED 2026-06-21 note). Long in-game restore chains run with
comfortable headroom. So "fix the restore-time transient peak / un-ratchet the restore" is **no longer the
framing** — do not chase it as a restore bug.

**What actually remains is plain heap fragmentation, and it is a GAMEPLAY phenomenon, not a restore one** (the
KEY FINDING above is the evidence): the arena climbs to the physical ceiling during ordinary room-2 *play*,
before any restore is involved, because normal play shreds the free list — room changes re-decode pics (big
transient alloc/free) interleaved with message-window / parser churn (small alloc/free). picolibc never
coalesces or returns the sbrk'd top, so:

- **The binding constraint is CONTIGUITY, not total free bytes, and not PSRAM capacity.** A 12-byte GC alloc
  forcing a +4096 sbrk grow (line 1109) proves the free space — tens of KB total — was shattered into chunks
  with no hole even for 12 bytes. Fresh boot is 17–40 chunks; play drives it to 100–200.
- **No leak is involved** — the leaks are fixed (cwd + console scrollback, clone-variables). This is pure
  fragmentation pressure, so byte-reclaim levers can't help: `malloc_trim` returns bytes the next peak
  re-grabs (RULED OUT), GC-on-OOM faults at unsafe points (RULED OUT), `.bss` mining is tapped out, and a
  read-only PSRAM script cache is impossible (`buf` is hot RW).
- **The ONLY safe attack is reducing transient-allocation CHURN** so the free list stops shredding — reuse a
  resettable scratch for the clearly-serial transient decode/parse buffers, exactly the pattern already
  applied: the permanent priority/decompress scratches (B-1/B-1.2) and the view-cel decode-into-idle-scratch
  (B-1.3). The remaining candidates of the same shape: the control/priority decode temporaries and the GNF
  parse transients (and the `gfx_tools.c:307` / `reg_t_hashmap.c:42` churn the `[arenagrow]` log names). This
  is incremental fragmentation reduction, not a single fix — and per the Codex assessment below, do not expect
  it to buy comfortable margin, only to push the ceiling-hit later.
- **NB the `reg_t_hashmap.c:42` GC-churn candidate above was TRIED and is net-negative — see the next note.**

### TRIED + REVERTED (2026-06-28) — GC/dirty-rect churn pools (L1/L2/L3) + GC-interval raise; do NOT re-attempt cold

The churn-reduction candidates named in the bullet above were all built behind `HAVE_PICO`, device-tested, and
**fully reverted** (`git checkout` — never committed). Recorded so they aren't redone blind:

- **L3 — persist the two GC `reg_t_hash_map`s** (a `clear_##TYPE##_hash_map` macro fn + `gc_acquire_map` in
  `gc.c`, allocate-once + clear-for-reuse instead of new/free per GC). **It WORKS at its narrow goal** —
  device `[arenagrow]` showed `reg_t_hashmap.c:42` (the named ratchet driver) fire **once** instead of
  repeatedly. **But it is net-negative:** the ~9KB resident (2 map headers + node pool) is **peak-NOT-neutral**
  and was held *through* in-game restores, raising the restore peak → the spaceship-hatch scene OOM'd at the
  physical ceiling (`malloc 9128 failed` → `kScriptID` no-dispatch → debugger) on a scene that was playable
  before. The GC churn it removes helps *steady play*, not the *restore peak* that's actually binding. A
  `game_exit`-time pool-free was considered (would make it restore-neutral) but not pursued — the fragmentation
  wall is at the ceiling regardless. **Lesson: a permanent GC-working-set scratch is the wrong shape — its cost
  lands on the restore peak, its benefit doesn't.**
- **L1 (dirty-rect node freelist, `operations.c`) + L2 (GC worklist chunk pool, `gc.c`)** — cheap (<2KB), low
  risk, but their fragmentation benefit was **unproven in the device log** (they don't force `[arenagrow]`
  grows, so the win is invisible). Reverted alongside L3.
- **`GC_INTERVAL` 2048→4096** (collect half as often to cut GC-churn) — device-tested clean (no debug console,
  **no `alloc_clone_entry` OOM** across a 3-restore session) but its benefit was **unattributable** (the L3
  revert in the same build did the real work) and it re-opens the documented clone-table-growth risk (2048 was
  chosen *because* a longer interval OOM'd `alloc_clone_entry` — see `vm.h`). Reverted to 2048.

**Bottom line:** the `reg_t_hashmap`/GC churn lever is *exhausted* (works but net-negative); the dirty-rect and
worklist pools are *unproven and not worth the complexity*; raising `GC_INTERVAL` trades GC-churn for
clone-table-growth and didn't clearly help. The still-untried churn candidates are only the **control/priority
decode temporaries and the GNF parse transients** — and per Codex, don't expect comfortable margin from them.

### ASSESSMENT (Codex, `PICO_SQ3_SRAM_CEILING_ASSESSMENT.md`, 2026-06-12) — at the practical SRAM ceiling

An independent Codex assessment (file in repo root) concurs with the above and adds two load-bearing facts:

1. **Diagnostic probes cost ~26KB of heap ceiling.** Clean current-feature build `__end__=0x2000f154` (heap span
   **462,508 B**) vs the diagnostic `build-pico` `__end__=0x200157fc` (heap span **436,228 B**) — a ~26KB
   difference. **Viability must be judged on a CLEAN build** (`FSCI_PROBE_*=OFF`, keep `FSCI_PROBE_STR=ON`,
   `PICO_CONTROL_MAP=ON`, `PICO_PACK_VOCAB=ON`, `PICO_PWM_AUDIO=OFF`); diagnostic firmware turns "barely works"
   into "fails early." Use probes to find causes, then retest clean.
2. **Best remaining engineering lever = a reusable transient view-cel decode scratch.** View cel `index_data` is
   already offloaded to PSRAM *after* decode, but each cel still allocates a transient SRAM `index_data` *during*
   decode — a fragmentation source. Decoding cels into a reusable scratch then storing to PSRAM attacks transient
   fragmentation without adding steady-state SRAM. A small **resettable SRAM arena** for clearly-serial decode/parse
   buffers (view-cel, pic/control decode temporaries, GNF transients) is the more general version of this — narrow
   and explicit, not a general allocator replacement (lifetime mistakes are the risk).

**Codex's verdict (and the working assumption now):** SQ3-without-sound can probably be made to fit "well enough"
with one more focused pass on transient scratch/fragmentation, but the port is at the practical ceiling — do NOT
expect another clean 50-100KB win, and treat "SQ3 + sound + robust arbitrary-length restore chains + comfortable
margin" as out of reach. Things NOT worth chasing further: more `.bss` mining (tapped out), audio while headroom
is this tight, a read-only PSRAM script cache (mutable `buf`), GC-on-OOM (faults at unsafe moments), `malloc_trim`
(tried, ineffective/harmful).

### DONE (device-confirmed, clean-build ladder clear) — B-1.3 view-cel decode borrows the idle priority scratch

Codex's "best remaining lever" (above) is implemented: `gfxr_draw_cel0` (`sci_view_0.c`, `HAVE_PICO`) no longer
`malloc`s a fresh per-cel `index_data`. Each cel is already offloaded to PSRAM immediately after decode (so the
SRAM peak was already a single cel), but the per-cel `gfx_pixmap_alloc_index_data` `malloc`/`free` **churn** was a
confirmed fragmentation driver (`[arenagrow]` lines `gfx_tools.c:307 req=1998 gfx_pixmap_alloc_index_data`). Now a
cel decodes straight into the **idle 32KB `g_pico_priority_scratch`** (B-1's permanent pic-priority buffer), then
`psram_store`s from it — **zero new steady-state SRAM**, no per-cel heap traffic.

- **Why borrowing the priority scratch is safe:** the priority scratch is only live *during* pic decode — its
  contents are offloaded to PSRAM inside `gfxr_interpreter_calculate_pic` and `state->priority_map` keeps only the
  PSRAM metadata (`index_data` NULL) afterward (`operations.c` ~2441). View decode is a *separate*,
  non-overlapping resmgr call on core0, so the scratch is idle and its data already safe in PSRAM. Cels are
  decoded → stored serially, so the scratch is reused only after the prior cel is in PSRAM.
- **Implementation detail that matters:** `retval->index_data` stays **NULL** on the borrow path (the scratch is
  held in a local `dest`), so the decode error paths' `gfx_free_pixmap` can never free the borrowed scratch. A
  near-fullscreen cel (`xl*yl > 32000`) falls back to the old per-cel `gfx_pixmap_alloc_index_data` + `free`. The
  PSRAM offload `free`s `dest` only `if (!dest_is_scratch)`. Desktop (`#else`) path is byte-for-byte unchanged.
- **Device result (clean build, log this session):** the per-cel `gfx_tools.c:307` arena-grows are **gone** (only
  2 such grows in the whole session, both non-view pixmaps — cursor/text/pic-aux). Decode peak unchanged.
- **Clean-build viability CONFIRMED.** Built probe-free (`build-pico-clean`: `FSCI_PROBE_*=OFF`,
  `FSCI_PROBE_STR=ON`, `PICO_CONTROL_MAP=ON`, `PICO_PACK_VOCAB=ON`, `PICO_PWM_AUDIO=OFF`). Clean `__end__=0x2000f150`
  → heap span **462,512 B** vs the diagnostic `build-pico` `__end__=0x200157f8` → **436,232 B** = **+26,280 B**
  (~25.7KB) headroom, exactly the probe cost. On the **diagnostic** build the rats-room ladder OOM'd at the exact
  ceiling (`malloc 3926 failed` → `kScriptID` no dispatch table → send to `0000:0000`, arena pinned at 436,232);
  on the **clean** build the player **cleared the ladder** — the +26KB is the whole margin. This is the concrete
  proof of the assessment's "viability MUST be judged on a clean build" and "SQ3-without-sound fits well enough at
  the ceiling with one transient-fragmentation pass." The arena ratchet itself is NOT fixed (churn-reduction ≠
  ratchet-stop); the clean build buys back the headroom the probes ate, and the view-cel fix lowers the
  fragmentation rate that climbs to the ceiling.

### RESOLVED (device-confirmed, log e8d3f32a) — second-restore vocab OOM is the SAME fragmentation class, one layer up

**Device-confirmed fixed (log e8d3f32a):** three consecutive in-game restores all printed `Pico: parser
vocab REUSED resident, 1489 words` (`game.c` `_init_vocabulary` Pico/PACK branch) — the packed blob is now
allocated once and reused across restores, so the `vocab.c:206` re-pack never runs on the fragmented heap.
No `malloc 29844 failed`, no vocab `[OOM]`. (That session later HardFaulted on an unrelated heap/stack
collision — see next note — but the vocab path itself is solved.)

Log ce81217b hit a NEW OOM on the **second** restore: `malloc 29844 failed` at `vocab.c:206` inside
`vocab_pack_words`, during "Initializing vocabulary" (free=119032 total but arena=425948 fragmented → no
29844-byte **contiguous** run). Decoded: the restore path tears down + rebuilds the engine
(`script_free_engine` → `_free_vocabulary` frees the packed-words blob; `script_init_engine` →
`_init_vocabulary` → `vocab_pack_words` re-packs it, `game.c:142`). The re-pack needs one contiguous 29844
block — the same fragmentation-OOM class B-1 just fixed for priority, now for the packed vocab. (`vocab.c:206`
*already* has a graceful unpacked fallback on malloc-NULL, but on Pico `sci_malloc` → `pico_oom_report`
**halts before** the NULL return is reached.)

**Fix (lead + C, mirrors B-1 and the resmgr-stays-resident restore design):**
1. **Keep the packed-words blob resident across restore.** Vocab is input-independent (identical all game) and
   the 29844 blob is *already* a permanent baseline cost — so pack it **once** into a static
   `g_pico_vocab_blob`, never free it (`_free_vocabulary` skips it on Pico), and on a later `_init_vocabulary`
   (restore) reuse it (re-point `s->parser_words`, no malloc). ~0 extra steady-state SRAM; eliminates the
   re-pack contiguous malloc entirely. Suffices/branches stay re-loaded (small ~6 KB contiguous allocs that
   fit even fragmented — left as-is to keep the change minimal).
2. **(C) belt-and-suspenders:** route `vocab_pack_words`' own alloc through raw `malloc` (not `sci_malloc`) so
   its existing graceful unpacked-fallback can actually fire on any *other* large vocab alloc instead of
   halting — degrades to unpacked (parser still works) rather than `[OOM]`.

### FIXED (pending device retest) — `decrypt1` HardFault is a HEAP/STACK COLLISION, not heap corruption (IMG_1683 + log e8d3f32a; RECURRED IMG_1685 + log b2a2b59f, 2026-06-08)

After the vocab + B-1 fixes, a session restored several saves, transitioned rooms (777→900→2, restore→9,
restore→12, restore→11→10→9→12→13) and HardFaulted in **room 13** with **plenty of free heap** (last
breakdown: `free=55672 arena=412628 chunks=206`) and **no `[OOM]` / no `malloc N failed`** anywhere near the
crash. LCD dump (IMG_1683):

```
HardFault PC=0x1006b858   → decrypt1, decompress0.c:153 (strh tokenlengthlist[tokenctr]=...)
LR  =0x00001019           → garbage (points into bootrom) — corrupted frame
xPSR=0x09100000
CFSR=0x00008200           → BFSR = 0x82 = BFARVALID | PRECISERR (precise data bus fault)
HFSR=0x40000000           → FORCED
BFAR=MMFAR=0x22ea52ea     → wild faulting address (not SRAM, not any mapped region)
```

**Root cause — the main stack is 8 KB but `decrypt1`'s frame is 16.4 KB, so it overflows into the heap.**
Linker layout (`arm-none-eabi-nm`): heap `__end__=0x2001742c` grows **up** to `__StackLimit=0x20080000`;
core0 stack `__StackTop=0x20082000` grows **down** — only **8 KB** of main stack
(`0x20080000`→`0x20082000`), **no MPU guard**. `decrypt1` (`decompress0.c`) puts `guint16 tokenlist[4096]` +
`guint16 tokenlengthlist[4096]` = **16384 B on the stack** (epilogue `add sp,sp,#16384`+`add sp,#28` ≈
16.4 KB) — the **single largest stack frame in the program, 2× the entire main stack**. Every decompress
drops SP ~16 KB *below* `__StackLimit`, straight into the heap region; it only works while the heap top is
far enough down. Disassembly of the faulting store:
```
add.w r4, sp, #8192 ; adds r4,#24   → r4 = tokenlengthlist base (SP-relative)
ldr.w ip, [sp, #20]                  → ip (tokenctr) reloaded from a SPILLED stack slot near frame bottom
strh.w r5, [r4, ip, lsl #1]          → tokenlengthlist[ip] = r5  (ip used UNMASKED; uxth is AFTER)
```
The spilled `tokenctr` at `[sp,#20]` sits near the frame bottom; once the **arena ratcheted** the heap top up
into that range, live heap data overwrote it → garbage `ip` → `(SP+0x2018)+ip*2 = 0x22ea52ea` → wild store.
`decrypt1` is the **victim/canary** (deepest frame), not the cause; the corruptor is the heap growing into
the stack.

**Why now, not at boot — the arena ratchet (the lever B-1 did NOT fix).** Heap top over the session: boot
room 777 arena 347092 → top ≈ `0x2006bfe0` (~80 KB clear of the stack, safe); room 13 arena 412628 → top ≈
`0x2007c080`, leaving **<16 KB** to `__StackLimit` — less than `decrypt1`'s frame → collision. No `[OOM]`
because nothing called `malloc` and failed; the stack silently overlapped live heap. **This is likely also
the real mechanism behind some of the open "aspb" corruption faults** — large frames (`decrypt1`, recursive
`run_vm`) overflowing the 8 KB stack into the heap, and vice-versa.

**RECURRED + CONFIRMED (IMG_1685 + log b2a2b59f, 2026-06-08).** After the view-RLE "aspb" fix (which DID
land — that room-13 fault is gone), a second restore into room 13 reproduced the *decrypt1* fault exactly:
LCD dump `PC=0x1006b87c` (= `decrypt1`, `decompress0.c:153`, instr `strh.w r5,[r4, ip, lsl #1]`),
`CFSR=0x8200` (BFARVALID|PRECISERR), `BFAR=MMFAR=0x4e468a2a` (wild — corrupted `tokenctr`). The log's own
breakdown predicted it: restore #2 arena=420820 → heap top `__end__`(0x2001742c)+0x66c04 = **0x2007e030**,
only **0x3FD0 ≈ 16.3 KB** below `__StackTop` 0x20082000 — just under `decrypt1`'s 16.4 KB frame. Same
instruction, same signature as IMG_1683. Decisive confirmation of the collision.

**FIX APPLIED (`decompress0.c`, `HAVE_PICO`-gated).** `tokenlist[4096]` + `tokenlengthlist[4096]` are now
`static guint16 *` file-scope-lifetime pointers, lazy-`sci_malloc`'d once on the first `decrypt1` call and
reused forever (never freed — game-independent scratch; `decrypt1` is core0-serial so non-reentrancy is
fine). Desktop keeps the on-stack arrays (`#else`) — its stack is huge and a perpetual 16 KB heap charge
there is pointless. **Verified in the Pico ELF:** `decrypt1`'s frame dropped `sub sp,#16384`+28 →
**`sub sp,#28`**, and **no 16 KB+ stack frame remains anywhere** in the binary; the largest is now **4128 B**
(~4 KB, well under the 8 KB stack) — so the heap must ratchet ~12 KB *further* before even those are at
risk. Lazy-malloc preferred over static `.bss` (which would permanently lower the arena ceiling even when
not decompressing). **Awaiting device retest.** The arena ratchet remains the underlying pressure (a
separate, secondary lever — see the arena-ratchet diagnosis); this fix removes the *largest* frame so the
collision is closed for the foreseeable arena range, but if the arena keeps climbing the next-largest frames
(the two ~4 KB ones) would eventually need the same treatment. **Also audit** `kpathing.c` and deep
`run_vm` recursion if it ever recurs at a ~4 KB frame.

**DEVICE-CONFIRMED FIXED (log 3396e873, 2026-06-08).** Same restore-into-room-13 + robot-ZOT-death
sequence that HardFaulted at `decrypt1` before now produces **no HardFault** — instead a clean `[OOM]`
halt at `decompress0.c:324` (`result->data = sci_malloc(11127)`), `free=30472 arena=424908`. The
stack/heap collision is gone (the off-stack arrays did their job); what surfaced underneath is the
*fragmentation* OOM (next note), which is the real remaining wall — NOT a regression of this fix.

### FIXED (pending device retest) — "aspb" corruptor found: mirrored view-RLE branch overruns index_data (room-13 fault, log 8b21d51a + IMG_1684)

A room-13 HardFault that is the **heap-corruption ("aspb") family**, NOT the decrypt1 stack collision
(this run had ~27 KB stack headroom, arena 400340). LCD dump (IMG_1684):
```
HardFault PC=0x10089cd8  → _malloc_r (_mallocr.c:2597), faulting instr str r3,[r2,#4]
CFSR=0x00008200          → BFSR = BFARVALID|PRECISERR (precise data bus WRITE)
HFSR=0x40000000          → FORCED
BFAR=MMFAR=0x200bdd64     → ~244 KB ABOVE SRAM top (0x20082000) — wild remainder-chunk pointer
```
Decode: the faulting store writes a **split-remainder chunk header** (`str r3,[r2,#4]`, r2≈0x200bdd60)
where `r2` was computed from a **corrupted chunk size field** → wild address → fault. So malloc is the
*canary*; an earlier OOB write smashed a chunk header. The room-13 `[mem] BREAKDOWN` confirms it:
`gfxpxm … big=ffffffff … 983147 B` — a `gfx_pixmap_t` size field smashed from ~2596 → 983147 B, plus
`gfxr_draw_cel0() L125 … writes RLE data over its designated end`, `invalid view 901`, `Gray magic 0202`.

**Root cause — `gfxr_draw_cel0` (`sci_view_0.c`), the MIRRORED branch lacks the bound check the
non-mirrored branch has.** The non-mirrored path (≈L113-131) guards every run with
`if (writepos + count > pixmap_size) { GFXERROR("…over its designated end…"); return NULL; }`. The
mirrored path's inner fill was `while (count)` — it checks **only `count`, never `yl`**. The fill wraps
lines backward (`writepos`/`line_base` advance by `xl` each line); once the last line is consumed
(`yl`→0) those pointers march **past `dest = index_data` (sized `xl*yl`)**, and a run with leftover
`count` `memset`s off the end into the adjacent heap chunk's header → the smash malloc later trips on.
Shared-engine code (no `HAVE_PICO` guard), so desktop survives the same write on slack — textbook "aspb".

**Fix (`sci_view_0.c`, shared, mirrored branch):** bound the inner loop on `yl` too
(`while (count && yl)`), then after it, leftover `count` is a genuine overrun → `gfx_free_pixmap` +
`GFXERROR(...over its designated end...)` + `return NULL`, exactly mirroring the non-mirrored branch
(it additionally frees the partial pixmap to avoid a leak on the corrupt path, like the `xl<=0` guard).
Builds clean desktop + pico. **Awaiting device retest** to confirm the room-13 fault is gone. If a
HardFault with a garbage/ASCII BFAR still appears after this, there is a *second* overflow source (the
string-kernel suspects below remain the next ASan target).

### CLOSED (2026-06-23) — heap corruption surfacing as a GC fault ("aspb")

**CLOSED (2026-06-23, user decision).** Removed from the open/watch list: the "aspb" corruption signature has
**not reappeared** on any clean- or diagnostic-build session since the view-RLE + decrypt1 fixes landed
(2026-06-08), so the every-observed instance is accounted for by those two fixes. The unconfirmed string-kernel
theory (below) is treated as resolved — most likely (b): the observed "aspb"-signature fault was always a
downstream symptom of the view-RLE overrun, already fixed. The `FSCI_PROBE_STR` canary stays **ON** as cheap
standing insurance (it costs only its small share of the ~26 KB diagnostic-probe ceiling and is the one probe
left at default-ON); if a `[strprobe]` line ever fires just before a `[FAULT]`, or a garbage/ASCII-BFAR +
no-`[OOM]` HardFault recurs, **re-open** this and run the desktop ASan hammer below. The detail is retained
for that contingency, but it is no longer tracked as active or watched work.

*Historical (the DOWNGRADED analysis that led to closing it):*

**DOWNGRADED from top-priority OPEN (2026-06-12).** The "aspb" family was a *conflation* of several
distinct faults. The ones we **actually observed on device** have each been individually root-caused and
fixed:
- **Mirrored view-RLE overrun in `gfxr_draw_cel0` (`sci_view_0.c`) — this was "the 'aspb' corruptor
  found."** The mirrored branch lacked the `yl` bound the non-mirrored branch had, so a leftover-`count`
  run `memset` past `index_data` into the adjacent chunk header — exactly the metadata smash below. FIXED
  and landed (room-13 fault gone). See the RESOLVED note above.
- **decrypt1 16.4 KB stack frame overflowing the 8 KB main stack into the heap** — DEVICE-CONFIRMED FIXED
  (log 3396e873). A second corruption mechanism that produced garbage-BFAR HardFaults.
- The 4ded2752 branch-to-NULL and grabber/motivator faults were **reclassified as fragmentation-OOM**
  (longjmp-into-dead-frame), not metadata smashes — the reg_t-shaped garbage was stale spilled locals.

What remains genuinely **unconfirmed** is only the *string-kernel theory* (`kFormat`/`kStrCat`/`kStrCpy`
in `kstring.c`, the prime-suspects bullet below). It was **never reproduced under ASan** and has **not
recurred since the view-RLE + decrypt1 fixes landed**. So either (a) it was a real independent overflow
not currently being triggered, or (b) the observed "aspb"-signature fault was always a *downstream symptom
of the view-RLE overrun* (a smashed pixmap propagating into the GC/free walk) and is already fixed. We
can't fully distinguish these from logs, but no recent clean- or diagnostic-build session shows the
corruption signature (HardFault with garbage/ASCII BFAR **and no `[OOM]` line**) — every recent fault is
either a clean `[OOM]` (fragmentation) or the now-fixed decrypt1 collision.

**Status:** treat as apparently resolved; keep `FSCI_PROBE_STR` **ON** as the canary. If a `[strprobe]`
line ever fires just before a `[FAULT]`, or a garbage/ASCII-BFAR + no-`[OOM]` HardFault recurs, re-open
this and run the desktop ASan hammer below. Until then it is a watch item, not active work.

---

*Historical detail (kept for the ASan repro recipe and the original three-fault analysis):*

Three HardFaults, all the same root cause — **heap allocator metadata smashed by an overflow** — caught
at different downstream sites:

- **Two land inside the GC `reg_t` hashmap walk** (`hashmap.c:135` `while (*node && COMP(value,
  (*node)->name)) node = &((*node)->next);`, comparator `compare_reg_t` in `reg_t_hashmap.c:33`). One had
  **BFAR `0x62707361`** = ASCII `61 73 70 62` = **"aspb"** (little-endian memory order) — a GC node's
  `next` pointer (a 12-byte `malloc`'d `{reg_t name; int value; node *next;}`) overwritten with **string
  bytes**. A pointer holding lowercase ASCII means a string-writing path overflowed into, or wrote
  through a freed-then-reused, GC node.
- **One lands inside newlib `free()` itself** (`_free_r`, `_mallocr.c:2675`, the chunk-unlink
  `FD=P->fd; BK=P->bk; …`), CFSR `0x8200` precise bus fault, **BFAR `0x2009f94e`** — ~121 KB *above* the
  RP2350 SRAM top (`0x20082000`), a wild chunk pointer. free() followed a smashed boundary tag into
  unmapped RAM. This is the **same corruption one layer deeper**: not a GC-specific bug — the allocator's
  own metadata is damaged, which is the signature of a heap buffer overflow into an adjacent chunk header.
  - **Context (pico.log up to this fault):** repeating `Activating port 1 after disposing window 4` +
    the SQ3 invalid-param spam = **message windows opening/closing** (player typing parser commands and
    reading responses). Last alloc before the fault `malloc 1282 2007FD10->20080212` **succeeded** (not
    OOM) and ended **7.6 KB under the SRAM ceiling** — i.e. heap nearly full, so the overflow lands on
    live metadata instead of slack. The faulting `free()` is almost certainly freeing a **window/pixmap
    backing buffer during window dispose**, whose neighbouring chunk was smashed by an earlier
    text-path write. Crash site = window teardown; overflow source = the message/text path.

GC and free() are only the **canaries** (densest small-block alloc + pointer-walk; GC runs every 2048
allocs on Pico, 16× more than desktop), so they trip on the damaged block first.

- **NOTE — distinguish the OOM variant from the corruption variant.** A *separate* GC HardFault class is
  a plain out-of-memory NULL-deref, not a metadata smash: on a full heap the GC node alloc at
  `hashmap.c` (`TYPE##_hash_map_check_value`) and the bucket alloc in `new_##TYPE##_hash_map` used **raw
  `malloc`/`calloc` with no NULL check**, then dereferenced immediately. The 4ded2752 session log shows
  exactly this — `calloc 2060` (bucket array) + a flood of ~270 `malloc 12` (the 12-byte nodes) then
  `[FAULT]` with **no `[OOM]` line** (raw malloc bypasses `pico_oom_report`). **Hardened:** both now use
  `sci_malloc`/`sci_calloc` (`hashmap.c` includes `sci_memory.h`), so a GC-time exhaustion self-reports
  via `pico_oom_report` (legible LCD dump + halt) instead of a blind NULL-deref. This does **not** address
  the "aspb" corruption variant above (BFAR holding ASCII = an overflow, not exhaustion) — that is still
  open and is the ASan target. Use the presence/absence of an `[OOM]` LCD line to tell the two apart on the
  next device run: `[OOM]` present = exhaustion (need more heap headroom); HardFault with no `[OOM]` and a
  garbage/ASCII BFAR = corruption.

- **Control map is NOT the corruptor (ruled out by static audit + log).** All Pico control writes go
  through `ctl_set`/`ctl_fill`/`ctl_draw_line`, which only ever write values 0–15 and are bounds-clipped;
  the map is read-only during gameplay. Enabling it (default ON) merely **fragments the heap** so a
  pre-existing string-overflow/UAF now lands on a live block instead of in slack. (The window-dispose log
  above shows **no** `control map: 32KB alloc failed` warning — the control map decoded fine that session;
  the fault is unrelated to control decode.) Desktop's roomy heap + 2× buffers absorb the same bad write
  silently — latent, not absent.
- **Prime suspects:** SCI string kernels (`kFormat`/`kStrcpy`/`kStrcat`/`kString` in `kstring.c`) writing
  past a `dynmem`/`sys_strings` buffer, or save/restore name handling — these build the **message-window
  strings** implicated by the dispose-time `free()` fault. These are the engine paths that write
  attacker-length ASCII into heap blocks.
- **Find it with desktop ASan, not the device.** `build-asan/src/freesci` is built with
  `-fsanitize=address -g -fno-omit-frame-pointer`. ASan traps the bad write at the instant it happens,
  independent of heap layout, and names the writing function:
  ```bash
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
    ./build-asan/src/freesci --gamedir ~/Downloads/sq3 --graphics sdl --disable-mouse
  ```
  **Hammer message windows** — that is the implicated churn, not just "trigger text": type parser
  commands, read the responses, open/close inventory and dialogs repeatedly, "look" at scenery, read
  signs. Each message window open/close runs the string-building kernels (the overflow source) and then
  frees the window buffer (the `free()` that trips). Capture the `==ERROR: AddressSanitizer:
  heap-buffer-overflow` / `heap-use-after-free` report. The 90s intro-only ASan run was clean → the bug
  is on the **text/message-window path**, not the decode path.
- **Why the Pico no-collision A/B is a weak test:** control-OFF changes heap layout (may move the
  overflow into slack → false "fixed") *and* disables the control triggers that run the trash-elevator
  script (→ may never execute the buggy path). Use it only to corroborate independence, never to refute.

Record the root cause here once ASan pinpoints it.

### RESOLVED — the ~35KB/revisit accumulation was TWO caller-side leaks (cwd + console scrollback)

The `--wrap` malloc census (`pico_mem_census.c`) plus the `SITES256:` call-site tag (tag every
sci_malloc whose block lands in the 256–511 B bucket — the size class the census showed climbing — with
its `__FILE__:__LINE__`, deregister by ptr in `__wrap_free`) named the two leaking sites outright. Both
are pre-existing FreeSCI bugs, latent on desktop (roomy heap) and only fatal under Pico's ~444 KB ceiling:

1. **`_scir_load_resource` leaked its saved cwd** (`resource.c`). It does `save_cwd = sci_getcwd()` (a
   `sci_malloc(256)`) and both error paths `chdir(save_cwd); free(save_cwd)` — but the **success path**
   (`close(fh)` → return) freed nothing. Every resource load (several per room) leaked one 256 B cwd
   buffer → census site `tools.c:720` (the `sci_malloc` inside `sci_getcwd`) climbed ~+9/room, never
   dropping. **Fix:** restore + free cwd before the success-path return too.
2. **The gfx console scrollback grew unbounded** (`console.c` `sciprintf` → `con_gfx_insert_string`).
   `WANT_CONSOLE` is defined unconditionally (`config.h.in`), so `con_gfx_init()` registered the string
   callback, and every `sciprintf` line was stored forever in `con_buffer` (an ever-growing,
   never-freed cluster list in `gfx_console.c`). With Pico's constant warning spam + message-window
   churn this ratcheted up → census site `console.c:52`. **Fix:** skip `con_gfx_init()` on Pico
   (`#if defined(WANT_CONSOLE) && !defined(HAVE_PICO)` in `main.c` `init_console`); `_con_string_callback`
   stays NULL so `sciprintf` frees its own buffer. No on-screen console exists on Pico anyway.

**Verified fixed (post-fix device session):** SQ3 walked + typed + bounced room 3↔4 six times, then a
clean quit — no crash, no OOM. `untracked` held flat at ~199.7–201 K across all six room-4 revisits (no
trend), `uord` pinned at ~266 K, arena stopped growing at 368 076, and BOTH former sites vanished from
`SITES256:` (remaining sites are flat 1–4 block working-set entries). The census + tag are diagnostic
build infrastructure — leave them in; `untracked` and `SITES256:` are now the regression watch for any
future per-revisit growth.

The history below is kept for context (how the leak was localized from "no leak" → untracked gfx region
→ the two call sites). The transient-peak / fragmentation OOM (item 1 of the original diagnosis) is a
*separate*, still-relevant pressure — a contiguous decode block can still be denied while KB remain free
— but the baseline that pushed the heap toward that edge on every revisit is now gone.

### DIAGNOSIS (HISTORICAL — leak now RESOLVED above) — transient peak + fragmentation, PLUS the (now-fixed) ~35KB/revisit accumulation

Two distinct pressures, established by the per-room breakdown probe:

1. **Transient peak + fragmentation** (the immediate OOM trigger). Contiguity — not total free bytes — is
   the limiting resource: a small `malloc` fails with several KB *total* free because that free space is
   shattered into chunks none of which is large enough.
2. **A genuine baseline accumulation** (CORRECTION to the earlier "no leak" claim). The 4e2df48b session
   re-entered the SAME room 3 twice and the resident `uord` rose **319 960 → 365 184 (+45 KB)** — for an
   identical room, with the arena flat at 411 360 (so not fragmentation slack, genuinely more live bytes),
   and *despite* the 2nd visit failing to allocate the 32 KB control map. Scripts were flat (~44 KB) and the
   seg tables only grew ~3 KB, so **~42 KB accumulated in allocations the probe did not yet count** (hunk /
   dynmem / sys_strings / non-seg gfx+widget). The probe was extended to itemize hunks (count+bytes), dynmem
   (count+bytes), locals, sys_strings to localize it; activity between the two visits was heavy
   message-window churn (`Activating port 1 after disposing window 4`) plus a save-game → suspects are
   graphics save-under hunks, a per-window-dispose leak, or save/restore buffers.

   **CONFIRMED non-seg-manager (97bdee70 session).** That run itemized hunks/dynmem/locals/sysstr and they
   are all **flat/zero** while the SAME room 3's `uord` rose 334 408 → 339 048 → **374 568** across heavy
   message-window churn (scripts went *down*, `hunks=0`, `dynmem=0`, tables flat). The ~35 KB jump lives in
   allocations the breakdown structurally cannot see (gfx pixmaps + widgets are **not** in
   `seg_manager.heap[]`). The session ended in a clean `[OOM]` (exhaustion, not corruption) at
   `decompress0.c:303` (the 12 664 B decompressed-resource buffer) with the heap **94 % full** (used
   400 656 / arena 427 744) and fragmented — the accumulation pushed the baseline up until a routine decode
   alloc could not find a contiguous block.

   **LOCALIZED to the untracked gfx region — NOT pixmaps, widgets, OR resources (cceae716 session).**
   The gfx-layer counters are now on the BREAKDOWN line (`gfxpxm=N (bytes…)`, `widgets=M`) and they decide
   it: across SQ3 room 3→4→3 round-trips, `uord` rose room-3 **356 208 → 390 744 (+34.5 KB)** and room-4
   **372 464 → 410 864 (+38.4 KB)**, while `gfxpxm` stayed **8**, `widgets` stayed **9**, scripts/objvar/
   clone-node-list tables/`hunks=0`/`dynmem=0` all flat, AND `reslru=0 reslock=0` every line. Subtracting
   every tracked category from `uord` leaves a **~287 KB untracked baseline that grows ~35 KB per revisit** —
   essentially 100 % of the growth. So the leak is in allocations the probe is structurally blind to and
   that are **not** the pixmap registry, the widget count, or the resource manager. Ruled out this session:
   - **Resources** — `reslru=0 reslock=0` proves the resmgr retains nothing across rooms (read → decode →
     free). Your original "resource-eviction" hunch is not where it hides.
   - **Window save-unders** — `gfxw_make_snapshot` (`widgets.c`) is a ~24-byte serial marker, not a pixel
     buffer, and its free path is balanced (`free(port->restore_snap)` on dispose).
   - **Pic/view containers** — `gfxr_free_all_pics` frees both pic AND view trees on room change, then
     `psram_reset()` (`resmgr.c`).

   Remaining untracked suspects (short list): **fonts** (`gfx_bitmap_font_t`, cached and NOT freed on room
   change), the gfx-resource **sbtree node** structures (kept alive across rooms), and driver-side state.

   **DESKTOP DOES NOT REPRODUCE — the leak is Pico-only code, NOT shared engine code (desktop probe
   session).** A desktop mallinfo probe was wired up (`desktop_mem_probe`, `kgraphics.c`, gated behind
   `FREESCI_MEMPROBE=1`, prints the same `uord/ford/arena/chunks` as the Pico BREAKDOWN line) and run with
   `--disable-mouse` while bouncing SQ3 room 3↔4 a dozen times with message-window churn. Result: **`uord`
   is flat.** Room 4 oscillated around 23.698 M with **no trend** (one inter-visit delta was *negative*);
   room 3 decelerated hard toward a plateau (deltas collapsed +26 KB → +5 KB → +2 KB and stopped climbing —
   the high-water-mark signature of clone/node/list tables + glibc free-list caching settling, not a
   per-revisit leak). `arena` was **pinned** at 26 079 232 from room 2 on — glibc never grew the heap across
   the whole bounce; `chunks` bounced 175–249 with no trend. Contrast Pico's sustained **+35 KB on every
   revisit**. This is **decisive**: any leak in *shared* code would surface as the same small-block growth in
   desktop `uord` (large SDL surfaces are mmap'd and escape mallinfo, but the Pico growth was measured in
   `uord` = small-block heap, so the shared small-block paths are the apples-to-apples comparison — and
   they're flat). So the ~35 KB/revisit lives in the **`HAVE_PICO`-only allocators**: the `pico_driver.c`
   save-under grab path or the Pico pixmap registry — exactly the suspects the desktop massif run alone
   could not rule out. This **retires** the `--wrap` malloc census as the next step (it would profile shared
   code, which is proven clean) and the shared-code suspects above (fonts/sbtree).

   **SAVE-UNDERS RULED OUT — grab/free balance counter (device session 2718a104).** `pico_driver.c`
   keeps `pico_grab_sram_live/total/bytes`, `pico_free_sram_total`, `pico_grab_psram_total` and the
   BREAKDOWN line prints them as `grab=<live>/<total> (<bytes> B live) gfree=<n> psgrab=<n>`. SRAM grabs are
   `sci_malloc`'d in `pico_grab_pixmap` (≤4096 B regions) and freed in `pico_unregister_pixmap`; PSRAM grabs
   (>4096 B) are bump-allocated and only reclaimed at `psram_reset()` on room change. **Result:**
   `grab=1/1 (256 B live) gfree=0` stayed **constant the entire session** (walk around → type parser
   commands → bounce room 3↔4 multiple times). SRAM save-unders do **not** leak — they are NOT the
   ~35KB/revisit growth. (`psgrab` climbed +2/room but those are PSRAM, reclaimed at every `psram_reset`.)
   The session ended in a **fragmentation OOM** (`malloc 12664 failed` at `decompress0.c:303`, free=20432
   but heap 94% full: used 416332 / arena 443484) — a contiguous decode block denied while KB remained
   free, the downstream consequence of the baseline climbing, NOT a corruption smash (clean `[OOM]` halt,
   no garbage BFAR). (`scilive`/`rawgap` on the BREAKDOWN line are the known-broken counters — sci_malloc'd
   memory freed via raw `free()` makes `scilive` ≫ `uord`; ignore them.)

   **TEXT-WIDGET DISPOSE PATH RULED OUT (static audit).** The message-window text free chain is balanced:
   `_gfxwop_text_free` (`widgets.c:1135` frees `text->text`) → `_gfxwop_basic_free` (`409`) →
   `_gfxw_unallocate_widget` (`221-228` frees `text_handle` via `gfxop_free_text`). Handle + its per-line
   pixmaps are released on dispose. The text widget itself is not the leak.

   **SBTREE + FONT-CACHE RULED OUT (static audit).** Neither grows per window-open:
   - **sbtree** (`sbtree.c`) is a **fixed pre-allocated cell table**. `sbtree_set` (`170-180`) writes into
     an existing cell located by `locate()` — there is **no per-insertion `malloc`**, and the table cannot
     grow at runtime. So the gfx-resource trees (PIC/VIEW/FONT/CURSOR, all sbtree-backed) add no SRAM as
     entries are inserted; their only heap cost is the cell payloads, which the room-change frees already
     account for (PIC/VIEW) or which cache once (FONT/CURSOR).
   - **font cache** (`gfxr_get_font`, `resmgr.c:646-696`) allocates each font **once per font nr**, caches
     it in the FONT sbtree, and returns the cached pointer on every subsequent call; built-in fonts (5x8,
     6x10) are returned with **no allocation at all**. SQ3 uses a tiny fixed set of fonts → bounded, one-time
     cost, **not** per-churn growth. Decisive corroboration: a one-time font alloc would also show up in
     desktop `uord`, which was **flat** across the same room bounce — so any per-revisit growth cannot be the
     (shared) font-cache code.

   **Census now in place to catch the actual allocator (awaiting one device flash).** With save-unders,
   text widgets, sbtree, and the font cache all ruled out by static audit, the next move is **runtime
   instrumentation**, not more guessing. Two new probes are wired:
   - **Untracked-gap line.** `pico_mem_breakdown` (`kgraphics.c`) now sums every category it *can* itemize
     (scripts + objvar + clonevar + locals + tables + hunks + dynmem + pxm + grabbed save-unders) into
     `tracked=`, and prints `untracked = uordblks − tracked` on the BREAKDOWN line. If `untracked` is what
     climbs ~35KB/revisit, the leak is provably in allocations no category counts — confirming the gap is
     real and sizing it precisely.
   - **Live-allocation size histogram** (`src/platform/pico/pico_mem_census.c`, NEW). It provides its own
     `__wrap_malloc/calloc/realloc/free`; the top-level `CMakeLists.txt` defines the `pico_malloc` target
     itself **before** `pico_sdk_init()`, so the SDK's `if(NOT TARGET pico_malloc)` guard skips compiling
     its own `malloc.c` (whose `WRAPPER_FUNC` would otherwise multiply-define ours, since the SDK adds
     `malloc.c` as an INTERFACE source straight into the executable, not an archive). We re-add the
     `-Wl,--wrap=*` flags so `__real_*` still resolve to picolibc's allocator. The census keys every
     alloc **and** free on the block's actual `malloc_usable_size` — so the histogram is self-consistent
     regardless of whether memory was taken via `sci_malloc` or raw `malloc` and freed via the other (the
     drift that makes `scilive` useless). `pico_mem_breakdown` prints a `[mem] CENSUS` line of non-empty
     buckets (`<lowerbound>:<count>/<bytes>`). **Diff a bucket across same-room revisits → the growing
     bucket's size range points straight at the leaking call site.** Caveat: diagnostic build only — it
     drops pico_malloc's malloc_mutex (safe here: FreeSCI allocates from core0 only) and uses a depth guard
     so calloc/realloc calling malloc/free internally aren't double-counted.

**Evidence (one flashed SQ3 session, intro → trash elevator → conveyor death):**
- `scripts`: 15 → 18, ~35KB → ~45KB, then **plateaus** (not unbounded).
- clone/list/node tables: tiny (~3KB total), ratchet up in small steps (grow-never-shrink, but capped).
- `chunks` (free-chunk count = fragmentation): 11 → 21 → 44 → 53 → 62 — *this* is what climbs.
- `uord` (live bytes): ~287KB during early play → +79KB death-animation spike to ~417KB (≈97% of the
  427744-byte arena ceiling).
- Crash: `malloc 2745 failed, free=10720` at `gfx_tools.c:256` `gfx_pixmap_alloc_index_data` — a **view-cel
  index buffer**. 10720 bytes free, but no 2745-byte contiguous run among 62 fragments. NB the view
  `index_data` → PSRAM offload is **already DONE** (`sci_resmgr.c:377-399`, all loops/cels), but it frees
  the cel buffer *after* decode — this alloc is the *transient decode-time* peak, which the offload does
  NOT shrink. So the remaining lever here is the transient per-cel decode peak (decode into a reused
  scratch) or the ~35KB/revisit gfx-region leak, **not** re-doing the (completed) view offload.

The earlier 961d1fc2 crash (`decompress0` needed 12664 with 19048 free) is the same class — a small
contiguous block denied while KB remain free. SCI0's working set already runs near the ceiling; normal play
fragments the heap (room changes re-decode pics = big transient allocs/frees; parser/dialog lines open and
close message windows = small alloc/free churn). No leak is required to OOM.

NB this is distinct from the "aspb" heap *corruption* above (a pointer overwritten with ASCII = an
overflow, not exhaustion). Fragmentation OOM ≠ metadata smash; both are open, tracked separately.

### WORKING-AS-DESIGNED — the `malloc 64000 failed` log lines are the deferred→pinned fallback, not a bug

A pico.log can show one or two `malloc 64000 failed to allocate memory` lines per room change (e.g.
55e45371 lines 54-55) and **still keep running** (no `[OOM]` halt, the next room draws). That is the
**designed deferred-visual fallback recovering**, not a failure to investigate. Decode tree:

- There are exactly **two raw `malloc(64000)` sites**, both for the *same* pic-decode visual buffer:
  the **deferred** alloc (`sci_resmgr.c:155`, the normal path) and the **early-pin fallback**
  (`operations.c:2281`, reached only if the deferred one failed). Raw `malloc` → the pico-sdk wrapper
  prints "malloc 64000 failed" *unconditionally* on NULL, but neither site halts — they recover.
- **Every OTHER 64KB consumer is fail-fast, not recoverable:** `visual[0]` (`pico_alloc_visual` →
  `sci_malloc`) and the priority map (`gfx_pixmap_alloc_index_data` → `sci_malloc`, `gfx_tools.c:307`)
  route through `sci_malloc`, which on Pico calls `pico_oom_report` → **LCD `[OOM]` + halt**. So if a
  log shows `malloc 64000 failed` *without* a following `[OOM]` halt, it is provably the deferred
  visual buffer (the only recoverable 64KB alloc), never visual[0] or priority.
- **Why deferred is the default (the tradeoff is real and asymmetric).** Deferred frees `visual[0]`
  *before* the pic-resource decompress (`operations.c:2254`) so `decompress0` gets its ~12-61KB; the
  cost is the late 64KB alloc can miss under post-decompress fragmentation → recover via early-pin
  (free the half-built pic to re-coalesce, retry once). The alternative — pinning 64KB *through* the
  decompress — would risk a `decompress0` OOM, and that path is `sci_malloc` = **fatal halt**, not
  recoverable. So deferred is chosen precisely because its failure mode (a double-decode) is survivable.
- **The only real cost of a deferred miss is a full double-decode** (`gfxr_get_pic` runs twice:
  decompress + draw discarded, then redone with the pin) — a CPU hitch, not a crash. The
  `GFXWARN("retrying with early pin")` at `operations.c:2283` is gated by gfx debug level, so the
  recovery line usually does NOT appear — only the wrapper's bare "failed" does. Absence of the GFXWARN
  is not evidence of non-recovery.
- **To make it stop *happening* (not just recover):** lower baseline fragmentation so the deferred
  alloc succeeds first-try. The view-cel offload lever is already pulled (DONE); the remaining levers
  are the transient per-cel decode peak and the ~35KB/revisit gfx leak (see DIAGNOSIS above). This is
  NOT a separate fix — it folds into the fragmentation/leak work.

### Input-scaled allocations audit (reviewed — only VIS_MATRIX is a real watch item)

A sweep for heap allocations with no fixed upper bound (they scale with game data, not a constant), and
their current status — keep this so they aren't re-investigated cold:

- **`vis_matrix` (`kpathing.c:1433`) — the one genuine input-scaled alloc, but small for SQ3.** It is
  `sci_calloc(vertices * VIS_MATRIX_ROW_SIZE(vertices), 1)` where `VIS_MATRIX_ROW_SIZE(N) = ceil(N/8)` —
  i.e. **bit-packed**, ~`N²/8` bytes, *not* `N²` (an earlier note claiming "~1MB for 1000 vertices,
  one char per cell" was 8× too high; it's ~125KB @ 1000). `vertices` = the count of pathfinding-polygon
  vertices in the current room (tens for SQ3, not thousands), so real risk on SQ3 is low. It remains the
  only `O(input²)` single allocation, so if a future room/game stalls or OOMs inside `kAvoidPath`, cap
  `vertices` here first. Freed at `kpathing.c:1294`.
- **SCI script heap (`heap.c:44`, `sci_calloc(SCI_HEAP_SIZE,1)`)** — normal VM working memory, already
  bounded by the resource LRU (`max_memory=1` → one script heap live at a time). Not a growth risk.
- **Text layout `fragments` (`font.c:177`)** and **drawn-pic cache (`s->pics`, `kgraphics.c:1377-1385`)**
  — both transient: `fragments` is a single upfront `sci_calloc` sized by `strlen(text)` and freed after
  render (no doubling loop, despite an old note); `s->pics` high-water-marks (+4 grow, `drawn_nr` resets
  per draw cycle) and is freed in `_free_graphics_input`. Neither accumulates across rooms. Dismissed.

**Elevator boarding is gated on the control map decoding, which is fragmentation-sensitive.** `rm004::doit`
requires `ego.onControl == 3`; `kOnControl` reads `pic->control_map`, which only exists if room 4's control
pic (2052) decoded — needing a contiguous `malloc(32000)`. When room 4 is entered with a fragmented heap the
decode fails silently (`psram_valid=0` → scan returns 0 → `onControl` always 0 → boarding impossible). When
entered with ~82KB contiguous free it decodes and boarding works. So "the elevator worked this time" was a
heap-state effect, not a logic change.

**PARKED — one-time fresh-boot elevator no-pickup, NOT pursuing.** A single device session that went
*straight* to room 4 from a cold boot once did not pick the player up. It has **not** reproduced —
the elevator works on current code (rideable repeatedly, incl. via the multi-room path below), so this
is treated as a non-reproducing one-off and is **not** an active investigation. If it ever recurs,
the decider is cheap: grep that boot's pico.log for `malloc 32000 failed … decoding without collision`
(`sci_resmgr.c:185`). If **present**, room 4's control pic (2052) didn't decode → it's the known 32 KB
control-alloc miss (a heap/fragmentation symptom, see the WORKING-AS-DESIGNED + DIAGNOSIS notes), not a
control-path bug. If **absent**, the map decoded and any failure is downstream (`_gfxop_scan_one_bitmask`
PSRAM round-trip / `onControl`) — but the control path is already statically verified correct (next note),
so absent + failure would be the only thing warranting a fresh look.

**DATA POINT (55e45371 session) — elevator WORKED when room 4 was reached via a 7-room path.** A flashed
session that walked intro → ... → trash elevator (room 4) the *long* way picked the player up, and the
following death scene did not crash. Room 4's control pic (2052) decoded fine — the log has **no**
`malloc 32000 failed … decoding without collision` line — reached with ~79 KB free. Combined with the
elevator being rideable on current code, this is why the one-time fresh-boot no-pickup above is parked,
not chased.

**The Pico control path itself is CORRECT — the "elevator regression" is this same alloc failure, not a
control bug (verified by static audit, cceae716 session).** The nibble-packed PSRAM round-trip was suspected
but is consistent: `ctl_set` (`sci_pic_0.c`) packs odd pixels into the high nibble / even into the low
(`b[j] = (i&1) ? (…|(v<<4)) : (…|v)`), and the scan reads them back identically
(`v = (p&1) ? (b>>4) : (b&0x0f)`, `operations.c` `_gfxop_scan_one_bitmask` Pico branch). Geometry (the
`ystart+10` titlebar offset, clip rects, row stride) also matches the working desktop path. So a
*correctly-decoded* control map yields the same `onControl` result as desktop. The cceae716 log shows the
2nd room-4 entry hit `malloc 32000 failed … decoding without collision (low heap)` (`sci_resmgr.c:185`) →
collision off that visit → elevator can't fire. The 1st entry decoded fine. **To rule out any *separate*
Pico-only control bug behind the alloc failure, do one desktop repro:**
`./build/src/freesci --gamedir ~/Downloads/sq3 --graphics sdl --disable-mouse --run` — if the elevator works
there, the regression is purely the 32 KB alloc failure (fix the leak → fixed); if it fails there too, it's
an engine/SQ3-version issue independent of Pico.

### Per-room SRAM breakdown probe (diagnostic, keep until OOM headroom is comfortable)

`pico_mem_breakdown` (`kgraphics.c`, called at the end of `kDrawPic` under `HAVE_PICO`) walks
`s->seg_manager.heap[]` and prints one `[mem] BREAKDOWN` line per room: loaded script count + hot `buf`
bytes + unlocked count, the clone/list/node tables (used/cap), seg-manager `mem_allocated`, and `mallinfo`
(`uord`=live, `ford`=free, `arena`, `chunks`=free-chunk count = fragmentation). Paired with the
`[mem] room enter/ready` lines in `gfxop_new_pic` (`operations.c`), a single flashed session shows which of
{scripts pile up, tables high-water, fragmentation climbs} is actually growing. This is what proved the
no-leak diagnosis above — leave it in to measure the ~35KB/revisit gfx-region growth. (The view-cel
PSRAM offload is already DONE, `sci_resmgr.c:377-399` — not a pending before/after to measure.)

**`bad=N/M` on the `gfxpxm` field is a probe FALSE-POSITIVE, not a leak (55e45371 session).** The
breakdown's per-pixmap sanity check counts a node "bad" when it can't reconcile `data_size` against
`xl*yl*bytespp`. Across the whole 55e45371 session `bad` sat at a stable **2–3 of 7–8** every room — it
does not grow. The flagged nodes are the **cursor pixmaps** (seen in the `[mem] PXM` dump: id=`0303e5`
16×16 and id=`ffffffff` 17×17), which are built with an **uninitialized/garbage `data_size`** (the dump
shows wild `dsz=` values like `1457830437`, `-554206328`). They are live, correctly registered, and freed
on teardown — the probe just can't validate their size. **Do not chase `bad=2-3` as corruption or a leak;**
it is constant and benign. (If `bad` ever *climbs* across same-room revisits, that's different — then it
would indicate registry nodes accumulating.)

**Clean-teardown evidence (55e45371): no working-set survives exit.** On quitting back to the SD chooser,
`used` dropped **341,992 → 85,256** — essentially the entire game working set was reclaimed, with no
orphaned allocation surviving the return to `freesci_main`'s caller. Combined with the gfxpxm/widgets/table
counters staying flat, this session showed **no leak across 10 distinct rooms** (uord 288K→342K is the
legitimate growing working set as scripts load and plateau at 17, not accumulation). Caveat: this session
did **not** revisit any single room, so the separately-tracked ~35 KB/revisit gfx-region growth (see
DIAGNOSIS above) was not exercised here and remains open.

### Static-buffer SRAM recovery (DONE — `.bss` → lazy malloc / link-discard / pool-shrink) — ~41 KB, now tapped out

A link-time static array reserves `.bss` permanently — it lowers the `mallinfo` arena ceiling
whether or not the feature ever runs. Converting the array to a `static T *p = NULL` pointer that is
`sci_malloc`'d on first use costs **zero** SRAM until the code path actually fires, and on Pico that
path never fires for the buffers below — so the recovery is pure. After the first wave the arena ceiling
rose **427,744 → 444,136** (+16.4 KB observed; ~25 KB nominal across the first three), plus a later
~2.8 KB from the bottom two rows (verified against the ELF `.bss`). A second wave (2026-06-21) added a
further **+12.6 KB** of heap ceiling (clean-build `__end__` 0x2000f150 → 0x2000c020, heap span
**462,512 → 475,104 B**) via the FatFS handle-pool shrink + the opl2/adlib link-discard (last two rows).

| Buffer | File | Size | Why free on Pico |
|--------|------|------|------------------|
| `tokens[0x1004]` + `stak[0x1014]` | `decompress01.c` (alloc in `decryptinit3`) | ~20.5 KB | SCI0/SQ3 routes through `decompress0` (own decrypt1/decrypt2); the shared decrypt3 LZW scratch is never touched |
| `said_tree[500]` + `said_tokens[128]` | `said.c` / `said.y` (alloc in `said()` under `if (s->parser_valid)`) | ~4.5 KB | `said()` only builds its tree when `parser_valid`, which needs a loaded vocab; Pico disables vocab → always 0 → dead |
| `bank` + `channels` (in `amiga.c`) | `softseq/amiga.c`, ref in `softsequencers.c` | ~1.5 KB | PicoCalc has no Amiga audio. `&sfx_softseq_amiga` is `#ifndef HAVE_PICO`-guarded; `scisoftseq` is a STATIC lib so the linker discards `amiga.o` entirely (`.bss` **and** flash) once unreferenced — no CMake change needed |
| `input[1024]` + `inputbuf[256]` | `main.c` `get_gets_input` / `scriptdebug.c` `_debug_get_input_default` | ~1.3 KB | Interactive debug console reads `stdin` via `fgets`; Pico has no stdin so neither runs. Lazy `sci_malloc` on first call → 0 `.bss`, 0 heap on Pico |
| `fat_files[MAX_FDS]` FatFS handle pool | `pico_io.c` (`MAX_FDS 16→8`) + `ffconf.h` (`FF_FS_TINY 0→1`) | ~9 KB | FreeSCI rarely opens >2 files at once; 8 handles is plenty. `FF_FS_TINY=1` collapses each `FIL`'s own 512 B sector buffer into the shared `FATFS` window → each handle drops ~608 B → ~96 B. Pool went 9728 B → 768 B (**device-confirmed working**) |
| `adlib_sbi` 1152 + `sci_adlib_vol_tables` 1024 + `adlib_reg_L/R` 512 + `KSL_TABLE/SL_TABLE/RATE_0` ~576 | `opl2.c` / `adlib.c` / `fmopl.c`, ref in `softsequencers.c` | ~3.3 KB | Sound is off on Pico (`-q` → NOSOUND, `sfx_init` early-returns before any softseq runs). Gating `&sfx_softseq_opl2` behind `#if !defined(HAVE_PICO) || defined(PICO_PWM_AUDIO)` link-discards opl2.o → fmopl.o → adlib.o together. Returns automatically with `-DPICO_PWM_AUDIO=ON` |

- **The two synth `.bss` claims are now BOTH stale — superseded by the opl2/adlib link-discard above.**
  The big `fmopl.c` synth tables are *already* lazy `static int *` (NULL under NOSOUND); the remaining
  ~3.3 KB of small Adlib/OPL `.bss` lookup tables (`adlib_sbi`/`sci_adlib_vol_tables`/`adlib_reg_L/R`/
  `KSL_TABLE`/`SL_TABLE`/`RATE_0`) used to be "**deliberately kept** for the PWM-Adlib path (roadmap #3)" —
  but they cost ceiling **every** sound-off build, so they are now link-discarded by default and re-enter
  with `-DPICO_PWM_AUDIO=ON`. No SRAM is "saved for later" while sound is off; the tables come back the
  moment the PWM-Adlib work is built.
- **Both conversions preserve the capability** — lazy malloc ≠ deletion. If vocab/parser is
  re-enabled (roadmap #1) or SCI01/SCI1 games are run, the buffers allocate on demand exactly as before.
- **`said.y` was edited in lockstep with the generated `said.c`** so a future bison regen won't clobber
  the change.
- **The amiga / opl2 link-discard is the cleanest pattern** for dead synths: guard the
  registration-array reference (`sw_sequencers[]` in `softsequencers.c`) so the static lib drops the
  whole object graph. amiga.o is `#ifndef HAVE_PICO`; opl2.o (which transitively pulls fmopl.o + adlib.o)
  is `#if !defined(HAVE_PICO) || defined(PICO_PWM_AUDIO)`. `sfx_find_softseq` is never called under
  NOSOUND, so the array's default-`[0]` shifting from opl2 to SN76496 is inert.
- **Not a reclaim, but related (CPU only): `gfx_sci0_pic_colors` init-once on Pico** (`sci_pic_0.c`
  `gfxr_init_static_palette`). The 256-entry blend table stays resident (2 KB — it's a writable
  per-pic `colors` slot, can't be const flash), but its 256× `sqrt` INTERCOL recompute is now done
  **once** instead of on every pic decode (`_gfxr_pic0_colors_initialized = 1` under `#ifdef HAVE_PICO`).
  Desktop keeps recompute-every-time because `sci0_palette` is runtime-mutable via the debug console
  (`con_hook_int`, `main.c`); Pico has no console so the palette never changes → safe to cache.
- **Static mining is now tapped out** (~41 KB total). The ~35 KB/revisit accumulation that was the
  remaining wall is now **RESOLVED** (two caller-side leaks: cwd + console scrollback — see the RESOLVED
  note above). What's left is the *transient* decode peak + fragmentation OOM (a contiguous block denied
  while KB remain free), not a steady-state baseline climb — attack it via the per-cel decode scratch or
  control-map peak-shrink levers, not more `.bss` conversion.

### Pico roadmap (remaining work, prioritized)

The unifying constraint is the **~388KB SRAM heap**. The remaining items are largely independent —
the "shared PSRAM read-cache keystone" idea below was investigated and ruled out (see ✗).

✗ **Resource/script data → PSRAM read cache — RULED OUT for SCI0.** This was the planned keystone,
   but it does not fit SCI0's execution model. `script_t.buf` (`vm.h:199`) is not read-only resource
   data — it is the VM's hot read-write working memory: bytecode is fetched per-instruction from
   `code_buf = scr->buf` (`vm.c:776`), and the *same* buffer holds object property vars + locals that
   the VM mutates in place via `reg_t` offsets during execution. The source resource bytes are
   *already* freed on Pico (`sm_mcpy_in_out` then `scir_evict_resource_data`, `vm.c:1957/1968`), so
   the steady-state cost is the live `buf`s themselves — which can't be offloaded to a read-only,
   bursty-access PSRAM cache. Consequence: the items below are independent; there is no shared
   read-cache infrastructure and no ordering dependency on this item.

1. **Vocab loading → re-enable the text parser. (DONE — device-validated on SQ3.)**

   **The text parser works on Pico.** Walk around, type "Look"/"stand up"/etc. in multiple rooms,
   ride the trash elevator + conveyor — eight+ parses in one session, no OOM, no fault, clean teardown
   (`free=399936 used=15748` back at the chooser → no leak). What it took:

   - **Re-enable the load** (`game.c` `_init_vocabulary`, `HAVE_PICO` branch): load
     words/suffices/branches resident; `parser_rules = NULL` (GNF rebuilt per command, NOT resident).
   - **Rebuild GNF per command** (`kstring.c` `kParse`, `HAVE_PICO`): `vocab_build_gnf` from the
     resident branches at the top of each parse, `vocab_free_rule_list` after — so the ~50KB rule list
     is a transient, not a permanent resident charge. (`s->parser_rules` stays NULL → no aliasing.)
   - **THE FIX that actually mattered — borrow the visual buffer to PSRAM during the parse**
     (`pico_driver.c` `pico_borrow_visual`/`pico_return_visual`, called from `kParse`). The GNF *build*
     (~50KB) plus the per-word candidate expansion in `vocab_gnf_parse` (`grammar.c:646-710`, the
     `_vinsert` double-loop at :690→:205) is a stacked transient that hit **~97KB for ambiguous commands
     like "stand up"** and OOM-halted at `grammar.c:205` (`_vinsert`, clean `[OOM]`, not corruption).
     The game is paused with the input window up and nothing draws during a parse, so `kParse` saves the
     64KB visual back-buffer to a fixed PSRAM scratch (`0x700000`, clear of the room bump arena), frees
     the SRAM, parses with **+64KB headroom**, then restores the frame byte-for-byte before returning to
     the VM. Device: room-2 `[gnf]` free-after-build went **23912 → 87264 B**; "stand up" (room 10) went
     **47800 B → OOM** → **117992 B, parses fine**. Cost: ~30ms PSRAM round-trip per parsed command (a
     paused moment, imperceptible). Residual risk (accepted): if a parse fragments the heap so the 64KB
     can't be reclaimed after, `pico_return_visual`'s `sci_malloc` halts with a legible `[OOM]` naming
     `pico_driver.c` — degrades loudly, never corrupts.
   - **Stage 2 word-packing** (`vocab_pack_words`, `vocab.c`; CMake `PICO_PACK_VOCAB`, default OFF):
     collapses the ~1489 per-word `sci_malloc`s into one allocation. **On device it bought only ~3KB**
     (vs the ~26KB the probe predicted — `vocab_get_words`' per-record malloc overhead was far smaller
     than estimated), so packing is NOT what made parsing fit — the PSRAM visual-borrow is. Kept as a
     cheap, harmless baseline trim behind its flag; not load-bearing.
   - **Stage 3 (PSRAM-resident words behind a `psram_set_floor()`) is NOT needed and was abandoned** —
     it would have saved roughly what packing did (~little), and the real lever was the transient parse
     peak, not the resident word baseline.

   The diagnostic `[gnf]` line (`kstring.c`, `HAVE_PICO`) prints the per-command rebuild's transient
   bytes + free heap. Leave it until the parser has more device mileage, then strip with the other probes.

   **FIXED (device-confirmed) — heavy-grammar GNF candidate-expansion OOM, fixed by a free-heap floor in
   `vocab_gnf_parse` (`grammar.c`, `HAVE_PICO`).** PQ2 (1843 words vs SQ3's ~1489) OOM-halted at
   `grammar.c:205` `_vinsert` on a multi-word ambiguous command ("lock car doors"). Root cause: the
   per-command candidate expansion in `vocab_gnf_parse` multiplies candidates **word-by-word** — for each
   non-final word, every surviving candidate with a remaining nonterminal is matched against the *entire*
   GNF rule list and `_vinsert`ed, so against a large grammar an ambiguous 3-word command fans out
   multiplicatively until the heap is exhausted (each `_vinsert` ≈ 248–268 B). This is a *transient
   parse-time* blowup, NOT a headroom problem — SQ3's small grammar parses fine with *less* free heap
   (~59–70 KB) than PQ2's failure point (~74 KB), so raising the baseline doesn't help; only the heavy
   grammar explodes.
   - **Fix:** inside the `subseeker`/`seeker` loops, every **64** subseeker iterations
     (`++pico_vinsert_ctr & 0x3f`) check `mallinfo().fordblks`; if free heap is below
     `PICO_GNF_HEAP_FLOOR` (**24 KB**), set `pico_floor_hit`, break both loops, free `reduced_rules`, and
     continue the parse with the candidates gathered so far. Prints a permanent telltale
     `[gnf] candidate expansion hit heap floor at word N/M, truncating (free=…B)`.
   - **Why truncation doesn't break parsing:** the explosion is mostly *junk* candidates — grammar
     ambiguity spawning thousands of alternative GNF paths, but the real sentence needs only **one** valid
     path, gathered early in the list. Lopping off the combinatorial tail keeps the matching candidate, so
     the command still resolves. Device-confirmed: "lock car doors" truncated at word 0 (free fell to a few
     KB) yet parsed correctly, three times; the 2-word case never hit the floor at all.
   - **Self-gating** (satisfies the "only large grammar" intent with no per-game threshold): small grammars
     never approach the 24 KB floor, so SQ3 is untouched in practice. Fully `HAVE_PICO`-gated (incl. the
     `<malloc.h>` include) → desktop is byte-for-byte unchanged. The interval was tightened 256→64 after the
     first device run showed a single 256-window could plunge free heap ~69 KB → ~3.4 KB (caught, but thin
     margin); at 64 it aborts nearer the floor. **`grammar.c` is hand-written, not generated** — no
     `grammar.y` to regenerate (contrast `said.c`←`said.y`).

   ---
   *Original design notes (kept for context / re-measuring other games):*

   `_init_vocabulary` (`game.c:63-85`, under `HAVE_PICO`) NULLs `parser_words`/`parser_rules`/
   `parser_suffices`/`parser_branches` to save ~80KB, so `kParse` matches an empty vocab and "look
   around" etc. do nothing. Re-enabling just means running the existing `#else` branch
   (`game.c:87-96`) on Pico too. This is **additive** SRAM spend, not a saving (vocab is NULL today),
   so it does **not** unblock the control-map item — keep them decoupled.

   **Code audit (verified, corrects the earlier lifetime note):** the four structures and their real
   access patterns —
   | Structure | Type | Loaded by | Read by | Lifetime |
   |---|---|---|---|---|
   | `parser_words` | `word_t**`, ~900 entries each its own `sci_malloc` | `vocab_get_words` (`vocab.c:72`) | `vocab_tokenize_string` in **kParse** (bsearch+strcmp) | per-kParse |
   | `parser_suffices` | `suffix_t**`, tens | `vocab_get_suffices` | `vocab_tokenize_string` in **kParse** | per-kParse |
   | `parser_branches` | `parse_tree_branch_t*` flat array, 44 B each | `vocab_get_branches` | `vocab_build_gnf` (init) + `vocab_gnf_parse` in **kParse** | per-kParse |
   | `parser_rules` | GNF linked list (`parse_rule_list_t`, pointer-chasing) | `vocab_build_gnf` (`grammar.c:518`) from branches | `vocab_gnf_parse` in **kParse** | per-kParse |

   **DECISIVE: all four are consumed entirely *within* `kParse`.** The only artifact crossing the
   `kParse → kSaid` boundary is `parser_nodes[500]` (`engine.h:230`), which is **already a resident
   fixed array in `state_t`** — `kSaid` (`said.c:2528`) reads `parser_nodes` only, never the rules.
   So the old "rules must survive until Said runs" worry is **WRONG**: rules can be freed at the end
   of `kParse`. (`vocab_gnf_parse`, the only rule consumer, is called from `kstring.c:331` in kParse,
   never from said.c.) Also: `parser_rules` is **input-independent** — a pure function of
   `parser_branches`, identical all game; the only question is resident-once vs rebuilt-per-command.

   **PSRAM lifetime gotcha (the real constraint):** `psram_alloc` (`psram_alloc.c:12`) is a single-
   offset bump allocator; `psram_reset()` rewinds to **0** on **every room change**
   (`gfxr_free_all_pics`). Vocab must survive room changes → it cannot sit in the resettable region.
   **Required:** add a **floor** — allocate vocab at boot, then make `psram_reset()` rewind to the
   floor (above vocab) not 0. PSRAM is 8MB vs ~80KB vocab, so space is a non-issue.

   **Recommended design (option B, refined):**
   - *Init:* load words/suffices/branches; pack `parser_words` into **one contiguous PSRAM blob**
     (offset table + packed records), replacing the ~900 small `sci_malloc`s (themselves a
     fragmentation source); keep branches+suffices small-SRAM-resident (~3-4 KB, cheap) or in the
     blob; `psram_set_floor()`; free the SRAM originals.
   - *Per kParse:* page the words blob into **one** ~23 KB SRAM scratch (not 900 allocs), build GNF
     rules in a transient SRAM arena, parse into `parser_nodes`, free scratch + rules.
   - *kSaid:* unchanged. Net steady-state SRAM ≈ 0; per-command cost is a few large allocs.

   **Measure first (step 1) — DONE. The `PICO_VOCAB_PROBE` build captured the numbers (SQ3):**
   ```
   [vocab] words=1489 cur=53080B packed=21419B | suffices=48 2400B | branches=60 3912B
   [vocab] GNF rules=395 nodes=395 resident=41616B (uord+50272B) build_peak=44664B
   ```
   - **Words:** 1489 entries cost **53 080 B** as ~1489 separate `sci_malloc`s (a fragmentation
     source); they **pack to 21 419 B** in one blob (offset table + `2B class + 2B group + str\0`
     records) → ~31.7 KB saved by packing alone.
   - **Suffices:** 48 / 2 400 B. **Branches:** 60 / 3 912 B. Both tiny → keep SRAM-resident.
   - **GNF rules: 395 rules, 41 616 B resident, 44 664 B transient build peak.** (`uord+50272B`
     includes the rules' own slack/alloc overhead; the byte-accurate figure is 41 616 B.)
   - **DECISION — rebuild-per-command.** 41.6 KB resident is firmly in the "large (40 KB+)" bucket of
     the tradeoff, so the GNF rules are **rebuilt inside each `kParse`** in a transient SRAM arena and
     freed at the end of the call, NOT kept resident. Steady-state vocab SRAM then ≈ branches+suffices
     (~6.3 KB) + the per-command scratch; the ~44.7 KB build peak is paid only while parsing a typed
     command. Total all-resident would have been ~69 KB (matches the old "~80 KB" estimate).
   - **Peak caveat to validate in code:** the ~44.7 KB GNF build peak coincides with paging the words
     blob to an SRAM bsearch scratch (~21 KB) — those two transients must not stack badly mid-room
     (SQ3 already runs near the ceiling). Measure the combined per-command peak before locking the
     scratch sizes.
   - Probe is THROWAWAY (`PICO_VOCAB_PROBE`, OFF by default): `grammar.c` `_gnf_rule_bytes[_peak]` +
     `GNF_ACCOUNT/UNACCOUNT` macros (compiled out when off → non-probe builds byte-identical), and the
     `_init_vocabulary` measurement block in `game.c`. Keep it for re-measuring other games; leave OFF.

   **File-change checklist:** `psram_alloc.{h,c}` (add `psram_set_floor()`); `game.c`
   `_init_vocabulary`/`_free_vocabulary` (Pico load→pack→floor path); `vocab.c` (pack + packed-blob
   read/bsearch helper); `kstring.c` `kParse` (page scratch + transient GNF + free after parse);
   optional `vocab_psram.c` for the helpers. **Risks:** per-command GNF rebuild CPU (≤30 fixed-point
   passes, once per typed command — measure); PSRAM read latency in bsearch (paging whole blob to
   SRAM scratch likely beats per-compare PSRAM reads); `synonyms` are loaded separately by scripts
   (`kSetSynonyms`), already work on Pico — out of scope.

2. **Control-map collision → DONE FOR NOW (flag ON, works in visited rooms).** Code is written and
   gated behind `PICO_DECODE_CONTROL_MAP` (CMake option `PICO_CONTROL_MAP`, **now default ON**).
   Rebuild with `-DPICO_CONTROL_MAP=OFF` as the escape hatch if an unvisited room OOMs (you keep
   playing, minus collision). Every room visited so far decodes without OOM. Without the control map, `kCanBeHere` → `gfxop_scan_bitmask(pic->control_map)`
   always returns 0: ego walks through blocking polygons and control triggers (SQ3's trash elevator)
   never fire. The decode adds a ~128KB transient peak (a temporary `aux_map` for `AUXBUF_FILL`'s flood
   fill + the control `index_data`, both freed before the priority pass) — it OOMs only on tight rooms,
   and none visited so far have hit it. Treated as done unless it resurfaces in an unvisited room.
   - **Remaining peak-shrink lever (if a new room OOMs):** bit-pack `aux_map`, or offload the priority
     map to PSRAM during decode (the other 64KB live buffer) — **not** a steady-state-SRAM problem, so
     the vocab item does not help here.
   - **OOM self-report — DONE (supersedes the old TODO).** The 32KB control buffer
     (`sci_resmgr.c:176`, raw `malloc` deliberately, since the caller has a real recovery path) already
     degrades gracefully on NULL: it skips Pass 2, leaves `control_map->index_data` NULL /
     `psram_valid` 0 (so `gfxop_scan_bitmask` returns 0 = no-collision, per-pic and recoverable on the
     next decode), and emits `GFXWARN("control map: 32KB alloc failed ... decoding without collision")`
     → `sciprintf` → pico.log over serial. That is the "legible AND recoverable" outcome the old TODO
     wanted — no further work. (It is **not** routed to the LCD on purpose: `pico_oom_report`/LCD is for
     *fatal halts*; splatting the LCD for a recoverable mid-game degrade would be wrong.) Nothing left
     to implement for this item; it is fully parked unless an unvisited room hits the peak-shrink lever
     above.
   - **Scope caveat (now a known limitation, not roadmap work):** restores only *static* pic control;
     runtime actor-to-actor blocking writes to `state->control_map`, which is NULL on Pico — see "Known
     graphics limitations on Pico". Lower priority.

3. **Sound *(independent track — gated on heap headroom, not CPU)*.** The whole sound stack
   (`scisound`/`scisoftseq`/`scipcm`/`scimixer`) already links into the firmware but is dormant:
   `pico_main.c` passes `-q` → `SFX_STATE_FLAG_NOSOUND`. The PicoCalc PWM synth is **now gated behind
   `-DPICO_PWM_AUDIO` (default OFF)** — when OFF `audio/pwm_synth.c` is unlinked and `pwm_synth_init(26)`
   is skipped, recovering ~6.8KB SRAM (see "Sound on Pico (TODO)"). To work on this item, build with
   `-DPICO_PWM_AUDIO=ON` to get the 8-bit mono 22 kHz PWM on GPIO 26/27 back; what's missing is then
   feeding *PCM* into it instead of the tiny_agi sine channels.
   - **CPU is fine:** OPL2 inner loop ≈ 9 voices × ~30 cyc × 22050 ≈ 4% at 150 MHz; PWM IRQ <1%.
     `OPLOpenTable` uses `pow/log10/sin` once at init (fast with the RP2350 FPU; slow on RP2040).
   - **RAM is the gate.** Static `.bss` is **already negligible** — the big synth tables
     (`TL_TABLE`, `SIN_TABLE`, `AMS_TABLE`, `VIB_TABLE`, `ENV_CURVE`) are `static int *` pointers
     **NULL until `OPLBuildTables()` malloc's them** (`fmopl.c:610-627`), so under NOSOUND they cost
     **zero** SRAM; only ~650 B of tiny lookup tables (`KSL_TABLE` 512 B, `SL_TABLE` 64 B, `RATE_0`
     64 B, `outd` 4 B) sit in `.bss`. (The old "~34 KB `ENV_CURVE` in `.bss`" claim was WRONG —
     `ENV_CURVE` is `static int *ENV_CURVE = NULL`, `fmopl.c:191`.) The cost is therefore **entirely
     dynamic, paid only when sound is enabled**: HQ stereo ~165 KB (**won't fit**), LQ mono default
     tables ~70 KB, LQ mono + shrunk tables (`EG_ENT=128`, `SIN_ENT=512`, drop the stereo OPL) ~40 KB.
     Need ≥~80 KB free before enabling — gated purely on measured steady-state headroom.
   - **CHEAPEST-SRAM DESIGN — flash-resident `const` tables drop the ask to ~10–13 KB, NOT ~40 KB
     (byte-split sanity-check, 2026-06-23).** The decisive point: the five big tables are pure functions
     of `EG_ENT`/`SIN_ENT` (`pow/log10/sin`, **sample-rate-independent**), so they can be **precomputed
     `const` and stored in flash** (RP2350 has MBs free). Once they're flash, **table resolution stops
     mattering for SRAM** — so pick HQ for quality and let flash eat it:

     | Table | Formula | HQ (EG_ENT=4096, SIN_ENT=2048) | LQ (EG_ENT=128, SIN_ENT=512) |
     |---|---|---|---|
     | `TL_TABLE` | `EG_ENT*16` | 65,536 | 2,048 |
     | `SIN_TABLE`* | `SIN_ENT*16` | 32,768 | 8,192 |
     | `AMS_TABLE` | `AMS_ENT*8` (512) | 4,096 | 4,096 |
     | `VIB_TABLE` | `VIB_ENT*8` (512) | 4,096 | 4,096 |
     | `ENV_CURVE` | `(2*EG_ENT+1)*4` | 32,772 | 1,028 |
     | **flash total** | | **~136 KB** | **~19 KB** |

     *`SIN_TABLE` is currently `int**` (pointers INTO `TL_TABLE`, `fmopl.c:181/612/647`). To be
     flash-storable it must become `int` **offsets** into `TL_TABLE` (same byte size); `OP_OUT`
     (`fmopl.c:445`) then indexes `TL_TABLE[base + sin_offset]` instead of dereferencing. **This is the
     only non-mechanical representation change.**

     **What genuinely stays in SRAM (mutated every sample, can NOT go to flash):**
     | SRAM cost | Size | Note |
     |---|---|---|
     | `FM_OPL` per-chip state | **~7 KB/chip** | `FN_TABLE[1024]`=4096 + `AR/DR_TABLE[75]`×2=600 + 9× `OPL_CH` (~190 B ea ≈ 1.7 KB) + scalars. **Mono=1 chip ≈7 KB; stereo=2 ≈14 KB.** |
     | mixer block buffer | ~1–2 KB | scimixer working block |
     | PCM ring → PWM IRQ | ~2–4 KB | 8-bit downconverted samples |
     | **SRAM total (mono)** | **~10–13 KB** | |

     **Bottom line:** cheapest sound ≈ **~10–13 KB resident, dominated by the OPL *chip state*, not the
     tables** (the prior ~40 KB plan assumed tables live in SRAM). Go **HQ + flash tables + mono** (PWM is
     mono anyway; mono halves the chip-state floor). Changes vs the current build: (1) `SIN_TABLE` →
     offsets; (2) precompute the five tables `const`; (3) `opl2.c:544` keep HQ but select the const tables
     instead of `malloc`; (4) `opl2.c:546-547` drop `ym3812_R` (stereo→mono); plus the five deliverables
     below. **NB the current build is HQ *stereo* (`opl2.c:544` + dual `ym3812_L/R`)** — the ~165 KB
     "won't fit" config; the cheap path is HQ mono with flash tables.
   - **Five deliverables:** (A) `src/sfx/pcm_device/pico_pwm.c` implementing `sfx_pcm_device_t` +
     ring buffer, downconverting the mixer's 16-bit samples to 8-bit; (B) rewrite `pwm_synth.c`'s
     IRQ to pop the PCM ring (also frees ~44 KB flash by dropping `pwm_strings.h`); (C)
     `src/sfx/timer/pico.c` using `add_repeating_timer_us(-16667,…)` (60 Hz) to replace POSIX
     `sigalrm.c`; (D) CMake wiring + register `pcm_driver_pico_pwm` behind `HAVE_PICO_PWM`, exclude
     `fluidsynth.c`; (E) drop `-q` from `pico_main.c` argv, pass `-m pico_pwm -p polled`. SQ3 ships
     `ADL.DRV` so its sound resources carry Adlib tracks — no extra resource handling needed.

Suggested order: 1 ∥ 2 ∥ 3 (all independent; pick by user-visible value vs. measured headroom).

#### The 16 KB XIP-RAM lever for PicoCalc sound — TRIED + ABANDONED (device-measured, 2026-09-04)

**DEAD END. Rolled back.** The idea (below) was built behind an `FSCI_XIP_RAM` CMake option on pico-sdk 2.3.0,
device-tested, and abandoned: pinning the XIP cache as SRAM **disables the flash instruction cache**, and
FreeSCI runs from flash, so decode craters. Measured with an `[perf]` pic-decode timer (`FSCI_PROBE_PERF`,
kept — a reusable gated diagnostic in `operations.c` `gfxop_new_pic` + `pico_perf_us` in `pico_time.c`),
same SQ3 rooms, flash-cache-ON baseline vs XIP-cache-as-RAM:

| room | cache ON | cache OFF (XIP RAM) | slowdown |
|---|---|---|---|
| pic 777 | 88 ms | **980 ms** | **11.1×** |
| pic 900 | 91 ms | **593 ms** | **6.5×** |

An **order of magnitude** slower. Fatal two ways: (1) room loads become ~0.6–1.0 s (very noticeable), and
(2) — decisively — the 22 kHz synth IRQ needs a sample every ~45 µs; flash-resident synth code running ~10×
slow cannot keep up, so this would break the very sound it was meant to enable. The `xip_cache_pin_range()`
sub-range pin (keep ~3 KB cache) **won't save it either**: the decoder's hot-code working set is far larger
than 3 KB, so it still thrashes. **Whole-16 KB pin or sub-range, XIP-cache-as-RAM is not viable for a
flash-resident interpreter.** (A hypothetical alternative — put only the *synth code* in RAM via
`__not_in_flash_func` and keep the cache — doesn't need XIP-RAM at all and doesn't solve the *memory* problem
this was for; the sound *data* still has nowhere to live off the fragmenting main heap.)

**Bottom line for PicoCalc sound: still unsolved, and XIP RAM is off the table.** The remaining paths are the
non-XIP memory levers (flash OPL tables + mono + evict — which CLAUDE.md already found don't fully fit at the
ceiling) or the **Pimoroni mapped-PSRAM** track (branch `pico-pimoroni-mapped-psram`), which dissolves the
ceiling but has its own bring-up bug.

**pico-sdk 2.3.0 upgrade (done for this, now moot):** validated — FreeSCI builds clean against 2.3.0 (the
`pico_malloc` override + moved RP2350 memmaps survive), costs only **+472 bytes** static SRAM, needs picotool
2.3.0 (`-DPICOTOOL_FORCE_FETCH_FROM_GIT=ON`). It lives isolated in the worktree `~/Source/pico-sdk-2.3.0`; the
default `~/Source/pico-sdk` stays 2.2.0. Since XIP-RAM was the only reason to upgrade, 2.3.0 is **not needed**
now — keep or remove the worktree (`git worktree remove ~/Source/pico-sdk-2.3.0`) at will. **Kept on master:**
the `PICO_STDIO_USB_STDOUT_TIMEOUT_US=0` fix (non-blocking stdout — a real standalone-run improvement, commit
`7a4f9623`) and the `FSCI_PROBE_PERF` decode timer.

---

*Original (optimistic) analysis, SUPERSEDED by the measurement above — kept for the mechanism detail:*

The most promising way to make sound *fit* on the PicoCalc (as opposed to the Pimoroni board, which dissolves
the ceiling entirely — see the RP2040/Pimoroni section) is to put the **resident sound stack in the RP2350's
16 KB XIP cache-as-SRAM**, off the fragmentation-prone main heap. The resident sound cost is a **fixed ~13 KB**
(OPL chip state ~7 KB, mixer buffers, the PCM ring) — exactly the shape that wants a separate fixed pool. The
main-heap fragmentation wall is the real blocker for sound; moving those ~13 KB out of the arena sidesteps it.

**Hardware:** the RP2350 XIP SRAM window is `XIP_SRAM_BASE 0x13ffc000 … 0x14000000` = **16 KB**. On the PicoCalc
this is a good fit because the XIP cache serves **only flash** there (the PIO PSRAM is uncached), so carving it
costs only flash instruction-cache — unlike the Pimoroni target, where the cache also serves the mapped PSRAM,
making the XIP-RAM lever counterproductive (they compete for the same 16 KB). So this lever is **PicoCalc-only**.

**SDK support — needs an upgrade:** the **installed pico-sdk is 2.2.0, which has NO turnkey XIP-RAM** (the
address is defined but there's no linker region, allocator, or enable helper — you'd roll it yourself with
XIP_CTRL). **pico-sdk 2.3.0 adds turnkey support** (verified by reading the 2.3.0 tree):
- `PICO_USE_XIP_CACHE_AS_RAM` config flag + an **`__in_xip_ram(...)`** attribute macro to place data/functions
  in the `.xip_ram` section (linker region + `memmap_xip_ram` support, with `test/pico_xip_sram_test/`).
- **`xip_cache_pin_range(offset, size)`** — pins a *sub-range* of the cache as SRAM, leaving the rest caching,
  so the cache cost is **tunable** (sacrifice only what the sound buffers need, not all 16 KB).

**How it'd be used:** `__in_xip_ram` is *static* placement (fits fixed sound buffers directly). FreeSCI currently
`calloc`s the OPL chip (`fmopl.c`), so either make the chip state / mixer buffers / PCM ring static-in-xip-ram
(the "flash tables + mono" plan already leans static) or run a tiny bump allocator over `0x13ffc000`.

**Caveats:** (1) upgrade 2.2.0 → 2.3.0 first — real step, the RP2350 memmap paths moved between the two, so it's
a rebuild + full PicoCalc retest, not a drop-in; (2) measure the (pinned-sub-range) cache cost against the
22 kHz audio IRQ before relying on it. **Verdict:** the right-shaped lever for PicoCalc sound specifically —
fixed pool, off the main heap, tunable cost — gated on an SDK upgrade and a perf check. Back-pocket for the
sound track; orthogonal to the render and Pimoroni work.

### PARKED — LCD loading-progress display (planned, not implemented)

Goal: after game selection clears the LCD, mirror the engine's load messages to the LCD until the
first room is drawn (intro), so the long load isn't a blank screen. **Chosen approach = A** (capture
`sciprintf` via the string callback). Parked pending the user's go-ahead.

**Output-path facts (verified):**
- `sciprintf` (console.c:46) has two independent sinks: `con_passthrough` → `printf` → USB/UART, and
  `_con_string_callback(buf)` settable via `con_set_string_callback()` (console.c:91). The callback
  **owns `buf` and must `free()` it**. Callback is currently UNUSED on Pico.
- `gfxprintf` is `#define gfxprintf sciprintf` (resource.h:396); `GFXWARN`/`GFXERROR` route entirely
  through `sciprintf` → **captured by A**.
- `[mem]` lines come from `printf` in the `MEMPRINT` macro (pico_main.c:20), NOT `sciprintf` →
  **MISSED by A** (recoverable with one extra edit pointing MEMPRINT at `lcd_print_string`).
- LCD text engine: `lcd_print_string` (lcdspi.c:429) appends-and-scrolls; `lcd_clear` (439).

**Implementation sketch (≈3 edits):**
- pico_main.c: after the chooser, before `freesci_main` (line ~210), set a flag and register a
  callback `cb(buf){ lcd_print_string(buf); free(buf); }` via `con_set_string_callback`.
- pico_driver.c: when the flag is active, skip `pico_clear_screen_black` in `pico_init` and skip the
  `flush_region` in `pico_update` GFX_BUFFER_FRONT (so load text isn't wiped/overdrawn early). Note
  `_reset_graphics_input` (game.c:218) issues a FRONT flush *during* load — that's why the handover
  point is the first room composite, not the first FRONT flush.
- End-hook at the first `pico_render_background` call (operations.c:2311, the "first draw" signal):
  `con_set_string_callback(NULL); pico_clear_screen_black(); flag=0`.

**Leak note:** gameplay emits `sciprintf` continuously (kNOP unmapped, vol/pri selector, invalid
param var, song-handle warnings). The end-hook unregistering the callback at first room composite is
what prevents load text from leaking over the running game — it is essential, not optional.

### FIXED (device-confirmed) — PQ2 missing foreground objects = static picviews erased by the BACK restore (one-buffer Pico vs three-buffer SDL)

**DEVICE-CONFIRMED (2026-06-17):** the `pico_bake_static_region` fix works — the car-interior **face** and the
**parking-lot cars** now render and persist. **One residual OPEN:** the **glovebox closeup items** (2 of them)
are still not visible.

**PARKED (2026-06-19, user decision) — narrowed to a coordinate / partial-window panel-addressing asymmetry
BELOW `flush_region`; two candidate mechanisms remain, neither resolvable offline.** All glovebox diagnostics
have been removed from the tree (see "Diagnostics removed" below); this is a watch item, not active work. The
rect/flush trace was exhausted and is verified correct — the items go through the full pipeline yet the panel
stays empty (user device-confirmed "Still completely empty", freshest log pico.log 18:33 2026-06-17).
What the trace established before parking:
- **Drawn with real content:** view54 cel1 `(52,119 117x36) drawn=3126/supp=43`, cel4 `(41,139 96x34)
  drawn=2251/supp=234`, loop1cel2 `(224,159→96x34 clipped) drawn=2054/supp=543` — non-zero opaque pixels.
- **Baked into PSRAM static_bg** (`[pbuf] STATIC baked=1` then `BACK`, the working face/cars pattern) and
  **explicitly FRONT-flushed** (`[pupd] FRONT flush` covering both item regions); steady state never
  re-restores/overwrites the item region. So by the rect trace the items SHOULD be on screen.
- **Color/palette/visual[0]/bake/overwrite are ALL ruled out (`bgrect.c` + the `[pflush]` byte-sample probe,
  this session).** `[pflush]` dumped the *actual `visual[0]` index bytes* at the middle row right before the
  SPI send: item1 `(52,119)` midrow read `255,1,1,1,1,255` (idx1→blue body, idx255→white edges) and item2
  `(224,159)` read `8,4,6,6,6,4` (idx6→brown) — i.e. **visual[0] genuinely HOLDS the item pixels at flush
  time**, real item colors, distinct from the dithered background (`bgrect.c` histogram confirmed the bg is
  dithered: idx 8/136/255 etc., not the items' idx1/6). The flush loop and the probe read the *same*
  `visual[0]+(y+row)*320+x` through the *same* palette as the visible door.
- **DECISIVE white-pixel argument:** item1's midrow edges are value **255 (white)** — the *same* white index
  the door paints (door midrow `255,15,11,15,11,255`). The door at X≈209 shows; item1 white at X=52 (same Y
  band) does NOT, despite 29 separate flushes. Same buffer, same palette, same `flush_region` code — so the
  asymmetry is purely **coordinate / partial-window addressing**, NOT color, palette, visual[0] contents,
  bake, or overwrite. Yet the *full-screen* background flush (which spans x=52 AND x=224) displays fine →
  those columns ARE addressable → the bug is specific to the **small partial-window** `define_region_spi`
  flush at those coordinates.

**Two remaining candidate mechanisms (unresolved, need a device flash to distinguish):**
1. **Shipping-path missing FRONT flush for kAddToPic static items.** The door is flushed only because it is a
   *live cast member* re-emitted every frame; the items rely on the one-time AddToPic draw/bake/flush. The
   per-frame `PICO_DIAG_REFLUSH_STATIC` re-flush experiment (now removed) was meant to test this but its
   device result was not captured before parking.
2. **Rapid-fire `define_region_spi` timing / CS-settle bug** in the partial-window path — a small window set
   immediately after the previous transmit may not latch the new address window before `hw_send_spi`.

**Proposed one-shot experiment when work resumes (do this BEFORE more probes):** in `flush_region`, for one
item-sized rect, fill `line_buf` with a SOLID known color (e.g. pure red) instead of the palette-mapped
pixels and send it. If the solid block appears at the item coordinates → visual[0]/palette is irrelevant and
the bug is upstream (the real pixels never reach this flush on the shipping path = mechanism 1). If the solid
block does NOT appear → `define_region_spi`/SPI at those coordinates is the fault (mechanism 2). This isolates
panel-addressing from buffer-contents in a single flash. Do NOT touch the shared compositing path until this
picks the mechanism — the bake rewrite risk to the validated SQ3 render is real.

**Diagnostics removed (2026-06-19) — tree is back to the committed baseline:** the uncommitted `[pflush]`
probe (`flush_region`) and `[pcel]` probe (`operations.c` `_gfxop_draw_cel_buffer`) are deleted, and the
always-on behavior-changing `PICO_DIAG_REFLUSH_STATIC` experiment (per-frame re-flush of baked AddToPic rects
+ its `static_regions[]`/`static_region_nr` struct fields) is fully removed. `pico_driver.c` and
`operations.c` are now zero-diff vs HEAD. The COMMITTED `FSCI_PROBE_GFX`-gated probes `[pstat]`/`[pbuf]`/
`[pupd]`/`[pblit]` are LEFT in place — they compile out of clean/default builds (option default OFF) and cost
nothing, so they stay available for the next glovebox session. Desktop harnesses under `tests/` (`bgrect.c`,
`viewdump.c`, `picbg.c`, `pridump.c`, `celblit.c`) are untracked and left on disk.

*Historical (kept for the record — the two-hypothesis framing below was the pre-18:33-log state; the 18:33 log
+ user confirmation now SUPERSEDE it: both hypotheses' rect conditions are satisfied in the log yet the items
are still empty, which is exactly why the next step moved from rect tracing to pixel-byte dumping):*

**PREMISE OVERTURNED (log mining 8b10806a + 632c9597, 2026-06-17) — the items are NOT static picviews and NOT
drawn-once-then-erased; they are redrawn to `GFX_BUFFER_BACK` with real content EVERY focused frame.** The
earlier guess (same `GFX_BUFFER_STATIC` picview path as face/cars, bake "necessary but not sufficient") is
WRONG. Both post-bake logs show:
- **632c9597:** items `(243,121 36x27) drawn=534` and `(263,109 19x44) drawn=309`, `supp≈0`, logged
  `buf=BACK baked=0` on every frame they are focused — the exact live-cast pattern SQ3's working ego follows.
  They reach `visual[0]` with correct pixels each frame.
- **8b10806a:** in the idle glovebox state the items appear ZERO times; only the cursor `(160,150 16x16)`
  redraws.

So a bake/un-bake scheme (the approved "Option 1") is likely the WRONG fix — and it touches the shared draw
path, risking the validated SQ3 render. Since SQ3 proves the single-buffer model renders such cels fine, the
items *should* be visible; the bug is downstream of the draw. **Two hypotheses, neither resolvable from the
current logs:** (1) the **FRONT flush dirty-rect** passed to `pico_update(FRONT)` never covers the item
regions → visual[0] has them but the LCD is never updated there; (2) a **`static_bg` BACK restore** (which
lacks the items) erases them on the transition-to-idle frame, and the items leave the cast so aren't redrawn.

**Diagnostic added (uncommitted-then-committed): the `[pupd]` probe** in `pico_update` (`pico_driver.c`,
`FSCI_PROBE_GFX`-gated) logs every FRONT flush rect and BACK restore rect — the deciding data that was never
captured. **Next device capture:** open the glovebox closeup, move focus away so the items leave the cast,
grep `[pupd]` around `(243,121)`/`(263,109)`. Item rect drawn but no FRONT flush covers it → hypothesis 1 (fix
the flush dirty-rect, NOT a bake). BACK restore covers it with no following redraw before the flush →
hypothesis 2. Held off building the bake rewrite until this capture picks the correct fix.

**The `color_key` fix below is REAL but was NOT the missing-object cause (device-tested, did NOT help).** The
user flashed the `color_key` int→byte truncation fix and reported the foreground objects still missing: "No
cars on parking lot. No face, no items in glove box, while in car." So `color_key` truncation is a *separate*,
correct fix (it stops white index-255 **background** pixels being dropped as transparent — keep it), but the
PQ2 **missing-object** bug is elsewhere. Re-narrowed and root-caused below; the old `color_key`-as-root-cause
heading is retained verbatim afterward for the record but is **superseded** as the explanation for the missing
objects.

**ROOT CAUSE (strong code evidence, pending device confirm) — static picviews are drawn into `visual[0]` then
erased by the next `GFX_BUFFER_BACK` restore, because Pico has ONE visual buffer where SDL has a dedicated
STATIC buffer.** PQ2's missing objects (parking-lot cars, car-interior face, glovebox items) are **static
picviews** — `kAddToPic` scene objects — which draw through a DIFFERENT path than animated actors:
- Actors → `_gfxwop_view_draw` (`widgets.c`) → `gfxop_draw_cel` → `static_buf=0` → **`GFX_BUFFER_BACK`**.
- Picviews → `_gfxwop_static_view_draw` (`widgets.c`, labelled "PICVIEW") → `gfxop_draw_cel_static`
  (`operations.c:2192`) → `gfxop_draw_cel_static_clipped` → `_gfxop_draw_cel_buffer(..., static_buf=1, ...)` →
  `_gfxop_draw_pixmap(..., GFX_BUFFER_STATIC)` (`operations.c:384`).

**SDL (correct) uses THREE buffers** (`sdl_driver.c`): `visual[2]`=STATIC (background + baked-in picviews),
`visual[1]`=BACK (working), `visual[0]`=FRONT. `GFX_BUFFER_STATIC` cels land in `visual[2]` (`bufnr =
(buffer==GFX_BUFFER_STATIC)?2:1`, line 709), and `sdl_update` for **`GFX_BUFFER_BACK` copies FROM visual[2]**
(`data_source = (buffer==GFX_BUFFER_BACK)?2:1`, line 863) — so picviews baked into the static buffer are
reproduced on every BACK restore.

**Pico has ONE `visual[0]`** and `pico_draw_pixmap` **ignored the `buffer` parameter** (`int bufnr = 0;
/* single visual buffer serves back and static */`), so a `GFX_BUFFER_STATIC` picview landed in `visual[0]` —
visible *momentarily*. But the next `GFX_BUFFER_BACK` restore (`pico_update`, the `GFX_BUFFER_BACK` case)
re-blits `static_bg` — the PSRAM background **without** picviews — over `visual[0]`, **erasing the picview**.
This exactly explains the symptom set: backgrounds render (they ARE `static_bg`); actors render (drawn into
`visual[0]` *after* each BACK restore, same frame, then flushed); static picviews vanish (erased by the BACK
restore that follows their one-time `kAddToPic` draw).

**FIX (`pico_driver.c`, `HAVE_PICO`/Pico-only, uncommitted — awaiting device flash): bake static-buffer cels
into the PSRAM `static_bg`.** New `pico_bake_static_region(ps, dest)`, called from `pico_draw_pixmap` only when
`buffer == GFX_BUFFER_STATIC`, `psram_store`s the just-drawn `dest` region of `visual[0]` back into
`static_bg->index_data` in PSRAM. This makes `static_bg` the Pico analogue of SDL's `visual[2]` — subsequent
BACK restores blit the picviews straight back. Safe because `visual[0]` holds palette-slot bytes and
`static_bg->index_data` is the *identical* palette slots (identity LUT for the 256-color background pic), and
the cel was already priority-gated against the background when drawn into `visual[0]`, so the baked region is
the correct composited result. Bounds-clamped to `static_bg`'s `index_xl/index_yl`. Both Pico configs build
clean; desktop untouched. **Residual (accepted, secondary):** picview *priority* is NOT baked into the
priority map (Pico's `priority_map`/`static_priority_map` index_data is in PSRAM/NULL so `_gfxop_draw_priority`
is skipped), so an actor walking "behind" a static picview won't be occluded by it — same class as the
documented "static_priority_map aliased / last-drawn-wins" limitations. The objects now *render*; inter-sprite
occlusion vs picviews is a later refinement.

**WHAT TO TEST on device:** load PQ2 → the car-interior opening scene (the face + dashboard items must be
visible, not just the background), exit the car (parking-lot cars present), open the glovebox closeup (its 2
items shown). Walk/animate near a static picview and confirm it persists across frames (not flickering in then
vanishing). Regression watch: backgrounds, actors (ego), text, cursor still draw; no new garbage where a
picview's region is baked.

---

*Superseded explanation (kept for the record — the `color_key` fix is correct but is NOT the missing-object
cause; see above):*

### FIXED (pending device retest) — `color_key` int→byte truncation drops every white (index-255) background pixel

**ROOT CAUSE FOUND — `pico_blit_indexed` (`pico_driver.c`) narrowed `color_key` to a byte BEFORE testing
`has_alpha`.** `gfx_pixmap_t::color_key` is an **`int`**; `GFX_PIXMAP_COLOR_KEY_NONE == -1`
(`gfx_system.h:293`). The blit did `byte color_key = pxm->color_key;` then
`int has_alpha = (color_key != GFX_PIXMAP_COLOR_KEY_NONE);`. Truncating `-1` to a byte yields **255**, and
`255 != -1` → **`has_alpha = 1` for a transparency-free pixmap**. So the background `visual_map` (decoded
with `color_key = NONE`, `sci_pic_0.c:307`) was blitted as if palette index **255 (solid white) were the
transparent key** — every white background pixel was skipped, leaving holes that show stale `visual[0]`
content. Desktop is correct (`color_key=NONE` → `has_alpha=0`, all pixels painted), so this is a **Pico-only**
divergence — and it's the BACKGROUND, not the cels (the entire prior cel investigation was looking in the
wrong place, though it correctly *cleared* the cel paths).

**Proven by exact log-match, no device flash.** The captured device `[pblit]` background lines show
full-screen blits with `opaque < 64000`: `opaque=57185` (6815 px dropped) and `opaque=63991` (9 px dropped).
A desktop harness (`tests/picbg.c`, in-tree) decodes every PQ2 pic and counts palette-index-255 pixels in the
visual map: **pic 1 has exactly 6815, pic 33 (the car interior) has exactly 9** → `64000 − idx255` reproduces
the device opaque values to the byte. Mechanism confirmed: the dropped "transparent" pixels ARE the index-255
white background pixels. (Most PQ2 pics have thousands of index-255 px, so nearly every room had white holes.)

**FIX (`pico_driver.c`, `HAVE_PICO`):** test `has_alpha` on the **int** before narrowing —
`int has_alpha = (pxm->color_key != GFX_PIXMAP_COLOR_KEY_NONE); byte color_key = has_alpha ? (byte)pxm->color_key : 0;`.
Now a `color_key=NONE` background paints all pixels (incl. index 255); real keyed pixmaps (view cels,
`color_key=255`, `sci_view_0.c:87`) are unchanged. Pico builds clean; desktop untouched (file is Pico-only).
**Awaiting device retest** (flash → load PQ2 → car interior / parking lot / glovebox should render fully, no
white holes / garbage). NB the earlier-cleared cel paths (priority, blank-cel, palette, geometry) stay
cleared — the bug was never in the cels.

*Original investigation notes (cel paths, all correctly CLEARED — kept for context):*

PQ2 on Pico shows: (a) opening car-interior scene — a "blue box" over the character's head; (b) exiting
the car — parking-lot cars missing; (c) glovebox closeup empty (should show 2 items). Background pic
renders fine; only certain cels fail. Desktop renders all three correctly.

**The Pico PRIORITY pipeline is byte-for-byte CLEARED — it is NOT the cause (proven, do not re-chase).**
A desktop reference harness (`/tmp/pridump.c`, built against the `build-pri` `FSCI_PROBE_GFX` static libs)
decodes PQ2 pic 33 through the **same desktop decode path** (`gfxr_init_pic` + `gfxr_clear_pic0` +
`gfxr_draw_pic01`, with the 8th `sci1` arg = **0** for SCI0 — passing `resmgr->sci_version`=1 there
mis-parses as SCI01 and yields garbage; that was a harness bug, now fixed) and cross-checks against the
device `[pblit]` log (f21cedf3). Decisive datum: the **fully-opaque** head cel `dest=(108,65 135x58)`
(`drawn+supp = 5356+2474 = 7830 = 135×58`, i.e. zero transparency) — the desktop harness counts **exactly
2474** pixels with priority>3 in that rect, **identical to the device `supp=2474`**. So the Pico
priority-map decode, the PSRAM nibble readback, and the `pico_blit_indexed` gate are pixel-identical to
desktop. The other cels' `harness_over ≥ dev_supp` gaps are fully explained by transparent pixels (harness
counts all pixels in the rect; device `supp` excludes transparent ones). Cel-priority computation is shared
engine code (`priority_first=42`, `priority_last` keyed on resource-detected `s->version`; `game.c`/
`kgraphics.c`), no Pico path. **Conclusion:** the pri-1/2/3 cels in the car scene are low-priority
windshield/interior overlays *correctly* occluded by the pri-12/13 car frame — the "blue box over the head"
is that pri-3 overlay correctly hidden behind the dashboard, render-identical to desktop. The stale CLAUDE.md
"color written unconditionally" note (see limitations below) is wrong — the gate matches `gfx_crossblit.c`.

**The log f21cedf3 contains ONLY the one car-interior scene (pic 33).** The cars/glovebox symptoms are
*different pics not captured* — they cannot be analyzed from this log. The only `drawn=0` cels in it are
tiny 10×1 menu-bar strips (top-left), not the reported objects.

**Redirect: the symptoms point at VIEW-CEL DECODE/CONTENT on Pico, not occlusion.** "Empty glovebox,"
"missing cars," and a head-as-solid-block are all consistent with view cels decoding to wrong/empty pixel
data — and this path has prior Pico-specific bugs (the mirrored view-RLE overrun in `gfxr_draw_cel0`,
`sci_view_0.c`; the B-1.3 view-cel-into-priority-scratch borrow). The `[pblit]` probe reports priority
stats only, not pixel *content*, so it can't tell "suppressed" from "empty source." **Next diagnostic:**
extend `pico_blit_indexed` to log EVERY cel (not just suppressed) with its opaque-pixel count, so a fresh
capture of the glovebox/cars scenes distinguishes `opaque=0` (empty/garbled source = decode bug) from
`opaque>0,drawn=0` (priority-suppressed) from garbage. Active investigation = `sci_view_0.c` decode path.

**CONFIRMED (every-cel `[pblit]` capture, pico.log, PQ2 run, 2026-06-16) — real view cels decode to EMPTY,
geometry guard is NOT the cause.** The probe was extended to log every cel (not just suppressed) with its
opaque-pixel count + a `<TOP>` tag for `dest.y<100`. A full PQ2 session captured **941 `[pblit]` lines**:
- **0 SKIP-GUARD drops** — the geometry guard (`pico_blit_indexed:475`) dropped nothing. **Cleared as a
  cause; do not re-chase it.**
- **psram=1 (real view cels from the PSRAM offload): 401.** Of these — **305 drew normally** (`drawn>0`,
  e.g. `pri=10 dest=(54,28 212x46) opaque=6635 drawn=6635`), **57 "suppressed"** (`opaque>0 drawn=0`) which
  are almost entirely **trivial 1-pixel menu-bar strips** (`pri=0 dest=(0,10 11x1)` ×33, `10x1` ×19 — benign
  top-bar redraw, not the missing objects), and **39 decoded to `opaque=0`** (all-transparent / empty pixel
  data).
- **The 39 empty cels are essentially ONE animated sprite plus one strip:** a `pri=6 dest=(x,76 12x35)` cel
  tracking across **x=14→31** frame-by-frame (`bgpri=99..-1` = the priority gate never even ran because there
  were no opaque pixels) — i.e. a **walking character rendering completely invisible** — and a single
  `pri=10 dest=(263,109 4x32)`. These are real cels that came through the PSRAM view-cel path with **zero
  drawable content**.
- **psram=0: 540** — background fills / text written-through (not view cels), irrelevant to this bug.

**Verdict (SUPERSEDED — see the OVERTURNED note two paragraphs below; kept for the device-data record):** the
every-cel capture *seemed* to show the bug was VIEW-CEL DECODE producing empty pixel data — `psram=1 opaque=0`
cels read back with no non-`color_key` pixels. The desktop harness later proved this reading wrong:
`opaque=0` is **normal** blank SCI0 content (SQ3 has it too and renders fine), so these blank cels are NOT the
missing objects. Do not act on this paragraph's verdict — read the OVERTURNED note below.

**RULED OUT — it is NOT a version-forked code path (PQ2 0.000.490 vs SQ3 0.000.685 run byte-identical view
decode).** The two games' interpreter revisions differ (~195 apart) but both **detect as `SCI_VERSION_0`**
(=1): both carry the SCI0 main vocab + non-VGA views, so resource.c:671-672 lands both on `SCI_VERSION_0`
(pico.log confirms PQ2 `Resmgr: Detected SCI0`). The interpreter version number is NOT consulted on the view
path — only the resmap shape + vocab/view-type probes, which agree. Consequence, traced through
`gfxr_interpreter_get_view` (`sci_resmgr.c`): line 417 `version < SCI_VERSION_01` → **both** get `palette=-1`;
the `switch` `case SCI_VERSION_0:` → **both** call `gfxr_draw_view0`; the `>= SCI_VERSION_01_VGA` palettize
(line 437) fires for **neither**. In `gfxr_draw_view0`, `palette=-1` fails the `(palette>=0)` guard (line 267)
so the translation table + `GFX_PIXMAP_FLAG_PALETTIZED` are skipped for both; and `gfxr_draw_cel0` has **zero**
version awareness (same 7-byte cel header, same pure-RLE `count=op>>4,color=op&0xf,memset`). So a version-keyed
branch CANNOT explain why PQ2 cels come back empty while SQ3's don't — **do not re-chase the version gap.** The
game-specific failure on a shared path narrows to: (1) **PQ2's cel DATA** — different mirror flags, or a
`color_key` (`resource[6]`) / run pattern this decoder mishandles (e.g. a cel whose only color equals its
`color_key` → every run maps to `color_key` → `opaque=0`); or (2) the Pico `psram_store`/`psram_load`
round-trip (`sci_view_0.c:181-189`), which SQ3 also uses (so less likely game-specific). Suspect #1 is the
cleaner explanation; the decisive test is the pre-store-vs-post-load `dest[]` byte dump named above.

**OVERTURNED (desktop harness `tests/viewdump.c`, 2026-06-16) — `opaque=0` is NORMAL SCI0 content, NOT a bug
signature. The "view cels decode to EMPTY = the bug" verdict above is WRONG.** A new desktop harness
(`tests/viewdump.c`, kept in-tree) decodes SCI0 view cels through the **same shared engine path** the Pico
uses (`gfxr_draw_view0` → `gfxr_draw_loop0` → `gfxr_draw_cel0`), no `HAVE_PICO` / no PSRAM, and counts opaque
(`index != color_key`) pixels per cel. It disproved BOTH remaining suspects at once:
- **The PSRAM round-trip (suspect #2) is innocent.** PQ2 **view 450 loop 0 cel 0 (12×35)** decodes
  **`opaque=0` on the DESKTOP harness** — byte-identical to the device `[pblit]` empty `pri=6 12x35` sprite.
  Same emptiness with **no PSRAM involved** → the round-trip is not losing content; the cel simply decodes
  blank.
- **The blank decode is CORRECT, not a decoder bug (suspect #1 also innocent for these cels).** View 450 raw
  bytes (`size=59`): `loops_nr=1`, one cel 12×35, **`color_key=0`**, data is dominated by `0xc0` runs
  (`count=12, color=0`). Color 0 **equals** the cel's `color_key=0`, so every run maps to the transparent key
  → a genuinely, intentionally transparent cel. `gfxr_draw_cel0` decoded it exactly right.
- **DECISIVE control: SQ3 (which renders perfectly) is ALSO full of `opaque=0` cels** — e.g. SQ3 view 1001
  loop 2 cel 0 is a **93×108 fully-blank cel** — yet SQ3 has no missing-object bug. So a cel decoding to all-
  `color_key` is **normal** SCI0 blank/placeholder data (animation-frame padding, unused loop slots), present
  in both games. The device `[pblit] opaque=0` lines are **noise**, not the failing objects.

**Consequence — the PQ2 investigation must be RE-NARROWED.** The 39 `opaque=0` device cels (one tracking
12×35 sprite + a strip) are blank-by-design, not the cars/glovebox/blue-box. The real missing objects must be
among the cels that **DO** decode with content (`opaque>0`) yet render wrong on Pico — so the bug is in
**positioning / occlusion / palette mapping of opaque cels**, NOT blank-cel decode. Next step: identify which
view/loop/cel the actually-missing objects use (from a targeted device `[pblit]` capture of the glovebox/cars
scenes), run those exact view numbers through `tests/viewdump.c` to confirm they decode `opaque>0` on desktop,
then compare desktop-decoded content vs the device blit for *those* cels — do NOT keep chasing the blank cels.

**PALETTE MAPPING + SKIP-GUARD GEOMETRY both CLEARED (desktop harness `tests/celblit.c`, 2026-06-16).** A
third harness (`tests/celblit.c`, in-tree) ports `pico_driver.c`'s two device-only blit decisions to the
desktop and runs them against the SAME shared-engine cel decode, so they can be diffed WITHOUT a flash:
1. **Palette mapping** — the Pico has no per-pixmap translation table; it maps each local cel colour to a
   256-slot palette entry via `nearest_pal()` (closest RGB in `gfx_sci0_pic_colors[]`), whereas SDL renders
   the cel colour's TRUE RGB. The harness flags every cel colour whose `nearest_pal`-mapped RGB ≠ its true
   RGB (a device-only colour error). 2. **Skip-guard geometry** — `pico_blit_indexed` silently DROPS (cel
   invisible) any cel whose `index_xl/index_yl` is outside `[1..320]/[1..200]`; the harness flags any cel the
   guard would drop.
   **Result: PQ2 — 230 views scanned, ZERO palette divergences, ZERO skip-guard drops** (only view 140,
   size 8, a trivial/empty resource, failed to decode). SQ3 control: 210 views, same — zero of either.
   Verbose dumps confirm the checks run (every opaque cel reports `divergent_px=0`). This is expected and
   *confirms the theory*: pure-EGA SCI0 cels map exactly (EGA colour k sits at slot k×17, distance 0), and no
   PQ2 cel exceeds 320×200. **So palette mapping and geometry-drop are RULED OUT as PQ2 failure modes.**

**Cumulative narrowing — what is now CLEARED for the PQ2 missing-object bug** (each by a desktop harness, no
device flash): priority decode/occlusion-gate (`tests/pridump.c`), blank-cel decode (`tests/viewdump.c`:
`opaque=0` is normal), palette mapping + skip-guard geometry (`tests/celblit.c`). **What REMAINS** — the
device-only render decisions a static cel-decode harness canNOT model, i.e. **composite-time placement and
inter-sprite occlusion**: (a) where the cel is *placed* (ego/actor x,y + `xoffset`/`yoffset` hotspot), (b)
the documented "last drawn wins" inter-sprite priority limitation (`pico_blit_indexed` skips the priority
writeback when reading from PSRAM → `row_pri` NULL → later sprites aren't occluded by earlier higher-priority
ones), and (c) the `static_priority_map`-aliased-to-`priority_map` accumulation (sprite priorities never
cleared between frames). These are all **dynamic** (depend on runtime actor coordinates + draw order).

**THE EXISTING `pico.log` ALREADY HAS the per-cel placement + draw-order data — no fresh flash needed for
it (2026-06-16).** The current pico.log is the 941-line every-cel `[pblit]` capture, spanning ~6 distinct
backgrounds (re-composited into 23 background-draw segments); every line carries the cel's `dest=(x,y wxh)`,
`pri`, `bgpri` range, `drawn`, and `supp`. Mining it for Pico-only render divergence among cels that reached
the blit comes up **EMPTY**:
- **305 psram=1 real cels drew normally**; the 39 `opaque=0` ones are normal blank SCI0 content (see the
  viewdump OVERTURNED note); the 57 "suppressed" are almost all trivial 1-px menu-bar strips `(0,10 10–11×1)`.
- **Exactly ONE genuinely-suppressed real object in the whole capture:** `pri=12 dest=(136,164 18x6)
  opaque=76 bgpri=13..13 drawn=0` (in the car-interior scene, pic 33). **VERIFIED CORRECT, not a bug:** a
  one-off desktop harness (`/tmp/prirect.c`, decodes pic 33's priority map via `gfxr_draw_pic01` and prints
  the rect) shows the desktop background priority over `(136,164 18×6)` is **uniformly 13 — identical to the
  device `bgpri=13..13`** → a pri-12 cel is correctly fully occluded on BOTH. Render-identical.

**Consequence — `[pblit]` has told us everything it structurally can; another `[pblit]` capture is the WRONG
next step.** A per-blit probe only logs cels the engine actually *submits* to the blit. If the PQ2 missing
objects (glovebox 2 items, parking-lot cars) are absent because their cel was **never submitted** (view not
loaded / wrong loop-cel index / disposed / a `kAnimate` cast-list issue), `[pblit]` is silent on them — you
cannot see a never-submitted cel in a per-blit log, and this capture shows no suppressed/garbled real cel
that would explain the symptom. So the discriminating next probe is **engine-side submission tracking** (was
the expected view/loop/cel ever handed to `gfxop_draw_cel` / added to the animate cast?), NOT more `[pblit]`.
Open sub-question to settle first: confirm whether the glovebox/parking scenes were even visited in this
session (the captured cels are dominated by the car-interior + message windows) — if not, one capture *known*
to include those scenes is still needed, but the probe to add for it is the engine-side "was this cel
submitted" trace, not the blit-side `[pblit]`.

(Cleanup still pending: the `[pblit]` probe in `pico_driver.c` currently prints EVERY cel — the
1-in-8 throttle `if ((pb_call++ & 7) == 0)` and the suppressed-only filter were removed, plus a `<TOP>`
tag added, for this diagnosis; restore the throttle and drop the every-cel/`<TOP>` instrumentation when done.)

### RESOLVED (device-confirmed 2026-06-22, all 3 scenes) — SQ3 intro background loss was the overlay (`add_to_pic`) path drawing onto a PSRAM-offloaded base; `color_key` only UNMASKED it

**Device-confirmed fixed:** all three reported cases — the SQ3 logo behind "Pirates of Pestulon", the
starfield behind the Two-Guys panels, AND a third overlay scene — now composite correctly. The fix is in
`gfxr_interpreter_calculate_pic` (`sci_resmgr.c`, SCI0 Pico decode block, `HAVE_PICO`-only).

**RE-INVESTIGATED + STILL-RESOLVED-AT-HEAD (2026-06-29) — a reported "Pestulon disappeared again" was NOT a
committed regression; it renders correctly at clean HEAD.** A device session reported the title hidden *behind*
the SQ3 logo (not on white). Findings:
- **No committed change broke it.** `6e38cb56` is the sole fix commit; the only render-path commit since
  (`7d5d767d`, sound) is entirely `PICO_PWM_AUDIO`-gated (default OFF), the rest are docs. So nothing in the
  default-build render path changed since the 2026-06-22 confirmation.
- **Added a gated `[ovl]` probe** (`sci_resmgr.c` overlay decode, `FSCI_PROBE_GFX`): logs `restore_base`, the
  base PSRAM `vaddr`, and the visual-buffer sum **before/after** the overlay's own draw (`delta`). It names the
  failing link directly — `restore_base=0` or `sum_before≈255*N`=white/no-base, garbage `sum_before`=stale
  `vaddr`, `delta=0`=overlay drew nothing.
- **Device: 3/3 cold boots on the probe build (≈HEAD) AND the clean shipping build rendered Pestulon
  correctly.** Healthy `[ovl]` baseline (pic id=2974, the overlay): `restore_base=1 vaddr=4904
  sum_before=2929353` (a real logo — NOT ~16.3M=white) `delta=405509` (the overlay **drew**). The 2nd `[ovl]`
  line is the `pic_unscaled` re-decode (`gfxop_add_to_pic` calls `gfxr_add_to_pic` twice): it reloads the
  composite (`sum_before` = prior `sum_after`) and redraws the same pixels (`delta=0`). Both correct.
- **Conclusion:** the overlay path is **runtime-state-fragile but correct at clean HEAD**; the earlier
  "disappeared" was almost certainly an artifact of *uncommitted* in-session experiments (an `operations.c`
  dirty-rect freelist, an overlay re-stage attempt, and ~9KB of extra GC-pool resident pressure — all
  reverted), not a HEAD bug. The `[ovl]` probe is **kept as standing diagnostic**: if it ever flickers again,
  capture the `[ovl]` line and diff against the baseline above to name the flip.

**Root cause — the Pico SCI0 overlay decode never preserved the base pic.** `overlay:` (sel 0x0111) →
`kDrawPic` with `add_to_pic=1` → `gfxop_add_to_pic` → `gfxr_add_to_pic`, which composites the overlay pic
onto the cached base pic (`res->scaled_data.pic`) with `DRAWPIC01_FLAG_OVERLAID_PIC`. Desktop preserves the
base by `memcpy`ing `undithered_buffer` back into `visual_map->index_data` before drawing the overlay commands
(`sci_resmgr.c` `#else` branch) and does **not** clear. The Pico branch did neither: it allocated a fresh
decode buffer, called `gfxr_clear_pic0` **unconditionally** (fills the play area with **0xff = white**,
`sci_pic_0.c:354`), then drew only the overlay's own commands — so the base logo/starfield was gone and
untouched areas were white. (On Pico the base's `visual_map->index_data` is also PSRAM-offloaded/NULL, so the
buffer started empty anyway.)

**Why it looked like a `color_key` regression (commit `4be87cb4`) but wasn't.** Before `4be87cb4`,
`pico_blit_indexed` truncated `color_key -1 → 255`, so index-255 (white) was wrongly treated as transparent.
The overlay buffer's white clear areas were therefore **skipped** at blit, letting the previously-drawn base
(still in `visual[0]` from the earlier `drawPic`) show through → looked correct by accident. The `color_key`
fix made white a real paintable color, so the white clear now **painted over** `visual[0]`, wiping the base.
The `color_key` fix is **correct** (needed for PQ2's NONE-keyed white backgrounds) — it merely removed the
camouflage on this pre-existing overlay bug. The earlier `pico_bake_static_region` suspicion was **wrong**:
offline disassembly of the intro scripts (script 1 = logo, script 19 = starfield) showed both use
`overlay:`/`drawPic:` and **never** `addToPic:`, so the STATIC bake path was never involved.

**Fix.** In the SCI0 Pico decode block: `restore_base = (flags & DRAWPIC01_FLAG_OVERLAID_PIC) &&
visual_map->psram_valid && priority_map->psram_valid`. When set, instead of `gfxr_clear_pic0`, `psram_load`
the base visual (`index_xl*index_yl` bytes) and base priority (nibble-packed, `(npix+1)>>1`) from the base's
own `psram_addr` back into the fresh decode buffers; the overlay's commands then draw on top, and the
composited result re-offloads as usual. The base's `psram_valid`/`psram_addr` survive because the PSRAM bump
arena is only rewound on a fresh `drawPic` (`gfxop_new_pic` → `gfxr_free_all_pics` → `psram_reset`), never on
`add_to_pic`, and the fields aren't overwritten until the re-offload (after the clear point). The non-overlay
path is unchanged (still clears). The Pico analogue of the desktop `undithered_buffer` restore; keeps the
`color_key` fix intact so PQ2 is unaffected. Both Pico configs build clean; desktop untouched.

---

*Historical (the OPEN investigation that led here — kept for the record):*

User-reported on device: in the SQ3 intro the **"Pirates of Pestulon"** title/credit is drawn on a **white
background** instead of composited over the actual **Space Quest 3 logo** that should be behind it. The logo
backdrop is missing/blanked under the title. User offered a photo ("I could provide a photo if I am fast
enough") — **not yet captured**, and **no `FSCI_PROBE_GFX` pico.log of the intro exists yet**, so this is a
report, not a diagnosis.

**Likely mechanism family (unconfirmed — do NOT act before a capture):** this is the SQ3 logo *screen*, which
is exactly the pic the pic-open-flash fix (above) staged through `pico_render_background`. Two candidate
causes, both in the single-`visual[0]` / static-bake area already mapped:
1. **The logo background pic never composited under the title** — if the Pestulon title is a static picview
   (`kAddToPic`) or a separate pic drawn after a *blank/white* fill, the `visual[0]` it lands on is white
   rather than the logo. Same one-buffer-vs-three-buffer class as the PQ2 static-picview bug
   (`pico_bake_static_region`) and the pic-open staging change.
2. **Palette/`color_key` on the logo pic** — a white-index background not being painted (cf. the `color_key`
   int→byte truncation fix) or the logo decoding into a buffer that the title's flush overwrites.

**PHOTO EVIDENCE (device, IMG_1773 mid-transition + IMG_1775 settled, 2026-06-22):**
- **IMG_1773:** a left-to-right **wipe/curtain transition** is revealing a **flat white** screen carrying the
  red "The Pirates of Pest…" script, *replacing* the blue SQ3 logo — which is still visible un-wiped on the
  right, sitting on a **dark** background. So the logo screen IS on dark, and the Pestulon screen wipes in
  over it.
- **IMG_1775:** settled full screen — "The Pirates of Pestulon" red script on **flat white**, logo gone.
- **Two refinements this gives us:** (1) the **red script decodes perfectly** (correct shape/colour/stair-
  stepping) → the cel/text path is fine; the bug is **purely the background** (white where it should be
  logo-on-dark). (2) There is a **real wipe transition** logo→white — a wipe is a `kDrawPic` *open
  animation*, i.e. SQ3 issues a genuine **new pic draw**, not a cel overlay on the persisting logo. So the
  white is a *decoded pic background*, not a failure to composite a picview over the logo.

**Narrowed hypotheses (post-photo):**
- **H-bg-colour:** the new Pestulon pic's background should be dark (logo/space showing or a dark card) and
  is decoding/filling **white** — a palette / fill-colour / `color_key` issue on the *background* of that
  specific pic (cf. the `color_key` int→byte fix, but here the wrong colour is white not transparent). The
  flat (un-dithered) white argues for a fill/clear-colour bug rather than a dithered light card.
- **H-no-overlay:** the logo is supposed to **persist** under the Pestulon text (text added via picview /
  `kAddToPic`, NOT a full new pic), and Pico is wrongly doing a full white pic redraw. The visible wipe
  transition makes this **less likely** (desktop would show no wipe if it were a pure overlay) but not
  impossible — confirm by whether desktop shows the same wipe.

**DESKTOP CONFIRMED CORRECT (user, 2026-06-22) → this is a Pico-ONLY background-loss bug.** The SDL build
shows the **SQ3 logo persisting behind** the red Pestulon script; Pico fills **flat white** and loses the
logo. So **H-bg-colour is the live hypothesis and H-no-overlay is essentially confirmed**: SQ3 draws the
Pestulon title as an **overlay that preserves the existing logo screen** (an add-to-pic / picview style draw,
NOT a fresh full background), and Pico is wrongly **clearing `visual[0]`/`static_bg` to white** instead of
keeping the logo underneath. This is the same single-`visual[0]` / static-bake family as the PQ2
missing-object work (`pico_bake_static_region`) — except here the failure is the *background* being wiped, not
a picview failing to bake.

**SECOND CASE + REGRESSION CONFIRMED (user, IMG_1776 Pico vs IMG_1777 desktop, 2026-06-22).** Another SQ3
intro scene — the "Two Guys"/Pestulon panel scene: two green-bordered view panels (a red ship on the left, an
alien-runic text block on the right) over a **black space starfield**.
- **IMG_1777 (desktop, correct):** dense blue/white **starfield** behind the panels; stars show *through* the
  panel interiors.
- **IMG_1776 (Pico, wrong):** ship + alien text render fine, but the background is **flat dark gray — the
  starfield is gone**.
- **The user states this is a REGRESSION — the intro rendered correctly on Pico before — and that it appeared
  at the same time as the Pestulon-on-white bug.** So treat both as ONE regression in the Pico **background**
  path: **foreground cels (red script, ship, alien text) draw correctly; the background pic content is lost
  and replaced by a flat colour** (white for the logo screen, gray for the starfield).

**SUSPECT (git, narrowed) — `4be87cb4` "bake static picviews into static_bg" is the prime suspect.** Only
three recent commits touch the Pico background path: `d5ba144e` (stage-without-eager-flush — pure flush
*timing*, leaves `visual[0]` content unchanged → unlikely, the symptom is wrong *content* not a flash),
`2b9f75db` (chooser/restore arena reset only — cannot affect a single fresh boot's intro), and **`4be87cb4`**,
the only one that changed what pixels land in the background buffer. `4be87cb4` did TWO things: (1) the
`color_key` int→byte fix in `pico_blit_indexed` (a NONE-keyed background now paints **all** pixels incl.
index-255 white, where before index-255 was dropped as transparent), and (2) added `pico_bake_static_region`,
which copies each just-drawn `GFX_BUFFER_STATIC` cel's bounding rect from `visual[0]` into the PSRAM
`static_bg`. Candidate mechanisms (not yet isolated): the bake **clobbers the background in `static_bg`** with
a static picview's opaque/white-filled rect (intro panels are `kAddToPic` STATIC picviews), so the next BACK
restore reproduces the clobbered background; and/or the `color_key` change now paints a white add-to-pic
overlay fill over the logo instead of dropping it. NB the `color_key` fix is *needed* for the PQ2 white-holes,
so a plain revert is not the answer — but it may be the lever that exposes the regression.

**Decisive next step — A/B revert test (one flash) over more probes.** Build a test firmware with `4be87cb4`'s
TWO changes split and tested independently: first revert *only* the `pico_bake_static_region` call (the
`if (buffer == GFX_BUFFER_STATIC) pico_bake_static_region(...)` in `pico_draw_pixmap`) and check the intro; if
still broken, restore that and revert *only* the `color_key` hunk. Whichever revert restores the starfield +
logo names the cause. Confirm the chosen revert does NOT re-break PQ2 (cars/face) before settling on a fix.
Alternatively/additionally an `FSCI_PROBE_GFX` intro log (grep `[pblit]`/`[pbuf]`/`[pupd]` for a STATIC bake
or a white background composite right before the panel cels). Held per the run-first /
don't-touch-the-shared-compositing-path rule; this is Pico-driver-local, not the shared path.

### OPEN (diagnosed, not fixed) — pic-open transition shows a shrinking garbage rectangle (Pico): `old_screen` clobbered by `psram_reset()`

User-reported on device: the PQ2 parking-lot fade-in (and pic-open transitions generally) shows a **rectangle of
garbage that shrinks then disappears** as the new room wipes in — "indicates a non-initialized graphics buffer."
**Root-caused (code, not yet fixed):** the transition draws a **stale screenshot** (`old_screen`) whose PSRAM bytes
were overwritten before it is read back. Sequence:

1. **`kgraphics.c:1472`** — `s->old_screen = gfxop_grab_pixmap(..., gfx_rect(0,10,320,190))`. The 320×190 = 60800-byte
   grab is >4096, so `pico_grab_pixmap` (`pico_driver.c:836`) bump-allocates it in the **PSRAM arena**
   (`psram_alloc(60800)`) at the current top.
2. **`kgraphics.c:1493`** — `gfxop_new_pic` → `gfxr_free_all_pics` → **`psram_reset()`** (`resmgr.c:245`) rewinds the
   bump offset to **0** (`psram_alloc.c:23`, no floor). The new pic then offloads its `visual_map` (64000) + priority
   (~16000) + view cels from offset 0 — **overwriting the bytes `old_screen` points at.**
3. **`kgraphics.c:3171`** — `animate_do_animation` grabs `newscreen` (valid — taken *after* the reset).
4. **`kgraphics.c:3189`** — draws the **corrupted** `s->old_screen` full-screen, then the `switch` reveals `newscreen`
   over it. The garbage = the overwritten region of `old_screen`; "shrinks then disappears" = the normal reveal
   covering it. It's a *rectangle* (not full screen) because the new pic's offloads only overlap part of `old_screen`'s
   stale offset; the non-overlapping high part still holds genuine old pixels.

Shared-engine grab-before-new-pic ordering, correct on desktop (3 real SRAM buffers survive); on Pico `old_screen` is
the one grabbed pixmap whose lifetime straddles a `psram_reset()`. Same family as the RESOLVED "garbage rectangle
during shadow/priority redraws" (that fixed cached *views*; this is the transition grab). **Recommended fix (not
built):** give `old_screen` a **dedicated fixed PSRAM slot** outside the bump arena — exactly the
`PICO_PARSE_SCRATCH_ADDR` (0x700000) visual-borrow pattern; reserve e.g. 0x710000. `newscreen` stays in the bump arena
(grabbed post-reset, freed before next room). Routing detail: `pico_grab_pixmap` is generic, so only the
`kgraphics.c:1472` `old_screen` grab must be steered to the fixed slot (a small Pico flag around the grab/free sites,
or a dedicated helper). ~1–2 file change; deferred per ask-first.

### OPEN — The Colonel's Bequest (SCI0): OOMs sooner + dialog boxes render transparent with sticky corners (Pico)

User-reported on device (2026-06-23): **The Colonel's Bequest** generally **loads and plays** on Pico, but:
- **Hits OOM sooner than SQ3** — a heavier SCI0 game (more/larger resources), so the working set runs nearer the
  ceiling. The captured `[OOM]` LCD dump (IMG_1780) is the **legible halt working as designed, NOT a HardFault**:
  `size=0xc` (12 B), `free=0x10` (16 B), `arena=0x73fe0` (**475,104 = the exact clean-build physical ceiling**),
  `line=0x2a` (42), `reg_t_hashmap.c` `reg_t_hash_map_check_value` — i.e. a 12-byte **GC** hashmap node alloc failing
  with the arena maxed at the absolute ceiling and only 16 B free. Same fragmentation-at-the-ceiling class as the SQ3
  notes; this game simply reaches it faster. No Pico-specific fix beyond the standing
  fragmentation/transient-churn levers (the port is at the practical SRAM ceiling).
- **Dialog boxes render incorrectly:** the box **stays transparent instead of a white background**, and the **fancy
  (ornate) box corners stick on the background and are not repainted** when the box is dismissed. The transparent-fill
  symptom is likely the same `color_key` / window-fill family as the PQ2 white-background and SQ3 dialogue-dismiss work
  (single-`visual[0]` vs SDL's dedicated buffers; box fill not painting, dismiss not restoring the region). NOT yet
  investigated — recorded as a known issue. Next step if pursued: an `FSCI_PROBE_GFX` capture of a Colonel's Bequest
  dialog open+dismiss, compared against the SDL render, to see whether the box-fill pixmap is dropped at blit
  (color_key) or the dispose path skips the BACK/static restore over the corner regions.

### Known graphics limitations on Pico (not yet fixed)

These are correctness gaps in the Pico render path vs the SDL pipeline. Lower priority than the
roadmap above (gameplay works without them), but documented so they aren't rediscovered cold.

**UPDATE 2026-07-18 — PARTLY SUPERSEDED: the occlusion half was since BUILT and is a zero-SRAM win (behind
`PICO_STATIC_VIEW_BAKE`, default OFF). The parked framing below assumed the only fix was a 32–64KB working
map; that was wrong for the actual user bug (actors over STATIC views), which is fixed by baking static-view
priority into the existing PSRAM map at zero new SRAM. The "inter-sprite z-order among MOVING actors" gap
below is still genuinely parked. See the "BUILT + DEVICE-TESTED → TOGGLED OFF (2026-07-18)" section below.**

**PARKED (2026-06-22, user decision) — investigated, root-caused, not pursued. The three bullets below are
ONE root cause (no SRAM working priority map on Pico), and the first bullet's old "writes color
unconditionally" claim is now CORRECTED: background occlusion already works.** The fix is real but spends
scarce resident SRAM directly against the OOM work (the port is at the SRAM ceiling), so it is documented
and left as a known limitation rather than built.

- **Background→actor occlusion ALREADY WORKS — the old "writes color unconditionally" note was STALE.**
  `pico_blit_indexed` (`pico_driver.c:559-584`) already gates the **color** write on the background
  priority: it reads the priority row back from PSRAM (`s_shared_priority`, nibble-unpacked per row into
  `s_pri_row`) and does `if ((int)pri_row[x] <= priority) row_dst[x] = lut[idx];` — exactly matching the
  SDL crossblit gating (`gfx_crossblit.c:79-84`). So the ego DOES hide behind higher-priority background
  scenery (e.g. walking behind a desk). Do **not** re-file this as a bug; verified by code read 2026-06-22.
- **The REAL gap = inter-sprite / actor-over-picview z-order ("last drawn wins" among sprites).** The one
  line that differs from SDL is `pico_driver.c:574`: `if (row_pri) row_pri[x] = (uint8_t)priority;` —
  Pico writes the drawn sprite's priority back ONLY to an SRAM working map (`pri_buf`/`row_pri`), but that
  map is always **NULL** on Pico (the 64KB working priority buffer was offloaded to PSRAM and is read-only
  there). SDL instead writes priority back into its resident working map (`gfx_crossblit.c:80`), so each
  drawn sprite raises priority at its pixels and the next sprite is gated against it → true z-order.
  Without the writeback, a later sprite's color is gated only against the *background* priority, never
  against an earlier sprite's → sprites paint over each other in draw order.
- **Static picviews have the same gap, plus their priority is never in the map at all.** `kAddToPic`
  picviews are baked into the PSRAM `static_bg` for *color* (`pico_bake_static_region`) but their
  *priority* is never written into the priority map (`_gfxop_draw_priority` is skipped — `index_data` is
  PSRAM/NULL), so actors at any priority paint over the **PQ2 parking-lot cars** / **SQ3 ship motivator**
  (device-observed 2026-06-22). Same root cause as the sprite bullet.
- **`static_priority_map` is aliased to `priority_map`** (`operations.c:724`, `_gfxop_init_common`,
  `HAVE_PICO`), so the per-frame static→working copyback (`operations.c:1557`,
  `gfx_copy_pixmap_box_i(priority_map, static_priority_map, box)`) is a self-copy no-op (and `index_data`
  is NULL anyway). On SDL these are distinct buffers and the copyback erases last frame's sprite
  priorities; aliased, there is nothing to erase because nothing was ever written.

**The blit code is already fix-ready — what's missing is a frame-level SRAM working priority map.** Lines
`559-587` fully support an SRAM `pri_buf`: pass one in and the existing logic reads from it, gates color,
AND writes priority back (so sprites gate against each other). The fix is therefore plumbing + SRAM, not
blit-logic: (1) maintain a resident SRAM working priority map; (2) at BACK-buffer reset, page the
background priority PSRAM→working for the dirty region; (3) bake static-picview priority into it alongside
the color bake; (4) pass it as `pri_buf` to every sprite blit. **Cost options weighed:** A = full unpacked
64KB (simplest, mirrors SDL, likely a non-starter at the ceiling); B = nibble-packed 32KB (half cost,
needs pack/unpack on read+write); C = dirty-rect scratch sized to the cast region (most memory-responsible,
most invasive — needs a shared per-frame buffer). **Decision: parked.** All three cost resident SRAM, the
scarcest resource, so the visual payoff (correct sprite/picview occlusion) does not currently justify the
SRAM + complexity. If revisited, C is the only memory-responsible route; the blit side needs no changes.

### BUILT + DEVICE-TESTED → TOGGLED OFF (2026-07-18) — static-view occlusion bake: PRIORITY half is a clean zero-SRAM win, COLOR half is un-bakeable on one buffer

The "parked" occlusion gap above was re-attacked and the outcome splits cleanly in two: **priority occlusion
can be baked for free and works; color persistence cannot be baked on a single buffer and regresses dynamic
scenes.** All of it is now behind CMake `option(PICO_STATIC_VIEW_BAKE)` (**default OFF** = exact pre-2026-07-18
baseline). Device-tested ON; **left OFF pending the user's baseline retest.** NOT committed at time of writing.

**Corrected root-cause read (supersedes the "occlusion already works / only inter-sprite is broken" note
above):** the actual user-visible bug was **actors drawn OVER static views** — Roger over SQ3's door +
motivator (room 2), the PQ2 parking-lot cars over the character, glovebox items missing. Diagnosis via the
disassembler (`stopUpd`/`setPri` in the scripts) proved these are **not** inter-*sprite* z-order: they are
`kAddToPic` picviews **and** settled `stopUpd`→NO_UPDATE dynviews whose **priority was never written into the
map** on Pico (the desktop `_gfxop_draw_priority(static_priority_map,…)` at `operations.c:380` is skipped
because `priority_map->index_data` is NULL/PSRAM). So the fix is *not* the 32–64KB working map the parked note
demanded — it is baking the **static** view's priority into the PSRAM map, which is **zero new SRAM**.

**Change A — priority bake (CLEAN, the keeper) — `pico_driver.c` `pico_blit_indexed`.** `pico_blit_indexed`
already reads the background priority row back from PSRAM every row to gate color (`s_pri_row`/`s_pri_pack`);
it just never wrote anything. Added a `bake_static_pri` param (set by `buffer == GFX_BUFFER_STATIC` in
`pico_draw_pixmap`): for a STATIC draw it now writes the cel's priority back into the PSRAM map, reusing the
same scratch, with the exact desktop `_gfxop_draw_priority` condition (`existing < priority`). The
nibble-pack read-modify-write was unit-tested off-device (312 alignment/width/priority cases + a mutation
negative-control; harness in scratch). **Cost measured, not asserted:** `__end__`/`.bss` byte-identical with
vs without (heap ceiling stays 475,104 clean), +312 B flash. This is the analogue of SDL writing priority into
`static_priority_map`; moving actors (`GFX_BUFFER_BACK`) only READ the map, so no cross-frame priority trail.
**Result:** PQ2 cars correctly occlude the character; SQ3 motivator correctly hides Roger. Priority occlusion
is a real, free win.

**Change B — route NO_UPDATE dynviews through the STATIC path — `widgets.c` `_gfxwop_dyn_view_draw`.** Change
A only fires for STATIC-buffer draws (=`kAddToPic` picviews). SQ3's door/motivator are `stopUpd`→NO_UPDATE
**dynviews** drawn to BACK, so A alone did NOT fix them (device-confirmed on the A-only flash: Roger still over
both). B makes a settled NO_UPDATE dynview (`view->signal & 0x0004`) draw STATIC-then-BACK like a picview
(`_gfxwop_pic_view_draw` already does this), so A bakes its priority AND `pico_bake_static_region` bakes its
color. **This is what makes the glovebox items appear and fixes the SQ3 door/motivator z-order — and it is
also what regresses every dynamic scene**, because the COLOR half is permanent and a single buffer cannot
un-bake it:
- **Picked-up items ghost** — PQ2 glovebox: take the ID, the view is removed but the baked pixels stay in
  `static_bg`.
- **Baked foreground redraws over dialogs** — PQ2 car windshield / police-dept front door are `stopUpd`
  views; once baked into `static_bg` a later BACK restore blits them back **on top of** an open dialog box
  → "dialogs overshadowed/corrupted."
- **Animating-away leaves a ghost** — SQ3 door baked closed, then its open animation plays but the closed
  door stays in the background; PQ2 glovebox "close animation then flips back open"; and the **SQ3 intro
  Pestulon-logo overlay disappears again** (the overlay path re-clobbered by the bake, cf. the RESOLVED
  overlay note — the color bake re-breaks it).

**The clean split (the load-bearing conclusion):** **priority (occlusion) is safe to bake; color (appearance)
is not.** Priority-only would fix SQ3 motivator + PQ2 cars + SQ3 door-occlusion with **none** of the four
regressions (no color in `static_bg` → nothing to ghost or overdraw dialogs), at the cost of leaving the
glovebox items *missing* (their appearance genuinely needs color persistence = save-unders, the parked memory
cost) and a minor door **priority**-ghost (Roger clipped by the now-open door's stale priority footprint —
far less visible than the color ghost). **Priority-only is a better axis than the user's mooted PQ2-vs-SQ3
runtime switch**: it is correct for both games at once (occlusion vs appearance), not a per-game guess.

**TOGGLE SPLIT DONE (2026-07-18, commit after `308d356d`) — two levels, priority-only is now the default:**
The single `PICO_STATIC_VIEW_BAKE` switch was split into two so the clean half ships on and the regressing
half stays opt-in:

- **`PICO_STATIC_VIEW_PRIORITY` (default ON)** — bake static-view **priority** only. Enables `bake_pri` in
  `pico_blit_indexed` (so kAddToPic picviews write priority) AND routes settled NO_UPDATE dynviews through
  the static path so they write priority too (`widgets.c` `_gfxwop_dyn_view_draw`, `view->signal & 0x0004`).
  For the dynview case it sets the new `pico_priority_only_static` global so `pico_draw_pixmap` **skips
  `pico_bake_static_region`** — priority is baked, color is NOT persisted to `static_bg` (the view's color
  still lands in this frame's `visual[0]` via the fall-through BACK draw, exactly like baseline). Net:
  actors are occluded by SQ3 door/motivator + PQ2 cars, **zero color regressions** (nothing in `static_bg`
  to ghost or overdraw dialogs), +4 B `.bss` (the one global int), heap ceiling unchanged. Residual: a
  view that later animates leaves a stale **invisible** priority footprint (minor door clip) — the accepted
  trade. **DEVICE-CONFIRMED (2026-07-18): SQ3 "worked great"** — Roger correctly occluded by the door +
  motivator, dialogs/overlay/glovebox-close all clean, no color regressions. Shipped ON.
- **`PICO_STATIC_VIEW_BAKE` (default OFF, implies PRIORITY)** — additionally persist NO_UPDATE-dynview
  **color** into `static_bg` (flag stays 0 → color bakes). Makes the PQ2 glovebox items *appear*, at the
  cost of the four device-confirmed color regressions above (picked-up-item ghost, dialog overdraw,
  door/glovebox animate-away ghost, Pestulon overlay loss). Opt-in experiment only.
- **True baseline escape hatch:** `-DPICO_STATIC_VIEW_PRIORITY=OFF` compiles both halves out (`bake_pri=0`,
  the widgets.c routing `#if`'d out) → exact pre-2026-07-18 render path.

Desktop untouched (both files `HAVE_PICO`-gated). All four configs build clean (priority-ON default, BAKE-ON,
desktop, priority-OFF baseline).

**PQ2 DEVICE RESULTS (2026-07-19) — priority-only default: 3 wins, 2 minor known limitations. LEFT AS-IS
(user decision).** Same priority-only build, PQ2 (parking lot / Lytton PD, room 10):
- ✅ **Cars occlude the officer correctly** (they are `kAddToPic` picviews — genuinely static, so permanent
  priority baking matches desktop's `static_priority_map` exactly).
- ✅ **Glovebox items show AND pick up cleanly** — UNEXPECTED (priority-only skips the `static_bg` color
  bake, so the earlier model predicted them *missing*). Best current theory: a settled `stopUpd` view is
  drawn twice — once via the static path (**fullscreen clip**) and once via the normal **port-clipped** draw
  — and in that closeup the port-clipped draw is clipped away, so the fullscreen-clip static draw is what
  makes the items visible (bypassing the clip that hid them in baseline). Unverified, but it means the
  glovebox win is **entangled** with the fullscreen-clip static draw (below).
- ⚠️ **Officer over-occluded by the "Detective Div" door** (device photo IMG_2159). The door is a `stopUpd`
  view (disasm: `door::cue` → `stopUpd`), same class as SQ3's door, so priority-only bakes its priority
  **permanently**. Desktop keeps a `stopUpd` view's priority **transient** (written to the per-frame working
  map, cleared each frame, re-applied only when the view is redrawn), so the officer passes *in front*;
  Pico's permanent bake stands the door priority in the map forever → over-occludes. **Same mechanism that
  is the SQ3 fix** — SQ3 *wanted* the persistent occlusion, PQ2 does not. Permanent baking of `stopUpd`-view
  priority therefore produces opposite correctness per scene; it cannot be "right" for all games without a
  transient map.
- ⚠️ **Door bleeds slightly over a dialog's top edge** (IMG_2160). The static routing uses
  `gfxop_draw_cel_static`'s **fullscreen clip**, so when the scene redraws (room 10 has continuous traffic
  → `kAnimate` runs even under the narration box) the door's color is written fullscreen-clipped, ignoring
  the message-window clip, landing over the dialog. The dialog fill itself is *not* priority-gated (it is a
  box fill), so this is a color-draw-over, not priority suppression.

**Why not fixed:** the obvious surgical fix for the dialog bleed (stop the static routing from writing color,
letting the normal port-clipped draw supply it) would very likely **also remove the glovebox items**, since
the same fullscreen-clip static draw appears to be what makes them visible — the win and the two bugs share
plumbing. So the two limitations are accepted as-is rather than risk the three wins on an offline guess.

**The real fix (parked) = the transient working priority map** (the 32–64 KB `.bss`/scratch weighed in "Known
graphics limitations", option C). That is the *only* thing that makes `stopUpd`-view priority behave per-frame
like desktop (occlude only where/when the view is actually active), fixing PQ2's door + dialog while keeping
SQ3 + the cars + the glovebox. Blocked on SRAM headroom (port is at the ceiling).

**On a runtime per-game/per-room mode (user idea):** the axis that actually decides correctness is **not**
per-game or per-room — it is **per-frame, per-pixel** (does an actor currently share the view's priority
band?), which is exactly what the transient working map computes. A hand-tuned per-room "bake this view or
not" table is *possible* as a stopgap but is fragile and game-specific (every affected room needs authoring,
and a wrong entry silently over/under-occludes). Recommendation: treat the working map as the real
"adopt-at-runtime" answer; a per-room table is a last resort, not the plan.

The glovebox-items case remains genuinely blocked on per-view save-unders / a working buffer (the 64KB-class
memory work); priority-only appears to get it "for free" in the static closeup but this is incidental to the
fullscreen-clip draw, not a general solution.

### OPEN (separate track, NOT the render work) — PQ2 clone-table OOM at copy-protection (2026-07-18)

On the priority-only build (above), PQ2 booted, played the intro, showed the copy-protection screen briefly,
then halted with a legible `[OOM]` LCD dump (photo, not a serial log — exact `free`/`arena` obscured by panel
reflections): **`realloc` failed in `alloc_clone_entry` (`seg_manager.c`)** — the documented clone-table
realloc-grow fragmentation OOM (see the `GC_INTERVAL 2048` correctness note: "if it OOMs again under heavy
animation, lower it"). **This is NOT caused by the render change and is not a crash:**
- The priority-only change is **gfx-layer only** — it does not create VM clones (clones come from `kClone` in
  scripts); the failing allocation is the VM clone table.
- It is **SRAM-neutral** — it writes priority into the PSRAM map (not SRAM) and in priority-only mode *skips*
  the `static_bg` color store, so it does **less** heap work than baseline.
- The full-BAKE build (which does *more*) got *past* copy-protection into the glovebox/car earlier, so
  priority-only cannot be consuming more SRAM to fail earlier — most likely run-to-run fragmentation variance
  (PQ2 runs closer to the ceiling than SQ3: more resources, 1843 vocab words vs SQ3's 1489).

**Not chased this session (user chose to commit the render win and park this).** Levers when picked up, in
order: (1) capture a `pico.log` for the exact `free`/`arena` (fragmentation = free ≫ size but no contiguous
block, vs true exhaustion); (2) lower `GC_INTERVAL` (2048→1024, `vm.h`) so disposed clones are reclaimed
before the table's `realloc`-grow can't find a contiguous run — the cheapest, lowest-risk shot, and exactly
the tunable the GC note calls out. Orthogonal to all the static-view render work.

### SCI version support on Pico — SCI0 ONLY (SCI1/VGA legibly rejected, not supported)

The Pico graphics path is **SCI0-only**. Attempting an SCI1/VGA game (e.g. **Jones in the Fast Lane**,
SCI1) used to **HardFault**: the player saw the credits text, hit Enter to start, and the device faulted
in `pico_blit_indexed` (`pico_driver.c`, the `byte idx = row_src[x]` read) with `BFAR=0x8000` — a wild
source-pointer deref. Decoded chain (PC→`pico_blit_indexed`, LR→`pico_render_background`, resolved against
`build-pico/src/freesci.elf`).

**Root cause — the Pico PSRAM decode wiring exists ONLY in the SCI0 branch.** `gfxr_interpreter_calculate_pic`
(`sci_resmgr.c`) splits at `if (state->version >= SCI_VERSION_01_VGA)`: the **lower** (`#else`, SCI0) branch
has the entire `HAVE_PICO` offload — borrow `visual[0]` as the decode buffer, nibble-pack the priority map,
`psram_alloc`/`psram_store` the visual/priority `index_data`, set `psram_valid=1`/`index_data=NULL`. The
**upper** (`version >= SCI_VERSION_01_VGA`, true for any SCI1/VGA game) branch decodes a VGA pic with **none**
of that. So `static_bg` reaches `pico_render_background` → `pico_blit_indexed` with `index_data==NULL` AND
`psram_valid==0` → neither blit source path is valid → wild deref. (View decode is the same story:
`gfxr_draw_view0`/`sci_view_0.c` has the per-cel PSRAM offload; the VGA `gfxr_draw_view1`/`gfxr_draw_view11`
paths do not.)

**EGA cannot be forced.** The SCI version is **detected from the resource files** (`resource_map.c` sets
`sci_version`), not a render-mode toggle. A VGA game's resources (256-colour palettes, view1/view11 cel
format) have no EGA equivalent unless a separate EGA *release* of the game is supplied; there is no FreeSCI
"force EGA" config that transcodes VGA resources. So this is not a flag — real SCI1 support would be a
substantial port (VGA palette handling, view1 cel decode into the per-cel scratch, and the PSRAM offload
wiring added to the entire `version >= SCI_VERSION_01_VGA` branch of `sci_resmgr.c` + the view1/view11
decoders).

**FIX (legible failure, not SCI1 support) — `operations.c` `gfxop_new_pic`, `HAVE_PICO`-gated.** A guard at
the very top of the Pico path: `if (state->version >= SCI_VERSION_01_VGA) { pico_oom_report("SCI1/VGA game
not supported (SCI0 only)", …); return GFX_FATAL; }`. `state->version` is `resmgr->sci_version` (the
`SCI_VERSION_*` enum; `SCI_VERSION_01_VGA`=3). It halts on the **LCD** via the same legible-halt channel as
every OOM (fault-safe SPI), naming the unsupported version, **before** any borrow/decode/blit runs — so an
SCI1 game shows a clear message instead of a mystery HardFault. Desktop is untouched (guard is inside
`#ifdef HAVE_PICO`; desktop renders SCI1/VGA fine through the SDL pipeline). Both configs build clean.
**Awaiting device retest** (flash + load Jones → expect the LCD "SCI1/VGA game not supported" halt, not a
HardFault).

### RP2040 portability note

Current heap on the RP2350/PicoCalc is ~388 KB post-init. The same firmware on an RP2040 would have
~388 − (520 − 264) ≈ **132 KB**, so the SCI0 pic decoder (~128 KB peak even after the visual/priority
split) leaves almost no room. RP2040 feasibility now looks **doubtful**: the obvious headroom win —
offloading script/resource data to PSRAM — was ruled out (script `buf`s are hot read-write VM memory,
see roadmap ✗), so the resident script working set stays in SRAM with no easy way to shed it. What's
left to keep out of SRAM is view-cel and control-map data, which alone is unlikely to bridge the
~256 KB gap. The PSRAM PIO driver, `lcdspi`, `i2ckbd`, and FatFS all already run on RP2040; the sound
path's software floats (no RP2040 FPU) would also need attention. Treat RP2040 as aspirational, not a
near-term target, until a way to shrink the SCI0 decode peak and the resident VM working set is found.

### Pimoroni Pico Plus 2 (memory-mapped PSRAM) — WORKING, branch `pico-pimoroni-mapped-psram`

**Status (2026-09-05): the mapped-PSRAM target runs, and it dissolves the SRAM ceiling.** SQ3, PQ2 and
Colonel's Bequest boot and play; SQ3↔PQ2 game switching works; in-game restore works. This is an
**additional** target — the PicoCalc PIO-PSRAM build remains supported and is byte-identical (`.bss`
17,280) because every change below is behind `PICO_PSRAM_MAPPED`.

The board is a **Pimoroni Pico Plus 2** (RP2350B, 8 MB PSRAM on the QMI second chip-select, mapped at
`0x11000000`) which drops into the PicoCalc socket. Unlike the PicoCalc's PIO-SPI PSRAM (store/load only),
this PSRAM is **addressable**, so it can hold hot read-write data directly.

**Bring-up fix (the old "TFT garbage" blocker):** the custom `psram_qmi_init` (manual `ASSERT_CS1N` toggling
plus a `0x9F` ID read) left the QMI/PSRAM in a bad state. Replaced with the device-tested **frank-snes**
sequence verbatim (`~/Source/frank-snes` `drivers/psram_init.c`): `AUTO_CS1N`, `NOPUSH`, **no ID read**,
hardcoded 8 MB, with the boot smoke test as the presence check. No SWD debugger was needed.

#### Memory model — the default is INVERTED on this target

`sci_malloc`/`calloc`/`realloc` allocate from a **6 MB PSRAM heap** by default; the few buffers that must
stay fast opt back into SRAM via **`sci_malloc_sram()`**. This replaced routing one subsystem at a time.

| region | contents |
|---|---|
| PSRAM `[0, 2MB)` | the per-room offload **bump arena** (`psram_alloc`/`psram_reset`) — unchanged |
| PSRAM `[2MB, 8MB)` | the persistent **heap** (`psram_heap.c`): scripts, object vars, vocab, resource cache, clone/node/list/hunk tables |
| SRAM | graphics only: `visual[0]`, the static visual buffer, both priority maps, pixmap `index_data`/`data`, `drv->state` |

**Two invariants make the wholesale flip safe:**
1. **Free and realloc route by OWNERSHIP** (`psram_heap_owns`), never by call site.
2. **Pre-init falls back**: before `psram_heap_init`, `psram_hmalloc` returns NULL and `psram_heap_owns`
   returns 0, so early-boot allocations use SRAM and still free correctly.

**THE CRITICAL GOTCHA — raw `free()` must be ownership-aware too.** Plenty of engine/gfx code frees
`sci_malloc`'d blocks through **raw `free()`/`realloc()`** (long accepted, because both APIs used to land on
the same heap: `sm_free_script`'s `free(object->variables)`, `game_exit`'s `free(s->game_version)`, the gfx
layer). Once the heaps diverged this HardFaulted in newlib `_free_r` — it read a "chunk header" out of PSRAM
payload bytes and faulted on the resulting wild `fd`/`bk`. Fixed at the **single chokepoint**: the linker's
`--wrap` routes every raw call through `pico_mem_census.c`, so `__wrap_free`/`__wrap_realloc` check ownership
there (both the census-on and census-off variants). Fix it there, never at call sites — that covers paths
nobody has enumerated.

**`malloc_usable_size()` must NEVER see a PSRAM pointer** — it walks picolibc chunk headers and faults on a
non-SRAM address (this is what killed the old Codex `load_script` rank-1 suggestion). Every site is now either
behind an ownership early-return or provably operating on a raw-`malloc` result.

**Kept in SRAM deliberately, for two distinct reasons:**
- *Touched per pixel* (the 16 KB XIP cache cannot hide the QSPI link): `visual[0]`, the static visual buffer,
  both priority maps, pixmap `index_data`/`data`, and **`drv->state`** — `ps->palette[]` is read PER PIXEL in
  the flush loop.
- *DMA targets*: the SD SPI driver uses DMA and `_read()` passes the caller's buffer straight to `f_read`, so
  the compressed-input buffers (`decompress0/01/1/11`) and the patch-file `res->data` would be DMA'd into
  PSRAM. RP2350's XIP cache is probably coherent across bus masters, but these are transient buffers so SRAM
  is near-free insurance. The DECOMPRESSED output is CPU-written and stays in PSRAM.

**`psram_heap.c` — first-fit + lazy coalescing + a NEXT-FIT ROVER.** The rover is not an optimisation, it is
required: a plain restart at `s_base` costs O(blocks) per malloc and **every step reads a block header out of
PSRAM over QSPI**, so the GNF parser (thousands of ~250 B `_vinsert` allocations per command) degraded to
O(n²) and produced a clearly noticeable typing lag. Measured on that pattern: **309.4 ms → 0.4 ms**.
*Invariant:* the rover must always point at a valid block START — coalescing can absorb the block it points
at, so every absorb site re-points it (`COALESCE_ABSORB`). Correctness covered by `tests/psram_heap_test.c`.

#### Measured result — the ceiling, and the fragmentation, are gone

SQ3 room 2, `FSCI_PROBE_MEM` build, engine-in-SRAM vs engine-in-PSRAM:

| metric | before | after |
|---|---|---|
| SRAM `used` | 323,812 | **148,660** (−175 KB) |
| SRAM `arena` | 351,844 | **163,428** (−188 KB) |
| `[psheap] used` | 51,648 | 256,960 |
| free `chunks` | 16 | **2** |

**The chunk count is the significant number.** The SRAM heap is essentially **unfragmented**, because almost
nothing churns in SRAM any more. On this target the whole fragmentation/arena-ratchet story that dominates the
rest of this file **does not apply** — do not carry those conclusions over. (They remain fully valid for PIO.)

#### The desktop render model, restored (device-confirmed)

The freed SRAM was spent on the two buffers the Pico path never had:

- **`PICO_WORKING_PRIORITY`** (+128 KB) — two real 320×200 maps. Un-aliasing `static_priority_map` from
  `priority_map` is the core: that alias is why the `PRECISE_PRIORITY_MAP` copyback was a self-copy no-op and
  a view's priority was baked PERMANENTLY. Now the clean plate is restored into the working copy every frame,
  so priority is TRANSIENT like desktop. Note `_gfxop_draw_priority` is **suppressed on Pico** — it reads the
  SOURCE cel's `index_data`, which is in PSRAM (NULL), so it can only emit "without index data!"; the driver's
  blit does the writeback instead. The driver takes BOTH maps so a STATIC draw writes the clean plate.
- **`PICO_STATIC_VISUAL`** (+64 KB) — the desktop `visual[2]` analogue; BACK restores copy from it (an SRAM
  memcpy replacing a per-row PSRAM read) and the PSRAM background stays PRISTINE.

**This closed BOTH PQ2 limitations recorded 2026-07-19** (dialog bleed, officer over-occluded by the Detective
Div door) **plus the glovebox items, with SQ3 and the Pestulon overlay unaffected**, and gives real
inter-sprite z-order for the first time. The record predicted exactly this and parked it as *"blocked on SRAM
headroom (port is at the ceiling)"* — that blocker does not exist here.

**CORRECTION to the 2026-07-19 notes:** the theory that the glovebox items were visible only because of the
static path's **fullscreen clip** is **WRONG**. They are `kAddToPic` picviews drawn via `_gfxwop_pic_view_draw`,
which was never routed through the (now compiled-out) dynview path; they were lost in a
`PICO_STATIC_VIEW_PRIORITY=OFF` build for a **priority** reason, and real per-frame priority fixes them properly.

**RULED OUT along the way:** raising the save-under SRAM threshold (to dodge `psram_reset` clobbering a
save-under). It did not fix the dialogs, and the `old_screen` grab alone is 320×190 = 60,800 B on every pic
transition — with the two new buffers it drove the arena to 470,628 of a 474,724 span and OOM'd. Save-unders
stay in PSRAM on both targets.

#### Options (all mapped-only, all default ON, each an A/B)

| option | effect |
|---|---|
| `PICO_PSRAM_SCRIPTS` | `script_t.buf` → PSRAM heap |
| `PICO_STATIC_VISUAL` | dedicated static visual buffer |
| `PICO_WORKING_PRIORITY` | desktop-style per-frame priority maps (supersedes `PICO_STATIC_VIEW_PRIORITY`) |

`PICO_PACK_VOCAB` now defaults **ON** for all Pico builds — it is the shipping config, and an unpacked vocab
(1843 separate allocations on PQ2) fragments SRAM badly enough to fail the 64 KB `visual[0]` on a cross-game
switch. Building it OFF caused that failure twice.

#### PARKED / NEXT STEPS (as of 2026-09-05, end of the sound session)

Ordered roughly by value. Nothing here is in progress.

**1. KQ4: Rosella "swims in the lawn"** (opening sequence). UNDIAGNOSED. My control-map theory was
DISPROVEN, so do not restart from it: static control collision already works (the scan reads the pic's own
nibble-packed PSRAM map), and a *missing* map would make her walk, not swim -- something actively reports
water. Two cheap captures first: the `Resmgr: Detected SCI...` line (KQ4 shipped in several forms; a
non-plain-SCI0 detection would explain divergent semantics), and `[oc]` control-mask scans from an
`FSCI_PROBE_GFX` build to see whether the map really says "water" over the lawn.

**2. Right-size the audio buffers (~24KB reclaimable, both targets).** `PICO_PWM_BUF_FRAMES` (rate/11) and
the 8192 ring were sized to survive RARE polls, before the poll fix existed. With polls now at 60Hz a batch
only needs rate/60 frames (~367 at 22kHz, ~184 at 11kHz). Keep the ring generous enough to ride a ~250ms
room-decode stall, but the mixer compbuf (2 * buf_size * 4) can shrink a lot. This is the cheapest large
saving available and it is what would decide item 3.

**3. PIO sound at 11kHz.** Analysed, NOT device-tested. The quality blocker is inherited for free (the poll
fix is shared code), so the only question is memory. With the oversized buffers it is ~27KB against a ~26KB
margin; with item 2's right-sized buffers it is closer to ~17KB, which is plausible. But the documented PIO
failure was `calloc 16384 failed` at `sm_allocate_stack` -- a CONTIGUITY failure at ~15KB resident -- so it
may trip regardless. Worth one flash, not worth engineering effort. `build-pico-sound11k` recipe:
`-DPICO_PWM_AUDIO=ON -DPICO_SND_RATE=11025`.

**4. Dropped notes: implement voice stealing** (`opl2.c` `adlibemu_start_note`). Upstream FreeSCI simply
discards a note when all ADLIB_VOICES (12) are busy -- literally `XXX implement overflow code`. Affects
desktop equally. This is what "some things are cut off" in the music actually is.

**5. `old_screen` transition garbage** (documented + diagnosed under "OPEN -- pic-open transition shows a
shrinking garbage rectangle"): the 320x190 grab lives in the PSRAM BUMP arena, which `psram_reset()` wipes
on the next room. The recorded fix is a dedicated fixed PSRAM slot outside the bump arena (the
`PICO_PARSE_SCRATCH_ADDR` pattern). NB do NOT "fix" it by moving save-unders to SRAM -- that was tried this
session and reverted: the `old_screen` grab alone is 60,800 bytes on every pic transition and it OOM'd.

**6. Colonel's Bequest dialog boxes** (transparent fill, sticky ornate corners). Recorded as open, never
investigated. Now worth a fresh look: this target has the desktop buffer set and per-frame priority maps,
which is exactly the machinery whose absence caused the analogous PQ2 problems.

**7. Runtime control writes (actor-to-actor blocking).** `state->control_map` is NULL, so
`draw_line_to_control_map` is a silent no-op. A real SRAM control map was BUILT AND REVERTED this session
because it buys nothing on its own: the other consumer, `_gfxop_draw_control`, reads the SOURCE cel's
`index_data`, which is in PSRAM (NULL), exactly as `_gfxop_draw_priority` does. The real fix is extending
`pico_blit_indexed` with a control buffer and writeback, mirroring the priority path -- the blit is the only
code that can read a PSRAM cel. Only worth doing if a game demonstrably needs it.

**8. SCI1/VGA.** Currently rejected with a legible halt (SCI0-only). The PSRAM headroom makes it far more
plausible than when that limit was set, but it still needs VGA palette handling, view1/view11 cel decode,
and the offload wiring across the whole `version >= SCI_VERSION_01_VGA` branch.

**9. Branch/maintenance question.** PIO and mapped are both supported, which means two render models and two
memory models to keep working. Every shared-file change needs the PIO `.bss == 17,280` check. If the
Pimoroni board ever becomes the standard (it drops into the PicoCalc socket), the offload layer,
store/load, nibble packing and the static-view bakes could all be deleted -- that is where the real
simplification is.

#### Open on this target

- **KQ4: Rosella "swims in the lawn"** in the opening. Suspected **control map**: `state->control_map` is NULL
  on Pico, so collision falls back to the pic's nibble-packed PSRAM map, and KQ4's opening picks swim-vs-walk
  from control colours. The fix is likely the same pattern as priority — give `control_map` a real 64 KB SRAM
  buffer and unpack the pic's control map into it (~119 KB free, so it fits). NEXT UP.
- SDK is pico-sdk **2.2.0** (no turnkey PSRAM — hence the vendored QMI init).
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
