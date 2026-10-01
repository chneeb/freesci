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

#ifndef _REG_T_HASHMAP_H_
#define _REG_T_HASHMAP_H_

#define DCS_REGT_HASH_MAX 512

#include "vm.h"
#ifdef HASH_MAX
#  undef HASH_MAX
#  undef COMP
#  undef HASH
#endif

#ifdef SCI_COMPACT_HASHMAPS
/* PIO Pico: the GC builds two of these per run and the second one was the
   allocation that failed in Jones in the Fast Lane (2,060 B). Never saved.
   128 buckets, hash 0..127: apply_to_* (hashmap.c) skips bucket HASH_MAX,
   exactly as the original's 0x1ff never reaches bucket 512. */
#  define HASH_MAX 128
#  define HASH(x) (((x.segment << 3) | x.offset) & 0x7f)
#else
#  define HASH_MAX DCS_REGT_HASH_MAX
#  define HASH(x) (((x.segment << 3) | x.offset) & 0x1ff)
#endif
#define COMP(x, y) compare_reg_t(x, y)
#undef MUST_FREE

#include "hashmap.h"
DECLARE_STRUCTS(reg_t)
DECLARE_FUNCTIONS(reg_t)

#ifndef BUILD_MAP_FUNCTIONS
#  undef HASH_MAX
#  undef COMP
#  undef HASH
#endif

/* see hashmap.h for descriptions of these functions */
reg_t_hash_map_ptr
new_reg_t_hash_map(void);

void
free_reg_t_hash_map(reg_t_hash_map_ptr);

int
reg_t_hash_map_check_value(reg_t_hash_map_ptr, reg_t value, char add_p, char *added);

int
reg_t_hash_map_remove_value(reg_t_hash_map_ptr, reg_t value);

void
apply_to_reg_t_hash_map(reg_t_hash_map_ptr map, void *param, void (*note) (void *param, reg_t name, int value));

#endif /* _INT_HASHMAP_H_ */
