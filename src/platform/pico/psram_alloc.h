#pragma once
#ifdef HAVE_PICO

#include <stdint.h>
#include <stddef.h>
/* psram_spi_inst_t (and g_psram) are in psram_alloc.c / pico_main.c only;
   callers of these functions do not need the pico-sdk type. */

uint32_t psram_alloc(size_t bytes);
void     psram_reset(void);
void     psram_store(uint32_t addr, const uint8_t *src, size_t len);
void     psram_load(uint32_t addr, uint8_t *dst, size_t len);

#endif /* HAVE_PICO */
