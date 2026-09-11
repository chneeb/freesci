/* ditherpreview.c -- render a pic BOTH ways and write BMPs, to answer
   "how different would 4bpp look?" without touching hardware.

     f256 : what the Pico renders today. Each visual byte is a DITHER PAIR
            (two EGA indices); INTERCOL blends them, hence the 256-entry
            gfx_sci0_pic_colors table and the 8bpp buffer.
     d16  : GFXR_DITHER_MODE_D16, commented "Sierra SCI style" -- one EGA index
            per pixel with a spatial checkerboard, 16 colours. This is what
            would allow a 4bpp visual buffer (64KB -> 32KB per buffer).

   Measured similarity (mean per-pixel RGB diff / share of byte-identical px):
     SQ3 pic 2   1.3/255   96.0%      KQ4 pic 25  7.2/255  78.5%
     SQ3 pic 3   4.5/255   87.9%      PQ2 pic 1  10.1/255  75.5%
   Most SCI0 art is solid colour, where both nibbles of the pair are equal and
   the blend equals the pure EGA colour -- so D16 matches EXACTLY there. Only
   genuinely dithered regions (PQ2's brickwork, skies) visibly change.

   Build (needs the desktop libs):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
         -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
         -o /tmp/ditherpreview tests/ditherpreview.c \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     DITHER_OUT=/tmp/out /tmp/ditherpreview ~/Downloads/quest/sq3 2 3
*/
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

static const char *outdir = ".";
static const char *modename = "f256";

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

	/* F256 (what the Pico renders today) leaves the pair-index map alone and
	   points colors[] at the 256-entry blend table. D16 ("Sierra SCI style")
	   rewrites index_data to 16-colour indices with a spatial checkerboard and
	   swaps colors[] to the 16-entry EGA palette -- which is what would let the
	   buffer be 4bpp. */
	if (strcmp(modename, "f256")) {
		pic->visual_map->colors = gfx_sci0_pic_colors;
		pic->visual_map->colors_nr = GFX_SCI0_PIC_COLORS_NR;
		gfxr_dither_pic0(pic, GFXR_DITHER_MODE_D16, GFXR_DITHER_PATTERN_1);
	} else {
		pic->visual_map->colors = gfx_sci0_pic_colors;
		pic->visual_map->colors_nr = GFX_SCI0_PIC_COLORS_NR;
	}

	{
		/* Write the visual map as a 24-bit BMP using whatever palette the
		   pixmap currently carries -- 256 blended pairs for F256, 16 EGA
		   colours for D16. */
		gfx_pixmap_t *vm = pic->visual_map;
		char name[256];
		FILE *f;
		int x, y, pad, i;
		int W = vm->index_xl, H = vm->index_yl;
		unsigned char hdr[54];
		unsigned int rowbytes = (unsigned)(W * 3 + 3) & ~3u;
		unsigned int datasize = rowbytes * H, filesize = 54 + datasize;

		sprintf(name, "%s/pic%03d_%s.bmp", outdir, picnum, modename);
		f = fopen(name, "wb");
		if (!f) { printf("  cannot write %s\n", name); return; }
		memset(hdr, 0, 54);
		hdr[0]='B'; hdr[1]='M';
		hdr[2]=filesize; hdr[3]=filesize>>8; hdr[4]=filesize>>16; hdr[5]=filesize>>24;
		hdr[10]=54; hdr[14]=40;
		hdr[18]=W; hdr[19]=W>>8; hdr[22]=H; hdr[23]=H>>8;
		hdr[26]=1; hdr[28]=24;
		hdr[34]=datasize; hdr[35]=datasize>>8; hdr[36]=datasize>>16; hdr[37]=datasize>>24;
		fwrite(hdr, 1, 54, f);
		pad = rowbytes - W * 3;
		for (y = H - 1; y >= 0; y--) {          /* BMP rows are bottom-up */
			for (x = 0; x < W; x++) {
				int idx = vm->index_data[y * W + x];
				gfx_pixmap_color_t *c;
				if (idx >= vm->colors_nr) idx = 0;
				c = &vm->colors[idx];
				fputc(c->b, f); fputc(c->g, f); fputc(c->r, f);
			}
			for (i = 0; i < pad; i++) fputc(0, f);
		}
		fclose(f);
		printf("  wrote %s  (%d colours)\n", name, vm->colors_nr);
	}
}

int main(int argc, char **argv)
{
	gfx_mode_t *mode;
	int i;

	if (argc < 3) {
		printf("usage: ditherpreview <gamedir> <picnum> [picnum...]\n"
		       "writes pic<NNN>_f256.bmp and pic<NNN>_d16.bmp into $DITHER_OUT (default .)\n");
		return 1;
	}
	if (getenv("DITHER_OUT"))
		outdir = getenv("DITHER_OUT");

	if (chdir(argv[1])) { printf("cannot chdir %s\n", argv[1]); return 1; }

	if (!(resmgr = scir_new_resource_manager(sci_getcwd(),
	                                         SCI_VERSION_AUTODETECT, 1, 1024*128))) {
		fprintf(stderr, "Could not find any resources; quitting.\n");
		return 1;
	}
	printf("sci_version=%d  resources_nr=%d\n", resmgr->sci_version, resmgr->resources_nr);
	fflush(stdout);

	gfxr_init_static_palette();          /* builds gfx_sci0_pic_colors */
	mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);

	for (i = 2; i < argc; i++) {
		modename = "f256";  dump_pic(mode, atoi(argv[i]));  fflush(stdout);
		modename = "d16";   dump_pic(mode, atoi(argv[i]));  fflush(stdout);
	}
	return 0;
}
