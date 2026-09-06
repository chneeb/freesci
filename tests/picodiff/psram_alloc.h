#ifndef PSRAM_ALLOC_H
#define PSRAM_ALLOC_H
#include <stdint.h>
#include <stddef.h>
uint32_t psram_alloc(size_t bytes);
void     psram_reset(void);
void     psram_store(uint32_t addr, const uint8_t *src, size_t len);
void     psram_load(uint32_t addr, uint8_t *dst, size_t len);
#endif
