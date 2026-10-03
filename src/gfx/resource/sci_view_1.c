/***************************************************************************
 view_1.c Copyright (C) 2000 Christoph Reichenbach


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

    Christoph Reichenbach (CR) <jameson@linuxgames.com>

***************************************************************************/
/* SCI 1 view resource defrobnicator */

#include <sci_memory.h>
#include <gfx_system.h>
#include <gfx_resource.h>
#include <gfx_tools.h>
#ifdef HAVE_PICO
#include "psram_alloc.h"
/* The permanent 32 KB pic-priority decode scratch (operations.c), idle outside
   pic decode -- borrowed per cel as in sci_view_0.c. */
extern byte *g_pico_priority_scratch;

/* Re-decoding a view that the VGA view cache evicted (resmgr.c) puts its cels
   back where they were in PSRAM instead of growing the arena: decoding is
   deterministic, so the cels come out in the same order with the same sizes.
   sci_resmgr.c sets the cursor around the decode. */
#if defined(PICO_STREAM_DECOMPRESS) && !defined(PICO_PSRAM_MAPPED)
static uint32_t s_cel_reuse_cursor;
static int s_cel_reuse = 0;

void pico_view_cels_reuse(uint32_t base) { s_cel_reuse_cursor = base; s_cel_reuse = 1; }
uint32_t pico_view_cels_reuse_end(void) { s_cel_reuse = 0; return s_cel_reuse_cursor; }

static uint32_t
pico_cel_psram_alloc(size_t sz)
{
	if (s_cel_reuse) {
		uint32_t a = s_cel_reuse_cursor;
		s_cel_reuse_cursor += (uint32_t) sz;
		return a;
	}
	return psram_alloc(sz);
}
#else
#  define pico_cel_psram_alloc(sz) psram_alloc(sz)
#endif
#endif

#if defined(HAVE_PICO) && defined(PICO_STREAM_DECOMPRESS) && !defined(PICO_PSRAM_MAPPED)
#  define PICO_VIEW_TO_PSRAM 1
/* A VGA view can live in PSRAM instead of SRAM (gfxr_draw_view1_psram,
   Jones in the Fast Lane: 14 views are 16-35 KB decompressed). The decoder
   below passes resource pointers around by offset (resource + cel_offset); for
   a PSRAM view the base pointer is NULL, so a pointer IS its offset and every
   read goes through a 512-byte cache over the PSRAM copy. Desktop and
   SRAM-resident views read the pointer directly. */
#define V1_CACHE 512
static struct { uint32_t addr; int size; int base; byte buf[V1_CACHE]; } _v1c;
static int _v1_psram = 0;

static byte
_v1_rb(unsigned int off)
{
	if (off >= (unsigned int) _v1c.size)
		return 0;
	if ((int) off < _v1c.base || (int) off >= _v1c.base + V1_CACHE) {
		int n = _v1c.size - (int) off;
		if (n > V1_CACHE)
			n = V1_CACHE;
		psram_load(_v1c.addr + off, _v1c.buf, n);
		_v1c.base = (int) off;
	}
	return _v1c.buf[off - _v1c.base];
}
#  define VRB(p)   (_v1_psram ? _v1_rb((unsigned int)(size_t)(p)) : *(byte *)(p))
#  define VR16(p)  (_v1_psram ? (gint16)(_v1_rb((unsigned int)(size_t)(p)) | (_v1_rb((unsigned int)(size_t)(p) + 1) << 8)) \
			      : get_int_16(p))
#  define VRU16(p) (_v1_psram ? (guint16)(_v1_rb((unsigned int)(size_t)(p)) | (_v1_rb((unsigned int)(size_t)(p) + 1) << 8)) \
			      : get_uint_16(p))
#  define VRCOPY(d, p, n) do { if (_v1_psram) { int _k; for (_k = 0; _k < (n); _k++) \
		(d)[_k] = _v1_rb((unsigned int)(size_t)(p) + _k); } else memcpy((d), (p), (n)); } while (0)
#else
#  define VRB(p)   (*(byte *)(p))
#  define VR16(p)  get_int_16(p)
#  define VRU16(p) get_uint_16(p)
#  define VRCOPY(d, p, n) memcpy((d), (p), (n))
#endif

#define V1_LOOPS_NR_OFFSET 0
#define V1_MIRROR_MASK 2
#define V1_PALETTE_OFFSET 6
#define V1_FIRST_LOOP_OFFSET 8

#define V1_PAL_START 260 /* sci_pal_1.c PALETTE_START: first of 256 4-byte entries */
#define V1_RLE 0x80 /* run-length encode? */
#define V1_RLE_BG 0x40 /* background fill */

#define NEXT_RUNLENGTH_BYTE(n) \
  if (literal_pos == runlength_pos) \
    literal_pos += n; \
  runlength_pos += n; 

#define NEXT_LITERAL_BYTE(n) \
  if (literal_pos == runlength_pos) \
    runlength_pos += n; \
  literal_pos += n; 

static int
decompress_sci_view(int id, int loop, int cel, byte *resource, byte *dest, int mirrored, int pixmap_size, int size, 
		    int runlength_pos, int literal_pos, int xl, int yl, int color_key)
{
	int writepos = mirrored? xl : 0;

	if (mirrored) {
		int linebase = 0;

		while (linebase < pixmap_size && literal_pos < size && runlength_pos < size) {
			int op = VRB(resource + runlength_pos);
			int bytes;
			int readbytes = 0;
			int color;

			NEXT_RUNLENGTH_BYTE(1);

			if (op & V1_RLE) {
				bytes = op & 0x3f;
				op &= (V1_RLE | V1_RLE_BG);
				readbytes = (op & V1_RLE_BG)? 0 : 1;
			} else {
				readbytes = bytes = op & 0x3f;
				op = 0;
			}

			if (runlength_pos + readbytes > size)
			{
				GFXWARN("View %02x:(%d/%d) requires %d bytes to be read when %d are available at pos %d\n",
					id, loop, cel, readbytes, size - runlength_pos, runlength_pos-1);
				return 1;
			}
/*
			if (writepos - bytes < 0) {
				GFXWARN("View %02x:(%d/%d) describes more bytes than needed: %d/%d bytes at rel. offset 0x%04x\n",
					id, loop, cel, writepos - bytes, pixmap_size, pos - 1);
				bytes = pixmap_size - writepos;
			}
*/			
			if (op==V1_RLE) 
			{
				color = VRB(resource + literal_pos);
				NEXT_LITERAL_BYTE(1);
			}

			if (!op && literal_pos + bytes > size)
			{
				GFXWARN("View %02x:(%d/%d) requires %d bytes to be read when %d are available at pos %d\n",
					id, loop, cel, bytes, size - literal_pos, literal_pos-1);
				return 1;
			}

			while (bytes--)
			{
				if (op) {
					if (op & V1_RLE_BG) {
						writepos--;
						*(dest + writepos) = color_key;
					} else {
						writepos--;
						*(dest + writepos) = color;
					}
				} else {
					writepos--;
					*(dest + writepos) = VRB(resource + literal_pos);
					NEXT_LITERAL_BYTE(1);
					
				}
				if (writepos == linebase)
				{
					writepos+=2*xl;
					linebase+=xl;
				}
			}
		}
	}
        else {
		while (writepos < pixmap_size && literal_pos < size && runlength_pos < size) {
			int op = VRB(resource + runlength_pos);
			int bytes;
			int readbytes = 0;

			NEXT_RUNLENGTH_BYTE(1);

			if (op & V1_RLE) {
				bytes = op & 0x3f;
				op &= (V1_RLE | V1_RLE_BG);
				readbytes = (op & V1_RLE_BG)? 0 : 1;
			} else {
				readbytes = bytes = op & 0x3f;
				op = 0;
			}

			if (runlength_pos + readbytes > size) {
				return 1;
			}

			if (writepos + bytes > pixmap_size) {
				GFXWARN("View %02x:(%d/%d) describes more bytes than needed: %d/%d bytes at rel. offset 0x%04x\n",
					id, loop, cel, writepos-bytes, pixmap_size, runlength_pos - 1);
				bytes = pixmap_size - writepos;
			}

			if (!op && literal_pos + bytes > size)
			{
				GFXWARN("View %02x:(%d/%d) requires %d bytes to be read when %d are available at pos %d\n",
					id, loop, cel, bytes, size - literal_pos, literal_pos-1);
				return 1;
			}

			if (writepos + bytes > pixmap_size)
			{
				GFXWARN("Writing out of bounds: %d bytes at %d > size %d\n", bytes, writepos, pixmap_size);
			}
					
			if (op) {
				if (op & V1_RLE_BG)
					memset(dest + writepos, color_key, bytes);
				else {
					int color = VRB(resource + literal_pos);

					NEXT_LITERAL_BYTE(1);
					memset(dest + writepos, color, bytes);
				}
			} else {
				VRCOPY(dest + writepos, resource + literal_pos, bytes);
				NEXT_LITERAL_BYTE(bytes);
			}
			writepos += bytes;

		}

	};

	return 0;
}

static int
decompress_sci_view_amiga(int id, int loop, int cel, byte *resource, byte *dest, int mirrored, int pixmap_size, int size, 
		    int pos, int xl, int yl, int color_key)
{
	int writepos = mirrored? xl - 1 : 0;

	while (writepos < pixmap_size && pos < size) {
		int op = VRB(resource + pos); pos++;
		int bytes;
		int color;

		if (op & 0x07) {
			bytes = op & 0x07;
			color = op >> 3;
		} else {
			bytes = op >> 3;
			color = color_key;
		}

		if (mirrored) {
			while (bytes--) {
				dest[writepos--] = color;
				/* If we've just written the first pixel of a line... */
				if (!((writepos + 1) % xl)) {
					/* Then move to the end of next line */
					writepos += 2 * xl;

					if (writepos >= pixmap_size && bytes)
					{
						GFXWARN("View %02x:(%d/%d) writing out of bounds\n", id, loop, cel);
						break;
					}
				}
			}
		} else {
			if (writepos + bytes > pixmap_size) {
				GFXWARN("View %02x:(%d/%d) describes more bytes than needed: %d/%d bytes at rel. offset 0x%04x\n",
					id, loop, cel, writepos-bytes, pixmap_size, pos - 1);
				bytes = pixmap_size - writepos;
			}
			memset(dest + writepos, color, bytes);
			writepos += bytes;
		}
	}

	if (writepos < pixmap_size) {
			GFXWARN("View %02x:(%d/%d) not enough pixel data in view\n", id, loop, cel);
			return 1;
	}

	return 0;
}

gfx_pixmap_t *
gfxr_draw_cel1(int id, int loop, int cel, int mirrored, byte *resource, int size, gfxr_view_t *view, int amiga_game)
{
	int xl = VR16(resource);
	int yl = VR16(resource + 2);
	int xhot = (gint8) VRB(resource + 4);
	int yhot = (guint8) VRB(resource + 5);
	int pos = 8;
	int pixmap_size = xl * yl;
	gfx_pixmap_t *retval;
	byte *dest;
	int decompress_failed;
#ifdef HAVE_PICO
	/* A cel of a real view (view != NULL; the pic decoder's embedded-view path
	   passes NULL and keeps index_data) decodes into the idle 32 KB priority
	   scratch and goes to PSRAM right after, as in sci_view_0.c: the SRAM peak
	   is one cel, not the whole view (Jones in the Fast Lane: up to 85 KB of
	   cels in one view, largest cel 20,496 B). index_data stays NULL so the
	   error paths never free the scratch. */
	int dest_is_scratch = 0;

	retval = gfx_new_pixmap(xl, yl, id, loop, cel);
	if (view && xl > 0 && yl > 0 && pixmap_size <= ((GFXR_AUX_MAP_SIZE + 1) >> 1)
	    && g_pico_priority_scratch) {
		dest = g_pico_priority_scratch;
		memset(dest, 0, pixmap_size);
		dest_is_scratch = 1;
	} else {
		gfx_pixmap_alloc_index_data(retval);
		dest = retval->index_data;
	}
#else
	retval = gfx_pixmap_alloc_index_data(gfx_new_pixmap(xl, yl, id, loop, cel));
	dest = retval->index_data;
#endif

	retval->color_key = VRB(resource + 6);
	retval->xoffset = (mirrored)? xhot : -xhot;
	retval->yoffset = -yhot;

	if (view) {
		retval->colors = view->colors;
		retval->colors_nr = view->colors_nr;
#ifdef HAVE_PICO
		retval->pico_pal_insert = view->pico_pal;
#endif
	}
		
	retval->flags |= GFX_PIXMAP_FLAG_EXTERNAL_PALETTE;

	if (xl <= 0 || yl <= 0) {
		gfx_free_pixmap(NULL, retval);
		GFXERROR("View %02x:(%d/%d) has invalid xl=%d or yl=%d\n", id, loop, cel, xl, yl);
		return NULL;
	}

	if (amiga_game)
		decompress_failed = decompress_sci_view_amiga(id, loop, cel,
			resource, dest, mirrored, pixmap_size, size, pos, 
			xl, yl, retval->color_key);
	else
		decompress_failed = decompress_sci_view(id, loop, cel,
			resource, dest, mirrored, pixmap_size, size, pos, 
			pos, xl, yl, retval->color_key);

	if (decompress_failed) {
		gfx_free_pixmap(NULL, retval);
		return NULL;
	}

#ifdef HAVE_PICO
	if (view) {
		size_t sz = (size_t) pixmap_size;
		retval->psram_addr  = pico_cel_psram_alloc(sz);
		retval->psram_valid = 1;
		psram_store(retval->psram_addr, dest, sz);
		if (!dest_is_scratch)
			free(dest);
		retval->index_data = NULL;
	}
#endif
	return retval;
}

static int
gfxr_draw_loop1(gfxr_loop_t *dest, int id, int loop, int mirrored, byte *resource, int offset, int size, gfxr_view_t *view, int amiga_game)
{
	int i;
	int cels_nr = VR16(resource + offset);

	if (VRU16(resource + offset + 2)) {
		GFXWARN("View %02x:(%d): Gray magic %04x in loop, expected white\n", id, loop, VRU16(resource + offset + 2));
	}

	if (cels_nr * 2 + 4 + offset > size) {
		GFXERROR("View %02x:(%d): Offset array for %d cels extends beyond resource space\n", id, loop, cels_nr);
		dest->cels_nr = 0; /* Mark as "delete no cels" */
		dest->cels = NULL;
		return 1;
	}

	dest->cels = (gfx_pixmap_t**)sci_malloc(sizeof(gfx_pixmap_t *) * cels_nr);

	for (i = 0; i < cels_nr; i++) {
		int cel_offset = VRU16(resource + offset + 4 + (i << 1));
		gfx_pixmap_t *cel;

		if (cel_offset >= size) {
			GFXERROR("View %02x:(%d/%d) supposed to be at illegal offset 0x%04x\n", id, loop, i, cel_offset);
			cel = NULL;
		} else
			cel = gfxr_draw_cel1(id, loop, i, mirrored, resource + cel_offset, size - cel_offset, view, amiga_game);

		if (!cel) {
			dest->cels_nr = i;
			return 1;
		}

		dest->cels[i] = cel;
	}

	dest->cels_nr = cels_nr;

	return 0;
}


#define V1_FIRST_MAGIC 1
#define V1_MAGICS_NR 5
/*static byte view_magics[V1_MAGICS_NR] = {0x80, 0x00, 0x00, 0x00, 0x00};*/

gfxr_view_t *
gfxr_draw_view1(int id, byte *resource, int size, gfx_pixmap_color_t *static_pal,
		int static_pal_nr)
{
	int i;
	int palette_offset;
	gfxr_view_t *view;
	int mirror_mask;
	int amiga_game = 0;

	if (size < V1_FIRST_LOOP_OFFSET + 8) {
		GFXERROR("Attempt to draw empty view %04x\n", id);
		return NULL;
	}

	view = (gfxr_view_t*)sci_malloc(sizeof(gfxr_view_t));
#ifdef HAVE_PICO
	view->pico_pal = NULL;
#endif
	view->ID = id;
	view->flags = 0;

	view->loops_nr = VRB(resource + V1_LOOPS_NR_OFFSET);
	palette_offset = VRU16(resource + V1_PALETTE_OFFSET);
	mirror_mask = VRU16(resource + V1_MIRROR_MASK);

	if (view->loops_nr * 2 + V1_FIRST_LOOP_OFFSET > size) {
		GFXERROR("View %04x: Not enough space in resource to accomodate for the claimed %d loops\n", id, view->loops_nr);
		free(view);
		return NULL;
	}

/*	fprintf(stderr, "View flags are 0x%02x\n", resource[3]);*/

/*
	for (i = 0; i < V1_MAGICS_NR; i++)
		if (resource[V1_FIRST_MAGIC + i] != view_magics[i]) {
			GFXWARN("View %04x: View magic #%d should be %02x but is %02x\n",
				id, i, view_magics[i], resource[V1_FIRST_MAGIC + i]);
		}
*/

#ifdef HAVE_PICO
	/* The Pico draws 256-colour cels through the identity LUT into the LCD's
	   palette slots, so a view does not need its own 2 KB colour table: keep
	   only its USED entries (byte 0 of each 4-byte entry), which
	   pico_blit_indexed writes into the palette when a cel is drawn -- as
	   Sierra's SCI1 interpreter inserts a view's palette -- and point colors at
	   the shared static palette. */
	if (palette_offset > 0 && static_pal && static_pal_nr == 256
	    && palette_offset + V1_PAL_START + 256 * 4 <= size) {
		int c, n = 0;
		byte *pe = resource + palette_offset + V1_PAL_START;

		for (c = 0; c < 256; c++)
			if (VRB(pe + c * 4))
				n++;
		view->pico_pal = (gfx_pal_insert_t *) sci_malloc(sizeof(gfx_pal_insert_t) + 4 * n);
		view->pico_pal->n = n;
		for (c = 0, n = 0; c < 256; c++)
			if (VRB(pe + c * 4)) {
				view->pico_pal->e[n][0] = c;
				view->pico_pal->e[n][1] = VRB(pe + c * 4 + 1);
				view->pico_pal->e[n][2] = VRB(pe + c * 4 + 2);
				view->pico_pal->e[n][3] = VRB(pe + c * 4 + 3);
				n++;
			}
		view->colors = static_pal;
		view->colors_nr = static_pal_nr;
		view->flags |= GFX_PIXMAP_FLAG_EXTERNAL_PALETTE;
	} else
#endif
	if (palette_offset > 0)
	{
	    if (palette_offset > size) {
		GFXERROR("Palette is outside of view %04x\n", id);
		free(view);
		return NULL;
	    }   
	    byte *pal_src = resource + palette_offset;
#ifdef PICO_VIEW_TO_PSRAM
	    /* PSRAM view: page the palette (to the end of the resource, as the
	       desktop call reads) into a buffer; gfxr_read_pal1 copies it. */
	    byte *pal_buf = NULL;
	    if (_v1_psram) {
		pal_buf = (byte *) sci_malloc(size - palette_offset);
		VRCOPY(pal_buf, resource + palette_offset, size - palette_offset);
		pal_src = pal_buf;
	    }
#endif
	    view->colors = gfxr_read_pal1(id, &(view->colors_nr), pal_src, size - palette_offset);
#ifdef PICO_VIEW_TO_PSRAM
	    if (pal_buf)
		free(pal_buf);
#endif
	    if (!view->colors) {
		GFXERROR("view %04x: Palette reading failed. Aborting...\n", id);
		free(view);
		return NULL;
	    }
	} else if (static_pal_nr == GFX_SCI1_AMIGA_COLORS_NR) {
		/* Assume we're running an amiga game. */
		amiga_game = 1;
		view->colors = static_pal;
		view->colors_nr = static_pal_nr;
		view->flags |= GFX_PIXMAP_FLAG_EXTERNAL_PALETTE;
	} else {
	    GFXWARN("view %04x: Doesn't have a palette. Can FreeSCI handle this?\n", view->ID);
	    view->colors = NULL;
	    view->colors_nr = 0;
	}

	view->loops = (gfxr_loop_t*)sci_malloc(sizeof (gfxr_loop_t) * view->loops_nr);

	for (i = 0; i < view->loops_nr; i++) {
		int error_token = 0;
		int loop_offset = VRU16(resource + V1_FIRST_LOOP_OFFSET + (i << 1));

		if (loop_offset >= size) {
			GFXERROR("View %04x:(%d) supposed to be at illegal offset 0x%04x\n", id, i);
			error_token = 1;
		}

		if (error_token || gfxr_draw_loop1(view->loops + i, id, i, mirror_mask & (1<<i), 
						   resource, loop_offset, size, view, amiga_game)) {
			/* An error occured */
			view->loops_nr = i;
			gfxr_free_view(NULL, view);
			return NULL;
		}
	}

	return view;
}

#ifdef PICO_VIEW_TO_PSRAM
/* Decode a VGA view whose decompressed resource sits in PSRAM at 'addr'
   (scir_pico_load_to_psram): the same decoder, reading through the cache. */
gfxr_view_t *
gfxr_draw_view1_psram(int id, uint32_t addr, int size, gfx_pixmap_color_t *static_pal,
		      int static_pal_nr)
{
	gfxr_view_t *view;

	_v1c.addr = addr;
	_v1c.size = size;
	_v1c.base = -(V1_CACHE + 1);
	_v1_psram = 1;
	view = gfxr_draw_view1(id, NULL, size, static_pal, static_pal_nr);
	_v1_psram = 0;
	return view;
}
#endif

#define V2_HEADER_SIZE 0
#define V2_LOOPS_NUM 2
#define V2_PALETTE_OFFSET 8
#define V2_BYTES_PER_LOOP 12
#define V2_BYTES_PER_CEL 13

#define V2_IS_MIRROR 1
#define V2_COPY_OF_LOOP 2
#define V2_CELS_NUM 4
#define V2_LOOP_OFFSET 14

#define V2_CEL_WIDTH 0
#define V2_CEL_HEIGHT 2
#define V2_X_DISPLACEMENT 4
#define V2_Y_DISPLACEMENT 6
#define V2_COLOR_KEY 8
#define V2_RUNLENGTH_OFFSET 24
#define V2_LITERAL_OFFSET 28

gfx_pixmap_t *
gfxr_draw_cel11(int id, int loop, int cel, int mirrored, byte *resource_base, byte *cel_base, int size, gfxr_view_t *view)
{
	int xl = get_uint_16(cel_base + V2_CEL_WIDTH);
	int yl = get_uint_16(cel_base + V2_CEL_HEIGHT);
	int xdisplace = get_uint_16(cel_base + V2_X_DISPLACEMENT);
	int ydisplace = get_uint_16(cel_base + V2_Y_DISPLACEMENT);
	int runlength_offset = get_uint_16(cel_base + V2_RUNLENGTH_OFFSET);
	int literal_offset = get_uint_16(cel_base + V2_LITERAL_OFFSET);
	int pixmap_size = xl * yl;
	
	gfx_pixmap_t *retval = gfx_pixmap_alloc_index_data(gfx_new_pixmap(xl, yl, id, loop, cel));
	byte *dest = retval->index_data;
	int decompress_failed;

	retval->color_key = cel_base[V2_COLOR_KEY];
	retval->xoffset = (mirrored)? xdisplace : -xdisplace;
	retval->yoffset = -ydisplace;

	if (view) {
		retval->colors = view->colors;
		retval->colors_nr = view->colors_nr;
	}
		
	retval->flags |= GFX_PIXMAP_FLAG_EXTERNAL_PALETTE;

	if (xl <= 0 || yl <= 0) {
		gfx_free_pixmap(NULL, retval);
		GFXERROR("View %02x:(%d/%d) has invalid xl=%d or yl=%d\n", id, loop, cel, xl, yl);
		return NULL;
	}

	decompress_failed = decompress_sci_view(id, loop, cel, resource_base, dest, mirrored, pixmap_size, size,
						runlength_offset, literal_offset, xl, yl, retval->color_key);

	if (decompress_failed) {
		gfx_free_pixmap(NULL, retval);
		return NULL;
	}

	return retval;
}

gfxr_loop_t *
gfxr_draw_loop11(int id, int loop, int mirrored, byte *resource_base, byte *loop_base, int size, int cels_nr,
		 gfxr_loop_t *result, gfxr_view_t *view, int bytes_per_cel)
{
	byte *seeker = loop_base;
	int i;

	result->cels_nr = cels_nr;
	result->cels = (gfx_pixmap_t **)
		sci_malloc(sizeof(gfx_pixmap_t *) * cels_nr);

	for (i = 0; i < cels_nr; i++)
	{
		result->cels[i] = gfxr_draw_cel11(id, loop, i, mirrored, resource_base, seeker, size, view);
		seeker += bytes_per_cel;
	}

	return result;
}

gfxr_view_t *
gfxr_draw_view11(int id, byte *resource, int size)
{
	gfxr_view_t *view;
 	int header_size = get_uint_16(resource + V2_HEADER_SIZE);
	int palette_offset = get_uint_16(resource + V2_PALETTE_OFFSET);
	int bytes_per_loop = resource[V2_BYTES_PER_LOOP];
	int loops_num = resource[V2_LOOPS_NUM];
	int bytes_per_cel = resource[V2_BYTES_PER_CEL];
	int i;
	byte *seeker;

	view = (gfxr_view_t*)sci_malloc(sizeof(gfxr_view_t));

	memset(view, 0, sizeof(gfxr_view_t));
	view->ID = id;
	view->flags = 0;

	view->loops_nr = loops_num;
	view->loops = (gfxr_loop_t *)calloc(view->loops_nr, sizeof(gfxr_loop_t));

	/* There is no indication of size here, but this is certainly large enough */
	view->colors = gfxr_read_pal11(id, &view->colors_nr, resource + palette_offset, 1284); 
 
	seeker = resource + header_size;
	for (i = 0; i < view->loops_nr; i++)
	{
		static char *truth[2] = {"not ",""};
		int loop_offset = get_uint_16(seeker + V2_LOOP_OFFSET);
		int cels = seeker[V2_CELS_NUM];
		int mirrored = seeker[V2_IS_MIRROR];
		int copy_entry = seeker[V2_COPY_OF_LOOP];

		printf("%d\n", mirrored);
		if (copy_entry == 255)
			gfxr_draw_loop11(id, i, 0, resource, resource + loop_offset, size, cels, view->loops + i, 
					 view, bytes_per_cel); else
					 {
						 byte *temp = resource + header_size + copy_entry * bytes_per_loop;
						 loop_offset = get_uint_16(temp + V2_LOOP_OFFSET);
						 cels = temp[V2_CELS_NUM];
						 gfxr_draw_loop11(id, i, 1, resource, resource + loop_offset, size, cels, 
								  view->loops + i, view, bytes_per_cel);
					 }
						 
		seeker += bytes_per_loop;
	}

	return view;
}
