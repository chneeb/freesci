/* view_arena.h -- a fixed SRAM block for decoded VGA view metadata (PIO only).

   Jones in the Fast Lane keeps one picture up all game, so its views are
   decoded, evicted (GFXR_VIEW_BUDGET) and re-decoded for hours. Each decode is
   a few hundred small blocks (view, loops, cel pointer arrays, 92-byte pixmaps,
   the palette insert list); on the main heap that churn fragments everything
   around it. Here the churn stays inside one block allocated while the heap is
   still clean. The __wrap_malloc chokepoint (pico_mem_census.c) routes
   allocations here while g_view_arena_active is set, and frees/reallocs/
   malloc_usable_size here by ownership. Plain C, no Pico dependencies, so the
   allocator is also built by tests/viewarena. */
#ifndef VIEW_ARENA_H
#define VIEW_ARENA_H

#include <stddef.h>

extern int g_view_arena_active; /* nonzero: malloc/calloc go to the arena first */

/* mem must be 8-byte aligned; size is rounded down to a multiple of 8.
   mem NULL detaches the arena (only when nothing in it is live). */
void view_arena_init(void *mem, size_t size);
void *view_arena_base(void); /* NULL until initialised */
void *view_arena_alloc(size_t n); /* NULL when nothing fits (caller falls back to the heap) */
void view_arena_free(void *p);
int view_arena_owns(const void *p);
size_t view_arena_usable(const void *p);
size_t view_arena_largest(void); /* largest payload one alloc can get now */
size_t view_arena_free_bytes(void); /* payload bytes free in total */
int view_arena_check(void); /* 0 if the block chain is consistent (tests) */

#endif
