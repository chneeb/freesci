/* Desktop reference harness for the PQ2 (SCI0) empty-view-cel bug.

   Decodes SCI0 view cels through the SAME shared engine path the Pico uses
   (gfxr_draw_view0 -> gfxr_draw_loop0 -> gfxr_draw_cel0) and reports, per cel,
   how many pixels are OPAQUE (index_data[i] != color_key).  The on-device
   [pblit] probe reports cels coming back opaque=0 (all transparent) for PQ2 —
   this harness establishes whether that emptiness already exists at DECODE time
   on the desktop (shared code, no PSRAM), which would point at the cel DATA /
   decoder; or whether desktop decodes the same cels opaque>0, which moves the
   bug onto the Pico-only scratch-borrow + PSRAM round-trip.

   gfxr_draw_cel0 sets color_key = 255 ("larger than 15"), and maps every run
   whose colour == resource[6] to 255.  So an opaque pixel is index_data[i] != 255.
   A cel that decodes to opaque=0 is genuinely all-transparent at decode time.

   This is desktop-only: HAVE_PICO is NOT defined here, so gfxr_draw_cel0 keeps
   index_data resident (no psram_store / index_data=NULL), which is exactly what
   we want to inspect.

   The two device-log culprits were dims 12x35 (pri 6) and 4x32 (pri 10); pass
   the matching view numbers as args, or pass none to scan every view and report
   only cels that decode opaque=0 (plus a per-view summary).

   BUILD (against the desktop static libs; build the desktop target first):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
       -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
       -o /tmp/viewdump tests/viewdump.c \
       -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl

   RUN (in, or pointed at, the PQ2 game dir):
     /tmp/viewdump ~/Downloads/pq2            # scan all views, report empties
     /tmp/viewdump ~/Downloads/pq2 5 17 132   # dump these view numbers in full
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

/* Count opaque pixels (index != color_key) and build a value histogram. */
static long cel_opaque(gfx_pixmap_t *cel, int *only_key_out, long hist[256])
{
	long opaque = 0;
	int i, n = cel->index_xl * cel->index_yl;
	int key = cel->color_key;
	int nonkey_values = 0, v;

	for (v = 0; v < 256; v++) hist[v] = 0;
	for (i = 0; i < n; i++) {
		byte px = cel->index_data[i];
		hist[px]++;
		if (px != key) opaque++;
	}
	for (v = 0; v < 256; v++)
		if (v != key && hist[v]) nonkey_values++;
	*only_key_out = (nonkey_values == 0);
	return opaque;
}

static void dump_view(int viewnum, int verbose)
{
	resource_t *res = scir_find_resource(resmgr, sci_view, viewnum, 0);
	gfxr_view_t *view;
	int loop, any_empty = 0;

	if (!res || !res->data) {
		if (verbose) printf("view %d: NOT FOUND\n", viewnum);
		return;
	}

	if (verbose) {
		/* Raw header so a mis-parsed loop/cel offset table is visible vs a
		   genuinely blank view: SCI0 view header is loops_nr(1) at +0, the
		   mirror bitmask, palette ofs at +6, then the loop offset table at
		   +8 (2 bytes each).  Each loop: cels_nr(2) at its offset, then a
		   cel offset table; each cel: xl(2) yl(2) xhot yhot color_key, data. */
		int b, n = res->size < 48 ? res->size : 48;
		printf("view %d: res->id=%d size=%d  raw[0..%d]:", viewnum, res->id, res->size, n - 1);
		for (b = 0; b < n; b++) printf(" %02x", res->data[b]);
		printf("\n");
		printf("    header: loops_nr=%d palette_ofs=%d  loop offsets:",
		       res->data[0], get_int_16(res->data + 6));
		{
			int l, ln = res->data[0];
			for (l = 0; l < ln && (8 + (l << 1) + 1) < res->size; l++)
				printf(" %d", get_uint_16(res->data + 8 + (l << 1)));
		}
		printf("\n");
	}

	/* SCI0: palette = -1 (gfxr_interpreter_get_view forces it when
	   version < SCI_VERSION_01, which is true for both PQ2 and SQ3). */
	view = gfxr_draw_view0(res->id, res->data, res->size, -1);
	if (!view) {
		printf("view %d (res->id=%d size=%d): DECODE FAILED (gfxr_draw_view0 returned NULL)\n",
		       viewnum, res->id, res->size);
		return;
	}

	for (loop = 0; loop < view->loops_nr; loop++) {
		int c;
		for (c = 0; c < view->loops[loop].cels_nr; c++) {
			gfx_pixmap_t *cel = view->loops[loop].cels[c];
			long hist[256];
			int only_key = 0;
			long opaque;

			if (!cel) {
				printf("view %d loop %d cel %d: NULL cel\n", viewnum, loop, c);
				any_empty = 1;
				continue;
			}
			opaque = cel_opaque(cel, &only_key, hist);

			if (opaque == 0) any_empty = 1;

			if (verbose || opaque == 0) {
				int v;
				printf("view %d loop %d cel %d: %dx%d color_key=%d "
				       "xoff=%d yoff=%d opaque=%ld%s\n",
				       viewnum, loop, c, cel->index_xl, cel->index_yl,
				       cel->color_key, cel->xoffset, cel->yoffset,
				       opaque, (opaque == 0) ? "  <-- EMPTY" : "");
				/* For empty cels (and in verbose mode) print the value
				   histogram so a "only the key colour was written" decode
				   is visible at a glance. */
				if (opaque == 0 || verbose) {
					printf("    hist:");
					for (v = 0; v < 256; v++)
						if (hist[v]) printf(" %d:%ld%s", v, hist[v],
						                    (v == cel->color_key) ? "(key)" : "");
					printf("%s\n", only_key ? "   [ONLY key colour present]" : "");
				}
			}
		}
	}

	if (verbose && !any_empty)
		printf("view %d: all cels opaque>0 (decode produced content)\n", viewnum);

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
	printf("sci_version=%d  resources_nr=%d\n",
	       resmgr->sci_version, resmgr->resources_nr);

	if (argc > 2) {
		/* Explicit view numbers: full verbose dump of each. */
		for (i = 2; i < argc; i++)
			dump_view(atoi(argv[i]), 1);
	} else {
		/* Scan every view; report only cels that decode opaque=0. */
		long scanned = 0;
		printf("scanning all views for opaque=0 cels...\n");
		for (i = 0; i < 1024; i++) {
			resource_t *res = scir_find_resource(resmgr, sci_view, i, 0);
			if (res && res->data) { scanned++; dump_view(i, 0); }
		}
		printf("scanned %ld views\n", scanned);
	}

	return 0;
}
