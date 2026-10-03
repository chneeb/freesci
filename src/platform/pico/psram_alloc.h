#pragma once
#ifdef HAVE_PICO

#include <stdint.h>
#include <stddef.h>
/* psram_spi_inst_t (and g_psram) are in psram_alloc.c / pico_main.c only;
   callers of these functions do not need the pico-sdk type. */

/* Recompute QMI flash timing for a raised system clock. MUST be called BEFORE
   set_sys_clock_khz raises the clock. PIO target only -- the mapped target has
   its own copy in psram_mapped.c. */
void     pico_set_flash_timings(int cpu_mhz, int flash_max_mhz);

uint32_t psram_alloc(size_t bytes);
void     psram_reset(void);
uint32_t psram_epoch(void);

/* ---- song slots: PSRAM that SURVIVES psram_reset -------------------------
   psram_alloc is a bump allocator that psram_reset() rewinds to 0 on every
   room change, so anything living there is gone the moment the player walks
   through a door.  Songs must outlive that.  These slots sit ABOVE the bump
   arena, alongside the parse scratch at 0x700000, and are never rewound.

   Layout: the bump arena grows from 0, the parse scratch owns
   [0x700000, 0x700000+64000), and song slots start at 0x710000.  PSRAM is
   8 MB, so there is ~960 KB up there for a handful of 64 KB slots -- the
   largest song measured in any SCI0 game we run is 59,153 B (PQ2 sound.001).

   A fixed slot table rather than a heap: songs are few and long-lived, so
   there is nothing for a real allocator to earn here. */
/* Staging slot for one VGA view's decompressed resource while it is decoded
   (scir_pico_load_to_psram -> gfxr_draw_view1_psram), above the PIO working
   priority map (0x790000 + 32,000). A fixed slot, not the arena: in a room
   that never changes picture (Jones in the Fast Lane's town board) the arena
   is not rewound, and a copy per view load would only grow it. */
#define PSRAM_VIEW_STAGE_ADDR 0x7A0000u
#define PSRAM_VIEW_STAGE_SIZE 0x10000u
/* The bump arena must stay below the fixed slots (the parse scratch at
   0x700000 is the lowest); psram_alloc halts legibly instead of overrunning. */
#define PSRAM_ARENA_LIMIT     0x700000u

#define PSRAM_SONG_BASE       0x710000u
#define PSRAM_SONG_SLOT_SIZE  65536u
#define PSRAM_SONG_SLOTS      8
#define PSRAM_SONG_NONE       0xffffffffu

uint32_t psram_song_alloc(size_t bytes);   /* refcount starts at 1 */
void     psram_song_incref(uint32_t addr);
void     psram_song_free(uint32_t addr);   /* decref; releases the slot at 0 */
int      psram_song_slots_used(void);
void     psram_store(uint32_t addr, const uint8_t *src, size_t len);
void     psram_load(uint32_t addr, uint8_t *dst, size_t len);

#endif /* HAVE_PICO */
