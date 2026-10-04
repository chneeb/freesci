/* pico_palmerge.h -- the LCD palette of a VGA game, merged like Sierra's SCI1.

   The PicoCalc shows VGA games through one 256-entry palette. Each view has
   its own palette; drawing a cel used to WRITE that palette into the shared
   one, so whatever the view drew over (another view's colours, a face) was
   recoloured on the next LCD push -- the face flicker in Jones in the Fast
   Lane. Sierra's interpreter merges instead (as ScummVM implements it): an
   entry in use is never overwritten. A view colour goes to the same index if
   it already holds that colour, else to another used entry with exactly that
   colour, else to its own index or the first entry that is free, and only
   when nothing is free to the closest colour in use. The cel's pixels are
   remapped to the result. The picture sets the palette and its used flags;
   the game frees entries again with kPalette(3, from, to, 1).

   Plain C, no Pico dependencies: tests/palmerge exercises it on desktop. */
#ifndef PICO_PALMERGE_H
#define PICO_PALMERGE_H

/* The palette as the LCD shows it -- the CALLER's buffer (the driver's own
   palette, so SRAM holds it once), attached with palmerge_attach -- and its
   used flags (index 0 and 255, black and SCI1's system white, always used). */
extern unsigned char (*palmerge_rgb)[3];
void palmerge_attach(unsigned char (*rgb)[3]);
int palmerge_is_used(int i);

/* Bumped whenever an entry's colour or used flag changes; a view's mapping
   stays valid while the version it was made at is current. */
unsigned short palmerge_version(void);

/* A new picture: its colours, and which of them it uses (bit i of the
   32-byte bitmap used). Entries it does not use are free; 0 and 255 are
   forced black and white. */
void palmerge_set_picture(const unsigned char rgb[256][3], const unsigned char used[32]);

/* kPalette 2/3: set or clear the used flag (bit 0 of flags) over [from, to]. */
void palmerge_set_flags(int from, int to, int flags, int on);

/* Merge n view colours, entries[k] = {index, r, g, b, mapped}: fills
   mapped (entries[k][4]). Returns nonzero if the palette changed (entries
   taken), so the caller can refresh what it derives from palmerge_rgb. */
int palmerge_merge(unsigned char (*entries)[5], int n);

#endif
