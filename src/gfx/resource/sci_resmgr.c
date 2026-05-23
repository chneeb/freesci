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
#include <malloc.h>
#include <stdio.h>
#include <pico/stdlib.h>
/* Globals in operations.c consumed here during pic decode */
extern byte *g_pico_decode_priority_buf;
extern byte *g_pico_decode_visual_buf;
#define PICO_MEMPRINT(tag) do { struct mallinfo _mi = mallinfo(); \
    printf("[resmgr] " tag ": free=%d arena=%d\n", _mi.fordblks, _mi.arena); \
    stdio_flush(); } while(0)
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

#ifdef HAVE_PICO
	PICO_MEMPRINT("after res load");
#endif

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
		PICO_MEMPRINT("after pic evict");
		scir_free_all_lru(resmgr);
		PICO_MEMPRINT("after lru flush");

		/* Pass 1: visual map.
		   Use the buffer pre-reserved in gfxop_new_pic to avoid a malloc
		   that would fail due to fragmentation of the freed visual[0] block. */
		PICO_MEMPRINT("before pass1 visual");
		scaled_pic->visual_map->index_data = g_pico_decode_visual_buf;
		g_pico_decode_visual_buf = NULL;
		if (!scaled_pic->visual_map->index_data) {
			pico_picdec_cache_end(); return GFX_ERROR;
		}
		gfxr_clear_pic0(scaled_pic, SCI_TITLEBAR_SIZE);

		PICO_MEMPRINT("before pass1 draw");
		gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, NULL,
				&style, res->id, 0,
				state->static_palette, state->static_palette_entries);

		{	/* Push visual to PSRAM; pico_blit_indexed handles psram_valid==1 */
			gfx_pixmap_t *vmap = scaled_pic->visual_map;
			size_t sz = (size_t)(vmap->index_xl * vmap->index_yl);
			vmap->psram_addr  = psram_alloc(sz);
			vmap->psram_valid = 1;
			psram_store(vmap->psram_addr, vmap->index_data, sz);
			free(vmap->index_data);
			vmap->index_data = NULL;
		}
		/* Pass 2: priority map */
		PICO_MEMPRINT("before pass2 priority");
		gfx_pixmap_alloc_index_data(scaled_pic->priority_map);
		if (!scaled_pic->priority_map->index_data) {
			pico_picdec_cache_end(); return GFX_ERROR;
		}
		gfxr_clear_pic0(scaled_pic, SCI_TITLEBAR_SIZE);

		PICO_MEMPRINT("before pass2 draw");
		gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, NULL,
				&style, res->id, 0,
				state->static_palette, state->static_palette_entries);
		pico_picdec_cache_end();

		/* Disown priority buffer — gfxop_new_pic assigns it to state->priority_map */
		g_pico_decode_priority_buf = scaled_pic->priority_map->index_data;
		scaled_pic->priority_map->index_data = NULL;
		PICO_MEMPRINT("after pass2 done");

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
		int l, total_freed = 0;
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
					total_freed += (int)sz;
				}
			}
		}
		scir_evict_resource_data(resmgr, res);
		{ struct mallinfo _mi = mallinfo();
		  printf("[view] nr=%d cels_freed=%d res_freed=%d free=%d\n",
		         nr, total_freed, res ? (int)res->size : 0, _mi.fordblks);
		  stdio_flush(); }
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



