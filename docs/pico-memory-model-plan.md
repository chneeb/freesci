# Pico PIO — memory-model refactor plan

> **Forward plan, not history.** Started 2026-09-27 from a comparison with `~/Source/pico-286`, which runs the
> same SCI0 games on the same PicoCalc hardware. Update the **Status** section as steps land; move finished
> investigation logs to `docs/history/pico-memory-oom.md` as usual.
>
> **Scope: the PIO / PicoCalc target only.** The Pimoroni mapped build (`PICO_PSRAM_MAPPED`) already has the
> desktop memory model and does not need this. The standing rule still applies: nothing here may change the
> mapped build, and every PIO change is an A/B behind an option until proven.

## 1. What pico-286 does, and why it works

Read from `pico-286` `src/emulator/memory.c`, `swap.c`, `src/pico-main.c` (commit `b022efe`):

- **No runtime allocation.** SRAM use is static, fixed at link time: `SRAM[0x2C000]` (176 KB of guest low
  memory) + `VIDEORAM` (`uint32_t[64K]`, 256 KB) ≈ 432 KB. The emulator core has no `malloc` (the only one
  is `FIL` handles in the network redirector). Fragmentation cannot happen.
- **Address-split guest RAM.** Guest `[0, 176 KB)` is SRAM. `[176 KB, 640 KB)`, UMB and HMA go straight to
  PIO PSRAM through `read86_mp`/`write86_mp` — **one SPI transaction per guest access, no cache.** (The
  SD-backed `swap.c` write-back page cache, 2 KB pages, is the fallback when PSRAM fails to init.) This is
  only possible because the CPU emulator already funnels every guest memory access through `read86`/`write86`.
- **The robustness is Sierra's, not the emulator's.** The guest is Sierra's own SCI0 interpreter, designed
  for 640 KB DOS: a fixed script heap plus a *hunk* of handle-based, lockable, **purgeable and compactable**
  blocks for resources and save-unders. Under pressure it purges LRU resources and reloads them later
  instead of fragmenting. pico-286 just gives it a flat address space.

### Clock and power — pico-286 does NOT need 396 MHz

pico-286 is being run here at a **reduced profile** to save battery, not at its 396 MHz / 1.60 V stock.
Its README documents device-verified profiles, all at 1.30 V (`VREG=15`):

| profile | CPU | core V | PSRAM SPI |
|---|---|---|---|
| Low | 240 MHz | 1.30 V | 60 MHz |
| Medium | 300 MHz | 1.30 V | 75 MHz |
| High | 360 MHz | 1.30 V | 90 MHz |

FreeSCI PIO ships at 133 MHz at the default 1.10 V (`pico_main.c` raises vreg only above 250 MHz).
By V²·f, **core dynamic** power is ~2.5× (Low) to ~3.2× (Medium) FreeSCI's — an **estimate, not a
measurement**, and core power is only part of the board draw (backlight, LCD, PSRAM, SD are shared costs
that do not scale with the core clock). So the battery gap between the two is real but smaller than the
core ratio suggests, and it is **unmeasured**.

**Consequence for this plan:** "FreeSCI saves battery" is not, on its own, a strong enough reason to do a
large refactor. Before step 3 (the expensive one), measure whole-board current draw for both at the settings
actually used (same game, same room, same backlight), and write down what else FreeSCI PIO offers over
pico-286 + Sierra's interpreter (e.g. no DOS disk image, native save files on FAT, text-parser tooling,
headroom for our own fixes). If neither is compelling, stop after step 2.

## 2. Why FreeSCI hits the wall and Sierra does not

Our SRAM heap (466,544 B raw on the current default build; see §3a) is in the same range as what Sierra had free under DOS. The difference is the
allocator discipline, not the byte count:

| | Sierra SCI0 (inside pico-286) | FreeSCI on PIO |
|---|---|---|
| allocation | fixed heap + hunk, sized once | general newlib `malloc` |
| big blocks (resources, decoded gfx) | handles — movable, purgeable | raw pointers held everywhere — pinned |
| under pressure | purge LRU, compact, continue | fragments → `pico_oom_report` halt |
| memory beyond SRAM | one accessor | hand-placed offloads (pic maps, cels, songs, fixed slots) |

Every mitigation in `docs/history/pico-memory-oom.md` (permanent decode scratches, the per-room bump arena,
reboot between games, the `malloc_trim` dead ends) is a workaround for **not being able to move a block**.

## 3. Steps, in order

### Step 0 — measure the movable share (decides everything below)

**One run is enough to decide:** KQ4 with sound ON, the plain fragmentation wall. Run the SQ3 savegame load
(sound ON) only if KQ4 gives no clear answer, or later, to size the hunk for the restore peak. That peak is
mostly transient CFSML/restore state, so it measures something different.

Build (fresh directory; the census costs ~26 KB of heap ceiling, so the OOM hits *earlier* than on a clean
build, but the composition at the failure point is still what we are after):

```bash
rm -rf build-pico-census
cmake -B build-pico-census -DPLATFORM=pico -DPICO_SDK_PATH=~/Source/pico-sdk -DPICO_BOARD=pico2 \
      -DFSCI_PROBE_MEM_CENSUS=ON
cmake --build build-pico-census -j$(nproc)   # -> build-pico-census/src/freesci.uf2
```

On an `sci_malloc` OOM, census builds now print, after the `[OOM]` block (`census_dump_oom`,
`pico_mem_census.c`):

- `[mem] OOM free=… largest~… arena=…`: total free vs the largest allocatable block (found by bisection).
  The gap between them is the fragmentation cost a hunk would recover.
- `[mem] LIVE …`: the size-class histogram of **every** live block, raw `malloc` included.
- `[mem] SITES: file:line=count/bytes`: the top 40 `sci_malloc` sites **by bytes**. Raw-`malloc` blocks
  (e.g. `visual[0]`, decode buffers) show up only in `LIVE`, not here.

The per-room `[mem] BREAKDOWN`/`CENSUS` lines still print at each room transition, which gives the build-up
to the failure.

Classify the live bytes at the failure into:

- **movable**: resource data, decoded pixmaps/cels, songs, save-unders. These could be purged and
  reloaded, or relocated.
- **pinned**: VM heap/stack, script bufs, clone tables, widgets/ports, parser/GNF, driver state.

Decision rule:

- movable is the **majority** of the peak → do step 2 (hunk for movable types only); good payoff without
  touching the VM.
- pinned dominates → the hunk will not save PIO. Record that here, treat the Pimoroni build as the
  "works like desktop" target and PIO as best-effort. Step 3 is the only lever left and needs the
  justification from §1.

### Step 1 — fixed pools for the fixed-size churn (cheap, independent)

pico-286's static-array principle applied to FreeSCI's small, frequent, fixed-size allocations (widgets,
ports, list nodes, clone entries — take the list from the census `SITES` histogram). Each gets a pool sized
at boot. OOM becomes "pool X too small" — measurable, tunable, and it stops small long-lived blocks from
pinning the middle of the arena. Caveat from the ruled-out table: persistent GC hashmap / dirty-rect /
worklist pools were **net-negative** because they raised the restore peak — size pools from measured
high-water marks, and A/B each one.

### Step 2 — Sierra-style hunk for movable data

A handle-based allocator for resource data, decoded pixmaps/cels and songs:

- `h = hunk_alloc(size, class)`; `p = hunk_lock(h)` … `hunk_unlock(h)`; unlocked blocks may be **compacted**
  (slid down to close gaps) or **purged** (LRU, and reloaded from the resource manager on next lock).
- Backing: a fixed SRAM region carved once at boot from the pristine heap (like the existing permanent
  scratches), so it never interleaves with `malloc` blocks. PSRAM can back purged-but-hot blocks as a
  second tier instead of re-reading SD.
- The cost is the lock discipline: FreeSCI holds raw `resource->data` and pixmap pointers across calls.
  Start with one class (decoded view cels are the best candidate — already offloaded, already read through
  a narrow path in `pico_blit_indexed`), prove it, then widen.
- Save-unders go here too — this is the real fix for the open **`old_screen` transition garbage** issue
  (Sierra keeps save-unders in the purgeable hunk; SRAM-resident save-unders were ruled out at 60,800 B).

Must respect the existing invariants: `psram_alloc` offset 0 is valid (never `if (!addr)`), anything surviving
a room change needs a slot outside the bump arena, raw-`malloc` recovery paths stay where callers have them,
and every new free site copies the `priority_is_scratch` / `PICO_IS_DECOMPRESS_SCRATCH` guards.

### Step 3 — one accessor for VM memory + write-back PSRAM page cache (expensive, uncertain)

The pico-286 `read86_mp` idea applied to the SCI VM: route script-memory accesses through accessors so
`script_t.buf` can live in PSRAM behind a small **write-back** page cache (the `swap.c` design, PSRAM instead
of SD). This is **not** the ruled-out read-only script cache — that failed because `script_t.buf` is hot
read-write; a write-back cache handles writes. It is untested, FreeSCI takes raw pointers into script bufs in
many places, and per-access cost at 133 MHz is unknown. Only attempt with the §1 justification in hand, and
measure the hit rate with a desktop model first.

## 3a. Where the 520 KB goes, and a proposed fixed layout

### Today (default PIO build, measured 2026-09-27 from `build-pico/src/freesci.elf`)

The RP2350's "520 KB" is 532,480 B: 512 KB main SRAM (`0x20000000–0x20080000`) plus two 4 KB scratch banks.

| region | bytes | notes |
|---|---:|---|
| SCRATCH_X + SCRATCH_Y | 8,192 | stacks (`__StackTop` = `0x20082000`) |
| `.ram_vector_table` | 272 | |
| `.data` | 34,444 | initialised tables + RAM-resident code. **`arm-none-eabi-size` reports this under `text`, not `data`, so the `.bss` baseline does not show it.** |
| `.bss` (+ 2 KB `.heap` min) | 23,024 (+2,048) | size's "bss 25,344" = `.bss` + `.heap` + vector table |
| **heap** (`__end__` `0x2000e190` → `__HeapLimit` `0x20080000`) | **466,544** | everything `malloc` can ever hand out, including the 2 KB `.heap` section |

The "~475 KB" in older notes is from earlier configs. Out of the 466,544 B, `visual[0]` (64,000) and the
decode scratches (48 KB) are permanent, which leaves ≈ 355 KB for the engine.

The largest `.data` items look like tables that are never written: fonts `gfxfont_5x8_*`/`gfxfont_6x10_*`
(6.6 KB), `kfunct_mappers` (2.7 KB), `con_builtin_font_data` (2 KB), `ascii_tree` (2 KB), `sci_games`,
`standard_options`. That is ~15 KB that could be made `const` and moved to flash. **Unverified**: check that
each is truly read-only before moving it. It is a cheap, independent win that is worth doing before step 1.

### Proposed SRAM layout (sizes marked `?` are placeholders until step 0)

```
 0x20000000 ┌──────────────────────────────────────┐
            │ vectors + .data + .bss  ~58 KB       │ link time (.data 34 KB of it;
            │                                      │ ~15 KB may be const-able)
            ├──────────────────────────────────────┤ ── carved once at boot,
            │ visual[0]               62.5 KB      │    never freed, never
            │  320×200 8bpp, resident              │    interleaved with malloc
            ├──────────────────────────────────────┤
            │ decode scratches        48 KB        │ existing: priority 32 KB
            │                                      │ + decompress 16 KB
            ├──────────────────────────────────────┤
            │ PINNED POOLS            ~150 KB  ?   │ ≈ Sierra's HEAP
            │ ┌ VM stack (0x1000 entries)        ┐ │ fixed-size slabs,
            │ │ script bufs (hot RW VM memory)   │ │ sized from census
            │ │ clone table / object slabs       │ │ high-water marks
            │ │ widget + port slabs              │ │
            │ │ parser / GNF, said scratch       │ │ never moves; OOM = "pool
            │ └ driver state, resource dir      ┘ │ X too small", tunable
            ├──────────────────────────────────────┤
            │ HUNK                    ~165 KB  ?   │ ≈ Sierra's HUNK
            │  handle-based, lock/unlock           │ resource data, decoded
            │  compacts unlocked blocks downward   │ cels/pixmaps, songs,
            │  purges least-recently-used to PSRAM │ save-unders (old_screen)
            │  or SD when full                     │
            ├──────────────────────────────────────┤
            │ general malloc          ~40 KB   ?   │ leftovers: libc, FatFS,
            │                                      │ odd one-off allocations
 0x20080000 ├──────────────────────────────────────┤
            │ SCRATCH_X / SCRATCH_Y   8 KB         │ stacks
 0x20082000 └──────────────────────────────────────┘
```

The three `?` regions share the ≈ 355 KB. If step 0 shows most of the peak is movable, the hunk grows and the
pools shrink. If most of it is pinned, the hunk ends up small and cannot rescue PIO (the stop condition in
step 0).

### How the hunk beats fragmentation

```
 today (general malloc)            hunk after compaction
 ┌────┬──┬─────┬─┬────┬───┐        ┌────┬─────┬────┬───┬──────────┐
 │cel │  │pic  │ │snd │   │        │cel │pic  │snd │L  │  free    │
 │    │fr│     │w│    │fr │   →    │    │     │    │   │ (one     │
 └────┴──┴─────┴─┴────┴───┘        └────┴─────┴────┴───┴  block)  ┘
   free space split into gaps       L = locked, stays put
   a small widget "w" pins the      widgets live in their own slabs,
   gap → a 32 KB request fails      so they can't split the hunk

 handle table:  h → { ptr, size, lock count, least-recently-used tick, source }
                code keeps h, calls lock(h) for a pointer, unlock(h) after
```

### Proposed PSRAM layout (8 MB, PIO)

```
 0x000000 ┌──────────────────────────────┐
          │ per-room bump arena          │ existing: pic maps, cel index
          │  rewound by psram_reset()    │ data (0 is a valid address)
 0x100000 ├──────────────────────────────┤
          │ HUNK TIER 2 (new)            │ purged hunk blocks parked here
          │  handle-owned, survives      │ instead of re-reading SD; a
          │  room changes                │ later lock copies them back
 0x600000 ├──────────────────────────────┤
          │ VM page backing (step 3,     │ only if the VM-accessor and
          │  optional)                   │ write-back page cache is built
 0x700000 ├──────────────────────────────┤
          │ parse scratch   64,000 B     │ existing fixed slot
 0x710000 ├──────────────────────────────┤
          │ song slots      8 × 64 KB    │ existing
 0x790000 ├──────────────────────────────┤
          │ save-under slot(s) (new)     │ fixes old_screen: outside the
          │                              │ arena, so a room change can't
          │                              │ wipe them
 0x800000 └──────────────────────────────┘
```

Two things are not measured yet: the bump arena's real high-water mark (measure it before fixing the tier-2
start at `0x100000`), and the pinned-pool sizes (listed by category, not by size).

## 4. Constraints (from `CLAUDE.md`)

- PIO `.bss` baseline **25,344** (sound ON). Any change to it must be deliberate and recorded here; check
  with `arm-none-eabi-size build-pico/src/freesci.elf` after every shared-file change.
- Mapped build untouched: everything here is `#if !defined(PICO_PSRAM_MAPPED)` or behind a PIO option.
- Judge viability on a **clean build** — probes cost ~26 KB of heap ceiling and manufacture OOMs.
- Fresh-configure the build dir after any option default change (CMake cache trap).
- Invariants that bound this work: `VM_STACK_SIZE` 0x1000, `GC_INTERVAL` 2048, never `run_gc` inside an
  allocation, OOMs legible never HardFault, no stack frame near 8 KB, `script_t.buf` never serialized.

## 5. Status

| step | state | notes |
|---|---|---|
| 0 measure movable share | ready to run | OOM-time dump added 2026-09-27; KQ4 + sound first |
| 0a const-ify `.data` tables (~15 KB) | not started | independent, cheap; verify each table is read-only |
| 0b board-current comparison vs pico-286 (Low/Medium) | not started | needed before step 3 |
| 1 fixed pools | not started | |
| 2 hunk | not started | first class: view cels |
| 3 VM accessor + page cache | not started | conditional on §1 |
