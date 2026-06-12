# PicoCalc SQ3 SRAM/PSRAM Ceiling Assessment

Date: 2026-06-12

Scope: follow-up assessment of whether Space Quest 3 can realistically fit in
the RP2350 SRAM budget on PicoCalc while using the slow SPI PSRAM effectively.
This supersedes parts of `PICO_SRAM_PSRAM_REVIEW.md`, which is now stale.

## Bottom line

SQ3 can probably be made to fit "well enough" without sound, but the port is
already at the practical SRAM ceiling. I would not expect another clean 50-100 KB
win. The remaining work is no longer "move obvious big buffers to PSRAM"; it is
heap-fragmentation engineering and transient-allocation control.

If the target is SQ3 with parser, collision, repeated restores, and sound, I
would stop treating this as likely to fit comfortably. If the target is SQ3
playable without sound, accepting tight margins and a few targeted allocator /
scratch fixes, one more focused optimization pass is still defensible.

## Why the older review is stale

Several items in `PICO_SRAM_PSRAM_REVIEW.md` have already been overtaken by the
current tree and `CLAUDE.md`:

- PWM audio is now gated by `PICO_PWM_AUDIO` and defaults OFF.
- The parser/vocab path has moved forward substantially; packed vocab and
  visual-buffer PSRAM borrow for parse are documented in `CLAUDE.md`.
- The picture decode path now has permanent priority scratch, permanent
  pic/view decompress output scratch, and compressed-input scratch.
- View cel `index_data` is already offloaded to PSRAM after decode.
- Control map decode is default ON in the current-feature build, with graceful
  degradation on control-map allocation failure.
- The old "big obvious PSRAM candidates" are mostly already done or ruled out.

## Clean build versus diagnostic build

The latest `pico.log` is from a diagnostic-style build with expensive probes
enabled, so it should not be used directly as the final viability number.

Measured builds:

```text
arm-none-eabi-size

text    data  bss    dec     file
790180  0     29796  819976  /tmp/freesci-pico-clean/src/freesci.elf
794988  0     56076  851064  build-pico/src/freesci.elf
796836  0     29792  826628  build-pico-off/src/freesci.elf
```

The clean current-feature build was configured with:

```text
FSCI_PROBE_ARENA=OFF
FSCI_PROBE_MEM_CENSUS=OFF
FSCI_PROBE_MEM=OFF
FSCI_PROBE_GFX=OFF
FSCI_PROBE_PARSER=OFF
FSCI_PROBE_STR=ON
PICO_CONTROL_MAP=ON
PICO_PACK_VOCAB=ON
PICO_PWM_AUDIO=OFF
```

Relevant linker symbols:

```text
clean current-feature:
__end__       = 0x2000f154
__StackLimit  = 0x20080000
__StackTop    = 0x20082000
heap span     = 462508 bytes

diagnostic build-pico:
__end__       = 0x200157fc
__StackLimit  = 0x20080000
heap span     = 436228 bytes

older build-pico-off:
__end__       = 0x20010d80
__StackLimit  = 0x20080000
heap span     = 455296 bytes
```

So the diagnostic probes cost about 26 KB of heap ceiling versus the clean
current-feature build. That is enough to turn "barely works" into "fails early",
but it does not create comfortable headroom.

## Current failure shape

The important signal from the latest logs is that failures have moved downward:
old walls were 64 KB and 32 KB contiguous allocations; current failures can be
ordinary 7-12 KB allocations after restores / room transitions.

That means the port is no longer blocked by one obvious large resident buffer.
It is near the heap ceiling, and picolibc heap fragmentation is denying small or
medium contiguous runs even when total free bytes exist.

This also means PSRAM capacity itself is not the bottleneck. The bottleneck is
the amount of live SRAM required by FreeSCI's VM, resource structures, parser,
display buffer, permanent scratches, and transient decode/parse allocations.

## PSRAM reality check

PSRAM is already being used for the right kind of data:

- decoded background visual map
- priority/control maps
- decoded view cel index data
- fixed visual scratch during parser operations
- raw pic data streaming during decode

The tempting remaining target, `script_t.buf`, is not a read-only resource cache.
It is hot read-write VM working memory: bytecode, locals, object properties, and
mutated script state live in that buffer. Moving it to slow SPI PSRAM would be a
major VM/cache redesign and probably not playable as a simple offload.

Conclusion: there is probably no remaining "magic PSRAM migration" that makes
SQ3 comfortably fit.

## Remaining realistic optimization potential

### 1. Test only clean builds for viability

Diagnostic firmware is still useful, but it changes the answer by around 26 KB.
Use it to identify causes, then retest clean:

```bash
cmake -B build-pico-clean \
  -DPLATFORM=pico \
  -DPICO_SDK_PATH=~/Source/pico-sdk \
  -DPICO_BOARD=pico2 \
  -DFSCI_PROBE_MEM_CENSUS=OFF \
  -DFSCI_PROBE_ARENA=OFF \
  -DFSCI_PROBE_MEM=OFF \
  -DFSCI_PROBE_GFX=OFF \
  -DFSCI_PROBE_PARSER=OFF \
  -DFSCI_PROBE_STR=ON \
  -DPICO_CONTROL_MAP=ON \
  -DPICO_PACK_VOCAB=ON \
  -DPICO_PWM_AUDIO=OFF
cmake --build build-pico-clean -j$(nproc)
```

### 2. Reusable transient view-cel decode scratch

This is the best remaining engineering target.

View cel `index_data` is already offloaded to PSRAM after decode, but the cel
still allocates a transient SRAM `index_data` buffer during decode. The current
failure class includes small/medium allocation failures in this area. Decoding
view cels into a reusable scratch, then immediately storing to PSRAM, would
attack transient fragmentation directly without adding steady-state SRAM.

This is a better next pass than looking for another resident structure to move.

### 3. Shrink control-map transient peak only if a room needs it

Control-map decode is now broadly working, but it remains a transient peak risk.
If a new room fails specifically on the control pass, possible levers are:

- bit-pack or otherwise shrink the aux/flood-fill scratch
- temporarily offload or stage priority/control data during decode
- selectively skip control map for non-critical rooms as a degraded mode

This should be reactive to a specific failing room, not a broad first move.

### 4. Consider a small custom transient arena

The current problem is allocator fragmentation. A resettable SRAM arena for
known short-lived decode/parse buffers may buy more reliability than further
individual `malloc` tweaks.

Candidate users:

- view-cel decode temporary buffers
- picture/control decode temporary buffers where lifetimes are clearly serial
- parser GNF transient allocations, if they continue to stress the heap

The risk is lifetime mistakes. This should be narrow and explicit, not a
general allocator replacement.

## Things I would not keep chasing

- More static `.bss` mining: already largely tapped out.
- Audio while SRAM headroom remains this tight: dynamic OPL/PWM path likely
  spends too much heap unless major headroom appears.
- Read-only PSRAM resource cache for SCI0 scripts: ruled out by the mutable
  `script_t.buf` execution model.
- Re-enabling GC from arbitrary allocation failure: previous notes indicate it
  can fault in unsafe moments and is not the real lever for fragmentation OOMs.
- `malloc_trim`: documented as tried and ineffective / harmful in `CLAUDE.md`.

## Decision recommendation

Continue only if the goal is:

- SQ3 playable without sound
- control map enabled where it fits
- clean-build testing
- one more focused pass on transient scratch allocation / fragmentation

Stop if the goal is:

- robust SQ3 across arbitrary restore chains and long sessions
- sound enabled
- no degraded collision cases
- comfortable memory margin

If the next clean build still dies during normal play on 5-10 KB allocations
after a reusable view-cel decode scratch or equivalent transient arena work,
I would call that the SRAM ceiling rather than keep optimizing indefinitely.
