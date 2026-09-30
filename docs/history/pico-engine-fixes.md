# Pico — engine correctness fixes and SCI-version limits

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 487-540 -->

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

<!-- from CLAUDE.md lines 629-634 -->

### RESOLVED — SQ3 plays on Pico (keyboard control + room-2 freeze)
SQ3 boots, reaches the first playable room, Roger walks, and gameplay runs without crashes.
The old "Roger won't walk / freeze on landing in room 2" was **never an input bug** — it was the
undersized VM value stack (`VM_STACK_SIZE 0x400`, see correctness fixes above). A second crash
(panic while walking / on text input) was the **clone-table OOM** fixed by `GC_INTERVAL 2048`.

<!-- from CLAUDE.md lines 3349-3384 -->

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


## Jones in the Fast Lane on desktop (2026-09-30)

Jones (VGA floppy, interpreter 1.000.060, detected as `SCI_VERSION_01_VGA`) now plays on desktop, keyboard only
(`--disable-mouse`, arrows + Enter), through character select, goals and the first turns. Three desktop/engine bugs:

- **Cursor save-under freed with `free()`** -- twice. `sdl_grab_pixmap` freed the pixmap's previous data even when it
  was an earlier grab's SDL surface pixels (SDL2 allocates those itself), and `_gfxop_grab_pixmap` did the same when
  a larger cursor needed a bigger save-under. SCI0 games have one cursor size, so only SCI1 hit it ("double free or
  corruption"). Both now release through the driver (`SDL_FreeSurface` / `unregister_pixmap`). The operations.c
  change is in the non-Pico branch; Pico keeps its own path.
- **`kEditControl` redrew non-edit controls** in whatever port was current. Sierra's interpreter only processes edit
  controls there (ScummVM agrees). Jones' menu loop restores the saved port (port 0) before calling it on its icon
  buttons, so arrow-key navigation drew a second set at window-relative coordinates on the full screen. Now icon/box/
  button/text are ignored; edit controls still redraw after a key. SQ3/PQ2 look + restore dialogs pixel-identical.

Found with the scratch key/screenshot harness plus an AddressSanitizer desktop build; the second free only showed
on a real window (user's ASan run), not under the dummy video driver. Pico status is unchanged: VGA games still stop
at the `gfxop_new_pic` guard. Panel text drawn as dark bars: fixed -- `get_pic_color` now uses the current picture's
palette for VGA indices (was the static palette 999; colour 99 = dark grey there, panel blue in the picture).

**Walking character hidden in week 1 (2026-09-30).** Two layering bugs:
- `_gfxwop_container_draw_contents` (`widgets.c`) looped dirty rectangles outside and children inside. A child
  container (the cast list) draws all its dirty areas on its first visit, so in every later rectangle the widgets
  before it were painted over it again (trace: panel in rect 1, walker in rects 1-4, panel again in rects 2-4). Now
  each child is drawn in all dirty rectangles before the next -- painter's order. **Shared with the Pico** (every
  game); desktop SQ3/KQ4/PQ2/CB screenshots are identical apart from animation timing; device test pending.
- Jones paints the centre panel with `DrawCel` in port 0, which in FreeSCI is a layer (`wm_port`) above the picture
  port and its cast. For VGA games only, `add_painted_widget` (`kgraphics.c`) puts `DrawCel` and `Graph` line/box
  painting from port 0 into the picture port (cast kept last), unless it overlaps an open window (the shop's speech
  bubble is painted over the shop window). `Display` text stays in port 0: it is painted over stopped actors, which
  FreeSCI redraws every frame (the money over the calculator vanished otherwise).
