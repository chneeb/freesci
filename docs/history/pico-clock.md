# Pico — clock, SD and overclocking history

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 3731-3864 -->

### DONE (device-confirmed 2026-09-18) — PIO runs at 396 MHz: decode 1.6-1.9x faster

**Device-measured on the PicoCalc, `[clk] sys_clk = 396000000 Hz` with no fallback:**

| | 133 MHz | 396 MHz | |
|---|---|---|---|
| pic 777 decode | 82.1 ms | **43.6 ms** | 1.88x |
| pic 900 decode | 79.4 ms | **50.2 ms** | 1.58x |
| pic 1 decode | 134.5 ms | **80.9 ms** | 1.66x |
| resource load | 15,006 ms | **11,672 ms** | 1.29x |

Not the full 3x because decode is partly PSRAM-bound and PSRAM only went 66.5 -> 99 MHz SPI (1.49x).

**CORRECTION TO THIS FILE: `[perf] resource load` is NOT purely SD-bound.** The timing-probe table says it is
"the part the CORE CLOCK CANNOT speed up (it is SD-clock bound)". Measured: 15,006 -> 11,672 ms at the same
SD rate, so the map parse has a real CPU component. The claim was never tested before; it is now.

**FOUR pieces of bring-up, all required, none of them the PSRAM divisor we spent 2026-09-10 tuning:**
1. **Flash timing recomputed BEFORE raising the clock** (`pico_set_flash_timings`, `psram_alloc.c`,
   `__no_inline_not_in_flash_func` so it does not execute from the flash it is reprogramming). QMI divides
   clk_sys for the flash, so at 252 MHz undivided the XIP reads were corrupt -- dead before serial, TFT
   noise. **This is what "RULED OUT (do not retry as-is)" actually was**, and the old note half-spotted it.
   Capped at `PICO_FLASH_MAX_MHZ` = 100, pico-286's device-proven `F100` value.
2. **`vreg_set_voltage(VREG_VOLTAGE_1_30)`** before `set_sys_clock_khz`, gated `> 250 MHz`. An under-volted
   core that DOES configure fails as random corruption, which is far harder to read than a refusal.
3. **PSRAM sampling phase: target an SM CLOCK, do NOT hold the SPI rate.** `PICO_PSRAM_SM_MHZ` (default 133;
   198 at 396 MHz -> clkdiv 2 -> SPI 99 MHz, pico-286's soak-tested PicoCalc point).
4. **PIN `clk_peri`** (`pico_main.c`). The SDK ties it to clk_sys UNDIVIDED -- *"CLK PERI = clk_sys. Used as
   reference clock for UART and SPI serial"* -- so 396 MHz also clocked the UART and BOTH SPI peripherals
   (SD card AND LCD) at 396 MHz. Device symptom: serial died right after the `[clk]` print, then SQ3
   HardFaulted on launch, which is what corrupt SD reads feeding the resource loader look like. Pinned at
   133 MHz so every existing SD/LCD divisor stays valid.

**TWO WRONG GUESSES ON THE WAY, recorded so they are not repeated:**
- **"Hold the SPI rate constant across the clock change."** Wrong: the PIO input synchronizer is clocked by
  **clk_sys**, not the SM clock, so its 2-cycle latency is ~15ns at 133 MHz and ~5ns at 396 -- MISO arrives
  ~10ns earlier against the sampling edge, most of a bit period at 66 MHz SPI. **The sampling phase moves
  with the system clock even at a fixed SPI rate.** This is exactly what pico-286 means by "fails at both
  faster AND slower settings", and why they ship a sweep.
- **Deriving the PSRAM divisor from `PICO_SYS_CLOCK_MHZ` (the REQUESTED clock).** If `set_sys_clock_khz`
  falls back, a divisor computed for 396 lands on a 133 MHz clock and puts the SPI at 22 MHz -- a dead bus
  that looks identical to an overclock failure. Now derived from `clock_get_hz(clk_sys)`, and printed:
  `[psram] PIO clkdiv N from achieved N MHz -> SPI N MHz`. This is the trap the `[clk]` line already existed
  to expose ("trust this, not CMakeCache").

**WAS the PIO default (2026-09-18, after play-testing: "feels much snappier"); REVERTED to 133 MHz on
2026-09-19 because the higher clock eats battery on a handheld.** Nothing here is retracted -- 396 works,
the measurements below stand, and it is one flag away (`-DPICO_SYS_CLOCK_MHZ=396`, which drags
`PICO_PSRAM_SM_MHZ` to 198 by itself). It is a POWER trade, and the soak caution below (one board, a
board-specific sampling phase, silent corruption as the failure mode) is a second reason to keep it opt-in.
**Do not read the 133 default as evidence that 396 is broken** -- that is exactly the wrong conclusion the
old "PIO stays at 133" note led to.

**`PICO_PSRAM_SM_MHZ` IS PAIRED TO THE CLOCK IN CMAKE, never defaulted independently** -- at 396 MHz a target
of 133 gives clkdiv 3 -> SPI 66 MHz, which DEVICE-FAILED the smoke test. `-DPICO_SYS_CLOCK_MHZ=133` drags the
SM target back to 133 by itself, so the fallback is one flag.

**TWO CMAKE TRAPS HIT WHILE LANDING THIS, both nearly shipped:**
1. **Ordering**: the `PICO_PSRAM_SM_MHZ` block sat EARLIER in CMakeLists.txt than `PICO_SYS_CLOCK_MHZ`, so
   `if(PICO_SYS_CLOCK_MHZ GREATER_EQUAL 300)` tested an UNSET variable and silently produced 396/133 -- the
   failing pair. A dependent default must come after what it depends on.
2. **The stale cache** (the trap already documented at the top of this file): after changing the default, the
   EXISTING `build-pico` still held 396/133 and BUILT CLEAN. Nothing in the compile output hinted at it; only
   reading `CMakeCache.txt` caught it. `rm -rf` the dir after a default changes -- and check the options you
   care about, not just that it compiled.

*Historical caution, overruled:* **NOT the default (`PICO_SYS_CLOCK_MHZ` stays 133) pending a soak.** Tested on ONE board for one short
session. The PSRAM sampling phase is board-specific -- that is why pico-286 ships a sweep rather than a
constant -- and a MARGINAL phase fails as silent data corruption, not a clean halt. This file documents how
expensive that class of bug is to chase. Soak it (an hour of play across games, watching for `[OOM]`, faults
and graphical garbage) before flipping the default, and consider the battery/heat cost on a handheld.

**Still open, unchanged by the clock:** loading a savegame in SQ3 **with sound ON** OOMs at
`pico_decompress_alloc` (`malloc 9,412, free 14,568, arena 462,400`) -- fragmentation with the arena 12 KB
from the ceiling. Memory is identical at any clock; sound costs ~18 KB and SQ3 runs close to the edge.
Workaround: `[S]` off to load. Also noticed: `pico_oom_report` printed `lipico_decompress_alloc`, i.e. the
`line=` field ran into the filename -- a format/buffer bug in the dump, harmless here but misleading later.

*Historical (the roadmap entry that led here):*
### ROADMAP (2026-09-18) — raise the PIO system clock to ~396 MHz: the largest remaining PIO win, and the blocker is FLASH, not PSRAM

**`~/Source/pico-286` runs 396 MHz on the SAME PicoCalc with the SAME vendored Ian Scott PIO-SPI PSRAM
driver.** So 3x our 133 MHz is demonstrably achievable on this hardware; we are not clock-limited, we are
missing three pieces of bring-up.

**THIS RETIRES the "RULED OUT (do not retry as-is)" verdict in the overclocking section.** That conclusion was
reached after guessing two PSRAM clkdivs (scaled 1.895, then integer 2) and watching the smoke test fail both
times -- with flash timings never in the picture. It was the wrong knob. The same section already half-spotted
this: *"psram_set_flash_timings() is `#ifdef PICO_PSRAM_MAPPED`, so PIO's flash timing is never adjusted at
all, and that may itself be the (or a) cause."* pico-286 confirms it is.

**The three missing pieces, with a working reference for each:**

1. **Flash timing recomputed for the clock, executed FROM RAM** (`pico-286 src/pico-main.c`,
   `void __not_in_flash() flash_timings()`): divisor = ceil(clock / max_flash_freq), forced to >=2 above
   100 MHz, plus an extra `rxdelay` cycle above 100 MHz, written to `qmi_hw->m[0].timing`. Their build is
   named `F100` -- flash held at 100 MHz under a 396 MHz core. **We do none of this on PIO**, so at 252 MHz
   the XIP reads were almost certainly corrupt: dead-before-serial with TFT noise is what bad instruction
   fetch looks like, and we misread it as a PSRAM fault.
2. **Core voltage** -- `hardware/vreg.h`, `vreg_set_voltage()` before `set_sys_clock_hz()`, with a failsafe
   retry (`set_sys_clock_hz(hz, 1)`) if the first attempt is refused. We never raise vreg.
3. **The PSRAM sampling phase found by SWEEP, not guessed.** They ship `PICOCALC_PSRAM_SWEEP` (walks
   divisor x fudge, prints SPI rate / errors / throughput, does not boot) and `PICOCALC_PSRAM_SOAK` (hammers
   the chosen point as the board warms). Their note is exactly our failure mode: *"Reliability is a
   sampling-phase problem that fails at both faster AND slower settings, so re-derive it with
   PICOCALC_PSRAM_SWEEP rather than guessing."* That is why our scaled-DOWN divisor did not help either.
   There is also `PSRAM_FUDGE_VAL`, an extra read-sync cycle REQUIRED above 83 MHz SPI and wrong below it --
   a knob our driver has no equivalent for, so any naive clock raise lands on the wrong side of it.

**Their operating point: `PSRAM_SM_CLOCK_VAL=198000000` -> SPI 99 MHz, soak-tested 0 errors, ~5.0 MB/s.**

**PAYOFF -- speed and audio quality, against OUR measured numbers:**

| measured at 133 MHz | CPU-bound? | expected at ~396 MHz |
|---|---|---|
| pic decode 82-170 ms | YES -- the XIP-RAM test showed 6.5-11x slowdown with the flash cache off, i.e. heavily instruction-fetch bound | ~30-60 ms |
| frame rate ~25 Hz ordinary | partly | higher -> **the remaining transition starvation goes away**, and `PICO_PWM_BUF_FRAMES` could drop back toward rate/30, returning ~5 KB of heap |
| OPL synth ~20% CPU at 11025 | yes | ~7% -> **22050 Hz becomes affordable on PIO for the first time** |
| transition stalls 150-250 ms | yes | proportionally shorter |
| **resource load 15 s** | **NO -- SD-clock bound** | **UNCHANGED** (the record is explicit that the core clock cannot touch this) |

**WHAT IT DOES NOT BUY: anything memory-bound, which is every hard limit left.** KQ4 with sound, Colonel's
Bequest exhaustion, the transient working priority map, inter-sprite z-order -- all SRAM capacity and
fragmentation. MHz adds no bytes. Nor does faster PSRAM make anything new offloadable: the flood-fill aux and
`script_t.buf` were ruled out for RANDOM ACCESS, and at 5 MB/s with 31-byte transactions random access is
still hopeless. Memory-MAPPED PSRAM (the Pimoroni target) remains the only thing that dissolves those.

**Note the PSRAM bandwidth itself is NOT the prize:** 5.0 MB/s vs our ~4.0 is ~25%, and 1.5x the SPI rate
(99 vs 66.5 MHz) yielding only 1.25x throughput says the driver is transaction-overhead-bound -- consistent
with our 31-byte reads / 27-byte writes. The CPU clock is the win; PSRAM tuning is the ENABLER for it.

**Also to check before trusting it:** SD and LCD SPI rates are requested in Hz and derived from the system
clock, so they should hold -- but verify on device. And 3x clock on a battery handheld costs power and heat.

<!-- from CLAUDE.md lines 4119-4167 -->

#### Overclocking and SD speed (device-tested 2026-09-10)

Prompted by `PICO_PERFORMANCE_VS_FRANK_QUEST.md` (frank-quest runs the same hardware noticeably faster).
Resource loading is SD read + decompression + decode: the first is SD-clock-bound, the rest CPU-bound.
**Display bandwidth is a SEPARATE factor and does nothing for load times.**

| knob | mapped (Pimoroni) | PIO (PicoCalc) |
|---|---|---|
| `PICO_SYS_CLOCK_MHZ` | **252** (default) — device-confirmed | **133** — 252 FAILS, see below |
| `PICO_SD_SPI_KHZ` | **30000** (default) | **30000** (default) |

- **SD 12500 -> 30000 kHz.** frank-quest runs 30 MHz on the same class of card with NO DMA; we have DMA.
  Safe by construction: init negotiates at 400 kHz and only then switches, so a card that cannot sustain
  it fails visibly at mount rather than corrupting data. This is the ONLY loading lever the core clock
  does not touch.
- **252 MHz on mapped: works.** Memory-mapped QMI PSRAM re-derives its divisor from `clock_get_hz(clk_sys)`
  (`psram_mapped.c`), so it re-caps itself at any clock. `psram_set_flash_timings()` must be passed the NEW
  target -- it is parameterised by the system clock, and getting that wrong corrupts XIP (dead before
  serial, TFT noise). Note PSRAM does NOT get faster, so expect sub-linear gains on this target: scripts,
  objects and the resource cache all live there.
- **252 MHz on PIO: the RULED-OUT verdict below is SUPERSEDED (2026-09-18)** -- see the 396 MHz roadmap
  entry. `~/Source/pico-286` runs 396 MHz on this same hardware with this same PIO PSRAM driver; what we were
  missing is flash-timing adjustment (never done on PIO), `vreg`, and a SWEPT rather than guessed PSRAM
  sampling phase. We tuned the PSRAM divisor twice, which was the wrong knob.
- **252 MHz on PIO: RULED OUT (do not retry as-is).** The bit-banged PIO-SPI PSRAM fails the boot smoke
  test (`[psram] FAIL: pattern mismatch`). Tried and failed: (a) scaling the clkdiv to hold the SPI at its
  133 MHz-equivalent rate (252/133 = 1.895), and (b) an INTEGER divisor of 2 (126 MHz) to remove PIO
  fractional-divider jitter. The driver header's "clkdiv >1.0 needed above 280 MHz" is **RP2040** guidance
  and does NOT transfer to RP2350 + this PCB/PSRAM -- do not quote it as licence to overclock.
  Still unaddressed if anyone retries: `psram_set_flash_timings()` is `#ifdef PICO_PSRAM_MAPPED`, so PIO's
  flash timing is never adjusted at all, and that may itself be the (or a) cause.
  Payoff is capped anyway: PSRAM would stay pinned at its proven rate, and PIO's render path is heavily
  PSRAM-bound (per-row priority readback, ~96KB offload per room), so a CPU-only gain is diluted.
  **The clkdiv is now derived from the clock regardless** (`pico_main.c`), computing to exactly 1 at 133,
  so the default build is unchanged.

**BUILD-SYSTEM TRAP that cost a device cycle.** These knobs were first added INSIDE the
`if(PICO_PSRAM_MAPPED)` branch that selects the sound default, so on PIO they never executed and the C-side
`#ifndef` fallbacks silently supplied 133/12500 -- while `CMakeCache.txt` reported 252/30000. A build that
failed with "PICO_SD_SPI_KHZ undeclared" was the real warning, and adding the `#ifndef` fallback turned that
loud error into a silent wrong value. The `[clk] sys_clk = ... (requested N MHz)` line printed at the
launch point (after the chooser, so it is reachable with uf2loader attached) exists to catch this: it
reports the ACHIEVED clock and flags a fallback explicitly. **Trust that line, not CMakeCache.**

**16-bit display (`PICO_LCD_16BIT`) is OFF and INCOMPLETE** -- see its CMake comment. The panel accepts
`0x65` fine; the problem is that only `flush_region` was converted, while `pico_clear_screen_black` and
~66 write sites in `lcdspi.c` (chooser, text, the OOM/HardFault dumps) still push 3 bytes/pixel. Device
result: sheared display. Completing it across all writers is worth ~2x display bandwidth.


---

### 360 MHz on PIO, pico-286 "High" profile (device-tested once, 2026-09-28; opt-in)

`-DPICO_SYS_CLOCK_MHZ=360`, fresh build dir, no code change: the existing bring-up derives exactly pico-286's
High profile -- 1.30 V raised before the clock (automatic above 250 MHz), PSRAM divider round(360/198) = 2 ->
90 MHz SPI with the extra read cycle on (pico-286: `PSRAM_SPI=90 PSRAM_FUDGE=1`), flash ceil(360/100) = 4 ->
90 MHz. The build differs from shipping only in `PICO_SYS_CLOCK_MHZ` and `PICO_PSRAM_SM_MHZ`.

Device (SQ3 + sound, intro + savegame load): `[clk] sys_clk = 360000000 Hz` (no fallback), clean run,
**resource load 14,808 -> 11,791 ms**, rooms "render much quicker". Not soaked yet -- the 396 caution applies:
a marginal point fails as silent corruption, so judge it over long sessions before making it a default.

**clk_peri note:** above 133 MHz `clk_peri` is pinned to **133 MHz** (the 396 bring-up fix), but the 133 MHz
shipping build does NOT pin and runs its peripherals from **48 MHz** (the `[clk] clk_peri = 48000000` line,
despite the "pinned" wording). So the SD/LCD SPI rates differ between the two: 30 MHz requested gives 24 MHz
from 48 MHz but ~22.2 MHz from 133 MHz; LCD 25 MHz requested gives 24 vs ~22.2 MHz. Harmless (396 ran the same
way), and it means the 360 MHz resource-load gain is pure CPU -- the SD card is slightly SLOWER there. Pinning
to the 48 MHz USB PLL instead would make both builds identical if that ever matters.

**Backlight (all Pico builds):** `PICO_LCD_BACKLIGHT` (default **96**) writes the panel backlight, register
0x05 on the keyboard MCU, once at keyboard init (`set_lcd_backlight`, write-only, as pico-286 does). pico-286's
measurement: ~38% of the current at 255 while looking only slightly dimmer. `-1` leaves the MCU default.

### Startup resource scan 6.4x faster: redundant filesystem lookups, not SD speed (2026-09-28)

The `[perf] resource load` phase scaled with the number of `resource.map` LINES (SQ3 954 lines 14.8 s, KQ4 1245
lines 21.1 s, ~15-17 ms per line at 133 MHz), and the only per-line card work was `detect_odd_sci01`
(`resource_map.c`) calling `sci_open("resource.00N")` + `close()` for EVERY line to check the volume exists --
and each `sci_open` is a case-insensitive directory scan, an open, a `getcwd` and a `chdir`. A desktop
`LD_PRELOAD` shim counting filesystem calls (12 s of KQ4) also found `_scir_load_resource` doing a `getcwd` +
`chdir` round trip on EVERY resource load although volume loads never change directory (1,590 `getcwd` /
1,599 `chdir` in 12 s).

Fixes: `detect_odd_sci01` checks each volume once (all platforms, identical result); `_scir_load_resource`
saves/restores the working directory only when it actually changes it (patch-file chdir, or the uppercase
`sci_open` fallback) (all platforms, identical result); on the Pico `sci_open` opens directly -- FAT is
case-insensitive -- keeping its chdir-into-the-path side effect, with the old code as fallback (small gain: most
resource loads use a plain `open()` first). Desktop shim, KQ4 12 s: `opendir` 1,263 -> 20, `chdir` 1,599 -> 23,
`getcwd` 1,590 -> 14, resource-file opens unchanged (896).

**Device (SQ3 + sound, 133 MHz): resource load 14,808 -> 2,315 ms**, savegame load and exit clean. That is far
faster than 360 MHz was before the fix (11.8 s), so the startup-time argument for a higher clock is gone.

### Open-volume cache + FatFS fast seek; per-room load timer (2026-09-28, device A/B)

`PICO_VOLUME_CACHE` (default ON; OFF is the A/B baseline): up to 4 resource volumes stay open for the game
(`scir_volume_open`, `resource.c`) instead of an open + seek + close per load, and each gets a FatFS fast-seek
cluster map (`pico_io_enable_fastseek`, `pico_io.c`, 4 x 32 DWORDs; prints `[sd] fast seek on for fd N (K
fragment(s))` or falls back to normal seeks if a file is too fragmented). The startup check
`sci_test_view_type` also uses the cache -- a desktop backtrace showed it opening a volume once per view and
pic (563 opens for KQ4). Desktop at the 32 KB LRU limit: identical eviction sequences for SQ3/PQ2/KQ4, volume
opens per 40 s session 585/523/904 -> 3/3/4. `.bss` +572 B (the tables), +28 B for the timer.

Always-on timer: `[perf] room N: pic X ms | K resource loads Y ms since last room | session K loads Y ms`.

Device A/B, SQ3 + sound at 133 MHz, same route (777 -> 900 -> 1 -> 2, savegame load, 43, 430), 343 loads both:

| | no cache | cache + fast seek |
|---|---:|---:|
| startup `resource load` | 2,293 ms | **309 ms** |
| session resource loads | 3,902 ms | **1,531 ms** (repeatable: 1,536 on a second run) |
| per load | 11.4 ms | 4.5 ms |
| resource loads around the savegame load (164) | 1,851 ms | 716 ms |

Startup since the start of this work: **14,808 -> 309 ms** (48x). All three SQ3 volumes: 1 fragment on this card.
`FF_FS_TINY 0` (a sector buffer per open file, +4 KB `.bss`) was not done: the remaining 4.5 ms per load is
mostly the read and decompression itself.
