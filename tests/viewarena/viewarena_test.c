/* viewarena_test -- randomised stress test of src/platform/pico/view_arena.c.

   Build and run (desktop):
     gcc -O1 -g -fsanitize=address,undefined -Isrc/platform/pico \
       -o tests/viewarena/viewarena tests/viewarena/viewarena_test.c \
       src/platform/pico/view_arena.c && tests/viewarena/viewarena

   Checks, after every operation:
   - every live payload still holds its own fill pattern (no overlap, no
     header written over a payload);
   - payloads are 8-aligned, inside the arena, and usable >= requested;
   - free_bytes + live usable + headers == arena size;
   - view_arena_largest() equals the largest run of free space recomputed from
     the live set (so lazy coalescing loses nothing);
   - an alloc fails only when the request is larger than view_arena_largest();
   - view_arena_check() passes.
   Two workloads: independent random alloc/free, and the real pattern --
   "views" of a few hundred blocks allocated together and freed together in
   LRU order, sizes taken from Jones' views (92-byte pixmaps, cel pointer
   arrays, loops, the palette insert list). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "view_arena.h"

#define ARENA 32768
#define MAXLIVE 4096

static unsigned char arena_mem[ARENA] __attribute__((aligned(8)));

struct blk { unsigned char *p; size_t n; unsigned char fill; int group; };
static struct blk live[MAXLIVE];
static int nlive = 0;
static unsigned long ops = 0, fails = 0;

static void
die(const char *msg)
{
	fprintf(stderr, "FAIL after %lu ops: %s\n", ops, msg);
	exit(1);
}

static int
cmp_ptr(const void *a, const void *b)
{
	const struct blk *x = (const struct blk *)a, *y = (const struct blk *)b;
	return x->p < y->p ? -1 : x->p > y->p;
}

static void
verify(void)
{
	static struct blk s[MAXLIVE];
	size_t used = 0, run, largest = 0;
	unsigned char *cur = arena_mem;
	int i;
	size_t k;

	if (view_arena_check())
		die("view_arena_check");
	memcpy(s, live, sizeof(live[0]) * nlive);
	qsort(s, nlive, sizeof(s[0]), cmp_ptr);
	for (i = 0; i < nlive; i++) {
		unsigned char *hdr = s[i].p - 8;
		size_t u = view_arena_usable(s[i].p);

		if (((uintptr_t)s[i].p & 7) || !view_arena_owns(s[i].p))
			die("misaligned or foreign payload");
		if (u < s[i].n)
			die("usable < requested");
		for (k = 0; k < s[i].n; k++)
			if (s[i].p[k] != (unsigned char)(s[i].fill + k))
				die("payload corrupted");
		if (hdr < cur)
			die("blocks overlap");
		/* free space before this block (whole blocks incl. headers) */
		run = (size_t)(hdr - cur);
		if (run > 8 && run - 8 > largest)
			largest = run - 8;
		used += u + 8;
		cur = hdr + u + 8;
	}
	run = (size_t)(arena_mem + ARENA - cur);
	if (run > 8 && run - 8 > largest)
		largest = run - 8;
	if (view_arena_largest() != largest) {
		fprintf(stderr, "largest %zu, expected %zu\n", view_arena_largest(), largest);
		die("largest mismatch");
	}
	/* free_bytes counts payloads of free blocks; free blocks may still be
	   unmerged, so compare whole-block totals after largest() merged them. */
	{
		size_t freeb = view_arena_free_bytes();
		size_t nfree = 0;
		unsigned char *c2 = arena_mem;
		for (i = 0; i < nlive; i++) {
			if (s[i].p - 8 > c2)
				nfree++;
			c2 = s[i].p + view_arena_usable(s[i].p);
		}
		if (c2 < arena_mem + ARENA)
			nfree++;
		if (freeb + nfree * 8 + used != ARENA)
			die("byte accounting");
	}
}

static void
do_alloc(size_t n, int group)
{
	size_t largest = view_arena_largest();
	unsigned char *p = (unsigned char *)view_arena_alloc(n);
	size_t k;

	ops++;
	if (!p) {
		fails++;
		/* a request that rounds up to <= largest must succeed */
		if (((n + 7) & ~(size_t)7) <= largest)
			die("alloc failed although it fit");
		return;
	}
	if (nlive == MAXLIVE)
		die("test live table full");
	live[nlive].p = p;
	live[nlive].n = n;
	live[nlive].fill = (unsigned char)rand();
	live[nlive].group = group;
	for (k = 0; k < n; k++)
		p[k] = (unsigned char)(live[nlive].fill + k);
	nlive++;
}

static void
do_free(int i)
{
	ops++;
	view_arena_free(live[i].p);
	live[i] = live[--nlive];
}

static size_t
view_like_size(void)
{
	switch (rand() % 8) {
	case 0: case 1: case 2: return 92;              /* gfx_pixmap_t */
	case 3: return 4 * (1 + rand() % 16);            /* cel pointer array */
	case 4: return 12 * (1 + rand() % 13);           /* loops */
	case 5: return 2 + 4 * (rand() % 257);           /* palette insert list */
	case 6: return 60;                               /* gfxr_view_t-ish */
	default: return 1 + rand() % 300;
	}
}

int
main(int argc, char **argv)
{
	unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
	int round, i;

	srand(seed);
	view_arena_init(arena_mem, ARENA);
	if (view_arena_alloc(0) == NULL)
		die("alloc(0) failed on an empty arena");
	nlive = 0;
	view_arena_init(arena_mem, ARENA);
	if (view_arena_largest() != ARENA - 8)
		die("empty arena largest");
	if (view_arena_alloc(ARENA) != NULL)
		die("oversized alloc succeeded");
	if (view_arena_owns(arena_mem + ARENA) || view_arena_owns(arena_mem))
		die("owns() bounds");

	/* 1: independent random alloc/free, sizes 0..2 KB, occasional big ones */
	for (round = 0; round < 200000; round++) {
		if (nlive && (rand() % 100) < 45)
			do_free(rand() % nlive);
		else {
			size_t n = (rand() % 50) ? (size_t)(rand() % 2048) : (size_t)(rand() % ARENA);
			do_alloc(n, 0);
		}
		if ((round & 15) == 0)
			verify();
	}
	while (nlive)
		do_free(nlive - 1);
	verify();
	if (view_arena_largest() != ARENA - 8)
		die("arena did not coalesce back to one block");

	/* 2: views -- groups allocated together, evicted LRU while space is short */
	{
		int next_group = 1, oldest = 1;
		for (round = 0; round < 3000; round++) {
			int blocks = 20 + rand() % 300;
			/* eviction like gfxr_enforce_view_budget + headroom: free whole
			   oldest groups until 16 KB is contiguous */
			while (view_arena_largest() < 16384 && oldest < next_group) {
				for (i = nlive - 1; i >= 0; i--)
					if (live[i].group == oldest)
						do_free(i);
				oldest++;
			}
			for (i = 0; i < blocks; i++)
				do_alloc(view_like_size(), next_group);
			next_group++;
			verify();
		}
	}
	while (nlive)
		do_free(nlive - 1);
	verify();
	if (view_arena_largest() != ARENA - 8)
		die("arena did not coalesce back to one block (views)");

	printf("viewarena: OK, seed %u, %lu ops, %lu allocs refused (no fit)\n", seed, ops, fails);
	return 0;
}
