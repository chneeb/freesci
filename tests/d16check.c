/* d16check.c -- verify the precondition the whole 4bpp plan rests on: that with
   GFXR_DITHER_MODE_D16 the visual map only ever holds 0..15, so it can be
   nibble-packed (64KB -> 32KB, PIO-only value; see CLAUDE.md).

   This is a PRECONDITION check, not a nicety. Packing a buffer that can hold a
   value above 15 corrupts the display silently -- the failure mode that made the
   16-bit LCD attempt painful -- so it is worth proving across every pic of every
   game available rather than reasoning from the dither code.

   Also reports how many DISTINCT indices each pic actually uses, which is the
   measure of how much the 8bpp buffer is wasting.

   Build (needs the desktop libs):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
         -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
         -o /tmp/d16check tests/d16check.c \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     /tmp/d16check ~/Downloads/quest/sq3 ~/Downloads/quest/pq2 ...
*/

#include <stdio.h>
#include <string.h>
#include <sciresource.h>
#include <gfx_resource.h>
#include <gfx_tools.h>
#include <gfx_options.h>
#include <sci_memory.h>
#include <unistd.h>
#include <engine.h>

extern gfx_pixmap_color_t gfx_sci0_pic_colors[];

static int
check_game(const char *dir, int *pics_out, int *worst_distinct)
{
	resource_mgr_t *resmgr;
	gfx_mode_t *mode;
	int i, bad_pics = 0, pics = 0;

	/* The resmgr wants the game dir as CWD and the path from sci_getcwd(), and
	   the version must be AUTODETECT -- passing 0 segfaults during the map
	   parse. Same setup as tests/ditherpreview.c, which is known to work. */
	if (chdir(dir)) { fprintf(stderr, "  !! cannot chdir %s\n", dir); return -1; }

	resmgr = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT,
					   1, 1024 * 128);
	if (!resmgr) {
		fprintf(stderr, "  !! no resources in %s\n", dir);
		return -1;
	}

	gfxr_init_static_palette();          /* builds gfx_sci0_pic_colors */
	mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);

	for (i = 0; i < 1000; i++) {
		resource_t *res = scir_test_resource(resmgr, sci_pic, i);
		gfxr_pic_t *pic;
		int x, seen[256], distinct = 0, maxv = 0, over = 0;
		gfxr_pic0_params_t style;

		if (!res)
			continue;

		res = scir_find_resource(resmgr, sci_pic, i, 0);
		if (!res || !res->data) continue;

		/* style must be a real struct -- gfxr_draw_pic01 dereferences it, so
		   passing NULL segfaults. Same values as tests/ditherpreview.c. */
		style.line_mode       = GFX_LINE_MODE_CORRECT;
		style.brush_mode      = GFX_BRUSH_MODE_SCALED;
		style.pic_port_bounds = gfx_rect(0, 10, 320, 190);

		pic = gfxr_init_pic(mode, res->id, 0);
		if (!pic)
			continue;
		gfxr_clear_pic0(pic, 10);

		/* NB the 8th arg (sci1) MUST be 0 for SCI0 -- passing the resmgr's
		   sci_version mis-parses as SCI01 and yields garbage (a harness bug
		   already paid for once, recorded in CLAUDE.md). */
		gfxr_draw_pic01(pic, 1, 0, res->size, res->data, &style, res->id,
				0, NULL, 0);

		/* the shipped Pico path: dither to D16 */
		pic->visual_map->colors = gfx_sci0_pic_colors;
		pic->visual_map->colors_nr = GFX_SCI0_PIC_COLORS_NR;
		gfxr_dither_pic0(pic, GFXR_DITHER_MODE_D16, GFXR_DITHER_PATTERN_1);

		memset(seen, 0, sizeof(seen));
		for (x = 0; x < pic->visual_map->index_xl * pic->visual_map->index_yl; x++) {
			int v = pic->visual_map->index_data[x];
			if (!seen[v]) { seen[v] = 1; distinct++; }
			if (v > maxv) maxv = v;
			if (v > 15) over++;
		}

		pics++;
		if (distinct > *worst_distinct) *worst_distinct = distinct;
		if (over) {
			bad_pics++;
			printf("  pic %-4d FAILS: %d px above 15 (max %d)\n", i, over, maxv);
		}
		gfxr_free_pic(NULL, pic);
	}

	scir_free_resource_manager(resmgr);
	*pics_out = pics;
	return bad_pics;
}

int
main(int argc, char **argv)
{
	int g, total_pics = 0, total_bad = 0, worst = 0;

	setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: a crash must not eat the trace */

	if (argc < 2) {
		fprintf(stderr, "usage: %s <gamedir> [gamedir...]\n", argv[0]);
		return 2;
	}

	for (g = 1; g < argc; g++) {
		int pics = 0, bad;
		printf("%s\n", argv[g]);
		bad = check_game(argv[g], &pics, &worst);
		if (bad < 0) continue;
		printf("  %d pics, %d with any index > 15\n", pics, bad);
		total_pics += pics;
		total_bad += bad;
	}

	printf("\n== %d pics across %d games; %d violate the 0..15 invariant ==\n",
	       total_pics, argc - 1, total_bad);
	printf("   worst-case distinct indices in one pic: %d (of 16 representable)\n", worst);
	printf("   %s\n", total_bad == 0
	       ? "PRECONDITION HOLDS: the visual map is nibble-packable."
	       : "PRECONDITION VIOLATED: do NOT pack the visual buffer.");
	return total_bad != 0;
}
