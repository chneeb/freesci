# Pico — build defaults, flag cleanup and chooser toggles

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 3865-3996 -->

### DONE (2026-09-18) — the DEFAULT PIO build is now the tested configuration, and sound is ON

The shipping PIO build had **no way to get sound at all** -- `PICO_PWM_AUDIO` defaulted OFF there, and the
chooser's `[S]` toggle only exists when it is ON. Fixed, and with it the three flags that have to move
together:

| | default PIO |
|---|---|
| `PICO_SYS_CLOCK_MHZ` / `PICO_PSRAM_SM_MHZ` | **133 / 133 -> SPI 66.5 MHz** (was 396 / 198; reverted 2026-09-19 for battery life) |
| `PICO_PWM_AUDIO` / `PICO_SND_RATE` | ON / **11025** |
| `PICO_PSRAM_SONGS` / `PICO_STREAM_DECOMPRESS` | ON / ON |
| `PICO_SONG_MAX_BYTES` | 65536 (follows the slots) |

Every value is one a device run produced today. `-DPICO_PWM_AUDIO=OFF` reverts the whole cluster coherently.

**THE SOUND CLUSTER COLLAPSE STOPPED BEING TIDINESS the moment sound became the default.** Four independent
flags meant the shipping build would have had sound but NO PQ2 THEME (`PICO_PSRAM_SONGS` off, so the cap skips
the 59KB song) at a sample rate never tested on PIO (`PICO_SND_RATE` defaulted 22050 while its own comment
argued for 11025 -- worth ~4KB `.bss` and ~10KB heap on the target that has neither). One decision, four
knobs, three of them wrong.

**MEASURED COST OF SOUND-BY-DEFAULT -- 8,080 bytes of heap ceiling, paid UNCONDITIONALLY:**

| | `.bss` | heap span |
|---|---|---|
| sound ON (default) | 25,344 | **466,544** |
| `-DPICO_PWM_AUDIO=OFF` | 17,608 | **474,624** |

**`[S]` off does NOT recover it** -- it passes `-q`, which skips the ~13KB of runtime allocation, but the code
and static tables are linked in either way. Against the two games nearest the ceiling that is a real
subtraction: KQ4 failed needing 12,532 with 20,512 free, Colonel's died at 304 bytes free. **Test KQ4 and
Colonel's with `[S]` OFF on this build**; if the lost ceiling bites, two images may be the honest answer.

**FOUR CMAKE ORDERING BUGS IN ONE PASS, all the same shape: a value USED BEFORE IT WAS SET.** CMake does not
warn when reading an unset variable -- it silently yields empty -- so the failure always surfaced somewhere
else:
1. `PICO_PSRAM_SM_MHZ` tested `PICO_SYS_CLOCK_MHZ` before it was set -> silently 396/133, the pair that
   DEVICE-FAILED the PSRAM smoke test. Built clean.
2. The stale cache: after the default changed, the existing `build-pico` still held 396/133 and BUILT CLEAN.
3. `PICO_SONG_MAX_BYTES` tested `PICO_PSRAM_SONGS` before it was set -> cap 32768 with slots ON, which is
   exactly the bug the device found earlier (the cap rejects the song before the slots see it).
4. Its `add_compile_definitions` was separated from its `set()` -> an EMPTY define, failing as `size > )` in
   `decompress0.c`, three directories from the cause.

**The rule: a dependent default must come after what it depends on, a `set()` and its
`add_compile_definitions` must stay adjacent, and after ANY default change reconfigure from scratch and READ
`CMakeCache.txt`.** "It built" was true in all four cases. Three were caught by reading the cache or by the
four-config matrix; only the missing `[S]` reached the device, and that was a default never flipped rather
than a regression.

### IN PROGRESS (2026-09-18) — build-flag cleanup: 45 -> 41 declarations, first pass

**Deleted:** `PICO_SOUND_SHED_FLOOR` (+ the whole shed: `pico_sound_shed_check`, `pico_sound_was_shed`, the
`kDrawPic` call site), `FSCI_PROBE_DIRTY`, `FSCI_SIM_PICO_STATIC`, `PICO_VOCAB_PROBE`,
`PICO_STATIC_VIEW_BAKE`, `PICO_LCD_16BIT`.

**The shed went entirely, not just its knob.** The mechanism worked (device: recovered 22,416 bytes, KQ4 kept
playing) but no usable trigger was ever found, and the `[S]` chooser toggle solves the same problem better --
at launch, no heuristic, no mid-game teardown, no restart hazard. Its reasoning stays HERE; keeping
dead-but-compiling code as the record is the clutter this pass removes.

**A HALF-REMOVAL nearly shipped, and only the SOUND config caught it:** dropping `PICO_SOUND_SHED_FLOOR`
while leaving `PICO_SOUND_SHED` defined left `if (PICO_SOUND_SHED_FLOOR <= 0)` as a syntax error reachable
ONLY with `-DPICO_PWM_AUDIO=ON`. The default, desktop and mapped builds all passed. **Build the four-config
matrix, not just the default.**

**Two more remnants found by GREP, not by the compiler** -- an `add_compile_definitions` referencing a now-unset
variable (expands to an empty define, silently) and a stale comment citing the deleted function. Neither
breaks a build, which is exactly why they would have survived and misled.

**REVISED from the original plan, on measurement:**
- **`FSCI_PROBE_STR` is NOT worth deleting.** Measured: 808 bytes of FLASH and **zero SRAM** -- identical
  `.bss` (17,608) and identical heap span (474,624) with it on and off. The plan listed it assuming it cost
  ceiling. It does not; it is genuinely free insurance.
- **`PICO_PWM_CARRIER_MULT` / `PICO_PWM_IDLE_LEVEL` kept for now.** Inlining them is behaviour-neutral only if
  the current values are kept, and removing a knob without a device test on the sound path is not worth it
  while the sound work is still settling.

**Still to do:** collapse the sound cluster so `PICO_PWM_AUDIO=ON` implies `PICO_STREAM_DECOMPRESS`,
`PICO_STREAM_METHODS` and `PICO_PSRAM_SONGS` (four flags, one decision -- the cap/PSRAM conflict that cost a
device cycle came from exactly this); promote the settled always-on flags; and remove the dead C-side `#ifdef`
blocks the deleted options leave behind. That last one is entangled in `widgets.c`
(`#if defined(PICO_STATIC_VIEW_PRIORITY) && !defined(PICO_STATIC_VIEW_BAKE)` now reduces to its first term)
and touches the routing the Colonel's `[V]` finding depends on, so it wants a focused pass with a device
check, not a tidy-up commit.

*Original plan:*
### PARKED PLAN (2026-09-13) — build-flag cleanup: ~40 knobs, about 22 are worth keeping

Not urgent, but the count is now a hazard in itself: getting a working PIO sound build currently means setting
FOUR flags consistently, and getting one wrong fails non-obviously (the cap pre-empting the PSRAM song path
cost a device cycle for exactly this reason).

**Delete outright -- dead or superseded:**

| flag | why |
|---|---|
| `PICO_SOUND_SHED_FLOOR` + the shed code | disabled; the chooser toggle is strictly better (no floor to mis-calibrate, no mid-game teardown, no restart hazard) |
| `PICO_PWM_CARRIER_MULT` | built for a theory the device DISPROVED; ~3% CPU for no demonstrated benefit |
| `PICO_PWM_IDLE_LEVEL` | 0 is proven correct -- a constant, not a choice |
| `FSCI_PROBE_DIRTY` + `FSCI_SIM_PICO_STATIC` | built for the dirty-rect theory, which was ruled out |
| `PICO_VOCAB_PROBE` | already labelled throwaway; measurement done |
| `PICO_STATIC_VIEW_BAKE` | OFF, device-confirmed to cause four regressions |
| `PICO_LCD_16BIT` | documented INCOMPLETE (sheared display) -- finish or drop |

**Collapse the sound cluster (7 flags, ~2 real decisions).** `PICO_PSRAM_SONGS` REQUIRES
`PICO_STREAM_DECOMPRESS`; `PICO_SONG_MAX_BYTES` already auto-follows `PICO_PSRAM_SONGS`;
`PICO_STREAM_METHODS` has one sensible value. `PICO_PWM_AUDIO=ON` should imply all of it.

**Promote to always-on:** `PICO_REBOOT_BETWEEN_GAMES`, `PICO_CONTROL_MAP`, `PICO_PACK_VOCAB` (PIO);
`PICO_PSRAM_SCRIPTS`, `PICO_STATIC_VISUAL`, `PICO_WORKING_PRIORITY` (mapped). All ON, all settled.

**Keep:** target selection, the tuning constants (clock, SD/LCD kHz, `PICO_SND_RATE`, `PICO_SND_BUF_FRAMES`),
`PICO_STATIC_COMPOSED` (the Colonel's A/B needs it), and the probes that earn their keep -- `MEM`, `GFX`,
`SND`, `PERF`, `MEM_CENSUS`.

**THE TRADE-OFF TO WEIGH FIRST: a flag is also a RECORD OF A DECISION.** `PICO_STATIC_VIEW_BAKE` being OFF
documents four measured regressions, and deleting it removes the ability to re-run that A/B. This file keeps
the reasoning but not the switch. So delete only where the experiment is genuinely closed, and keep what is
still usefully A/B-able -- which is exactly why `PICO_STATIC_COMPOSED` stays while it is a live suspect.

### DONE (2026-09-13) — chooser toggles: `[S]` sound and `[C]` composed, both no-rebuild A/Bs

`[C]` gates the whole composed-surface path at runtime, default ON. **One line in `pico_compose_ensure`
suffices**, because every composed reader is downstream of it or of `composed_valid` -- the bake,
`pico_invalidate_static_region`, the fullscreen static draw, and the BACK restore's `srcmap` choice. With it
returning 0, `composed_valid` never becomes true and all four fall back to `static_bg`: the exact
pre-composed path, not an approximation.

Both choices are logged at launch (`[snd] launching with sound ...`, `[gfx] composed surface ...`) so any log
records the combination that produced it.

<!-- from CLAUDE.md lines 4168-4186 -->

#### Log noise and the PIO `.bss` baseline (2026-09-10)

Default builds are quiet. Two causes were fixed:
- **Leftover probe flags in the build dir** -- `build-pico` had `FSCI_PROBE_GFX` (which emits
  `[pblit]`/`[pbuf]`/`[pupd]` PER CEL) and `FSCI_PROBE_PARSER` on from earlier debugging. Most of the
  volume was this, not the code. Check `grep '^FSCI_PROBE.*ON' <builddir>/CMakeCache.txt` before
  concluding anything about output volume.
- **Genuinely ungated upstream prints**, now behind the matching existing probe flag: the clone-table
  `Free list:` / `Entries w/zero vars:` dumps in `savegame.c` (the worst -- one `sciprintf` PER ENTRY,
  twice, on EVERY restore) -> `FSCI_PROBE_MEM`; `Activating port ...` on every window dispose
  (`kgraphics.c`) -> `FSCI_PROBE_GFX`; the `[play]`/`Morphing`/`SI_MORPH` sound prints -> `FSCI_PROBE_SND`.

Also a small speed win: `printf` formatting costs CPU whether or not the bytes reach a terminal, and with
non-blocking USB stdout they are DROPPED rather than waited on -- so that work was pure loss.

**PIO `.bss` baseline is now 17,284** (was quoted as 17,280 for most of this session -- that figure was a
`FSCI_PROBE_GFX=ON` build). Probes off took it to 17,276; the always-on timing above adds 8. Use 17,284 as
the "PicoCalc build unchanged" check after shared-file edits.

