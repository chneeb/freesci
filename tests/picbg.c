/* Desktop background-fingerprint harness for the PQ2 (SCI0) missing-cel bug.

   The on-device [pblit] probe logs each BACKGROUND blit with an opaque-pixel
   count (pb_opaque = pixels whose visual index != color_key, or every pixel if
   the visual map has no color key).  A captured PQ2 session showed exactly these
   distinct background opaque values:

       64000, 60800, 57185, 53985, 63991, 60791

   The probe does NOT log the pic number, so we can't tell which captured
   background is which scene (glovebox / parking lot / car interior).  This
   harness closes that gap WITHOUT a device flash: it decodes every PQ2 pic
   through the same desktop path (gfxr_init_pic + gfxr_clear_pic0 +
   gfxr_draw_pic01) and computes the SAME opaque metric over the visual_map, so
   each captured bg_opaque value can be matched to a pic number.

   Why two counts per pic: on device the menu-bar 10-row band is sometimes part
   of the blit and sometimes not, so the same pic appears at full-height
   (320x200) and play-area (320x190 == full-3200) opacity.  We print both.

   8th arg to gfxr_draw_pic01 is sci1 = 0 (SCI0); passing resmgr->sci_version
   (=1) mis-parses as SCI01 and yields garbage -- keep it 0 for SCI0.

   BUILD (against the desktop static libs; build the desktop target first):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
       -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
       -o /tmp/picbg tests/picbg.c \
       -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl

   RUN (in, or pointed at, the PQ2 game dir):
     /tmp/picbg ~/Downloads/pq2          # scan all pics, print fingerprints + matches
     /tmp/picbg ~/Downloads/pq2 33 1     # detail these pic numbers
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

/* The captured device background opaque values to match against. */
static const int targets[] = { 64000, 60800, 57185, 53985, 63991, 60791 };
#define NTARGETS ((int)(sizeof(targets)/sizeof(targets[0])))

/* Compute opaque pixels (index != color_key, or all if no key) over a sub-band
   of the visual map: rows [y0, y1).  Mirrors the [pblit] background metric. */
static long visual_opaque(gfx_pixmap_t *vm, int y0, int y1)
{
	int xl = vm->index_xl, yl = vm->index_yl;
	int key = vm->color_key;
	int has_key = (key != GFX_PIXMAP_COLOR_KEY_NONE);
	long opaque = 0;
	int x, y;

	if (y1 > yl) y1 = yl;
	for (y = y0; y < y1; y++)
		for (x = 0; x < xl; x++) {
			byte px = vm->index_data[y * xl + x];
			if (!has_key || px != key) opaque++;
		}
	return opaque;
}

static int match_target(long v)
{
	int i;
	for (i = 0; i < NTARGETS; i++)
		if (targets[i] == v) return 1;
	return 0;
}

static void scan_pic(gfx_mode_t *mode, int picnum, int verbose)
{
	resource_t *res = scir_find_resource(resmgr, sci_pic, picnum, 0);
	gfxr_pic0_params_t style;
	gfxr_pic_t *pic;
	gfx_pixmap_t *vm;
	long full, play;
	int hit;

	if (!res || !res->data) {
		if (verbose) printf("pic %d: NOT FOUND\n", picnum);
		return;
	}

	style.line_mode       = GFX_LINE_MODE_CORRECT;
	style.brush_mode      = GFX_BRUSH_MODE_SCALED;
	style.pic_port_bounds = gfx_rect(0, 10, 320, 190);

	pic = gfxr_init_pic(mode, res->id, 0);
	gfxr_clear_pic0(pic, 10);
	gfxr_draw_pic01(pic, 1, 0, res->size, res->data, &style, res->id, 0, NULL, 0);

	vm = pic->visual_map;
	full = visual_opaque(vm, 0, vm->index_yl);          /* 320x200 */
	play = visual_opaque(vm, 10, vm->index_yl);          /* 320x190 (skip menu bar) */

	/* Count palette-index-255 (solid white) pixels: these are the pixels the
	   device's color_key=int(-1)->byte(255) truncation bug drops as transparent.
	   full255 should equal (64000 - device_opaque); play255 (in the play area,
	   matching the device 320x190 blit) equals (60800 - device_opaque). */
	long full255 = 0, play255 = 0;
	{
		int x, y;
		for (y = 0; y < vm->index_yl; y++)
			for (x = 0; x < vm->index_xl; x++)
				if (vm->index_data[y * vm->index_xl + x] == 255) {
					full255++;
					if (y >= 10) play255++;
				}
	}
	hit  = match_target(full) || match_target(play)
	       || full255 == 6815 || full255 == 9
	       || play255 == 6815 || play255 == 9;

	if (verbose || hit) {
		printf("pic %4d (res->id=%d): %dx%d color_key=%d  full=%ld play=%ld  "
		       "idx255: full=%ld play=%ld  (devbg full=%ld play=%ld)%s\n",
		       picnum, res->id, vm->index_xl, vm->index_yl, vm->color_key,
		       full, play, full255, play255, 64000 - full255, 60800 - play255,
		       (full255 == 6815 || full255 == 9 || play255 == 6815 || play255 == 9)
		           ? "  <== idx255 MATCH" : "");
	}

	gfxr_free_pic(NULL, pic);
}

int main(int argc, char **argv)
{
	gfx_mode_t *mode;
	int i;

	if (argc < 2) {
		printf("usage: %s <gamedir> [picnum ...]\n", argv[0]);
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
	printf("sci_version=%d  resources_nr=%d\n",
	       resmgr->sci_version, resmgr->resources_nr);

	mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);

	printf("matching device bg_opaque targets:");
	for (i = 0; i < NTARGETS; i++) printf(" %d", targets[i]);
	printf("\n");

	if (argc > 2) {
		for (i = 2; i < argc; i++)
			scan_pic(mode, atoi(argv[i]), 1);
	} else {
		for (i = 0; i < 1024; i++)
			scan_pic(mode, i, 0);
	}
	return 0;
}
