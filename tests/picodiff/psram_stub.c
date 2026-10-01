#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define ARENA (8u*1024*1024)
static uint8_t arena[ARENA];
static uint32_t top = 0;
/* Exactly the device allocator (psram_alloc.c): no rounding. The VGA view
   re-decode (sci_view_1.c) relies on cels landing where the first decode put
   them, which only holds if this models the real bump pointer. */
uint32_t psram_alloc(size_t n){ uint32_t a = top; top += (uint32_t) n; return a; }
void psram_reset(void){ top = 0; }
void psram_store(uint32_t a, const uint8_t *s, size_t n){ memcpy(arena + a, s, n); }
void psram_load(uint32_t a, uint8_t *d, size_t n){ memcpy(d, arena + a, n); }
