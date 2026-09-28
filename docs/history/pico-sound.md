# Pico — sound history (PWM/OPL2, PIO and mapped)

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 240-486 -->

### Sound on Pico

> ## ✅ RESOLVED on the MAPPED-PSRAM target (2026-09-05) — music plays at correct speed
>
> **The root cause was never memory and never the synth.** `pico_sfx_poll()` had exactly ONE call site,
> `pico_get_event()`, so audio production was gated on **how often the game asked for input** — which during
> an animation sequence is barely ever (measured **0.3–7 polls/sec**, and **50 seconds with zero** at startup).
> The mixer emits at most `buf_size` frames per call, so a starved poll rate starves the ring, and the PWM IRQ
> then holds `last_sample` — which **stretches AND chops** the audio at once. One cause, both recorded
> symptoms ("way too slow" + the "broken tractor" buzz).
>
> Fixed by also polling from the per-frame **front flush** (where `poll_keyboard` already lives for exactly
> this reason) and from inside **`usec_sleep`**, sliced. Device-measured: `produced` 6,144–14,336 → **~22,000**,
> `underrun` 7,000–19,700 → **0**.
>
> **This RETIRES the 2026-07-10 conclusion below** that "the bad audio is the PWM/mixer/OPL output path
> itself, which is untested/untuned" and that a future attempt must "start from tuning the PWM/mixer path".
> The path was fine — it was starved. Measured, all within budget: PWM IRQ **~1% CPU** (RAM-resident, fully
> inlined), OPL synth **33–47% CPU**, hardware consuming at exactly 22 kHz.
>
> **METHOD NOTE, because it cost several device cycles:** four successive hypotheses (buffer size, CPU-bound
> synth, XIP cache thrashing, IRQ overhead) were each reasoned from the code and each disproved by the first
> measurement. What localised it in one run was an `[snd]` probe printing **production vs consumption rates**
> (`FSCI_PROBE_SND`, default OFF). The tell was that every `produced` value was an exact multiple of
> `buf_size` — full batches, too few calls. **For a timing bug, measure the two rates before reasoning about
> the code.**
>
> **Still open even with sound working:**
> - **Dropped notes** — `ADLIB: All voices full`. Upstream FreeSCI has **no voice stealing** (literally
>   `XXX implement overflow code`, `opl2.c`), so a passage wanting >12 simultaneous voices loses one. Not
>   Pico-specific. Its `printf` is now rate-limited: it sits in the note-start path and goes over USB, so
>   unthrottled it could stall the very loop feeding the ring.
> - **SN76496 is NOT a cheaper drop-in** — it produces SILENCE on SQ3 (the resource almost certainly carries
>   no Tandy/PCjr track). Selectable via `PICO_SOFTSEQ` if ever revisited.
> - **The PicoCalc PIO target is NOT fixed by this** — see below.
>
> #### Would sound work on the PicoCalc PIO target? (analysed 2026-09-05, NOT device-tested)
>
> **The quality blocker is gone for free.** The poll fix is in shared code (`pico_driver.c` front flush +
> `usec_sleep`), so PIO inherits it. The 2026-07-10 "broken tractor" on PIO was almost certainly THIS bug,
> not an untuned DSP path — so if PIO sound can run at all, it should now run at the right speed.
>
> **The memory blocker is untouched.** The recorded PIO failure was `calloc 16384 failed` at
> `sm_allocate_stack` — the 16 KB VM value stack failing to find a **contiguous** block. That is the SRAM
> ceiling/fragmentation problem which ONLY the mapped target escaped.
>
> Measured static cost of a `-DPICO_PWM_AUDIO=ON -DPICO_SND_RATE=11025` PIO build:
> `.bss` 17,280 → 25,000, heap span **475,088 → 467,080 (−8 KB)**. Runtime resident on top is ~19 KB
> (OPL chip ~7 KB, compbuf 2×1002×4, feed + writebuf), so **~27 KB effective** against the documented
> **~26 KB clean-build margin**. It consumes essentially the whole margin, and the failure mode is
> contiguity, not total bytes.
>
> **Verdict: worth exactly one experiment, do not expect robustness.** 11025 halves the synth cost
> (33–47% → ~20% CPU) and every derived buffer, and `PICO_SND_RATE` makes it a build flag rather than a
> source edit. But do not expect it to survive long play or restore chains — the record's "sound + long
> restore chains + comfortable margin is out of reach" verdict still stands for PIO. Sound is a
> mapped-PSRAM feature.
>
> NB the buffer sizes were raised (`buf_size` 512→2048, ring 2048→8192 at 22 kHz) to survive rare polls
> BEFORE the poll fix existed. With polls now at 60 Hz a batch only needs ~367 frames, so if PIO memory is
> ever the deciding factor these can come back down — that is the cheapest ~24 KB available.

*Historical (the pre-2026-09-05 state — memory analysis still valid for the PIO target):*
Sound is currently disabled, in two layers:
- **FreeSCI engine sound:** `--no-sound` (`-q`) in `pico_main.c`'s argv → `SFX_STATE_FLAG_NOSOUND`.
- **PicoCalc PWM synth:** gated behind CMake `option(PICO_PWM_AUDIO)` (**default OFF**). When OFF,
  `audio/pwm_synth.c` is **not linked at all** (`src/platform/pico/CMakeLists.txt`) and
  `pwm_synth_init(26)` is `#ifdef PICO_PWM_AUDIO`-skipped (`pico_main.c`). This recovers the ~6.8KB
  SRAM its `strings[6924]` waveform table reserved (the synth otherwise ran an idle 22kHz IRQ
  outputting silence, since nothing ever drives a channel while engine sound is off) plus its flash +
  channel globals. `strings[]` is now `static const` (`audio/pwm_strings.h`) so even an ON build keeps
  it in flash (`.rodata`), not SRAM — it's read-only in the IRQ.

To bring sound back:
- Build with `-DPICO_PWM_AUDIO=ON` (re-links the synth + re-arms `pwm_synth_init`).
- Wire FreeSCI's OPL2 softsynth (fmopl.c) output into a PCM callback feeding `pwm_synth`
- Add a Pico PCM device driver under `src/sfx/pcm_device/pico_pwm.c`
- Remove `--no-sound` from `pico_main.c`'s argv

#### Sound stack now wired (default OFF) + the SQ3-intro sound OOM (graceful-skip DONE, music still doesn't fit)

The PCM/softseq plumbing above is now **implemented behind `PICO_PWM_AUDIO` (still default OFF)**: a Pico PCM
device (`src/sfx/pcm_device/pico_pwm.c`, polled at 60Hz from the main loop → lock-free ring → 22050Hz mono
8-bit PWM IRQ), the OPL2 softseq with **flash-resident `const` tables** (`fmopl_tables.{c,h}`,
`FMOPL_FLASH_TABLES`, ~136KB in `.rodata`, zero heap; mono = one ~7KB `FM_OPL` chip), and the CMake wiring.
**Default builds stay sound-off** — none of this links or runs unless `-DPICO_PWM_AUDIO=ON`.

**The blocker is unchanged: there is no SRAM headroom for music on SQ3.** With sound ON, the SQ3 intro OOM'd
**before the first note** at `decompress0.c:50` (`malloc 18996 failed`) — a **fragmentation OOM**: ~25KB free
but no 18996-contiguous run, arena 456416 of the ~469216 PWM-build physical ceiling (**~13KB growth room
left**). The resident sound stack (OPL chip + mixer compbuf + ring) ate the margin that the intro pic/view
decodes need.

**DONE — graceful skip for non-essential sound resources** (`decompress0.c`, `HAVE_PICO`). Per the directive
"assume it's a sound resource, don't touch pic/view": `pico_decompress_alloc` now routes `sci_sound` decodes
through **raw `malloc`** (not `sci_malloc`), so an OOM returns **NULL** instead of the fatal `pico_oom_report`
halt; a NULL-guard right after the alloc fails the decode cleanly (`SCI_STATUS_NOMALLOC` →
`SCI_ERROR_DECOMPRESSION_INSANE`), and `resource.c`'s existing decompressor-error path leaves `res->data=NULL`
so the song load returns empty and **the game keeps playing silently**. Every *other* resource type still
halts legibly (pic/view/script unchanged — they MUST fit). This makes a too-big song a non-event, but it does
NOT make music play — it just stops sound from crashing the intro.

**Levers assessed for actually fitting intro music — only one is viable, and it's unbuilt:**
*(SUPERSEDED 2026-07-10 — see the "RE-ANALYSIS" section below: three cheaper levers were missed here; the
vocab-borrow is now a last-mile helper, not the sole path. Kept for the rejection reasoning on PWM_BUF/lazy
chip/fixed buffer, which still stands.)*
- **`PICO_PWM_BUF_FRAMES` 512→256 — REJECTED.** Frees only ~2KB steady-state, and breaks audio: 256 frames =
  11.6ms produced/poll < the 16.6ms 60Hz drain → ring underrun/stutter. 512 (23.2ms/poll) is the floor.
- **Lazy OPL chip alloc — only shifts the peak.** Defers the ~7KB chip, but the intro is *where* music starts,
  so the chip is live exactly when the intro decode peaks. Helps only music-free rooms, not the intro OOM.
- **A fixed "intro-only" sound buffer — relocates the OOM, doesn't remove it.** It would have to be
  boot-allocated (you can't grab a contiguous block mid-intro on the fragmented heap — that's the OOM itself),
  so it's permanently resident; and it can't be a *reusable* scratch because song data is read every tick
  during playback (`iterator.c` reads `self->data` per tick, refcounted) — it must stay resident while
  playing. The intro is the peak, so reserving for it just moves the wall.
- **Share one buffer with vocab/grammar — fails on lifetime overlap.** Vocab is permanently resident; the GNF
  parse peak is already solved via the visual-borrow and overlaps *gameplay* music; every large buffer is
  busy during the intro.
- **Borrow the VOCAB blob to PSRAM during the intro — THE one viable path (NOT built).** The packed vocab blob
  (`g_pico_vocab_blob`, ~21419B, contiguous, > the 18996B song) is **idle during the intro** — its only
  gameplay reader is `vocab_tokenize_string` inside `kParse` (`kstring.c:325`), and you can't type a parser
  command during the intro. So mirror the `pico_borrow_visual`/`pico_return_visual` pattern: page the vocab
  blob to PSRAM for the duration of intro sound, freeing ~21KB SRAM for the song decode, then restore it
  before the first parse. **Mechanism must be borrow-to-PSRAM, NOT destroy-and-reload** (re-packing the blob
  needs a 21KB contiguous run → would re-OOM on the fragmented post-intro heap). **Catches (why it's a spike,
  not a quick edit):** the song must be **cut off / handed back before the first parse** (intro-only — it does
  nothing for in-room music, which overlaps the parser); it needs **sfx↔parser coordination** (who owns the
  blob when); it assumes a **single** large sound resource at a time; and the borrow/return `sci_malloc` on
  restore could itself halt if the heap fragmented (degrades loudly via `pico_oom_report`, never corrupts).
  Deferred — not worth the multi-file coordination for intro-only music while the port is at the ceiling.

**Bottom line: sound stays disabled.** The stack is ready to switch on (`-DPICO_PWM_AUDIO=ON`) and won't crash
the intro anymore (graceful skip), but real intro music needs the vocab-borrow spike above, which isn't built.

#### RE-ANALYSIS (2026-07-10, code-read — the "vocab-borrow is the ONLY path" conclusion above is SUPERSEDED)

A fresh read of the actual sound stack (not the CLAUDE.md summary) found **three levers the earlier analysis
missed**, two of them cheap and mechanical, which together change the recommended plan. The vocab-borrow
(above) is now **demoted from "the one viable path" to a small last-mile helper** — see lever 4. Nothing
built yet; this records the corrected plan. All line numbers verified in-tree this session.

**Corrected budget (the earlier "~10–13KB resident, ~19KB transient" was wrong on the transient):**
- **Resident with sound ON ≈ 13KB** (verified): OPL chip state ~7KB (`fmopl.c:1094-1105`, one `calloc` of
  `sizeof(FM_OPL)` + `sizeof(OPL_CH)*9`), mixer compbufs 2×512×4 = 4KB (`mixer/soft.c:95-96`) + feed buf
  ~1KB (`:180`) + writebuf ~0.5KB (`:311`), PCM ring `pcm_ring[2048]` = 2KB `.bss`
  (`audio/pwm_synth.c:15`), **plus the song data resident for the whole playback** (18,996B for the SQ3
  intro).
- **Transient at song START ≈ 38KB, NOT 19KB (finding #2 below):** the resource's decompressed copy AND the
  iterator's `memdup` copy are co-resident. The recorded intro OOM (`malloc 18996 failed`,
  `decompress0.c:50`) died at the *decompress* (step 1) before the doubling even happened — so the doubling
  was never in the earlier accounting. Contiguity, not total free, remains the binding ask.

**Finding #1 (CHEAP, verified) — ~4.7KB of the ~7KB OPL "chip state" can move to flash `const`.**
`FN_TABLE[1024]` (4096B) + `AR_TABLE[75]` + `DR_TABLE[75]` (600B) live *inside* `FM_OPL` (`fmopl.h:127-129`)
but are computed **only** in `OPL_initalize`/`init_timetables` (`fmopl.c:588-608`, `:764-768`) as pure
functions of `freqbase = ((double)clock/rate)/72` — both compile-time constants on Pico (fixed 22050Hz,
fixed clock). They are never rewritten after init (the per-slot `AR`/`DR` pointers just index into them, and
`FN_TABLE` is read-only in `OPLWriteReg`). So they can be precomputed `const` in `fmopl_tables.c` exactly
like the five big tables already are (`FMOPL_FLASH_TABLES`, `OPLOpenTable`, `fmopl.c:610-624`), dropping the
per-chip SRAM from ~7KB → **~2.3KB**. Mechanical extension of the existing flash-table pattern; low risk.
(Caveat: they'd have to move OUT of the `FM_OPL` struct to a shared const — the struct is `calloc`'d as one
block, so this is a small representation change like the `SIN_TABLE`→offsets one already done.)

**Finding #2 (CHEAP, verified) — the iterator DOUBLES every song; halve the song-start transient for free.**
`songit_new` does `it->data = sci_refcount_memdup(data, size)` (`iterator.c:1985`), and the caller
`ksound.c:116-121` (`_pick_song`/`kDoSound`) passes `song->data` from `scir_find_resource(..., lock=0)`
(`ksound.c:97,116`) — i.e. the resource copy is **still live in the LRU** when the memdup runs, so both are
resident. **Pico-gated fix:** steal `res->data` into the iterator and immediately evict the resource (the
exact evict-immediately pattern pic/view decode already uses), instead of memdup+leave-in-LRU. The iterator
is the sole reader after that point (loop-rewinds read `self->data`, never the resource;
`iterator.c:172,386,419`; teardown `sci_refcount_decref(self->data)` `:722`). Halves the ~38KB peak → ~19KB.

**Finding #3 (the NEW real lever, verified) — song playback reads are SEQUENTIAL → the song can live in
PSRAM, streamed through a small SRAM window.** Playback consumes the song strictly as
`cmd = self->data[channel->offset++]` (`iterator.c:172`) plus tiny `memcpy(buf+1, self->data+offset,
paramsleft)` of ≤ a few param bytes (`:209`) and `_parse_ticks(self->data+offset, ...)` (`:419`) — monotonic
per channel, single-digit bytes per 60Hz tick, with only occasional loop-point rewinds. On the ~4MB/s PSRAM
SPI link a 1–2KB SRAM window refilled on a boundary crossing costs ~0.5ms and rarely — imperceptible at
60Hz. So a song resource can sit in a **fixed PSRAM slot** (the `0x700000` visual-borrow scratch pattern,
`pico_driver.c:101`) with playback SRAM cost ≈ the window, not the whole song — this is what unlocks
**in-room** music (which overlaps the parser, so the vocab borrow can't help it). **Exclusion:** embedded-PCM
songs (`self->data[0] == 2`, `iterator.c:518,710`) read bulk digitized-sample data, not a byte stream — those
keep the current graceful-skip or an SRAM fallback.

**Revised ranked levers:**
| # | Lever | Effect | Risk | Effort |
|---|---|---|---|---|
| 1 | FN/AR/DR → flash `const` | −4.7KB resident/chip | low | small (`fmopl.c` + `fmopl_tables.{c,h}`) |
| 2 | Steal `res->data` + evict, drop the memdup | −~19KB song-start transient | med-low | small (`iterator.c`/`ksound.c`, `HAVE_PICO`) |
| 3 | Song → fixed PSRAM slot, streamed SRAM window | song size → ~1–2KB during playback; **enables in-room music** | medium (loop rewind across window, PCM-song fallback, refcount teardown from PSRAM) | the real spike (SCI0 `iterator.c` read path) |
| 4 | Vocab-borrow (above) — **demoted** | with #2+#3 the vocab hole is needed only for the ms of *decode*, not the whole intro → the recorded sfx↔parser-coordination problem largely evaporates | low once #3 exists | small |

**Do NOT** trade away the B-1.2 16KB decompress scratch to fund sound — it is what holds restore chains
together; sound must not regress restores.

**Recommended scope (honest, given the Codex "sound + long restore chains + comfortable margin = out of
reach" verdict):**
- **Phase A (cheap, ~1 session): levers 1+2.** Resident ~8.5KB, song-start transient ~19KB. Flash
  `-DPICO_PWM_AUDIO=ON`, measure with `[arenagrow]`/`[mem] BREAKDOWN` whether the intro song now decodes
  (graceful skip still catches misses). May already yield in-room music where the heap is calmer.
- **Phase B (the spike): lever 3 (+4 if needed).** Full music incl. intro; playback ~12KB total resident.
- Even A+B spends roughly **half** the ~26KB clean-build margin, so **sound remains best-effort alongside
  long restore chains** — declare that scope rather than chase "sound + robust arbitrary-length restores".

#### PHASE A BUILT + DEVICE-TESTED → REVERTED (2026-07-10) — sound stays disabled; two findings banked

Levers 1 and 2 were built (`FMOPL_FLASH_TABLES`/`HAVE_PICO`-gated), both builds clean, flashed with
`-DPICO_PWM_AUDIO=ON`, and **device-tested for the first time ever** (the sound stack had never actually
driven the PWM before). Result: **not viable — reverted (all code back to HEAD; sound stays OFF by default).**
Two concrete, load-bearing findings were captured before reverting:

- **FINDING (decisive) — the OPL synth output is garbled ("broken tractor"), and it is NOT the memory
  changes.** Lever 1 baked `FN_TABLE[1024]`+`AR_TABLE[75]`+`DR_TABLE[75]` into flash `.rodata` for the fixed
  Pico config (mono/22050Hz/clock 3579545), dropping ~4.7KB from the per-chip `FM_OPL` (ELF-verified: symbols
  at `0x100bxxxx`, sizes 0x1000/0x12c/0x12c = 4696B). **Those baked tables are byte-for-byte CORRECT** —
  proven by an independent re-derivation of `OPL_initalize`/`init_timetables` at 22050Hz diffed against
  `fmopl_tables.c` (`OK: … EXACTLY match the fmopl.c runtime formula`, not via the generator). So the bad
  audio is **the PWM/mixer/OPL output path itself, which is untested/untuned** (ring underrun at the 60Hz
  poll vs 22050Hz drain, sample-rate/format, or mixer immaturity) — a **separate, larger Phase-B-class job**,
  NOT lever 1. **A future sound attempt starts from "tune the PWM/mixer path," not "re-derive tables."**
- **FINDING — sound is fatal at the SRAM ceiling on game load (confirms the Codex verdict quantitatively).**
  With sound ON, a game load OOM'd: `calloc 16384 failed` at `sm_allocate_stack` (`seg_manager.c`) — the
  **VM value stack** (one of the three irreducible 16KB baselines) couldn't find a contiguous 16KB block
  (`free=80752` total but `arena=469216`, i.e. maxed + fragmented). The resident sound stack (~13KB + song
  data) ate the margin the load needs. **Phase A's ~5KB (lever 1) + evict (lever 2) cannot close this** — the
  arena still maxes and fragments during play. This is the documented "sound doesn't fit at the ceiling" wall,
  now reproduced with sound actually on.

- **Levers as built (reverted, but recorded so a Phase-B restart doesn't re-derive them):** *Lever 1* — flash
  FN/AR/DR via `tools/gen_fmopl_tables.c` `build_rate()` (mirrors the runtime math; regen per the file
  header), struct members `#ifndef FMOPL_FLASH_TABLES`'d out, init write-loops compiled out, 4 read sites
  through `OPL_{FN,AR,DR}_TABLE()` accessor macros. *Lever 2 (safe half)* — `build_iterator` (`ksound.c`,
  `HAVE_PICO`) evicts the sound resource via `scir_evict_resource_data` right after `songit_new` (guarded
  `lockers==0`), killing the steady-state duplicate copy; the `memdup` **2× transient** is unchanged (its
  removal needs the fragile refcount-ownership transfer — `res->data` is plain `sci_malloc`, tee-iterators
  incref/decref-share it, `iterator.c:616,1165,722` — do NOT attempt as a "steal the pointer" one-liner).
- **A `[pwm]` boot marker** was added to `pico_main.c` (also reverted) to confirm the sound firmware actually
  ran (`[pwm] sound-enabled firmware: launching freesci_main argc=5 (no -q)`) — it settled an initial
  wrong-uf2 upload. Keep in mind for the next attempt: the DEFAULT build (`build-pico`, no `PICO_PWM_AUDIO`)
  still passes `-q` and prints `[SFX] Sound disabled.`; only the PWM build drops `-q`.

**Bottom line unchanged:** sound stays disabled. Phase A's memory wins are real but small and only matter with
sound ON, so they were reverted with it. The two blockers for audible music are now sharply named: (1) the
PWM/mixer output path needs actual DSP tuning (garbled today), and (2) even tuned, the resident stack OOMs
game-load at the ceiling — needs Lever 3 (Phase B: PSRAM-streamed song data, the real unlock) AND likely a
"sound incompatible with restore chains" scope. Neither is a quick edit.

<!-- from CLAUDE.md lines 2407-2537 -->

3. **Sound *(independent track — gated on heap headroom, not CPU)*.** The whole sound stack
   (`scisound`/`scisoftseq`/`scipcm`/`scimixer`) already links into the firmware but is dormant:
   `pico_main.c` passes `-q` → `SFX_STATE_FLAG_NOSOUND`. The PicoCalc PWM synth is **now gated behind
   `-DPICO_PWM_AUDIO` (default OFF)** — when OFF `audio/pwm_synth.c` is unlinked and `pwm_synth_init(26)`
   is skipped, recovering ~6.8KB SRAM (see "Sound on Pico (TODO)"). To work on this item, build with
   `-DPICO_PWM_AUDIO=ON` to get the 8-bit mono 22 kHz PWM on GPIO 26/27 back; what's missing is then
   feeding *PCM* into it instead of the tiny_agi sine channels.
   - **CPU is fine:** OPL2 inner loop ≈ 9 voices × ~30 cyc × 22050 ≈ 4% at 150 MHz; PWM IRQ <1%.
     `OPLOpenTable` uses `pow/log10/sin` once at init (fast with the RP2350 FPU; slow on RP2040).
   - **RAM is the gate.** Static `.bss` is **already negligible** — the big synth tables
     (`TL_TABLE`, `SIN_TABLE`, `AMS_TABLE`, `VIB_TABLE`, `ENV_CURVE`) are `static int *` pointers
     **NULL until `OPLBuildTables()` malloc's them** (`fmopl.c:610-627`), so under NOSOUND they cost
     **zero** SRAM; only ~650 B of tiny lookup tables (`KSL_TABLE` 512 B, `SL_TABLE` 64 B, `RATE_0`
     64 B, `outd` 4 B) sit in `.bss`. (The old "~34 KB `ENV_CURVE` in `.bss`" claim was WRONG —
     `ENV_CURVE` is `static int *ENV_CURVE = NULL`, `fmopl.c:191`.) The cost is therefore **entirely
     dynamic, paid only when sound is enabled**: HQ stereo ~165 KB (**won't fit**), LQ mono default
     tables ~70 KB, LQ mono + shrunk tables (`EG_ENT=128`, `SIN_ENT=512`, drop the stereo OPL) ~40 KB.
     Need ≥~80 KB free before enabling — gated purely on measured steady-state headroom.
   - **CHEAPEST-SRAM DESIGN — flash-resident `const` tables drop the ask to ~10–13 KB, NOT ~40 KB
     (byte-split sanity-check, 2026-06-23).** The decisive point: the five big tables are pure functions
     of `EG_ENT`/`SIN_ENT` (`pow/log10/sin`, **sample-rate-independent**), so they can be **precomputed
     `const` and stored in flash** (RP2350 has MBs free). Once they're flash, **table resolution stops
     mattering for SRAM** — so pick HQ for quality and let flash eat it:

     | Table | Formula | HQ (EG_ENT=4096, SIN_ENT=2048) | LQ (EG_ENT=128, SIN_ENT=512) |
     |---|---|---|---|
     | `TL_TABLE` | `EG_ENT*16` | 65,536 | 2,048 |
     | `SIN_TABLE`* | `SIN_ENT*16` | 32,768 | 8,192 |
     | `AMS_TABLE` | `AMS_ENT*8` (512) | 4,096 | 4,096 |
     | `VIB_TABLE` | `VIB_ENT*8` (512) | 4,096 | 4,096 |
     | `ENV_CURVE` | `(2*EG_ENT+1)*4` | 32,772 | 1,028 |
     | **flash total** | | **~136 KB** | **~19 KB** |

     *`SIN_TABLE` is currently `int**` (pointers INTO `TL_TABLE`, `fmopl.c:181/612/647`). To be
     flash-storable it must become `int` **offsets** into `TL_TABLE` (same byte size); `OP_OUT`
     (`fmopl.c:445`) then indexes `TL_TABLE[base + sin_offset]` instead of dereferencing. **This is the
     only non-mechanical representation change.**

     **What genuinely stays in SRAM (mutated every sample, can NOT go to flash):**
     | SRAM cost | Size | Note |
     |---|---|---|
     | `FM_OPL` per-chip state | **~7 KB/chip** | `FN_TABLE[1024]`=4096 + `AR/DR_TABLE[75]`×2=600 + 9× `OPL_CH` (~190 B ea ≈ 1.7 KB) + scalars. **Mono=1 chip ≈7 KB; stereo=2 ≈14 KB.** |
     | mixer block buffer | ~1–2 KB | scimixer working block |
     | PCM ring → PWM IRQ | ~2–4 KB | 8-bit downconverted samples |
     | **SRAM total (mono)** | **~10–13 KB** | |

     **Bottom line:** cheapest sound ≈ **~10–13 KB resident, dominated by the OPL *chip state*, not the
     tables** (the prior ~40 KB plan assumed tables live in SRAM). Go **HQ + flash tables + mono** (PWM is
     mono anyway; mono halves the chip-state floor). Changes vs the current build: (1) `SIN_TABLE` →
     offsets; (2) precompute the five tables `const`; (3) `opl2.c:544` keep HQ but select the const tables
     instead of `malloc`; (4) `opl2.c:546-547` drop `ym3812_R` (stereo→mono); plus the five deliverables
     below. **NB the current build is HQ *stereo* (`opl2.c:544` + dual `ym3812_L/R`)** — the ~165 KB
     "won't fit" config; the cheap path is HQ mono with flash tables.
   - **Five deliverables:** (A) `src/sfx/pcm_device/pico_pwm.c` implementing `sfx_pcm_device_t` +
     ring buffer, downconverting the mixer's 16-bit samples to 8-bit; (B) rewrite `pwm_synth.c`'s
     IRQ to pop the PCM ring (also frees ~44 KB flash by dropping `pwm_strings.h`); (C)
     `src/sfx/timer/pico.c` using `add_repeating_timer_us(-16667,…)` (60 Hz) to replace POSIX
     `sigalrm.c`; (D) CMake wiring + register `pcm_driver_pico_pwm` behind `HAVE_PICO_PWM`, exclude
     `fluidsynth.c`; (E) drop `-q` from `pico_main.c` argv, pass `-m pico_pwm -p polled`. SQ3 ships
     `ADL.DRV` so its sound resources carry Adlib tracks — no extra resource handling needed.

Suggested order: 1 ∥ 2 ∥ 3 (all independent; pick by user-visible value vs. measured headroom).

#### The 16 KB XIP-RAM lever for PicoCalc sound — TRIED + ABANDONED (device-measured, 2026-09-04)

**DEAD END. Rolled back.** The idea (below) was built behind an `FSCI_XIP_RAM` CMake option on pico-sdk 2.3.0,
device-tested, and abandoned: pinning the XIP cache as SRAM **disables the flash instruction cache**, and
FreeSCI runs from flash, so decode craters. Measured with an `[perf]` pic-decode timer (`FSCI_PROBE_PERF`,
kept — a reusable gated diagnostic in `operations.c` `gfxop_new_pic` + `pico_perf_us` in `pico_time.c`),
same SQ3 rooms, flash-cache-ON baseline vs XIP-cache-as-RAM:

| room | cache ON | cache OFF (XIP RAM) | slowdown |
|---|---|---|---|
| pic 777 | 88 ms | **980 ms** | **11.1×** |
| pic 900 | 91 ms | **593 ms** | **6.5×** |

An **order of magnitude** slower. Fatal two ways: (1) room loads become ~0.6–1.0 s (very noticeable), and
(2) — decisively — the 22 kHz synth IRQ needs a sample every ~45 µs; flash-resident synth code running ~10×
slow cannot keep up, so this would break the very sound it was meant to enable. The `xip_cache_pin_range()`
sub-range pin (keep ~3 KB cache) **won't save it either**: the decoder's hot-code working set is far larger
than 3 KB, so it still thrashes. **Whole-16 KB pin or sub-range, XIP-cache-as-RAM is not viable for a
flash-resident interpreter.** (A hypothetical alternative — put only the *synth code* in RAM via
`__not_in_flash_func` and keep the cache — doesn't need XIP-RAM at all and doesn't solve the *memory* problem
this was for; the sound *data* still has nowhere to live off the fragmenting main heap.)

**Bottom line for PicoCalc sound: still unsolved, and XIP RAM is off the table.** The remaining paths are the
non-XIP memory levers (flash OPL tables + mono + evict — which CLAUDE.md already found don't fully fit at the
ceiling) or the **Pimoroni mapped-PSRAM** track (branch `pico-pimoroni-mapped-psram`), which dissolves the
ceiling but has its own bring-up bug.

**pico-sdk 2.3.0 upgrade (done for this, now moot):** validated — FreeSCI builds clean against 2.3.0 (the
`pico_malloc` override + moved RP2350 memmaps survive), costs only **+472 bytes** static SRAM, needs picotool
2.3.0 (`-DPICOTOOL_FORCE_FETCH_FROM_GIT=ON`). It lives isolated in the worktree `~/Source/pico-sdk-2.3.0`; the
default `~/Source/pico-sdk` stays 2.2.0. Since XIP-RAM was the only reason to upgrade, 2.3.0 is **not needed**
now — keep or remove the worktree (`git worktree remove ~/Source/pico-sdk-2.3.0`) at will. **Kept on master:**
the `PICO_STDIO_USB_STDOUT_TIMEOUT_US=0` fix (non-blocking stdout — a real standalone-run improvement, commit
`7a4f9623`) and the `FSCI_PROBE_PERF` decode timer.

---

*Original (optimistic) analysis, SUPERSEDED by the measurement above — kept for the mechanism detail:*

The most promising way to make sound *fit* on the PicoCalc (as opposed to the Pimoroni board, which dissolves
the ceiling entirely — see the RP2040/Pimoroni section) is to put the **resident sound stack in the RP2350's
16 KB XIP cache-as-SRAM**, off the fragmentation-prone main heap. The resident sound cost is a **fixed ~13 KB**
(OPL chip state ~7 KB, mixer buffers, the PCM ring) — exactly the shape that wants a separate fixed pool. The
main-heap fragmentation wall is the real blocker for sound; moving those ~13 KB out of the arena sidesteps it.

**Hardware:** the RP2350 XIP SRAM window is `XIP_SRAM_BASE 0x13ffc000 … 0x14000000` = **16 KB**. On the PicoCalc
this is a good fit because the XIP cache serves **only flash** there (the PIO PSRAM is uncached), so carving it
costs only flash instruction-cache — unlike the Pimoroni target, where the cache also serves the mapped PSRAM,
making the XIP-RAM lever counterproductive (they compete for the same 16 KB). So this lever is **PicoCalc-only**.

**SDK support — needs an upgrade:** the **installed pico-sdk is 2.2.0, which has NO turnkey XIP-RAM** (the
address is defined but there's no linker region, allocator, or enable helper — you'd roll it yourself with
XIP_CTRL). **pico-sdk 2.3.0 adds turnkey support** (verified by reading the 2.3.0 tree):
- `PICO_USE_XIP_CACHE_AS_RAM` config flag + an **`__in_xip_ram(...)`** attribute macro to place data/functions
  in the `.xip_ram` section (linker region + `memmap_xip_ram` support, with `test/pico_xip_sram_test/`).
- **`xip_cache_pin_range(offset, size)`** — pins a *sub-range* of the cache as SRAM, leaving the rest caching,
  so the cache cost is **tunable** (sacrifice only what the sound buffers need, not all 16 KB).

**How it'd be used:** `__in_xip_ram` is *static* placement (fits fixed sound buffers directly). FreeSCI currently
`calloc`s the OPL chip (`fmopl.c`), so either make the chip state / mixer buffers / PCM ring static-in-xip-ram
(the "flash tables + mono" plan already leans static) or run a tiny bump allocator over `0x13ffc000`.

**Caveats:** (1) upgrade 2.2.0 → 2.3.0 first — real step, the RP2350 memmap paths moved between the two, so it's
a rebuild + full PicoCalc retest, not a drop-in; (2) measure the (pinned-sub-range) cache cost against the
22 kHz audio IRQ before relying on it. **Verdict:** the right-shaped lever for PicoCalc sound specifically —
fixed pool, off the main heap, tunable cost — gated on an SDK upgrade and a perf check. Back-pocket for the
sound track; orthogonal to the render and Pimoroni work.

<!-- from CLAUDE.md lines 3522-3730 -->

#### Sound on the PIO target — WORKS FOR SQ3, marginal beyond it (2026-09-06)

Device-tested, and it exceeded both the record's prediction and my own analysis. `PICO_PWM_AUDIO` now
defaults **ON for mapped, OFF for PIO** — PIO must be opted into explicitly:

```bash
cmake -B build-pico-snd -DPLATFORM=pico -DPICO_SDK_PATH=~/Source/pico-sdk -DPICO_BOARD=pico2 \
      -DPICO_PWM_AUDIO=ON -DPICO_SND_RATE=11025 -DPICO_SND_BUF_FRAMES=512
```

- ✅ **SQ3 plays with music** — intro and game. The poll fix is shared code, so PIO inherited the whole
  quality fix for free; the 2026-07-10 "broken tractor" on PIO was almost certainly that same starved poll.
- ❌ **PQ2 does not fit.** OOM at `sci_refcount_alloc` (the song-data memdup, 9,019 B) with
  **`free=104 bytes` and `arena` at its exact maximum** — TRUE EXHAUSTION, not the contiguity failure that
  was predicted. PQ2 is simply the heavier game (1843 vocab words vs 1489, ~59 KB songs vs ~19 KB).
- ### RESOLVED (device-confirmed 2026-09-13) — the "shrill feep" is the PWM IDLE DUTY CYCLE, not starvation, not the carrier

**One value: `PICO_PWM_IDLE_LEVEL` 127 -> 0** (`pwm_synth.c` + CMake knob). A sustained 50% duty cycle is
AUDIBLE on this hardware. `last_sample` was initialised to the 127 midpoint, so an idle device sat at 50%
duty forever and sang. Device-confirmed both ways: tone gone at the chooser, music unaffected.

**Safe because it is only the INITIAL/reset value.** During an underrun the IRQ holds the last REAL sample,
so the anti-click behaviour 127 was chosen for is untouched; during playback every sample comes from the
ring. Cost is one DC step at the first sample.

**THE DIAGNOSTIC PATH MATTERS MORE THAN THE FIX — five theories died, and the two that mattered were killed
by USER OBSERVATIONS, not by reading code:**

| theory | killed by |
|---|---|
| underrun / 8-bit quantisation / stuck OPL voice (the three previously recorded here) | never tested; all wrong |
| room-load poll starvation | **"the feep starts at the chooser"** — no game, no song, no underrun |
| PWM carrier frequency | **"no feep on Pimoroni at 22 kHz"** sent me here; then 11,025 -> 44,100 Hz changed NOTHING |
| idle duty cycle | duty 0 silent, duty 127 feeps — confirmed by a one-line bisect |

**The lesson: when a symptom appears BEFORE the subsystem you suspect can possibly run, that fact alone
eliminates the whole subsystem.** "It is there at the chooser" was worth more than any amount of code
reading, and it was available from the start. A one-line bisect (`PICO_PWM_IDLE_LEVEL=0`) then settled in
one flash what two rounds of reasoning got wrong.

**STILL UNEXPLAINED, deliberately not papered over:** why the mapped target at idle 127 reportedly did NOT
feep. The frequency explanation that would have covered it is disproved. The fix is correct for both targets
regardless, since duty 0 is silent on any hardware.

**`PICO_PWM_CARRIER_MULT` (default 4) is KEPT but is NOT the fix** — it was built for the disproved carrier
theory. It runs the carrier 4x the sample rate (11,025 -> 44,100 Hz) and pops a sample every 4th IRQ, costing
~3% CPU. Kept as cheap insurance now that the output stage is known to be duty-sensitive; set to 1 to undo.

### RESOLVED (device-confirmed 2026-09-13) — PQ2 WITH SOUND RUNS on PIO, via a song size cap

**`PICO_SONG_MAX_BYTES` (default 32768, PIO + `PICO_PWM_AUDIO` only).** PQ2 with sound now plays: **8 rooms
traversed, ZERO OOMs**, peak `used` 398,464 vs the 442,440 that previously died 12 bytes short of a GC alloc.
This supersedes the long-standing "sound on PIO is an SQ3-specific build" verdict for the OOM specifically.

**Measured, not guessed.** A `[mem] SOUND` probe (`kgraphics.c` + refcount counters in `sci_memory.c`, both
`FSCI_PROBE_MEM`-gated) reported at room 46: `sndres=0 B refcnt=60947 B (3 blk)`. Two facts fell out:
`sndres=0` proves the resource side is already optimal (the evict-after-`songit_new` fix works, no cheap win
left there), and the 60,947 was **ONE song** — 59,153 B = PQ2's `sound.001`, its theme. One song was the
entire problem.

**Threshold chosen from measured DECOMPRESSED song sizes, not taste** (median ~1-2 KB; only outliers hurt):

| game | max | 2nd | 3rd | effect of a 32 KB cap |
|---|---|---|---|---|
| SQ3 (the config that WORKS) | 30,324 | 18,996 | 13,548 | **nothing skipped — not regressed** |
| PQ2 | 59,153 | 26,197 | 22,287 | 1 of 47 skipped |
| KQ4 | 41,747 | 41,490 | 24,031 | 2 of 80 skipped |

**TWO PLACEMENT BUGS, both mine, both device-caught — this is the part to read:**

1. **The cap must precede EVERY allocation, not sit in the allocator.** First version lived in
   `pico_decompress_alloc` (the `result->data` path). But `sci_malloc_sram(compressedLength)` for the INPUT
   buffer is allocated FIRST and is FATAL, so the cap was never reached: PQ2 halted at `decompress0` with the
   cap compiled in. It now runs as soon as type and size are known, which also skips the decompress work and
   makes it independent of `PICO_STREAM_DECOMPRESS`.
2. **A skipped song must NOT return a NULL iterator — that opens the SCI CONSOLE.** `build_iterator` returning
   NULL makes `sfx_add_song` print `[SFX] Attempt to add empty song` and return -1, which `SCRIPT_ASSERT_ZERO`
   turns into `script_debug_flag`: the debugger opens and the game stops. Fixed in `ksound.c` by treating a
   missing song exactly like NOSOUND at INIT (skip `sfx_add_song`, set `signal=-1`), and guarding the PLAY
   path with `song_lib_find` so a never-added song cannot leave `state=PLAYING` with `signal` unset (which
   would hang a script waiting for it). **Shared engine code, not `HAVE_PICO`-gated** — the NULL is
   mishandled identically on desktop, it just cannot occur there.

   **I had diagnosed hazard 2 correctly EARLIER in the session and then talked myself out of it**, because a
   log showed play continuing after `malloc 59169 failed`. I read "the game kept going" as proof the path was
   graceful without grepping for the console line, which was almost certainly present. Cheap check, skipped.

**Verified not a regression:** SQ3 on the same build prints no `[snd] song` line and plays its sounds.
PQ2 is quiet mainly because the ONE skipped song is its theme — that is the designed trade.

### RESOLVED (device-confirmed 2026-09-13) — ordinary-frame mixer starvation: `PICO_PWM_BUF_FRAMES` rate/30 -> rate/13

**Every ordinary-frame starvation is gone on PQ2.** Before: `demand 443`, `437` against `buf_size 367`.
After: only 2 starving lines in an 11-room run, at 1,240 and 1,499 -- both TRANSITIONS, both the category no
sane buffer covers. Run health: 0 OOMs, 0 console, peak `used` 407,776 of 475,104.

**The value is set from measured ordinary-frame demands, not from a rule of thumb**
(the old comment asserted "the real floor is about rate/22 ~ 500" -- the SQ3 data contradicts it):

| game | ordinary-frame demands |
|---|---|
| PQ2 | 443, 437 |
| **SQ3** | 477, 632, **831** |

SQ3 sets the bar AND is the config where PIO sound actually works, so it must not regress -- `rate/22` = 501
would have fixed PQ2 and left SQ3 sticking. `rate/13` = **848 frames = 77 ms**, clearing the measured worst
case (831) by 17 frames. `rate/11` = 1002 is the fallback with real margin, +1.8 KB from here.

| | frames | latency | heap total |
|---|---|---|---|
| rate/30 (old) | 367 | 33 ms | 4.3 KB |
| rate/22 | 501 | 45 ms | 5.9 KB |
| **rate/13 (now)** | **848** | **77 ms** | **9.9 KB** |
| rate/11 | 1002 | 91 ms | 11.7 KB |

**THIS ONLY BECAME AFFORDABLE BECAUSE OF THE SONG CAP.** Raising `buf_size` used to push the song allocation
over -- `rate/11` on PIO previously produced NO SOUND AT ALL for exactly that reason, which is what "pinned
between two moving limits" meant. Freeing ~59 KB moved one limit, and the two no longer strangle each other.
**When a value is stuck between two constraints, fixing either one may unstick it -- re-test the pinned value
after any memory change rather than treating it as settled.**

**Thin margin, stated plainly:** 848 vs SQ3's measured 831. PQ2's run does not exercise that. If an ordinary
demand above 848 ever appears, go to `rate/11`.

### RESOLVED (device-confirmed 2026-09-13) — PQ2 keeps ALL its music: large MIDI songs live in PSRAM (`PICO_PSRAM_SONGS`)

**PQ2's 59,153-byte theme now PLAYS on PIO** instead of being skipped by the cap. Device run: no
`[snd] song ... skipping`, no `Decompression failed`, no OOM, no fault, 8 rooms, and ONE starvation line in
the whole session. This supersedes the cap as the *mechanism* for large songs -- the cap becomes a safety net
for what a slot cannot hold.

| | resident song data | outcome |
|---|---|---|
| original | 60,947 B | OOM, 12 bytes short |
| `PICO_SONG_MAX_BYTES` cap | 1,794 B | runs, theme SILENT |
| **`PICO_PSRAM_SONGS`** | **1,794 B** | **runs, theme PLAYS** |

`refcnt` holding at 1,794 B -- the same as when the song was skipped entirely -- is the proof the 59 KB is
genuinely off the SRAM books rather than moved around.

**Why it works, and the measurements that made it cheap:**
- **SCI0 playback is a SINGLE forward cursor.** `sci0_song_iterator_t` has ONE channel; `channels[MIDI_CHANNELS]`
  is SCI1. That is what makes a 64-byte per-iterator window sufficient -- a few bytes per 60Hz tick, and
  `psram_load` moves 31 bytes per transaction so a refill is 3 of them every 10-20 ticks. **Device-measured: no
  new starvation from refills at all.**
- **Embedded-PCM songs are EXCLUDED and it costs nothing.** `_sci0_get_pcm_data` hands `self->data` straight to
  `sfx_iterator_make_feed`, which needs a real address, so PCM songs cannot stream. Measured: SQ3 has 23 of
  them (PQ2 and KQ4 zero) but the largest is **9,774 B**, while every song too big to keep is MIDI. The
  exclusion is free in practice.
- **Slots sit ABOVE the bump arena** (`0x710000`, 8 x 64 KB): `psram_alloc` is rewound by `psram_reset()` on
  every room change, and music plays across rooms.

**FOUR TRAPS, all caught by grep or by the device, none by reading the code:**
1. **`SONGDATA` was already taken** by the SCI1 path (`iterator.c:826`). Mine are `PSONG_*`.
2. **Two sites deref `self->data[0]`** to test for embedded PCM -- and `data` is NULL for a PSRAM song, so they
   would fault BEFORE the PCM check could protect anything. Routed through the accessor.
3. **`songit_new` uses `sci_malloc` with NO memset**, so `psram_addr` is garbage until assigned -- the same
   trap already recorded for `_gfxw_new_widget`. Initialised before any path can read it.
4. **`_SIMSG_BASEMSG_CLONE` memcpy's the iterator then increfs `mem->data`** -- NULL for a PSRAM song, and the
   two copies would share a slot with no ownership. Slots are refcounted; the clone takes a slot reference.

**THE CONFIGURATION CONFLICT, which only the device found:** the cap runs during DECOMPRESSION, long before
`songit_new` can park anything, so a 32 KB cap silently rejected the exact 59 KB song the slots exist for
(`[snd] song 1 ... skipping` then `Decompression failed`, PSRAM path never reached). `PICO_SONG_MAX_BYTES` now
follows the slot size automatically when `PICO_PSRAM_SONGS` is on. **Two mechanisms aimed at the same object
must be ordered deliberately; building them in sequence and never configuring them together is how this hid.**

**`PICO_STREAM_DECOMPRESS` is a PRECONDITION here**, which retires its own "worth ~nothing today" verdict:
without it the 59 KB compressed INPUT buffer is a FATAL `sci_malloc_sram`, so raising the cap alone would have
traded a silent skip for a hard halt. Banking it default-OFF rather than deleting it paid off for a reason not
anticipated at the time.

### MEASURED LIMIT (2026-09-13) — KQ4 does NOT fit with sound, and it is NOT song-related

Both configurations fail identically on KQ4, second visit to the first room:

| | failing alloc | free | arena |
|---|---|---|---|
| `PICO_PSRAM_SONGS=ON` | 12,532 | 27,672 | 458,500 |
| `PICO_PSRAM_SONGS=OFF` | 12,532 | **20,512** | **466,696** |

**The OFF build had LESS free and a HIGHER arena, so the PSRAM song work made KQ4 marginally BETTER.** The
failing allocation is `decompress0.c` `pico_decompress_alloc` at the `sci_malloc` branch -- the NON-sound path
(pic/view/script) -- and no KQ4 sound resource is 12,532 bytes. Decisively, the OFF build logged **zero**
`[snd] song` skips, so KQ4's two 41 KB songs never loaded on that route and **the slots were never exercised
in either run**.

So this is the general fragmentation wall on the heaviest game (150 pics, arena within 8 KB of the 475,104
ceiling). What sound costs KQ4 is the BASELINE stack (~7.7 KB `.bss` + ~10 KB mixer buffers), not song
storage. KQ4 had never been tested with sound before, so this is a newly-measured limit, not a regression.

**PIO sound scoreboard: SQ3 works, PQ2 works with ALL music, KQ4 does not fit** -- and the CHOOSER TOGGLE
makes that a per-game choice at launch rather than a build-time one (below).

### DONE (device-confirmed 2026-09-13) — per-game sound toggle in the chooser: ONE uf2 for every game

`[S]` in the game chooser toggles sound; the header line shows `[S] sound: ON` / `off`, and OFF simply appends
`-q` to the argv, i.e. the long-established silent path, not a new mechanism. A `[snd] launching with sound
ON/OFF` line records the choice.

**This is the answer the automatic shed was reaching for.** Read at LAUNCH it beats an in-engine heuristic on
every axis: no floor to mis-calibrate, no mid-game teardown, no restart hazard. Device-confirmed in ONE boot:
SQ3 with sound ON, KQ4 with sound OFF, 0 OOMs, 0 faults, 0 spurious sheds, 1 starvation line (a transition),
2 chooser reboots, peak `used` 405,000 of 475,104.

**TRAP caught before it shipped:** the sound build's `argv[]` was a bare initialiser list sized to EXACTLY its
6 entries, so appending `-q` wrote `argv[6]` -- one past the end, into whatever followed on the stack. It is
`char *argv[7]` now. That would have presented as a random fault far from the cause.

<!-- from CLAUDE.md lines 4049-4118 -->

### BUILT, DISABLED BY DEFAULT (2026-09-13) — graceful sound shedding: the mechanism works, the TRIGGER does not

`PICO_SOUND_SHED` tears the whole sound stack down at a `kDrawPic` boundary when free heap is low, so a heavy
game keeps PLAYING instead of dying for a decode buffer. **`PICO_SOUND_SHED_FLOOR` now defaults to 0 =
DISABLED**, because no usable floor was found.

**The mechanism is proven:** device run on KQ4 shed **22,416 bytes** (more than the ~14 KB predicted) and the
game kept playing. It is deliberately NOT hooked into `pico_reclaim_heap` — that runs inside a FAILED
allocation, the arbitrary point where `run_gc` already HardFaulted. It also explicitly NULLs `s->sound.song`,
because `sfx_exit` frees the song library but leaves the active-song pointer dangling and `sfx_poll`
dereferences it (`self->song->handle`). And it only works at all because the same day's `ksound.c` fix made a
missing song graceful; before that, shedding mid-game would have opened the SCI console.

**THE TRIGGER IS THE PROBLEM -- an off-by-a-PHASE calibration error.** The floor of 40960 was picked from
`room ready` (POST-decode) figures, but the check runs at `kDrawPic` entry = `room enter`, where free heap is
at its MINIMUM:

| game | `room ready` (used to calibrate) | `room enter` (what the check sees) |
|---|---|---|
| PQ2 | 20,376 - 75,120 | **16,952**, 32,408 |
| KQ4 | 35,632 - 62,280 | fired at **23,816** |

Every game is below 40960 at room-enter, so it fired on ALL of them: **SQ3 went silent and PQ2 would have
lost its music.** Right metric, wrong point in the cycle.

**Lowering the floor does not obviously rescue the idea:** KQ4 FAILED at 20,512 free while PQ2 runs FINE at
16,952 -- free-heap-at-room-enter does not separate "about to die" from "normal tight operation". The actual
failure was FRAGMENTATION (20,512 free vs a 12,532 request), which this metric does not measure at all.

**Second device finding, fixed: a shed must survive a RESTART.** A restart builds a fresh `state_t`, resetting
`s->sound.flags`, so sound re-initialised on a heap that had just proved it was short and HardFaulted --
`PC=0xd85a429e`, `CFSR=1` (IACCVIOL, i.e. a branch through a stale function pointer), `LR` in
`OPL_STATUS_RESET`. `pico_sound_was_shed` is now a global consulted by `game_init_sound`. **The exact dangling
pointer was never identified** (`opl2_exit` does null its own globals, so the obvious suspect is innocent);
the fix works by never re-entering the sound stack rather than by repairing the teardown.

**To revive this, measure first:** log `room enter` free across SQ3/PQ2/KQ4 on one build and check whether ANY
threshold separates KQ4-before-failure from the other two operating normally. If none does, shedding has to
trigger on a real failed allocation rather than a heuristic -- and that lands back on the unsafe hook, so it
would need a "shed requested" flag executed at the next safe point.

One untried lever, judged a poor trade: `buf_size` at `rate/13` costs ~5.6 KB over `rate/30`, and KQ4 failed
by 12,532 with 20,512 fragmented-free. A smaller buffer for KQ4 might squeeze it in at the cost of
reintroducing the audio sticking.

**Still open (unchanged by this):** the TRANSITION stalls, 150-250 ms and occasionally 1.5 s, caused by room
loads blocking the poll. Poll hooks exist in the pic-decode loop (`sci_pic_0.c`) and `_read()` (`pico_io.c`);
they help but are NOT sufficient, and no buffer size can cover a 1.5 s gap. Also unchanged: PQ2's theme is
still SKIPPED, which is the designed trade -- keeping ALL music needs the PSRAM song spike, now with a
measured target of ~59 KB in a single block.

⚠️ **OPEN: after a failed game, no further game starts** ("Please wait" then exit) until a power cycle.
  Not diagnosed. The obvious leaks were checked and are clean (`opl2_exit` does `OPLDestroy`, `mix_exit`
  frees the compbufs, `main.c:1481` calls `game_exit`→`sfx_exit`), so it is NOT a simple "sound never
  frees". Decisive next data: the chooser's `[mem] post-trim` line after the failed game (high arena =>
  teardown did not complete) plus any `malloc N failed` during the next load. **Worth fixing even if PIO
  sound is abandoned — the same failure would be just as bad on the mapped target.**

**Use 11025, not 22050, on PIO.** Memory differs by only ~4KB (the ring), but CPU compounds: 22050 costs
the synth ~35-47% vs ~20%, which lowers the frame rate, which lowers the poll rate -- while simultaneously
REQUIRING double the poll rate for the same buf_size (>=43Hz vs >=22Hz).

**Song loads now degrade instead of halting** (both targets). `sci_refcount_alloc` has exactly ONE caller,
`songit_new`'s memdup, so it is song-only and safe to make non-fatal: on PIO it uses
`pico_sram_alloc_soft()` and returns NULL, `songit_new` unwinds, and the game plays on silently rather than
dying because a piece of MUSIC did not fit. **`pico_sram_alloc_soft` keeps sci_malloc's reclaim-and-retry**
-- an earlier attempt used raw `malloc` and broke the intro, because allocations that only succeed AFTER a
reclaim are exactly the near-the-ceiling ones this targets. Not applied on mapped, where `sci_malloc` means
the PSRAM heap.

<!-- from CLAUDE.md lines 4886-5017 -->

**2. `PICO_PWM_BUF_FRAMES` is squeezed from BOTH SIDES; `rate/30` is the value that works TODAY.**
Device-measured on current master, 11025Hz:

| buf_size | SQ3 | PQ2 |
|---|---|---|
| `rate/11` (1002) | **NO SOUND AT ALL** | dies at vocab init |
| `rate/30` (367) | **plays** (occasional sticking) | dies later, at decompress |

**TOO SMALL starves the mixer, TOO LARGE does not fit.** Both limits are real and the window between them is
narrow.

- *Lower limit (starvation).* `soft.c`'s pre-existing `[sfx-mixer] Output starving: demand N > buf_size` is
  a **free poll-rate meter** -- `demand / rate` is the frame time. SQ3 reads demand 402/496 on ORDINARY
  frames = 36/44ms = **23-28Hz**, not the 60Hz that was assumed, so the floor is ~`rate/22` = 500. At
  `rate/30` = 367 ordinary frames do starve: the PWM IRQ holds `last_sample` and the audio sticks.
- *Upper limit (memory).* `rate/11` was correct on 2026-09-06 but master has since gained the composed
  surface and more; the extra ~7.6KB (compbuf `2*N*4` + feed `~4*N`) no longer fits. The SQ3 log shows the
  song's own `malloc 19008 failed`, which SUCCEEDS on retry at `rate/30`.

So `rate/30` is chosen with eyes open: occasional sticking beats no sound. **Re-measure if the memory
picture changes** -- this is not a constant, it is a value pinned between two moving limits.

**The multi-hundred-ms starving lines (demand 2400-9339) are EXPECTED, not a fault.** No sane buf_size
covers them (9339 frames = 112KB of compbuf); they are the ring's job. Only ordinary-frame lines (demand a
few hundred) indicate mis-sizing.

**ALSO TRIED AND REVERTED (2026-09-12): making the decompress INPUT buffer fail softly for sound.** The
graceful skip guards only the decompressed OUTPUT, so a song too big to read IN halts via
`pico_oom_report`. Routing `sci_sound` through `pico_sram_alloc_soft` looked like a clean symmetry fix, but
on device it correlated with SQ3 losing sound entirely and PQ2 dying EARLIER (vocab init, `realloc 4192`
with `free=264,400` -- itself unexplained). Reverted to get back to a known-good baseline. The asymmetry is
real and still worth fixing, but **one change at a time, from a state that is known to play**.

**3. PIO sound at 11kHz -- SETTLED (2026-09-12): SQ3 YES, PQ2 NO.**

**SQ3 plays with music.** Confirmed twice now (2026-09-06 and 2026-09-12) with
`-DPICO_PWM_AUDIO=ON -DPICO_SND_RATE=11025`. Use `rate/11` buffers (see item 2 -- shrinking them makes the
audio stick).

**PQ2 does NOT fit, measured twice at two different sites:**
- `sci_refcount_alloc` (song memdup, 9,019 B) with `free=104` -- true exhaustion.
- `decompress0.c:370` `sci_malloc_sram(compressedLength)` -- the COMPRESSED INPUT buffer for a 59,153-byte
  resource, with `free=48,760`, `arena=450,364` (24,740 below the 475,104 ceiling). `free < size`, so a
  genuine shortfall; even spending all remaining arena growth leaves 59KB *contiguous* wanted out of ~73KB
  scattered.

PQ2 is simply the heavier game (1843 vocab words vs SQ3's 1489, 540 resources) and sound's ~13KB resident
plus song data is what pushes it over -- **with sound OFF, that same allocation succeeds.** Note the
direction: the item-2 buffers are load-bearing, so there is no shrinking them to make room; `rate/11` is
~7.6KB *larger* than the build PQ2 was measured on, i.e. it fails harder, not closer.

**Verdict: sound on PIO is an SQ3-specific build.** The failure is a clean legible `[OOM]` halt, not a
fault, so trying it costs nothing but a power cycle.

**STREAMING THE COMPRESSED INPUT -- BUILT, VERIFIED, DEVICE-TESTED, and currently WORTH ~NOTHING
(`PICO_STREAM_DECOMPRESS`, default OFF; `PICO_STREAM_METHODS` default 1). Read the verdict before
spending any more time here.**

**VERDICT (2026-09-13): it works exactly as designed and has no live failure to fix.** Kept default-OFF as
banked insurance, NOT because it earns its place today.
- **With sound OFF (the shipping config) `METHODS=1` is INERT.** Every large method-0 block is a SOUND
  resource, and sound resources never load under `-q`: `ksound.c:228`'s `build_iterator` (the SCI0 path) sits
  *inside* `if (!(s->sound.flags & SFX_STATE_FLAG_NOSOUND))`. Largest method-0 NON-sound block in any of the
  three games is **327 bytes**.
- **With sound ON it works, but rescues nothing that matters.** Device-confirmed on PQ2: the
  `decompress0.c:370` fatal halt is GONE (`malloc 59165 failed` now lands on the *output* buffer, which
  routes through `pico_decompress_alloc` -> raw malloc -> NULL -> the existing graceful skip, and the game
  plays on). But PQ2+sound then dies anyway at `reg_t_hashmap.c:42` with **`chunks=24`** (unfragmented) and
  **used=442,272 of a 475,104 ceiling** -- TRUE EXHAUSTION, which streaming cannot touch. And SQ3+sound,
  which does work, has a largest method-0 sound block of only 9,774 bytes, so it was never failing.
- **`METHODS=7` addresses a real class** (the 7-19 KB method-1 blocks; the recorded 7,286-byte OOM at that
  same line was almost certainly one) **but costs 4.6 KB permanent `.bss` + 38% decompress time** -- and the
  measured failure above is steady-state exhaustion, where more `.bss` is strictly worse. It helps
  fragmentation-shaped failures and hurts exhaustion-shaped ones.
- **Both recorded failures at that line are otherwise accounted for**: the cross-game one is fixed by the
  chooser reboot, the sound one is on a config that does not fit regardless.

**THE PROCESS LESSON, which is the real return here: ask "which resources actually load in the SHIPPING
configuration?" BEFORE building anything.** That single question would have shown the method-0 prize is
sound-only and sound is off by default, and the work would have been scoped differently or skipped. Phases
were first sequenced by RESOURCE COUNT (wrong -- the problem is contiguity, so block SIZE binds), then
corrected to block size -- but REACHABILITY was never checked at all until after phase 3.

---

*Implementation detail (all verified, kept for whenever this is needed):*
 That buffer (`decompress0.c` `sci_malloc_sram(compressedLength)`)
is the largest single contiguous transient left on the resource path and a recorded OOM site. NB this is NOT
the thing that was tried and reverted before: that was a fixed permanently-resident 16KB *scratch* for the
same site, both too small (59,153 here) and net-negative in arena. Streaming is a different shape.

**Source is the FILE, not PSRAM** (corrects the earlier "stream it from PSRAM" framing): the buffer is filled
by a single `read(resh, buffer, compressedLength)` from an already-open fd, so the window refills straight
from the file and a PSRAM round-trip would buy nothing.

**All three SCI0 methods are streamable -- including the one that looked fatal** (verified by code read):

| method | access pattern | what the window needs |
|---|---|---|
| 0 uncompressed | `memcpy(result->data, buffer, len)` | nothing -- read *directly* into `result->data`, dropping the buffer AND the memcpy |
| 1 LZW (`decrypt1`) | `src[bytectr]`, `+1`, `+2` | forward, <=2 bytes lookahead |
| 2 Huffman (`decrypt2`) | a node table at `src+2` walked RANDOMLY by `getc2` (`node += next<<1`), concurrent with a forward bitstream | the table is bounded at **510 bytes** (`numnodes = src[0]`, a byte) -> copy it to SRAM up front, stream the rest |

**Gated on `PICO_STREAM_DECOMPRESS`, deliberately NOT on `HAVE_PICO`**, so the desktop differ can build and
prove both paths.

**`tests/decompdiff` proves it byte-identical** -- decompresses every resource twice (stock vs streaming),
comparing return code, `result->size` and every output byte. **2,139 resources across SQ3/PQ2/KQ4, 0 differ,
at every one of `METHODS=0/1/2/3/4/7`.** Per-method negative controls confirm each path is actually live
(mutate method 0's read -> exactly 32 diffs on SQ3; mutate the window -> 603 = 497+106, method 0 correctly
unaffected since it uses no window; mutate the node table -> 106). That rigour exists because `drvdiff`
passed three times on paths its scene never walked. See `tests/decompdiff/README`.

**MEASURED COST (device, SQ3 first five rooms, `[perf] decompress since last room`):** streaming all three
methods took decompress **594 -> 819 ms (+37.9%)**; method 1 alone with a 1 KB window and an unconditional
seek was 889 ms. The tuning that closed that gap (sequential-aware refill + 1 KB -> 4 KB window) recovered
only ~24% -- **the seek hypothesis was mostly WRONG; per-`read()` FatFS overhead dominates.** `METHODS=1`
costs **0 bytes `.bss`** (with method 1/2 off, `stream_win` and `stream_nodes` are unreferenced and the
compiler drops them) and +136 bytes of flash.

**INSTRUMENT TRAP, cost a device cycle:** the obvious timer `[perf] pic N decode` is USELESS here -- pics are
method 2 (or 0), NEVER method 1, so it is structurally blind to what streaming changed. An A/B on it came
back +0.3% having exercised the streaming path zero times. `[perf] decompress since last room` (decompress0.c
+ operations.c, `FSCI_PROBE_PERF`) times `decompress0` itself and is the right one.

**The one implementation subtlety already identified:** cap total reads at `compressedLength` so the file
position on return matches the stock path -- a read-ahead window that over-reads past the compressed block
would desync the caller.

**Still on the output side:** `result->data` (up to `result->size`) still needs contiguous SRAM. This removes
the input-side requirement only.


---

### PARKED (2026-09-28) — loudness: master volume implemented, `PICO_PWM_VOLUME` ceiling, loudness unconfirmed on device

**Report:** sound is too loud on the PicoCalc; only the hardware volume wheel controls it.

**Not a volume fix to port.** pico-286 has none (its PicoCalc PWM path outputs the 16-bit mix at full scale,
`src/pico-main.c:352`); shapones' PicoCalc `pwm_audio.hpp` has no gain stage either. The remembered fix is
rp2040-ili9341-infones `70f83e4` (I2S gain default 150% -> 100%, a different board).

**Not clipping (measured on desktop, mixer output in 10 s windows):** SQ3 intro peak 21,809-32,768, RMS
5,318-9,651 (-10..-16 dBFS), clipped <= 0.034% and only in the first window; PQ2 peak 20,341-26,832,
RMS 3,325-5,148, clipped 0. KQ4 was silent on desktop in that run. So the output is simply full scale into
the amplifier, and no ceiling has to sit below a clip point.

**Two changes:**
- `sfx_set_volume` / `sfx_get_volume` (`sfx/core.c`) were upstream stubs (`FIXME: Implement volume`): a
  game's kDoSound volume (0..15, passed shifted left by 15) was ignored and read back as 0. Now stored and
  returned; `sfx_master_level()` gives the 0..15 level. PQ2 sets 15 at startup. Desktop output is unchanged
  (no desktop PCM device applies it), but reading the volume back now returns the real level.
- `pico_pwm.c` applies `PICO_PWM_VOLUME` (percent, CMake, **default 50**) x master level as one gain in
  1/256ths at the single S16 -> 8-bit conversion. 100% at level 15 reduces exactly to the old
  `(src >> 8) + 128`, so `-DPICO_PWM_VOLUME=100` restores the previous output. Each halving costs one of the
  8 PWM bits (~6 dB). A `[snd] PWM volume = N%` startup line names the compiled value.

**Device, one quick test:** "still sounds like before". The log could not show which firmware was flashed
(the startup line was added afterwards), so whether 50% was actually heard is **unconfirmed**. Next time:
check the `[snd] PWM volume` line, then compare at the same wheel position. Also worth doing: the same game
in pico-286 at the same wheel position -- if FreeSCI is clearly louder, the fix belongs in the OPL synth
(`opl2.c` volume tables), not the output stage.

### PARKED (2026-09-28) — shrill drum sounds: aliasing at 11,025 Hz (analysis only)

Two causes, both confirmed in the code, neither fixed:
1. **Aliasing in the synth (likely dominant).** `opl2.c` renders the OPL directly at `SAMPLE_RATE`
   (11,025 on PIO, Nyquist 5.5 kHz) and never enables OPL rhythm mode (`0xBD` = `0xC0`, rhythm bit clear), so
   SCI0 drums are melodic FM patches with bright, noise-like spectra far above Nyquist, folding back as
   metallic inharmonic tones. Same mechanism as rp2040-ili9341-infones `6538f7c` (NES noise channel
   aliasing at 22,050).
2. **Imaging in the PWM output.** `pwm_synth.c` runs the carrier at 4x but HOLDS each sample for 4 carrier
   cycles (zero-order hold) instead of interpolating.

Options, cheapest first: (A) linear interpolation across the 4 carrier sub-steps in the PWM IRQ -- fixes the
imaging, nearly free; (B) a one-pole low-pass (~4 kHz) on the output as a CMake option -- tames the band
where aliases and harshness sit, slightly duller; (C) 22,050 Hz on PIO with a higher clock (at 133 MHz it
costs the synth 35-47% CPU vs ~20%, see "Use 11025, not 22050, on PIO" above); (D) oversample only the
synth -- same CPU as C. **Before building any of them:** render the same SQ3 music on desktop to WAV at
11,025 (as shipped), with A, with A+B, and at 44,100 as a reference, and compare by ear; and if the
Pimoroni board is at hand, listen there (22,050 Hz) -- clean drums there confirm aliasing.
