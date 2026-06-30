/* One-off: decode a PQ2 pic's visual_map and print the EGA-index histogram
   inside given screen rects, to test whether the glovebox item cels collide in
   colour with the background underneath them on Pico (raw EGA index path).

   BUILD:
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
       -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
       -o /tmp/bgrect tests/bgrect.c \
       -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
   RUN:
     /tmp/bgrect ~/Downloads/pq2 1
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

static void hist_rect(gfx_pixmap_t *vm, const char *tag, int x0, int y0, int w, int h)
{
	long hist[256];
	int x, y, v;
	int xl = vm->index_xl, yl = vm->index_yl;
	for (v = 0; v < 256; v++) hist[v] = 0;
	for (y = y0; y < y0 + h && y < yl; y++)
		for (x = x0; x < x0 + w && x < xl; x++)
			hist[vm->index_data[y * xl + x]]++;
	printf("%s rect (%d,%d %dx%d) bg hist:", tag, x0, y0, w, h);
	for (v = 0; v < 256; v++)
		if (hist[v]) printf(" %d:%ld", v, hist[v]);
	printf("\n");
}

int main(int argc, char **argv)
{
	gfx_mode_t *mode;
	int picnum;
	resource_t *res;
	gfxr_pic0_params_t style;
	gfxr_pic_t *pic;
	gfx_pixmap_t *vm;

	if (argc < 3) { printf("usage: %s <gamedir> <picnum>\n", argv[0]); return 1; }
	if (chdir(argv[1])) { printf("cannot chdir %s\n", argv[1]); return 1; }
	if (!(resmgr = scir_new_resource_manager(sci_getcwd(),
	                                         SCI_VERSION_AUTODETECT, 1, 1024 * 128))) {
		fprintf(stderr, "no resources\n"); return 1;
	}
	picnum = atoi(argv[2]);
	mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);
	res = scir_find_resource(resmgr, sci_pic, picnum, 0);
	if (!res || !res->data) { printf("pic %d not found\n", picnum); return 1; }

	style.line_mode       = GFX_LINE_MODE_CORRECT;
	style.brush_mode      = GFX_BRUSH_MODE_SCALED;
	style.pic_port_bounds = gfx_rect(0, 10, 320, 190);
	pic = gfxr_init_pic(mode, res->id, 0);
	gfxr_clear_pic0(pic, 10);
	gfxr_draw_pic01(pic, 1, 0, res->size, res->data, &style, res->id, 0, NULL, 0);
	vm = pic->visual_map;
	printf("pic %d: %dx%d color_key=%d\n", picnum, vm->index_xl, vm->index_yl, vm->color_key);

	/* item1 = view54 loop0 cel1 at (52,119) 117x36; item2 = loop1 cel2 at (224,159) 96x34 */
	hist_rect(vm, "item1", 52, 119, 117, 36);
	hist_rect(vm, "item2", 224, 159, 96, 34);
	gfxr_free_pic(NULL, pic);
	return 0;
}
