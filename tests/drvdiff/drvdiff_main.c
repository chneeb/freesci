/* drvdiff -- exercise the real pico_driver.c on the DESKTOP and capture exactly
   what it would send to the panel, so the driver half of the 4bpp conversion can
   be verified without a device.

   WHY: visdiff covers pic DECODE only. The driver half -- flush_region,
   pico_blit_indexed, draw_line, filled_rect, grab/restore, bake, the BACK
   restore -- had NO offline coverage at all, and that is exactly where the
   16-bit LCD attempt died: one writer was converted, the rest still emitted the
   old format, and it only showed up as a sheared picture on hardware.

   HOW: pico_driver.c's hardware surface is small (13 functions), so drvstub.c
   stands in for it and captures hw_send_spi -- the bytes that actually reach the
   panel -- plus every define_region_spi header, because a correct pixel stream
   sent to the wrong window is still a wrong picture.

   Packed vs unpacked is a COMPILE-TIME choice, so rather than link two renamed
   copies (picodiff's trick) this builds TWICE and diffs the two captures. Same
   rigour, much less machinery.

     cd tests/drvdiff
     LIBS=$(find ../../build -name '*.a' | tr '\n' ' ')
     for cfg in "" "-DPICO_PACK_VISUAL=1"; do
       out=$([ -z "$cfg" ] && echo unpacked || echo packed)
       gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 -DHAVE_PICO=1 \
           -DPICO_DITHER_D16=1 $cfg \
           -I. -I../../src/include -I../../build -I/usr/include/SDL2 -D_REENTRANT \
           -o drvdiff_$out drvdiff_main.c ../../src/gfx/drivers/pico_driver.c drvstub.c \
           -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     done
     ./drvdiff_unpacked cap_unpacked.bin && ./drvdiff_packed cap_packed.bin
     cmp cap_unpacked.bin cap_packed.bin && echo "driver output identical"

   Until the driver is converted both builds are identical, and the compare
   passing is the harness validating itself.
*/
#include <stdint.h>
#include <gfx_driver.h>
#include <gfx_tools.h>
#include <gfx_resource.h>
#include "psram_alloc.h"
#include <sci_memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern gfx_driver_t gfx_driver_pico;
void capture_reset(void);
size_t capture_len(void);
const uint8_t *capture_data(void);

#define W 320
#define H 200

/* mask MUST be set or every draw is silently a no-op: pico_draw_filled_rect and
   friends gate on (color.mask & GFX_MASK_VISUAL). A zeroed gfx_color_t produces
   a blank frame and no error -- which is exactly what the first run of this
   harness did. */
static gfx_color_t
vis_color(int index)
{
	gfx_color_t c;

	memset(&c, 0, sizeof c);
	c.visual.global_index = index;
	c.visual.r = (byte)(index * 7);
	c.visual.g = (byte)(index * 13);
	c.visual.b = (byte)(index * 29);
	c.mask = GFX_MASK_VISUAL;
	return c;
}

/* A cel with transparency and a couple of solid runs -- enough that a blit that
   mishandles the colour key, the nibble parity or the row stride shows up. */
static gfx_pixmap_t *
make_cel(int xl, int yl)
{
	gfx_pixmap_t *p = gfx_pixmap_alloc_index_data(gfx_new_pixmap(xl, yl, 42, 0, 0));
	int x, y;

	/* colors/colors_nr must be set: pico_blit_indexed maps through them, and a
	   NULL palette silently collapses every index to one colour. colors_nr 256
	   selects the identity path (what backgrounds use), so index i lands in
	   palette slot i and an index error shows up as a colour change. */
	gfxr_init_static_palette();
	p->colors = gfx_sci0_pic_colors;
	p->colors_nr = GFX_SCI0_PIC_COLORS_NR;
	p->color_key = 255;
	p->index_xl = xl;
	p->index_yl = yl;
	p->xl = xl;
	p->yl = yl;
	for (y = 0; y < yl; y++)
		for (x = 0; x < xl; x++)
			p->index_data[y * xl + x] =
				((x + y) % 7 == 0) ? 255            /* transparent */
				: (byte)((x * 3 + y * 5) & 0x0f);   /* all 16 indices */
	return p;
}

int
main(int argc, char **argv)
{
	gfx_driver_t *drv = &gfx_driver_pico;
	gfx_pixmap_t *cel;
	FILE *f;
	int i;

	if (argc < 2) { fprintf(stderr, "usage: %s <capture-out>\n", argv[0]); return 2; }

	if (drv->init(drv)) { fprintf(stderr, "driver init failed\n"); return 1; }

	/* A deterministic palette: index i -> a distinct RGB, so any index error
	   shows up as a colour change rather than being masked by duplicates. */
	for (i = 0; i < 256; i++)
		drv->set_palette(drv, i, (byte)(i * 7), (byte)(i * 13), (byte)(i * 29));

	capture_reset();

	/* Exercise the driver paths that touch the visual buffer. */
	drv->draw_filled_rect(drv, gfx_rect(10, 12, 120, 40),
			      vis_color(6), vis_color(6), GFX_SHADE_FLAT);
	drv->draw_line(drv, gfx_point(3, 15), gfx_point(300, 150),
		       vis_color(12), GFX_LINE_MODE_CORRECT, GFX_LINE_STYLE_NORMAL);

	cel = make_cel(60, 30);
	drv->draw_pixmap(drv, cel, GFX_NO_PRIORITY,
			 gfx_rect(0, 0, 60, 30), gfx_rect(40, 55, 60, 30),
			 GFX_BUFFER_BACK);

	/* A STATIC draw, so pico_bake_static_region runs. Needs a PSRAM-backed
	   static_bg, because the bake bails without one -- and if it bails the
	   capture proves nothing about it (see the grab/restore false pass). */
	{
		gfx_pixmap_t *bg = gfx_new_pixmap(W, H, GFX_RESID_NONE, 0, 0);
		gfx_pixmap_t *sv = make_cel(47, 24);   /* odd width */

		bg->index_xl = W; bg->index_yl = H;
		bg->index_data = NULL;            /* PSRAM-resident, as on device */
		bg->psram_addr = psram_alloc((size_t)W * H);
		bg->psram_valid = 1;
		/* MUST match the driver's format, or the bake stores nibbles while the
		   BACK restore reads bytes. On device the pic decoder sets this when
		   the visual map is packed; the harness has to model it or it tests a
		   configuration that cannot exist. */
#ifdef PICO_PACK_VISUAL
		bg->nibble_packed = 1;
#endif
		drv->set_static_buffer(drv, bg, NULL);

		/* odd x and odd width on purpose: that is where a packed bake has to
		   read-modify-write the shared edge bytes. */
		drv->draw_pixmap(drv, sv, GFX_NO_PRIORITY,
				 gfx_rect(0, 0, 47, 24), gfx_rect(97, 40, 47, 24),
				 GFX_BUFFER_STATIC);   /* src == dest: no scaling */
	}

	/* THE BAKE WRITES TO PSRAM, NOT THE PANEL -- so a static draw alone still
	   proves nothing about it. A BACK restore reads the composed surface back
	   into visual[0], which is what finally puts the baked bytes on the wire.
	   (Third coverage gap in this harness; see the README.) */
	drv->update(drv, gfx_rect(90, 38, 60, 28), gfx_point(90, 38), GFX_BUFFER_BACK);

	if (!getenv("NOGRAB")) {
	/* Grab a region and restore it elsewhere. Without this the capture would
	   NOT cover grab/restore at all, and cmp passing would be a FALSE pass --
	   the harness only proves the paths the scene actually walks. */
	{
		gfx_pixmap_t *grab = gfx_new_pixmap(80, 40, GFX_RESID_NONE, 0, 0);

		grab->xl = grab->index_xl = 80;
		grab->yl = grab->index_yl = 40;
		drv->grab_pixmap(drv, gfx_rect(20, 20, 80, 40), grab, GFX_MASK_VISUAL);
		drv->draw_pixmap(drv, grab, GFX_NO_PRIORITY,
				 gfx_rect(0, 0, 80, 40), gfx_rect(150, 100, 80, 40),
				 GFX_BUFFER_BACK);
	}
	}

	/* Push it to the "panel" -- this is what gets captured. */
	drv->update(drv, gfx_rect(0, 0, W, H), gfx_point(0, 0), GFX_BUFFER_FRONT);

	f = fopen(argv[1], "wb");
	if (!f) { perror(argv[1]); return 1; }
	fwrite(capture_data(), 1, capture_len(), f);
	fclose(f);

	/* PPM=1 also dumps the frame as an image, so a difference can be LOOKED at
	   rather than only counted -- the 16-bit LCD failure was a shear, which is
	   obvious by eye and tedious to characterise from bytes. */
	if (getenv("PPM")) {
		char name[512];
		size_t px = capture_len() > 20 ? (capture_len() - 20) / 3 : 0;

		snprintf(name, sizeof name, "%s.ppm", argv[1]);
		f = fopen(name, "wb");
		if (f && px >= (size_t)(W * H)) {
			fprintf(f, "P6\n%d %d\n255\n", W, H);
			fwrite(capture_data() + 20, 1, (size_t)W * H * 3, f);
			printf("  wrote %s\n", name);
		}
		if (f) fclose(f);
	}

	printf("%s: captured %zu bytes of panel traffic\n", argv[1], capture_len());
	return 0;
}
