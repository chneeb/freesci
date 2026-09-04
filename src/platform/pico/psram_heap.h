/* Minimal malloc/free/realloc heap over a fixed region — used on the Pimoroni
 * mapped-PSRAM target to hold long-lived VM working memory (clone/node/list
 * tables, later script bufs / the seg-manager heap) in the 8 MB PSRAM instead of
 * the 475 KB SRAM, dissolving the SRAM ceiling.
 *
 * Why not the offload bump allocator (psram_alloc/psram_reset): that arena is
 * rewound to 0 on every room change and never frees, so it cannot hold data that
 * (a) must survive room changes and (b) is realloc-grown + freed repeatedly (the
 * clone table would leak unboundedly). This is a proper first-fit free-list with
 * lazy coalescing instead.
 *
 * The core operates on any (base,size) so it can be unit-tested on the host with a
 * plain byte array; on device, base = PSRAM_XIP_BASE + PSRAM_HEAP_OFFSET.
 */
#pragma once
#include <stddef.h>

void  psram_heap_init(void *base, size_t size);
void *psram_hmalloc(size_t n);
void  psram_hfree(void *p);
void *psram_hrealloc(void *p, size_t n);
int   psram_heap_owns(const void *p);   /* 1 if p was handed out by this heap */

/* Diagnostics */
size_t psram_heap_used(void);            /* bytes currently allocated (payload+hdr) */
size_t psram_heap_free_largest(void);    /* largest contiguous free payload */
