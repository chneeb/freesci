/* Decode one pic's CONTROL map two ways -- the desktop byte-per-pixel path and
   the Pico nibble-packed path -- and diff them. */
#include <sciresource.h>
#include <gfx_resource.h>
#include <gfx_tools.h>
#include <sci_memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gfxr_pic_t *pico_gfxr_init_pic(gfx_mode_t *, int, int);
void pico_gfxr_clear_pic0(gfxr_pic_t *, int);
void pico_gfxr_draw_pic01(gfxr_pic_t *, int, int, int, byte *, gfxr_pic0_params_t *,
                          int, int, gfx_pixmap_color_t *, int);
void pico_gfxr_init_static_palette(void);

static resource_mgr_t *resmgr;
#define W 320
#define H 200

static void run(gfx_mode_t *mode, int picnum)
{
	resource_t *res = scir_find_resource(resmgr, sci_pic, picnum, 0);
	gfxr_pic0_params_t style;
	gfxr_pic_t *d, *p;
	byte *packed, *aux, *unpacked;
	int i, diffs = 0, firstd = -1;

	if (!res) { printf("pic %d: missing\n", picnum); return; }
	style.line_mode = GFX_LINE_MODE_CORRECT;
	style.brush_mode = GFX_BRUSH_MODE_SCALED;
	style.pic_port_bounds = gfx_rect(0, 10, 320, 190);

	/* --- desktop reference --- */
	d = gfxr_init_pic(mode, res->id, 0);
	gfxr_clear_pic0(d, 10);
	gfxr_draw_pic01(d, 1, 0, res->size, res->data, &style, res->id, 0, NULL, 0);

	/* --- Pico packed path, mirroring sci_resmgr.c pass 2 --- */
	pico_gfxr_init_static_palette();
	p = pico_gfxr_init_pic(mode, res->id, 0);
	packed = calloc((W*H + 1) / 2, 1);
	aux    = calloc(W*H, 1);
	p->control_map->index_data = packed;
	p->control_map->nibble_packed = 1;
	p->aux_map = aux;
	p->visual_map->index_data = NULL;
	p->priority_map->index_data = NULL;
	pico_gfxr_clear_pic0(p, 10);
	pico_gfxr_draw_pic01(p, 1, 0, res->size, res->data, &style, res->id, 0, NULL, 0);

	/* --- unpack and diff --- */
	unpacked = malloc(W*H);
	for (i = 0; i < W*H; i++)
		unpacked[i] = (i & 1) ? (packed[i>>1] >> 4) : (packed[i>>1] & 0x0f);
	for (i = 0; i < W*H; i++)
		if (unpacked[i] != d->control_map->index_data[i]) {
			if (firstd < 0) firstd = i;
			diffs++;
		}
	if (diffs && getenv("DUMPMAP")) {
		int yy, xx;
		printf("  --- DESKTOP control map (8x4 blocks) ---\n");
		for (yy = 0; yy < H; yy += 4) {
			printf("    ");
			for (xx = 0; xx < W; xx += 8) {
				int c[16], k, best=0, bx, by;
				for (k=0;k<16;k++) c[k]=0;
				for (by=yy; by<yy+4 && by<H; by++)
					for (bx=xx; bx<xx+8 && bx<W; bx++)
						c[d->control_map->index_data[by*W+bx] & 15]++;
				for (k=1;k<16;k++) if (c[k]>c[best]) best=k;
				putchar(best==0?'.':(best<10?'0'+best:'a'+best-10));
			}
			printf("\n");
		}
		printf("  --- PICO control map (8x4 blocks) ---\n");
		for (yy = 0; yy < H; yy += 4) {
			printf("    ");
			for (xx = 0; xx < W; xx += 8) {
				int c[16], k, best=0, bx, by;
				for (k=0;k<16;k++) c[k]=0;
				for (by=yy; by<yy+4 && by<H; by++)
					for (bx=xx; bx<xx+8 && bx<W; bx++)
						c[unpacked[by*W+bx] & 15]++;
				for (k=1;k<16;k++) if (c[k]>c[best]) best=k;
				putchar(best==0?'.':(best<10?'0'+best:'a'+best-10));
			}
			printf("\n");
		}
	}
	printf("pic %3d: control diffs = %6d / %d", picnum, diffs, W*H);
	if (diffs) printf("   first at (%d,%d) desktop=%d pico=%d",
	                  firstd % W, firstd / W,
	                  d->control_map->index_data[firstd], unpacked[firstd]);
	printf("\n");
}

int main(int argc, char **argv)
{
	gfx_mode_t *mode = gfx_new_mode(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0);
	int i;
	if (argc > 1 && chdir(argv[1])) { printf("cannot chdir\n"); return 1; }
	if (!(resmgr = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT, 1, 1024*128))) {
		printf("no resmgr\n"); return 1;
	}
	gfxr_init_static_palette();
	for (i = 2; i < argc; i++) run(mode, atoi(argv[i]));
	return 0;
}
