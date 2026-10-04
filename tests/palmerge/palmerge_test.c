/* palmerge_test -- src/platform/pico/pico_palmerge.c against Jones in the
   Fast Lane's real palettes.

   BUILD (desktop build first):
     LIBS=$(find build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
         -Isrc/include -Ibuild -I/usr/include/SDL2 -D_REENTRANT \
         -o tests/palmerge/palmerge tests/palmerge/palmerge_test.c src/platform/pico/pico_palmerge.c \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     tests/palmerge/palmerge ~/Downloads/quest/jones [pic]

   Replays the game's pattern: the board picture (pic 11 by default) sets the
   palette, kPalette(3) frees 8..16 and 144..255 (what Jones calls, measured),
   then every view's palette is merged; every 4 views the game's unset is
   replayed, as around a dialog. Checked after every merge:
   - no entry that was in use changed colour (the flicker this replaces);
   - mapped indices are in range and point at used entries;
   - a mapping onto an entry the view took holds exactly the view's colour.
   Reported: how many colours were exact and how far off the rest were. */
#include <sciresource.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "../../src/platform/pico/pico_palmerge.h"

static int
pic_palette(resource_t *r, unsigned char rgb[256][3], unsigned char used[256])
{
	unsigned int p;

	for (p = 0; p + 2 + 1284 <= r->size; p++)
		if (r->data[p] == 0xfe && r->data[p + 1] == 0x02) {
			const unsigned char *e = r->data + p + 2 + 260;
			int c, ok = 1;
			for (c = 0; c < 256 && ok; c++)
				if (e[c * 4] > 1)
					ok = 0;
			if (!ok)
				continue;
			for (c = 0; c < 256; c++) {
				used[c] = e[c * 4];
				memcpy(rgb[c], e + c * 4 + 1, 3);
			}
			return 1;
		}
	return 0;
}

int
main(int argc, char **argv)
{
	resource_mgr_t *rm;
	resource_t *r;
	unsigned char rgb[256][3], used[256], usedbits[32];
	static unsigned char lcd[256][3]; /* the "driver's" palette palmerge works on */
	int pic = argc > 2 ? atoi(argv[2]) : 11;
	int v, views = 0, colours = 0, exact = 0, bad = 0;
	long worst = 0, sum = 0;

	chdir(argv[1]);
	rm = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT, 1, 1 << 24);
	rm->sci_version = SCI_VERSION_01_VGA;
	r = scir_find_resource(rm, sci_pic, pic, 0);
	if (!r || !pic_palette(r, rgb, used)) {
		printf("pic %d: no SET_PALETTE found\n", pic);
		return 1;
	}
	memset(usedbits, 0, sizeof(usedbits));
	for (v = 0; v < 256; v++)
		if (used[v])
			usedbits[v >> 3] |= 1 << (v & 7);
	palmerge_attach(lcd);
	palmerge_set_picture(rgb, usedbits);
	palmerge_set_flags(8, 16, 1, 0);
	palmerge_set_flags(144, 255, 1, 0);
	{
		int u = 0, c;
		for (c = 0; c < 256; c++) u += palmerge_is_used(c);
		printf("pic %d: %d entries used after the game's unsets\n", pic, u);
	}

	for (v = 0; v < 1000; v++) {
		unsigned char e[256][5], before_rgb[256][3], before_used[256], claimed[256];
		int po, c, n = 0, k;

		if (!scir_test_resource(rm, sci_view, v))
			continue;
		r = scir_find_resource(rm, sci_view, v, 0);
		if (!r || r->size < 8)
			continue;
		po = r->data[6] | (r->data[7] << 8);
		if (!po || po + 260 + 1024 > (int) r->size)
			continue;
		for (c = 0; c < 256; c++)
			if (r->data[po + 260 + c * 4]) {
				e[n][0] = c;
				memcpy(&e[n][1], r->data + po + 260 + c * 4 + 1, 3);
				n++;
			}
		if (views % 4 == 0) { /* a dialog: the game frees its pool again */
			palmerge_set_flags(8, 16, 1, 0);
			palmerge_set_flags(144, 255, 1, 0);
		}
		memcpy(before_rgb, palmerge_rgb, sizeof(before_rgb));
		for (c = 0; c < 256; c++) before_used[c] = palmerge_is_used(c);
		palmerge_merge(e, n);
		views++;
		memset(claimed, 0, sizeof(claimed));
		for (c = 0; c < 256; c++)
			if (before_used[c] && memcmp(before_rgb[c], palmerge_rgb[c], 3)) {
				printf("view %d: entry %d was in use and changed colour\n", v, c);
				bad++;
			}
		for (k = 0; k < n; k++) {
			int m = e[k][4];
			long dr = palmerge_rgb[m][0] - e[k][1], dg = palmerge_rgb[m][1] - e[k][2], db = palmerge_rgb[m][2] - e[k][3];
			long d = dr * dr + dg * dg + db * db;
			if (!palmerge_is_used(m)) {
				printf("view %d: colour %d mapped to unused entry %d\n", v, e[k][0], m);
				bad++;
			}
			if (!before_used[m] && !claimed[m]) {
				/* the first colour mapped to an entry taken in this merge
				   is the one that took it: it must hold exactly that colour
				   (later ones may share it as their closest match) */
				claimed[m] = 1;
				if (d) {
					printf("view %d: took entry %d but it does not hold the colour\n", v, m);
					bad++;
				}
			}
			colours++;
			if (!d)
				exact++;
			sum += d;
			if (d > worst)
				worst = d;
		}
	}
	printf("%d views, %d colours: %d exact (%.1f%%), worst distance^2 %ld, mean %.1f; %d violations\n",
	       views, colours, exact, 100.0 * exact / colours, worst, (double) sum / colours, bad);
	return bad ? 1 : 0;
}
