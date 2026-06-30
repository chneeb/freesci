/* Desktop cel-blit comparison harness for the PQ2 (SCI0) missing/garbled-cel bug.

   The companion harness tests/viewdump.c established that the PQ2 missing-object
   bug is NOT blank-cel decode (opaque=0 is normal SCI0 content).  The bug must be
   in how the Pico renders the cels that DO decode with content (opaque>0):
   positioning / occlusion / palette mapping.  This harness reproduces the Pico
   blit's per-pixel decisions on the desktop, against the SAME shared-engine cel
   decode, so those three render paths can be diffed WITHOUT a device flash.

   What it models (a faithful host port of pico_blit_indexed / nearest_pal,
   pico_driver.c):

   1. PALETTE MAPPING.  The Pico has no per-pixmap translation table; it maps each
      local cel colour to a 256-slot palette entry with nearest_pal() (closest RGB
      in gfx_sci0_pic_colors[]).  The SDL pipeline instead renders each cel colour's
      TRUE RGB.  So on device a cel pixel of local colour i shows
          gfx_sci0_pic_colors[ nearest_pal(colors[i].rgb) ]
      whereas the correct (SDL) result is
          colors[i].rgb .
      For pure EGA cels these agree (EGA colour k sits exactly at slot k*17), so a
      DIVERGENCE here is a real device-only colour error.  The harness flags every
      cel colour whose Pico-mapped RGB != its true RGB.

   2. SKIP-GUARD GEOMETRY.  pico_blit_indexed silently DROPS (returns, cel invisible)
      any cel whose index_xl/index_yl falls outside [1..PICO_XSIZE]/[1..PICO_YSIZE]
      (320x200) — desktop would just clip it.  A PQ2 object that decodes fine but is
      oversized would vanish only on device.  The harness flags any cel that the
      guard would drop.

   3. OPACITY.  Reports opaque (index != color_key) pixel count, so an opaque>0 cel
      (a real candidate) is distinguished from a blank one (noise, per viewdump).

   gfxr_draw_cel0 sets color_key=255 and maps every run whose colour == resource[6]
   to 255; for SCI0 the cel carries colors = gfx_sci0_image_colors[] (16 EGA), so a
   pixel's local index is 0..15 or 255(=transparent).

   This is desktop-only (no HAVE_PICO): the cels keep index_data resident, which is
   what we read here; the Pico decisions are re-derived from that resident data.

   BUILD (against the desktop static libs; build the desktop target first):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
       -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
       -o /tmp/celblit tests/celblit.c \
       -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl

   RUN (in, or pointed at, the PQ2 game dir):
     /tmp/celblit ~/Downloads/pq2            # scan all views, report only problem cels
     /tmp/celblit ~/Downloads/pq2 5 17 132   # full per-cel dump of these view numbers
*/
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <sciresource.h>
#include <engine.h>
#include <console.h>
#include <versions.h>
#include <gfx_resource.h>
#include <gfx_tools.h>
#include <gfx_system.h>

#define PICO_XSIZE 320
#define PICO_YSIZE 200

static resource_mgr_t *resmgr;
static int pico_pal[256][3];   /* mirror of pico_driver.c ps->palette[] */

/* Faithful port of pico_driver.c nearest_pal(): closest RGB slot, first wins. */
static int nearest_pal(int r, int g, int b)
{
	int best = 0, best_d = 0x7fffffff, i;
	for (i = 0; i < 256; i++) {
		int dr = r - pico_pal[i][0];
		int dg = g - pico_pal[i][1];
		int db = b - pico_pal[i][2];
		int d = dr * dr + dg * dg + db * db;
		if (d < best_d) { best_d = d; best = i; if (!d) break; }
	}
	return best;
}

/* Per-cel comparison.  Returns 1 if the cel has ANY device-only problem
   (skip-guard drop or a palette divergence), else 0. */
static int compare_cel(int viewnum, int loop, int c, gfx_pixmap_t *cel, int verbose)
{
	int n = cel->index_xl * cel->index_yl;
	int key = cel->color_key;
	int i, problem = 0;
	long opaque = 0, divergent_px = 0;
	int used[256]; long used_count[256];
	int skip_guard;

	for (i = 0; i < 256; i++) { used[i] = 0; used_count[i] = 0; }

	/* SKIP-GUARD: pico_blit_indexed drops cels outside these bounds (invisible). */
	skip_guard = (cel->index_xl <= 0 || cel->index_xl > PICO_XSIZE
	              || cel->index_yl <= 0 || cel->index_yl > PICO_YSIZE);

	/* Histogram the local colours actually present in the opaque pixels. */
	for (i = 0; i < n; i++) {
		byte px = cel->index_data[i];
		if (px != key) { opaque++; used[px]++; used_count[px]++; }
	}

	/* PALETTE MAPPING: for each used local colour, does the Pico nearest_pal slot
	   reproduce the cel colour's true RGB?  (SDL renders the true RGB.) */
	{
		int col, ndiv = 0;
		for (col = 0; col < 256; col++) {
			if (!used[col]) continue;
			if (col >= cel->colors_nr) {
				/* Out-of-range local index: no colour entry -> Pico LUT is garbage.
				   (Decoder should not emit these; flag loudly if it does.) */
				if (verbose || 1)
					printf("view %d loop %d cel %d: local colour %d >= colors_nr %d "
					       "(%ld px) -- NO COLOUR ENTRY\n",
					       viewnum, loop, c, col, cel->colors_nr, used_count[col]);
				problem = 1;
				divergent_px += used_count[col];
				continue;
			}
			{
				int tr = cel->colors[col].r, tg = cel->colors[col].g, tb = cel->colors[col].b;
				int slot = nearest_pal(tr, tg, tb);
				int pr = pico_pal[slot][0], pg = pico_pal[slot][1], pb = pico_pal[slot][2];
				if (pr != tr || pg != tg || pb != tb) {
					problem = 1;
					ndiv++;
					divergent_px += used_count[col];
					if (verbose || ndiv <= 8)
						printf("view %d loop %d cel %d: PALETTE DIVERGENCE local %d "
						       "true=%02x/%02x/%02x -> slot %d = %02x/%02x/%02x (%ld px)\n",
						       viewnum, loop, c, col, tr, tg, tb,
						       slot, pr, pg, pb, used_count[col]);
				}
			}
		}
	}

	if (skip_guard) problem = 1;

	if (verbose || problem) {
		printf("view %d loop %d cel %d: %dx%d color_key=%d xoff=%d yoff=%d "
		       "opaque=%ld divergent_px=%ld%s\n",
		       viewnum, loop, c, cel->index_xl, cel->index_yl, cel->color_key,
		       cel->xoffset, cel->yoffset, opaque, divergent_px,
		       skip_guard ? "  <-- SKIP-GUARD: DROPPED ON DEVICE (invisible)" : "");
	}
	return problem;
}

static void dump_view(int viewnum, int verbose)
{
	resource_t *res = scir_find_resource(resmgr, sci_view, viewnum, 0);
	gfxr_view_t *view;
	int loop, any_problem = 0;

	if (!res || !res->data) {
		if (verbose) printf("view %d: NOT FOUND\n", viewnum);
		return;
	}

	view = gfxr_draw_view0(res->id, res->data, res->size, -1);
	if (!view) {
		printf("view %d (res->id=%d size=%d): DECODE FAILED\n",
		       viewnum, res->id, res->size);
		return;
	}

	for (loop = 0; loop < view->loops_nr; loop++) {
		int c;
		for (c = 0; c < view->loops[loop].cels_nr; c++) {
			gfx_pixmap_t *cel = view->loops[loop].cels[c];
			if (!cel) {
				printf("view %d loop %d cel %d: NULL cel\n", viewnum, loop, c);
				any_problem = 1;
				continue;
			}
			if (compare_cel(viewnum, loop, c, cel, verbose))
				any_problem = 1;
		}
	}

	if (verbose && !any_problem)
		printf("view %d: all cels render-equivalent to SDL (no palette/geometry divergence)\n",
		       viewnum);

	gfxr_free_view(NULL, view);
}

int main(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		printf("usage: %s <gamedir> [viewnum ...]\n", argv[0]);
		return 1;
	}

	if (chdir(argv[1])) {
		printf("cannot chdir %s\n", argv[1]);
		return 1;
	}

	if (!(resmgr = scir_new_resource_manager(sci_getcwd(),
	                                         SCI_VERSION_AUTODETECT, 1, 1024 * 128))) {
		fprintf(stderr, "Could not find any resources; quitting.\n");
		return 1;
	}

	/* Populate gfx_sci0_pic_colors[] (the 256-slot dithered SCI0 palette), then
	   mirror it into pico_pal[] exactly as pico_setup_sci0_palette() does. */
	gfxr_init_static_palette();
	for (i = 0; i < 256; i++) {
		pico_pal[i][0] = gfx_sci0_pic_colors[i].r;
		pico_pal[i][1] = gfx_sci0_pic_colors[i].g;
		pico_pal[i][2] = gfx_sci0_pic_colors[i].b;
	}

	printf("sci_version=%d  resources_nr=%d\n",
	       resmgr->sci_version, resmgr->resources_nr);

	if (argc > 2) {
		for (i = 2; i < argc; i++)
			dump_view(atoi(argv[i]), 1);
	} else {
		long scanned = 0, flagged = 0;
		printf("scanning all views for device-only render divergences "
		       "(palette mapping / skip-guard geometry)...\n");
		for (i = 0; i < 1024; i++) {
			resource_t *res = scir_find_resource(resmgr, sci_view, i, 0);
			if (res && res->data) {
				scanned++;
				/* dump_view prints per problem cel; track flagged views cheaply
				   by re-deriving via a verbose=0 pass. */
				dump_view(i, 0);
			}
		}
		printf("scanned %ld views\n", scanned);
		(void)flagged;
	}

	return 0;
}
