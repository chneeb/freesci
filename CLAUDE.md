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

## Diagnostic probe toggles

The leftover instrumentation from the SQ3 bring-up is kept in the tree for debugging OTHER games,
gated behind compile-time CMake `option()`s so default builds are clean. They are **top-level**
(not Pico-scoped) so the desktop mirror probes compile on desktop too; Pico-only probes also require
`HAVE_PICO` in their own `#if`. Default OFF for clean builds, **except `FSCI_PROBE_STR`** (default ON,
it guards the still-open "aspb" heap-corruption bug).

| Option (default) | Define | Probes gated | Where |
|---|---|---|---|
| `FSCI_PROBE_STR` (**ON**) | `FSCI_PROBE_STR` | `[strprobe]` — SCI string kernels writing past the dest buffer's real size; `kFormat` overflow check in `CHECK_OVERFLOW1` | `kstring.c` |
| `FSCI_PROBE_GFX` (OFF) | `FSCI_PROBE_GFX` | `[pcol]`/`[ctl]` (priority/control decode), `[oc]` (onControl scans), `[pblit]` (occlusion) + desktop mirrors `[dpcol]`/`[dpblit]` (env `FREESCI_PRIPROBE=1`) | `sci_resmgr.c`, `kgraphics.c`, `pico_driver.c`, `operations.c`, `gfx_support.c` |
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

### Sound on Pico (TODO)
Sound is currently disabled (`--no-sound` in `pico_main.c`). To enable:
- Wire FreeSCI's OPL2 softsynth (fmopl.c) output into a PCM callback feeding `pwm_synth`
- Add a Pico PCM device driver under `src/sfx/pcm_device/pico_pwm.c`
- Remove `--no-sound` from `pico_main.c`'s argv

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

- **OPEN but PARKED (stable) — clone-`variables` leak (`kscripts.c:212`, the dominant ~15-block/restore
  leaker).** `kClone` `sci_malloc`s `clone_obj->variables` separately from the clone table slot.
  `kDisposeClone` only flags `OBJECT_FLAG_FREED`; `run_gc()` reclaims the **table slot** but (apparently)
  not the `variables` block, so orphans accumulate through play/restores and are swept only by the final
  `sm_destroy` at quit (the session ends clean: `used` 274 K → **17 K** at the chooser — no orphan
  survives process teardown). **Left unfixed per user (2026-06-05): stable.** Each orphan is ~48 B and GC
  cadence keeps the heap under the ceiling for normal sessions. If a future session OOMs the control
  decode again after heavy cloning/restores, fix here: free `clone->variables` when GC reclaims a
  `FREED` clone (check the GC clone-reclaim path vs `sm_destroy`'s `_sm_deallocate` MEM_OBJ_CLONES case).

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

### OPEN — top priority — heap corruption surfacing as a GC fault ("aspb")

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

### Static-buffer SRAM recovery (DONE — `.bss` → lazy malloc / link-discard) — ~28 KB, now tapped out

A link-time static array reserves `.bss` permanently — it lowers the `mallinfo` arena ceiling
whether or not the feature ever runs. Converting the array to a `static T *p = NULL` pointer that is
`sci_malloc`'d on first use costs **zero** SRAM until the code path actually fires, and on Pico that
path never fires for the buffers below — so the recovery is pure. After this work the arena ceiling
rose **427,744 → 444,136** (+16.4 KB observed; ~25 KB nominal across the first three), plus a later
~2.8 KB from the bottom two rows (verified against the ELF `.bss`).

| Buffer | File | Size | Why free on Pico |
|--------|------|------|------------------|
| `tokens[0x1004]` + `stak[0x1014]` | `decompress01.c` (alloc in `decryptinit3`) | ~20.5 KB | SCI0/SQ3 routes through `decompress0` (own decrypt1/decrypt2); the shared decrypt3 LZW scratch is never touched |
| `said_tree[500]` + `said_tokens[128]` | `said.c` / `said.y` (alloc in `said()` under `if (s->parser_valid)`) | ~4.5 KB | `said()` only builds its tree when `parser_valid`, which needs a loaded vocab; Pico disables vocab → always 0 → dead |
| `bank` + `channels` (in `amiga.c`) | `softseq/amiga.c`, ref in `softsequencers.c` | ~1.5 KB | PicoCalc has no Amiga audio. `&sfx_softseq_amiga` is `#ifndef HAVE_PICO`-guarded; `scisoftseq` is a STATIC lib so the linker discards `amiga.o` entirely (`.bss` **and** flash) once unreferenced — no CMake change needed |
| `input[1024]` + `inputbuf[256]` | `main.c` `get_gets_input` / `scriptdebug.c` `_debug_get_input_default` | ~1.3 KB | Interactive debug console reads `stdin` via `fgets`; Pico has no stdin so neither runs. Lazy `sci_malloc` on first call → 0 `.bss`, 0 heap on Pico |

- **adlib/`fmopl.c` — nothing to recover.** The big synth tables are *already* lazy `static int *`
  pointers (NULL under NOSOUND, malloc'd in `OPLBuildTables`, `fmopl.c:610-627`); only ~650 B of tiny
  lookup tables remain in `.bss`. (This corrected the stale "~34 KB `ENV_CURVE` in `.bss`" claim.)
- **Both conversions preserve the capability** — lazy malloc ≠ deletion. If vocab/parser is
  re-enabled (roadmap #1) or SCI01/SCI1 games are run, the buffers allocate on demand exactly as before.
- **`said.y` was edited in lockstep with the generated `said.c`** so a future bison regen won't clobber
  the change.
- **The amiga link-discard is the cleanest pattern** for dead synths: guard the registration-array
  reference under `#ifndef HAVE_PICO` and the static lib drops the whole object. The remaining sound
  `.bss` (`adlib_sbi` 1152, `sci_adlib_vol_tables` 1024, `adlib_reg_L/R` 512, `KSL_TABLE`/`SL_TABLE`
  ~576, all from `opl2.c`/`adlib.c`/`fmopl.c`) is **deliberately kept** — that is the planned PWM-Adlib
  path (roadmap #3), so reclaiming it now just gets re-spent when sound lands.
- **Static mining is now tapped out** (~28 KB total). The ~35 KB/revisit accumulation that was the
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
   `pico_main.c` passes `-q` → `SFX_STATE_FLAG_NOSOUND`. PWM output already runs (`pwm_synth_init(26)`
   at boot: 8-bit mono, 22 kHz PWM on GPIO 26/27); what's missing is feeding *PCM* into it instead
   of the tiny_agi sine channels.
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
   - **Five deliverables:** (A) `src/sfx/pcm_device/pico_pwm.c` implementing `sfx_pcm_device_t` +
     ring buffer, downconverting the mixer's 16-bit samples to 8-bit; (B) rewrite `pwm_synth.c`'s
     IRQ to pop the PCM ring (also frees ~44 KB flash by dropping `pwm_strings.h`); (C)
     `src/sfx/timer/pico.c` using `add_repeating_timer_us(-16667,…)` (60 Hz) to replace POSIX
     `sigalrm.c`; (D) CMake wiring + register `pcm_driver_pico_pwm` behind `HAVE_PICO_PWM`, exclude
     `fluidsynth.c`; (E) drop `-q` from `pico_main.c` argv, pass `-m pico_pwm -p polled`. SQ3 ships
     `ADL.DRV` so its sound resources carry Adlib tracks — no extra resource handling needed.

Suggested order: 1 ∥ 2 ∥ 3 (all independent; pick by user-visible value vs. measured headroom).

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

### Known graphics limitations on Pico (not yet fixed)

These are correctness gaps in the Pico render path vs the SDL pipeline. Lower priority than the
roadmap above (gameplay works without them), but documented so they aren't rediscovered cold.

- **Per-pixel priority occlusion is wrong — "last drawn wins" instead of "highest priority wins."**
  `pico_blit_indexed` (`pico_driver.c`) writes the color unconditionally (gated only on the cel's
  `color_key`) and gates *only* the priority write on `row_pri[x] <= priority`. The SDL crossblit
  (`gfx_crossblit.c`) instead gates the **color** write on the priority test, so background priority
  occludes actors. Effect on Pico: the ego won't hide behind higher-priority scenery (e.g. walking
  behind a desk). Fix: gate `row_dst[x] = lut[idx]` on the same `pri_row[x] <= priority` test, after
  paging the priority rows in (they're resident in SRAM via the disowned priority buffer, so no
  PSRAM read needed unless that changes).
- **`static_priority_map` is aliased to `priority_map`** (`operations.c` `_gfxop_init_common`, under
  `HAVE_PICO`). On SDL these are distinct: the static one holds the pic's base priority and is copied
  back over the working map each frame to erase last frame's sprite priorities. Aliased, that copy
  (`gfx_copy_pixmap_box_i`) is a no-op, so sprite priorities accumulate and z-ordering degrades the
  longer you stand in a room. Proper fix: keep the static priority map PSRAM-resident and page the
  dirty rect back into an SRAM scratch on BACK-buffer update (same scratch the occlusion fix uses).

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
