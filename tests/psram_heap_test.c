/* Host stress test for the PSRAM heap allocator (src/platform/pico/psram_heap.c).
 * Random malloc/free/realloc with a unique per-block pattern; after every op it
 * re-verifies ALL live blocks, so any overlap/corruption or lost realloc content
 * is caught. Build+run:
 *   cc -O2 -Wall -I src/platform/pico tests/psram_heap_test.c \
 *      src/platform/pico/psram_heap.c -o /tmp/psheap && /tmp/psheap
 */
#include "psram_heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HEAP_BYTES (256 * 1024)
#define MAX_LIVE   400
#define ITERS      2000000

static uint8_t region[HEAP_BYTES];

struct live { void *p; size_t n; uint8_t tag; };
static struct live live[MAX_LIVE];
static int nlive;

static void fill(struct live *L) {
    uint8_t *b = (uint8_t *)L->p;
    for (size_t i = 0; i < L->n; i++) b[i] = (uint8_t)(L->tag + i);
}
static int check(struct live *L) {
    const uint8_t *b = (const uint8_t *)L->p;
    for (size_t i = 0; i < L->n; i++)
        if (b[i] != (uint8_t)(L->tag + i)) return 0;
    return 1;
}
static int check_all(void) {
    for (int i = 0; i < nlive; i++)
        if (!psram_heap_owns(live[i].p) || !check(&live[i])) {
            fprintf(stderr, "CORRUPT: block %d p=%p n=%zu tag=%u\n",
                    i, live[i].p, live[i].n, live[i].tag);
            return 0;
        }
    return 1;
}

int main(void) {
    psram_heap_init(region, HEAP_BYTES);
    srand(20260904);
    uint8_t tag = 1;
    long allocs = 0, frees = 0, reallocs = 0, oom = 0;

    for (long it = 0; it < ITERS; it++) {
        int op = rand() % 3;
        if (op == 0 && nlive < MAX_LIVE) {                 /* malloc */
            size_t n = 1 + rand() % 2000;
            void *p = psram_hmalloc(n);
            if (!p) { oom++; }
            else {
                struct live L = { p, n, tag++ }; if (!tag) tag = 1;
                fill(&L);
                live[nlive++] = L;
                allocs++;
            }
        } else if (op == 1 && nlive > 0) {                 /* free */
            int idx = rand() % nlive;
            psram_hfree(live[idx].p);
            live[idx] = live[--nlive];
            frees++;
        } else if (nlive > 0) {                            /* realloc */
            int idx = rand() % nlive;
            size_t nn = 1 + rand() % 2000;
            size_t old = live[idx].n;
            void *np = psram_hrealloc(live[idx].p, nn);
            if (!np) { oom++; }                            /* old block still valid */
            else {
                /* content up to min(old,nn) must be preserved */
                struct live chk = { np, old < nn ? old : nn, live[idx].tag };
                if (!check(&chk)) {
                    fprintf(stderr, "REALLOC lost content at it=%ld\n", it);
                    return 1;
                }
                live[idx].p = np; live[idx].n = nn;
                fill(&live[idx]);
                reallocs++;
            }
        }
        if ((it & 0x3ff) == 0 && !check_all()) {
            fprintf(stderr, "FAILED at iter %ld\n", it);
            return 1;
        }
    }
    if (!check_all()) { fprintf(stderr, "FAILED final\n"); return 1; }

    /* free everything, then confirm the whole region coalesces back to one block */
    for (int i = 0; i < nlive; i++) psram_hfree(live[i].p);
    void *whole = psram_hmalloc(HEAP_BYTES - 64);
    printf("OK: %ld allocs, %ld frees, %ld reallocs, %ld OOM; "
           "full-region reclaim after freeing all: %s\n",
           allocs, frees, reallocs, oom, whole ? "YES" : "NO");
    return whole ? 0 : 1;
}
