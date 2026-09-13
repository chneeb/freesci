#pragma once
#ifdef HAVE_PICO

#include <stdint.h>
#include <stddef.h>
/* psram_spi_inst_t (and g_psram) are in psram_alloc.c / pico_main.c only;
   callers of these functions do not need the pico-sdk type. */

uint32_t psram_alloc(size_t bytes);
void     psram_reset(void);

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
