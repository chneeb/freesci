# Heap Growth Considerations on Pico

Unresolved memory growth concerns for the FreeSCI Pico port. Each entry documents a heap allocation with no explicit upper bound on Pico (256KB sbrk heap).

---

## 1. Pathfinding VIS_MATRIX

- **File:** `src/engine/kpathing.c:1433`
- **Allocation:** `vis_matrix = sci_calloc(vertices × vertices, sizeof(char))`
- **Risk:** `O(vertices²)` — a game with 1000 pathfinding vertices consumes ~1 MB.
- **Details:** The pathfinding system (likely used in games like SQ3 for NPC movement) allocates a full adjacency/matrix buffer. No cap is applied on `vertices`.
- **Impact:** Can dominate the entire Pico heap if a map has a large waypoint set.

## 2. SCI Script Heap Size

- **File:** `src/engine/heap.c:44`
- **Allocation:** `SCI_HEAP_SIZE` fixed heap per script
- **Risk:** Per-script object allocation bounded only by heap size.
- **Details:** Each instantiated script gets its own object heap. The total memory footprint grows linearly with the number of cached scripts.
- **Impact:** Already mitigated by `max_memory = 1` in LRU cache (`src/scicore/resource.c:855`), so only one script's heap is live at a time.

## 3. Text Layout Fragment Arrays

- **File:** `src/gfx/font.c:212,246`
- **Allocation:** `fragments` array, doubles on each expansion pass during layout
- **Risk:** Peaks during rendering of complex multi-line dialogue or long status-bar strings.
- **Details:** `text_fragment_t` array grows dynamically during the text layout pass, doubling each time. The fragments are freed after rendering.
- **Impact:** Transient spike during layout; bounded by total output string length.

## 4. Drawn Pic Cache

- **File:** `src/engine/kgraphics.c:1163`
- **Allocation:** `s->pics = sci_realloc(pics, sizeof(drawn_pic_t) * ++s->pics_nr)`
- **Risk:** Unbounded growth as picts are cached.
- **Details:** Every time a pic is drawn it is appended to the cached `s->pics` array. No upper limit on the number of cached picts. The array is freed in `_free_graphics_input()`.
- **Impact:** Transient during gameplay. Could be a problem in games with very large animated sequences.

## Summary

| # | Allocation | Worst case (Pico 256KB heap) | Mitigation |
|---|---|---|---|
| 1 | `kpathing.c:1433` VIS_MATRIX | ~1 MB for 1000 vertices | Monitor vertex count in game data; consider `O(√heap_budget)` cap |
| 2 | `heap.c:44` SCI heap | Per-script (game-dependent) | LRU `max_memory=1` limits to one script |
| 3 | `gfx/font.c:212` fragments | Bounded by string length | Transient; freed after render |
| 4 | `kgraphics.c:1163` pics | Unbounded per session | Transient; freed on engine shutdown |

The most critical is (#1) pathfinding VIS_MATRIX because it can easily exceed the total available heap in a single allocation. If SQ3 or similar games use maps with large waypoint sets, this should be proactively addressed.
