# Pico — 4bpp visual buffer attempt (abandoned; branch pico-4bpp-packing)

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 4187-4583 -->

#### 4bpp visual buffer (D16 dithering) -- ATTEMPTED, REVERTED to a branch (2026-09-12)

> **OUTCOME: packing is REVERTED from master and preserved on branch `pico-4bpp-packing` (pushed to
> `fork`). KEPT on master: `PICO_DITHER_D16` (free, device-confirmed indistinguishable) and all three
> harnesses -- `tests/picodiff` (control), `tests/picodiff/visdiff` (visual), `tests/drvdiff` (driver +
> panel), plus `tests/d16check.c` and `tests/ditherpreview.c`. All three verified working against the
> reverted tree: 0/64000, 115 pics 0 differ, driver output identical.**
>
> **Why it was abandoned.** Not because packing is hard -- the decode half reached 111/115 pics byte-exact
> and the driver half produced byte-identical panel output, with the PQ2 pic rendering correctly on real
> hardware. It failed on two things:
> 1. **The 8bpp visual buffer was load-bearing TWICE.** It is the display buffer AND the control pass's
>    flood-fill aux, for free, precisely because it is 8bpp. Halving it does not halve the requirement -- the
>    aux still needs its own 64,000 bytes, a large contiguous transient of exactly the kind B-1/B-1.2/B-1.3
>    existed to remove. Net at the decode PEAK: worse, not better.
> 2. **Packing the aux to recover that did not converge.** The bit analysis says four bits suffice and the
>    accessors unit-test correct, but packed control maps stay wrong (SQ3 pic 3: 51,917/64,000) and each
>    conversion moved the number negligibly -- meaning the model of what the aux holds is wrong somewhere,
>    not merely incomplete. Best lead: `sci_pic_0.c:1912` reads `aux & 0xf` as a COLOUR, which the bit remap
>    would collide with.
>
> **Cost: three device cycles, two of them on faults I introduced** (an aux-overrun from a half-size buffer,
> and a use-after-free from omitting a `priority_is_scratch` guard the two existing free sites both had).
>
> **Techniques worth keeping regardless** -- these are the real return from the exercise:
> - **POISON THE FIELD** to enumerate every use: renaming a struct member so the compiler lists every access
>   found the aux line tracer, which aliases through a local (`buffer = pic->aux_map`) and never matched a
>   grep for `aux_map[`.
> - **CLASSIFY mismatches, don't just count them.** Asking "does this pixel equal its NEIGHBOUR i^1?" and
>   checking x/y parity is what found the fourth packed writer; counting diffs only says how much is wrong.
> - **A harness only proves the paths its scene walks.** `drvdiff` gave three false passes in a row and one
>   false failure (its `static_bg` had no palette), each from harness fidelity rather than the code.
> - **When the plumbing traces identical and the content still differs, suspect the harness's model first.**

*Historical (the original assessment that led here):*

#### 4bpp visual buffer (D16 dithering) -- ASSESSED, not built. PIO-only value.

Asked: would EGA/CGA reduce the buffers? **The answer is not CGA.** The 64KB visual buffer is 8bpp not
because of 16 colours but because each byte holds a **dither PAIR** (two EGA indices, `i & 0xf` and
`i >> 4`), which `INTERCOL` blends -- hence the 256-entry `gfx_sci0_pic_colors`. All 8 bits are
load-bearing. CGA (4 colours, 2bpp) is also not applicable: SCI0 resources carry EGA pairs, there is no
CGA data to switch to.

The real option is **`GFXR_DITHER_MODE_D16`** -- commented `/* Sierra SCI style */` in `gfx_resource.h`,
i.e. what the original hardware actually showed: one EGA index per pixel with a **spatial checkerboard**,
16 colours, which permits a **4bpp buffer (64KB -> 32KB each)**.

**Measured similarity** (`tests/ditherpreview.c` renders a pic both ways to BMP):

| pic | byte-identical pixels | mean RGB diff |
|---|---|---|
| SQ3 pic 2 | **96.0%** | 1.3 / 255 |
| SQ3 pic 3 | 87.9% | 4.5 |
| KQ4 pic 25 | 78.5% | 7.2 |
| PQ2 pic 1 | 75.5% | 10.1 |

Most SCI0 art is solid colour -- both nibbles equal, so the blend equals the pure EGA colour and D16
matches EXACTLY. Only genuinely dithered regions change (PQ2's brickwork and road are the visible case).
SQ3 pic 2 uses just **18 distinct output colours** in F256, so the 256-entry table is barely exercised.

**Value: ~32KB on PIO (more than doubling its ~26KB margin); NOTHING on Pimoroni** (~120KB free, and it
would slow the hot flush loop). So this is a PIO-only lever.

**Plan (as a build option, default OFF):**
1. **Engine: select D16 at decode.** `gfxr_dither_pic0` already exists. Verifiable OFFLINE -- confirm the
   visual map only ever holds 0-15.
2. **Engine: packed visual writes.** Teach the visual draw paths the `nibble_packed` trick that
   priority/control already use (`ctl_set`/`ctl_fill`). Riskiest phase: the visual writers (fills, lines,
   brush/pattern, RLE cels) are MORE numerous than control's. Verifiable OFFLINE with a `tests/picodiff`-style
   differ.
3. **Driver: ~10 sites.** `flush_region` (nibble unpack per pixel -- the hottest loop), `pico_blit_indexed`,
   `draw_line_raw`, filled-rect memset, grab/restore, `bake_static_region`, BACK-restore memcpy, alloc/clear,
   buffer size 64000 -> 32000.
4. **The borrow paths**: parse-time PSRAM borrow sizes, and the decode-buffer reuse (`operations.c:2401`
   hands visual[0] to the decoder as `visual_map->index_data`).

**PHASE 1 DONE (2026-09-11).**

- **The D16 look is ACCEPTED on device, at zero cost.** `pic0_dither_mode` is already a config-file option
  (`config.l:244`) and Pico already reads `0:/freesci.cfg`, so `pic0_dither_mode = d16` answered the visual
  question with NO firmware change at all. Device verdict: "can't really tell if it did anything" with
  `Reading configuration...` confirmed in the log -- i.e. indistinguishable, exactly as the 96%-identical
  measurement predicted for SQ3. **Always settle the cheap visual question before the expensive packing
  work.**
- **Correction to the table above:** the shipped default is `GFXR_DITHER_MODE_D256` (`config.l:596`), not
  F256, so the measured F256-vs-D16 figures are approximately, not exactly, the right baseline. D256 keeps
  the 256-entry palette and only alternates nibble order, which is why the on-device delta is even smaller.
- **`PICO_DITHER_D16` (CMake, default OFF) FORCES the mode** rather than defaulting it -- deliberately. Once
  the buffer is packed, a stray `pic0_dither_mode = d256` on the SD card would write values >15 into nibbles
  and silently corrupt the display. A packed buffer and a runtime-selectable dither mode cannot coexist. It
  overrides both use sites (`sci_resmgr.c` line ~429 and the pic cache key at line ~58) so they cannot
  disagree.
- **PRECONDITION PROVEN, not assumed** (`tests/d16check.c`): decoding EVERY pic of the three SCI0 games we
  actually run through the shared engine path with D16 --

  ```
  == 343 pics across 3 games; 0 violate the 0..15 invariant ==
     worst-case distinct indices in one pic: 16 (of 16 representable)
  ```

  SQ3 115, PQ2 78, KQ4 150 pics, zero above 15. (The other games on disk crash or hang the harness and were
  not counted -- do not read this as all-SCI0 coverage.) This is the assumption the entire packing plan rests
  on, and packing a buffer that can exceed 15 corrupts the display silently, so it was worth proving.

**PHASE 2 STARTED (2026-09-11). Shape decision + first two writers + the measurement.**

**THE SHAPE DECISION: branch on `pxm->nibble_packed` in the WRAPPER, and never hand-write a second
traversal.** `gfx_draw_box_pixmap_i` / `gfx_draw_line_pixmap_i` are thin wrappers over `_buffer` functions,
so the branch goes in the wrapper: desktop pixmaps have `nibble_packed == 0` and take the byte path
untouched, and there is ONE clipping rule. For the actual writing, parameterise the STORE, never
re-implement the walk -- `gfx_support.c` already does this for the crossblits (`#include "gfx_crossblit.c"`
twice with different `FUNCTION_NAME`/`BYTESPP`). **This is not stylistic.** The Pico `ctl_draw_line` was once
hand-written as "a correct line" and picked different pixels from the desktop midpoint DDA on ~32% of
segments, opening a one-pixel gap a flood fill leaked through. Duplicating a traversal duplicates its
pixel-SELECTION, which is exactly where the last bug of this kind came from. A rectangle is the one safe
exception -- it has no selection ambiguity -- which is why `gfx_draw_box_buffer_packed` is hand-written and
lines/ellipses/fills must not be.

**Converted so far: the `gfxr_clear_pic0` visual clear, and `gfx_draw_box_pixmap_i`.** The clear's FILL
VALUES are unchanged by packing and that is not luck: under D16 the `0xff` below the titlebar reduces to
index `0x0f`, and two `0x0f` nibbles are again `0xff`. Only the byte counts halve.

**THE DIFF COUNT IS NOT A PROGRESS BAR -- do not read it as one.** With packing forced on (`PACK=1`) and
nothing converted, SQ3 pic 2 showed 44,752/64,000 differing. After converting the clear it went **UP** to
45,132, and that is correct behaviour: the clear now fills the whole packed buffer with white (15) where
before it filled half the buffer and left the rest 0, so more pixels now disagree with the un-converted
drawing writers. The metric is "**must reach 0**", never "must decrease monotonically".

**The regression guard that DOES have to hold every time: with packing OFF, all three games must stay at 0
differing** (sq3 115 / pq2 78 / kq4 150 pics). That is what proves a conversion did not disturb the byte
path that desktop and today's PIO firmware actually use. It held for these two writers.

Cost so far: PIO `.text` +256 B for the packed box fill (it lives in shared `gfx_support.c`, ungated, and is
dead code until something sets `nibble_packed` on a visual map); `.bss` unchanged at 17,608.

**KEY STRUCTURAL FINDING (2026-09-11): packing MOVES DITHERING FROM A POST-PASS INTO EVERY STORE.** The
decoder writes dither PAIRS (two EGA indices in one byte) and `gfxr_dither_pic0` collapses each byte to one
nibble afterwards. A 4bpp buffer has no spare byte to collapse later, so every packed writer must apply the
D16 selection AT STORE TIME -- and the post-pass must then skip the map or it would dither twice.

The rule, extracted from `gfxr_dither_pic0`'s toggle and now shared as `GFX_D16_SELECT` (`gfx_tools.h`) so
every writer applies the identical one: with `GFXR_DITHER_PATTERN_1` the `selection` flag flips on every
pixel AND at the end of every row, i.e. it is **`(x + y) & 1`** -- even takes the LOW nibble of the pair,
odd the HIGH one. The first two packed stores took the low nibble unconditionally and were therefore wrong
for half the pixels; caught offline, fixed, and it is why the shared macro exists rather than the rule being
re-derived per writer.

A consequence for the packed box fill: each packed byte holds pixels (even x, odd x) and `linewidth` is
even, so the fill byte is constant along a row but **SWAPS between even and odd rows**.

**`gfx_draw_line_pixmap_i` converted, and it validates the shape decision.** `gfx_line.c` is now included a
FIFTH time from `gfx_support.c` with only a different `PLOT` macro -- same source, same DDA, same pixels,
only the store differs. `PLOT` defaults to the original `memcpy`, so the four existing inclusions are
byte-for-byte unchanged. Packed diffs on SQ3 pic 2 fell 45,132 -> 39,517.

**THE FLOOD FILL AND THE DITHER PASS ARE CONVERTED, and both taught something the survey missed.**
Packed diffs on SQ3 pic 2: **39,517 -> 23,970 -> 1,414 of 64,000 (97.8% correct)**; pic 3 1,355.

1. **The fill's BOUNDARY TEST reads dither pairs, and packing made it never terminate.** `legalmask = 0x0ff0`
   checks the HIGH nibble on odd coordinates and the LOW one on even -- it is reading the two halves of one
   pixel's PAIR. Packed, that byte holds two DIFFERENT pixels, so the test reads nonsense and the fill
   recursed **~52,000 frames deep into a stack overflow**. Fixed by routing it through the `bounds_packed` /
   `ctl_get` path that already existed for the packed PRIORITY map, with nibble-wide masks
   (`legalcolor = 0x0f`, `legalmask = 0x0f0f`): `ctl_get` returns the pixel's OWN nibble, so both coordinate
   parities want the same mask and the background to compare against is index 0x0f, not the 0xff pair.
   **A crash, not a wrong picture -- so "it segfaulted" was a CORRECTNESS signal here, not a bad pointer;
   check recursion depth before suspecting the new code's addressing.**
2. **The dither post-pass had to be SKIPPED for packed maps** -- the other half of "dithering moves into the
   store". A packed map was already dithered per-pixel by the writers; walking it again treats each byte as
   a pair and re-selects from two unrelated pixels. This alone took 23,970 -> 1,414.

The fill's span write reuses `gfx_d16_fill_span_packed`, shared with the packed box, so the (x+y)&1 parity
rule has exactly one implementation.

**BRUSH + ELLIPSE CONVERTED -- the decode half is essentially done.** Packed results:

| game | pics | fully exact | differing |
|---|---|---|---|
| SQ3 | 115 | **99** | 16 (thirteen of them by 1-6 pixels) |
| PQ2 | 78 | **74** | 4 |
| KQ4 | 150 | **131** | 19 |

SQ3 pic 2 went 1,414 -> 108 of 64,000; **pic 3 reached exactly 0**.

**`packed` is now THREE-state, because the two packed formats are NOT interchangeable** (`PICO_PACK_D16`
in `gfx_tools.h`): 0 unpacked, 1 packed CONSTANT nibble (priority/control, one value), 2 packed DITHER PAIR
(visual, nibble varies by (x+y)&1). `_gfxr_auxplot_brush` and `_gfxr_fill_ellipse` already took a `packed`
flag from the priority work and used `ctl_fill` -- correct for priority, and it would have written the low
nibble everywhere on a visual map, silently losing half the dither. The visual call sites had been
hardcoding 0.

**A FOURTH packed writer was hiding, found by classifying the residual rather than re-reading code:
`_gfxr_plot_aux_pattern`.** It selects its target `map` at RUNTIME (`case GFX_MASK_VISUAL: map =
pic->visual_map`), so the visual map reaches it without ever being named at the call site -- which is why the
survey missed it. When packed it wrote via `ctl_fill`, i.e. ONE CONSTANT NIBBLE from a dither pair. Same trap
as the brush and ellipse, harder to see. Fixing it took the differing-pic counts **SQ3 16 -> 4, PQ2 4 -> 1,
KQ4 19 -> 6**, i.e. SQ3 111/115, PQ2 77/78, KQ4 144/150 now byte-exact.

**How it was found, because the method generalises:** the differ was extended to CLASSIFY each mismatch
rather than just count it -- does pico's value equal desktop's value at the NEIGHBOUR pixel `i^1` (the other
pixel in the same packed byte)? what is the x- and y-parity of the differing pixels? Dumping coordinates then
showed a contiguous horizontal run whose packed byte was the exact NIBBLE-SWAP of the correct one. Counting
diffs tells you how much is wrong; classifying them tells you WHICH WRITER. Use `CLASSIFY=1` and `COORDS=1`.

**Also fixed along the way, both real bugs:** the ellipse's `offset1` guard had been changed from the
original `if (offset1)` -- a SENTINEL, zeroed to mean "skip, menu bar" -- to `if (offset1 != offset0)`,
which both dropped and duplicated spans; and `ELLIPSE_OR` still OR-ed a whole dither pair into a packed map.
Neither moved the counts (these paths are not exercised at `xfact == 1`, where the ellipse radius is 0), but
both would have bitten later.

**SCOPE REDUCTION: `sci_view_0.c`'s cel RLE is OUT of the 4bpp work, and cannot be packed at all.**
Cel buffers use **255** as the transparency key -- `retval->color_key = 255; /* Pick something larger than
15 */` (`sci_view_0.c:87`), the comment saying outright that it is chosen to be out of nibble range. A packed
cel has no way to express "transparent", so the format is a hard blocker, not an effort question. It is also
unnecessary: cels decode into `g_pico_priority_scratch` (the B-1.3 borrow) and are offloaded to PSRAM, and
**nothing decodes a cel into `visual[0]`**, so they never touch the buffer being packed. Only the DRIVER's
blit has to read an 8bpp cel and write packed. Do not re-open this.

**DRIVER HALF: MOSTLY CONVERTED (2026-09-12), verified by `tests/drvdiff` -- packed and unpacked produce
BYTE-IDENTICAL panel output.** `PICO_VIS_BYTES` goes 64,000 -> **32,000**, which is the whole point.

Everything routes through three macros so the unpacked build is byte-identical and a MISSED site is a
compile-visible direct `visual[0][i]` rather than a silent shear: `PICO_VIS_BYTES`, `VIS_GET`, `VIS_SET`.

| site | how |
|---|---|
| `flush_region` | per-pixel unpack, row base hoisted (hottest loop) |
| `draw_line_raw` | pointer arithmetic -> INDEX arithmetic; same algorithm, same pixels |
| `pico_draw_filled_rect` | packed span fill, constant index (the driver's colour is already resolved to ONE slot -- not a dither pair) |
| `pico_blit_indexed` | `destbuf` changed from "already homed to (dest.x,dest.y)" to BASE + `dest_index` + `dest_packed` -- **a packed buffer cannot be homed by pointer at an odd x**. 5 call sites updated. `dest_packed` is a RUNTIME flag because the blit serves visual[0], the mapped static buffer AND the one-row compose scratch, which are not all the same format |
| `pico_grab_pixmap` / grabbed restore | grabs stay 8bpp (their data is consumed elsewhere as bytes, and a grab can start at an odd x), so the pack boundary is crossed on the way in and out, symmetrically |

**FALSE-PASS TRAP, worth internalising:** the first `cmp` passed while grab/restore were still UNCONVERTED --
because the harness scene never grabbed anything. **A harness only proves the paths its scene actually
walks.** The scene was extended to grab a region and restore it elsewhere before the pass meant anything.
Check coverage before trusting a green result.

**DRIVER HALF COMPLETE (2026-09-12) -- packed and unpacked produce BYTE-IDENTICAL panel output across a
scene covering filled rect, line, cel blit, STATIC draw, bake, compose, BACK restore, grab, restore and
flush.** `pico_bake_static_region`, `pico_compose_ensure` and `pico_invalidate_static_region` are converted:
both surfaces share the pixel->byte mapping, so the interior is a straight byte copy and only the shared
first/last bytes need read-modify-write (at an odd x0 the low nibble belongs to x0-1, at an odd end the high
nibble belongs to x0+w).

**Two REAL bugs surfaced on the way, neither found by reading the code:**
- `pico_compose_ensure` / `pico_invalidate_static_region` copied BYTE-PER-PIXEL rows, which on a packed
  surface reads past the row and leaves the tail unwritten.
- **the blit's PSRAM VISUAL source read was never packed-aware** -- the priority map had a packed branch, the
  visual one did not -- so a packed composed surface came back as garbage.

**The last "failure" was HARNESS FIDELITY, not a driver bug, and it cost four patches to learn:** the
harness's `static_bg` had no `colors[]`, so the BACK restore blitted THROUGH an uninitialised `lut[]`, which
differs between builds and looks exactly like a packing bug. Instrumenting finally settled it -- a probe
showed the bake wrote pixel 98 = 3 and the restore read byte 0x63 (low nibble 3) at the same address, i.e.
bake and restore already AGREED. **When the plumbing traces identical and the content still differs, suspect
the harness's model of the world before the code under test.**

**`PICO_PACK_VISUAL` IS NOW A CMAKE OPTION (default OFF) and the firmware BUILDS.** It forces
`PICO_DITHER_D16` on (a nibble holds 0..15, so the two must not disagree) and FATAL_ERRORs if combined with
`PICO_PSRAM_MAPPED` -- packing is PIO-only; the mapped target has SRAM to spare and a different render model.

```
cmake -B build-pico-4bpp -DPLATFORM=pico -DPICO_SDK_PATH=~/Source/pico-sdk \
      -DPICO_BOARD=pico2 -DPICO_PACK_VISUAL=ON
```

The decode path was wired to match: `GFXR_VIS_BYTES` in `sci_resmgr.c` is the ONE place that knows the
visual map's byte extent, so the deferred decode alloc, the borrowed visual[0], the PSRAM offload, the
overlay base-restore and the `[ovl]` probe's byte sums cannot drift apart; `operations.c`'s early-pin
fallback matches it. `nibble_packed` is set on the visual map at both hand-off points.

**Size: `.bss` 17,768 -> 18,088 (+320, the row staging). The 32,000-byte saving is in the HEAP**, since
visual[0] is `sci_malloc`'d -- it shows as runtime headroom, not in `arm-none-eabi-size`.

**DEVICE-TESTED 2026-09-12: renders, but WRONG -- and it exposed a DESIGN COLLISION that changes the value
proposition. Read this before doing more 4bpp work.**

Observed on device: SQ3 boots to a grey box with a green stripe; the Sierra logo renders with black
horizontal slashes; intro text is surrounded by **2x-magnified image fragments**; PQ2's intro shows the same,
the glovebox scene too, and **leaving the car HardFaults**.

**THE HARD FAULT: packing visual[0] destroys the buffer the CONTROL PASS borrows.** `sci_resmgr.c` hands the
just-offloaded visual buffer to the control pass as its flood-fill `aux_map` (`reuse_aux_buf`). But
`GFXR_AUX_MAP_SIZE` is `320*200` = 64,000 bytes, **ONE BYTE PER PIXEL**: it carries flag bits `0x40`
(FRESH_PAINT) and `0x10` alongside the colour nibble, so it needs >=7 bits per pixel and **cannot be
packed**. A packed visual[0] is half that, so the control pass overran it by 32,000 bytes -- heap
corruption, hence the fault. Now fixed by allocating a real aux when packed.

**WHY THAT MATTERS MORE THAN THE BUG:** the 32KB saved on visual[0] is handed straight back as a 64KB
TRANSIENT for the aux -- and a large contiguous transient is exactly what this port spent months
eliminating (B-1, B-1.2, B-1.3, the whole fragmentation story). Net at the decode PEAK the packed build may
be WORSE, not better; it only wins in the valley between decodes. **Before any further 4bpp work, measure
peak arena with and without packing.** If the peak regresses, the honest options are to drop the control map
(loses collision), find another 64KB donor, or abandon 4bpp.

**THE HARD FAULT IS EXPLAINED and matches a documented signature.** The dump decodes to
`LR = _gfxwop_container_free` (`widgets.c:1630`), `PC = 0`, `CFSR = 0x00020000` (UFSR INVSTATE), BFAR/MMFAR
holding the registers' OWN addresses (so no valid fault address) -- the branch-to-NULL family already in this
file: a widget freed through a `widfree` pointer that had been overwritten with zero. That is heap
corruption, which is exactly what a 32,000-byte `aux_map` overrun produces. Very likely fixed by the aux
change; needs a retest to confirm.

**TWO ALLOCATION SITES DISAGREED, which explains the TIMING.** `pico_init_specific` allocated
`xsize * ysize` (64,000) while `pico_alloc_visual` used `PICO_VIS_BYTES` (32,000). So visual[0] started
FULL-size at boot -- the aux reuse fitted, nothing overran, the game looked mostly fine -- and only became
half-size after the first realloc (a restore, or the parse-time borrow), after which the control pass
overran it. Hence a fault late, on leaving the car, rather than at boot. Both sites now use
`PICO_VIS_BYTES`. **Two allocation sites with different sizes is a trap, not an optimisation.**

**GENUINE MILESTONE in the PQ2 shots: the pic renders CORRECTLY** -- colours, dithering, the lot -- so
decode + background blit + flush are all correct at 4bpp on real hardware. That is the core of the scheme
working.

**AUX PACKING ATTEMPTED AND NOT CONVERGING (2026-09-12) -- `PICO_PACK_VISUAL` is KNOWN BROKEN, do not
flash it.** The bit analysis was right as far as it went: on the unscaled path the aux's live bits are the
clipmask (1/2/4) plus `CLIPMASK_HARD_BOUND`, four bits, so a nibble should suffice. Accessors
(`AUX_NIB`/`AUX_GET`/`AUX_TEST`/`AUX_OR`/`AUX_LINE_STORE`, now in `gfx_resource.h`) were unit-tested
correct in isolation, and every live site was converted:

- all 14 accesses in `sci_picfill_aux.c`
- the two live writes in `_gfxr_plot_aux_pattern` (`sci_pic_0.c`)
- the aux LINE tracer's store -- found only by POISONING the struct field, because
  `_gfxr_auxbuf_line_draw`/`_clear` alias the buffer through a local (`buffer = pic->aux_map`) and so never
  matched a grep for `aux_map[`
- the aux clear in `gfxr_clear_pic0`

Confirmed genuinely dead (not linked, `nm`): `_gfxr_auxbuf_tag_line`, `_gfxr_auxbuf_spread` (hence
FRESH_PAINT and the `0x10` test); the `SCALED_CHECK` block is inside `#ifdef DRAW_SCALED`; `TEST_POINT` is
`WITH_PIC_SCALING`-only.

**And it still fails**: `tests/picodiff` shows SQ3 pic 3 at 51,917/64,000 control diffs packed vs 0 unpacked,
pic 2 at 18,666, pic 4 at 36,879 (pics 9 and 25 are clean). **The diagnostic signal is that each conversion
moved the number NEGLIGIBLY** -- 51,526 -> 51,917 after the line tracer. If the dominant site had been
found, the number would collapse. So the model of what the aux holds is probably wrong somewhere, not merely
incomplete. A candidate worth checking next: the dead debug at `sci_pic_0.c:1912` reads `aux & 0xf` as a
COLOUR, which would mean the low nibble carries data that the bit-remap collides with.

**Everything remains inert with the option OFF** -- unpacked picodiff is 0/64,000 on every pic tested and all
five real configs build clean. The CMake option now emits a loud WARNING.

*Time spent vs returned: the 4bpp line has now cost three device cycles (two on faults I introduced) and has
not yet produced a working build. The aux insight may still be correct, but it needs a fresh look rather than
more incremental conversion.*

**SECOND DEVICE TEST: a NEW fault, and it was MY bug in the aux fallback.** Dump decoded to
`PC = free_int_hash_map_node_t_recursive` (`int_hashmap.c:33`), `LR = sm_free_script`
(`seg_manager.c:490`), `CFSR = 0x8200` (precise bus fault), `BFAR = 0x35fe159b` (wild) -- a script hashmap
node with a garbage `next`, i.e. heap corruption again with a different victim.

Cause: the aux-allocation-failure path I added did a bare `free(control_buf)`. **`control_buf` IS the
permanent B-1 priority scratch when `priority_is_scratch`**, and both pre-existing free sites guard on
exactly that ("never free it, just detach"). Omitting the guard freed a buffer used for the rest of the
session -- a use-after-free, which is why the victim rotated (a widget `widfree` pointer first, then a
hashmap node). **When adding a code path that frees a shared buffer, copy the guards from the existing free
sites; here there were two, both correct, and the new path had neither.** Fixed.

That path is also reached far more often than it looks: the extra 64KB aux transient makes the malloc fail
on a tight heap, which is precisely when the bad free fires.

**Do NOT chase the 2x-magnified artifacts yet.** The build that produced them had an active 32KB heap
overrun, and heap corruption can produce arbitrary visual garbage; some or all of the artifacts may simply
be it. Retest with the aux fix first, then re-characterise whatever survives. If they do survive, the
signature is diagnostic -- writing one byte per pixel into a packed buffer makes each byte display as TWO
pixels -- and the culprit is a path NEITHER harness covers (the driver is clean by drvdiff, the pic decoder
by visdiff), most likely the kgraphics transition code (`old_screen`/`newscreen`) or text layout.









**RESIDUAL (open), sharply characterised:** SQ3 pic 2's 108 pixels are ALL ON ONE ROW -- y=18, x 66..319,
non-contiguous -- and were unmoved by the `_gfxr_plot_aux_pattern` fix. Decisive detail: pico's underlying
pair is `0x9B` where desktop's is `0xF9`, and those are NOT nibble-swaps of each other (0xF9 swapped is
0x9F). Different COLOURS, not a mis-selected nibble -- so this looks like an ordering/overwrite difference
on that row, i.e. a later op that applied on desktop and partially did not on the packed path, NOT a writer
format bug. SQ3 pic 28 (31 px) is probably the same. Chase it with `CLASSIFY=1 COORDS=1`, and treat "which
op paints y=18 last" as the question. Ruled out: `gfxr_remove_artifacts_pic0` (scaled path only), the
`getenv("FOO1")` debug writes (dead), and the line writer's coordinate convention.

**Coverage limit, to be explicit:** `visdiff` covers pic decode only. View cel decode, runtime drawing
(dialog fills, kGraph lines) and the entire DRIVER half -- flush, blit, grab/restore, bake -- have NO offline
coverage, and the driver half is exactly where the 16-bit LCD attempt died. Extend the harness (a viewdiff,
and a packed-buffer-to-PNG renderer) before trusting those, or accept device testing for them.



**RISK -- this is the same shape as the 16-bit LCD attempt that failed, four times bigger.** That broke
because one writer was converted out of many; here there are ~10 in the driver PLUS shared decode paths, and
any missed writer shears the image. Mitigation: phases 1-2 are fully offline-verifiable, so only phase 3
needs hardware. Costs beyond the work: a slower flush loop, and a visibly different (arguably more
authentic) picture.

