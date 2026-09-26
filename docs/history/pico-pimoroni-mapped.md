# Pimoroni Pico Plus 2 (memory-mapped PSRAM) — details

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 3397-3521 -->

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

Audio knobs (any Pico target, only meaningful with `PICO_PWM_AUDIO`):

| option | effect |
|---|---|
| `PICO_PWM_AUDIO` | **defaults ON for mapped, OFF for PIO** — where it actually works |
| `PICO_SND_RATE` (22050) | sample rate. Use **11025 on PIO**: memory differs by only ~4KB, but 22050 costs the synth ~35-47% CPU vs ~20%, lowering the frame rate AND hence the poll rate while simultaneously requiring double it |
| `PICO_SND_BUF_FRAMES` (rate/11) | frames the mixer may emit per call. Must exceed `rate / poll_rate` or the ring starves; every 512 frames costs ~4KB of compbuf, so it is the main audio memory knob |
| `PICO_SOFTSEQ` ("") | softseq name: empty = opl2 (Adlib FM). `SN76496` produces SILENCE on SQ3 (no Tandy/PCjr track in the resource) — not a drop-in |

`PICO_PACK_VOCAB` now defaults **ON** for all Pico builds — it is the shipping config, and an unpacked vocab
(1843 separate allocations on PQ2) fragments SRAM badly enough to fail the 64 KB `visual[0]` on a cross-game
switch. Building it OFF caused that failure twice.

<!-- from CLAUDE.md lines 5049-5055 -->

#### Open on this target

- **KQ4: Rosella "swims in the lawn"** in the opening. Suspected **control map**: `state->control_map` is NULL
  on Pico, so collision falls back to the pic's nibble-packed PSRAM map, and KQ4's opening picks swim-vs-walk
  from control colours. The fix is likely the same pattern as priority — give `control_map` a real 64 KB SRAM
  buffer and unpack the pic's control map into it (~119 KB free, so it fits). NEXT UP.
- SDK is pico-sdk **2.2.0** (no turnkey PSRAM — hence the vendored QMI init).

