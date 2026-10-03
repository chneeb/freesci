/***************************************************************************
 int_hashmap.h Copyright (C) 2001 Christoph Reichenbach


 This program may be modified and copied freely according to the terms of
 the GNU general public license (GPL), as long as the above copyright
 notice and the licensing information contained herein are preserved.

 Please refer to www.gnu.org for licensing details.

 This work is provided AS IS, without warranty of any kind, expressed or
 implied, including but not limited to the warranties of merchantibility,
 noninfringement, and fitness for a specific purpose. The author will not
 be held liable for any damage caused by this work or derivatives of it.

 By using this source code, you agree to the licensing terms as stated
 above.


 Please contact the maintainer for bug reports or inquiries.

 Current Maintainer:

    Christoph Reichenbach (CR) <jameson@linuxgames.com>

***************************************************************************/

#ifndef _INT_HASHMAP_H_
#define _INT_HASHMAP_H_

/* Assumes that the ints are relatively evenly distributed */

/* The savegame format: 256 bucket chains, chain b holding the keys with
   (key & 0xff) == b (savegame.c writes and reads exactly these). */
#define DCS_INT_HASH_MAX 255

#ifdef SCI_COMPACT_HASHMAPS
/* PIO Pico: 32 buckets in memory instead of 256 -- every loaded script has
   one of these maps (its object indices), and 256 buckets are 1 KB each.
   The in-memory hash MUST be a function of (key & 0xff) alone: savegame.c
   rebuilds each format chain from the one memory bucket holding it. Buckets
   0..31 of 33, like the original's 0..255 of 256 plus one: apply_to_*
   (hashmap.c) never visits the last bucket. */
#  define INT_HASH_MEM(x) ((((x) ^ ((x) >> 3)) & 31))
#  define INT_HASH_MEM_MAX 32
#else
#  define INT_HASH_MEM(x) ((x) & 0xff)
#  define INT_HASH_MEM_MAX DCS_INT_HASH_MAX
#endif

#define HASH_MAX INT_HASH_MEM_MAX
#define COMP(x, y) ((x)-(y))
#define HASH(x) INT_HASH_MEM(x)
#undef MUST_FREE

#include "hashmap.h"
DECLARE_STRUCTS(int)
DECLARE_FUNCTIONS(int)

#ifndef BUILD_MAP_FUNCTIONS
#  undef HASH_MAX
#  undef COMP
#  undef HASH
#endif

/* see hashmap.h for descriptions of these functions */
int_hash_map_ptr
new_int_hash_map(void);

void
free_int_hash_map(int_hash_map_ptr);

int
int_hash_map_check_value(int_hash_map_ptr, int value, char add_p, char *added);

int
int_hash_map_remove_value(int_hash_map_ptr, int value);

#endif /* _INT_HASHMAP_H_ */
