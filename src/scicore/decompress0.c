/***************************************************************************
 decompress0.c Copyright (C) 1999 Christoph Reichenbach, TU Darmstadt


 This program may be modified and copied freely according to the terms of
 the GNU general public license (GPL), as long as the above copyright
 notice and the licensing information contained herein are preserved.

 Please refer to www.gnu.org for licensing details.

 This work is provided AS IS, without warranty of any kind, expressed or
 implied, including but not limited to the warranties of merchantibility,
 noninfringement, and fitness for a specific purpose. The author will not
 be held liable for any damage caused by this work or derivatives of it.

 By using this source code, you agree to the licensing terms as stated
 above.


 Please contact the maintainer for bug reports or inquiries.

 Current Maintainer:

    Christoph Reichenbach (CJR) [creichen@rbg.informatik.tu-darmstadt.de]

***************************************************************************/
/* Reads data from a resource file and stores the result in memory.
** This is for SCI version 0 style compression.
*/

#include <sci_memory.h>
#include <sciresource.h>

/* #define _SCI_DECOMPRESS_DEBUG */

#ifdef HAVE_PICO
/* pic/view decompress reuses the permanent g_pico_decompress_scratch
   (operations.c) rather than a fresh contiguous malloc, so the fragmented
   post-restore heap never has to find one (the decompress0.c OOM).  Any other
   resource type, or a pic/view bigger than the scratch, falls back to
   sci_malloc.  Caller evicts pic/view data immediately (sci_resmgr.c), so the
   scratch is only ever live for one decode at a time. */
static unsigned char *
pico_decompress_alloc(int type, unsigned int size)
{
	if ((type == sci_pic || type == sci_view)
	    && g_pico_decompress_scratch
	    && size <= PICO_DECOMPRESS_SCRATCH_SIZE)
		return g_pico_decompress_scratch;
	/* Sound is non-essential: on the SRAM-tight Pico its resource may not find
	   a contiguous block.  Use raw malloc so OOM returns NULL (the caller fails
	   the song load and the game keeps running silently) instead of sci_malloc's
	   fatal pico_oom_report halt.  Every other resource type still halts legibly. */
	if (type == sci_sound) {
#ifdef PICO_PSRAM_MAPPED
		/* On the mapped target put it in PSRAM instead. Raw malloc is SRAM,
		   and the flip to a PSRAM default did NOT move raw allocations -- so
		   a large song (PQ2's sound.001 decompresses to 59,153 bytes) was left
		   competing with the visual and priority buffers in SRAM and failed,
		   producing an empty song and dropping the VM into the debugger.
		   PSRAM suits it: the iterator reads the song strictly sequentially,
		   a few bytes per 60Hz tick, so the reads are cached and trivial.
		   Falls back to raw malloc if the PSRAM heap is not up yet, and still
		   returns NULL on genuine exhaustion so the graceful skip is kept. */
		extern void *psram_hmalloc(size_t n);
		unsigned char *psram_buf = (unsigned char *) psram_hmalloc(size);
		if (psram_buf)
			return psram_buf;
#endif
		return (unsigned char *) malloc(size);
	}
	return (unsigned char *) sci_malloc(size);
}

#  define DECOMPRESS_ALLOC_DATA(type, size) pico_decompress_alloc((type), (size))
#  define DECOMPRESS_FREE_DATA(p) \
	do { if ((unsigned char*)(p) != g_pico_decompress_scratch) free(p); } while (0)
#else
#  define DECOMPRESS_ALLOC_DATA(type, size) ((unsigned char*)sci_malloc(size))
#  define DECOMPRESS_FREE_DATA(p) free(p)
#endif

#ifdef PICO_STREAM_DECOMPRESS
/* ---- sliding window over the compressed block ------------------------------
   Replaces the one contiguous sci_malloc_sram(compressedLength) with a small
   fixed window refilled from the fd.  That allocation is the largest single
   contiguous transient left on the resource path -- 59,153 bytes for one PQ2
   resource -- and a recorded OOM site; nothing about the decode actually needs
   the whole block resident.

   The window lives in .bss rather than on the stack ON PURPOSE: decompress0's
   own frame is on the same 8 KB core0 stack that decrypt1's 16 KB of token
   tables once overflowed into live heap (the 2026-06-08 HardFault).  Trading a
   ~59 KB contiguous transient for ~1 KB of permanent .bss is the whole point,
   and it also removes an allocation-failure path instead of adding one.  Costs
   nothing when PICO_STREAM_DECOMPRESS is OFF, which is the default.

   Refill seeks absolutely, so a BACKWARD read is merely slow rather than wrong.
   That is deliberate: it makes the window correct by construction instead of
   correct only while the "strictly forward" reading of decrypt1/decrypt2 holds.

   NOT gated on HAVE_PICO -- tests/decompdiff builds this on the desktop to
   prove it byte-identical to the stock path over every resource of every game. */
/* 4 KB rather than 1 KB: the average method-1 block is ~1.9 KB, so a 1 KB
   window cost ~2-3 refills per resource and measured +50% decompress time on
   device (594 -> 889 ms over SQ3's first five rooms). */
#define STREAM_WINDOW 4096

typedef struct {
	int fd;
	unsigned int start;   /* file offset of logical byte 0 of the block */
	unsigned int total;   /* compressedLength: never read past this */
	unsigned int base;    /* logical offset of stream_win[0] */
	unsigned int fill;    /* valid bytes currently in stream_win */
	unsigned int pos;     /* where the fd actually is, to skip needless seeks */
} decomp_stream_t;

static guint8 stream_win[STREAM_WINDOW];
/* decrypt2's Huffman node table, lifted out of the stream. numnodes is a byte,
   so 255 nodes x 2 bytes is the hard maximum. */
static guint8 stream_nodes[510];

static void
stream_init(decomp_stream_t *s, int fd, unsigned int total)
{
	s->fd = fd;
	/* decompress0 has already consumed the 8-byte header, so the compressed
	   data starts at the current position. */
	s->start = (unsigned int) lseek(fd, 0, SEEK_CUR);
	s->total = total;
	s->base = 0;
	s->fill = 0;
	s->pos = 0;
}

/* Leave the fd exactly where a single read(resh, buffer, compressedLength)
   would have left it.  The window reads ahead, so without this the caller's
   position is desynced -- the one subtlety flagged before any of this was
   written. */
static void
stream_finish(decomp_stream_t *s)
{
	lseek(s->fd, (long) (s->start + s->total), SEEK_SET);
}

static void
stream_refill(decomp_stream_t *s, unsigned int i)
{
	unsigned int want;
	int got;

	if (i >= s->total) {
		s->fill = 0;
		return;
	}
	want = s->total - i;
	if (want > STREAM_WINDOW)
		want = STREAM_WINDOW;

	/* Only seek when the fd is not already there. Refills are sequential in
	   practice, and FatFS f_lseek can walk the FAT chain, so an unconditional
	   seek per refill is the expensive part -- not the read. */
	if (i != s->pos)
		lseek(s->fd, (long) (s->start + i), SEEK_SET);
	got = read(s->fd, stream_win, want);
	s->base = i;
	s->fill = (got > 0) ? (unsigned int) got : 0;
	s->pos = i + s->fill;
}

static guint8
stream_at(decomp_stream_t *s, unsigned int i)
{
	if (i < s->base || i >= s->base + s->fill) {
		stream_refill(s, i);
		if (i < s->base || i >= s->base + s->fill)
			return 0;   /* past end of block, or a short read */
	}
	return stream_win[i - s->base];
}

/* Which methods stream is a per-method COMPILE-TIME choice, because the trade
   is different for each one.  Device-measured over SQ3's first five rooms
   (decompress time, streaming all three vs none): 594 -> 819 ms, +37.9%.  That
   cost is paid per read() and is therefore proportional to RESOURCE COUNT,
   while the benefit is proportional to the largest BLOCK -- and the two point
   in opposite directions:

     method 0   largest 59,153 / 41,747     32 resources in SQ3
     method 1   largest 15,685 / 19,087    497 resources
     method 2   largest 10,385 / 10,429    106 resources

   So method 0 alone captures the whole prize (it is the 59 KB allocation that
   motivated this work) for a small fraction of the cost, and additionally drops
   a 59 KB memcpy rather than adding work.  Hence the default of 1.

   Bit 0 = method 0, bit 1 = method 1, bit 2 = method 2.  7 streams everything. */
#ifndef PICO_STREAM_METHODS
#  define PICO_STREAM_METHODS 1
#endif
#  define STREAM_M0 (PICO_STREAM_METHODS & 1)
#  define STREAM_M1 (PICO_STREAM_METHODS & 2)
#  define STREAM_M2 (PICO_STREAM_METHODS & 4)
#else
#  define STREAM_M0 0
#  define STREAM_M1 0
#  define STREAM_M2 0
#endif

/* Separate accessors per decryptor so each method can be switched independently
   -- one shared SRC() would force decrypt1 and decrypt2 to stream together. */
#if STREAM_M1
#  define SRC1(i) stream_at((decomp_stream_t *) src, (unsigned int) (i))
#else
#  define SRC1(i) src[(i)]
#endif
#if STREAM_M2
#  define SRC2(i) stream_at((decomp_stream_t *) src, (unsigned int) (i))
#else
#  define SRC2(i) src[(i)]
#endif

#ifdef HAVE_PICO
/* decrypt1's two LZW token tables, hoisted off its stack frame — see the long
   comment at the top of decrypt1.  File scope (rather than function-scope
   statics) purely so pico_reset_decrypt_scratch can reach them. */
static guint16 *pico_tokenlist = NULL;        /* pointers to dest[] */
static guint16 *pico_tokenlengthlist = NULL;  /* char length of each token */

/* Called from the chooser after a game exits.  These 16 KB are allocated on the
   first resource decompress and would otherwise stay live forever, pinning the
   next game's heap floor that much higher — the cross-game arena ratchet.  They
   re-allocate on the next game's first decompress. */
void
pico_reset_decrypt_scratch(void)
{
	if (pico_tokenlist)
		free(pico_tokenlist);
	if (pico_tokenlengthlist)
		free(pico_tokenlengthlist);
	pico_tokenlist = NULL;
	pico_tokenlengthlist = NULL;
}
#endif

/* 9-12 bit LZW encoding */
int
decrypt1(guint8 *dest, guint8 *src, int length, int complength)
     /* Doesn't do length checking yet */
{
	/* Theory: Considering the input as a bit stream, we get a series of
	** 9 bit elements in the beginning. Every one of them is a 'token'
	** and either represents a literal (if < 0x100), or a link to a previous
	** token (tokens start at 0x102, because 0x101 is the end-of-stream
	** indicator and 0x100 is used to reset the bit stream decoder).
	** If it's a link, the indicated token and the character following it are
	** placed into the output stream. Note that the 'indicated token' may
	** very well consist of a link-token-plus-literal construct again, so
	** it's possible to represent strings longer than 2 recursively.
	** If the maximum number of tokens has been reached, the bit length is
	** increased by one, up to a maximum of 12 bits.
	** This implementation remembers the position each token was print to in
	** the output array, and the length of this token. This method should
	** be faster than the recursive approach.
	*/

	guint16 bitlen = 9; /* no. of bits to read (max. 12) */
	guint16 bitmask = 0x01ff;
	guint16 bitctr = 0; /* current bit position */
	guint16 bytectr = 0; /* current byte position */
	guint16 token; /* The last received value */
	guint16 maxtoken = 0x200; /* The biggest token */

#ifdef HAVE_PICO
	/* These two 4096-entry arrays are 16 KB combined — larger than the
	   PicoCalc's entire 8 KB core0 stack. As the heap arena ratchets up
	   (across savegame restores) the heap top climbs to within ~16 KB of the
	   stack top, and this single largest frame then punches its locals into
	   live heap → the spilled tokenctr gets overwritten → a wild strh to
	   garbage (the decrypt1 HardFault). Move them off the stack to malloc-once
	   file-scope statics so this frame stays small. decrypt1 is core0-serial
	   (SCI decompression is never concurrent), so non-reentrancy is fine; the
	   buffers are game-independent scratch, allocated once and reused for the
	   rest of the game — but they ARE freed at the chooser
	   (pico_reset_decrypt_scratch), or they would be 16 KB of the next game's
	   inherited heap floor.  These two locals are just ALIASES onto those
	   file-scope buffers, so the frame costs 8 bytes rather than 16 KB and the
	   body below needs no changes. */
	guint16 *tokenlist;
	guint16 *tokenlengthlist;
#else
	guint16 tokenlist[4096]; /* pointers to dest[] */
	guint16 tokenlengthlist[4096]; /* char length of each token */
#endif
	guint16 tokenctr = 0x102; /* no. of registered tokens (starts here)*/

	guint16 tokenlastlength = 0;

	guint16 destctr = 0;

#ifdef HAVE_PICO
	if (!pico_tokenlist) {
		pico_tokenlist       = (guint16 *) sci_malloc(4096 * sizeof(guint16));
		pico_tokenlengthlist = (guint16 *) sci_malloc(4096 * sizeof(guint16));
	}
	tokenlist       = pico_tokenlist;
	tokenlengthlist = pico_tokenlengthlist;
#endif

	while (bytectr < complength) {

		/* The only three reads of the compressed stream in decrypt1, and
		   all of them are at bytectr or bytectr+1 AFTER the increment --
		   i.e. strictly forward with at most 2 bytes of lookahead, which is
		   what makes a small sliding window sufficient here.  SRC1() is
		   plain src[i] unless PICO_STREAM_DECOMPRESS is on. */
		guint32 tokenmaker = SRC1(bytectr++) >> bitctr;
		if (bytectr < complength)
			tokenmaker |= (SRC1(bytectr) << (8-bitctr));
		if (bytectr+1 < complength)
			tokenmaker |= (SRC1(bytectr+1) << (16-bitctr));

		token = tokenmaker & bitmask;

		bitctr += bitlen - 8;

		while (bitctr >= 8) {
			bitctr -= 8;
			bytectr++;
		}

		if (token == 0x101) return 0; /* terminator */
		if (token == 0x100) { /* reset command */
			maxtoken = 0x200;
			bitlen = 9;
			bitmask = 0x01ff;
			tokenctr = 0x0102;
		} else {

			{
				int i;

				if (token > 0xff) {
				  if (token >= tokenctr)
				    {
#ifdef _SCI_DECOMPRESS_DEBUG
				      fprintf(stderr, "decrypt1: Bad token %x!\n", token);
#endif
				      /* Well this is really bad  */
				      /* May be it should throw something like SCI_ERROR_DECOMPRESSION_INSANE */
				    } else
				      {
					tokenlastlength = tokenlengthlist[token]+1;
					if (destctr+tokenlastlength>length)
					  {
#ifdef _SCI_DECOMPRESS_DEBUG

					    /* For me this seems a normal situation, It's necessary to handle it*/
					    printf ("decrypt1: Trying to write beyond the end of array(len=%d, destctr=%d, tok_len=%d)!\n",
						    length, destctr, tokenlastlength);
#endif

					    i = 0;
					    for (; destctr<length; destctr++) {
					      dest[destctr++] = dest [tokenlist[token]+i];
					      i++;
					    }
					  } else
					for (i=0; i< tokenlastlength; i++) {
						dest[destctr++] = dest[tokenlist[token]+i];
					}
				      }
				} else {
					tokenlastlength = 1;
				  if (destctr >= length)
				    {
#ifdef _SCI_DECOMPRESS_DEBUG
				      printf ("decrypt1: Try to write single byte beyond end of array!\n");
#endif
				    } else
					dest[destctr++] = (byte)token;
				}

			}

			if (tokenctr == maxtoken) {
				if (bitlen < 12) {
					bitlen++;
					bitmask <<= 1;
					bitmask |= 1;
					maxtoken <<= 1;
				} else continue; /* no further tokens allowed */
			}

			tokenlist[tokenctr] = destctr-tokenlastlength;
			tokenlengthlist[tokenctr++] = tokenlastlength;

		}

	}

	return 0;

}


/* Huffman-style token encoding */
/***************************************************************************/
/* This code was taken from Carl Muckenhoupt's sde.c, with some minor      */
/* modifications.                                                          */
/***************************************************************************/

/* decrypt2 helper function */
gint16 getc2(guint8 *node, guint8 *src,
	     guint16 *bytectr, guint16 *bitctr, int complength)
{
	guint16 next;

	while (node[1] != 0) {
		/* node[] is a genuine pointer either way -- under streaming it
		   points at the SRAM copy of the Huffman table, which is what
		   makes decrypt2 streamable at all. Only the bitstream reads
		   below go through the window. */
		gint16 value = (SRC2(*bytectr) << (*bitctr));
		(*bitctr)++;
		if (*bitctr == 8) {
			(*bitctr) = 0;
			(*bytectr)++;
		}

		if (value & 0x80) {
			next = node[1] & 0x0f; /* low 4 bits */
			if (next == 0) {
				guint16 result = (SRC2(*bytectr) << (*bitctr));

				if (++(*bytectr) > complength)
					return -1;
				else if (*bytectr < complength)
					result |= SRC2(*bytectr) >> (8-(*bitctr));

				result &= 0x0ff;
				return (result | 0x100);
			}
		}
		else {
			next = node[1] >> 4;  /* high 4 bits */
		}
		node += next<<1;
	}
	return getInt16(node);
}

/* Huffman token decryptor */
int decrypt2(guint8* dest, guint8* src, int length, int complength)
     /* no complength checking atm */
{
	guint8 numnodes, terminator;
	guint8 *nodes;
	gint16 c;
	guint16 bitctr = 0, bytectr;

#if STREAM_M2
	/* The ONLY random access in decrypt2 is the Huffman node table, which
	   getc2 walks by node += next<<1. It is bounded at 510 bytes because
	   numnodes is src[0], a single byte -- so copy it to SRAM once and the
	   remainder of the resource is a forward bitstream the window can serve.
	   That bound is what makes method 2 streamable; without it the random
	   walk would thrash the window. */
	{
		unsigned int k, tbytes;

		numnodes = SRC2(0);
		terminator = SRC2(1);
		tbytes = (unsigned int) numnodes << 1;
		for (k = 0; k < tbytes; k++)
			stream_nodes[k] = SRC2(2 + k);
		nodes = stream_nodes;
	}
	bytectr = 2 + (numnodes << 1);
#else
	numnodes = src[0];
	terminator = src[1];
	bytectr = 2+ (numnodes << 1);
	nodes = src+2;
#endif

	while (((c = getc2(nodes, src, &bytectr, &bitctr, complength))
		!= (0x0100 | terminator)) && (c >= 0)) {
		if (length-- == 0) return SCI_ERROR_DECOMPRESSION_OVERFLOW;

		*dest = (guint8)c;
		dest++;
	}

	return (c == -1) ? SCI_ERROR_DECOMPRESSION_OVERFLOW : 0;

}
/***************************************************************************/
/* Carl Muckenhoupt's decompression code ends here                         */
/***************************************************************************/

int sci0_get_compression_method(int resh)
{
	guint16 compressedLength;
	guint16 compressionMethod;
	guint16 result_size;

	/* Dummy variable */
	if (read(resh, &result_size, 2) != 2)
		return SCI_ERROR_IO_ERROR;

	if ((read(resh, &compressedLength, 2) != 2) ||
	    (read(resh, &result_size, 2) != 2) ||
	    (read(resh, &compressionMethod, 2) != 2))
		return SCI_ERROR_IO_ERROR;

#ifdef WORDS_BIGENDIAN
	compressionMethod = GUINT16_SWAP_LE_BE_CONSTANT(compressionMethod);
#endif

	return compressionMethod;
}


#if defined(FSCI_PROBE_PERF) && defined(HAVE_PICO)
/* Time decompress0 itself, because the obvious instrument is the WRONG one for
   the streaming work: [perf] pic N decode covers a pic, and pics are method 2
   (or 0) -- never method 1 -- so it is structurally blind to what phase 1
   changed. Method 1 is views/scripts/text/sound, which this counts directly.
   Printed per room by the pic-decode probe in operations.c. */
unsigned long long pico_decomp_us = 0;
unsigned long pico_decomp_count = 0;
unsigned long pico_decomp_bytes = 0;
extern unsigned long long pico_perf_us(void);
#endif

int decompress0(resource_t *result, int resh, int sci_version)
{
#if defined(FSCI_PROBE_PERF) && defined(HAVE_PICO)
	unsigned long long _dc_t0 = pico_perf_us();
#  define DECOMP_ACCOUNT(nbytes) \
	do { pico_decomp_us += pico_perf_us() - _dc_t0; \
	     pico_decomp_count++; pico_decomp_bytes += (nbytes); } while (0)
#else
#  define DECOMP_ACCOUNT(nbytes) do { } while (0)
#endif
	guint16 compressedLength;
	guint16 compressionMethod;
	guint16 result_size;
	guint8 *buffer;
#ifdef PICO_STREAM_DECOMPRESS
	/* Small (~20 byte) descriptor; the 1 KB window itself is file-scope .bss,
	   so this does NOT grow decompress0's stack frame. */
	decomp_stream_t stm;
	int streaming = 0;
#endif

	if (read(resh, &(result->id),2) != 2)
		return SCI_ERROR_IO_ERROR;

#ifdef WORDS_BIGENDIAN
	result->id = GUINT16_SWAP_LE_BE_CONSTANT(result->id);
#endif
	result->number = result->id & 0x07ff;
	result->type = result->id >> 11;

	if ((result->number > sci_max_resource_nr[sci_version]) || (result->type > sci_invalid_resource))
		return SCI_ERROR_DECOMPRESSION_INSANE;

	if ((read(resh, &compressedLength, 2) != 2) ||
	    (read(resh, &result_size, 2) != 2) ||
	    (read(resh, &compressionMethod, 2) != 2))
		return SCI_ERROR_IO_ERROR;

#ifdef WORDS_BIGENDIAN
	compressedLength = GUINT16_SWAP_LE_BE_CONSTANT(compressedLength);
	result_size = GUINT16_SWAP_LE_BE_CONSTANT(result_size);
	compressionMethod = GUINT16_SWAP_LE_BE_CONSTANT(compressionMethod);
#endif
	result->size = result_size;

	if ((result->size > SCI_MAX_RESOURCE_SIZE) ||
	    (compressedLength > SCI_MAX_RESOURCE_SIZE))
		return SCI_ERROR_RESOURCE_TOO_BIG;
	/* With SCI0, this simply cannot happen. */

	if (compressedLength > 4)
		compressedLength -= 4;
	else { /* Object has size zero (e.g. view.000 in sq3) (does this really exist?) */
		result->data = 0;
		result->status = SCI_STATUS_NOMALLOC;
		return SCI_ERROR_EMPTY_OBJECT;
	}

#ifdef PICO_STREAM_DECOMPRESS
	/* Phase 1: only method 1 (LZW) streams.  Methods 0 and 2 still take the
	   flat buffer, so they must still allocate it.  Method 1 is the dominant
	   case by a wide margin -- measured over SQ3/PQ2/KQ4, 1747 of 2139
	   resources -- so this already removes most of the large contiguous
	   allocations. */
	if ((compressionMethod == 0 && STREAM_M0) ||
	    (compressionMethod == 1 && STREAM_M1) ||
	    (compressionMethod == 2 && STREAM_M2)) {
		buffer = NULL;
		stream_init(&stm, resh, compressedLength);
		streaming = 1;
	} else
		buffer = (guint8*)sci_malloc_sram(compressedLength);
#else
	buffer = (guint8*)sci_malloc_sram(compressedLength);
#endif
	result->data = DECOMPRESS_ALLOC_DATA(result->type, result->size);

#ifdef HAVE_PICO
	/* A NULL here means the graceful (raw-malloc) path in pico_decompress_alloc
	   OOM'd on a non-essential (sound) resource.  Fail the decode cleanly so the
	   resource load returns empty and the game keeps running without the song. */
	if (!result->data) {
		free(buffer);
		result->status = SCI_STATUS_NOMALLOC;
		return SCI_ERROR_DECOMPRESSION_INSANE;
	}
#endif

#ifdef PICO_STREAM_DECOMPRESS
	if (!streaming)
#endif
	if (read(resh, buffer, compressedLength) != compressedLength) {
		DECOMPRESS_FREE_DATA(result->data);
		free(buffer);
		return SCI_ERROR_IO_ERROR;
	};


#ifdef _SCI_DECOMPRESS_DEBUG
	fprintf(stderr, "Resource %s.%03hi encrypted with method %hi at %.2f%%"
		" ratio\n",
		sci_resource_types[result->type], result->number, compressionMethod,
		(result->size == 0)? -1.0 :
		(100.0 * compressedLength / result->size));
	fprintf(stderr, "  compressedLength = 0x%hx, actualLength=0x%hx\n",
		compressedLength, result->size);
#endif

	switch(compressionMethod) {

	case 0: /* no compression */
		if (result->size != compressedLength) {
			DECOMPRESS_FREE_DATA(result->data);
			result->data = NULL;
			result->status = SCI_STATUS_NOMALLOC;
			free(buffer);
			return SCI_ERROR_DECOMPRESSION_OVERFLOW;
		}
#if STREAM_M0
		/* THE case this whole exercise was for: the largest compressed
		   blocks in every game measured are method 0 (pq2 sound.001 at
		   59,153 bytes, kq4 sound.200 at 41,747), and method 0 needs no
		   window at all -- the bytes go straight to their destination.
		   This removes the contiguous input allocation AND the 59 KB
		   memcpy that used to follow it. */
		{
			unsigned int done = 0;

			while (done < compressedLength) {
				unsigned int want = compressedLength - done;
				int got;

				if (want > STREAM_WINDOW)
					want = STREAM_WINDOW;
				got = read(resh, result->data + done, want);
				if (got <= 0)
					break;
				done += (unsigned int) got;
			}
			stream_finish(&stm);
			if (done != compressedLength) {
				DECOMPRESS_FREE_DATA(result->data);
				result->data = NULL;
				result->status = SCI_STATUS_NOMALLOC;
				return SCI_ERROR_IO_ERROR;
			}
		}
#else
		memcpy(result->data, buffer, compressedLength);
#endif
		result->status = SCI_STATUS_ALLOCATED;
		break;

	case 1: /* LZW compression */
	{
		int rc;

		/* rc is computed first so stream_finish runs on the failure path
		   too -- the fd position has to match the stock path's whether the
		   decode succeeded or not. */
#if STREAM_M1
		rc = decrypt1(result->data, (guint8 *) &stm, result->size,
			      compressedLength);
		stream_finish(&stm);
#else
		rc = decrypt1(result->data, buffer, result->size, compressedLength);
#endif
		if (rc) {
			DECOMPRESS_FREE_DATA(result->data);
			result->data = 0; /* So that we know that it didn't work */
			result->status = SCI_STATUS_NOMALLOC;
			free(buffer);
			return SCI_ERROR_DECOMPRESSION_OVERFLOW;
		}
	}
		result->status = SCI_STATUS_ALLOCATED;
		break;

	case 2: /* Some sort of Huffman encoding */
	{
		int rc;

#if STREAM_M2
		rc = decrypt2(result->data, (guint8 *) &stm, result->size,
			      compressedLength);
		stream_finish(&stm);
#else
		rc = decrypt2(result->data, buffer, result->size, compressedLength);
#endif
		if (rc) {
			DECOMPRESS_FREE_DATA(result->data);
			result->data = 0; /* So that we know that it didn't work */
			result->status = SCI_STATUS_NOMALLOC;
			free(buffer);
			return SCI_ERROR_DECOMPRESSION_OVERFLOW;
		}
	}
		result->status = SCI_STATUS_ALLOCATED;
		break;

	default:
		fprintf(stderr,"Resource %s.%03hi: Compression method %hi not "
			"supported!\n", sci_resource_types[result->type], result->number,
			compressionMethod);
		DECOMPRESS_FREE_DATA(result->data);
		result->data = 0; /* So that we know that it didn't work */
		result->status = SCI_STATUS_NOMALLOC;
		free(buffer);
		return SCI_ERROR_UNKNOWN_COMPRESSION;
	}

	free(buffer);
	DECOMP_ACCOUNT(compressedLength);
	return 0;
}

