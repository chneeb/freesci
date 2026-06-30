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
		gfx_pixmap_t *pmap = pic->priority_map;
		int xl = pmap->index_xl, yl = pmap->index_yl;
		long nz = 0; int maxv = 0; int hist[16] = {0};
		int px, py;
		for (py = 0; py < yl; py++)
			for (px = 0; px < xl; px++) {
				int v = pmap->index_data[py * xl + px];
				if (v) nz++;
				if (v > maxv) maxv = v;
				if (v >= 0 && v < 16) hist[v]++;
			}
		{
			gfx_pixmap_t *vm = pic->visual_map;
			long vnz = 0; int vp;
			for (vp = 0; vp < vm->index_xl * vm->index_yl; vp++)
				if (vm->index_data[vp]) vnz++;
			printf("  visual nonzero=%ld / %d\n", vnz, vm->index_xl*vm->index_yl);
		}
		printf("pic %d (res->id=%d) size=%d xl=%d yl=%d nonzero=%ld max=%d\n",
		       picnum, res->id, res->size, xl, yl, nz, maxv);
		printf("  prio histogram:");
		for (px = 0; px < 16; px++) if (hist[px]) printf(" %d:%d", px, hist[px]);
		printf("\n  [pcol] x=82 rows35-120 pri:");
		for (y = 35; y <= 120; y++)
			printf(" %d", pmap->index_data[y * xl + 82]);
		printf("\n  [prow] y=100 x60-150 pri:");
		for (px = 60; px <= 150; px++)
			printf(" %d", pmap->index_data[100 * xl + px]);
		printf("\n");

		/* Per-cel cross-check: for each [pblit] cel rect, count desktop-harness
		   pixels whose priority EXCEEDS the device cel priority, and compare to
		   the device's supp count.  For a fully-opaque cel (head: drawn+supp==area)
		   harness_over should EQUAL device_supp if the priority maps are identical.
		   A large divergence means the Pico decode produced a spurious priority
		   block the desktop decode does not have. Fields: x,y,w,h,celpri,devsupp. */
		{
			static const int rects[][6] = {
				{108,65,135,58, 3,2474}, {43,33,70,82, 12,1085},
				{84,108,142,14, 1,1150}, {115,112,31,34, 12,95},
				{116,113,21,37, 12,213}, {123,141,21,37, 12,362},
				{136,133,18,37, 12,76},  {141,111,15,14, 2,3},
				{0,0,0,0,0,0}
			};
			int r;
			for (r = 0; rects[r][2]; r++) {
				int rx=rects[r][0], ry=rects[r][1], rw=rects[r][2], rh=rects[r][3];
				int celpri=rects[r][4], devsupp=rects[r][5];
				int mn=99, mx=-1, ix, iy, over=0, area=0;
				for (iy=ry; iy<ry+rh && iy<yl; iy++)
					for (ix=rx; ix<rx+rw && ix<xl; ix++) {
						int v = pmap->index_data[iy*xl+ix];
						if (v<mn) mn=v; if (v>mx) mx=v;
						if (v > celpri) over++;
						area++;
					}
				printf("  [rect] (%d,%d %dx%d) celpri=%d bgpri=%d..%d "
				       "harness_over=%d dev_supp=%d area=%d\n",
				       rx,ry,rw,rh,celpri,mn,mx,over,devsupp,area);
			}
		}

		/* 2D priority grid over the head cel (108,65 135x58), 1 char per 5px col,
		   every 3rd row, so the structure of the 13/12/0 regions is visible. */
		{
			int gy;
			printf("  [grid] cel(108,65 135x58) prio (cols 108..242 step5, rows step3):\n");
			for (gy = 65; gy < 65+58; gy += 3) {
				int gx;
				printf("   y%3d ", gy);
				for (gx = 108; gx < 108+135; gx += 5) {
					int v = pmap->index_data[gy*xl+gx];
					putchar(v<10 ? ('0'+v) : ('a'+v-10));
				}
				putchar('\n');
			}
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
