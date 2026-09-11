/* visdiff -- decode a pic's VISUAL map two ways and diff every pixel.
   The safety net for the 4bpp (D16 + nibble-packed visual buffer) work.

   Sibling of diff_main.c, which does the same for the CONTROL map and is what
   localised the KQ4 "Rosella swims on the lawn" bug entirely offline.

   WHY THIS EXISTS FIRST, before a single writer is converted: packing the visual
   buffer means teaching nibble writes to gfx_draw_box_pixmap_i (2 sites),
   gfx_draw_line_pixmap_i, _gfxr_auxplot_brush (2 sites), _gfxr_fill_ellipse, the
   flood-fill core, the memset clears, the dither pass and the artifact-removal
   copy -- plus sci_view_0.c's cel RLE and ~10 driver sites. The 16-bit LCD
   attempt failed for exactly one reason: one writer out of many was converted
   and the rest still wrote the old format, so the picture sheared. A per-pixel
   differ turns "did I miss a writer?" from a device question into an offline
   one, and can be re-run after every single conversion.

   Both sides are dithered to D16 first, because a packed buffer only makes sense
   with one index per pixel (see PICO_DITHER_D16, and tests/d16check.c which
   proves D16 output never exceeds 15).

   Until the packing actually lands this reports 0 diffs, which is the harness
   validating itself: the two copies of sci_pic_0.c agree when configured the
   same way. It reads nibble_packed off the pixmap, so it keeps working
   unchanged once the Pico copy starts producing packed output.

     cd tests/picodiff
     cp ../../src/gfx/resource/sci_pic_0.c pico_pic.c
     gcc -c -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 -DHAVE_PICO=1 \
         -DPICO_DITHER_D16=1 -DPICO_PACK_VISUAL=1 -include picoprefix.h \
         -I. -I../../src/gfx/resource -I../../src/include -I../../build \
         -I/usr/include/SDL2 -D_REENTRANT -o pico_pic.o pico_pic.c
     LIBS=$(find ../../build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
         -I../../src/include -I../../build -I/usr/include/SDL2 -D_REENTRANT \
         -o visdiff visdiff_main.c pico_pic.o psram_stub.c \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     ./visdiff /path/to/game            # every pic in the game
     ./visdiff /path/to/game 2 3 25     # just these
*/
#include <sciresource.h>
#include <gfx_resource.h>
#include <gfx_tools.h>
#include <sci_memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

gfxr_pic_t *pico_gfxr_init_pic(gfx_mode_t *, int, int);
void pico_gfxr_clear_pic0(gfxr_pic_t *, int);
void pico_gfxr_draw_pic01(gfxr_pic_t *, int, int, int, byte *, gfxr_pic0_params_t *,
                          int, int, gfx_pixmap_color_t *, int);
void pico_gfxr_dither_pic0(gfxr_pic_t *, int, int);
void pico_gfxr_init_static_palette(void);

static resource_mgr_t *resmgr;
#define W 320
#define H 200

/* Read pixel i out of a visual map that may or may not be nibble-packed, so the
   comparison is format-agnostic and survives the conversion it is checking. */
static int
pixel_at(gfx_pixmap_t *pxm, int i)
{
	if (!pxm->nibble_packed)
		return pxm->index_data[i];
	return (i & 1) ? (pxm->index_data[i >> 1] >> 4)
		       : (pxm->index_data[i >> 1] & 0x0f);
}

static int
run(gfx_mode_t *mode, int picnum, int verbose)
{
	resource_t *res = scir_find_resource(resmgr, sci_pic, picnum, 0);
	gfxr_pic0_params_t style;
	gfxr_pic_t *d, *p;
	int i, n, diffs = 0, first = -1;

	if (!res || !res->data)
		return 0;

	style.line_mode       = GFX_LINE_MODE_CORRECT;
	style.brush_mode      = GFX_BRUSH_MODE_SCALED;
	style.pic_port_bounds = gfx_rect(0, 10, 320, 190);

	/* --- desktop reference: byte per pixel --- */
	d = gfxr_init_pic(mode, res->id, 0);
	gfxr_clear_pic0(d, 10);
	gfxr_draw_pic01(d, 1, 0, res->size, res->data, &style, res->id, 0, NULL, 0);
	gfxr_dither_pic0(d, GFXR_DITHER_MODE_D16, GFXR_DITHER_PATTERN_1);

	/* --- Pico copy: packed once the writers are converted ---
	   Under HAVE_PICO the decoder DEFERS every index_data allocation to its
	   caller (sci_pic_0.c: "Defer all 64KB index_data allocations to
	   gfxr_interpreter_calculate_pic"), so the harness supplies them, exactly
	   as diff_main.c does for the control map. Allocating the full W*H is
	   correct for both formats -- a packed map simply uses the first half. */
	pico_gfxr_init_static_palette();
	p = pico_gfxr_init_pic(mode, res->id, 0);
	/* PACK=1 turns the Pico side's visual map into a nibble-packed buffer
	   BEFORE the writers understand it. The diff count is then a direct
	   measure of how much of phase 2 is left: it starts near 100% and must
	   reach 0 when every writer has been converted. */
	if (getenv("PACK"))
		p->visual_map->nibble_packed = 1;
	p->visual_map->index_data   = calloc(W * H, 1);
	p->priority_map->index_data = calloc(W * H, 1);
	p->aux_map                  = calloc(W * H, 1);
	pico_gfxr_clear_pic0(p, 10);
	pico_gfxr_draw_pic01(p, 1, 0, res->size, res->data, &style, res->id, 0, NULL, 0);
	pico_gfxr_dither_pic0(p, GFXR_DITHER_MODE_D16, GFXR_DITHER_PATTERN_1);

	n = d->visual_map->index_xl * d->visual_map->index_yl;
	{
		/* Classify each mismatch. "neighbour" means pico's value at i equals
		   desktop's at i^1 -- the OTHER pixel sharing the same packed byte --
		   which is an indexing/parity slip in a writer, NOT a wrong D16
		   selection. Distinguishing the two decides where to look. */
		int neigh = 0;
		for (i = 0; i < n; i++) {
			int a = d->visual_map->index_data[i];
			int b = pixel_at(p->visual_map, i);

			if (a != b) {
				if (first < 0) first = i;
				diffs++;
				if (d->visual_map->index_data[i ^ 1] == b)
					neigh++;
			}
		}
		if (diffs && getenv("CLASSIFY")) {
			int oddx = 0, oddy = 0, j;
			for (j = 0; j < n; j++)
				if (d->visual_map->index_data[j] != pixel_at(p->visual_map, j)) {
					if ((j % W) & 1) oddx++;
					if ((j / W) & 1) oddy++;
				}
			if (getenv("COORDS")) {
				int shown = 0;
				for (j = 0; j < n && shown < 24; j++)
					if (d->visual_map->index_data[j] != pixel_at(p->visual_map, j)) {
						printf("             (%3d,%3d) d=%2d p=%2d\n",
						       j % W, j / W,
						       d->visual_map->index_data[j],
						       pixel_at(p->visual_map, j));
						shown++;
					}
			}
			printf("           %d diffs: %d neighbour-match (%.0f%%), "
			       "odd-x %d (%.0f%%), odd-y %d (%.0f%%)\n",
			       diffs, neigh, 100.0 * neigh / diffs,
			       oddx, 100.0 * oddx / diffs, oddy, 100.0 * oddy / diffs);
		}
	}

	if (diffs || verbose)
		printf("  pic %-4d %s  %d/%d differ\n", picnum,
		       p->visual_map->nibble_packed ? "packed  " : "unpacked",
		       diffs, n);
	if (first >= 0)
		printf("           first at (%d,%d): desktop %d vs pico %d\n",
		       first % W, first / W,
		       d->visual_map->index_data[first], pixel_at(p->visual_map, first));

	gfxr_free_pic(NULL, d);
	free(p->visual_map->index_data);
	free(p->priority_map->index_data);
	free(p->aux_map);
	return diffs ? 1 : 0;
}

int
main(int argc, char **argv)
{
	gfx_mode_t *mode;
	int i, bad = 0, checked = 0;
	int verbose = getenv("VERBOSE") != NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: %s <gamedir> [picnum...]\n", argv[0]);
		return 2;
	}
	if (chdir(argv[1])) { fprintf(stderr, "cannot chdir %s\n", argv[1]); return 1; }
	if (!(resmgr = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT,
						 1, 1024 * 128))) {
		fprintf(stderr, "no resources in %s\n", argv[1]);
		return 1;
	}

	gfxr_init_static_palette();
	mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);

	if (argc > 2) {
		for (i = 2; i < argc; i++) { checked++; bad += run(mode, atoi(argv[i]), 1); }
	} else {
		for (i = 0; i < 1000; i++)
			if (scir_test_resource(resmgr, sci_pic, i)) {
				checked++;
				bad += run(mode, i, verbose);
			}
	}

	printf("\n== %d pics checked, %d differ ==\n", checked, bad);
	printf("   %s\n", bad == 0
	       ? "visual maps agree: no writer has been missed."
	       : "MISMATCH: a visual writer is still emitting the old format.");
	return bad != 0;
}
