/* resdiff -- the Pico resource manager's scratch ownership, proven offline
   (branch pico-sci1).

   On the Pico an SCI01 script resource is decompressed into the shared 16 KB
   decompress scratch and OWNS it until the next user takes it (a pic or view
   decompress, or the PSRAM view staging), which evicts the script first; the
   script's next access reloads it. This builds the Pico resource manager
   (resource.c + resource_map.c + resource_patch.c + the decompressors, all
   with HAVE_PICO) on the desktop, records every script's bytes, then loads
   scripts interleaved with pics, views and PSRAM view loads in a scrambled
   order and checks that every script read returns exactly the recorded bytes
   and that a displaced script was really evicted (data NULL) before its
   reload. Exit 1 on any mismatch.

   BUILD (desktop build first; the Pico copies are self-contained, so nothing
   needs renaming -- the desktop libscicore members are never pulled in):
     cd tests/resdiff
     for f in resource resource_map resource_patch decompress0 decompress01 decompress1 decompress11; do
       cp ../../src/scicore/$f.c p_$f.c
     done
     CF="-fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 -I../../src/include -I../../build \
         -I../../src/scicore -I/usr/include/SDL2 -D_REENTRANT -DHAVE_PICO=1 -DPICO_STREAM_DECOMPRESS=1 \
         -DPICO_STREAM_METHODS=7 -DPICO_VOLUME_CACHE=1 -include stdint.h -iquote ../../src/platform/pico"
     for f in p_*.c; do gcc -c $CF -o ${f%.c}.o $f; done
     gcc -c $CF -o resdiff_main.o resdiff_main.c
     LIBS=$(find ../../build -name '*.a' | tr '\n' ' ')
     gcc -o resdiff resdiff_main.o p_*.o ../picodiff/psram_stub.c \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     ./resdiff ~/Downloads/quest/jones
*/
#include <sciresource.h>
#include <sci_memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What the device provides elsewhere */
unsigned char *g_pico_decompress_scratch;
unsigned char *g_pico_priority_scratch;
unsigned long long pico_perf_us(void) { return 0; }
void pico_io_enable_fastseek(int fd) { (void) fd; }
void scir_evict_resource_data(resource_mgr_t *mgr, resource_t *res);
int scir_pico_load_to_psram(resource_mgr_t *mgr, int type, int number,
			    uint32_t addr, unsigned int max_size, int *size);

#define MAXN 1000
static unsigned char *ref[MAXN];
static int refsize[MAXN];

int
main(int argc, char **argv)
{
	resource_mgr_t *mgr;
	int nr, i, scripts = 0, checks = 0, evictions = 0, bad = 0, others = 0;
	int order[MAXN], n = 0;

	if (argc < 2) { fprintf(stderr, "usage: %s <gamedir>\n", argv[0]); return 2; }
	setvbuf(stdout, NULL, _IONBF, 0);
	g_pico_decompress_scratch = malloc(PICO_DECOMPRESS_SCRATCH_SIZE);
	g_pico_priority_scratch = malloc(32000);
	if (chdir(argv[1])) return 1;
	mgr = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_01_VGA, 1, 32 * 1024);
	if (!mgr) { fprintf(stderr, "no resource manager\n"); return 1; }

	/* reference bytes, one script at a time */
	for (nr = 0; nr < MAXN; nr++) {
		resource_t *r = scir_find_resource(mgr, sci_script, nr, 0);
		if (!r || !r->data)
			continue;
		ref[nr] = malloc(r->size);
		memcpy(ref[nr], r->data, r->size);
		refsize[nr] = r->size;
		order[n++] = nr;
		scripts++;
	}
	srand(1);
	for (i = n - 1; i > 0; i--) { int j = rand() % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }

	/* interleave: script, then something that takes the scratch, then the
	   script again (must be evicted and reload identically) */
	for (i = 0; i < 3 * n; i++) {
		int s = order[i % n], kind = i % 3, other = rand() % 1000;
		resource_t *r = scir_find_resource(mgr, sci_script, s, 0), *o = NULL;
		int took = 0; /* the other load really used the scratch */

		checks++;
		if (!r || !r->data || r->size != (unsigned) refsize[s] || memcmp(r->data, ref[s], refsize[s])) {
			printf("  script %d: wrong bytes on read %d\n", s, i); bad++; continue;
		}
		if (kind == 0) {        /* a view: pico_decompress_alloc may hand it the scratch */
			while (other < MAXN && !scir_test_resource(mgr, sci_view, other)) other++;
			if (other < MAXN && (o = scir_find_resource(mgr, sci_view, other, 0))) {
				took = (o->data == g_pico_decompress_scratch);
				scir_evict_resource_data(mgr, o);     /* as gfxr_interpreter_get_view does */
			}
		} else if (kind == 1) { /* a pic */
			while (other < MAXN && !scir_test_resource(mgr, sci_pic, other)) other++;
			if (other < MAXN && (o = scir_find_resource(mgr, sci_pic, other, 0))) {
				took = (o->data == g_pico_decompress_scratch);
				scir_evict_resource_data(mgr, o);
			}
		} else {                /* a PSRAM view load: stages through the scratch */
			int vs;
			while (other < MAXN && !scir_test_resource(mgr, sci_view, other)) other++;
			if (other < MAXN && scir_pico_load_to_psram(mgr, sci_view, other, 0x7A0000u, 0x10000u, &vs) == 0)
				o = r, took = 1;  /* staged through the scratch */
		}
		if (o && took) {
			others++;
			if (r->data == g_pico_decompress_scratch) {
				printf("  script %d still points at the scratch after it was taken (kind %d)\n", s, kind);
				bad++;
			}
			if (!r->data)
				evictions++;
		}
		/* read it again: must reload correctly */
		r = scir_find_resource(mgr, sci_script, s, 0);
		checks++;
		if (!r || !r->data || memcmp(r->data, ref[s], refsize[s])) {
			printf("  script %d: wrong bytes after reload (read %d)\n", s, i); bad++;
		}
	}
	printf("\n== %d scripts, %d reads, %d scratch takeovers, %d evictions of the owning script, %d mismatches ==\n",
	       scripts, checks, others, evictions, bad);
	return bad != 0;
}
