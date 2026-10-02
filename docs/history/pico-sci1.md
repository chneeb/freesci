# SCI01 VGA on the PicoCalc (branch `pico-sci1`)

Goal: Jones in the Fast Lane (VGA floppy, interpreter 1.000.060, `SCI_VERSION_01_VGA`) on the **PIO** PicoCalc
build. Desktop plays it since 2026-09-30 (see `pico-engine-fixes.md`). The Pimoroni target would be easier but
needs the board swapped into the case, so the PIO build comes first.

## Why it can fit (2026-09-30 estimate)

- Script/object/locals footprint (desktop probe) is at or below the SCI0 games that already fit: Jones worst room
  39 KB scripts + 15 KB object vars + 3 KB locals, vs KQ4 42-46 KB scripts, SQ3 40-50 KB, PQ2 45-51 KB.
- No text parser ("Assuming that this game does not use a parser"): none of the parser's SRAM (resident vocab,
  the ~45 KB GNF build peak per command).
- The problem is resource SIZE, not count: largest pic 55,611 B decompressed (54,871 compressed, method 1), largest
  view 34,693 B, largest sound 46,715 B (method 2). The SCI0 games top out at ~12 KB pics and 13-21 KB views, which
  is what the 16 KB decompress scratch was sized for. KQ4's measured contiguous margin in play is ~41 KB.
- Resource methods in Jones: pics are all method 1 (Huffman); everything else is method 0 or 2 (LZW, `decrypt3`).
  No reorder methods (3/4).

## Step 1 -- `visdiff` decodes VGA pictures as the device does

`tests/picodiff/visdiff` hardcoded version 0 and no static palette, so a first run "agreed" on Jones by decoding its
VGA pictures as SCI0 on both sides (blank pictures). Now: a game with a palette 999 resource is treated as
`SCI_VERSION_01_VGA` (the resource map alone says SCI0 for Jones -- the game learns version 3 from the interpreter
hash in main.c), the static palette is passed, the EGA dither is skipped, the priority map is diffed, and the Pico
side decodes with `resource == NULL` through the PSRAM stream cache exactly as on the device (one pic per child
process; `RAWPTR=1` passes the raw pointer; `DUMP=dir` writes PPMs; all-blank runs fail).

Found at once: the VGA `SET_PALETTE` opcode and the equidistant priority table read the resource pointer directly
(NULL on the Pico) -- every Jones picture would have faulted. Both page through PSRAM now. Harness note: the decoder's
`sci1` argument must be 0 for SCI0 pictures (it is a flag, not a version); passing `SCI_VERSION_0` (=1) crashed the
desktop reference.

## Step 2 -- picture memory

A VGA picture is mostly one embedded bitmap (pic 11, the town board: first cel 33,259 B). The existing Pico branch
malloc'd the whole cel, decoded it into a full-size pixmap and translated that -- three large SRAM blocks -- to copy
rows into the visual map. `_pico_draw_embedded_cel1` (`sci_pic_0.c`) decodes the RLE stream straight from PSRAM into
`visual_map->index_data`, reproducing the desktop blit exactly (stride 320, no clipping, zeroed cel, the "titlebar
hack"). Overlaid pictures keep the buffered path: desktop's `view_transparentize` indexes the picture with the view's
width, which a direct decode cannot reproduce.

The load: VGA resources go through `decompress01`, which had none of decompress0's Pico treatment (flat input
buffer + flat output, ~110 KB for the board) -- and a latent bug: with `PICO_STREAM_METHODS` bit 2 compiled in,
`decrypt2` takes a *stream descriptor*, but decompress01's method 1 passed it a plain buffer, so every SCI01 Huffman
resource would have decoded garbage on the device. Now:
- `pico_stream_decompress01` (`decompress0.c`) runs SCI01 methods 0 and 1 through the 4 KB window (SCI01 method 1 is
  the same Huffman coder as SCI0 method 2).
- `pico_decompress_alloc` is shared and gains a one-load lent buffer (`g_pico_decompress_borrow`):
  `gfxr_interpreter_calculate_pic` lends the resident `visual[0]` (already borrowed as the decode buffer) while a
  VGA pic loads; the data goes to PSRAM and is evicted, then the loan ends and the decode reuses the buffer. Every
  res->data free site treats the lent buffer like the scratch.
- On the Pico, VGA pics take the Pico (PSRAM) branch of `calculate_pic` with the version passed to the decoder.
- `gfxop_new_pic` lets `SCI_VERSION_01_VGA` through; later versions still halt legibly.

Verified offline: `decompdiff` gained an SCI01 mode (stock `decompress01` vs the Pico stream, bytes + return code +
fd position): Jones 266 resources, 0 differ, and the SCI0 corpus unchanged (3,471 resources across 5 games).
`visdiff`: all 7 Jones pictures identical through the direct decoder, SCI0 games 0 differ. PIO `.bss` 30,840 (+8:
the lent-buffer pointer and size).

Not done yet: method 2 (`decrypt3`) still reads a flat input (Jones: 240 resources, up to 27.5 KB), the palette is
still the EGA mapping on the LCD (step 3), VGA views have no PSRAM offload (step 4). Nothing flashed yet.

## Step 3 -- 256-colour palette on the LCD

The driver already works in 256 palette slots (`palette[256][3]`, `pal565` for the 16-bit LCD path) and `visual[0]`
holds slot numbers; SCI0 fills the slots from the 256 dithered EGA combinations (`pico_setup_sci0_palette`). For VGA
games `gfxop_new_pic` now calls `pico_setup_vga_palette` with the picture's own 256 colours (its `SET_PALETTE`
opcode; `gfxr_read_pal1` always returns all 256 entries), or the static palette 999 when it sets none -- what
desktop draws the picture with. Nothing else needed: `pico_blit_indexed` already uses the identity LUT for
256-colour pixmaps (VGA cels index the system palette, as in Sierra's interpreter), resolves text/window colours to
the nearest slot, and takes transparency from each pixmap's own `color_key` (the VGA cel header). `kPalette`
subfunction 3 (Jones calls it) is unimplemented on desktop too. The picture colours are the ones `visdiff DUMP`
renders (same pic-palette mapping), which match desktop. `.bss` unchanged (30,840).

Known risk before a device run: VGA views already get the Pico's cel-to-PSRAM offload (it runs after the version
switch), but a view is decoded completely first -- all cels in SRAM at once, plus the decompressed resource, plus
decrypt3's flat compressed input. For the largest Jones view (view.711: 27.5 KB compressed, 34.7 KB decompressed)
that is well past the heap margin; an OOM halts legibly. That is step 4.

## Step 4 -- views

Measured first (scratch tool over every Jones view, decoded with `gfxr_draw_view1`): 90 views, 752 cels, largest cel
20,496 B (50 cels > 16 KB, none > 32 KB), all cels of one view up to 85 KB (view 607), decompressed views up to
34,693 B (14 over 16 KB). So "decode the whole view in SRAM, then offload" could never fit.

- **4a -- LZW input streamed.** `gbits` reads strictly forward (<= 3 bytes past its bit position), so `decrypt3` now
  reads through decompress0.c's 4 KB window (`pico_stream_begin/_byte/_end`); `pico_decompress01_stream` handles
  methods 0-4 (reorder of 3/4 by the caller). That removes the last flat compressed-input buffer for Jones (views,
  scripts, sounds: up to 27.5 KB).
- **4b -- one cel at a time.** A cel of a real view (`view != NULL`; the pic decoder's embedded path passes NULL and
  keeps `index_data`) decodes into the idle 32 KB priority scratch and goes to PSRAM immediately, as `sci_view_0.c`
  already did for SCI0.
- **4c -- the view never in SRAM.** `decrypt3` writes its output strictly forward and keeps its state in statics, so
  it resumes across calls: `pico_decompress01_to_psram` decompresses a view straight into the PSRAM arena, staged
  through the idle 16 KB decompress scratch; `scir_pico_load_to_psram` (resource.c) opens the volume like the normal
  loader and returns -1 (normal load) for anything it does not handle. `gfxr_draw_view1_psram` runs the unchanged
  decoder with a NULL base pointer, every read going through a 512-byte cache (`VRB/VR16/VRU16/VRCOPY` macros, direct
  reads on desktop). PIO only (`PICO_STREAM_DECOMPRESS && !PICO_PSRAM_MAPPED`): the mapped target's heap is PSRAM.

Verified offline with a new harness, `tests/viewdiff` (desktop stock path vs the Pico path over the PSRAM stub): all 90
Jones views -- decompressed bytes, palette values, and per cel the geometry, hotspot, colour key and every pixel read
back from PSRAM: 0 mismatches. It needs a Pico-layout pixmap: `gfx_pixmap_t` has trailing psram fields under
HAVE_PICO (and `resource_t` is packed differently), so only `pico_view1.o` and a tiny `pico_pixmap.c` are HAVE_PICO
compiles, and `-Wl,--wrap=gfx_new_pixmap` grows each desktop-allocated pixmap to the Pico layout. visdiff and
decompdiff re-run: unchanged (0 differ across all five games).

`.bss`: PIO 31,396 (+556: the 512-byte view cache and the second stream descriptor); Pimoroni unchanged at 29,624 (the
PSRAM view path is not compiled there).

## First device run (2026-10-01) and fixes

Jones started and the intro played in the right colours (picture palettes). Two problems in play:

**Wrong colours, e.g. the player-number buttons white.** Every Jones view has its own palette (90 of 90, ~134 used
entries each; a third differ from the town board's -- views 1/2: 48/39). Sierra's SCI1 interpreter inserts a view's
USED entries (byte 0 of each 4-byte entry) into the system palette when it draws the view (ScummVM's "insert" for
early SCI1); the Pico only had the picture's palette, so a cel index that is white in the board's palette came out
white. Desktop never showed it: it is true-colour and draws every pixmap through its own palette. Fix: a VGA view keeps
only its used entries as a compact `gfx_pal_insert_t` (`view->pico_pal`, ~0.5 KB instead of a 2 KB colour table;
`colors` points at the shared static palette), each cel carries a pointer to it (`pico_pal_insert`), and
`pico_blit_indexed` writes the entries into the palette slots before drawing the cel.

**OOM twice** (captured: `calloc 2060` in `new_reg_t_hash_map`, free 8,496 B of 477 KB -- exhaustion, not
fragmentation). Decoded views stay cached until the next picture, and Jones keeps the town board up all game. Desktop
count: 15 live views after a short session, ~74 KB at 2 KB palette + 132 B per Pico cel struct, heading for 250-280
KB with all 90. Fix: `GFXR_VIEW_BUDGET` (CMake `PICO_VGA_VIEW_BUDGET`, default 49152, PIO only) -- after decoding a
VGA view, `gfxr_get_view` frees the least recently used cached views while the estimated total is above the budget;
never the view just requested nor the one holding the mouse-pointer cel (the only cel pixmap anything keeps across
calls; `g_gfxr_pointer_pixmap`). A re-decoded view puts its cels back where they were in PSRAM (deterministic decode;
a 64-entry heap table of base/size per view, valid while `psram_epoch()` is unchanged), so eviction does not grow the
arena. The budget is a guess until a probe build measures the real headroom.

Found while there: the PSRAM arena had no upper bound and is not rewound while a picture stays up, and step 4c put
each view's decompressed copy in it. The copy now goes to a fixed 64 KB staging slot (`PSRAM_VIEW_STAGE_ADDR`
0x7A0000, above the working priority map), and `psram_alloc` halts legibly ("PSRAM arena full") before the parse
scratch at 0x700000 instead of overwriting the song slots. The duplicate "pic->internal is not NULL; possible memory
corruption" per VGA picture was the control pass rebuilding the priority-band table -- it is dropped before that pass
now. The `gfx_free_color` "unused color index" errors are the Pico's 8-bit mode palette refcounting a 256-colour
picture with duplicate RGB values; no memory involved, same path as SCI0, left alone.

Offline: viewdiff now passes the static palette and checks every insert list against the desktop palette's used
entries (90/90), that each cel carries its view's list, and re-decodes each view into its first PSRAM location (90/90
same addresses and bytes). The PSRAM stub allocator rounded to 8 bytes, unlike the device -- fixed, since the
re-decode relies on the real bump pointer. Eviction stress: desktop ASan build with a budget of 8000 B, 3.7 min of
Jones: 50,667 evictions, 0 ASan errors, all screens correct; SQ3/CB/KQ4/PQ2 saves under the same build: 0 errors, 0
evictions. visdiff/decompdiff unchanged. `.bss` PIO 31,424, Pimoroni 29,624.

## Second device run (2026-10-01): HardFault, then SCI1 songs into PSRAM

**HardFault** at `pico_blit_indexed` (`pico_driver.c:706`, BFAR 0x70000000): the palette-insert loop followed a
pixmap's `pico_pal_insert` (offset 88 of the 92-byte ARM `gfx_pixmap_t`) to garbage. Every pixmap is zeroed by
`gfx_new_pixmap` and every object was rebuilt after the field was added, so this is a real overwrite. The blit now
range-checks the pointer (SRAM heap, even, n <= 256), logs the pixmap once (`[pal] bad insert ptr ...`) and draws
without the insert. The next run logged it: a TEXT pixmap (ID ffffffff, 245x12, 3 colours, font flags) whose last
byte was 0x70 -- looks like a one-byte overrun of a palette colour component by Pico-only code (desktop ASan never saw
it). Still open.

**OOM again** (`malloc 10672` for the largest script, 55 KB free but fragmented; 12/136/4332-byte failures earlier,
during music). PSRAM songs (`PICO_PSRAM_SONGS`) only parked SCI0 songs; SCI1 songs (Jones: up to 46.7 KB) were full
SRAM copies. Now `songit_new` parks SCI1 songs above `PICO_PSRAM_SONG_MIN` too, unless a track list carries a digital
sample (`_sci1_song_has_samples`: a 0xfe track; `_sci1_get_pcm` hands the sample's address to the mixer). SCI1's
direct reads (`SONGDATA`, `SCI1_CHANDATA`, track table, dump) go through `PSONG_BYTE`; the SCI1 clone takes a slot
reference like SCI0's; cleanup already went through `_sci0_cleanup`. SCI1 reads up to 16 tracks at interleaved
offsets, so a parked SCI1 song splits the 64-byte window into 4 x 16-byte ways (round-robin); SCI0 keeps its single
window.

New harness `tests/songdiff`: every song through the stock iterator and through a HAVE_PICO + PICO_PSRAM_SONGS copy
of iterator.c (stub PSRAM slots), 8 device play masks each, event streams compared step by step. Jones: 34 songs, 272
runs, 57,667 events, sounds 6 (46,715 B) and 100 (18,164 B) parked, 0 mismatches. SQ3/KQ4/PQ2/CB through the SCI0
iterator (their existing PSRAM path): 1.36 M events, 0 mismatches; all slots released afterwards.

Also from that run: the centre panel is where desktop puts it (black edge row 44, white bar 45-54, blue from 55).
The real glitch: the bottom-row signs ("EMPLOYMENT OFFICE", "HI-TECH U") drawn twice ~10 px apart, covering the lower
"1" boxes -- a Pico redraw path restoring background with a title-bar offset. Open.

## Third and fourth device runs: the census at the first failure

Run 3 (SCI1 songs in PSRAM) played much longer, then ran out completely (16 B free). The census build
(`build-pico-sci1-census`, `-DFSCI_PROBE_MEM_CENSUS=ON`) twice ended in the SCI console instead of an OOM halt: a
script's data allocation failed and returned NULL (a raw `malloc` path), the script came up empty ("does not have a
dispatch table") and the next `send` hit the console. So the census now also dumps at the FIRST failed allocation of
any kind (`census_first_failure` in `pico_mem_census.c`, census builds only).

Board start (room 11 ready): 129-130 KB free, largest 116 KB. At the first failure: 2,680 B free, 436 KB live (+125 KB).
Growth by site: view cel pixmap structs `gfx_tools.c:200` +29.6 KB (329 cels), scripts `seg_manager.c:242` +20.6 KB,
LZW tables `decompress01.c:83/84` +20.5 KB (permanent since the first SCI01 LZW decompression), cached resource data
`decompress0.c:82` 15 KB (within the resource manager's 32 KB LRU), int hash maps +13.5 KB (one 256-bucket map, 1 KB
of pointers, per loaded script), VM tables +5 KB.

Fixes:
- The LZW tables live in the 32 KB priority scratch once it exists: it is busy only during a pic decode (after the
  pic's resource loaded) and a view's per-cel decode (after the view decompressed), and no LZW decompression runs
  inside either. Before the first pic (scripts at game start) they are a temporary allocation released after each
  decompression (`decrypt3_release`). -20.5 KB. viewdiff now uses a HAVE_PICO copy of decompress01.c, so its LZW tables
  sit in the same scratch the cel decode borrows: 90/90 views unchanged.
- View budget 48 -> 32 KB (`PICO_VGA_VIEW_BUDGET`; existing build dirs need `-DPICO_VGA_VIEW_BUDGET=32768`).
- Tried and reverted: int hash maps with 32 buckets on the Pico (-~23 KB). Savegames serialise every map's bucket array
  with the compile-time count (`_cfsml_write/read_int_hash_map_t`), so a save from desktop or an older build (256) would
  be read past a 32-bucket struct on restore. Would need a rehash on load.

## Fifth run: fragmentation -- scripts stop taking heap blocks

One full round of normal play, then the opponent's turn failed: `malloc 6628` in `pico_decompress_alloc` with 48 KB free
in total -- fragmentation this time (earlier `calloc 2060` and two `malloc 4332` had failed too). The failing block is
the transient copy of a resource the generic path allocates, a script: Jones' scripts reach 10.7 KB and the opponent's
turn loads new ones.

A script's resource is copied into its segment on instantiation (`sm_mcpy_in_out` in `script_instantiate_sci0`), and
every consumer (instantiation, the class-table scan, the version scan, savegame restore, `sm_set_script_size`) reads it
right after loading it, with no other load in between. So in `decompress01` an SCI01 script that fits is decompressed
into the 16 KB decompress scratch and becomes its OWNER (`g_pico_scratch_owner`); the next user of the scratch -- a
small pic/view decompress (`pico_decompress_alloc`) or the PSRAM view staging (`scir_pico_load_to_psram`) -- calls
`pico_scratch_take`, which evicts the owner (`scir_evict_resource_data`, installed by the resource manager) if it still
points there; its next access reloads it. All resource-data free sites already skip the scratch. Text resources are NOT
included: `kFormat` can look up a second text while still using the first. SCI0 games are untouched (decompress0 never
hands a script the scratch; with no owner, `pico_scratch_take` is a no-op).

New harness `tests/resdiff`: the Pico resource manager itself (resource.c, resource_map.c, resource_patch.c and the
decompressors, all HAVE_PICO, no renaming needed) on the desktop. Jones: every script recorded, then 3 scrambled rounds
interleaving pic, view and PSRAM-view loads: 414 script reads, 97 real takeovers of the scratch, 97 evictions of the
owning script, 0 mismatches. `.bss` PIO 31,440.

## Sixth run: the script came up as garbage -- the cache flush evicted it mid-copy

Jones played much longer (1.2 M VM steps vs 196 K), then "Script 0xd2 does not have a dispatch table" and the SCI
console. The `malloc 3974 failed` just before was NOT the failure: `sci_malloc` (`pico_sram_alloc`) runs
`pico_reclaim_heap` and retries, and the retry succeeded. But the reclaim flushes the resource cache
(`scir_free_all_lru`), and the script being instantiated was in it -- in the decompress scratch, as its owner. The
flush set its `data` to NULL (freeing nothing), the segment allocation's retry succeeded, and `script_instantiate`
copied from NULL: garbage. `scir_free_all_lru` now leaves entries in the scratch enqueued (they free nothing; the next
user of the scratch evicts them). `_scir_free_old_resources` (trim after a load) is unchanged: it counts scratch
entries in `memory_lru`, so skipping them would need the accounting changed, and during instantiation the only load is
the script itself, protected as the newest entry.

resdiff now also flushes the cache right after each script load and checks the bytes: with the previous resource.c
207/207 reads lost their data (the device bug, reproduced offline); now 0/207.

## Board 10 rows too high -- the "titlebar hack" (desktop too)

User report: the marble, the clock and the centre panel's content sat too low on the town board. Desktop FreeSCI showed
the same, so the earlier comparison against desktop proved nothing. Cause: pic 11 opens with a 319x199 embedded cel at
row 0; 199 + the 10-row titlebar > 200, and `gfxr_draw_pic01`'s "titlebar hack" then set the titlebar offset to 0 for
the REST of the pic -- the whole board (visual, priority, control) drew 10 rows higher than the picture port the views
are placed in. Sierra's interpreter (as ScummVM implements it) draws at the port top and clips at the screen bottom. The
hack is replaced by clipping (`embedded_rows` in the buffered path; the Pico direct decoder already drops writes past
the buffer). It never fired in SQ3/KQ4/PQ2/CB (checked over all their pics), so only such pics change. visdiff: Pico ==
desktop on all five games. The duplicate bottom-row signs on the device were very likely the same 10-row offset.

## Seventh run: exhaustion after the titlebar fix -- the view arena

With the titlebar fix Jones played longer, then `reg_t_hash_map_check_value` failed on `malloc 12` with 16 bytes
free: the heap was used up, not fragmented (an earlier run with older firmware had `sm_initialise_script` fail on 6,840
bytes with 42 KB free, fragmentation). The view cache is the big churner on a game that keeps one picture up all game:
every decode and every budget eviction is a few hundred small blocks (`gfxr_view_t`, loops, cel pointer arrays,
92-byte pixmaps, the palette insert list) scattered through the heap between long-lived script and VM blocks.

Per-view metadata on ARM (sizes + 8-byte malloc headers, measured with a scratch tool): view 501 3,010 B, 340 4,894,
609 5,622, 751 6,074, 270 6,606, and view 0 (13 loops, 145 cels) 15,878 B.

**The view arena** (`src/platform/pico/view_arena.{c,h}`, PIO only, `PICO_VIEW_ARENA` = `PICO_VGA_VIEW_BUDGET`, 32 KB):
one SRAM block, allocated on the first VGA view while the heap is still clean (SCI0 games never allocate it), with a
first-fit allocator inside (8-byte headers, lazy coalescing). While a view is decoded (`g_view_arena_active`, set
around `gfxr_draw_view1_psram`/`gfxr_draw_view1` in sci_resmgr.c and the `gfx_resource_t` allocation in resmgr.c)
`__wrap_malloc`/`__wrap_calloc` serve from the arena first and fall back to the heap when nothing fits;
`__wrap_free`/`__wrap_realloc` route by ownership like the mapped target's PSRAM heap; the PIO link also wraps
`malloc_usable_size` so `g_sci_live_bytes` and the census keep working on arena blocks. The budget eviction
(`gfxr_enforce_view_budget`) now also evicts LRU views until the arena has 16 KB contiguous (`GFXR_VIEW_ARENA_HEADROOM`,
the largest view) for the next decode. The census build prints the arena state at the first failed allocation.
`.bss` PIO 31,440 -> 31,456; Pimoroni unchanged (29,648).

Offline:
- `tests/viewarena` -- randomised stress against a reference model (5 seeds, ~1.1 M operations each, under
  ASan/UBSan): payload integrity, alignment, byte accounting, `largest` vs the recomputed largest free run, "an
  allocation fails only if it does not fit", and a view-shaped workload (groups of 20-320 blocks freed LRU with the
  16 KB headroom rule). Three mutated allocators (overlapping split, too-small remainder, lost coalescing) all fail it.
  Under ASan the arena poisons free payloads, so a use-after-free inside it is still reported.
- `viewdiff` routed through a 32 KB arena (scratch shim wrapping malloc/free and `gfxr_draw_view1_psram`): 90 views,
  752 cels, both decodes of each view, 0 mismatches, chain intact. (viewdiff needed two stubs for the script-scratch
  ownership symbols, which live in decompress0.c's HAVE_PICO part.)
- Desktop Jones under ASan with the same routing (shim; `GFXR_VIEW_BUDGET=8000`, a 20 KB arena so it runs full),
  the eviction key script for 225 s: 1.59 M arena allocations and frees, 0 ASan errors, chain checked on every free.

**Headroom rule removed (same day).** The first census run with the arena: Jones moved very slowly, and the faces
were sometimes garbled with random colours. The eviction that kept 16 KB free in the arena thrashed: view 0 alone
is 15.9 KB, so with it cached almost any second view pushed the free space under 16 KB and evicted a view the next
frame re-decoded from SD. Eviction is back to the plain 32 KB budget (as before the arena); a view that does not fit
the arena goes to the heap. No code path keeps a cel pointer across another `gfxr_get_view` (only the mouse
pointer, which is never evicted) and the census writes no tags into blocks, so the garbled faces are suspected to
be a side effect of the thrashing -- unconfirmed, to re-check on the next run.

## Eighth run: the GC's maps -- compact hash maps (PIO)

With the eviction fix Jones ran much longer, then during Jones' turn the GC's second temporary map failed
(`new_reg_t_hash_map`, calloc 2,060 with 1,872 free; the earlier `malloc 12` failures were its nodes, absorbed by the
reclaim-and-retry). Pure exhaustion: the census at the previous failure had one 144-byte gap. The steady state on
the board leaves too little room for the GC's peak.

`SCI_COMPACT_HASHMAPS` (PIO only, CMake, next to the view budget):
- **int hash maps 256 -> 32 buckets.** Every loaded script has one (its object indices) plus the segment manager's
  script-number map; 1 KB of buckets each. The test saves held 16-23 such maps (SQ3 16, KQ4 18, PQ2 19, CB 21, Jones
  23), so ~14-21 KB of heap back in every game. The memory hash `(x ^ x >> 3) & 31` is a function of `x & 0xff`
  alone, so the savegame FORMAT is unchanged: the writer gathers each of the 256 format chains out of its one memory
  bucket (no allocation) and writes it, the reader appends each chain to its memory bucket (buckets zeroed first --
  the reader mallocs the map and the stock code relied on reading all 256 slots). Hand edits in the generated
  savegame.c, noted in savegame.cfsml.
- **reg_t (GC) maps 512 -> 128 buckets**, never saved; the failing allocation shrinks 2,060 -> 524 B.
- Both keep the original shape `hash 0..N-1` in `N+1` buckets: `apply_to_*_hash_map` loops `i < HASH_MAX` and would
  silently skip a used last bucket -- for the GC that would free live objects.

Offline (desktop builds with and without `-DSCI_COMPACT_HASHMAPS=1`, ASan, scratch hooks for restore->save and a
mid-game save):
- First-generation re-saves of the SQ3, KQ4, PQ2 and CB savegames and a mid-game Jones save: compact == stock, byte
  for byte. Second generation (256->32, 32->256, 32->32) == stock re-saving its own save; stock itself changes song
  handles on a re-save, so that is the right baseline.
- Restore + play on with the compact build: all four SCI0 games back in their rooms and answering `look`; Jones from
  the start into week 2; 0 ASan errors.
- Found on the way, NOT caused by this: on desktop an in-game restart with a "tee" song iterator active hits the
  debug breakpoint in `songit_tee_death_notification` (iterator.c:1894, "Missed breakpoint") and the process dies.
  The stock build does it on a forced restart (a scratch hook calling `kRestartGame` at 70 s). Not investigated.
`.bss` PIO unchanged (31,456); Pimoroni and desktop untouched.

## Ninth run: Jones completes its round -- and two colour bugs

No OOM with the compact maps: Jones finished its turn and the player's next one started. Two glitches, and an SQ3
regression from the same build:

- **Uninitialised `pico_pal_insert` (all games).** `gfx_new_pixmap` never set the field, so text and other non-view
  pixmaps carried malloc leftovers, and `pico_blit_indexed` wrote them into the LCD palette whenever they looked like
  an SRAM pointer. The `[pal] bad insert ptr` lines (Jones: a text pixmap; SQ3: the 320x200 background) and the
  earlier HardFault with BFAR 0x70000000 were the cases the guard caught; the rest silently recoloured the screen.
  The compact hash maps moved the heap layout and so the leftovers: SQ3's quit dialog came up with a light brown
  text background. Now NULL in `gfx_new_pixmap`; this also closes the "text pixmap one-byte overwrite" open item --
  there was no overwrite.
- **VGA menu black on black (desktop too).** For VGA games the title-bar port got two uninitialised stack colours
  (the static-palette setup in `game.c` was `#if 0`), and `ega_colors[16]` -- which the interpreter's own UI uses --
  was never filled. `_sci1_alloc_ega_colors` fills them for VGA (0 black, 15 white = SCI1's system 0 and 255, the
  rest nearest in the static palette) and the title bar uses them for every version. Entry 255 of Jones' palette 999
  is not white, so the Pico driver now forces LCD entry 255 to white for VGA pictures, as SCI1 does (get_pic_color
  already returned white for 255). Desktop: menu black on white, selection inverted.
- Jones' clock red during his turn: not explained yet -- either the garbage inserts above or the view-palette
  conflict (the merge work). Re-check with this build.

## Tenth run: menu fixed; text backgrounds and the clock -- the colour allocator

Menu black on white and the SQ3 dialog fixed on the device. Still wrong in Jones: dialog text backgrounds, and the
clock stays red for Jones' whole turn. Two allocator problems, both palette-mode only (desktop draws by RGB):

- **The engine allocated over the picture's colours.** `gfx_alloc_color` (from `gfxop_set_color` and every text
  pixmap install) hands out the first unlocked entry of the driver's mode palette and writes it through
  `pico_set_palette`. For a VGA game only 0 and 255 were locked (the system colours), so allocations took 1, 2, 3...
  -- the board's own colours. `pico_setup_vga_palette` now locks all 256 entries as system colours with the
  picture's RGB, so an allocation always matches an existing entry, as in Sierra's interpreter where every entry
  belongs to the picture.
- **VGA kernel colours had no index.** `get_pic_color` hands out the picture's (or static) palette entries, whose
  `global_index` is UNMAPPED; a Pico fill by such a colour (`pico_map_color`) drew index 255. The entry now carries
  its own index (a VGA colour IS a palette index); `white` is index 255. This also makes the allocator see these
  colours as already mapped.
Desktop Jones (shop dialogs): unchanged, 0 ASan errors. NB on desktop the clock shows a red wedge for the hours
used, so a red clock may be partly the game's own display -- check whether it fills over the turn.

## Eleventh run: the clock stays red -- Sierra's save-unders of stopped views (engine, desktop too)

Device: dialogs and speech bubbles correct now; the clock is red from the start of Jones' turn, and from week 2 on for
the player too, never refreshing. Desktop showed the same, so this was traced there (scratch probes, none kept):

- The clock is one stopped view, `timeKeep` (view 270, signal 0x5914: frozen, no update, fixed priority). Its cel
  shows the hours used; the cels are cumulative wedges, 0/0 fully transparent, 6/0 mostly red. Every hour the script
  ALSO `DrawCel`s the same cel at the same place.
- FreeSCI keeps every DrawCel as a widget and repaints it every frame, so at a new week `timeKeep` correctly went back
  to 0/0 but the week's DrawCel widgets (up to 6/0) were still painted over it.
- In Sierra's Animate (as ScummVM implements it, from memory: GfxAnimate::update) a no-update view keeps the screen
  bits under it; when the update subalgorithm runs on a picture that was not just drawn, those bits are restored --
  erasing everything painted over the view since -- and saved again before it is redrawn. That restore is what wipes
  the stamps. FreeSCI had no save-unders for dynamic views at all (`_k_redraw_view_list` only cleared flags).
- FreeSCI's `pic_not_valid` doubles as the counter `_k_prepare_view_list` bumps per view needing an update; Sierra
  keeps the real flag apart. At the week change the real flag was 0 (+1 from timeKeep's force update): Sierra restores.

Fix (kgraphics.c, all games): `_k_redraw_view_list` restores a no-update view's save-under unless the REAL flag
(captured before `_k_prepare_view_list`) is 1, and records a new one for every shown no-update view after the flag
pass. In widget terms a save-under is a snapshot of the view's rectangle and the restore a snapshot restore: widgets
created inside it since then are freed (dynamic views and windows are immune). Records are per object in a 32-slot
table, dropped on DrawPic and on a state change (restore). `.bss` +260 on all targets (PIO 31,716).

Desktop checks: Jones into week 2 -- the clock resets and shows only the new wedge. Regression: SQ3, KQ4, PQ2 and CB
restored and played with the same keys against a baseline binary, screenshots every 10 s: SQ3 and PQ2 pixel-identical;
KQ4 and CB differ only where the walking character stopped (timing; two runs of the same binary differ the same way);
0 ASan errors. NB CB's "sticky kDrawCel corners" may be this mechanism too -- re-check.

## Twelfth run: a Jones savegame restore -- the picture loaded just to be evicted

Normal build: Jones' week 2 ran out of heap in the GC's map nodes (12 B, 24 B free; a 4,230 B song had already fallen
back to silence). A periodic census checkpoint (every 2 minutes of play, census builds only) was added to find a slow
climb. The census run restored a week-2 savegame: the first Jones restore on the device. The restore itself worked
(checkpoints `restore-1` .. `restore-5-done`), then re-entering the board failed on `malloc 55611` with ~23 KB
contiguous: `gfxop_new_pic`'s prologue "evicts the old room's raw pic resource" with `scir_find_resource`, which LOADS
a resource that is not in memory -- after a restore the cache is empty and the old room is the same room, so the 55 KB
board was decompressed into a fresh heap block just to be freed, before `visual[0]` is lent as the decompress target.
Now `scir_test_resource` (look up, never load). Pico-only code; it also spares SCI0 games a pointless pic load after a
restore. The slow-climb question is still open: the census run did not get far enough for periodic checkpoints.
