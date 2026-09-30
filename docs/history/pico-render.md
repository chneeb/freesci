# Pico — render-path history (PIO single-buffer model)

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 635-748 -->

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

<!-- from CLAUDE.md lines 2382-2406 -->

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

<!-- from CLAUDE.md lines 2568-3327 -->

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

**RE-CONFIRMED INTERMITTENT (2026-09-12), and the mechanism is now named.** The SAME binary
(`build-pico`, composed ON) rendered Pestulon MISSING on one run and CORRECTLY on the next -- so it is
neither a composed-surface regression nor memory exhaustion, it is the documented runtime-state fragility
below, still present.

**What the failing run showed, and what it ruled out:** two `malloc 64000 failed` lines immediately before
the Pestulon screen, but **NO `Could not add pic`** -- so `gfxr_add_to_pic` SUCCEEDED and the overlay was
drawn; the logo was lost inside a decode that completed. That rules out the obvious causal story (failed
allocation -> overlay skipped) which was proposed and disproved by that one grep. **Check for the error line
before building a theory on an allocation failure; the recovery paths here are designed to be silent.**

**The fragility has a concrete shape: an OVERLAY decode is the most exposed one in the engine**, because
`gfxop_add_to_pic` has NEITHER mitigation `gfxop_new_pic` gives a fresh pic -- no `visual[0]` borrow (so it
must find a FRESH 64,000-byte contiguous block) and no early-pin retry. That is why the deferred-alloc
failure correlates with the flip even though it is not the direct cause.

**Untried fix, built once and reverted unbundled (2026-09-12):** give `gfxop_add_to_pic` the same
`visual[0]` borrow the fresh-pic path uses, which removes the 64KB allocation from the overlay path
entirely rather than adding a retry. Safe for the same reason as the fresh-pic borrow -- `restore_base`
reloads the base from PSRAM into the decode buffer first, and `pico_render_background` restages `visual[0]`
after -- and both `gfxr_add_to_pic` calls need arming, since `sci_resmgr` consumes the pointer on use. It
targets the fragility rather than a hard failure, so **judging it needs SEVERAL COLD BOOTS, not one**.

**To name the exact flip, capture a FAILING run with `FSCI_PROBE_GFX`** and read `[ovl]` against the healthy
baseline below: `restore_base=0` means the base-restore condition failed; `sum_before` around 16.3M means
the base came back white (the clear ran instead of the restore); garbage `sum_before` means a stale vaddr;
`delta=0` means the overlay drew nothing.

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

### ACCEPTED LIMITATION (2026-09-12) — SQ3 intro: Two Guys panels survive as partial bands

**Bisected to `PICO_STATIC_VIEW_PRIORITY`, DETERMINISTIC, and deliberately NOT fixed.** In the SQ3 intro the
Two Guys portrait panels persist into the following credits screens as partial horizontal bands with the new
screen's text drawn over them, every run. `-DPICO_STATIC_VIEW_PRIORITY=OFF` clears it completely;
`-DPICO_STATIC_COMPOSED=OFF` does not. Same flag, and per the record the same flag, that causes the PIO
dialog bleed.

**DECISION: keep `PICO_STATIC_VIEW_PRIORITY=ON` (user, 2026-09-12).** The ledger:

| ON buys | ON costs |
|---|---|
| actors occluded by settled stopUpd views -- Roger behind SQ3's door and motivator, PQ2's cars in front of the officer | SQ3 intro panel bands (this item) |
| | the PQ2 dialog bleed (open item 1b) |

Gameplay occlusion outweighs two intro/dialog cosmetics. Note the SQ3 door **not closing** is broken either
way, so it is NOT on the ON side of this ledger. The proper fix for both costs is the **transient working
priority map** (already parked for SRAM; it is what the mapped target does by default), not flipping this flag.

**THREE HYPOTHESES TRIED AND DISPROVED on this bug -- do not repeat them:**
1. *The composed surface persists the panels.* No: `-DPICO_STATIC_COMPOSED=OFF` does not clear it, and in
   priority-only mode `pico_priority_only_static` skips the colour bake, so nothing puts them in `composed`.
2. *`pico_compose_ensure` failing makes the fullscreen static draw fall through into `visual[0]`.* Removing
   `compose_ensure` from that gate, so the draw always goes to the scratch row and never touches `visual[0]`,
   **changed nothing on device**.
3. *Cels with `pxm->data` set bypass the gate via the crossblit.* No: `gfx_xlate_pixmap` is skipped on Pico,
   so `pxm->data` is NULL and the gate was already being taken.

So the mechanism is NOT simply "the fullscreen static draw paints visual[0]" -- routing that draw away from
visual[0] entirely does not help. Whatever `PICO_STATIC_VIEW_PRIORITY` does that matters here is something
else in that block (it also sets `pico_priority_only_static` and bakes priority into the PSRAM map).
**Start there, and bisect rather than reason -- the bisect found in one flash what three rounds of code
reading did not.**

### FIXED (device-confirmed 2026-09-27) — SQ3 "Pirates of Pestulon" missing: the overlay decode's 64 KB buffer

**Symptom → cause, closed.** The user established the correlation on device: the title is missing
**exactly when** the log shows the `malloc 64000 failed` pair. Mechanism, read from the code:
`gfxop_add_to_pic` had no `visual[0]` borrow, so the overlay decode (`sci_resmgr.c`, the deferred
`malloc(GFXR_AUX_MAP_SIZE)`) needed a fresh contiguous 64 KB block while `visual[0]` stayed resident. On a
fragmented heap it failed and returned `GFX_ERROR` — but **`gfxr_add_to_pic` ignores the return value of
`gfxr_interpreter_calculate_pic`**, so nothing errored (hence never a `Could not add pic`), the overlay's
commands never ran, and the logo stayed on screen without the title. Two lines because `gfxop_add_to_pic`
called `gfxr_add_to_pic` twice (scaled + "unscaled"), and both allocations failed. The 2026-06-29 note that
"the logo was lost inside a decode that completed" was wrong about completion: the decode aborted, silently.

**Fix (`operations.c` `gfxop_add_to_pic`, PIO only — `HAVE_PICO && !PICO_PSRAM_MAPPED`):** the untried
fix recorded above. Arm the `visual[0]` borrow before the decode exactly as `gfxop_new_pic` does
(`restore_base` reloads the base pic from PSRAM into it first; `sci_resmgr` skips every free of a borrowed
buffer), detach it afterwards, and restage `visual[0]` with `pico_render_background` after `_gfxop_set_pic`
(stage only, no LCD flush). Also skip the redundant second `gfxr_add_to_pic` at 1x: it returned the same
`res->scaled_data.pic` after re-decoding the overlay onto the finished composite (`gfxop_new_pic` already
skips it the same way). Net: an overlay decode now needs **no** 64 KB allocation and runs once, not twice.

**Device:** SQ3 with sound, the new firmware (also `PICO_STREAM_METHODS=7`): the `malloc 64000 failed` pair
is gone and the title shows over the logo; intro, savegame load and exit all clean. The Pimoroni build is
untouched (its `.bss` is unchanged); the PIO `.bss` is unchanged by this fix.

### Also accepted: SQ3 "Pirates of Pestulon" is INTERMITTENT, and separate from the above

**Superseded 2026-09-27: FIXED, see the section just above.**

Same binary, missing on one run and correct on the next (2026-09-12). Unlike the panels this is NOT
deterministic, so it is a different bug -- the long-standing runtime-state fragility of the overlay path.
Accepted as-is; see the Pestulon section for the `[ovl]` probe fields that would name it.

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

<!-- from CLAUDE.md lines 3997-4048 -->

### RESOLVED-BY-TOGGLE (device-confirmed 2026-09-13) — Colonel's Bequest fingerprints need `[V]` OFF, i.e. `PICO_STATIC_VIEW_PRIORITY`

**Cause identified: `PICO_STATIC_VIEW_PRIORITY`, not the composed surface.** With `[V]` off the
copy-protection fingerprints render and the game can be entered. Composed was the original suspect and is
**EXONERATED** -- device-tested with `[C]` off and the fingerprints were still missing.

Consistent with what the record already said about this flag: it decides whether views APPEAR, not merely how
they are occluded (PQ2's glovebox items were MISSING with it off -- here the polarity is reversed, and the
fingerprints are missing with it ON). It routes settled `stopUpd` dynviews through the fullscreen static draw,
so a view that settles immediately -- exactly what a copy-protection screen does -- lands in its blast radius.

**Each target offers only the toggles that DO something** -- verified from the ELF strings:
PIO+sound shows `[S] [C] [V]`, PIO silent shows `[C] [V]`, **mapped shows `[S]` only**. `[C]` was already
excluded there (`PICO_STATIC_COMPOSED=OFF`); `[V]` had to be gated on `!PICO_WORKING_PRIORITY` because it is
INERT on mapped -- that flag compiles the `widgets.c` routing out, and its real working priority map makes
`row_pri` non-NULL, which forces `psram_pri` and therefore `bake_pri` to 0 whatever the toggle says.
Harmless, but a toggle that silently does nothing invites a false A/B, which is the exact failure mode that
cost time elsewhere in this session.

**Per-game toggles make this a launch-time choice rather than a rebuild**, which is what turned a parked
mystery into a two-boot answer: `[C]` first (eliminated composed), then `[V]` (found it).

**Evidence note:** the captured log shows `[gfx] composed surface OFF` but NO
`[gfx] static view priority` line, so it predates the `[V]` toggle -- the finding rests on the device
observation, not on that log.

**Colonel's still hits its OWN memory wall**, unchanged by any of this and documented before today:
`calloc 2060 failed, free=304, arena=466,672` at `reg_t_hashmap.c` `new_reg_t_hash_map` -- 304 bytes free is
TRUE EXHAUSTION, not fragmentation. The same signature is already on record for Colonel's with sound OFF; with
sound ON it simply arrives sooner. Toggle `[S]` off to get further.

### SUPERSEDED — the original suspicion (kept for the reasoning)

*The composed surface was suspected purely by symptom; the one-flash A/B disproved it. Attributing a rendering
bug to the most recently changed subsystem is a reasonable first guess and was wrong here.*

*Historical framing:*
### OPEN, PARKED (2026-09-13) — Colonel's Bequest: copy-protection fingerprints do not render

**Blocks entry to the game** (the fingerprints must be compared to proceed). **Suspected `PICO_STATIC_COMPOSED`**
(default ON for PIO) -- unproven, and attributed by symptom rather than evidence.

The log says nothing useful, which is itself consistent with a compositing problem: no OOM, no fault, no GFX
error near the copy-protection screen, peak `used` 424,000 with headroom. So this is NOT memory.

**The cheap A/B when picked up: press `[C]` in the chooser -- no rebuild, no flash.** If the fingerprints come back, the
composed surface is confirmed and the question becomes which of its paths (the bake, the invalidation rules,
or the `pico_static_fullscreen` routing) drops them. Note the record already lists two OTHER unfixed Colonel's
rendering bugs (transparent dialog fill, sticky ornate corners), so this may share a cause with them.

**GOOD NEWS from the same run: Colonel's Bequest intro sound WORKS** -- a third game playing music on PIO.

<!-- from CLAUDE.md lines 4588-4885 -->

**1. KQ4: Rosella "swims in the lawn" -- FIXED (2026-09-12), one line, verified offline.**

The two-pass Pico decode stripped `GFX_MASK_VISUAL` from **`original_drawenable`** as well as `drawenable`
(`sci_picfill.c`, "Strip masks for NULL index_data buffers to avoid crashes"). Only `drawenable` gates the
actual writes; **`original_drawenable` feeds AUXBUF_FILL's clipmask**, and dropping VISUAL there leaves the
control fill bounded by control-marked pixels ALONE -- where desktop's combined pass is also bounded by the
VISUAL boundaries. KQ4 pic 25 has a fill that relies on those, so it escaped and flooded the map with
control 3; `smallBase::doit` then read "not land" under the ego and picked swimming.

**Fix: strip only `drawenable`.** The visual writers are already NULL-guarded (`gfx_draw_line_pixmap_i`,
`gfx_draw_box_pixmap_i` both early-return on `!index_data`) and the fill's own memsets are gated on
`drawenable`, so the visual map is still never written -- but the aux now receives its visual boundary marks
(the marking `mask` in `_gfxr_draw_line` comes from `gfxr_draw_pic01`'s OWN `drawenable`, a different local
that was never stripped) and the fill is bounded correctly.

**Measured with `tests/picodiff` (control maps, desktop byte path vs Pico), before -> after:**

| game | baseline | with fix |
|---|---|---|
| KQ4 | **8 pics** differ (7, 13, 15, 19, 23, 25, 32, 34), 24k-57k diffs each | **1 pic** (7), and only **53** diffs |
| PQ2 | 2 pics (15: 56,180; 30: 8,709) | **unchanged -- pre-existing, NOT a regression** |
| SQ3 | 0 | 0 |

`tests/picodiff/visdiff` confirms the VISUAL maps are untouched (115/78/150 pics, 0 differ). Desktop and
both Pico targets build clean; PIO `.text` +32 B, `.bss` unchanged.

**Still open, and now cleanly separated from this:** KQ4 pic 7's residual 53 diffs, and PQ2 pics 15/30 which
this fix does not touch at all -- a different cause, possibly the same class with another trigger. Both are
`picodiff`-visible, so they can be chased entirely offline.

**DEVICE-CONFIRMED (2026-09-12): Rosella walks on the lawn.** Closed.

**1b. PIO dialog bleed — mechanism identified (2026-09-06), fix not built.**

PQ2's dialog boxes are overpainted by scene content on the **PicoCalc PIO target only**. Pimoroni is clean
because `PICO_WORKING_PRIORITY` compiles the static-view routing out; PIO cannot afford the priority maps, so
it still relies on that routing.

**RULED OUT — port-clipping the static draw.** Device-tested (`PICO_STATIC_VIEW_CLIPPED`, since reverted):
replacing `gfxop_draw_cel_static`'s forced FULLSCREEN clip with the port clip (mirroring
`_gfxwop_pic_view_draw`) **did NOT fix the bleed**. So `view->parent->zone` still covers the dialog area, and
**no clip change can fix this** -- do not re-attempt it.

**RULED OUT by an offline desktop trace (2026-09-11) -- the "static path skips DIRTY-RECT tracking" mechanism
is WRONG, and the dirty-rect fix it implied would have been a NO-OP.** Do not build it.

The retracted claim was: `operations.c:2211` skips `_gfxop_add_dirty` when `static_buf`, so a static-routed
draw paints the displayed `visual[0]` on Pico while registering nothing, and is never repaired. Correct about
the skip; wrong about the consequence. **Both** widget paths follow the static draw with an UNCONDITIONAL
normal draw of the same cel at the same position -- `_gfxwop_pic_view_draw` ("Draw again on the back buffer",
`widgets.c:983`) and the Pico stopUpd route in `_gfxwop_dyn_view_draw` (`widgets.c:943`, which falls through).
That normal draw registers `_gfxop_add_dirty` with the **UNCLIPPED** cel rect, which is exactly the area the
static draw painted. So the region is already tracked.

**Measured, not argued** (`FSCI_PROBE_DIRTY` + `FSCI_SIM_PICO_STATIC`, SQ3, 4352 frames):

```
static (GFX_BUFFER_STATIC)  14281
  paired with a normal draw 14281  (100.0%)
  rect covered by a +dirty  14281  (100.0%)
```

The trace runs on the DESKTOP because the rect bookkeeping is shared code -- desktop executes the identical
sequence, it just cannot show the artifact (its STATIC draws land in the off-screen `visual[2]`; Pico writes
the displayed `visual[0]`). See `tests/dirtytrace.py` for capture + analysis.

**What this leaves, and the corrected framing.** The dirty rect is not the missing repair -- it is the
*delivery* mechanism: it FRONT-flushes the region, corrupted pixels included. `_gfxop_buffer_propagate_box`
is the chokepoint for both, and a dirty rect only ever produces the FRONT flush (`_gfxop_update_box`), never
a BACK restore. So what actually protects a dialog from a view drawn at overlapping coordinates is **draw
ORDER** (the dialog's port is drawn after), not the clip -- which independently explains why the
`PICO_STATIC_VIEW_CLIPPED` experiment failed, and why `nopri` works (no static draw at all).

One asymmetry worth keeping: `_gfxwop_pic_view_draw` uses `gfxop_draw_cel_static_clipped` (PORT clip), so
kAddToPic picviews are already clipped. Only the Pico stopUpd dynview route uses `gfxop_draw_cel_static`,
which forces `gfx_rect_fullscreen`. So the fullscreen clip is specific to the routing PIO added.

**TRIED AND REVERTED (2026-09-11): ambient clip -- `gfxop_draw_cel_static_clipped` instead of
`gfxop_draw_cel_static`. DEVICE-TESTED: fixed PQ2, BROKE SQ3. Do not re-attempt.**

Device result was genuinely split: **PQ2 fully fixed** -- dialogs clean, glovebox items show AND pick up
cleanly, cars still occlude correctly. **SQ3 regressed** -- the spaceship door no longer visibly closes.

**Why, and it is systemic, not one door.** The ambient `state->clip_zone` at a dynview's static draw is
frequently **STALE** -- often disjoint from the cel entirely (measured: `rect=(22,16,60,66)` against
`clip=(478,32,120,132)`; the pair `601/1/9` and `601/0/9` each carry the OTHER's zone, i.e. the clip lags a
widget). That staleness is precisely why upstream overrides it with `gfx_rect_fullscreen` (`operations.c`
comment: "Except that the area it's clipped against is... unusual ;-)"). Re-measured per static draw against
its paired fall-through `gfxop_draw_cel`:

| | static paints nothing | normal paints nothing | **both** -> cel invisible |
|---|---|---|---|
| fullscreen (baseline) | 0.0% | 75.8% | **0.0%** |
| ambient clip (tried) | 75.7% | 75.7% | **75.7%** |

So in **75.8% of cases the fullscreen static draw is the ONLY thing painting the cel** -- the paired normal
draw is already clipped to nothing. Matching the static draw to it does not make the two agree; it makes
both paint nothing. The door is one visible instance.

**METHOD TRAP, recorded because it cost a device cycle.** The same number was in hand BEFORE the flash,
under the label "static painted MORE than the paired normal draw: 75.8%, 32.3M excess pixels", and was read
as excess-to-eliminate. Driving that metric to zero looked like a clean win ("excess 32,272,744 -> 0") while
actually meaning "the static draw now paints nothing". **An `excess` metric cannot distinguish harmful
overdraw from the only draw there is** -- the right metric was the one in the table above, *does any draw
cover the cel at all*. Measure coverage, not excess.

*Historical (the analysis that led to the reverted attempt -- the defect it names is REAL and is what the
second-surface work below addresses; only the clip-based remedy was wrong):* every static draw uses
`clip=(0,0,640,400)` (14281 of 14281), because
`gfxop_draw_cel_static` overrides `state->clip_zone` with `gfx_rect_fullscreen` (`operations.c`, comment:
"Except that the area it's clipped against is... unusual ;-)"). Upstream that is harmless -- the draw lands
in the off-screen `visual[2]`. On Pico `pico_draw_pixmap` ignores the buffer and writes the DISPLAYED
`visual[0]`, so those out-of-port pixels are painted where nothing repaints them. Measured against each
static draw's PAIRED normal draw:

| | static draws | painted more than the paired draw | excess pixels |
|---|---|---|---|
| `gfxop_draw_cel_static` (fullscreen) | 14281 | **10832 (75.8%)** | **32,272,744** |
| `gfxop_draw_cel_static_clipped` (ambient) | 13961 | **0 (0.0%)** | **0** |

**Why this is NOT the ruled-out `PICO_STATIC_VIEW_CLIPPED` attempt:** that one clipped to
`view->parent->zone` -- the whole room port, which does cover the dialog area, hence "no clip change can
fix this". This inherits `state->clip_zone`, the *narrower* clip the paired fall-through `gfxop_draw_cel`
already uses (trace: `(204,44,242,210)`, `(126,256,58,64)`, `(588,152,24,70)` ... never fullscreen). The
static draw then covers EXACTLY what the normal draw covers, so by construction it cannot write a pixel the
frame does not legitimately show -- and it cannot lose anything either, for the same reason.

**The glovebox coupling does NOT apply here** (this supersedes the 2026-07-19 worry): the items are
`kAddToPic` picviews drawn by `_gfxwop_pic_view_draw`, which already calls `gfxop_draw_cel_static_clipped`
under `view->parent->zone`. The stopUpd dynview route is the ONLY user of the fullscreen override, so
clipping it is decoupled from the items.

Change is one line in `_gfxwop_dyn_view_draw` (`widgets.c`). **Firmware diff verified to be exactly one real
instruction** -- `bl gfxop_draw_cel_static` -> `bl gfxop_draw_cel_static_clipped` -- every other `.text`
difference being a shifted `__LINE__` immediate; `.rodata`/`.data`/`.bss` byte-identical, `.bss` still
17,284. Mapped is untouched (`PICO_WORKING_PRIORITY` compiles the block out); desktop untouched.

**THE REAL FIX -- a SECOND PSRAM surface on PIO. PHASE 1 BUILT (2026-09-11), awaiting device test;
phase 2 (the invalidation trigger) still to wire.** The defect the reverted attempt correctly identified is
that on PIO, STATIC *is* the displayed buffer, so one write serves two incompatible roles -- it must persist
the view (desktop does this in the off-screen `visual[2]`, from which BACK restores composite) AND it must
not paint the live frame out-of-port. No clip setting can satisfy both; the surfaces must be separated.

**This costs essentially NO SRAM.** The confusion to avoid: `PICO_STATIC_VISUAL` (+64KB SRAM) is the MAPPED
implementation, affordable only because the engine moved to PSRAM there. PIO already keeps its static
surface in PSRAM (`static_bg`, the room's `visual_map`) and BACK restores already read it per-row. Phase 1
adds the SECOND PSRAM surface:

| | before | after |
|---|---|---|
| `static_bg` | bake target -- clean plate DESTROYED | stays **PRISTINE** |
| composed (new) | -- | background + baked static views; BACK restores read this |
| undo a bake | impossible (nothing to restore from) | copy rect pristine -> composed |

**Measured cost:** `.bss` 17,284 -> **17,604** (+320 B, exactly `s_compose_row[320]`, the staging buffer a
PSRAM->PSRAM copy needs since the PIO link is store/load only), `.text` +184 B, 64KB of PSRAM out of 8MB,
and **zero heap / zero arena change** -- the fragmentation and OOM picture is untouched.

- **Lazy**: allocated on the FIRST bake, so a room that draws no static view allocates nothing, and BACK
  restores fall back to `static_bg` -- byte-for-byte the old behaviour.
- **Per-room**: `composed_valid` is cleared in `pico_set_static_buffer`, because `psram_reset()` rewinds the
  bump arena on every room change and the old offset would alias the new room's decode.
- **Live in the DEFAULT (priority-only) build**, not just with `PICO_STATIC_VIEW_BAKE`: the bake gate is
  `buffer == GFX_BUFFER_STATIC && !pico_priority_only_static`, and kAddToPic picviews (PQ2 cars, glovebox
  items) never set that flag, so they bake -- and now bake somewhere reversible.
- **Expected side benefit to watch:** the driver comment at `pico_driver.c` already blamed baking-into-the-
  clean-plate for corrupting the add_to_pic overlay base (the SQ3 "Pirates of Pestulon" title). That
  corruption source is now gone. Not a claim -- something to check, ideally with the `[ovl]` probe.

**TRAP recorded: `psram_alloc` is an unchecked bump allocator** (`psram_alloc.c`) -- it never signals
failure, and **offset 0 is a legitimate address**, so the reflexive `if (!addr)` both rejects a valid
allocation and catches nothing. Bound it against `PICO_PARSE_SCRATCH_ADDR` (0x700000) instead.

**PHASE 2a BUILT (2026-09-11), behind `PICO_STATIC_COMPOSED` (default OFF) -- the desktop TWO-DESTINATION
model, which the second surface is what makes expressible.** The insight the reverted clip attempt was
missing: desktop never picks ONE clip for a static view. It uses two destinations, each with its own clip --
`visual[2]` (persistence) takes the FULLSCREEN-clipped draw, `visual[1]` (displayed) takes the port-clipped
one. Both calls ALREADY happen on Pico (`gfxop_draw_cel_static` then the fall-through `gfxop_draw_cel`); the
bug is that Pico collapsed both into `visual[0]`, so one clip had to serve both roles. **That is why there
was no third answer with a single destination: fullscreen bleeds over dialogs, ambient loses the door.**

With `PICO_STATIC_COMPOSED=ON`, the fullscreen static draw of a settled stopUpd dynview goes to the
**composed surface ONLY** and never touches `visual[0]`, so it cannot overpaint a dialog; the view still
reaches the screen via the paired port-clipped draw and via BACK restores, which read composed. Implemented
as a row-at-a-time read-modify-write (the cel may be transparent, so each row is preloaded from composed
before the blit); priority is unchanged -- the same `bake_static_pri` path runs, just per row. The widget
layer marks the one fullscreen call with `pico_static_fullscreen` (mirrors the existing
`pico_priority_only_static` pattern).

**The old objection is measured dead.** `pico_draw_pixmap` carried a comment that static-only draws are
invisible because "the Pico engine path does not reliably issue" update(BACK)+update(FRONT). On the trace
that is false: **a BACK restore covers 100% of static draws in the SAME frame, and a FRONT flush 100%**
(14281/14281). What was missing when that comment was written was not the updates -- it was a static surface
worth restoring FROM, which is exactly what phase 1 added.

**Deliberately NOT applied to kAddToPic picviews.** They use `gfxop_draw_cel_static_CLIPPED` (already
port-clipped, so they never bleed) and they draw exactly ONCE -- `_gfxwop_pic_view_draw` sets
`draw = _gfxwop_draw_nop`. Routing them composed-only would leave them waiting on a later BACK restore to
appear, which is very likely the original "invisible" observation. They keep writing `visual[0]`.

**DEVICE RESULT (2026-09-11): the dialog bleed is FIXED and the door is NOT sacrificed** -- SQ3 "most of
it looks correct", PQ2 dialogs clean, cars and HQ door fine. It also confirmed the predicted gap: with the
colour now persisted and nothing un-persisting it, SQ3's door stayed **CLOSED through its open animation**
and PQ2's picked-up card, glovebox lid and closeup overlay all lingered. That is phase 2b.

Cost: `.text` +376 B, `.bss` +4 B over phase 1 (the flag). Default build byte-unchanged (`pico_static_fullscreen`
is absent from it entirely). **Perf risk to watch, unmeasured:** the RMW does a PSRAM load+store per row per
static draw per frame (~2.5ms for a 100x50 cel); several settled views redrawing each frame could add up.
Watch with `FSCI_PROBE_FPS`/`FSCI_PROBE_PERF`; if it bites, the fix is to skip the write when the view has
not moved or changed cel.

**PHASE 2b BUILT (2026-09-11) -- "re-bake or be erased", the invalidation trigger.** Phase 2a persisted a
stopUpd view's COLOUR, and device-testing showed exactly the predicted consequence: nothing un-persisted it.
SQ3's door stayed **CLOSED through its open animation**; PQ2's picked-up card, glovebox lid and closeup
overlay all lingered. (PQ2 leans heavily on stopUpd -- room 10 disassembles to 16 `stopUpd` vs 3 `addToPic`
-- which is why its glovebox is affected at all, having been assumed picview-only.)

**The rule needs no widget identity, which the driver does not have.** A settled view is redrawn every frame
it is still part of the scene, at the SAME rect. So: track the rects baked into composed, mark each one that
re-bakes, and at the frame boundary erase the ones that did not. One rule covers both failure modes -- a
view that MOVED leaves its old rect unmarked, a view that was DISPOSED leaves its only rect unmarked. Steady
state is free: an unmoved view matches its entry and no PSRAM is touched.

- `composed_mark(dest)` on every composed static draw; `composed_sweep()` from the FRONT flush;
  `composed_forget_all()` in `pico_set_static_buffer` (the old rects referred to the previous room's surface,
  which `psram_reset` has already rewound).
- Sweeping at the FRONT flush means a rect that goes stale is corrected on the NEXT frame's BACK restore --
  one frame of latency, not a persistent ghost.
- Fixed 24-slot table. If it ever fills, the view still renders and merely cannot be un-baked (it may
  ghost) -- never a crash. Far above any observed scene's settled-view count.

**DEVICE RESULT (2026-09-11): fixed PQ2, broke the SQ3 door -- the rule is WRONG and 2c replaces it.**
PQ2 on a fresh boot is a clear net win (dialogs, glovebox items showing AND the card disappearing on pickup,
lid closing, overlay dismissing, cars, HQ door); only a minor "blue box in the car" remains, to be checked
against the historical note that PQ2's car-interior blue box is a pri-3 overlay CORRECTLY occluded and
render-identical to desktop. But SQ3's spaceship door now **does not close**.

**Why the premise is false:** "a settled view is redrawn every frame it is still part of the scene" -- but
`stopUpd` MEANS it stops being redrawn. So a door that settles closed stops drawing, the sweep sees an
unmarked rect and erases it. Combined with the 2a result, both symptoms are one missing distinction --
*settled* vs *gone* -- and **that distinction does not exist in rects**, only in the widget/cast layer.
Two rules have now failed the same way (the ambient clip, and this sweep): each fixed one symptom by
accepting another, because each inferred intent from geometry instead of observing an event.

**PHASE 2c BUILT + DEVICE-VALIDATED (2026-09-11) -- per-widget lifecycle invalidation. `PICO_STATIC_COMPOSED`
is now the DEFAULT for the PIO target.** The 2b sweep is retired. `gfxw_dyn_view_t` gains a `HAVE_PICO`-only
last-baked rect, and three observable EVENTS make a persisted view stale -- none inferred from geometry:

| event | where | fixes |
|---|---|---|
| MOVED (baked rect != rect about to bake) | `_gfxwop_dyn_view_draw` | door/lid animations |
| RESUMED updating (NO_UPDATE cleared) | `_gfxwop_dyn_view_draw` | a view stuck through its own open animation |
| DISPOSED (widget freed) | `_gfxwop_basic_free` | PQ2 picked-up card, dismissed overlay |

**DEVICE RESULT: PQ2 is clean** -- dialogs, glovebox items, card disappearing on pickup, lid closing,
overlay dismissing, cars, HQ door. Only a small blue rectangle in the car around the glovebox overlay
remains, accepted by the user as cosmetic; note the record already carries a PRE-EXISTING PQ2 "blue box in
the car interior" from 2026-06, investigated then and concluded to be a pri-3 overlay CORRECTLY occluded and
render-identical to desktop, so it may not be this work's at all. Confirmed NOT an under-covered
invalidation: baking the driver's exact dest rect instead of `draw_bounds` changed nothing on device and was
reverted rather than kept as unvalidated complexity.

**Why it is the default despite SQ3's door:** the door does not visibly stay closed with this ON -- but it
does not on baseline either. Priority-only never persisted a settled view's COLOUR, so the door has been
like that since priority-only shipped; this is no regression, and PQ2 gains a great deal. Withheld on the
MAPPED target (which has `PICO_STATIC_VISUAL` + real per-frame priority maps instead): the define is
suppressed there even if the option is forced ON, because the widget hooks would otherwise reference
`pico_invalidate_static_region`, which lives behind `!PICO_USE_STATIC_VISUAL`.

**THE DOOR IS STILL OPEN, and it is not a tuning problem -- BOTH invalidation rules fail on the SAME single
case.** 2b's sweep erases a view that stopped being redrawn; 2c's dispose hook erases a view whose widget is
freed -- and SCI frees the widget PRECISELY when a stopUpd view becomes background. PQ2's views survive
both because they are redrawn while present. SQ3's door is the one view that is baked once, never redrawn,
and must persist, and **neither frame timing nor widget lifetime distinguishes that from a stale bake.**
Per-widget state cannot bridge it either: widgets do not survive the settle->animate transition, so the new
widget for the opening animation starts with `pico_has_baked = 0` and cannot undo the old bake.

**Next idea (NOT built): mirror save-under restores into composed.** SCI has its own erase mechanism --
`under_bits`, already in `gfxw_dyn_view_t`. When the engine wants a view gone it restores the region saved
beneath it, and `pico_draw_pixmap` already has a `PICO_HANDLE_GRABBED` path for those restores; today that
writes `visual[0]` but NOT composed, so composed keeps the stale view and every BACK restore paints it back.
Using the engine's explicit "this area reverts to background" signal beats inferring one, and unlike widget
identity it survives across frames. It would mean REMOVING the dispose hook, which is currently what makes
PQ2 correct -- so it is a real A/B with a real risk of regressing PQ2 and must be tested in isolation.

**TRAP recorded -- adding a field to a FreeSCI widget struct.** `_gfxw_new_widget` allocates with
`sci_malloc` and initialises every field BY HAND; its `memset` sits behind `SATISFY_PURIFY`, which is not
defined. A field left out therefore holds heap garbage, silently. Here a garbage `pico_has_baked` made the
first draw invalidate a garbage rect, and `pico_invalidate_static_region` looped over a garbage `yl` doing
PSRAM I/O -- presenting as a TOTAL FREEZE with screen and UART both dead, no fault, no log. Initialise new
fields in the constructor, and bound any loop driven by widget-supplied geometry.

**Also unmeasured:** the first bake in a room triggers a full pristine->composed copy, 64,000 B read +
64,000 B written over a ~4MB/s link, so roughly 32ms plus per-chunk overhead (`psram_store`/`psram_load`
move only 27/31 bytes per transaction). Once per room, on top of a ~100ms decode. Time it with
`FSCI_PROBE_PERF` before assuming it is free, and keep the rule that invalidation copies a RECT, never the
screen.


---

### PSRAM working priority map (`PICO_PSRAM_WORKING_PRIORITY`, PIO, opt-in) — device results 2026-09-29

The desktop/DOS two-map model with BOTH maps in PSRAM, no SRAM: the room's decoded priority map is the STATIC
map (background + `kAddToPic` picviews, which still bake into it), and a WORKING copy lives in a fixed PSRAM slot
at `0x790000` (32,000 B, nibble-packed, above the song slots). Seeded from static whenever a pic or overlay is
set (`pico_priority_seed`), reset per box in `gfxop_clear_box` (`pico_priority_restore_box`, the desktop's
`PRECISE_PRIORITY_MAP` copyback point), and a static picview's box is carried over after it bakes. Moving views
gate against the working map and write their priority into it (`pico_blit_indexed`, the same packed row
read-modify-write as the static bake), so views occlude each other. The routed static draw of a settled stopUpd
view keeps sending its COLOUR to the composed surface but no longer bakes PRIORITY (its normal draw writes it
into the working map each redraw, as on desktop).

The restore's edge-nibble merge was unit-tested against an unpacked reference (2,000 random boxes, 0 mismatches;
removing either edge merge fails it: 840 / 385 bad). `.bss` +168 B with the option on; the option-off build
keeps `.bss` identical.

**This is not what the old "prior attempt" was:** the priority bake ("Change A" above) wrote into the ONE PSRAM
map, permanently; this adds the second, transient map that section's parked notes asked for, in PSRAM instead of
the SRAM they weighed.

Device, first pass:
- **SQ3 room 2: FIXED** -- the door now closes after Roger (the long-standing "SQ3 door not closing" open
  issue), and Roger is still hidden behind the door and motivator.
- **Colonel's Bequest: fingerprints show with `[V]` ON** -- the toggle is no longer needed for it.
- Sprite-over-sprite overlap looks right; animation looks smooth (more testing pending).
- **SQ3 intro "Two Guys" panels: UNCHANGED** -- they still stay up instead of disappearing when the text
  starts. So the old attribution to `PICO_STATIC_VIEW_PRIORITY`'s priority bake was wrong: it is COLOUR
  persistence, most likely the routed stopUpd draw's colour in the composed surface not being invalidated by
  however the panels are taken down. Next test: the same build with `[V]` off (routing off).
- PQ2 (cars, dialogs, glovebox) not yet tested.

**Second pass (2026-09-29) -> DEFAULT ON, `[V]` retired.** PQ2 with the working map and `[V]` on: cars occlude,
dialogs clean, glovebox items visible. With SQ3 and Colonel's Bequest already confirmed, `PICO_PSRAM_WORKING_PRIORITY`
is now default ON for PIO and the chooser's `[V]` toggle is compiled out with it (static-view handling is
always on; the launch log prints `[gfx] PSRAM working priority map ON` instead of the `[V]` state).

**"Two Guys" diagnosis:** the same build with `[V]` OFF makes the panels disappear as they should. So the
persistence is the routed stopUpd draw's COLOUR in the composed surface (`PICO_STATIC_COMPOSED`, widgets.c
phase-2c invalidation): the panels are taken down in a way none of its three stale cases (moved, resumed
updating, disposed) catches. Cosmetic; parked. The fix belongs in that invalidation, not in priority.

### "Two Guys" panels: desktop trace of their lifecycle (2026-09-29; parked, cosmetic)

**Method (reproducible, nothing committed):** a desktop build with `-DFSCI_SIM_PICO_STATIC=1` (mirrors the PIO
stopUpd routing; it needs temporary desktop definitions of `pico_static_view_priority_enabled = 1` and
`pico_priority_only_static = 0` to link) plus temporary `fprintf(stderr, "[vt] ...")` lines in `widgets.c`: every
`_gfxwop_dyn_view_draw` (widget pointer, view/loop/cel, moved draw rect, signal), the routed
`gfxop_draw_cel_static`, and every dyn-view `_gfxwop_basic_free`. Run the SQ3 intro headless for ~200 s
(`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`; the desktop pacing varies, so give it time), then apply the three
phase-2c stale rules (moved / NO_UPDATE cleared / freed) offline to the event stream.

**Findings:**
- The panels are **view 601, loops 0 and 1**: they zoom in (cels 1-8) and settle at **cel 9**, 60x66 px at
  (22,-18) and (238,-18), signal `0x4814` (NO_UPDATE set). Like all dynviews their widgets are **recreated every
  animation cycle** (drawn, then freed right after, often at the same address), so on the Pico a settled panel is
  re-persisted into the composed surface every frame and invalidated by the free that follows.
- **The phase-2c rules are not what fails:** applied to the trace, every persisted view is invalidated within about
  one frame (largest gap 82 trace events), and none is left persisted at the end. Negative `y` is handled correctly
  by `pico_invalidate_static_region` (rows above the screen are skipped).
- **The scene ends in this order:** the last panel draws -> two `kDisplay restore_under` -> `kAnimate: PicNotValid`
  (the picture changes) -> **only then are the panel widgets freed** -> the credit texts are drawn with
  save-unders (`Saving (30,35) size (195,132)`, `(80,35) size (179,144)`, ...), which overlap the panels' bottom
  rows (y 35-48).

**Where that points (untested; needs the Pico driver's own event order):** at the picture change
`pico_set_static_buffer` drops the composed surface (`composed_valid = 0`), so the panels' later frees invalidate
nothing -- harmless by itself, but every restore after that depends on what the driver rebuilds from. Two
candidates: (a) a text save-under captures `visual[0]` while it still shows the panels and a later `restore_under`
pastes them back (fits the original "bands" description: rows 35-48); (b) a BACK restore around the picture change
brings them back from a composed surface that still held them. `[V]` off avoids both because without the routing the
panels never enter the composed surface.

**Next step if revisited:** one device run of the SQ3 intro up to the credits with `FSCI_PROBE_GFX` (`[pstat]` static
buffer swaps, `[pupd]` flush/BACK-restore rects, `[ovl]`), to see which restore brings the panels back.

### Colonel's Bequest windows (2026-09-30): `Graph` was never called -- an engine bug, not the Pico port

Symptoms (PicoCalc and desktop FreeSCI alike): ragged black strips behind each text line instead of a box, no frame,
and the corner ornaments left behind after the window closes.

**Root cause: CB's kernel name table (vocab 999) ends at `TimesCot` (0x6f).** FreeSCI appends its "mystery function"
placeholder `[Unknown]` after the last name, which put it on 0x70 -- the slot of `Graph` in every SCI0 interpreter.
So `Graph` mapped to `kNOP` and every call did nothing: the fill box, the eight frame lines (colour 31), save box,
restore box and redraw box. KQ4 and PQ2 have the same short table (`Kernel function [Unknown][70] unmapped` at
startup); SQ3 names `Graph` and its placeholder sits on 0x71 as intended. Sierra's interpreter dispatches by number,
and ScummVM ignores vocab 999 for SCI0 altogether. Fix: `vocabulary_get_knames0` (`vocab_debug.c`) gives any
`[Unknown]` slot the standard SCI0 name from `sci0_default_knames` when that table has one. Now CB/KQ4/PQ2 report
`Handled 113/113`, SQ3 is unchanged.

How CB draws a window (kernel-call trace, desktop): `SetPort 0`; `Graph` save box, fill box (black) over the frame
rectangle; four `DrawCel` corners (view 657); eight `Graph` lines; `Graph` update box; `NewWindow` flags 0x81
(transparent, script-drawn) with the text controls inside. Closing: `SetPort 0`, `Graph` restore box (which also
frees the corner and line widgets), `Graph` redraw box, `DisposeWindow`. After the fix the desktop render matches a
screenshot of Sierra's interpreter (filled box, double white frame, ornaments on its corners) and nothing is left on
screen after closing. FreeSCI's save box is a widget snapshot (serial number + rectangle), not pixels, so it costs
no SRAM on the Pico. `.bss` unchanged.

**Tracing lessons.** The first trace printed inside `kGraph` and therefore saw no calls at all, which read as "the
script does not use Graph" -- trace at the VM's kernel dispatch (`vm.c`, name + args) instead, which also shows the
name each slot really got. FreeSCI's `kNOP` warning has no trailing newline, so a trace line printed right after it
gets glued onto its line. Harness: restore a device savegame (`freesci --gamedir <cb> --run CB1 save_0`, save from
`~/.freesci/CB1/save_0`) and a temporary SDL-driver hook that types keys and saves screenshots, because CB's intro
needs copy-protection input.

**Also changed: text and edit controls erase their rectangle first** (`_sciw_add_text_to_list`,
`sciw_new_edit_control` in `sci_widgets.c`), as Sierra's interpreter does (ScummVM erases the rect in
`kernelDrawText`); FreeSCI only filled behind each line. Found while chasing the missing box, before the `Graph`
cause; kept because it is the correct behaviour. SQ3's `look` dialog and parser line are pixel-identical to before.

**KQ4 and PQ2 are not affected in practice:** their scripts never call `Graph` (disassembly of all 159 / 98 scripts:
no `callk #Graph`), and restoring their savegames, opening a `look` message and closing it gives pixel-identical
desktop screenshots with the fix on and off. In CB only scripts 000 and 981 (its window code) call it. So the PQ2
dialog/overlay work on the composed surface stands; the fix only changes CB.
