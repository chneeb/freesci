/* viewdiff -- the VGA view path on the Pico, proven offline (branch pico-sci1).

   For every view of an SCI01 VGA game (Jones in the Fast Lane):
     desktop: stock decompress01 into res->data, gfxr_draw_view1 from memory.
     Pico:    pico_decompress01_to_psram (decrypt3 resumed in 16 KB stages)
              into a PSRAM stub, then the HAVE_PICO copy of sci_view_1.c's
              gfxr_draw_view1_psram, which reads through the 512-byte cache and
              sends each cel to PSRAM as soon as it is decoded (via the 32 KB
              priority scratch).
   Checked: the decompressed bytes, and per cel the geometry, hotspot, colour
   key and every pixel (read back from the PSRAM stub). Exit 1 on any mismatch.

   BUILD (desktop build + tests/decompdiff objects first, see that README):
     cd tests/viewdiff
     cp ../../src/gfx/resource/sci_view_1.c pico_view1.c
     CF="-fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 -I../../src/include -I../../build -I/usr/include/SDL2 -D_REENTRANT"
     gcc -c $CF -DHAVE_PICO=1 -DPICO_STREAM_DECOMPRESS=1 -include viewprefix.h \
         -I. -I../picodiff -o pico_view1.o pico_view1.c
     LIBS=$(find ../../build -name '*.a' | tr '\n' ' ')
     gcc -c $CF -DHAVE_PICO=1 -include stdint.h -o pico_pixmap.o pico_pixmap.c
     cp ../../src/scicore/decompress01.c pico_decomp01.c   # HAVE_PICO: LZW tables in the scratch
     gcc -c $CF -DHAVE_PICO=1 -DPICO_STREAM_DECOMPRESS=1 -DPICO_STREAM_METHODS=7 \
         -include decomp01prefix.h -include stdint.h -o pico_decomp01.o pico_decomp01.c
     gcc $CF -o viewdiff viewdiff_main.c pico_view1.o pico_pixmap.o pico_decomp01.o \
         ../picodiff/psram_stub.c ../decompdiff/stream_decomp.o \
         -Wl,--wrap=gfx_new_pixmap \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     ./viewdiff ~/Downloads/quest/jones
*/
#include <sciresource.h>
#include <gfx_resource.h>
#include <gfx_tools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>

gfxr_view_t *gfxr_draw_view1(int id, byte *resource, int size, gfx_pixmap_color_t *static_pal, int static_pal_nr);
gfxr_view_t *gfxr_draw_view1_psram(int id, uint32_t addr, int size, gfx_pixmap_color_t *static_pal, int static_pal_nr);
int pico_decompress01_to_psram(int resh, int method, unsigned int complength, int size,
			       uint32_t addr, guint8 *stage, int stage_size);
uint32_t psram_alloc(size_t n);
void psram_reset(void);
void psram_load(uint32_t a, uint8_t *d, size_t n);

byte *g_pico_priority_scratch;   /* what the device allocates at boot */

/* pico_pixmap.c (a HAVE_PICO compile): the pixmap wrap and this accessor. */
int vd_cel_psram(gfx_pixmap_t *p, uint32_t *addr);
void *vd_view_pal(gfxr_view_t *v);
void *vd_cel_pal(gfx_pixmap_t *p);
void pico_view_cels_reuse(uint32_t base);
uint32_t pico_view_cels_reuse_end(void);
gfx_pixmap_color_t *gfxr_read_pal1(int id, int *colors_nr, byte *resource, int size);
typedef struct { unsigned short n; unsigned char e[][4]; } vd_pal_insert_t; /* gfx_pal_insert_t */
static guint8 stage[16384];      /* the decompress scratch the device stages through */
static gfx_pixmap_color_t *g_spal;
static int g_spal_nr;

int
main(int argc, char **argv)
{
	resource_mgr_t *rm;
	int i, views = 0, cels = 0, bad = 0, reused = 0, inserts = 0;

	if (argc < 2) { fprintf(stderr, "usage: %s <gamedir>\n", argv[0]); return 2; }
	setvbuf(stdout, NULL, _IONBF, 0);
	g_pico_priority_scratch = malloc((GFXR_AUX_MAP_SIZE + 1) >> 1);
	if (chdir(argv[1])) return 1;
	rm = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT, 1, 8 * 1024 * 1024);
	if (!rm) return 1;
	rm->sci_version = SCI_VERSION_01_VGA; /* what main.c's hash detection gives Jones */
	{	/* the static palette, as gfxr_interpreter_get_view passes it on the device */
		resource_t *pal = scir_find_resource(rm, sci_palette, 999, 0);
		if (pal && pal->data)
			g_spal = gfxr_read_pal1(pal->id, &g_spal_nr, pal->data, pal->size);
	}

	for (i = 0; i < 1000; i++) {
		resource_t *r = scir_test_resource(rm, sci_view, i);
		gfxr_view_t *d, *p;
		unsigned char hdr[8], *pbytes;
		int fh, l, c, clen, dsz, method, rc;
		uint32_t addr;

		if (!r || r->source->source_type != RESSOURCE_TYPE_VOLUME)
			continue;
		/* Pico side first (the resource is not loaded yet, as on the device) */
		fh = open(r->source->location.file.name, O_RDONLY);
		lseek(fh, r->file_offset, SEEK_SET);
		read(fh, hdr, 8);
		clen = hdr[2] | (hdr[3] << 8); dsz = hdr[4] | (hdr[5] << 8); method = hdr[6] | (hdr[7] << 8);
		psram_reset();
		addr = psram_alloc(dsz);
		rc = pico_decompress01_to_psram(fh, method, clen - 4, dsz, addr, stage, sizeof(stage));
		{	/* fd must end where a flat read would have */
			long pos = lseek(fh, 0, SEEK_CUR);
			if (rc == 0 && pos != (long)(r->file_offset + 8 + clen - 4)) {
				printf("  view %d: fd at %ld, expected %ld\n", i, pos, (long)(r->file_offset + 8 + clen - 4));
				bad++;
			}
		}
		close(fh);
		if (rc) { printf("  view %d: method %d not handled (rc %d)\n", i, method, rc); continue; }

		r = scir_find_resource(rm, sci_view, i, 0);
		if (!r || !r->data) { printf("  view %d: stock load failed\n", i); bad++; continue; }
		views++;
		pbytes = malloc(dsz);
		psram_load(addr, pbytes, dsz);
		if (dsz != (int) r->size || memcmp(pbytes, r->data, dsz)) {
			printf("  view %d: decompressed bytes differ (size %d vs %d)\n", i, dsz, r->size);
			bad++; free(pbytes); continue;
		}
		free(pbytes);

		d = gfxr_draw_view1(i, r->data, r->size, NULL, 0);
		{
			uint32_t cel_base = psram_alloc(0);
			p = gfxr_draw_view1_psram(i, addr, dsz, g_spal, g_spal_nr);
			if (p && getenv("NOREUSE") == NULL) {
				/* Re-decode into the same PSRAM, as after an eviction: the
				   cels must land at the same addresses with the same bytes. */
				uint32_t used = psram_alloc(0) - cel_base, end;
				gfxr_view_t *q;
				pico_view_cels_reuse(cel_base);
				q = gfxr_draw_view1_psram(i, addr, dsz, g_spal, g_spal_nr);
				end = pico_view_cels_reuse_end();
				if (!q || end - cel_base != used || psram_alloc(0) - cel_base != used) {
					printf("  view %d: re-decode used %u bytes, first decode %u (arena moved by %u)\n",
					       i, end - cel_base, used, psram_alloc(0) - cel_base - used);
					bad++;
				} else {
					int l2, c2;
					for (l2 = 0; l2 < p->loops_nr; l2++)
						for (c2 = 0; c2 < p->loops[l2].cels_nr; c2++) {
							uint32_t a1, a2;
							vd_cel_psram(p->loops[l2].cels[c2], &a1);
							vd_cel_psram(q->loops[l2].cels[c2], &a2);
							if (a1 != a2) {
								printf("  view %d %d/%d: re-decoded cel at %u, was %u\n", i, l2, c2, a2, a1);
								bad++;
							}
						}
					reused++;
				}
			}
		}
		if (!d || !p || d->loops_nr != p->loops_nr) {
			printf("  view %d: decode failed or loop count differs (%p %p)\n", i, (void *)d, (void *)p);
			bad++; continue;
		}
		{	/* The Pico keeps only the USED palette entries (byte 0 of each
			   4-byte entry) as an insert list and points colors at the static
			   palette; each must match the desktop palette and the raw flags. */
			vd_pal_insert_t *pi = (vd_pal_insert_t *) vd_view_pal(p);
			int po = r->data[6] | (r->data[7] << 8), c, n = 0, k = 0;

			if (po && !pi) { printf("  view %d: no insert list\n", i); bad++; }
			else if (pi) {
				inserts++;
				for (c = 0; c < 256; c++)
					if (r->data[po + 260 + c * 4]) {
						if (k >= pi->n || pi->e[k][0] != c
						    || pi->e[k][1] != d->colors[c].r || pi->e[k][2] != d->colors[c].g
						    || pi->e[k][3] != d->colors[c].b) {
							printf("  view %d: insert entry %d (index %d) differs\n", i, k, c);
							bad++;
							break;
						}
						k++; n++;
					}
				if (k != pi->n) { printf("  view %d: insert list has %d entries, palette %d used\n", i, pi->n, n); bad++; }
				if (p->colors != g_spal) { printf("  view %d: colors is not the static palette\n", i); bad++; }
			}
		}
		for (l = 0; l < d->loops_nr; l++) {
			if (d->loops[l].cels_nr != p->loops[l].cels_nr) {
				printf("  view %d loop %d: cel count differs\n", i, l); bad++; continue;
			}
			for (c = 0; c < d->loops[l].cels_nr; c++) {
				gfx_pixmap_t *a = d->loops[l].cels[c], *b = p->loops[l].cels[c];
				int n = a->index_xl * a->index_yl;
				byte *px;
				cels++;
				if (a->index_xl != b->index_xl || a->index_yl != b->index_yl
				    || a->xoffset != b->xoffset || a->yoffset != b->yoffset
				    || a->color_key != b->color_key) {
					printf("  view %d %d/%d: geometry/key differs\n", i, l, c); bad++; continue;
				}
				uint32_t caddr;
				if (vd_cel_pal(b) != vd_view_pal(p)) {
					printf("  view %d %d/%d: cel does not carry the view's insert list\n", i, l, c); bad++;
				}
				if (b->index_data || !vd_cel_psram(b, &caddr)) {
					printf("  view %d %d/%d: cel not offloaded to PSRAM\n", i, l, c); bad++; continue;
				}
				px = malloc(n ? n : 1);
				psram_load(caddr, px, n);
				if (memcmp(px, a->index_data, n)) {
					printf("  view %d %d/%d: pixels differ\n", i, l, c); bad++;
				}
				free(px);
			}
		}
	}
	printf("\n== %d views, %d cels checked, %d palette insert lists, %d re-decoded in place, %d mismatches ==\n",
	       views, cels, inserts, reused, bad);
	return bad != 0;
}
