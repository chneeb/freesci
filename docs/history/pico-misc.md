# Pico — miscellaneous history (branch status, parked ideas, RP2040)

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 5-48 -->

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

> **Branch `pico-4bpp-packing`** (pushed to `fork`) holds the abandoned 4bpp/nibble-packed visual-buffer
> experiment. Master keeps `PICO_DITHER_D16` and the harnesses; see "4bpp visual buffer" near the end.

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

<!-- from CLAUDE.md lines 2538-2567 -->

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

<!-- from CLAUDE.md lines 3385-3396 -->

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

<!-- from CLAUDE.md lines 4584-4587 -->

#### PARKED / NEXT STEPS (as of 2026-09-05, end of the sound session)

Ordered roughly by value. Nothing here is in progress.

<!-- from CLAUDE.md lines 5018-5048 -->

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

