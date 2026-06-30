# Pico SRAM/PSRAM review notes

Date: 2026-06-11

Scope: quick static review of the current PicoCalc SRAM/PSRAM memory strategy, using `CLAUDE.md` as context. I did not change source code. The checkout already had a local edit in `src/engine/game.c` and generated/log/build artifacts.

## Priority findings

### 1. Current `visual[0]` reuse likely moves pressure back onto decompression

`gfxop_new_pic()` now borrows the resident 64 KB display buffer before `gfxr_get_pic()`:

- `src/gfx/operations.c:2294-2308` explicitly calls this a tradeoff/experiment.
- `src/gfx/operations.c:2310-2313` sets `g_pico_decode_visual_buf` to `pico_get_visual()`.
- `src/gfx/resource/sci_resmgr.c:135-142` only stores the already-decompressed pic resource into PSRAM after `scir_find_resource()` has returned.
- `src/scicore/decompress0.c:323-324` still allocates the compressed buffer and the decompressed `result->data` in SRAM before the Pico pic code can evict/offload it.

This fixes the later 64 KB decode-visual allocation, but it means the 64 KB display buffer is resident during the resource decompression peak. That matches the recent notes where the visible wall moved from pic decode to `decompress0.c:324` allocations of about 11 KB after restores.

Suggested next experiment: add a Pico build switch for an A/B run:

- mode A: current `visual[0]` reuse.
- mode B: free/borrow `visual[0]` before `gfxr_get_pic()` so decompression gets the 64 KB hole back, then allocate the decode visual only after `res->data` has been copied to PSRAM and evicted.

If mode B fixes `decompress0.c:324` but reintroduces a visual decode alloc failure, the right fix is probably a staged room-load path: visual freed for decompression, priority scratch permanent, and visual allocated/borrowed only after resource data is offloaded.

### 2. Last-ditch allocator reclaim currently does not run GC

The local checkout has `run_gc(s)` commented out in `pico_reclaim_heap()`:

- `src/engine/game.c:855-863` still documents LRU flush plus GC.
- `src/engine/game.c:872-875` only flushes LRU, then skips GC.
- `git diff` shows this is an uncommitted local change.

This may be intentional because of GC-time fault risk, but it changes the allocator recovery model in `CLAUDE.md`: on a fragmented heap with `reslru=0`, reclaim is basically a no-op. It also means disposed clones/lists/nodes wait for the normal `GC_INTERVAL`, which can matter during death scenes and restore churn.

Suggested next step: either restore GC after validating the current corruption fixes, or make this explicit and move GC to safer known points, such as before room transitions or before restore replay, rather than from inside an allocation failure.

### 3. Audio costs SRAM even though FreeSCI sound is disabled

`CLAUDE.md` says Pico runs FreeSCI with `--no-sound`, but Pico still initializes the PWM synth:

- `src/platform/pico/pico_main.c:162-164` calls `pwm_synth_init(26)`.
- `src/platform/pico/audio/pwm_synth.c:6-18` includes the waveform table and channel globals.
- `src/platform/pico/audio/pwm_strings.h:9` defines mutable `signed char strings[6924]`.
- `arm-none-eabi-nm` on `build-pico-off/src/freesci.elf` shows `strings` at `0x20007fbc`, size `0x1b0c` = 6924 bytes.

This is a cheap SRAM win. If sound remains disabled, do not compile/link/init `pwm_synth.c`. If the table is needed later, make it `static const` so it can live in flash instead of SRAM. That recovers about 6.8 KB, plus avoids the always-running PWM IRQ.

### 4. `decompress0` allocation order can make fragmentation failures worse

SCI0 decompression currently allocates the compressed input first, then the output:

- `src/scicore/decompress0.c:323` allocates `buffer = sci_malloc(compressedLength)`.
- `src/scicore/decompress0.c:324` allocates `result->data = sci_malloc(result->size)`.

On Pico, `sci_malloc` halts on failure, so if the second allocation fails there is no opportunity to free `buffer` and retry/reclaim. In a fragmented post-restore heap, the first allocation may consume the last hole large enough for the second allocation.

Suggested next step: test allocating `result->data` first, then `buffer`, at least under `HAVE_PICO`. A stronger version is a Pico decompression wrapper that uses raw `malloc`, frees any partial allocation on failure, runs `pico_reclaim_heap()`, and retries in a controlled order before calling `pico_oom_report`.

### 5. `scir_evict_resource_data()` can evict locked resources without accounting

The helper is currently unconditional:

- `src/scicore/resource.c:932-941` frees `res->data`, sets `status = SCI_STATUS_NOMALLOC`, but does not check `SCI_STATUS_LOCKED`, clear `lockers`, or update `memory_locked`.
- `src/gfx/operations.c:2277-2280` calls it on `old_res` returned by `scir_find_resource(..., lock=0)`. If that resource was already locked by another path, `lock=0` does not unlock it.

Current Pico callers may only intend to evict unlocked pic/view data, but the helper is footgun-shaped. A locked-resource eviction would leave stale memory accounting and could invalidate a caller that believed its lock preserved `res->data`.

Suggested next step: make the helper refuse locked resources with a warning/assert, or add a separate explicitly named force-evict helper that fixes `memory_locked`/`lockers` and is only used where lifetime is proven.

### 6. PSRAM bump allocator has no bounds or reserved-region protection

`psram_alloc()` is a raw bump pointer:

- `src/platform/pico/psram_alloc.c:10-17` increments without capacity checks.
- `src/gfx/drivers/pico_driver.c:98-126` uses fixed `PICO_PARSE_SCRATCH_ADDR = 0x700000` for 64 KB visual borrow.

The comments say the room bump arena stays well under 1 MB, so this is not likely today. But if more resources are pushed into PSRAM, the allocator can eventually collide with the fixed parse scratch or wrap past 8 MB with no diagnostic.

Suggested next step: define PSRAM size and reserved top region in `psram_alloc.c`, add an overflow check, and expose the high-water mark in the memory probes. That will make future PSRAM offloads safer.

## Lower-priority observations

- The current Pico ELF reports `text=796836 data=0 bss=29792` via `arm-none-eabi-size`. Some large SRAM allocations are runtime heap, not visible in `.bss`: `visual[0]` 64 KB, permanent priority scratch 32 KB, `decrypt1` scratch 16 KB, VM stack 16 KB, packed vocab about 29 KB, and resource directory about 25 KB.
- The permanent priority scratch is still a good tradeoff: it removes a fragmented 32 KB allocation from room decode. I would not move it to PSRAM because pic decode writes it randomly.
- The `decrypt1` scratch also should stay in SRAM unless rewritten; it is hot random access and fixed a stack/heap collision. Do not move it to slow SPI PSRAM as a quick hack.
- The resource directory and packed vocab remain possible SRAM targets only if there is a cached access layer. `CLAUDE.md` already explains why moving them blindly to PSRAM is low value.

## Suggested next work order

1. Reclaim the easy ~6.8 KB by disabling PWM audio or making `strings[]` `static const`.
2. A/B the `visual[0]` reuse path against the current `decompress0.c:324` failure.
3. Change SCI0 decompression allocation order or add a Pico-specific controlled retry.
4. Decide whether GC should be restored in `pico_reclaim_heap()` or moved to safer pre-peak points.
5. Add PSRAM bounds/high-water diagnostics before pushing more data into PSRAM.
