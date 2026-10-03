/* view_arena.c -- first-fit allocator over one fixed block (see view_arena.h).

   Each block starts with an 8-byte header: word 0 is the whole block's size
   (a multiple of 8, header included) with bit 0 set while it is in use; word 1
   is a magic used by view_arena_check and to reject foreign pointers. Payloads
   are therefore 8-aligned like the heap's. Adjacent free blocks are merged
   lazily, whenever a walk passes over them (alloc, largest), so free() is O(1).
   The arena is small (32 KB, a few hundred blocks), so a linear walk is cheap
   next to the view decode that drives it. */
#include <stdint.h>
#include <string.h>
#include "view_arena.h"

#define VA_HDR 8
#define VA_MIN 16 /* smallest block worth splitting off: header + 8 */
#define VA_MAGIC 0x56414ea5u
#define VA_USED 1u

/* Under AddressSanitizer (the desktop stress build) free payloads are
   poisoned, so a use-after-free inside the arena is still reported. */
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define VA_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define VA_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define VA_POISON(p, n) ((void)0)
#define VA_UNPOISON(p, n) ((void)0)
#endif

int g_view_arena_active = 0;

static unsigned char *va_base = NULL;
static unsigned char *va_end = NULL;

#define VA_WORD(b, i) (((uint32_t *)(void *)(b))[i])
#define VA_SIZE(b) (VA_WORD(b, 0) & ~(uint32_t)7)
#define VA_ISUSED(b) (VA_WORD(b, 0) & VA_USED)

void
view_arena_init(void *mem, size_t size)
{
	size &= ~(size_t)7;
	if (!mem || size < VA_MIN) { /* detach (the caller frees the block) */
		va_base = va_end = NULL;
		return;
	}
	va_base = (unsigned char *)mem;
	va_end = va_base + size;
	VA_WORD(va_base, 0) = (uint32_t)size;
	VA_WORD(va_base, 1) = VA_MAGIC;
}

void *
view_arena_base(void)
{
	return va_base;
}

/* Merge every free block that follows the free block b into it. */
static void
va_absorb(unsigned char *b)
{
	unsigned char *n = b + VA_SIZE(b);

	while (n < va_end && !VA_ISUSED(n)) {
		VA_WORD(b, 0) = VA_SIZE(b) + VA_SIZE(n);
		n = b + VA_SIZE(b);
	}
}

void *
view_arena_alloc(size_t n)
{
	unsigned char *b;
	uint32_t need;

	if (!va_base || n > (size_t)(va_end - va_base))
		return NULL;
	need = (uint32_t)((n + VA_HDR + 7) & ~(size_t)7);
	if (need < VA_MIN)
		need = VA_MIN;
	for (b = va_base; b < va_end; b += VA_SIZE(b)) {
		if (VA_ISUSED(b))
			continue;
		va_absorb(b);
		if (VA_SIZE(b) >= need) {
			uint32_t rest = VA_SIZE(b) - need;

			VA_UNPOISON(b + VA_HDR, VA_SIZE(b) - VA_HDR);
			if (rest >= VA_MIN) {
				unsigned char *r = b + need;
				VA_WORD(r, 0) = rest;
				VA_WORD(r, 1) = VA_MAGIC;
				VA_WORD(b, 0) = need;
				VA_POISON(r + VA_HDR, rest - VA_HDR);
			}
			VA_WORD(b, 0) |= VA_USED;
			return b + VA_HDR;
		}
	}
	return NULL;
}

int
view_arena_owns(const void *p)
{
	const unsigned char *q = (const unsigned char *)p;

	return va_base && q >= va_base + VA_HDR && q < va_end;
}

void
view_arena_free(void *p)
{
	unsigned char *b = (unsigned char *)p - VA_HDR;

	if (VA_WORD(b, 1) != VA_MAGIC || !VA_ISUSED(b))
		return; /* not a live block: ignore rather than corrupt the chain */
	VA_WORD(b, 0) &= ~VA_USED;
	VA_POISON(b + VA_HDR, VA_SIZE(b) - VA_HDR);
}

size_t
view_arena_usable(const void *p)
{
	const unsigned char *b = (const unsigned char *)p - VA_HDR;

	return VA_SIZE(b) - VA_HDR;
}

size_t
view_arena_largest(void)
{
	unsigned char *b;
	uint32_t best = 0;

	if (!va_base)
		return 0;
	for (b = va_base; b < va_end; b += VA_SIZE(b))
		if (!VA_ISUSED(b)) {
			va_absorb(b);
			if (VA_SIZE(b) > best)
				best = VA_SIZE(b);
		}
	return best > VA_HDR ? best - VA_HDR : 0;
}

size_t
view_arena_free_bytes(void)
{
	unsigned char *b;
	size_t sum = 0;

	if (!va_base)
		return 0;
	for (b = va_base; b < va_end; b += VA_SIZE(b))
		if (!VA_ISUSED(b))
			sum += VA_SIZE(b) - VA_HDR;
	return sum;
}

int
view_arena_check(void)
{
	unsigned char *b = va_base;

	if (!va_base)
		return 0;
	while (b < va_end) {
		if (VA_WORD(b, 1) != VA_MAGIC)
			return 1;
		if (VA_SIZE(b) < VA_MIN || b + VA_SIZE(b) > va_end)
			return 2;
		b += VA_SIZE(b);
	}
	return b == va_end ? 0 : 3;
}
