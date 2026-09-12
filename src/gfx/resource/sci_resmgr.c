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
extern byte *g_pico_decode_visual_buf;

/* The visual map is nibble-packed (2 px/byte) when the driver's visual[0] is --
   see PICO_PACK_VISUAL in pico_driver.c. GFXR_VIS_BYTES is the ONE place that
   knows the decode buffer's byte extent, so the deferred alloc, the PSRAM
   offload and the overlay base-restore cannot drift apart. */
#ifdef PICO_PACK_VISUAL
#  define GFXR_VIS_PACKED    1
#  define GFXR_VIS_BYTES(n)  (((n) + 1) >> 1)
#else
#  define GFXR_VIS_PACKED    0
#  define GFXR_VIS_BYTES(n)  (n)
#endif
extern byte *g_pico_priority_scratch;
extern int g_pico_visual_defer_failed;
extern int g_pico_decode_visual_borrowed;
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
#ifdef PICO_DITHER_D16
				(GFXR_DITHER_MODE_D16 << 12)
#else
				(options->pic0_dither_mode << 12)
#endif
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

		/* Allocate the visual buffer (64KB).  Two modes:
		   - Deferred (g_pico_decode_visual_buf NULL, the normal case): allocate
		     HERE, after the resource was decompressed and evicted to PSRAM above,
		     so the 64KB is NOT held during the decompress — frees 64KB at the
		     decompress0 OOM moment.
		   - Pinned (g_pico_decode_visual_buf set): the fallback retry in
		     gfxop_new_pic pre-reserved it from the pristine room-change region
		     because a deferred alloc just failed under fragmentation; consume it. */
		int visual_pinned = (g_pico_decode_visual_buf != NULL);
		/* When set, the pinned visual buffer is the driver's resident display
		   buffer (visual[0]), NOT a throwaway decode buffer.  Every free of it
		   below must be skipped — it is owned by the driver and repainted after
		   the decode by pico_render_background. */
		int visual_borrowed = g_pico_decode_visual_borrowed;

		/* OVERLAY (overlay: selector → add_to_pic): this pic is composited onto
		   the cached base pic, whose visual+priority were offloaded to PSRAM
		   (index_data NULL) by its own earlier decode.  Desktop preserves the
		   base by copying undithered_buffer back before drawing the overlay
		   commands (see the #else branch); the Pico analogue is to load the base
		   maps back from PSRAM into the fresh decode buffers and SKIP the white
		   clear, so untouched areas keep the base (logo/starfield) instead of
		   becoming gfxr_clear_pic0's 0xff fill.  psram_valid/psram_addr survive
		   here because the PSRAM bump arena is only rewound on a fresh drawPic
		   (gfxop_new_pic → psram_reset), never on add_to_pic; and the fields are
		   not overwritten until the re-offload below (after the clear point). */
		int restore_base = (flags & DRAWPIC01_FLAG_OVERLAID_PIC)
			&& scaled_pic->visual_map->psram_valid
			&& scaled_pic->priority_map->psram_valid;
		uint32_t base_vis_addr = scaled_pic->visual_map->psram_addr;
		uint32_t base_pri_addr = scaled_pic->priority_map->psram_addr;

		if (g_pico_decode_visual_buf) {
			scaled_pic->visual_map->index_data = g_pico_decode_visual_buf;
			/* This IS the driver's visual[0], so it carries the driver's
			   format; every writer keys off nibble_packed. */
			scaled_pic->visual_map->nibble_packed = GFXR_VIS_PACKED;
			g_pico_decode_visual_buf = NULL;
		} else {
			scaled_pic->visual_map->index_data =
				(byte*)malloc(GFXR_VIS_BYTES(GFXR_AUX_MAP_SIZE));
			PICO_ARENA_PROBE_RAW(GFXR_VIS_BYTES(GFXR_AUX_MAP_SIZE));
			scaled_pic->visual_map->nibble_packed = GFXR_VIS_PACKED;
			if (!scaled_pic->visual_map->index_data) {
				/* Deferred alloc failed.  Signal gfxop_new_pic to retry with an
				   early pin from the freshly-freed region; if that also fails
				   it's a genuine out-of-64KB OOM. */
				g_pico_visual_defer_failed = 1;
				pico_picdec_cache_end(); return GFX_ERROR;
			}
		}

		/* Allocate the priority buffer NIBBLE-PACKED (32KB, 2 px/byte) so it can
		   be decoded in the SAME pass as the 64KB visual map without exceeding the
		   ~96KB decode budget (visual 64KB + priority 32KB).  Merging the two
		   passes is what fixes z-layering: a standalone priority pass ran with
		   visual_map->index_data NULL, so priority FILL ops were bounded by the
		   sparse priority map itself rather than the visual outlines and
		   over-spread.  With both maps live the flood fill uses the visual map as
		   its boundary (sci_picfill.c), giving correct priority-region edges. */
		/* Priority (32KB nibble-packed).  Use the permanent scratch (B-1):
		   allocated once from the pristine boot heap, reused every decode, never
		   freed — so the priority alloc can't be denied by fragmentation mid-game
		   or post-restore, and the restore path no longer pre-reserves 32KB.  Fall
		   back to a per-decode malloc only if the scratch was somehow not taken. */
		int priority_is_scratch = 0;
		if (g_pico_priority_scratch) {
			scaled_pic->priority_map->index_data = g_pico_priority_scratch;
			priority_is_scratch = 1;
		} else {
			scaled_pic->priority_map->index_data = (byte*)malloc((GFXR_AUX_MAP_SIZE + 1) >> 1);
			PICO_ARENA_PROBE_RAW((GFXR_AUX_MAP_SIZE + 1) >> 1);
		}
		if (!scaled_pic->priority_map->index_data) {
			/* Priority (32KB nibble) couldn't find a contiguous block after the
			   visual (64KB) took its share — a fragmentation OOM, not a true
			   out-of-memory.  Free the visual and, when it was the *deferred*
			   alloc (not an already-pinned retry), signal gfxop_new_pic to retry
			   with BOTH maps early-pinned from the freshly re-coalesced room-
			   change region.  If the visual was already pinned, this is a genuine
			   OOM — fall through to GFX_ERROR. */
			if (!visual_borrowed)
				free(scaled_pic->visual_map->index_data);
			/* Detach either way: if borrowed, visual[0] is owned by the driver
			   and must survive; NULLing prevents the half-built-pic teardown in
			   gfxr_get_pic from double-freeing the display buffer. */
			scaled_pic->visual_map->index_data = NULL;
			if (!visual_pinned)
				g_pico_visual_defer_failed = 1;
			pico_picdec_cache_end(); return GFX_ERROR;
		}
		scaled_pic->priority_map->nibble_packed = 1;

		if (restore_base) {
			/* Restore the base pic from PSRAM into the fresh buffers instead of
			   clearing to white; the overlay's own commands then draw on top. */
			gfx_pixmap_t *vmap = scaled_pic->visual_map;
			gfx_pixmap_t *pmap = scaled_pic->priority_map;
			size_t vsz = GFXR_VIS_BYTES((size_t)(vmap->index_xl * vmap->index_yl));
			size_t pnpix = (size_t)(pmap->index_xl * pmap->index_yl);
			psram_load(base_vis_addr, vmap->index_data, vsz);
			psram_load(base_pri_addr, pmap->index_data, (pnpix + 1) >> 1);
		} else {
			gfxr_clear_pic0(scaled_pic, SCI_TITLEBAR_SIZE);
		}

#ifdef FSCI_PROBE_GFX
		/* [ovl] overlay-decode probe: sum the visual buffer right after the base
		   restore/white-clear and again after the overlay's own draw, so a capture
		   shows whether restore_base engaged + from what PSRAM addr, what the base
		   looked like (sum_before ~= 0xff*N => white clear, varied => real logo,
		   odd => stale/wrong addr) and whether the overlay actually drew (delta!=0). */
		unsigned long _ovl_before = 0;
		if (flags & DRAWPIC01_FLAG_OVERLAID_PIC) {
			gfx_pixmap_t *_vm = scaled_pic->visual_map;
			size_t _n = GFXR_VIS_BYTES((size_t)(_vm->index_xl * _vm->index_yl)), _i;
			for (_i = 0; _i < _n; _i++) _ovl_before += _vm->index_data[_i];
		}
#endif

		/* Merged pass: draw visual + priority together.  control_map->index_data
		   is still NULL (control gets its own pass below), so control draws no-op
		   via the index_data / NULL-buffer guards in the draw helpers. */
		gfxr_draw_pic01(scaled_pic, flags, default_palette, res->size, NULL,
				&style, res->id, 0,
				state->static_palette, state->static_palette_entries);

#ifdef FSCI_PROBE_GFX
		if (flags & DRAWPIC01_FLAG_OVERLAID_PIC) {
			gfx_pixmap_t *_vm = scaled_pic->visual_map;
			size_t _n = GFXR_VIS_BYTES((size_t)(_vm->index_xl * _vm->index_yl)), _i;
			unsigned long _ovl_after = 0;
			for (_i = 0; _i < _n; _i++) _ovl_after += _vm->index_data[_i];
			sciprintf("[ovl] id=%d restore_base=%d vaddr=%lu N=%lu "
				  "sum_before=%lu sum_after=%lu delta=%ld\n",
				  res->id, restore_base, (unsigned long)base_vis_addr,
				  (unsigned long)_n, _ovl_before, _ovl_after,
				  (long)(_ovl_after - _ovl_before));
		}
#endif

		byte *reuse_aux_buf = NULL;
		{	/* Push visual to PSRAM; pico_blit_indexed handles psram_valid==1.
			   Keep the 64KB buffer (don't free yet) to reuse as the control
			   pass's flood-fill aux_map below. */
			gfx_pixmap_t *vmap = scaled_pic->visual_map;
			size_t sz = GFXR_VIS_BYTES((size_t)(vmap->index_xl * vmap->index_yl));
			vmap->psram_addr  = psram_alloc(sz);
			vmap->psram_valid = 1;
			psram_store(vmap->psram_addr, vmap->index_data, sz);
#ifdef PICO_PACK_VISUAL
			/* DESIGN COLLISION: the control pass borrows this buffer as its
			   flood-fill aux_map, which is GFXR_AUX_MAP_SIZE = 320*200 bytes,
			   ONE BYTE PER PIXEL -- it carries flag bits 0x40 (FRESH_PAINT) and
			   0x10 alongside the colour nibble, so it needs >=7 bits per pixel
			   and canNOT be packed. A packed visual[0] is only half that, so
			   reusing it here overran the buffer by 32,000 bytes: heap
			   corruption, and the device HardFaulted leaving the PQ2 car.
			   Allocate a real aux instead, and let the cost be visible rather
			   than hidden -- see CLAUDE.md, this materially changes the 4bpp
			   value proposition. */
			if (visual_borrowed)
				reuse_aux_buf = NULL;       /* visual[0] is the driver's; leave it */
			else
				free(vmap->index_data);
			vmap->index_data = NULL;
#else
			reuse_aux_buf = vmap->index_data;
			vmap->index_data = NULL;
#endif
		}

		{	/* Push priority (nibble-packed) to PSRAM; gfxop_scan_bitmask and
			   pico_blit_indexed read it back row-by-row, unpacking nibbles.  Stash
			   the metadata on pic->priority_map so gfxop_new_pic can copy it onto
			   state->priority_map.  index_data stays allocated for now — it is
			   reused as the control buffer (same 32KB packed size) below. */
			gfx_pixmap_t *pmap = scaled_pic->priority_map;
			size_t npix = (size_t)(pmap->index_xl * pmap->index_yl);
			size_t sz = (npix + 1) >> 1;
			pmap->psram_addr  = psram_alloc(sz);
			pmap->psram_valid = 1;
			psram_store(pmap->psram_addr, pmap->index_data, sz);
		}

		/* [pcol] probe: dump the decoded priority column x=82, rows 35-120,
		   unpacking nibbles from the still-resident packed SRAM buffer.  Mirrors
		   the desktop [dpcol] line so the two priority maps can be diffed
		   value-for-value.  Strip with the other Pico probes once z-layering
		   is resolved. */
#ifdef FSCI_PROBE_GFX
		{
			gfx_pixmap_t *pmap = scaled_pic->priority_map;
			if (pmap->index_data) {
				int _y, _xl = pmap->index_xl, _n = 0;
				char _buf[512];
				_n += snprintf(_buf + _n, sizeof(_buf) - _n,
					       "[pcol] x=82 rows35-120 pri:");
				for (_y = 35; _y <= 120 && _n < (int)sizeof(_buf) - 8; _y++) {
					int _p = _y * _xl + 82;
					byte _b = pmap->index_data[_p >> 1];
					int _v = (_p & 1) ? (_b >> 4) : (_b & 0x0f);
					_n += snprintf(_buf + _n, sizeof(_buf) - _n, " %d", _v);
				}
				sciprintf("%s\n", _buf);
			}
		}
#endif /* FSCI_PROBE_GFX */

#ifdef PICO_DECODE_CONTROL_MAP
		/* Pass 2: control map.
		   Needed for collision detection and control-line scripts — kCanBeHere
		   (kgraphics.c) scans pic->control_map via gfxop_scan_bitmask.  Without it
		   every scan returns 0: the ego walks through blocking polygons and control
		   triggers (e.g. SQ3's trash elevator) never fire.  Control FILLS flood-fill
		   the enclosed region through the aux_map (AUXBUF_FILL), so a temporary
		   aux_map must exist for this pass.

		   Reuse the priority 32KB packed buffer (already offloaded to PSRAM above)
		   directly as the control buffer — identical size, so no free/malloc
		   round-trip (which fragments the heap).  Reuse the 64KB visual buffer as
		   the flood-fill aux_map.  Peak here is 64KB (aux) + 32KB (control) = 96KB,
		   same as the merged pass — within the known budget. */
		{
			byte *control_buf = scaled_pic->priority_map->index_data;
			scaled_pic->priority_map->index_data = NULL;

#ifdef PICO_PACK_VISUAL
			/* A packed visual[0] cannot serve as the aux (see above), so the
			   control pass needs its own 64KB transient -- exactly the kind of
			   large contiguous allocation this port spent months eliminating. */
			if (!reuse_aux_buf)
				reuse_aux_buf = (byte *)malloc(GFXR_AUX_MAP_SIZE);
			if (!reuse_aux_buf) {
				GFXWARN("aux_map: 64KB alloc failed - decoding without collision\n");
				/* MUST honour priority_is_scratch, exactly like the other two
				   free sites below: control_buf IS the permanent B-1 priority
				   scratch in that case, so freeing it is a use-after-free for
				   the rest of the session -- observed as heap corruption with
				   rotating victims (a widget widfree pointer, then a script
				   hashmap node). Omitting this guard was the bug. */
				if (!priority_is_scratch)
					free(control_buf);
				control_buf = NULL;
			}
#endif
			scaled_pic->aux_map = reuse_aux_buf;            /* the 64KB visual buffer */
			scaled_pic->control_map->index_data = control_buf; /* 32KB packed */
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
				/* Probe: count non-background control nibbles the decode produced,
				   so a blank map (e.g. after restore) is distinguishable from a
				   stale/wrong-address read on the scan side ([oc] probe). */
#ifdef FSCI_PROBE_GFX
				{
					size_t _i, _nz = 0;
					for (_i = 0; _i < sz; _i++) {
						uint8_t _b = cmap->index_data[_i];
						if (_b & 0x0f) _nz++;
						if (_b & 0xf0) _nz++;
					}
					sciprintf("[ctl] pic id=%d decoded nonzero=%u/%u addr=%lu\n",
						  (int)res->id, (unsigned)_nz, (unsigned)npix,
						  (unsigned long)cmap->psram_addr);
				}
#endif /* FSCI_PROBE_GFX */
				/* The reused 32KB buffer is the permanent priority scratch (B-1)
				   when priority_is_scratch — never free it, just detach. */
				if (!priority_is_scratch)
					free(cmap->index_data); /* frees the reused 32KB priority buffer */
				cmap->index_data = NULL;
			}
			/* The 64KB aux_map IS the borrowed visual[0] when reuse is active —
			   skip the free (driver owns it; pico_render_background repaints it). */
			if (!visual_borrowed)
				free(scaled_pic->aux_map); /* frees the 64KB visual buffer */
			scaled_pic->aux_map = NULL;
		}
		pico_picdec_cache_end();
#else
		/* No control pass: free the priority 32KB buffer (already offloaded to
		   PSRAM) and the 64KB visual buffer (held as reuse_aux_buf).  The priority
		   buffer is the permanent scratch (B-1) when priority_is_scratch — never
		   free it, just detach. */
		if (!priority_is_scratch)
			free(scaled_pic->priority_map->index_data);
		scaled_pic->priority_map->index_data = NULL;
		/* reuse_aux_buf IS the borrowed visual[0] when reuse is active — skip the
		   free (driver owns it; pico_render_background repaints it). */
		if (!visual_borrowed)
			free(reuse_aux_buf);
		reuse_aux_buf = NULL;
		pico_picdec_cache_end();
#endif /* PICO_DECODE_CONTROL_MAP */

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

#ifdef PICO_DITHER_D16
		/* Forced, not defaulted -- see PICO_DITHER_D16 in CMakeLists.txt.
		   The cache key below folds pic0_dither_mode in, so it is overridden
		   there too and the two cannot disagree. */
		gfxr_dither_pic0(scaled_pic, GFXR_DITHER_MODE_D16, state->options->pic0_dither_pattern);
#else
		gfxr_dither_pic0(scaled_pic, state->options->pic0_dither_mode, state->options->pic0_dither_pattern);
#endif
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



