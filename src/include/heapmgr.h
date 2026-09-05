/***************************************************************************
 heapmgr.h Copyright (C) 2002 Christoph Reichenbach


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
/* Heap-like managed structure */

#ifndef _FREESCI_HEAPMGR_H_
#define _FREESCI_HEAPMGR_H_

#include <resource.h>
#include <sci_memory.h>

/* On the Pimoroni mapped-PSRAM target the clone/node/list/hunk tables (below) —
   the realloc-grow fragmentation drivers that never leave SRAM on the PicoCalc —
   are allocated from the 8 MB PSRAM heap instead, off the 475 KB SRAM ceiling.
   The free/realloc macros are OWNERSHIP-AWARE (psram_heap_owns): a table that came
   from sci_malloc (e.g. the savegame restore path, not yet routed) is still freed
   with sci_free, so there is no allocator mismatch regardless of where a given
   table was allocated. Everywhere else (desktop, PicoCalc) these are plain sci_*. */
#if defined(HAVE_PICO) && defined(PICO_PSRAM_MAPPED)
extern void *psram_hmalloc(size_t n);
extern void *psram_hrealloc(void *p, size_t n);
extern void  psram_hfree(void *p);
extern int   psram_heap_owns(const void *p);
#  define HEAP_TBL_MALLOC(n)     psram_hmalloc(n)
#  define HEAP_TBL_REALLOC(p, n) (psram_heap_owns(p) ? psram_hrealloc((p), (n)) : sci_realloc((p), (n)))
#  define HEAP_TBL_FREE(p)       do { if (psram_heap_owns(p)) psram_hfree(p); else sci_free(p); } while (0)
#else
#  define HEAP_TBL_MALLOC(n)     sci_malloc(n)
#  define HEAP_TBL_REALLOC(p, n) sci_realloc((p), (n))
#  define HEAP_TBL_FREE(p)       sci_free(p)
#endif

/* Phase 2: script_t.buf (the per-script VM bytecode + object-var + locals working
   memory) → PSRAM heap. Separately gated by PICO_PSRAM_SCRIPTS so it can be A/B'd
   against SRAM-resident scripts. The FREE is ownership-aware so a buf that came
   from sci_malloc/raw malloc (any not-yet-routed path) is still freed correctly.
   NB the psram_h* externs above are declared inside the same PICO_PSRAM_MAPPED
   block, so PICO_PSRAM_SCRIPTS is only ever set together with PICO_PSRAM_MAPPED. */
#if defined(HAVE_PICO) && defined(PICO_PSRAM_MAPPED) && defined(PICO_PSRAM_SCRIPTS)
#  define HEAP_SCRIPT_MALLOC(n)  psram_hmalloc(n)
#  define HEAP_SCRIPT_FREE(p)    do { if (psram_heap_owns(p)) psram_hfree(p); else sci_free(p); } while (0)
#else
#  define HEAP_SCRIPT_MALLOC(n)  sci_malloc(n)
#  define HEAP_SCRIPT_FREE(p)    sci_free(p)
#endif

#define HEAPENTRY_INVALID -1

#define ENTRY_IS_VALID(t, i) ((i) >= 0 && (i) < (t)->max_entry && (t)->table[(i)].next_free == (i))

#define DECLARE_HEAPENTRY(ENTRY)						\
typedef struct {								\
	int next_free; /* Only used for free entries */				\
	ENTRY##_t entry;							\
} ENTRY##_entry_t;								\
										\
typedef struct {								\
	int entries_nr; /* Number of entries allocated */			\
	int first_free; /* Beginning of a singly linked list for entries */	\
	int entries_used; /* Statistical information */				\
	int max_entry; /* Highest entry used */					\
	ENTRY##_entry_t *table;							\
} ENTRY##_table_t;								\
										\
void										\
init_##ENTRY##_table(ENTRY##_table_t *table);					\
int										\
alloc_##ENTRY##_entry(ENTRY##_table_t *table);					\
void										\
free_##ENTRY##_entry(ENTRY##_table_t *table, int index);



#define DEFINE_HEAPENTRY_WITH_CLEANUP(ENTRY, INITIAL, INCREMENT, CLEANUP_FN)	\
void										\
init_##ENTRY##_table(ENTRY##_table_t *table)					\
{										\
	table->entries_nr = INITIAL;						\
	table->max_entry = 0;							\
	table->entries_used = 0;						\
	table->first_free = HEAPENTRY_INVALID;					\
	table->table = (ENTRY##_entry_t*)HEAP_TBL_MALLOC(sizeof(ENTRY##_entry_t) * INITIAL);\
	memset(table->table, 0, sizeof(ENTRY##_entry_t) * INITIAL);		\
}										\
										\
void										\
free_##ENTRY##_entry(ENTRY##_table_t *table, int index)				\
{										\
	ENTRY##_entry_t *e = table->table + index;				\
										\
	if (index < 0 || index >= table->max_entry) {				\
		fprintf(stderr, "heapmgr: Attempt to release"			\
			" invalid table index %d!\n", index);			\
		BREAKPOINT();							\
	}									\
	CLEANUP_FN(&(e->entry));						\
										\
	e->next_free = table->first_free;					\
	table->first_free = index;						\
	table->entries_used--;							\
}										\
										\
int										\
alloc_##ENTRY##_entry(ENTRY##_table_t *table)					\
{										\
	table->entries_used++;							\
	if (table->first_free != HEAPENTRY_INVALID) {				\
		int oldff = table->first_free;					\
		table->first_free = table->table[oldff].next_free;		\
										\
		table->table[oldff].next_free = oldff;				\
		return oldff;							\
	} else {								\
		if (table->max_entry == table->entries_nr) {			\
			table->entries_nr += INCREMENT;				\
										\
			table->table = (ENTRY##_entry_t*)HEAP_TBL_REALLOC(table->table,\
						   sizeof(ENTRY##_entry_t)	\
						   * table->entries_nr);	\
			memset(&table->table[table->entries_nr-INCREMENT],	\
			       0, INCREMENT*sizeof(ENTRY##_entry_t));		\
		}								\
		table->table[table->max_entry].next_free =			\
			table->max_entry; /* Tag as 'valid' */			\
		return table->max_entry++;					\
	}									\
}

#define _HEAPENTRY_IGNORE_ME(x)
#define DEFINE_HEAPENTRY(e, i, p) DEFINE_HEAPENTRY_WITH_CLEANUP(e, i, p, _HEAPENTRY_IGNORE_ME)

#endif /* !_FREESCI_HEAPMGR_H_ */
