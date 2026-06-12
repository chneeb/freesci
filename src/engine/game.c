/***************************************************************************
 game.c Copyright (C) 1999 Christoph Reichenbach


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

    Christoph Reichenbach (CJR) [jameson@linuxgames.com]

***************************************************************************/


/* Attempt to guess if recent version of Platform SDK */
#ifdef _MSC_VER
#	pragma message("******************** IMPORTANT MESSAGE ********************")
#	pragma message("You must have installed a recent Platform and DirectX SDK")
#	pragma message("for this build to be successful. Download MS SDKs from:")
#	pragma message("www.microsoft.com/msdownload/platformsdk/sdkupdate")
#	pragma message("***********************************************************")
#	include <windows.h>
#	if (WINVER < 0x500)
#		error *** BUILD FAILED: Need more recent SDKs or fix your SDK paths in Tools Options ***
#	endif
#endif

#include <sciresource.h>
#include <engine.h>
#include <versions.h>
#include <kernel.h>
#include "kernel_types.h"
#ifdef HAVE_PICO
#include "gc.h"
#endif
#ifdef PICO_VOCAB_PROBE
#include <malloc.h>
#include <string.h>
#endif

/* Structures and data from vm.c: */
extern calls_struct_t *send_calls;
extern int send_calls_allocated;
extern int bp_flag;


#if defined(HAVE_PICO) && defined(PICO_PACK_VOCAB)
/* The packed-words blob is input-independent (identical the whole game) and is
   already a permanent baseline SRAM cost. Pack it ONCE and keep it resident
   across savegame restores instead of freeing + re-packing it on the replay
   teardown: the re-pack needs one contiguous ~30KB block that fails on the
   fragmented post-restore heap (the vocab.c:206 OOM). Mirrors the B-1 priority
   scratch and the resmgr/gfx_state stay-resident restore design. It is freed
   only when a game fully exits to the chooser (pico_reset_resident_vocab, called
   from pico_main.c after freesci_main returns) so the next, possibly different,
   game re-packs its own vocab. */
static word_t **g_pico_vocab_blob = NULL;
static int g_pico_vocab_blob_nr = 0;

void
pico_reset_resident_vocab(void)
{
	if (g_pico_vocab_blob)
		free(g_pico_vocab_blob);
	g_pico_vocab_blob = NULL;
	g_pico_vocab_blob_nr = 0;
}
#endif


static int
_init_vocabulary(state_t *s) /* initialize vocabulary and related resources */
{
	s->parser_lastmatch_word = SAID_NO_MATCH;
	s->parser_rules = NULL;

	sciprintf("Initializing vocabulary\n");

#ifdef HAVE_PICO
#ifdef PICO_VOCAB_PROBE
	/* THROWAWAY measurement for roadmap #1 (text parser). Load the full vocab
	   once, report its SRAM cost + the GNF-build transient peak, then free it and
	   fall through to the normal parser-less skip so the game still runs. Decides
	   resident-vs-rebuild for the GNF rules. Build with -DPICO_VOCAB_PROBE=ON for
	   one flash, read the [vocab] lines from pico.log, then turn it back OFF. */
	{
		extern int _gnf_rule_bytes_peak;
		extern int _allocd_rules;
		word_t **words = NULL;
		suffix_t **suffices = NULL;
		parse_tree_branch_t *branches = NULL;
		parse_rule_list_t *rules = NULL;
		int words_nr = 0, suffices_nr = 0, branches_nr = 0;
		int i, base, d_words, d_suff, d_branch, d_gnf;
		size_t packed = 0, rule_resident = 0, rule_nodes = 0;

		base = mallinfo().uordblks;
		words = vocab_get_words(s->resmgr, &words_nr);
		d_words = mallinfo().uordblks - base;

		if (!words) {
			sciprintf("[vocab] no parser vocab resource for this game\n");
		} else {
			for (i = 0; i < words_nr; i++)
				packed += 4 + strlen(words[i]->word) + 1;  /* 2B class + 2B group + str */
			packed += (size_t)words_nr * sizeof(unsigned int); /* offset table */

			base = mallinfo().uordblks;
			suffices = vocab_get_suffices(s->resmgr, &suffices_nr);
			d_suff = mallinfo().uordblks - base;

			base = mallinfo().uordblks;
			branches = vocab_get_branches(s->resmgr, &branches_nr);
			d_branch = mallinfo().uordblks - base;

			_gnf_rule_bytes_peak = 0;
			base = mallinfo().uordblks;
			rules = vocab_build_gnf(branches, branches_nr);
			d_gnf = mallinfo().uordblks - base;

			{
				parse_rule_list_t *r = rules;
				for (; r; r = r->next) {
					rule_nodes++;
					if (r->rule)
						rule_resident += sizeof(int) * (r->rule->length + 4);
				}
			}

			sciprintf("[vocab] words=%d cur=%dB packed=%uB | suffices=%d %dB | branches=%d %dB\n",
				  words_nr, d_words, (unsigned)packed,
				  suffices_nr, d_suff, branches_nr, d_branch);
			sciprintf("[vocab] GNF rules=%d nodes=%u resident=%uB (uord+%dB) build_peak=%dB\n",
				  _allocd_rules, (unsigned)rule_nodes, (unsigned)rule_resident,
				  d_gnf, _gnf_rule_bytes_peak);

			vocab_free_rule_list(rules);
			vocab_free_branches(branches);
			vocab_free_suffices(s->resmgr, suffices, suffices_nr);
			vocab_free_words(words, words_nr);
		}
	}
#endif /* PICO_VOCAB_PROBE */
	/* Roadmap #1: re-enable the text parser on Pico. Words/suffices/branches are
	   kept resident (they are read on every kParse), but the GNF rule list — the
	   ~42KB the probe measured — is NOT built here. It is rebuilt per typed command
	   inside kParse and freed afterwards (see kstring.c), so its cost is transient
	   rather than a permanent resident charge. parser_rules stays NULL at init. */
#ifdef PICO_PACK_VOCAB
	if (g_pico_vocab_blob) {
		/* Restore path: reuse the resident packed vocab — no re-pack, no OOM. */
		s->parser_words    = g_pico_vocab_blob;
		s->parser_words_nr = g_pico_vocab_blob_nr;
		s->parser_suffices = vocab_get_suffices(s->resmgr, &(s->parser_suffices_nr));
		s->parser_branches = vocab_get_branches(s->resmgr, &(s->parser_branches_nr));
		s->parser_rules    = NULL;  /* rebuilt per-command in kParse */
		sciprintf("Pico: parser vocab REUSED resident, %d words (GNF rebuilt per command).\n",
			  s->parser_words_nr);
	} else
#endif
	if ((s->resmgr->sci_version < SCI_VERSION_01_VGA)
	    && (s->parser_words = vocab_get_words(s->resmgr, &(s->parser_words_nr)))) {
#ifdef PICO_PACK_VOCAB
		/* Stage 2: collapse the ~1489 per-word blocks into one packed
		   allocation. On success keep it resident across restore
		   (g_pico_vocab_blob, freed only on full exit by
		   pico_reset_resident_vocab); on failure vocab_pack_words returns the
		   same unpacked array untouched, which is freed normally. */
		{
			word_t **packed = vocab_pack_words(s->parser_words, s->parser_words_nr);
			if (packed != s->parser_words) {
				s->parser_words      = packed;
				g_pico_vocab_blob    = packed;
				g_pico_vocab_blob_nr = s->parser_words_nr;
			}
		}
#endif
		s->parser_suffices = vocab_get_suffices(s->resmgr, &(s->parser_suffices_nr));
		s->parser_branches = vocab_get_branches(s->resmgr, &(s->parser_branches_nr));
		s->parser_rules    = NULL;  /* rebuilt per-command in kParse */
#ifdef PICO_PACK_VOCAB
		sciprintf("Pico: parser vocab loaded, %d words PACKED (GNF rebuilt per command).\n",
			  s->parser_words_nr);
#else
		sciprintf("Pico: parser vocab loaded, %d words unpacked (GNF rebuilt per command).\n",
			  s->parser_words_nr);
#endif
	} else {
		s->parser_words    = NULL;
		s->parser_rules    = NULL;
		s->parser_suffices = NULL;
		s->parser_branches = NULL;
		sciprintf("Pico: no parser vocabulary for this game.\n");
	}

	s->opcodes = vocabulary_get_opcodes(s->resmgr);

	/* selector_names must stay resident: write_selector() bounds-checks selector
	   ids against selector_names_nr, so freeing them (nr=0) silently drops every
	   kernel-side PUT_SEL32 write (e.g. Bresenham dx/dy/b_di). ~2-3KB, kept. */
	if (!(s->selector_names = vocabulary_get_snames(s->resmgr, NULL, s->version))) {
		sciprintf("_init_vocabulary(): Could not retreive selector names (vocab.997)!\n");
		return 1;
	}
	for (s->selector_names_nr = 0; s->selector_names[s->selector_names_nr]; s->selector_names_nr++);

	script_map_selectors(s, &(s->selector_map));

	return 0;
#else
	if ((s->resmgr->sci_version < SCI_VERSION_01_VGA)&&(s->parser_words = vocab_get_words(s->resmgr, &(s->parser_words_nr)))) {
		s->parser_suffices = vocab_get_suffices(s->resmgr, &(s->parser_suffices_nr));
		if ((s->parser_branches = vocab_get_branches(s->resmgr, &(s->parser_branches_nr))))
		    /* Now build a GNF grammar out of this */
		    s->parser_rules = vocab_build_gnf(s->parser_branches, s->parser_branches_nr);

	} else {
		sciprintf("Assuming that this game does not use a parser.\n");
		s->parser_rules = NULL;
	}


	s->opcodes = vocabulary_get_opcodes(s->resmgr);

	if (!(s->selector_names = vocabulary_get_snames(s->resmgr, NULL, s->version))) {
		sciprintf("_init_vocabulary(): Could not retreive selector names (vocab.997)!\n");
		return 1;
	}

	for (s->selector_names_nr = 0; s->selector_names[s->selector_names_nr]; s->selector_names_nr++);
	/* Counts the number of selector names */

	script_map_selectors(s, &(s->selector_map));
	/* Maps a few special selectors for later use */

	return 0;
#endif
}

extern int _allocd_rules;
static void
_free_vocabulary(state_t *s)
{
	sciprintf("Freeing vocabulary\n");

	if (s->parser_words) {
#ifdef PICO_PACK_VOCAB
		/* The resident packed blob is kept across restore — do NOT free it here
		   (pico_reset_resident_vocab frees it on full exit). Only free if this is
		   the unpacked fallback array (pack failed), which is many small blocks. */
		if (s->parser_words != g_pico_vocab_blob)
			vocab_free_words(s->parser_words, s->parser_words_nr);
#else
		vocab_free_words(s->parser_words, s->parser_words_nr);
#endif
		vocab_free_suffices(s->resmgr, s->parser_suffices, s->parser_suffices_nr);
		vocab_free_branches(s->parser_branches);
		vocab_free_rule_list(s->parser_rules);  /* NULL on Pico, safe */
	}

	vocabulary_free_snames(s->selector_names);
	vocabulary_free_knames(s->kernel_names);
	vocabulary_free_opcodes(s->opcodes);
	s->opcodes = NULL;

	s->selector_names = NULL;
	s->kernel_names = NULL;
	s->opcodes = NULL;
}


static int
_init_graphics_input(state_t *s)
{
	s->pic_priority_table = NULL;
	s->pics = NULL;
	s->pics_nr = 0;
	return 0;
}

static void
_sci1_alloc_system_colors(state_t *s)
{
	gfx_color_t white;
	gfx_color_t black;

	white.visual.global_index = 255;
	white.visual.r = white.visual.g = white.visual.b = 255;
	white.alpha = 0;
	white.priority = white.control = 0;
	white.mask = GFX_MASK_VISUAL;
	gfxop_set_system_color(s->gfx_state, &white);

	black.visual.global_index = 0;
	black.visual.r = black.visual.g = black.visual.b = 0;
	black.alpha = 0;
	black.priority = black.control = 0;
	black.mask = GFX_MASK_VISUAL;
	gfxop_set_system_color(s->gfx_state, &black);
}

int
_reset_graphics_input(state_t *s)
{
	resource_t *resource;
	int font_nr;
	gfx_color_t transparent;
	sciprintf("Initializing graphics\n");

	if (s->resmgr->sci_version <= SCI_VERSION_01) {
		int i;

		for (i = 0; i < 16; i++) {
			if (gfxop_set_color(s->gfx_state, &(s->ega_colors[i]),
					    gfx_sci0_image_colors[sci0_palette][i].r,
					    gfx_sci0_image_colors[sci0_palette][i].g,
					    gfx_sci0_image_colors[sci0_palette][i].b,
					    0, -1, -1))
				return 1;
			gfxop_set_system_color(s->gfx_state, &(s->ega_colors[i]));
		}
	} else
	{
		/* Check for Amiga palette file. */
		FILE *f = sci_fopen("spal", "rb");
		if (f) {
			s->gfx_state->resstate->static_palette = 
			  gfxr_read_pal1_amiga(&s->gfx_state->resstate->static_palette_entries, f);
			fclose(f);
			_sci1_alloc_system_colors(s);
		} else {
			resource = scir_find_resource(s->resmgr, sci_palette, 999, 1);
			if (resource) {
				if (s->version < SCI_VERSION(1,001,000))
					s->gfx_state->resstate->static_palette = 
					  gfxr_read_pal1(999, &s->gfx_state->resstate->static_palette_entries, 
							 resource->data, resource->size); 
			    	else
					s->gfx_state->resstate->static_palette = 
					  gfxr_read_pal11(999, &s->gfx_state->resstate->static_palette_entries, 
							    resource->data, resource->size); 
				_sci1_alloc_system_colors(s);
				scir_unlock_resource(s->resmgr, resource, sci_palette, 999);
			} else
				sciprintf("Couldn't find the default palette!\n");
		}
	}
	transparent.mask = 0;

	gfxop_fill_box(s->gfx_state, gfx_rect(0, 0, 320, 200), s->ega_colors[0]); /* Fill screen black */
	gfxop_update(s->gfx_state);

	s->mouse_pointer_view = s->mouse_pointer_loop = s->mouse_pointer_cel = -1; /* No mouse pointer resource */
	s->save_mouse_pointer_view = s->save_mouse_pointer_loop = s->save_mouse_pointer_cel = -1; /* No mouse pointer resource */
	gfxop_set_pointer_position(s->gfx_state, gfx_point(160, 150));

	s->mouse_pointer_view = s->mouse_pointer_loop = s->mouse_pointer_cel = -1; /* No mouse pointer resource */
	s->save_mouse_pointer_view = s->save_mouse_pointer_loop = s->save_mouse_pointer_cel = -1; /* No mouse pointer resource */


	s->pic_is_new = 0;
	s->pic_visible_map = GFX_MASK_NONE; /* Other values only make sense for debugging */
	s->dyn_views = NULL; /* no DynViews */
	s->drop_views = NULL; /* And, consequently, no list for dropped views */

	s->priority_first = 42; /* Priority zone 0 ends here */

	if (s->version < SCI_VERSION_FTU_PRIORITY_14_ZONES)
		s->priority_last = 200; else
		s->priority_last = 190;

	font_nr = -1;
	do {
		resource = scir_test_resource(s->resmgr, sci_font, ++font_nr);
	} while ((!resource) && (font_nr < sci_max_resource_nr[s->resmgr->sci_version]));

	if (!resource) {
		sciprintf("No text font was found.\n");
		return 1;
	}

	s->visual = gfxw_new_visual(s->gfx_state, font_nr);

	s->wm_port = gfxw_new_port(s->visual, NULL, s->gfx_state->options->pic_port_bounds, s->ega_colors[0], transparent);
	s->iconbar_port = gfxw_new_port(s->visual, NULL, gfx_rect(0, 0, 320, 200), s->ega_colors[0], transparent);
	s->iconbar_port->flags |= GFXW_FLAG_NO_IMPLICIT_SWITCH;

	if (s->resmgr->sci_version >= SCI_VERSION_01_VGA)
	{
		gfx_color_t fgcolor;
		gfx_color_t bgcolor;

#if 0
		fgcolor.visual = s->gfx_state->resstate->static_palette[0];
		fgcolor.mask = GFX_MASK_VISUAL;
		bgcolor.visual = s->gfx_state->resstate->static_palette[255];
		bgcolor.mask = GFX_MASK_VISUAL;
#endif
		s->titlebar_port = gfxw_new_port(s->visual, NULL, gfx_rect(0, 0, 320, 10), 
						 fgcolor, bgcolor);
	} else
		s->titlebar_port = gfxw_new_port(s->visual, NULL, gfx_rect(0, 0, 320, 10), 
						 s->ega_colors[0], s->ega_colors[15]);
	s->titlebar_port->color.mask |= GFX_MASK_PRIORITY;
	s->titlebar_port->color.priority = 11;
	s->titlebar_port->bgcolor.mask |= GFX_MASK_PRIORITY;
	s->titlebar_port->bgcolor.priority = 11;
	s->titlebar_port->flags |= GFXW_FLAG_NO_IMPLICIT_SWITCH;

	/* but this is correct */
	s->picture_port = gfxw_new_port(s->visual, NULL, s->gfx_state->options->pic_port_bounds, s->ega_colors[0], transparent);

	s->pics_drawn_nr = 0;

	s->visual->add(GFXWC(s->visual), GFXW(s->wm_port));
	s->visual->add(GFXWC(s->visual), GFXW(s->titlebar_port));
	s->visual->add(GFXWC(s->visual), GFXW(s->picture_port));
	s->visual->add(GFXWC(s->visual), GFXW(s->iconbar_port));
	/* Add ports to visual */

	s->port = s->picture_port; /* Currently using the picture port */

#if 0
	s->titlebar_port->bgcolor.mask |= GFX_MASK_PRIORITY;
	s->titlebar_port->bgcolor.priority = 11; /* Standard priority for the titlebar port */
#endif

	return 0;
}

int
game_init_graphics(state_t *s)
{
#ifndef WITH_PIC_SCALING
	if (s->gfx_state->options->pic0_unscaled == 0)
		sciprintf("WARNING: Pic scaling was disabled; your version of FreeSCI has no support for scaled pic drawing built in.\n");

	s->gfx_state->options->pic0_unscaled = 1;
#endif
	return _reset_graphics_input(s);
}


static void
_free_graphics_input(state_t *s)
{
	sciprintf("Freeing graphics\n");

	s->visual->widfree(GFXW(s->visual));

	s->wm_port = s->titlebar_port = s->picture_port = NULL;
	s->visual = NULL;
	s->dyn_views = NULL;
	s->port = NULL;

	if (s->pics)
		sci_free(s->pics);
	s->pics = NULL;
}

/*------------------------------------------------------------*/

int
game_init_sound(state_t *s, int sound_flags)
{
	if (s->resmgr->sci_version >= SCI_VERSION_01)
		sound_flags |= SFX_STATE_FLAG_MULTIPLAY;

	s->sfx_init_flags = sound_flags;
	sfx_init(&s->sound, s->resmgr, sound_flags);
	return 0;
}


/* Maps a class ID to the script the corresponding class is contained in */
/* Returns the script number suggested by vocab.996, or -1 if there's none */
static int
suggested_script(resource_t *res, unsigned int class)
{
	int offset;
	if (!res || class >= res->size >> 2)
		return -1;

	offset = 2 + (class << 2);

	return getInt16(res->data + offset);
}


int 
test_cursor_style(state_t *s)
{
	int resource_nr = 0;
	int ok = 0;

	do {
		ok |= scir_test_resource(s->resmgr, sci_cursor, resource_nr++) != NULL;
	} while (resource_nr < 1000 && !ok);

	return ok;
}

int
create_class_table_sci11(state_t *s)
{
	int scriptnr;
	unsigned int seeker_offset;
	char *seeker_ptr;
	int classnr;

	resource_t *vocab996 = scir_find_resource(s->resmgr, sci_vocab, 996, 1);

	if (!vocab996)
		s->classtable_size = 20;
	else
		s->classtable_size = vocab996->size >> 2;

	s->classtable = (class_t*)sci_calloc(sizeof(class_t), s->classtable_size);

	for (scriptnr = 0; scriptnr < 1000; scriptnr++) {
		resource_t *heap = scir_find_resource(s->resmgr, sci_heap,
						      scriptnr, 0);

		if (heap) {
			int global_vars = getUInt16(heap->data + 2);

			seeker_ptr = (char*)heap->data + 4 + global_vars*2;
			seeker_offset = 4 + global_vars*2;

			while (getUInt16((byte*)seeker_ptr) == SCRIPT_OBJECT_MAGIC_NUMBER)
			{
				if (getUInt16((byte*)seeker_ptr + 14) & SCRIPT_INFO_CLASS)
				{
					classnr = getUInt16((byte*)seeker_ptr + 10);
					if (classnr >= s->classtable_size) {

						if (classnr >= SCRIPT_MAX_CLASSTABLE_SIZE) {
							fprintf(stderr,"Invalid class number 0x%x in script.%d(0x%x), offset %04x\n",
								classnr, scriptnr, scriptnr, seeker_offset);
							return 1;
						}

						s->classtable = (class_t*)sci_realloc(s->classtable, sizeof(class_t) * (classnr + 1));
						memset(&(s->classtable[s->classtable_size]), 0,
						       sizeof(class_t) * (1 + classnr - s->classtable_size)); /* Clear after resize */

						s->classtable_size = classnr + 1; /* Adjust maximum number of entries */
					}
					
					s->classtable[classnr].reg.offset = seeker_offset;
					s->classtable[classnr].reg.segment = 0;
					s->classtable[classnr].script = scriptnr;
				}

				seeker_ptr += getUInt16((byte*)seeker_ptr + 2) * 2;
				seeker_offset += getUInt16((byte*)seeker_ptr + 2) * 2;
			}
		}
	}

	return 0;
}
static int
create_class_table_sci0(state_t *s)
{
	int scriptnr;
	unsigned int seeker;
	int classnr;
	int magic_offset; /* For strange scripts in older SCI versions */

	resource_t *vocab996 = scir_find_resource(s->resmgr, sci_vocab, 996, 1);

	if (!vocab996)
		s->classtable_size = 20;
	else
		s->classtable_size = vocab996->size >> 2;

	s->classtable = (class_t*)sci_calloc(sizeof(class_t), s->classtable_size);

#ifdef HAVE_PICO
	/* Force immediate eviction after each script: keeps only 1 script in LRU
	   at a time, cutting sbrk growth from ~127KB down to ~1x max_script_size. */
	{ int _saved_max = s->resmgr->max_memory;
	  s->resmgr->max_memory = 1;
#endif
	for (scriptnr = 0; scriptnr < 1000; scriptnr++) {
		int objtype = 0;
		resource_t *script = scir_find_resource(s->resmgr, sci_script,
							scriptnr, 0);

		if (script) {
			if (s->version < SCI_VERSION_FTU_NEW_SCRIPT_HEADER)
				magic_offset = seeker = 2;
			else
				magic_offset = seeker = 0;

			do {

				while (seeker < script->size)	{
					unsigned int lastseeker = seeker;
					objtype = getInt16(script->data + seeker);
					if (objtype == sci_obj_class || objtype == sci_obj_terminator)
						break;
					seeker += getInt16(script->data + seeker + 2);
					if (seeker <= lastseeker) {
						sciprintf("Warning: Script version is invalid.\n");
						sci_free(s->classtable);
						return  SCI_ERROR_INVALID_SCRIPT_VERSION;
					}
				}

				if (objtype == sci_obj_class) {
					int sugg_script;

					seeker -= SCRIPT_OBJECT_MAGIC_OFFSET; /* Adjust position; script home is base +8 bytes */

					classnr = getInt16(script->data + seeker + 4 + SCRIPT_SPECIES_OFFSET);
					if (classnr >= s->classtable_size) {

						if (classnr >= SCRIPT_MAX_CLASSTABLE_SIZE) {
							fprintf(stderr,"Invalid class number 0x%x in script.%d(0x%x), offset %04x\n",
								classnr, scriptnr, scriptnr, seeker);
							return 1;
						}

						s->classtable = (class_t*)sci_realloc(s->classtable, sizeof(class_t) * (classnr + 1));
						memset(&(s->classtable[s->classtable_size]), 0,
						       sizeof(class_t) * (1 + classnr - s->classtable_size)); /* Clear after resize */

						s->classtable_size = classnr + 1; /* Adjust maximum number of entries */
					}

					sugg_script = suggested_script(vocab996, classnr);

					/* First, test whether the script hasn't been claimed, or if it's been claimed by the wrong script */

					if (sugg_script == -1 || scriptnr == sugg_script /*|| !s->classtable[classnr].reg.segment*/)  {
						/* Now set the home script of the class */
						s->classtable[classnr].reg.offset = seeker + 4 - magic_offset;
						s->classtable[classnr].reg.segment = 0;
						s->classtable[classnr].script = scriptnr;
					}

					seeker += SCRIPT_OBJECT_MAGIC_OFFSET; /* Re-adjust position */

					seeker += getInt16(script->data + seeker + 2); /* Move to next */
				}

			} while (objtype != sci_obj_terminator && seeker <= script->size);

		}
	}
#ifdef HAVE_PICO
	s->resmgr->max_memory = _saved_max;
	} /* close block opened around the loop */
#endif
	scir_unlock_resource(s->resmgr, vocab996, sci_vocab, 996);
	vocab996 = NULL;
	return 0;
}

/* Architectural stuff: Init/Unintialize engine */
int
script_init_engine(state_t *s, sci_version_t version)
{
	int result;

	s->max_version = SCI_VERSION(9,999,999); /* :-) */
	s->min_version = 0; /* Set no real limits */
	s->version = SCI_VERSION_DEFAULT_SCI0;
	s->kernel_opt_flags = 0;
	
	if (!version) {
		s->version_lock_flag = 0;
	} else {
		s->version = version;
		s->version_lock_flag = 1; /* Lock version */
	}

#ifndef HAVE_PICO
	/* Skip on Pico: iterates up to 1000 scripts via LRU cache, growing sbrk by
	   ~118KB that is never reclaimed, making GFX buffer allocation impossible.
	   SCI_VERSION_DEFAULT_SCI0 (0.000.685) is correct for all supported Pico games. */
	script_detect_versions(s);
#endif

        if (s->version >= SCI_VERSION(1,001,000))
		result = create_class_table_sci11(s);
	else
		result = create_class_table_sci0(s);

	sm_init(&s->seg_manager, s->version >= SCI_VERSION(1,001,000));
	s->gc_countdown = GC_INTERVAL - 1;

	if (result)
	{
		sciprintf("Failed to initialize class table\n");
		return 1;
	}

	s->script_000_segment = script_get_segment(s, 0, SCRIPT_GET_LOCK);

	if (s->script_000_segment <= 0) {
		sciprintf("Failed to instantiate script.000\n");
		return 1;
	}

	s->script_000 = &(s->seg_manager.heap[s->script_000_segment]->data.script);


	s->sys_strings = sm_allocate_sys_strings(&s->seg_manager,
						 &s->sys_strings_segment);
	/* Allocate static buffer for savegame and CWD directories */
	sys_string_acquire(s->sys_strings, SYS_STRING_SAVEDIR, "savedir", MAX_SAVE_DIR_SIZE);

	s->save_dir_copy = make_reg(s->sys_strings_segment, SYS_STRING_SAVEDIR);
	s->save_dir_edit_offset = 0;

	s->r_acc = s->r_prev = NULL_REG;
	s->r_amp_rest = 0;

	s->execution_stack = NULL;    /* Start without any execution stack */
	s->execution_stack_base = -1; /* No vm is running yet */
	s->execution_stack_pos = -1;   /* Start at execution stack position 0 */


	s->kernel_names = vocabulary_get_knames(s->resmgr, &s->kernel_names_nr);
	script_map_kernel(s);
	/* Maps the kernel functions */
	/* kernel_names must stay resident: freeing it leaves kfunct_table[].orig_name
	   dangling and makes has_kernel_function() always return 0. */

	if (_init_vocabulary(s)) return 1;

	if (s->selector_map.cantBeHere != -1)
		version_require_later_than(s, SCI_VERSION_FTU_INVERSE_CANBEHERE);

	s->restarting_flags = SCI_GAME_IS_NOT_RESTARTING;

	s->bp_list = NULL; /* No breakpoints defined */
	s->have_bp = 0;

	s->file_handles_nr = 5;
	s->file_handles = (FILE**)sci_calloc(sizeof(FILE *), s->file_handles_nr);
	/* Allocate memory for file handles */

	sci_init_dir(&(s->dirseeker));
	s->dirseeker_outbuffer = NULL_REG;
	/* Those two are used by FileIO for FIND_FIRST, FIND_NEXT */

	if (s->version >= SCI_VERSION_FTU_LOFS_ABSOLUTE &&
	    s->version < SCI_VERSION(1,001,000))
		sm_set_export_width(&s->seg_manager, 1);
	else
		sm_set_export_width(&s->seg_manager, 0);

	sciprintf("Engine initialized\n");

	if (_init_graphics_input(s))
		return 1;

	return 0;
}


void
script_set_gamestate_save_dir(state_t *s, const char* path)
{
	sys_string_set(s->sys_strings, SYS_STRING_SAVEDIR, path);
}

void
script_free_vm_memory(state_t *s)
{
	int i;

	sciprintf("Freeing VM memory\n");
	s->save_dir_copy_buf = NULL;

	sci_free(s->classtable);
	s->classtable = NULL;

	/* Close all opened file handles */
#ifndef _DOS
	for (i = 1; i < s->file_handles_nr; i++)
		if (s->file_handles[i])
			fclose(s->file_handles[i]);
#endif

	sci_free(s->file_handles);
	s->file_handles = NULL;

	/* FIXME: file handles will NOT be closed under DOS. DJGPP generates an
	   exception fault whenever you try to close a never-opened file */
}

extern void
free_kfunct_tables(state_t *s);
/* From kernel.c */

void
script_free_engine(state_t *s)
{
	script_free_vm_memory(s);

	sciprintf("Freeing state-dependant data\n");

	free_kfunct_tables(s);

	_free_vocabulary(s);

}

void
script_free_breakpoints(state_t *s)
{
	breakpoint_t *bp, *bp_next;

	/* Free breakpoint list */
	bp = s->bp_list;
	while (bp) {
		bp_next = bp->next;
		if (bp->type == BREAK_SELECTOR) sci_free (bp->data.name);
		free (bp);
		bp = bp_next;
	}

	s->bp_list = NULL;
}

/*************************************************************/
/* Game instance stuff: Init/Unitialize state-dependant data */
/*************************************************************/

#ifdef HAVE_PICO
/* The live gamestate, published while a game is running (game_init..game_exit)
   so the allocator's last-ditch reclaim can reach the resmgr + seg manager.
   NULL outside a running game (e.g. during the chooser / restore teardown). */
state_t *g_pico_current_state = NULL;

/* Last-ditch heap reclaim, called by _SCI_MALLOC/_SCI_CALLOC/_SCI_REALLOC when an
   allocation fails BEFORE giving up to pico_oom_report. On SQ3's tight, fragmented
   heap a routine decode/deserialize alloc (e.g. decompress0.c:324) can be denied
   while KB remain free total — the free space is just shattered into sub-block
   chunks. Two cheap things can re-coalesce a contiguous run without losing game
   state: flush the resource LRU (reloads on demand) and run the GC (reclaims
   disposed clones the death-animation piled up that GC hasn't swept yet). The
   re-entrancy guard lives in sci_memory.c: run_gc itself allocates, so a failure
   *inside* reclaim must fall straight through to pico_oom_report, not recurse. */
void
pico_reclaim_heap(void)
{
	state_t *s = g_pico_current_state;

	if (!s)
		return;

	if (s->resmgr)
		scir_free_all_lru(s->resmgr);

	//run_gc(s);
}
#endif

int
game_init(state_t *s)
{
#ifdef __GNUC__
#  warning "Fixme: Use new VM instantiation code all over the place"
#endif
	reg_t game_obj; /* Address of the game object */
	dstack_t *stack;

	stack = sm_allocate_stack(&s->seg_manager, VM_STACK_SIZE,
					      &s->stack_segment);
	s->stack_base = stack->entries;
	s->stack_top = s->stack_base + VM_STACK_SIZE;

	if (!script_instantiate(s, 0)) {
		sciprintf("game_init(): Could not instantiate script 0\n");
		return 1;
	}

	s->parser_valid = 0; /* Invalidate parser */
	s->parser_event = NULL_REG; /* Invalidate parser event */

	s->synonyms = NULL;
	s->synonyms_nr = 0; /* No synonyms */

	/* Initialize send_calls buffer */

	if (!send_calls_allocated)
		send_calls = (calls_struct_t*)sci_calloc(sizeof(calls_struct_t), send_calls_allocated = 16);

	if (s->gfx_state && _reset_graphics_input(s))
		return 1;

	s->successor = NULL; /* No successor */
	s->status_bar_text = NULL; /* Status bar is blank */
	s->status_bar_foreground = 0;
	s->status_bar_background = s->resmgr->sci_version >= SCI_VERSION_01_VGA ? 255 : 15;

	sys_string_acquire(s->sys_strings, SYS_STRING_PARSER_BASE, "parser-base", MAX_PARSER_BASE);
	s->parser_base = make_reg(s->sys_strings_segment, SYS_STRING_PARSER_BASE);

	sci_get_current_time(&(s->game_start_time)); /* Get start time */
	memcpy(&(s->last_wait_time), &(s->game_start_time), sizeof(GTimeVal));
	/* Use start time as last_wait_time */

	s->debug_mode = 0x0; /* Disable all debugging */
	s->onscreen_console = 0; /* No onscreen console unless explicitly requested */

	srand(time(NULL)); /* Initialize random number generator */

	/*	script_dissect(0, s->selector_names, s->selector_names_nr); */
	game_obj = script_lookup_export(s, 0, 0);
	/* The first entry in the export table of script 0 points to the game object */

	s->game_name = sci_strdup(obj_get_name(s, game_obj));

	if (!s->game_name) {
		sciprintf("Error: script.000, export 0 ("PREG") does not\n"
			  "  yield an object with a name -> sanity check failed\n",
			  PRINT_REG(game_obj));
		return 1;
	}

	sciprintf(" \"%s\" at "PREG"\n", s->game_name, PRINT_REG(game_obj));

	if (strlen((char *) s->game_name) >= MAX_GAMEDIR_SIZE) {

		s->game_name[MAX_GAMEDIR_SIZE - 1] = 0; /* Fix length with brute force */
		sciprintf(" Designation too long; was truncated to \"%s\"\n", s->game_name);
	}

	s->game_obj = game_obj;

	/* Mark parse tree as unused */
	s->parser_nodes[0].type = PARSE_TREE_NODE_LEAF;
	s->parser_nodes[0].content.value = 0;

	s->menubar = menubar_new(); /* Create menu bar */

#ifdef HAVE_PICO
	g_pico_current_state = s; /* Publish for the allocator's last-ditch reclaim */
#endif

	return 0;
}

int
game_exit(state_t *s)
{
#ifdef HAVE_PICO
	g_pico_current_state = NULL; /* Resmgr/seg manager are about to be torn down */
#endif

	if (s->execution_stack) {
		sci_free(s->execution_stack);
	}

#if 0
	sfx_exit(&s->sound);
/* Reinit because some other code depends on having a valid state */
	game_init_sound(s, SFX_STATE_FLAG_NOSOUND); 
#else
#endif

	sm_destroy(&s->seg_manager);
	
	if (s->synonyms_nr) {
		sci_free(s->synonyms);
		s->synonyms = NULL;
		s->synonyms_nr = 0;
	}

	sciprintf("Freeing miscellaneous data...\n");

#ifdef __GNUC__
#warning "Free parser segment here"
#endif
	if (send_calls_allocated) {
		sci_free(send_calls);
		send_calls_allocated = 0;
	}

#ifdef __GNUC__
#warning "Free scripts here"
#endif

	menubar_free(s->menubar);

	_free_graphics_input(s);

	sci_free(s->game_name);

	free(s->game_version);
	s->game_version = NULL;

	return 0;
}

