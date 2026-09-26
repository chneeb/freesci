# Pico PIO — memory, OOM, fragmentation and restore history

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 200-233 -->

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

<!-- from CLAUDE.md lines 749-2245 -->

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

### MEASURED + FIX BUILT, awaiting device test (2026-09-12) — the cross-game floor is `malloc_trim`'s top-chunk limit, not a leak

**The `[mem] post-trim` measurement settled it, and the answer was none of the three cases predicted below.**
SQ3 -> chooser, `FSCI_PROBE_MEM` build:

```
[mem] after chooser: free=3720   arena=3724   used=4        <- cold boot, first ever
[mem] post-trim:     free=145056 arena=167564 used=22508    <- after quitting SQ3
```

**The trim WORKS** (arena 413,324 -> 167,564) -- so "the trim released nothing" is dead, as is the vocab
hypothesis. What it could not do is get past **22,508 bytes that survive the game exit** where a cold boot
has 4.

**Two lazy "allocate once, never free" scratches were the bulk**, both correct within a game and pure
inherited ballast across the chooser:

| | bytes | why it was never freed |
|---|---|---|
| `decrypt1` LZW token tables (`decompress0.c`) | 16,384 | hoisted off the stack to fix the 2026-06-08 decrypt1 HardFault; "game-independent scratch" |
| `said_tree` + `said_tokens` (`said.c`) | 4,512 | same reasoning; only live once the player actually types a parser command |

Both now have `pico_reset_decrypt_scratch()` / `pico_reset_said_scratch()` called from the chooser reset
(`said.y` patched in lockstep with the generated `said.c`). Device-measured result: `used` 22,508 -> **6,108**
-- i.e. exactly the decrypt1 block came off. said's 4,512 did NOT, because that session typed no parser
command, so it was never allocated; the reset is still correct, just unexercised.

**THE LOAD-BEARING FINDING -- freeing bytes moved the floor the WRONG WAY.** Same session, after the fix:

```
[mem] post-trim: free=386736 arena=392844 used=6108
```

`used` fell by 16,400 and the arena floor ROSE 167,564 -> **392,844**. `malloc_trim` can only release the
**top** free chunk, so the arena floor is set by the single **highest-addressed survivor**, whatever its
size. decrypt1's block had been that top block; removing it merely exposed a ~6KB residual sitting higher
still. **A 6KB residual in an unlucky spot costs just as much floor as a 22KB one.** Chasing residuals down
one at a time is whack-a-mole and cannot converge -- which retires the "find the leftover allocations"
framing entirely.

**FIX BUILT (default ON, awaiting device test): `PICO_REBOOT_BETWEEN_GAMES`.** After a game exits, reboot
into the chooser (`watchdog_reboot`) instead of looping back to it in-process. The destination is unchanged
-- that loop was headed to the chooser anyway -- and the next game starts on a genuinely cold heap **by
construction**, with no dependence on where the allocator happened to place anything. This is precisely the
power-cycle workaround, automated. Cost: an SD remount + chooser redraw. Safe on both targets (PIO rebuilds
its PIO state machine; the mapped QMI init already exits QPI first *because* a watchdog reset does not
power-cycle the PSRAM). Escape hatch: `-DPICO_REBOOT_BETWEEN_GAMES=OFF` restores the in-process trim path,
which is kept intact and is still correct as far as it goes.

**The scratch resets are kept regardless** -- they are right in their own right, and they are what the OFF
path depends on.

**Census instrumentation, if the residual ever needs naming anyway:** `census_dump_sites()` is now also
called at the chooser reset, and prints a `[mem] LIVE <bytes> in <n> blocks:` bucket histogram (covers
EVERY live block, including raw mallocs and anything outside the SITES window) before the `[mem] SITES:`
line. The SITES window was retargeted `[32,128)` -> `[128, 1<<24)`: the old window was aimed at the
clone-variables hunt and misses a few-KB cross-game residual entirely. **Do not drop `SITE_LO` to 0** --
the 8-byte blocks alone run ~1900 live in a room and would blow `CENSUS_NPTRS`.

---

*Historical (the OPEN framing this replaced -- its three-case prediction was wrong, but the A/B that proved
the failure is cross-game still stands):*

### OPEN (2026-09-12) — cross-game switch STILL leaves PQ2 short, despite the chooser reset

**Confirmed cross-game, by the cheapest possible A/B: PQ2 cold-boots fine, but OOMs after a KQ4 session.**
Quit KQ4 -> start PQ2 -> leave the car:

```
[OOM] malloc  size=0x1c76 (7,286)  free=0x14238 (82,488)  arena=0x73e90 (474,768)
decompress0.c line=370   <- the compressed INPUT buffer
```

**Fragmentation, not exhaustion**: free is 11x the request, and the arena sits 336 bytes below the 475,104
ceiling. The same build cold-boots PQ2 through that scene, so this is inherited state, not an in-game wall.

**The chooser reset is intact and correctly ordered** (`pico_main.c`): `pico_reset_resident_vocab()` ->
`pico_reset_decode_scratches()` -> `malloc_trim(0)` -> `MEMPRINT("post-trim")`. The vocab blob IS released
before the trim, so that hypothesis is dead. `malloc_trim` can only release the TOP free chunk, so the
working theory is that something long-lived still sits high in the heap and blocks it -- but that is
UNVERIFIED.

**Next step is measurement, not more guessing: read `[mem] post-trim`** (build with `-DFSCI_PROBE_MEM=ON`;
`build-pico-mem` exists). It prints `free`/`arena`/`used` right after the trim and separates three cases:
`arena` still ~474k = the trim released nothing; `arena` low but `used` large = genuine leftover
allocations; `arena` low AND `used` small = the reset is clean and PQ2 merely sits close enough to the edge
that any change in starting conditions tips it. Also worth diffing PQ2's first-room `[mem]` line cold-boot
vs post-KQ4, which quantifies what is actually inherited.

**Workaround meanwhile: power-cycle between games.** Cold boot works reliably.

**Ruled out along the way:** `GC_INTERVAL` (the cold-boot test exonerates it for this failure -- but note it
remains an UNTESTED speculative change whose own target, PQ2's clone-table OOM at copy protection, has not
recurred to confirm it helps; the code comment records that it trades clone-table growth against GC-churn
fragmentation).

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

<!-- from CLAUDE.md lines 3328-3348 -->

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

