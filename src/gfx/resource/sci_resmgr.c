/***************************************************************************
 sci_resmgr.c Copyright (C) 2000 Christoph Reichenbach

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
/* The interpreter-specific part of the resource manager, for SCI */

#include <sci_memory.h>
#include <sciresource.h>
#include <gfx_widgets.h>
#include <gfx_resmgr.h>
#include <gfx_options.h>
#ifdef HAVE_PICO
#include "psram_alloc.h"
#include <pico/stdlib.h>
/* Globals in operations.c consumed here during pic decode */
extern byte *g_pico_decode_priority_buf;
extern byte *g_pico_decode_visual_buf;
extern int g_pico_visual_defer_failed;
#endif

int
gfxr_interpreter_options_hash(gfx_resource_type_t type, int version,
			      gfx_options_t *options,
			      void *internal, int palette)
{
	switch (type) {

	case GFX_RESOURCE_TYPE_VIEW:
		return palette;

	case GFX_RESOURCE_TYPE_PIC:
		if (version >= SCI_VERSION_01_VGA)
			return options->pic_port_bounds.y;
		else
			return (options->pic0_unscaled)? 0x10000 :
				(options->pic0_dither_mode << 12)
				| (options->pic0_dither_pattern << 8)
				| (options->pic0_brush_mode << 4)
				| (options->pic0_line_mode);

	case GFX_RESOURCE_TYPE_FONT:
		return 0;

	case GFX_RESOURCE_TYPE_CURSOR:
		return 0;

	case GFX_RESOURCE_TYPES_NR:
	default:
		GFXERROR("Invalid resource type: %d\n", type);
		return -1;
	}
}


gfxr_pic_t *
gfxr_interpreter_init_pic(int version, gfx_mode_t *mode, int ID, void *internal)
{
	return gfxr_init_pic(mode, ID, version >= SCI_VERSION_01_VGA);
}


void
gfxr_interpreter_clear_pic(int version, gfxr_pic_t *pic, void *internal)
{
	gfxr_clear_pic0(pic, SCI_TITLEBAR_SIZE);
}


int
gfxr_interpreter_calculate_pic(gfx_resstate_t *state, gfxr_pic_t *scaled_pic, gfxr_pic_t *unscaled_pic,
			       int flags, int default_palette, int nr, void *internal)
{
	resource_mgr_t *resmgr = (resource_mgr_t *) state->misc_payload;
	resource_t *res = scir_find_resource(resmgr, sci_pic, nr, 0);
	int need_unscaled = unscaled_pic != NULL;
	gfxr_pic0_params_t style, basic_style;
	
	basic_style.line_mode = GFX_LINE_MODE_CORRECT;
	basic_style.brush_mode = GFX_BRUSH_MODE_SCALED;
	basic_style.pic_port_bounds = state->options->pic_port_bounds;
	
	style.line_mode = state->options->pic0_line_mode;
	style.brush_mode = state->options->pic0_brush_mode;
	style.pic_port_bounds = state->options->pic_port_bounds;

	if (!res || !res->data)
		return GFX_ERROR;

	if (state->version >= SCI_VERSION_01_VGA) {
		if (need_unscaled)
		{
			if (state->version == SCI_VERSION_1_1)
				gfxr_draw_pic11(unscaled_pic, flags, default_palette, res->size, res->data, &basic_style, res->id,
						state->static_palette, state->static_palette_entries); 
			else
				gfxr_draw_pic01(unscaled_pic, flags, default_palette, res->size, res->data, &basic_style, res->id, 1,
						state->static_palette, state->static_palette_entries);
		}
		if (scaled_pic && scaled_pic->undithered_buffer)
			memcpy(scaled_pic->visual_map->index_data, scaled_pic->undithered_buffer, scaled_pic->undithered_buffer_size);

		if (state->version == SCI_VERSION_1_1)
			gfxr_draw_pic11(scaled_pic, flags, default_palette, res->size, res->data, &style, res->id,
					state->static_palette, state->static_palette_entries);
		else
			gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, res->data, &style, res->id, state->version,
					state->static_palette, state->static_palette_entries);
	} else {
#ifdef HAVE_PICO
		extern void pico_picdec_cache_begin(uint32_t addr, int size);
		extern void pico_picdec_cache_end(void);

		/* Store pic data to PSRAM so gfxr_draw_pic01 can stream it via _RB/_RBS/_RU16 macros. */
		{
			uint32_t _ra = psram_alloc(res->size);
			psram_store(_ra, res->data, res->size);
			pico_picdec_cache_begin(_ra, res->size);
		}
		scir_evict_resource_data(resmgr, res);
		scir_free_all_lru(resmgr);

		/* Pass 1: visual map (64KB).  Two modes:
		   - Deferred (g_pico_decode_visual_buf NULL, the normal case): allocate
		     HERE, after the resource was decompressed and evicted to PSRAM above,
		     so the 64KB is NOT held during the decompress — frees 64KB at the
		     decompress0 OOM moment.
		   - Pinned (g_pico_decode_visual_buf set): the fallback retry in
		     gfxop_new_pic pre-reserved it from the pristine room-change region
		     because a deferred alloc just failed under fragmentation; consume it. */
		if (g_pico_decode_visual_buf) {
			scaled_pic->visual_map->index_data = g_pico_decode_visual_buf;
			g_pico_decode_visual_buf = NULL;
		} else {
			scaled_pic->visual_map->index_data = (byte*)malloc(GFXR_AUX_MAP_SIZE);
			if (!scaled_pic->visual_map->index_data) {
				/* Deferred alloc failed.  Signal gfxop_new_pic to retry with an
				   early pin from the freshly-freed region; if that also fails
				   it's a genuine out-of-64KB OOM. */
				g_pico_visual_defer_failed = 1;
				pico_picdec_cache_end(); return GFX_ERROR;
			}
		}

#ifdef PICO_DECODE_CONTROL_MAP
		/* Allocate the 32KB packed control buffer HERE — after the pic resource
		   was decompressed and evicted to PSRAM (lines above) so its decompress
		   buffer is already freed, and before Pass 1's draw churns the heap.  At
		   this exact point the decompress hole is back and the 64KB priority hole
		   is still pristine (priority is Pass 3), so a 32KB-contiguous block is
		   reliably available.  Earlier attempts failed at the two extremes: pinning
		   it back in gfxop_new_pic held 32KB through the resource decompress and
		   worsened the decompress0 OOM; allocating it mid-decode (Pass 2) hit
		   fragmentation and left no 32KB-contiguous block (disabling collision in
		   tight rooms like the trash elevator). */
		byte *control_buf = (byte*)malloc((GFXR_AUX_MAP_SIZE + 1) >> 1);
		if (!control_buf) {
			/* Allocation failed (very tight/fragmented heap).  Do NOT abort the
			   pic decode: gfxop_new_pic would return GFX_ERROR, which kDrawPic
			   escalates to a FATAL VM error -> HardFault.  Instead degrade to
			   no-collision for this pic — skip Pass 2 below, leaving
			   control_map->index_data NULL / psram_valid 0 so gfxop_scan_bitmask
			   returns 0, exactly like -DPICO_CONTROL_MAP=OFF but per-pic and
			   recoverable on the next decode with more free heap. */
			GFXWARN("control map: 32KB alloc failed for pic %d — decoding "
			        "without collision (low heap)\n", res->id);
		}
#endif
		gfxr_clear_pic0(scaled_pic, SCI_TITLEBAR_SIZE);

		gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, NULL,
				&style, res->id, 0,
				state->static_palette, state->static_palette_entries);

#ifdef PICO_DECODE_CONTROL_MAP
		byte *reuse_aux_buf = NULL;
#endif
		{	/* Push visual to PSRAM; pico_blit_indexed handles psram_valid==1 */
			gfx_pixmap_t *vmap = scaled_pic->visual_map;
			size_t sz = (size_t)(vmap->index_xl * vmap->index_yl);
			vmap->psram_addr  = psram_alloc(sz);
			vmap->psram_valid = 1;
			psram_store(vmap->psram_addr, vmap->index_data, sz);
#ifdef PICO_DECODE_CONTROL_MAP
			/* Reuse the 64KB visual buffer in place as the control pass's
			   aux_map instead of free()+malloc.  The free/realloc round-trip
			   fragments the heap: a fresh 32KB control alloc lands inside the
			   freed 64KB hole, leaving no 64KB-contiguous block for aux_map. */
			reuse_aux_buf = vmap->index_data;
#else
			free(vmap->index_data);
#endif
			vmap->index_data = NULL;
		}
#ifdef PICO_DECODE_CONTROL_MAP
		/* Pass 2: control map.
		   Needed for collision detection and control-line scripts — kCanBeHere
		   (kgraphics.c) scans pic->control_map via gfxop_scan_bitmask.  Without it
		   every scan returns 0: the ego walks through blocking polygons and control
		   triggers (e.g. SQ3's trash elevator) never fire.  Control FILLS flood-fill
		   the enclosed region through the aux_map (AUXBUF_FILL), so a temporary
		   aux_map must exist for this pass.  Both control index_data and aux_map are
		   freed before the priority pass so decode peak stays ~128KB, not ~192KB.

		   DISABLED by default: the extra ~128KB transient decode peak OOMs on some
		   rooms.  Re-enable with -DPICO_CONTROL_MAP=ON once the decode budget allows
		   (e.g. priority map also offloaded to PSRAM).  See CLAUDE.md open issues. */
		/* Nibble-pack the control map (2 px/byte, 32KB) so it + the 64KB aux_map
		   fit during the flood fill — two 64KB buffers can't coexist near the
		   ~388KB heap ceiling.  Skipped entirely if the control buffer OOM'd
		   above (control_buf NULL): degrade to no-collision, don't abort. */
		if (control_buf) {
			scaled_pic->aux_map = reuse_aux_buf; /* the 64KB visual buffer */
			/* Consume the control buffer pinned at the start of Pass 1;
			   gfxr_clear_pic0 zeroes it, so no calloc needed. */
			scaled_pic->control_map->index_data = control_buf;
			scaled_pic->control_map->nibble_packed = 1;

			gfxr_clear_pic0(scaled_pic, SCI_TITLEBAR_SIZE);

			gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, NULL,
					&style, res->id, 0,
					state->static_palette, state->static_palette_entries);

			{	/* Offload control to PSRAM; gfxop_scan_bitmask reads it back
				   row-by-row.  Stored nibble-packed, so half the pixel count. */
				gfx_pixmap_t *cmap = scaled_pic->control_map;
				size_t npix = (size_t)(cmap->index_xl * cmap->index_yl);
				size_t sz = (npix + 1) >> 1;
				cmap->psram_addr  = psram_alloc(sz);
				cmap->psram_valid = 1;
				psram_store(cmap->psram_addr, cmap->index_data, sz);
				free(cmap->index_data);
				cmap->index_data = NULL;
			}
			free(scaled_pic->aux_map);
			scaled_pic->aux_map = NULL;
		} else {
			/* Control OOM'd: free the 64KB visual buffer we earmarked as the
			   flood-fill aux_map; control_map->index_data stays NULL so
			   gfxop_scan_bitmask returns 0 (no collision) for this pic. */
			free(reuse_aux_buf);
			reuse_aux_buf = NULL;
		}
#endif /* PICO_DECODE_CONTROL_MAP */

		/* Pass 3: priority map */
		gfx_pixmap_alloc_index_data(scaled_pic->priority_map);
		if (!scaled_pic->priority_map->index_data) {
			pico_picdec_cache_end(); return GFX_ERROR;
		}
		gfxr_clear_pic0(scaled_pic, SCI_TITLEBAR_SIZE);

		gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, NULL,
				&style, res->id, 0,
				state->static_palette, state->static_palette_entries);
		pico_picdec_cache_end();

		/* Disown priority buffer — gfxop_new_pic assigns it to state->priority_map */
		g_pico_decode_priority_buf = scaled_pic->priority_map->index_data;
		scaled_pic->priority_map->index_data = NULL;

#else
		if (need_unscaled)
			gfxr_draw_pic01(unscaled_pic, flags, default_palette, res->size, res->data, &basic_style, res->id, 0,
					state->static_palette, state->static_palette_entries);

		if (scaled_pic && scaled_pic->undithered_buffer)
			memcpy(scaled_pic->visual_map->index_data, scaled_pic->undithered_buffer, scaled_pic->undithered_buffer_size);

		gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, res->data, &style, res->id, 0,
				state->static_palette, state->static_palette_entries);
		if (need_unscaled)
			gfxr_remove_artifacts_pic0(scaled_pic, unscaled_pic);

		if (!scaled_pic->undithered_buffer)
			scaled_pic->undithered_buffer = sci_malloc(scaled_pic->undithered_buffer_size);

		memcpy(scaled_pic->undithered_buffer, scaled_pic->visual_map->index_data, scaled_pic->undithered_buffer_size);

		gfxr_dither_pic0(scaled_pic, state->options->pic0_dither_mode, state->options->pic0_dither_pattern);
#endif
	}

	/* Mark default palettes */
	if (scaled_pic)
		scaled_pic->visual_map->loop = default_palette;

	if (unscaled_pic)
		unscaled_pic->visual_map->loop = default_palette;

	return GFX_OK;
}


void
gfxr_palettize_view(gfxr_view_t *view, gfx_pixmap_color_t *source, int source_entries)
{
    int i;
    
    for (i=0;i<MIN(view->colors_nr,source_entries);i++)
    {
	if ((view->colors[i].r == 0) &&
	    (view->colors[i].g == 0) &&
	    (view->colors[i].b == 0))
	{
	    view->colors[i] = source[i];
	}
    }
}

gfxr_view_t *
gfxr_draw_view11(int id, byte *resource, int size);

gfxr_view_t *
gfxr_interpreter_get_view(gfx_resstate_t *state, int nr, void *internal, int palette)
{
	resource_mgr_t *resmgr = (resource_mgr_t *) state->misc_payload;
	resource_t *res = scir_find_resource(resmgr, sci_view, nr, 0);
	int resid = GFXR_RES_ID(GFX_RESOURCE_TYPE_VIEW, nr);
	gfxr_view_t *result;

	if (!res || !res->data)
		return NULL;

	if (state->version < SCI_VERSION_01) palette=-1;

	switch (state->version)
	{
	case SCI_VERSION_0:
	case SCI_VERSION_01:
		result=gfxr_draw_view0(resid, res->data, res->size, palette);
		break;
	case SCI_VERSION_01_VGA:
	case SCI_VERSION_01_VGA_ODD:
	case SCI_VERSION_1_EARLY:
	case SCI_VERSION_1_LATE:
		result=gfxr_draw_view1(resid, res->data, res->size, state->static_palette, state->static_palette_entries); 
		break;
	case SCI_VERSION_1_1:
	case SCI_VERSION_32:
		result=gfxr_draw_view11(resid, res->data, res->size); 
		break;
	}

	if (state->version >= SCI_VERSION_01_VGA)
	{
		    if (!result->colors)
		    {
			result->colors = (gfx_pixmap_color_t*)sci_malloc(sizeof(gfx_pixmap_color_t) * state->static_palette_entries);
			memset(result->colors, 0, sizeof(gfx_pixmap_color_t) * state->static_palette_entries);
			result->colors_nr = state->static_palette_entries;
		    }
		    gfxr_palettize_view(result, state->static_palette, state->static_palette_entries);
	}

#ifdef HAVE_PICO
	/* Offload all cel index_data to PSRAM; pico_blit_indexed reads it back
	   row-by-row via psram_load.  Evict the raw resource data immediately
	   so the freed 64KB block stays available for visual[0] lazy allocation. */
	if (result) {
		int l;
		for (l = 0; l < result->loops_nr; l++) {
			gfxr_loop_t *loop = &result->loops[l];
			int c;
			for (c = 0; c < loop->cels_nr; c++) {
				gfx_pixmap_t *cel = loop->cels[c];
				if (cel && cel->index_data) {
					size_t sz = (size_t)(cel->index_xl * cel->index_yl);
					cel->psram_addr  = psram_alloc(sz);
					cel->psram_valid = 1;
					psram_store(cel->psram_addr, cel->index_data, sz);
					free(cel->index_data);
					cel->index_data = NULL;
				}
			}
		}
		scir_evict_resource_data(resmgr, res);
	}
#endif

	return result;
}


gfx_bitmap_font_t *
gfxr_interpreter_get_font(gfx_resstate_t *state, int nr, void *internal)
{
	resource_mgr_t *resmgr = (resource_mgr_t *) state->misc_payload;
	resource_t *res = scir_find_resource(resmgr, sci_font, nr, 0);
	if (!res || !res->data)
		return NULL;

	return gfxr_read_font(res->id, res->data, res->size);
}


gfx_pixmap_t *
gfxr_interpreter_get_cursor(gfx_resstate_t *state, int nr, void *internal)
{
	resource_mgr_t *resmgr = (resource_mgr_t *) state->misc_payload;
	resource_t *res = scir_find_resource(resmgr, sci_cursor, nr, 0);
	int resid = GFXR_RES_ID(GFX_RESOURCE_TYPE_CURSOR, nr);

	if (!res || !res->data)
		return NULL;

	if (state->version >= SCI_VERSION_1_1) {
		GFXWARN("Attempt to retreive cursor in SCI1.1 or later\n");
		return NULL;
	}

	if (state->version == SCI_VERSION_0)
		return gfxr_draw_cursor0(resid, res->data, res->size);
	else
		return gfxr_draw_cursor01(resid, res->data, res->size);
}


int *
gfxr_interpreter_get_resources(gfx_resstate_t *state, gfx_resource_type_t type,
			       int version, int *entries_nr, void *internal)
{
	resource_mgr_t *resmgr = (resource_mgr_t *) state->misc_payload;
	int restype;
	int *resources;
	int count = 0;
	int top = sci_max_resource_nr[version] + 1;
	int i;
	switch (type) {

	case GFX_RESOURCE_TYPE_VIEW: restype = sci_view;
		break;

	case GFX_RESOURCE_TYPE_PIC: restype = sci_pic;
		break;

	case GFX_RESOURCE_TYPE_CURSOR: restype = sci_cursor;
		break;

	case GFX_RESOURCE_TYPE_FONT: restype = sci_font;
		break;

	default:
		GFX_DEBUG("Unsupported resource %d\n", type);
		return NULL; /* unsupported resource */

	}

	resources = (int*)sci_malloc(sizeof(int) * top);

	for (i = 0; i < top; i++)
		if (scir_test_resource(resmgr, restype, i))
			resources[count++] = i;

	*entries_nr = count;

	return resources;
}

gfx_pixmap_color_t *
gfxr_interpreter_get_static_palette(gfx_resstate_t *state, int version, int *colors_nr, void *internal)
{
	if (version >= SCI_VERSION_01_VGA)
		return gfxr_interpreter_get_palette(state, version, colors_nr, internal, 999);

	*colors_nr = GFX_SCI0_PIC_COLORS_NR;
	return gfx_sci0_pic_colors;
}

gfx_pixmap_color_t *
gfxr_interpreter_get_palette(gfx_resstate_t *state, int version, int *colors_nr, 
			     void *internal, int nr)
{
	resource_mgr_t *resmgr = (resource_mgr_t *) state->misc_payload;
	resource_t *res;

	if (version < SCI_VERSION_01_VGA)
		return NULL;

	res = scir_find_resource(resmgr, sci_palette, nr, 0);
	if (!res || !res->data)
		return NULL;

	switch (version)
	{
	case SCI_VERSION_01_VGA :
	case SCI_VERSION_01_VGA_ODD :
	case SCI_VERSION_1_EARLY :
	case SCI_VERSION_1_LATE :
		return gfxr_read_pal1(res->id, colors_nr, res->data, res->size);
	case SCI_VERSION_1_1 :
	case SCI_VERSION_32 :
		GFX_DEBUG("Palettes are not yet supported in this SCI version\n");
		return NULL;

	default:
		BREAKPOINT();
		return NULL;
	}
}

int
gfxr_interpreter_needs_multicolored_pointers(int version, void *internal)
{
	return (version > SCI_VERSION_1);
}



