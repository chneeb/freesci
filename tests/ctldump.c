/* Desktop reference for the PQ2 Pico over-occlusion bug.
   Decodes a SCI0 pic through the SAME desktop path the engine uses
   (gfxr_init_pic + gfxr_clear_pic0 + gfxr_draw_pic01) and dumps the
   decoded priority map column x=82, rows 35..120, in the exact format of
   the on-device [pcol] probe, so the two priority maps can be diffed
   value-for-value.

   8th arg to gfxr_draw_pic01 is sci1 = 0 (SCI0). Passing resmgr->sci_version
   (=1) there mis-parses as SCI01 and yields garbage — keep it 0 for SCI0.

   Pico log (f21cedf3):
     pic 2081 (res->id) -> pic number 33 : 13..12..0..13
     pic 2049 (res->id) -> pic number 1

   The harness mimics the device [pcol] format itself; it does NOT need the
   FSCI_PROBE_GFX engine probe, so it links against the plain desktop libs.

   BUILD (against the desktop static libs; build the desktop target first):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
       -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
       -o /tmp/pridump tests/pridump.c \
       -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl

   RUN (in, or pointed at, the PQ2 game dir):
     /tmp/pridump ~/Downloads/pq2            # dumps pics 33 and 1
     /tmp/pridump ~/Downloads/pq2 33 1       # dump these pic numbers
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

static resource_mgr_t *resmgr;

static void dump_pic(gfx_mode_t *mode, int picnum)
{
	resource_t *res = scir_find_resource(resmgr, sci_pic, picnum, 0);
	gfxr_pic0_params_t style;
	gfxr_pic_t *pic;
	int y;

	if (!res || !res->data) {
		printf("pic %d: NOT FOUND\n", picnum);
		return;
	}

	style.line_mode    = GFX_LINE_MODE_CORRECT;
	style.brush_mode   = GFX_BRUSH_MODE_SCALED;
	style.pic_port_bounds = gfx_rect(0, 10, 320, 190);

	pic = gfxr_init_pic(mode, res->id, 0);
	gfxr_clear_pic0(pic, 10);
	gfxr_draw_pic01(pic, 1, 0, res->size, res->data, &style, res->id,
	                0, NULL, 0);

	{
		gfx_pixmap_t *cmap = pic->control_map;
		gfx_pixmap_t *pmap = pic->priority_map;
		int xl, yl, px, py, v;
		int hist[16];

		if (!cmap || !cmap->index_data) {
			printf("pic %d: NO CONTROL MAP decoded\n", picnum);
			return;
		}
		xl = cmap->index_xl; yl = cmap->index_yl;
		for (px = 0; px < 16; px++) hist[px] = 0;
		for (py = 0; py < yl; py++)
			for (px = 0; px < xl; px++) {
				v = cmap->index_data[py * xl + px];
				if (v >= 0 && v < 16) hist[v]++;
			}
		printf("pic %d  control map %dx%d\n", picnum, xl, yl);
		printf("  control histogram:");
		for (px = 0; px < 16; px++) if (hist[px]) printf(" c%d:%d", px, hist[px]);
		printf("\n");

		/* Coarse ASCII map: each cell = the dominant control colour of an
		   8x4 block, so the water/land split is visible at a glance. */
		printf("  control map (each char = 8x4 block, '.'=0 no-control, digit=control colour):\n");
		for (py = 0; py < yl; py += 4) {
			printf("    ");
			for (px = 0; px < xl; px += 8) {
				int cnt[16], i, best = 0, bx, by;
				for (i = 0; i < 16; i++) cnt[i] = 0;
				for (by = py; by < py + 4 && by < yl; by++)
					for (bx = px; bx < px + 8 && bx < xl; bx++) {
						v = cmap->index_data[by * xl + bx];
						if (v >= 0 && v < 16) cnt[v]++;
					}
				for (i = 1; i < 16; i++) if (cnt[i] > cnt[best]) best = i;
				putchar(best == 0 ? '.' : (best < 10 ? '0' + best : 'a' + best - 10));
			}
			printf("  y=%d\n", py);
		}
		if (pmap && pmap->index_data) {
			long pnz = 0;
			for (px = 0; px < pmap->index_xl * pmap->index_yl; px++)
				if (pmap->index_data[px]) pnz++;
			printf("  (priority map nonzero=%ld)\n", pnz);
		}
	}
}

int main(int argc, char **argv)
{
	gfx_mode_t *mode;
	int i;

	if (argc > 1 && chdir(argv[1])) {
		printf("cannot chdir %s\n", argv[1]);
		return 1;
	}

	if (!(resmgr = scir_new_resource_manager(sci_getcwd(),
	                                         SCI_VERSION_AUTODETECT, 1, 1024*128))) {
		fprintf(stderr, "Could not find any resources; quitting.\n");
		return 1;
	}
	printf("sci_version=%d  resources_nr=%d\n", resmgr->sci_version, resmgr->resources_nr);

	mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);

	/* pic res->id 2081 = number 33 ; 2049 = number 1 */
	for (i = 2; i < argc; i++)
		dump_pic(mode, atoi(argv[i]));

	if (argc <= 2) {
		dump_pic(mode, 33);
		dump_pic(mode, 1);
	}
	return 0;
}
