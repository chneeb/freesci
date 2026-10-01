/* pico_mem_census.c — live-allocation size histogram for the Pico leak hunt.
 *
 * Why this exists: the breakdown probe (kgraphics.c) itemizes only what it can
 * walk (seg-manager heap + gfx pixmap registry). The ~35KB/revisit growth lives
 * OUTSIDE those categories, and g_sci_live_bytes (sci_memory.c) is unreliable as
 * a leak signal because the gfx layer allocates with sci_malloc but frees with
 * raw free() — so that counter drifts. This census sidesteps both problems by
 * hooking malloc/free at the lowest level and keying every alloc/free on the
 * block's *actual* malloc_usable_size. Because alloc and free hit the same
 * bucket regardless of which API (sci_ or raw) was used, the histogram is
 * self-consistent. Diff a bucket across same-room revisits and the growing
 * bucket's size range points straight at the leaking call site.
 *
 * How it hooks: pico-sdk's pico_malloc normally compiles its own
 * WRAPPER_FUNC __wrap_malloc/calloc/realloc/free (malloc.c is added as an
 * INTERFACE source straight into the executable, NOT a static archive — so two
 * definitions collide rather than one winning). The top-level CMakeLists.txt
 * preempts this: it defines the pico_malloc target itself before pico_sdk_init(),
 * tripping the SDK's `if(NOT TARGET pico_malloc)` guard so the SDK skips its
 * malloc.c, and re-adds the -Wl,--wrap=* flags. This file then owns the __wrap_*
 * symbols outright, and the --wrap flags still resolve __real_*. We preserve the
 * PICO_DEBUG_MALLOC "<fn> N failed" log line so OOM diagnostics are unchanged.
 *
 * Caveats (diagnostic build only):
 *  - Drops pico_malloc's malloc_mutex. FreeSCI allocates from core0 only
 *    (audio/core1 is disabled with -q), so this is safe here, not in general.
 *  - A depth guard suppresses double-counting when picolibc's calloc/realloc
 *    call malloc/free internally; the outer wrapper does the single accounting.
 */
#ifdef HAVE_PICO

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void *__real_realloc(void *mem, size_t size);
extern void  __real_free(void *mem);

#ifdef PICO_PSRAM_MAPPED
/* --- Raw free()/realloc() must be ownership-aware on the mapped target -------
   sci_malloc defaults to the PSRAM heap there, but a fair amount of engine and
   gfx code releases sci_malloc'd blocks through RAW free()/realloc() (long
   accepted, because both APIs used to land on the same picolibc heap -- e.g.
   sm_free_script's free(object->variables), game_exit's free(s->game_version)).

   Since the linker's --wrap routes EVERY raw call through these wrappers, this
   is the single chokepoint that keeps that safe: route by ownership before
   newlib ever sees the pointer. Without it a PSRAM pointer reaches _free_r,
   which reads a "chunk header" out of PSRAM payload bytes and then faults
   dereferencing the resulting wild fd/bk -- precisely the observed HardFault
   (PC/LR in _free_r, precise bus fault, garbage BFAR).

   Fixing it here rather than at each call site is deliberate: it covers every
   present and future raw free of engine memory, including paths we have not
   enumerated. psram_heap_owns(NULL) is false, so NULL frees fall through to
   __real_free() unchanged. */
extern void  psram_hfree(void *p);
extern void *psram_hrealloc(void *p, size_t n);
extern int   psram_heap_owns(const void *p);
#	define PSRAM_FREE_IF_OWNED(p) \
		do { if (psram_heap_owns(p)) { psram_hfree(p); return; } } while (0)
#	define PSRAM_REALLOC_IF_OWNED(p, n) \
		do { if ((p) && psram_heap_owns(p)) return psram_hrealloc((p), (n)); } while (0)
#else
#	define PSRAM_FREE_IF_OWNED(p)       ((void)0)
#	define PSRAM_REALLOC_IF_OWNED(p, n) ((void)0)
#endif

#ifdef PICO_VIEW_ARENA
/* --- The VGA view arena (PIO; view_arena.h) ----------------------------------
   While a view is being decoded (g_view_arena_active, set by gfxr_get_view)
   every malloc/calloc is served from the arena first and falls back to the
   heap when the arena has no fitting block. free/realloc route by ownership,
   like the mapped target's PSRAM heap above, because the view's blocks are
   released through every path (gfxr_free_view, raw free, sci_free). Arena
   blocks are never counted in the census histogram; their site registrations
   (sci_memory.c) are dropped on free like any other. */
#include "view_arena.h"

static void *
view_arena_try(size_t n)
{
	return g_view_arena_active ? view_arena_alloc(n) : NULL;
}

/* realloc of an arena block: in place when it fits, else move (to the arena
   while a decode is active, else to the heap). */
static void *
view_arena_realloc(void *p, size_t n)
{
	void *q;
	size_t old = view_arena_usable(p);

	if (n == 0) {
		view_arena_free(p);
		return NULL;
	}
	if (n <= old)
		return p;
	q = view_arena_try(n);
	if (!q)
		q = __real_malloc(n);
	if (!q) {
		printf("realloc %u failed to allocate memory\n", (unsigned) n);
		return NULL; /* p stays valid, as realloc promises */
	}
	memcpy(q, p, old);
	view_arena_free(p);
	return q;
}
#	define VIEW_ARENA_MALLOC(n) \
		do { void *_va = view_arena_try(n); if (_va) return _va; } while (0)
#	define VIEW_ARENA_CALLOC(c, n) \
		do { if ((n) == 0 || (c) <= (size_t)-1 / (n)) { \
			void *_va = view_arena_try((c) * (n)); \
			if (_va) { memset(_va, 0, (c) * (n)); return _va; } } } while (0)
#	define VIEW_ARENA_REALLOC_IF_OWNED(p, n) \
		do { if (view_arena_owns(p)) return view_arena_realloc((p), (n)); } while (0)
#	define VIEW_ARENA_OWNS(p) view_arena_owns(p)
#else
#	define VIEW_ARENA_MALLOC(n)              ((void)0)
#	define VIEW_ARENA_CALLOC(c, n)           ((void)0)
#	define VIEW_ARENA_REALLOC_IF_OWNED(p, n) ((void)0)
#	define VIEW_ARENA_OWNS(p)                0
#endif

#ifndef PICO_PSRAM_MAPPED
/* PIO links with --wrap=malloc_usable_size (top-level CMakeLists.txt), so the
   live-byte and census accounting that calls it on every block keeps working
   for arena blocks. Heap blocks pass straight through. */
extern size_t __real_malloc_usable_size(void *ptr);

size_t
__wrap_malloc_usable_size(void *ptr)
{
	if (VIEW_ARENA_OWNS(ptr))
		return view_arena_usable(ptr);
	return __real_malloc_usable_size(ptr);
}
#endif

/* Largest block the allocator can hand out right now, by bisection with the
   REAL allocator (so no "malloc N failed" noise and no census accounting).
   Includes what the heap can still grow into via sbrk -- it is literally "would
   an N-byte malloc succeed now". Side effect: a successful probe may sbrk the
   heap up (arena grows; the memory stays free in the top chunk). Used by the
   FSCI_PROBE_MEM room lines to measure the real per-room margin. */
size_t
pico_largest_alloc(void)
{
	size_t lo = 0, hi = 512 * 1024;

	while (hi - lo > 16) {
		size_t mid = lo + (hi - lo) / 2;
		void *p = __real_malloc(mid);
		if (p) {
			__real_free(p);
			lo = mid;
		} else
			hi = mid;
	}
	return lo;
}

#ifdef FSCI_PROBE_MEM_CENSUS

/* Bucket b (b>=1) covers [1<<(b+2), 1<<(b+3)); bucket 0 is < 8 bytes.
   18 buckets reach 1<<20 = 1 MB, more than enough for SCI0's 64KB peaks. */
#define CENSUS_NBUCKETS 18

size_t pico_census_bytes[CENSUS_NBUCKETS];   /* live bytes per bucket  */
int    pico_census_count[CENSUS_NBUCKETS];   /* live blocks per bucket */
size_t pico_census_total_bytes = 0;          /* live bytes, all buckets */
int    pico_census_total_count = 0;          /* live blocks, all buckets */

/* Suppresses inner accounting when calloc/realloc call malloc/free internally. */
static int census_depth = 0;

/* ── Call-site tagging — window [32,128) (the per-restore clone-variables leaker).
 * Retarget SITE_LO/HI to another size class to name a different bucket's blocks;
 * the one-flash [16384,32768) run that named the ~71KB baseline lump (resource
 * directory / packed vocab / VM stack — docs/history/pico-memory-oom.md "DIAGNOSIS — the post-restore
 * OOM is FRAGMENTATION + arena ratchet") has been reverted back to [32,128).
 *
 * The histogram (CENSUS line) localizes the lump to a size class; this names the
 * source line. sci_memory.c calls census_site_register() after every successful
 * sci_malloc/sci_calloc, passing the block's __FILE__/__LINE__ (it already
 * carries them). We record {ptr -> site} only for blocks whose usable size lands
 * in [SITE_LO,SITE_HI). Every free routes through __wrap_free (raw free() too,
 * which is how the gfx layer releases sci_malloc'd blocks), so deregistration by
 * ptr is symmetric regardless of which API freed it.
 *
 * CAVEAT: only sci_malloc/sci_calloc blocks are tagged (those carry __FILE__/
 * __LINE__). A block allocated via RAW malloc (e.g. visual[0], the transient
 * decode buffers) will appear in the CENSUS bucket count but NOT in this SITES
 * line — its absence is itself a signal that the block is a raw allocation.
 *
 * Site identity is the (file-pointer, line) pair: __FILE__ is a string literal
 * with a stable address, so comparing the pointer is enough and avoids strcmp. */
/* SITES window.  Retarget this at whatever size class a hunt is chasing -- it
   was [32,128) for the clone-variables leak.  It is now wide-open at the bottom
   end of "big enough to matter": the cross-game residual at the chooser is only
   a few KB total, so a narrow window simply misses it.  Do NOT drop SITE_LO to
   0: the 8-byte blocks alone run ~1900 live in a room and would blow
   CENSUS_NPTRS. */
/* EVERY live block is tagged since 2026-09-28 (for census_heap_walk): the
   fragmentation question is which SMALL blocks sit between free gaps, so the
   small ones are exactly the ones that must be named. To afford it the table is
   compact -- 4096 pointers + a 1-byte site index (20 KB) instead of 2048 x
   {ptr, site, usable} (24 KB); a block's size is read back with
   malloc_usable_size() when it is freed.
   Two kinds of site:
     - sci_malloc/sci_calloc blocks: (__FILE__, __LINE__), from sci_memory.c;
     - everything else reaching the wrappers (raw malloc/calloc/realloc): the
       caller's return address (file == NULL, line == PC). Resolve offline with
         arm-none-eabi-addr2line -f -e build-pico-census/src/freesci.elf 0xPC
   A sci_malloc block is first tagged by PC inside __wrap_malloc and then
   re-tagged with its file:line, so it is never counted twice. */
#define CENSUS_NSITES   255    /* site index 255 = untracked */
#define CENSUS_NPTRS    4096   /* power of two; > peak live blocks */
#define CENSUS_NO_SITE  255

struct census_site { const char *file; int line; int live_count; size_t live_bytes; };

static struct census_site census_sites[CENSUS_NSITES];
static int                 census_nsites = 0;
static void               *census_ptrs[CENSUS_NPTRS];
static uint8_t             census_ptr_site[CENSUS_NPTRS];
static unsigned            census_untracked = 0;  /* table or site overflow */

static unsigned
census_ptr_hash(void *p)
{
	/* Fibonacci hash of the pointer; low bits of malloc'd ptrs are aligned. */
	return (unsigned) (((uintptr_t) p >> 3) * 2654435761u) & (CENSUS_NPTRS - 1);
}

static int
census_find_site(const char *file, int line)
{
	int i;
	for (i = 0; i < census_nsites; i++)
		if (census_sites[i].file == file && census_sites[i].line == line)
			return i;
	if (census_nsites < CENSUS_NSITES) {
		census_sites[census_nsites].file = file;
		census_sites[census_nsites].line = line;
		census_sites[census_nsites].live_count = 0;
		census_sites[census_nsites].live_bytes = 0;
		return census_nsites++;
	}
	return -1;   /* table full — block goes untracked (won't corrupt counts) */
}

/* Slot holding ptr, or -1. */
static int
census_slot_of(void *ptr)
{
	unsigned h = census_ptr_hash(ptr), i;

	for (i = 0; i < CENSUS_NPTRS; i++) {
		unsigned slot = (h + i) & (CENSUS_NPTRS - 1);
		if (census_ptrs[slot] == ptr)
			return (int) slot;
		if (census_ptrs[slot] == NULL)
			return -1;
	}
	return -1;
}

/* Tag ptr with (file, line), replacing any earlier tag of the same block. */
static void
census_tag(void *ptr, const char *file, int line)
{
	size_t usable;
	unsigned h, i;
	int site, slot;

	if (!ptr)
		return;
	usable = malloc_usable_size(ptr);
	site = census_find_site(file, line);
	slot = census_slot_of(ptr);
	if (slot >= 0) {
		/* Re-tag: sci_malloc naming a block its __wrap_malloc tagged by PC. */
		int old = census_ptr_site[slot];
		if (old != CENSUS_NO_SITE) {
			census_sites[old].live_count--;
			census_sites[old].live_bytes -= usable;
		}
		census_ptr_site[slot] = site < 0 ? CENSUS_NO_SITE : (uint8_t) site;
		if (site >= 0) {
			census_sites[site].live_count++;
			census_sites[site].live_bytes += usable;
		}
		return;
	}

	h = census_ptr_hash(ptr);
	for (i = 0; i < CENSUS_NPTRS; i++) {
		unsigned s = (h + i) & (CENSUS_NPTRS - 1);
		if (census_ptrs[s] == NULL) {
			census_ptrs[s] = ptr;
			census_ptr_site[s] = site < 0 ? CENSUS_NO_SITE : (uint8_t) site;
			if (site >= 0) {
				census_sites[site].live_count++;
				census_sites[site].live_bytes += usable;
			} else
				census_untracked++;
			return;
		}
	}
	census_untracked++;   /* ptr table full — deregister will simply miss it */
}

void
census_site_register(void *ptr, const char *file, int line)
{
	census_tag(ptr, file, line);
}

/* Tag a raw allocation by the wrapper's caller. Must be expanded directly in a
   __wrap_* function so __builtin_return_address(0) is that function's caller. */
#define CENSUS_TAG_PC(ptr) \
	census_tag((ptr), NULL, (int) (uintptr_t) __builtin_return_address(0))

/* Called from __wrap_free/__wrap_realloc BEFORE the block is released
   (malloc_usable_size must still be valid). No-op for unregistered ptrs. */
static void
census_site_deregister(void *ptr)
{
	int slot;

	if (!ptr || (slot = census_slot_of(ptr)) < 0)
		return;
	if (census_ptr_site[slot] != CENSUS_NO_SITE) {
		int site = census_ptr_site[slot];
		census_sites[site].live_count--;
		census_sites[site].live_bytes -= malloc_usable_size(ptr);
	}
	census_ptrs[slot] = NULL;
	/* Re-probe the rest of the cluster so a deleted slot doesn't strand later
	   entries that collided past it. */
	{
		unsigned j = ((unsigned) slot + 1) & (CENSUS_NPTRS - 1);
		while (census_ptrs[j]) {
			void   *rp = census_ptrs[j];
			uint8_t rs = census_ptr_site[j];
			unsigned nh = census_ptr_hash(rp), k;
			census_ptrs[j] = NULL;
			for (k = 0; k < CENSUS_NPTRS; k++) {
				unsigned s2 = (nh + k) & (CENSUS_NPTRS - 1);
				if (census_ptrs[s2] == NULL) {
					census_ptrs[s2] = rp;
					census_ptr_site[s2] = rs;
					break;
				}
			}
			j = (j + 1) & (CENSUS_NPTRS - 1);
		}
	}
}

static void
census_site_name(int site, char *buf, size_t n)
{
	if (site < 0 || site == CENSUS_NO_SITE)
		snprintf(buf, n, "untracked");
	else if (census_sites[site].file == NULL)
		snprintf(buf, n, "pc:0x%08x", (unsigned) census_sites[site].line);
	else {
		const char *f = census_sites[site].file, *q = f;
		while (*q) { if (*q == '/') f = q + 1; q++; }
		snprintf(buf, n, "%s:%d", f, census_sites[site].line);
	}
}

static int
census_site_of_mem(void *mem)
{
	int slot = census_slot_of(mem);
	return slot < 0 ? -1 : census_ptr_site[slot];
}

/* Prints the live tagged sites in [SITE_LO,SITE_HI), most blocks first, for
   offline resolution against the source.  Called from pico_mem_breakdown and
   from the chooser reset (pico_main.c), where it names the cross-game residual.
   Prints the bucket histogram first: that covers EVERY live block including raw
   mallocs and anything outside the SITES window, so if the sites line comes up
   short the histogram still gives the size classes to retarget on. */
static void
census_print(int by_bytes, int max_sites)
{
	int i, printed = 0;

	printf("[mem] LIVE %lu B in %d blocks:",
	       (unsigned long) pico_census_total_bytes, pico_census_total_count);
	for (i = 0; i < CENSUS_NBUCKETS; i++)
		if (pico_census_count[i])
			printf(" %lu:%d/%lu", (unsigned long) ((size_t) 1 << (i + 3)),
			       pico_census_count[i], (unsigned long) pico_census_bytes[i]);
	printf("\n");

	printf("[mem] SITES:");
	for (;;) {
		int best = -1, b;
		for (b = 0; b < census_nsites; b++) {
			if (census_sites[b].live_count <= 0)
				continue;
			if (best < 0 || (by_bytes
				? census_sites[b].live_bytes > census_sites[best].live_bytes
				: census_sites[b].live_count > census_sites[best].live_count))
				best = b;
		}
		if (best < 0 || printed >= max_sites)
			break;
		{
			char nm[48];
			census_site_name(best, nm, sizeof(nm));
			printf(" %s=%d/%lu", nm, census_sites[best].live_count,
			       (unsigned long) census_sites[best].live_bytes);
		}
		/* Mark printed by flipping sign of count temporarily. */
		census_sites[best].live_count = -census_sites[best].live_count;
		printed++;
	}
	/* Restore the counts we negated as a print-order marker. */
	for (i = 0; i < census_nsites; i++)
		if (census_sites[i].live_count < 0)
			census_sites[i].live_count = -census_sites[i].live_count;
	printf("\n");
}

void
census_dump_sites(void)
{
	census_print(0, 24);
}

/* ── Heap walk (2026-09-28): WHICH blocks split the free space ───────────────
 * The KQ4+sound census showed the OOM is fragmentation (29 KB free, largest
 * piece 5.3 KB), but counts by size/site cannot say which blocks sit BETWEEN the
 * gaps. This walks newlib's heap chunk by chunk and names them.
 *
 * Layout relied on (full newlib mallocr, not nano -- __malloc_av_ is present):
 * a chunk is {prev_size, size}, size bit 0 = PREV_INUSE, bit 1 = IS_MMAPPED;
 * chunks are contiguous from the 8-aligned start of the sbrk region (pico-sdk's
 * _sbrk starts at __end__) up to the "top" chunk, av_[2]. A chunk is in use iff
 * the NEXT chunk's PREV_INUSE bit is set. The walk is SELF-CHECKING: free gaps
 * + top must equal mallinfo().fordblks, printed as ok/MISMATCH -- a wrong
 * layout assumption shows up as MISMATCH, never as plausible numbers.
 *
 * A "pin" is an allocated run of at most WALK_PIN_MAX bytes with a free gap on
 * both sides: moving it elsewhere (a fixed pool) would merge the two gaps.
 * "largest if pins moved" is the biggest gap that merging would produce -- what
 * step 1 of docs/pico-memory-model-plan.md (fixed pools) could buy. */
extern char __end__;
extern char __HeapLimit;
extern void *__malloc_av_[];

static int census_bucket(size_t n);

#define WALK_PIN_MAX  512
#define WALK_TOPGAPS  12
#define WALK_RUNMAX   (WALK_PIN_MAX / 16)   /* chunks in a pin run, max */

struct walk_gap { uintptr_t addr; size_t size; int below; size_t below_sz; int above; size_t above_sz; };
static struct walk_gap walk_top[WALK_TOPGAPS];
static unsigned walk_pin_n[256];
static size_t   walk_pin_b[256];

static int
walk_site_ix(int site)
{
	return site < 0 ? CENSUS_NO_SITE : site;
}

static void
walk_note_gap(uintptr_t addr, size_t size, int below, size_t below_sz)
{
	int i, j;

	for (i = 0; i < WALK_TOPGAPS; i++)
		if (walk_top[i].size < size)
			break;
	if (i == WALK_TOPGAPS)
		return;
	for (j = WALK_TOPGAPS - 1; j > i; j--)
		walk_top[j] = walk_top[j - 1];
	walk_top[i].addr = addr;
	walk_top[i].size = size;
	walk_top[i].below = below;
	walk_top[i].below_sz = below_sz;
	walk_top[i].above = -2;          /* filled in when the next chunk is seen */
	walk_top[i].above_sz = 0;
}

void
census_heap_walk(const char *tag)
{
	struct mallinfo mi = mallinfo();
	uintptr_t p = ((uintptr_t) &__end__ + 7) & ~(uintptr_t) 7;
	uintptr_t top = (uintptr_t) __malloc_av_[2];
	size_t used_b = 0, gap_b = 0, largest = 0, top_sz = 0;
	unsigned used_n = 0, gap_n = 0, pins = 0, steps = 0;
	int bad = 0, i;
	/* run = allocated chunks since the last gap */
	size_t run_b = 0;
	unsigned run_n = 0;
	int run_site[WALK_RUNMAX];
	size_t run_sz[WALK_RUNMAX];
	int have_gap = 0;               /* a gap precedes the current run */
	size_t merged = 0, merged_max = 0;
	int last_site = -2;             /* last allocated chunk, for "below" */
	size_t last_sz = 0;
	int pending = -1;               /* walk_top slot waiting for "above" */
	unsigned gh_n[CENSUS_NBUCKETS];
	size_t gh_b[CENSUS_NBUCKETS];
	char a[48], b[48];

	memset(walk_top, 0, sizeof(walk_top));
	memset(walk_pin_n, 0, sizeof(walk_pin_n));
	memset(walk_pin_b, 0, sizeof(walk_pin_b));
	memset(gh_n, 0, sizeof(gh_n));
	memset(gh_b, 0, sizeof(gh_b));

	if (!top || top < p) {
		printf("[heap] %s walk: no heap yet\n", tag);
		return;
	}

	while (p < top) {
		size_t sz = ((size_t *) p)[1] & ~(size_t) 3;
		uintptr_t nx = p + sz;
		int in_use;

		if (sz < 16 || nx > top || ++steps > 20000) {
			bad = 1;
			break;
		}
		in_use = ((size_t *) nx)[1] & 1;

		if (in_use) {
			int site = census_site_of_mem((void *) (p + 8));
			used_n++;
			used_b += sz;
			if (pending >= 0) {
				walk_top[pending].above = site;
				walk_top[pending].above_sz = sz;
				pending = -1;
			}
			if (run_n < WALK_RUNMAX) {
				run_site[run_n] = site;
				run_sz[run_n] = sz;
			}
			run_n++;
			run_b += sz;
			last_site = site;
			last_sz = sz;
		} else {
			int k;
			gap_n++;
			gap_b += sz;
			if (sz > largest)
				largest = sz;
			k = census_bucket(sz);
			gh_n[k]++;
			gh_b[k] += sz;

			if (have_gap && run_b <= WALK_PIN_MAX && run_n <= WALK_RUNMAX) {
				/* The run is a pin between the previous gap and this one. */
				unsigned r;
				pins++;
				for (r = 0; r < run_n; r++) {
					int s = walk_site_ix(run_site[r]);
					walk_pin_n[s]++;
					walk_pin_b[s] += run_sz[r];
				}
				merged += run_b + sz;
			} else
				merged = sz;
			if (merged > merged_max)
				merged_max = merged;

			walk_note_gap(p, sz, run_n ? last_site : -2, run_n ? last_sz : 0);
			pending = -1;
			for (i = 0; i < WALK_TOPGAPS; i++)
				if (walk_top[i].addr == p) {
					pending = i;
					break;
				}
			have_gap = 1;
			run_b = 0;
			run_n = 0;
		}
		p = nx;
	}
	if (!bad && p != top)
		bad = 1;
	if (!bad) {
		top_sz = ((size_t *) top)[1] & ~(size_t) 3;
		/* The top chunk is free too: a small final run merges into it. */
		if (have_gap && run_b <= WALK_PIN_MAX && run_n <= WALK_RUNMAX
		    && merged + run_b + top_sz > merged_max)
			merged_max = merged + run_b + top_sz;
	}

	printf("[heap] %s walk %s: used %u blocks %lu B, %u gaps %lu B, top %lu"
	       " (+%lu unsbrk'd), largest gap %lu, largest if pins moved %lu,"
	       " %u pins | mallinfo free %lu, untracked %u\n",
	       tag,
	       bad ? "BROKEN" : (gap_b + top_sz == mi.fordblks ? "ok" : "MISMATCH"),
	       used_n, (unsigned long) used_b, gap_n, (unsigned long) gap_b,
	       (unsigned long) top_sz,
	       (unsigned long) ((uintptr_t) &__HeapLimit - (top + top_sz)),
	       (unsigned long) largest, (unsigned long) merged_max, pins,
	       (unsigned long) mi.fordblks, census_untracked);
	if (bad)
		printf("[heap]   stopped at 0x%08lx (top 0x%08lx) after %u chunks\n",
		       (unsigned long) p, (unsigned long) top, steps);

	printf("[heap]   gaps by size:");
	for (i = 0; i < CENSUS_NBUCKETS; i++)
		if (gh_n[i])
			printf(" <%lu:%u/%lu", (unsigned long) ((size_t) 1 << (i + 3)),
			       gh_n[i], (unsigned long) gh_b[i]);
	printf("\n");

	for (i = 0; i < WALK_TOPGAPS && walk_top[i].size; i++) {
		census_site_name(walk_top[i].below == -2 ? -1 : walk_top[i].below, a, sizeof(a));
		census_site_name(walk_top[i].above == -2 ? -1 : walk_top[i].above, b, sizeof(b));
		printf("[heap]   gap %lu @%08lx  below %s/%lu  above %s/%lu\n",
		       (unsigned long) walk_top[i].size, (unsigned long) walk_top[i].addr,
		       walk_top[i].below == -2 ? "-" : a, (unsigned long) walk_top[i].below_sz,
		       walk_top[i].above == -2 ? "-" : b, (unsigned long) walk_top[i].above_sz);
	}

	printf("[heap]   pins by site (chunks/bytes):");
	for (i = 0; i < 30; i++) {
		int best = -1, s;
		for (s = 0; s < 256; s++)
			if (walk_pin_n[s] && (best < 0 || walk_pin_n[s] > walk_pin_n[best]))
				best = s;
		if (best < 0)
			break;
		census_site_name(best, a, sizeof(a));
		printf(" %s=%u/%lu", a, walk_pin_n[best], (unsigned long) walk_pin_b[best]);
		walk_pin_n[best] = 0;
	}
	printf("\n");
}

/* Named snapshot for bracketing a code path (e.g. the clean-heap restore): the
   live sites by BYTES plus a heap walk. A site live at one checkpoint and gone at
   the next is a transient; the walk shows the holes it leaves behind. */
void
census_checkpoint(const char *tag)
{
	struct mallinfo mi = mallinfo();

	printf("[mem] CHECKPOINT %s: free=%lu arena=%lu\n", tag,
	       (unsigned long) mi.fordblks, (unsigned long) mi.arena);
	census_print(1, 40);
	census_heap_walk(tag);
}

/* Called from pico_oom_report just before the halt: the heap composition AT the
   failing allocation, which the per-room breakdowns never see.  Sites are ranked
   by BYTES here (not count) so single large blocks are not crowded out, and the
   largest allocatable block is found by bisection with the real allocator --
   "largest free vs total free" is the fragmentation cost the plan's step 0
   needs (docs/pico-memory-model-plan.md). */
void
census_dump_oom(void)
{
	struct mallinfo mi = mallinfo();
	size_t lo = 0, hi = mi.fordblks + 1;

	while (hi - lo > 16) {
		size_t mid = lo + (hi - lo) / 2;
		void *p = __real_malloc(mid);
		if (p) {
			__real_free(p);
			lo = mid;
		} else
			hi = mid;
	}
	printf("[mem] OOM free=%lu largest~%lu arena=%lu\n",
	       (unsigned long) mi.fordblks, (unsigned long) lo,
	       (unsigned long) mi.arena);
	census_print(1, 40);
	census_heap_walk("OOM");
}

static int
census_bucket(size_t n)
{
	int b = 0;
	while (b < CENSUS_NBUCKETS - 1 && n >= ((size_t) 1 << (b + 3)))
		b++;
	return b;
}

static void
census_add_size(size_t n)
{
	int b = census_bucket(n);
	pico_census_bytes[b] += n;
	pico_census_count[b]++;
	pico_census_total_bytes += n;
	pico_census_total_count++;
}

static void
census_sub_size(size_t n)
{
	int b = census_bucket(n);
	pico_census_bytes[b] -= n;
	pico_census_count[b]--;
	pico_census_total_bytes -= n;
	pico_census_total_count--;
}

/* Dump the census at the FIRST failed allocation, raw ones included. Many
   callers handle a NULL themselves (script and song loads, the GC's hash map),
   so a heap that runs out often never reaches pico_oom_report and its dump:
   Jones in the Fast Lane ran out loading a script and dropped into the SCI
   console instead (2026-10-01). Once per session; census builds only. */
static void
census_first_failure(void)
{
	static int dumped = 0;

	if (dumped || census_depth)
		return;
	dumped = 1;
	census_depth++;
#ifdef PICO_VIEW_ARENA
	if (view_arena_base())
		printf("[view] arena: %u bytes free, largest %u\n",
		       (unsigned) view_arena_free_bytes(), (unsigned) view_arena_largest());
#endif
	census_dump_oom();
	census_depth--;
}

void *
__wrap_malloc(size_t size)
{
	void *rc;
	VIEW_ARENA_MALLOC(size);
	rc = __real_malloc(size);
	if (!rc) {
		printf("malloc %u failed to allocate memory\n", (unsigned) size);
		census_first_failure();
		return rc;
	}
	if (census_depth == 0) {
		census_add_size(malloc_usable_size(rc));
		CENSUS_TAG_PC(rc);
	}
	return rc;
}

void *
__wrap_calloc(size_t count, size_t size)
{
	void *rc;
	VIEW_ARENA_CALLOC(count, size);
	census_depth++;
	rc = __real_calloc(count, size);  /* may call __wrap_malloc (suppressed) */
	census_depth--;
	if (!rc) {
		printf("calloc %u failed to allocate memory\n",
		       (unsigned) (count * size));
		census_first_failure();
		return rc;
	}
	census_add_size(malloc_usable_size(rc));
	CENSUS_TAG_PC(rc);
	return rc;
}

void *
__wrap_realloc(void *mem, size_t size)
{
	size_t oldb;
	PSRAM_REALLOC_IF_OWNED(mem, size);
	if (mem && VIEW_ARENA_OWNS(mem)) {
		void *moved;
		census_site_deregister(mem); /* the sci layer re-registers the result */
		moved = view_arena_realloc(mem, size);
		if (moved && !VIEW_ARENA_OWNS(moved)) /* moved out to the heap */
			census_add_size(malloc_usable_size(moved));
		else if (!moved && size)
			census_first_failure();
		return moved;
	}
	oldb = mem ? malloc_usable_size(mem) : 0;
	void *rc;
	if (mem)
		census_site_deregister(mem);  /* old block gone; sci layer re-registers new */
	census_depth++;
	rc = __real_realloc(mem, size);   /* may call __wrap_free/malloc (suppressed) */
	census_depth--;
	if (!rc) {
		printf("realloc %u failed to allocate memory\n", (unsigned) size);
		census_first_failure();
		return rc;  /* original block still live; accounting unchanged */
	}
	if (mem)
		census_sub_size(oldb);    /* old block gone (moved or grown in place) */
	census_add_size(malloc_usable_size(rc));
	CENSUS_TAG_PC(rc);
	return rc;
}

void
__wrap_free(void *mem)
{
	PSRAM_FREE_IF_OWNED(mem);
	if (mem && VIEW_ARENA_OWNS(mem)) {
		census_site_deregister(mem);
		view_arena_free(mem);
		return;
	}
	if (mem && census_depth == 0)
		census_sub_size(malloc_usable_size(mem));
	if (mem)
		census_site_deregister(mem);
	__real_free(mem);
}

#else /* !FSCI_PROBE_MEM_CENSUS — census off: thin pass-throughs + no-op stubs */

/* The top-level CMake adds -Wl,--wrap=* whenever this file is compiled (it owns
   pico_malloc), so __wrap_* must stay defined even with the census disabled.
   Keep the PICO_DEBUG_MALLOC "<fn> N failed" log so OOM diagnostics are intact;
   drop only the histogram/site bookkeeping (and its ~27.6KB of .bss arrays). */

/* Called unconditionally from sci_memory.c under HAVE_PICO; no-op when off. */
void census_site_register(void *ptr, const char *file, int line)
{ (void)ptr; (void)file; (void)line; }

void census_dump_sites(void) {}
void census_dump_oom(void) {}
void census_heap_walk(const char *tag) { (void)tag; }
void census_checkpoint(const char *tag) { (void)tag; }

void *
__wrap_malloc(size_t size)
{
	void *rc;
	VIEW_ARENA_MALLOC(size);
	rc = __real_malloc(size);
	if (!rc)
		printf("malloc %u failed to allocate memory\n", (unsigned) size);
	return rc;
}

void *
__wrap_calloc(size_t count, size_t size)
{
	void *rc;
	VIEW_ARENA_CALLOC(count, size);
	rc = __real_calloc(count, size);
	if (!rc)
		printf("calloc %u failed to allocate memory\n",
		       (unsigned) (count * size));
	return rc;
}

void *
__wrap_realloc(void *mem, size_t size)
{
	void *rc;
	PSRAM_REALLOC_IF_OWNED(mem, size);
	VIEW_ARENA_REALLOC_IF_OWNED(mem, size);
	rc = __real_realloc(mem, size);
	if (!rc)
		printf("realloc %u failed to allocate memory\n", (unsigned) size);
	return rc;
}

void
__wrap_free(void *mem)
{
	PSRAM_FREE_IF_OWNED(mem);
	if (VIEW_ARENA_OWNS(mem)) {
		view_arena_free(mem);
		return;
	}
	__real_free(mem);
}

#endif /* FSCI_PROBE_MEM_CENSUS */

#endif /* HAVE_PICO */
