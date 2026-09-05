/* First-fit free-list heap with lazy coalescing. See psram_heap.h.
 *
 * Layout: the region is a contiguous run of blocks, each an 8-byte header
 * { uint32_t size (payload bytes, 8-aligned); uint32_t used; } followed by its
 * payload. next(b) = (char*)b + HDR + b->size. Free blocks are found by walking
 * the implicit list; adjacent free blocks are merged lazily during the malloc
 * walk (so free() is O(1) and no memory is lost to fragmentation across a
 * free/alloc cycle). Correctness is covered by tests/psram_heap_test.c. */
#include "psram_heap.h"
#include <string.h>
#include <stdint.h>

#define HDR      8u                 /* sizeof(blk_t), also the alignment */
#define ALIGN(n) (((n) + 7u) & ~7u)
#define MIN_PAY  8u                 /* smallest payload we bother splitting for */

typedef struct { uint32_t size; uint32_t used; } blk_t;

static uint8_t *s_base;
static uint8_t *s_end;

/* Next-fit search cursor. A plain first-fit restart at s_base costs O(blocks)
   per malloc, and every step of that walk reads a block header OUT OF PSRAM over
   the QSPI link -- so a workload that allocates thousands of small blocks (the
   GNF parser's _vinsert, ~250 bytes a time) degrades to O(n^2) PSRAM reads and
   becomes visibly slow. Resuming the search where the last allocation ended
   makes a run of allocations roughly linear instead.

   INVARIANT: s_rover always points at a valid block START. Coalescing can absorb
   the block it points at, which would leave it addressing payload bytes that
   would then be misread as a header -- so every place that absorbs a block must
   re-point the rover at the absorbing block (see COALESCE_ABSORB). */
static blk_t   *s_rover;

/* Absorb free block NB into B, keeping the rover invariant. */
#define COALESCE_ABSORB(b, nb)                  \
    do {                                        \
        if ((nb) == s_rover) s_rover = (b);     \
        (b)->size += HDR + (nb)->size;          \
    } while (0)

static inline blk_t *blk_at(void *p)        { return (blk_t *)((uint8_t *)p - HDR); }
static inline void  *pay_of(blk_t *b)        { return (uint8_t *)b + HDR; }
static inline blk_t *next_blk(blk_t *b)      { return (blk_t *)((uint8_t *)b + HDR + b->size); }

void
psram_heap_init(void *base, size_t size)
{
    s_base = (uint8_t *)base;
    s_end  = s_base + size;
    blk_t *b = (blk_t *)s_base;
    b->size = (uint32_t)(size - HDR);
    b->used = 0;
    s_rover = b;
}

void *
psram_hmalloc(size_t n)
{
    uint32_t want = (uint32_t)ALIGN(n ? n : 1);
    blk_t   *start = s_rover;
    int      phase;

    if (!s_base)
        return NULL;                 /* heap not initialised yet (early boot) */
    if ((uint8_t *)start < s_base || (uint8_t *)start >= s_end)
        start = (blk_t *)s_base;

    /* Next fit: phase 0 scans [rover, end), phase 1 wraps to [base, rover). */
    for (phase = 0; phase < 2; phase++) {
        blk_t   *b   = (phase == 0) ? start : (blk_t *)s_base;
        uint8_t *lim = (phase == 0) ? s_end : (uint8_t *)start;

        for (; (uint8_t *)b < lim; b = next_blk(b)) {
            if (b->used)
                continue;
            /* Lazy coalesce: absorb any immediately-following free blocks. */
            for (blk_t *nb = next_blk(b);
                 (uint8_t *)nb < s_end && !nb->used;
                 nb = next_blk(b))
                COALESCE_ABSORB(b, nb);

            if (b->size < want)
                continue;

            /* Split off the tail if there is room for another usable block. */
            if (b->size >= want + HDR + MIN_PAY) {
                blk_t *tail = (blk_t *)((uint8_t *)b + HDR + want);
                tail->size = b->size - want - HDR;
                tail->used = 0;
                b->size = want;
            }
            b->used = 1;
            /* Resume the next search just past this block. */
            s_rover = next_blk(b);
            if ((uint8_t *)s_rover >= s_end)
                s_rover = (blk_t *)s_base;
            return pay_of(b);
        }
    }
    return NULL;
}

void
psram_hfree(void *p)
{
    blk_t *b;
    if (!p)
        return;
    b = blk_at(p);
    b->used = 0;           /* coalescing is deferred to the next malloc walk */
    /* Reuse just-freed space promptly: without this the rover would march to
       the end of the heap during an alloc/free-heavy burst (the parser) and
       only find the freed blocks after a full wrap. b is a valid block start,
       so the rover invariant holds. */
    if (b < s_rover)
        s_rover = b;
}

void *
psram_hrealloc(void *p, size_t n)
{
    if (!p)
        return psram_hmalloc(n);
    if (n == 0) {
        psram_hfree(p);
        return NULL;
    }
    blk_t *b = blk_at(p);
    uint32_t want = (uint32_t)ALIGN(n);

    if (b->size >= want)
        return p;                          /* shrink/same: keep the block as-is */

    /* Try to grow in place by absorbing following free blocks. */
    uint32_t avail = b->size;
    blk_t *nb = next_blk(b);
    while ((uint8_t *)nb < s_end && !nb->used && avail < want) {
        avail += HDR + nb->size;
        if (nb == s_rover)                 /* keep the rover on a block start */
            s_rover = b;
        nb = next_blk(nb);
    }
    if (avail >= want) {
        b->size = avail;                   /* absorb the run of free blocks */
        if (b->size >= want + HDR + MIN_PAY) {   /* return the excess */
            blk_t *tail = (blk_t *)((uint8_t *)b + HDR + want);
            tail->size = b->size - want - HDR;
            tail->used = 0;
            b->size = want;
        }
        b->used = 1;
        return p;
    }

    /* Relocate. */
    void *np = psram_hmalloc(n);
    if (np) {
        memcpy(np, p, b->size);            /* b->size = old payload (< want) */
        psram_hfree(p);
    }
    return np;
}

int
psram_heap_owns(const void *p)
{
    return p && (const uint8_t *)p >= s_base + HDR && (const uint8_t *)p < s_end;
}

size_t
psram_heap_used(void)
{
    size_t used = 0;
    for (blk_t *b = (blk_t *)s_base; (uint8_t *)b < s_end; b = next_blk(b))
        if (b->used)
            used += HDR + b->size;
    return used;
}

size_t
psram_heap_free_largest(void)
{
    size_t best = 0, run = 0;
    for (blk_t *b = (blk_t *)s_base; (uint8_t *)b < s_end; b = next_blk(b)) {
        if (!b->used) {
            run += (run ? HDR : 0) + b->size;   /* contiguous free payload */
            if (run > best) best = run;
        } else {
            run = 0;
        }
    }
    return best;
}
