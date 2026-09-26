# Pico — text parser, vocab and Said history

> Moved verbatim out of `CLAUDE.md` on 2026-09-26 (original line ranges noted below). This is
> **historical record**: investigation logs, retracted theories and reverted experiments. Line numbers,
> "pending retest" markers and `.bss` figures reflect the date of each entry, not the current tree.
> The current state and the standing rules live in `CLAUDE.md`.

<!-- from CLAUDE.md lines 589-628 -->

### RESOLVED — SQ3 conveyor "jump" fails (bison `wordset` paren rule discarded the group)

The conveyor-shredder puzzle ("stand up" then "jump") was unwinnable: "stand up" worked, but "jump"
→ narrator "Check again" until Roger died in the shredder. **Not Pico-specific** — a shared-engine
FreeSCI bug. Diagnosed via the device parser trace (`debug_mode = pS`, see Debugging item 3) + the
rm10 disassembly (`/tmp/sq3_disasm/010.script`).

**Symptom mechanism:** bare "jump" spuriously **FULL-matched** rm10's **stand** Said-spec `0f2d`
(`(acquire<up),stand [<up] [/belt,conveyer]`, handler #9 of 13). Full match + no `>` marker →
`SAID_FULL_MATCH` → `PUT_SEL32V(parser_event, claimed, 1)` claimed the event, stopping the handler
chain before the **real** bare-jump handler `0f74` (`jump,leap[...] [/banister]`, handler #13 →
`setScript railJump`). Both "stand up" AND "jump" hit the identical
`augment_match_expression_p(): Empty condition → return 1` wildcard path, so the matcher could not
be fixed there (legit "stand" depends on it too) — the bug was upstream in the parser.

**Root cause — `said.y` / generated `said.c`, the `wordset` grammar production.** The rule
`wordset : YY_PARENO expr YY_PARENC` assigned **`$$ = $1`** (the open-paren *token's* `yylval`)
instead of `$$ = $2` (the parsed inner expression). Operator tokens never set `yylval` (`yylex`
only assigns it for `WGROUP` words, `said.y:267`), so `$1` was a **stale word value left over from a
prior parse** — a garbage tree index. This discarded the entire `(acquire<up)` group from the
required clause; with cross-parse `yylval` residue (belt = 0x924 from an earlier spec) it planted
the `(141 14f Error(0924))` degenerate node the device dump showed, emptying the required-word check
→ the empty-condition wildcard → "jump" matched.

**Fix (one rule, two files in lockstep):** `src/engine/said.y:201` `{ $$ = $1; }` → `{ $$ = $2; }`,
and the generated reduction `src/engine/said.c` case 19 `(yyvsp[(1) - (3)])` → `(yyvsp[(2) - (3)])`.
The required clause now correctly contains `acquire`/`stand`/`<up`; bare "jump" no longer matches
0f2d, "stand"/"stand up" still does. Because `$2` is a real node, the fix is immune to the
cross-parse residue that made the device case worse than a single isolated parse.

**Validated three ways:** (1) standalone harness feeding the exact 0f2d spec bytes through
`said_parse_spec` + `vocab_dump_parse_tree` — before/after trees confirm the required clause goes
from `Error(0924)` to `acquire/stand/<up`; (2) the same harness with case 19 reverted reproduces the
broken tree; (3) **device-confirmed (2026-06-07)** — pico.log `pS` trace shows "jump" now skips the
stand spec 0f2d (no "Match.") and the chain continues to the rail-jump spec, `railJump` fires, Roger
grabs the rail. **Blast radius:** affects every parenthesized word-group `(…)` in every SCI game's
Said specs (previously all silently dropped) — strictly more correct. The two analogous `<(…)` paren
rules (`said.y:234`, `248`, `wordrefset`/`recref`) share the same stale-operator pattern but were
left untouched (rarer, untested, not implicated here).

<!-- from CLAUDE.md lines 2246-2381 -->

1. **Vocab loading → re-enable the text parser. (DONE — device-validated on SQ3.)**

   **The text parser works on Pico.** Walk around, type "Look"/"stand up"/etc. in multiple rooms,
   ride the trash elevator + conveyor — eight+ parses in one session, no OOM, no fault, clean teardown
   (`free=399936 used=15748` back at the chooser → no leak). What it took:

   - **Re-enable the load** (`game.c` `_init_vocabulary`, `HAVE_PICO` branch): load
     words/suffices/branches resident; `parser_rules = NULL` (GNF rebuilt per command, NOT resident).
   - **Rebuild GNF per command** (`kstring.c` `kParse`, `HAVE_PICO`): `vocab_build_gnf` from the
     resident branches at the top of each parse, `vocab_free_rule_list` after — so the ~50KB rule list
     is a transient, not a permanent resident charge. (`s->parser_rules` stays NULL → no aliasing.)
   - **THE FIX that actually mattered — borrow the visual buffer to PSRAM during the parse**
     (`pico_driver.c` `pico_borrow_visual`/`pico_return_visual`, called from `kParse`). The GNF *build*
     (~50KB) plus the per-word candidate expansion in `vocab_gnf_parse` (`grammar.c:646-710`, the
     `_vinsert` double-loop at :690→:205) is a stacked transient that hit **~97KB for ambiguous commands
     like "stand up"** and OOM-halted at `grammar.c:205` (`_vinsert`, clean `[OOM]`, not corruption).
     The game is paused with the input window up and nothing draws during a parse, so `kParse` saves the
     64KB visual back-buffer to a fixed PSRAM scratch (`0x700000`, clear of the room bump arena), frees
     the SRAM, parses with **+64KB headroom**, then restores the frame byte-for-byte before returning to
     the VM. Device: room-2 `[gnf]` free-after-build went **23912 → 87264 B**; "stand up" (room 10) went
     **47800 B → OOM** → **117992 B, parses fine**. Cost: ~30ms PSRAM round-trip per parsed command (a
     paused moment, imperceptible). Residual risk (accepted): if a parse fragments the heap so the 64KB
     can't be reclaimed after, `pico_return_visual`'s `sci_malloc` halts with a legible `[OOM]` naming
     `pico_driver.c` — degrades loudly, never corrupts.
   - **Stage 2 word-packing** (`vocab_pack_words`, `vocab.c`; CMake `PICO_PACK_VOCAB`, default OFF):
     collapses the ~1489 per-word `sci_malloc`s into one allocation. **On device it bought only ~3KB**
     (vs the ~26KB the probe predicted — `vocab_get_words`' per-record malloc overhead was far smaller
     than estimated), so packing is NOT what made parsing fit — the PSRAM visual-borrow is. Kept as a
     cheap, harmless baseline trim behind its flag; not load-bearing.
   - **Stage 3 (PSRAM-resident words behind a `psram_set_floor()`) is NOT needed and was abandoned** —
     it would have saved roughly what packing did (~little), and the real lever was the transient parse
     peak, not the resident word baseline.

   The diagnostic `[gnf]` line (`kstring.c`, `HAVE_PICO`) prints the per-command rebuild's transient
   bytes + free heap. Leave it until the parser has more device mileage, then strip with the other probes.

   **FIXED (device-confirmed) — heavy-grammar GNF candidate-expansion OOM, fixed by a free-heap floor in
   `vocab_gnf_parse` (`grammar.c`, `HAVE_PICO`).** PQ2 (1843 words vs SQ3's ~1489) OOM-halted at
   `grammar.c:205` `_vinsert` on a multi-word ambiguous command ("lock car doors"). Root cause: the
   per-command candidate expansion in `vocab_gnf_parse` multiplies candidates **word-by-word** — for each
   non-final word, every surviving candidate with a remaining nonterminal is matched against the *entire*
   GNF rule list and `_vinsert`ed, so against a large grammar an ambiguous 3-word command fans out
   multiplicatively until the heap is exhausted (each `_vinsert` ≈ 248–268 B). This is a *transient
   parse-time* blowup, NOT a headroom problem — SQ3's small grammar parses fine with *less* free heap
   (~59–70 KB) than PQ2's failure point (~74 KB), so raising the baseline doesn't help; only the heavy
   grammar explodes.
   - **Fix:** inside the `subseeker`/`seeker` loops, every **64** subseeker iterations
     (`++pico_vinsert_ctr & 0x3f`) check `mallinfo().fordblks`; if free heap is below
     `PICO_GNF_HEAP_FLOOR` (**24 KB**), set `pico_floor_hit`, break both loops, free `reduced_rules`, and
     continue the parse with the candidates gathered so far. Prints a permanent telltale
     `[gnf] candidate expansion hit heap floor at word N/M, truncating (free=…B)`.
   - **Why truncation doesn't break parsing:** the explosion is mostly *junk* candidates — grammar
     ambiguity spawning thousands of alternative GNF paths, but the real sentence needs only **one** valid
     path, gathered early in the list. Lopping off the combinatorial tail keeps the matching candidate, so
     the command still resolves. Device-confirmed: "lock car doors" truncated at word 0 (free fell to a few
     KB) yet parsed correctly, three times; the 2-word case never hit the floor at all.
   - **Self-gating** (satisfies the "only large grammar" intent with no per-game threshold): small grammars
     never approach the 24 KB floor, so SQ3 is untouched in practice. Fully `HAVE_PICO`-gated (incl. the
     `<malloc.h>` include) → desktop is byte-for-byte unchanged. The interval was tightened 256→64 after the
     first device run showed a single 256-window could plunge free heap ~69 KB → ~3.4 KB (caught, but thin
     margin); at 64 it aborts nearer the floor. **`grammar.c` is hand-written, not generated** — no
     `grammar.y` to regenerate (contrast `said.c`←`said.y`).

   ---
   *Original design notes (kept for context / re-measuring other games):*

   `_init_vocabulary` (`game.c:63-85`, under `HAVE_PICO`) NULLs `parser_words`/`parser_rules`/
   `parser_suffices`/`parser_branches` to save ~80KB, so `kParse` matches an empty vocab and "look
   around" etc. do nothing. Re-enabling just means running the existing `#else` branch
   (`game.c:87-96`) on Pico too. This is **additive** SRAM spend, not a saving (vocab is NULL today),
   so it does **not** unblock the control-map item — keep them decoupled.

   **Code audit (verified, corrects the earlier lifetime note):** the four structures and their real
   access patterns —
   | Structure | Type | Loaded by | Read by | Lifetime |
   |---|---|---|---|---|
   | `parser_words` | `word_t**`, ~900 entries each its own `sci_malloc` | `vocab_get_words` (`vocab.c:72`) | `vocab_tokenize_string` in **kParse** (bsearch+strcmp) | per-kParse |
   | `parser_suffices` | `suffix_t**`, tens | `vocab_get_suffices` | `vocab_tokenize_string` in **kParse** | per-kParse |
   | `parser_branches` | `parse_tree_branch_t*` flat array, 44 B each | `vocab_get_branches` | `vocab_build_gnf` (init) + `vocab_gnf_parse` in **kParse** | per-kParse |
   | `parser_rules` | GNF linked list (`parse_rule_list_t`, pointer-chasing) | `vocab_build_gnf` (`grammar.c:518`) from branches | `vocab_gnf_parse` in **kParse** | per-kParse |

   **DECISIVE: all four are consumed entirely *within* `kParse`.** The only artifact crossing the
   `kParse → kSaid` boundary is `parser_nodes[500]` (`engine.h:230`), which is **already a resident
   fixed array in `state_t`** — `kSaid` (`said.c:2528`) reads `parser_nodes` only, never the rules.
   So the old "rules must survive until Said runs" worry is **WRONG**: rules can be freed at the end
   of `kParse`. (`vocab_gnf_parse`, the only rule consumer, is called from `kstring.c:331` in kParse,
   never from said.c.) Also: `parser_rules` is **input-independent** — a pure function of
   `parser_branches`, identical all game; the only question is resident-once vs rebuilt-per-command.

   **PSRAM lifetime gotcha (the real constraint):** `psram_alloc` (`psram_alloc.c:12`) is a single-
   offset bump allocator; `psram_reset()` rewinds to **0** on **every room change**
   (`gfxr_free_all_pics`). Vocab must survive room changes → it cannot sit in the resettable region.
   **Required:** add a **floor** — allocate vocab at boot, then make `psram_reset()` rewind to the
   floor (above vocab) not 0. PSRAM is 8MB vs ~80KB vocab, so space is a non-issue.

   **Recommended design (option B, refined):**
   - *Init:* load words/suffices/branches; pack `parser_words` into **one contiguous PSRAM blob**
     (offset table + packed records), replacing the ~900 small `sci_malloc`s (themselves a
     fragmentation source); keep branches+suffices small-SRAM-resident (~3-4 KB, cheap) or in the
     blob; `psram_set_floor()`; free the SRAM originals.
   - *Per kParse:* page the words blob into **one** ~23 KB SRAM scratch (not 900 allocs), build GNF
     rules in a transient SRAM arena, parse into `parser_nodes`, free scratch + rules.
   - *kSaid:* unchanged. Net steady-state SRAM ≈ 0; per-command cost is a few large allocs.

   **Measure first (step 1) — DONE. The `PICO_VOCAB_PROBE` build captured the numbers (SQ3):**
   ```
   [vocab] words=1489 cur=53080B packed=21419B | suffices=48 2400B | branches=60 3912B
   [vocab] GNF rules=395 nodes=395 resident=41616B (uord+50272B) build_peak=44664B
   ```
   - **Words:** 1489 entries cost **53 080 B** as ~1489 separate `sci_malloc`s (a fragmentation
     source); they **pack to 21 419 B** in one blob (offset table + `2B class + 2B group + str\0`
     records) → ~31.7 KB saved by packing alone.
   - **Suffices:** 48 / 2 400 B. **Branches:** 60 / 3 912 B. Both tiny → keep SRAM-resident.
   - **GNF rules: 395 rules, 41 616 B resident, 44 664 B transient build peak.** (`uord+50272B`
     includes the rules' own slack/alloc overhead; the byte-accurate figure is 41 616 B.)
   - **DECISION — rebuild-per-command.** 41.6 KB resident is firmly in the "large (40 KB+)" bucket of
     the tradeoff, so the GNF rules are **rebuilt inside each `kParse`** in a transient SRAM arena and
     freed at the end of the call, NOT kept resident. Steady-state vocab SRAM then ≈ branches+suffices
     (~6.3 KB) + the per-command scratch; the ~44.7 KB build peak is paid only while parsing a typed
     command. Total all-resident would have been ~69 KB (matches the old "~80 KB" estimate).
   - **Peak caveat to validate in code:** the ~44.7 KB GNF build peak coincides with paging the words
     blob to an SRAM bsearch scratch (~21 KB) — those two transients must not stack badly mid-room
     (SQ3 already runs near the ceiling). Measure the combined per-command peak before locking the
     scratch sizes.
   - Probe is THROWAWAY (`PICO_VOCAB_PROBE`, OFF by default): `grammar.c` `_gnf_rule_bytes[_peak]` +
     `GNF_ACCOUNT/UNACCOUNT` macros (compiled out when off → non-probe builds byte-identical), and the
     `_init_vocabulary` measurement block in `game.c`. Keep it for re-measuring other games; leave OFF.

   **File-change checklist:** `psram_alloc.{h,c}` (add `psram_set_floor()`); `game.c`
   `_init_vocabulary`/`_free_vocabulary` (Pico load→pack→floor path); `vocab.c` (pack + packed-blob
   read/bsearch helper); `kstring.c` `kParse` (page scratch + transient GNF + free after parse);
   optional `vocab_psram.c` for the helpers. **Risks:** per-command GNF rebuild CPU (≤30 fixed-point
   passes, once per typed command — measure); PSRAM read latency in bsearch (paging whole blob to
   SRAM scratch likely beats per-compare PSRAM reads); `synonyms` are loaded separately by scripts
   (`kSetSynonyms`), already work on Pico — out of scope.

