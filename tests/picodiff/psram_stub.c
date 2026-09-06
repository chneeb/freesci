#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define ARENA (8u*1024*1024)
static uint8_t arena[ARENA];
static uint32_t top = 0;
uint32_t psram_alloc(size_t n){ uint32_t a = top; top += (n + 7u) & ~7u; return a; }
void psram_reset(void){ top = 0; }
void psram_store(uint32_t a, const uint8_t *s, size_t n){ memcpy(arena + a, s, n); }
void psram_load(uint32_t a, uint8_t *d, size_t n){ memcpy(d, arena + a, n); }
