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
#include <malloc.h>

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void *__real_realloc(void *mem, size_t size);
extern void  __real_free(void *mem);

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

/* ── Call-site tagging for the 32-127 byte buckets (the per-restore leaker) ──
 * The histogram above localizes the leak to a size class; this names the
 * source line. sci_memory.c calls census_site_register() after every
 * successful sci_malloc/sci_calloc, passing the block's __FILE__/__LINE__ (it
 * already carries them). We record {ptr -> site} only for blocks whose usable
 * size lands in [32,128). Every free routes through __wrap_free (raw free()
 * too, which is how the gfx layer releases sci_malloc'd blocks), so
 * deregistration by ptr there is symmetric regardless of which API freed it.
 * Diff a site's live_count across same-room restores → the growing site is the
 * leak. The measured restore leak is ~15 blocks/restore in [32,64) plus ~3 in
 * [64,128), so this window brackets it. Many more distinct call sites emit
 * small blocks than 256B blocks, hence the larger site table.
 *
 * Site identity is the (file-pointer, line) pair: __FILE__ is a string literal
 * with a stable address, so comparing the pointer is enough and avoids strcmp. */
#define SITE_LO 32
#define SITE_HI 128
#define CENSUS_NSITES   192
#define CENSUS_NPTRS    2048   /* power of two; > peak live 32-127 byte blocks */

struct census_site { const char *file; int line; int live_count; size_t live_bytes; };
struct census_ptr  { void *ptr; int site; unsigned usable; };

static struct census_site census_sites[CENSUS_NSITES];
static int                 census_nsites = 0;
static struct census_ptr   census_ptrs[CENSUS_NPTRS];

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

void
census_site_register(void *ptr, const char *file, int line)
{
	size_t usable;
	unsigned h, i;
	int site;

	if (!ptr)
		return;
	usable = malloc_usable_size(ptr);
	if (usable < SITE_LO || usable >= SITE_HI)
		return;
	site = census_find_site(file, line);
	if (site < 0)
		return;

	h = census_ptr_hash(ptr);
	for (i = 0; i < CENSUS_NPTRS; i++) {
		unsigned slot = (h + i) & (CENSUS_NPTRS - 1);
		if (census_ptrs[slot].ptr == NULL) {
			census_ptrs[slot].ptr = ptr;
			census_ptrs[slot].site = site;
			census_ptrs[slot].usable = (unsigned) usable;
			census_sites[site].live_count++;
			census_sites[site].live_bytes += usable;
			return;
		}
	}
	/* ptr table full — drop silently; deregister will simply miss it. */
}

/* Called from __wrap_free/__wrap_realloc by ptr. No-op for unregistered ptrs
   (raw allocs, out-of-range sizes), so it is safe to call on every free. */
static void
census_site_deregister(void *ptr)
{
	unsigned h, i;

	if (!ptr)
		return;
	h = census_ptr_hash(ptr);
	for (i = 0; i < CENSUS_NPTRS; i++) {
		unsigned slot = (h + i) & (CENSUS_NPTRS - 1);
		if (census_ptrs[slot].ptr == ptr) {
			int site = census_ptrs[slot].site;
			census_sites[site].live_count--;
			census_sites[site].live_bytes -= census_ptrs[slot].usable;
			census_ptrs[slot].ptr = NULL;
			/* Re-probe the rest of the cluster so a deleted slot doesn't
			   strand later entries that collided past it. */
			{
				unsigned j = (slot + 1) & (CENSUS_NPTRS - 1);
				while (census_ptrs[j].ptr) {
					void *rp = census_ptrs[j].ptr;
					int   rs = census_ptrs[j].site;
					unsigned ru = census_ptrs[j].usable;
					unsigned nh = census_ptr_hash(rp), k;
					census_ptrs[j].ptr = NULL;
					for (k = 0; k < CENSUS_NPTRS; k++) {
						unsigned s2 = (nh + k) & (CENSUS_NPTRS - 1);
						if (census_ptrs[s2].ptr == NULL) {
							census_ptrs[s2].ptr = rp;
							census_ptrs[s2].site = rs;
							census_ptrs[s2].usable = ru;
							break;
						}
					}
					j = (j + 1) & (CENSUS_NPTRS - 1);
				}
			}
			return;
		}
	}
}

/* Prints the live [256,512)-byte sites, most blocks first, for offline
   resolution against the source. Called from pico_mem_breakdown. */
void
census_dump_sites(void)
{
	int i, printed = 0;
	printf("[mem] SITES256:");
	for (;;) {
		int best = -1, b;
		for (b = 0; b < census_nsites; b++) {
			if (census_sites[b].live_count <= 0)
				continue;
			if (best < 0 || census_sites[b].live_count > census_sites[best].live_count)
				best = b;
		}
		if (best < 0 || printed >= 8)
			break;
		printf(" %s:%d=%d/%lu", census_sites[best].file, census_sites[best].line,
		       census_sites[best].live_count,
		       (unsigned long) census_sites[best].live_bytes);
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

void *
__wrap_malloc(size_t size)
{
	void *rc = __real_malloc(size);
	if (!rc) {
		printf("malloc %u failed to allocate memory\n", (unsigned) size);
		return rc;
	}
	if (census_depth == 0)
		census_add_size(malloc_usable_size(rc));
	return rc;
}

void *
__wrap_calloc(size_t count, size_t size)
{
	void *rc;
	census_depth++;
	rc = __real_calloc(count, size);  /* may call __wrap_malloc (suppressed) */
	census_depth--;
	if (!rc) {
		printf("calloc %u failed to allocate memory\n",
		       (unsigned) (count * size));
		return rc;
	}
	census_add_size(malloc_usable_size(rc));
	return rc;
}

void *
__wrap_realloc(void *mem, size_t size)
{
	size_t oldb = mem ? malloc_usable_size(mem) : 0;
	void *rc;
	if (mem)
		census_site_deregister(mem);  /* old block gone; sci layer re-registers new */
	census_depth++;
	rc = __real_realloc(mem, size);   /* may call __wrap_free/malloc (suppressed) */
	census_depth--;
	if (!rc) {
		printf("realloc %u failed to allocate memory\n", (unsigned) size);
		return rc;  /* original block still live; accounting unchanged */
	}
	if (mem)
		census_sub_size(oldb);    /* old block gone (moved or grown in place) */
	census_add_size(malloc_usable_size(rc));
	return rc;
}

void
__wrap_free(void *mem)
{
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

void *
__wrap_malloc(size_t size)
{
	void *rc = __real_malloc(size);
	if (!rc)
		printf("malloc %u failed to allocate memory\n", (unsigned) size);
	return rc;
}

void *
__wrap_calloc(size_t count, size_t size)
{
	void *rc = __real_calloc(count, size);
	if (!rc)
		printf("calloc %u failed to allocate memory\n",
		       (unsigned) (count * size));
	return rc;
}

void *
__wrap_realloc(void *mem, size_t size)
{
	void *rc = __real_realloc(mem, size);
	if (!rc)
		printf("realloc %u failed to allocate memory\n", (unsigned) size);
	return rc;
}

void
__wrap_free(void *mem)
{
	__real_free(mem);
}

#endif /* FSCI_PROBE_MEM_CENSUS */

#endif /* HAVE_PICO */
