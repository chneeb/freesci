/* songdiff -- SCI1 songs parked in PSRAM, proven offline (branch pico-sci1).

   Every sound resource of a game is played twice, through the stock song
   iterator (desktop, the song in memory; SCI0 or SCI1 iterator by the game's
   version) and through a copy of iterator.c
   built with HAVE_PICO + PICO_PSRAM_SONGS, where a large song lives in a
   (stub) PSRAM slot and is read through the windows. For each of the 8 device
   play masks the two must produce the same event stream: return value, MIDI
   bytes and result, step for step, until the song finishes or 5000 events
   (songs loop). Also reports how many songs were parked and how many stayed in
   SRAM because they carry a digital sample.

   BUILD (desktop build first):
     cd tests/songdiff
     cp ../../src/sfx/iterator.c pico_iterator.c
     CF="-fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 -I../../src/include -I../../build -I/usr/include/SDL2 -D_REENTRANT"
     gcc -c $CF -DHAVE_PICO=1 -DPICO_PSRAM_SONGS=1 -include songprefix.h \
         -iquote ../../src/platform/pico -o pico_iterator.o pico_iterator.c
     LIBS=$(find ../../build -name '*.a' | tr '\n' ' ')
     gcc $CF -o songdiff songdiff_main.c pico_iterator.o ../picodiff/psram_stub.c \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     ./songdiff ~/Downloads/quest/jones
*/
#include <sciresource.h>
#include <sfx_iterator.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

song_iterator_t *pico_songit_new(unsigned char *data, unsigned int size, int type, songit_id_t id);
int pico_songit_next(song_iterator_t **it, unsigned char *buf, int *result, int mask);
int pico_songit_handle_message(song_iterator_t **it_reg, song_iterator_message_t msg);
song_iterator_message_t pico_songit_make_message(songit_id_t id, int recipient, int type, int a1, int a2);
void pico_songit_free(song_iterator_t *it);

/* PSRAM song slots, as psram_alloc.c lays them out (data in psram_stub.c) */
#define SLOT_BASE 0x710000u
#define SLOT_SIZE 65536u
#define SLOTS 8
static int slot_refs[SLOTS], parked = 0;
uint32_t psram_song_alloc(size_t bytes)
{
	int i;
	if (bytes > SLOT_SIZE) return 0xffffffffu;
	for (i = 0; i < SLOTS; i++)
		if (!slot_refs[i]) { slot_refs[i] = 1; parked++; return SLOT_BASE + i * SLOT_SIZE; }
	return 0xffffffffu;
}
void psram_song_incref(uint32_t a) { slot_refs[(a - SLOT_BASE) / SLOT_SIZE]++; }
void psram_song_free(uint32_t a) { slot_refs[(a - SLOT_BASE) / SLOT_SIZE]--; }
int psram_song_slots_used(void) { int i, n = 0; for (i = 0; i < SLOTS; i++) n += !!slot_refs[i]; return n; }

int
main(int argc, char **argv)
{
	resource_mgr_t *rm;
	int i, songs = 0, bad = 0, runs = 0, type;
	long events = 0;

	if (argc < 2) { fprintf(stderr, "usage: %s <gamedir>\n", argv[0]); return 2; }
	setvbuf(stdout, NULL, _IONBF, 0);
	if (chdir(argv[1])) return 1;
	rm = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT, 1, 8 * 1024 * 1024);
	if (!rm) return 1;
	if (scir_test_resource(rm, sci_palette, 999))
		rm->sci_version = SCI_VERSION_01_VGA; /* as main.c's hash detection gives Jones */
	/* SCI0 songs through the SCI0 iterator (their single-window PSRAM path),
	   SCI01+ through the SCI1 one -- as ksound.c chooses. */
	type = (rm->sci_version >= SCI_VERSION_01) ? SCI_SONG_ITERATOR_TYPE_SCI1 : SCI_SONG_ITERATOR_TYPE_SCI0;
	printf("%s song iterator\n", type == SCI_SONG_ITERATOR_TYPE_SCI1 ? "SCI1" : "SCI0");

	for (i = 0; i < 1000; i++) {
		resource_t *r;
		int mask;

		if (!scir_test_resource(rm, sci_sound, i)) continue;
		r = scir_find_resource(rm, sci_sound, i, 0);
		if (!r || !r->data) continue;
		songs++;
		for (mask = 1; mask < 0x100; mask <<= 1) {
			int step, parked_before = parked;
			song_iterator_t *a = songit_new(r->data, r->size, type, i);
			song_iterator_t *b = pico_songit_new(r->data, r->size, type, i);

			if (!a || !b) { printf("  sound %d: iterator creation failed (%p %p)\n", i, (void *)a, (void *)b); bad++; break; }
			songit_handle_message(&a, songit_make_message(i, SIMSG_SET_PLAYMASK(mask)));
			pico_songit_handle_message(&b, pico_songit_make_message(i, SIMSG_SET_PLAYMASK(mask)));
			runs++;
			for (step = 0; step < 5000; step++) {
				unsigned char ba[16], bb[16];
				int ra = 0, rb = 0, xa, xb;
				memset(ba, 0, sizeof ba); memset(bb, 0, sizeof bb);
				xa = songit_next(&a, ba, &ra, IT_READER_MASK_ALL);
				xb = pico_songit_next(&b, bb, &rb, IT_READER_MASK_ALL);
				events++;
				if (xa != xb || ra != rb || memcmp(ba, bb, sizeof ba)) {
					printf("  sound %d mask %02x: step %d differs (stock %d/%d %02x %02x %02x, pico %d/%d %02x %02x %02x)\n",
					       i, mask, step, xa, ra, ba[0], ba[1], ba[2], xb, rb, bb[0], bb[1], bb[2]);
					bad++;
					break;
				}
				if (xa == SI_FINISHED || !a || !b)
					break;
			}
			if (a) songit_free(a);
			if (b) pico_songit_free(b);
			if (mask == 1 && getenv("VERBOSE"))
				printf("  sound %d: %u bytes, %s\n", i, r->size, parked > parked_before ? "parked in PSRAM" : "in SRAM");
		}
	}
	printf("\n== %d songs x 8 device masks (%d runs, %ld events), %d PSRAM parkings, %d mismatches; slots still held: %d ==\n",
	       songs, runs, events, parked, bad, psram_song_slots_used());
	return bad != 0;
}
