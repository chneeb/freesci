/* pico_palmerge.c -- see pico_palmerge.h. */
#include <string.h>
#include "pico_palmerge.h"

unsigned char (*palmerge_rgb)[3] = NULL;
static unsigned char used_bits[32];
static unsigned short palmerge_ver = 1;

#define USED(i) (used_bits[(i) >> 3] & (1 << ((i) & 7)))
#define SET_USED(i) (used_bits[(i) >> 3] |= (unsigned char)(1 << ((i) & 7)))
#define CLR_USED(i) (used_bits[(i) >> 3] &= (unsigned char)~(1 << ((i) & 7)))

void
palmerge_attach(unsigned char (*rgb)[3])
{
	palmerge_rgb = rgb;
}

int
palmerge_is_used(int i)
{
	return USED(i) != 0;
}

unsigned short
palmerge_version(void)
{
	return palmerge_ver;
}

static void
palmerge_changed(void)
{
	if (++palmerge_ver == 0) /* 0 never matches a fresh list's stamp */
		palmerge_ver = 1;
}

static void
palmerge_system_colours(void)
{
	palmerge_rgb[0][0] = palmerge_rgb[0][1] = palmerge_rgb[0][2] = 0;
	palmerge_rgb[255][0] = palmerge_rgb[255][1] = palmerge_rgb[255][2] = 255;
	SET_USED(0);
	SET_USED(255);
}

void
palmerge_set_picture(const unsigned char rgb[256][3], const unsigned char used[32])
{
	if (palmerge_rgb != rgb)
		memcpy(palmerge_rgb, rgb, 256 * 3);
	memcpy(used_bits, used, sizeof(used_bits));
	palmerge_system_colours();
	palmerge_changed();
}

void
palmerge_set_flags(int from, int to, int flags, int on)
{
	int i, changed = 0;

	if (!(flags & 1))
		return;
	if (from < 1)
		from = 1;
	if (to > 254)
		to = 254; /* black and white stay used */
	for (i = from; i <= to; i++)
		if (!USED(i) != !on) {
			if (on)
				SET_USED(i);
			else
				CLR_USED(i);
			changed = 1;
		}
	if (changed)
		palmerge_changed();
}

static int
same_colour(int i, const unsigned char *c)
{
	return palmerge_rgb[i][0] == c[0] && palmerge_rgb[i][1] == c[1] && palmerge_rgb[i][2] == c[2];
}

int
palmerge_merge(unsigned char (*entries)[5], int n)
{
	int k, changed = 0;

	for (k = 0; k < n; k++) {
		int i = entries[k][0], j, best = -1;
		long best_d = -1;
		const unsigned char *c = &entries[k][1];

		if (i == 0 || i == 255) { /* system black and white */
			entries[k][4] = i;
			continue;
		}
		if (!USED(i)) { /* own index free: take it */
			memcpy(palmerge_rgb[i], c, 3);
			SET_USED(i);
			entries[k][4] = i;
			changed = 1;
			continue;
		}
		if (same_colour(i, c)) { /* already there */
			entries[k][4] = i;
			continue;
		}
		for (j = 1; j < 255; j++) /* exact colour in use elsewhere */
			if (USED(j) && same_colour(j, c))
				break;
		if (j < 255) {
			entries[k][4] = j;
			continue;
		}
		for (j = 1; j < 255; j++) /* a free entry */
			if (!USED(j))
				break;
		if (j < 255) {
			memcpy(palmerge_rgb[j], c, 3);
			SET_USED(j);
			entries[k][4] = j;
			changed = 1;
			continue;
		}
		for (j = 0; j < 256; j++) { /* nothing free: the closest colour */
			long dr = palmerge_rgb[j][0] - c[0], dg = palmerge_rgb[j][1] - c[1], db = palmerge_rgb[j][2] - c[2];
			long d = dr * dr + dg * dg + db * db;
			if (best_d < 0 || d < best_d) {
				best_d = d;
				best = j;
			}
		}
		entries[k][4] = best;
	}
	if (changed)
		palmerge_changed();
	return changed;
}
