/***************************************************************************
 resource.c Copyright (C) 1999 Christoph Reichenbach, TU Darmstadt


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

    Christoph Reichenbach (CJR) [creichen@rbg.informatik.tu-darmstadt.de]

 History:

   990327 - created (CJR)

***************************************************************************/
/* Resource library */

#include <sci_memory.h>
#include <sciresource.h>
#include <vocabulary.h> /* For SCI version auto-detection */

#include <ctype.h>

#ifdef _WIN32
#include <direct.h>
#endif

#undef SCI_REQUIRE_RESOURCE_FILES
/* #define SCI_VERBOSE_RESMGR 1 */

/* On Pico, decompress0 hands the permanent pic/view decompress scratch
   (operations.c) out as res->data; it must never be sci_free'd or the next
   decode would reuse a freed block.  Every res->data free site guards on this.
   The same goes for a lent buffer (g_pico_decompress_borrow: visual[0] during
   a VGA pic load). */
#ifdef HAVE_PICO
#	define PICO_IS_DECOMPRESS_SCRATCH(p) ((unsigned char*)(p) == g_pico_decompress_scratch \
		|| ((p) && (unsigned char*)(p) == g_pico_decompress_borrow))
#else
#	define PICO_IS_DECOMPRESS_SCRATCH(p) (0)
#endif

const char* sci_version_types[] = {
	"SCI version undetermined (Autodetect failed / not run)",
	"SCI version 0.xxx",
	"SCI version 0.xxx w/ 1.000 compression",
	"SCI version 1.000 w/ 0.xxx resource.map",
	"SCI version 1.000 w/ special resource.map",
	"SCI version 1.000 (early)",
	"SCI version 1.000 (late)",
	"SCI version 1.001",
	"SCI WIN/32"
};

const int sci_max_resource_nr[] = {65536, 1000, 2048, 2048, 2048, 8192, 8192, 65536};

const char* sci_error_types[] = {
	"No error",
	"I/O error",
	"Resource is empty (size 0)",
	"resource.map entry is invalid",
	"resource.map file not found",
	"No resource files found",
	"Unknown compression method",
	"Decompression failed: Decompression buffer overflow",
	"Decompression failed: Sanity check failed",
	"Decompression failed: Resource too big",
	"SCI version is unsupported"};

const char* sci_resource_types[] = {"view","pic","script","text","sound",
				    "memory","vocab","font","cursor",
				    "patch","bitmap","palette","cdaudio",
				    "audio","sync","message","map","heap"};
/* These are the 18 resource types supported by SCI1 */

const char *sci_resource_type_suffixes[] = {"v56","p56","scr","tex","snd",
					    "   ","voc","fon","cur","pat",
					    "bit","pal","cda","aud","syn",
					    "msg","map","hep"};


int resourcecmp(const void *first, const void *second);


typedef int decomp_funct(resource_t *result, int resh, int sci_version);
typedef void patch_sprintf_funct(char *string, resource_t *res);

static decomp_funct *decompressors[] = {
	NULL,
	&decompress0,
	&decompress01,
	&decompress01,
	&decompress01,
	&decompress1,
	&decompress1,
	&decompress11,
	NULL
};

static patch_sprintf_funct *patch_sprintfers[] = {
	NULL,
	&sci0_sprintf_patch_file_name,
	&sci0_sprintf_patch_file_name,
	&sci1_sprintf_patch_file_name,
	&sci1_sprintf_patch_file_name,
	&sci1_sprintf_patch_file_name,
	&sci1_sprintf_patch_file_name,
	&sci1_sprintf_patch_file_name,
	&sci1_sprintf_patch_file_name
};


int resourcecmp (const void *first, const void *second)
{
	if (((resource_t *)first)->type ==
	    ((resource_t *)second)->type)
		return (((resource_t *)first)->number <
			((resource_t *)second)->number)? -1 :
		!(((resource_t *)first)->number ==
		  ((resource_t *)second)->number);
	else
		return (((resource_t *)first)->type <
			((resource_t *)second)->type)? -1 : 1;
}





/*-----------------------------*/
/*-- Resmgr helper functions --*/
/*-----------------------------*/

#ifdef SCIR_PACKED
/* resource_t.source_idx -> resource_source_t*. Only a handful of sources exist
   (one per volume file plus the patch directory), all registered while the map
   is read, so a linear search on insert is fine and lookup is O(1). */
/* 24 B on 32-bit targets (the Pico); a layout change that re-adds padding
   fails the build here instead of silently costing ~1 KB per 250 resources. */
typedef char scir_packed_size_check[(sizeof(void *) != 4 || sizeof(resource_t) == 24) ? 1 : -1];

#define SCIR_MAX_SOURCES 32 /* SCI0 games have <= 10 volumes plus the patch dir */
static resource_source_t *scir_source_table[SCIR_MAX_SOURCES];
static unsigned int scir_sources_nr = 0;

resource_source_t *
scir_source_ptr(unsigned int idx)
{
	return (idx && idx <= scir_sources_nr) ? scir_source_table[idx - 1] : NULL;
}

unsigned int
scir_source_idx(resource_source_t *source)
{
	unsigned int i;

	if (!source)
		return 0;
	for (i = 0; i < scir_sources_nr; i++)
		if (scir_source_table[i] == source)
			return i + 1;
	if (scir_sources_nr == SCIR_MAX_SOURCES) {
		sciprintf("Resmgr: more than %d resource sources\n", SCIR_MAX_SOURCES);
		return 0; /* reads as a missing source: the map entry is rejected */
	}
	scir_source_table[scir_sources_nr] = source;
	return ++scir_sources_nr;
}

#define SCIR_LRU_NEXT(mgr, r) ((r)->lru_next ? (mgr)->resources + (r)->lru_next - 1 : NULL)
#define SCIR_LRU_PREV(mgr, r) ((r)->lru_prev ? (mgr)->resources + (r)->lru_prev - 1 : NULL)
#define SCIR_LRU_IDX(mgr, p) ((p) ? (guint16) ((resource_t *) (p) - (mgr)->resources + 1) : 0)
#define SCIR_LRU_SET_NEXT(mgr, r, p) ((r)->lru_next = SCIR_LRU_IDX(mgr, p))
#define SCIR_LRU_SET_PREV(mgr, r, p) ((r)->lru_prev = SCIR_LRU_IDX(mgr, p))
#else
#define SCIR_LRU_NEXT(mgr, r) ((r)->next)
#define SCIR_LRU_PREV(mgr, r) ((r)->prev)
#define SCIR_LRU_SET_NEXT(mgr, r, p) ((r)->next = (p))
#define SCIR_LRU_SET_PREV(mgr, r, p) ((r)->prev = (p))
#endif


void
_scir_add_altsource(resource_t *res, resource_source_t *source, unsigned int file_offset)
{
#ifdef SCIR_PACKED
	/* No alt_sources when packed: the list is never read (see sciresource.h),
	   so building it only cost a heap block per resource. */
	(void) res; (void) source; (void) file_offset;
#else
	resource_altsource_t *rsrc = (resource_altsource_t*)sci_malloc(sizeof(resource_altsource_t));

	rsrc->next = res->alt_sources;
	rsrc->source = source;
	rsrc->file_offset = file_offset;
	res->alt_sources = rsrc;
#endif
}

resource_t *
_scir_find_resource_unsorted(resource_t *res, int res_nr, int type, int number)
{
	int i;
	for (i = 0; i < res_nr; i++)
		if (res[i].number == number && res[i].type == type)
			return res + i;
	return NULL;
}

/*-----------------------------------*/
/** Resource source list management **/
/*-----------------------------------*/

resource_source_t *
scir_add_external_map(resource_mgr_t *mgr, char *file_name)
{
	resource_source_t *newsrc = (resource_source_t *) 
		malloc(sizeof(resource_source_t));

	/* Add the new source to the SLL of sources */
	newsrc->next = mgr->sources;
	mgr->sources = newsrc;

	newsrc->source_type = RESSOURCE_TYPE_EXTERNAL_MAP;
	newsrc->location.file.name = strdup(file_name);
	newsrc->scanned = 0;
	newsrc->associated_map = NULL;

	return newsrc;
}

resource_source_t *
scir_add_volume(resource_mgr_t *mgr, resource_source_t *map, char *filename,
		int number, int extended_addressing)
{
	resource_source_t *newsrc = (resource_source_t *) 
		malloc(sizeof(resource_source_t));

	/* Add the new source to the SLL of sources */
	newsrc->next = mgr->sources;
	mgr->sources = newsrc;

	newsrc->source_type = RESSOURCE_TYPE_VOLUME;
	newsrc->scanned = 0;
	newsrc->location.file.name = strdup(filename);
	newsrc->location.file.volume_number = number;
	newsrc->associated_map = map;
}

resource_source_t *
scir_add_patch_dir(resource_mgr_t *mgr, int type, char *dirname)
{
	resource_source_t *newsrc = (resource_source_t *) 
		malloc(sizeof(resource_source_t));

	/* Add the new source to the SLL of sources */
	newsrc->next = mgr->sources;
	mgr->sources = newsrc;

	newsrc->source_type = RESSOURCE_TYPE_DIRECTORY;
	newsrc->scanned = 0;
	newsrc->location.dir.name = strdup(dirname);
}

resource_source_t *
scir_get_volume(resource_mgr_t *mgr, resource_source_t *map, int volume_nr)
{
	resource_source_t *seeker = mgr->sources;

	while (seeker)
	{
		if (seeker->source_type == RESSOURCE_TYPE_VOLUME &&
		    seeker->associated_map == map &&
		    seeker->location.file.volume_number == volume_nr)
			return seeker;
		seeker = seeker->next;
	}

	return NULL;
}

/*------------------------------------------------*/
/** Resource manager constructors and operations **/
/*------------------------------------------------*/

static void
_scir_init_trivial(resource_mgr_t *mgr)
{
	mgr->resources_nr = 0;
	mgr->resources = (resource_t*)sci_malloc(1);
}


static void
_scir_load_from_patch_file(int fh, resource_t *res, char *filename)
{
	int really_read;

	res->data = (unsigned char*)sci_malloc_sram(res->size);
	really_read = read(fh, res->data, res->size);

	if (really_read < res->size) {
		sciprintf("Error: Read %d bytes from %s but expected %d!\n",
			  really_read, filename, res->size);
		exit(1);
	}

	res->status = SCI_STATUS_ALLOCATED;
}

#if (defined(HAVE_PICO) && !defined(SCIR_NO_VOLUME_CACHE)) || defined(SCIR_VOLUME_CACHE)
/* SCIR_VOLUME_CACHE: desktop test switch; SCIR_NO_VOLUME_CACHE: Pico A/B baseline */
#  define SCIR_KEEP_VOLUMES_OPEN 1
#endif
#ifdef SCIR_KEEP_VOLUMES_OPEN
/* Resource volumes stay OPEN on the Pico (up to SCIR_VOLUME_FDS of them), with
   FatFS fast seek. Otherwise every resource load re-opens
   "<gamedir>/RESOURCE.00N" -- a directory lookup per path level on the SD card
   -- seeks by walking the FAT chain from the start of the file, and closes it
   again. Volumes beyond the cache fall back to open/close per load. Closed by
   scir_close_volume_fds() when the resource manager is freed. */
#define SCIR_VOLUME_FDS 4
static struct { resource_source_t *src; int fd; } scir_volume_fds[SCIR_VOLUME_FDS];
#ifdef HAVE_PICO
extern int pico_io_enable_fastseek(int fd);
#else
#  define pico_io_enable_fastseek(fd) ((void) (fd))
#endif

static int
scir_volume_open(resource_source_t *src, const char *filename, int *cached)
{
	int i, fd;

	*cached = 0;
	for (i = 0; i < SCIR_VOLUME_FDS; i++)
		if (scir_volume_fds[i].src == src) {
			*cached = 1;
			return scir_volume_fds[i].fd;
		}
	fd = open(filename, O_RDONLY | O_BINARY);
	if (!IS_VALID_FD(fd))
		return fd;
	for (i = 0; i < SCIR_VOLUME_FDS; i++)
		if (!scir_volume_fds[i].src) {
			scir_volume_fds[i].src = src;
			scir_volume_fds[i].fd = fd;
			*cached = 1;
			pico_io_enable_fastseek(fd);
			break;
		}
	return fd;
}

static void
scir_close_volume_fds(void)
{
	int i;

	for (i = 0; i < SCIR_VOLUME_FDS; i++)
		if (scir_volume_fds[i].src) {
			close(scir_volume_fds[i].fd);
			scir_volume_fds[i].src = NULL;
		}
}
#endif /* SCIR_KEEP_VOLUMES_OPEN */

#ifdef HAVE_PICO
/* Time spent in resource loads, for the per-room [perf] line (operations.c). */
unsigned long long pico_resload_us = 0, pico_resload_us_total = 0;
unsigned pico_resload_n = 0, pico_resload_n_total = 0;
extern unsigned long long pico_perf_us(void);
#endif

static void
_scir_load_resource_inner(resource_mgr_t *mgr, resource_t *res, int protect)
{
	char filename[PATH_MAX];
	int fh;
	int fh_cached = 0;   /* fh belongs to the open-volume cache: do not close */
	resource_t backup;
	/* The working directory is saved only right before this function changes
	   it (a patch file's chdir, or the uppercase sci_open fallback, which
	   chdirs into a path's directory) and restored only then. It used to be
	   saved and restored on EVERY load, although volume files -- nearly all
	   loads -- are opened by full path and never change it: on FatFS that was
	   a getcwd() (which rebuilds the path by walking up the directory tree on
	   the card) plus a chdir() per resource. Same result, minus that cost. */
	char *save_cwd = NULL;
#define SCIR_SAVE_CWD() do { if (!save_cwd) save_cwd = sci_getcwd(); } while (0)
#define SCIR_RESTORE_CWD() do { if (save_cwd) { chdir(save_cwd); free(save_cwd); save_cwd = NULL; } } while (0)

	memcpy(&backup, res, sizeof(resource_t));

	/* First try lower-case name */
	if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_DIRECTORY) {

		if (!patch_sprintfers[mgr->sci_version]) {
			sciprintf("Resource manager's SCI version (%d) has no patch file name printers -> internal error!\n",
				  mgr->sci_version);
			exit(1);
		}

		/* Get patch file name */
		patch_sprintfers[mgr->sci_version](filename, res);
		SCIR_SAVE_CWD();
		chdir(SCIR_SOURCE(res)->location.dir.name);
	} else
		strcpy(filename, SCIR_SOURCE(res)->location.file.name);

#ifdef SCIR_KEEP_VOLUMES_OPEN
	if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_VOLUME)
		fh = scir_volume_open(SCIR_SOURCE(res), filename, &fh_cached);
	else
#endif
	fh = open(filename, O_RDONLY | O_BINARY);


	if (!IS_VALID_FD(fh)) {
		char *raiser = filename;
		while (*raiser) {
			*raiser = toupper(*raiser); /* Uppercasify */
			++raiser;
		}
		SCIR_SAVE_CWD();   /* sci_open chdirs into a path's directory */
		fh = sci_open(filename, O_RDONLY|O_BINARY);
	}    /* Try case-insensitively name */

	if (!IS_VALID_FD(fh)) {
		sciprintf("Failed to open %s!\n", filename);
		res->data = NULL;
		res->status = SCI_STATUS_NOMALLOC;
		res->size = 0;
		SCIR_RESTORE_CWD();
		return;
	}


	lseek(fh, res->file_offset, SEEK_SET);

	if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_DIRECTORY ||
	    SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_AUDIO_DIRECTORY)
		_scir_load_from_patch_file(fh, res, filename);
	else if (!decompressors[mgr->sci_version]) {
		/* Check whether we support this at all */
		sciprintf("Resource manager's SCI version (%d) is invalid!\n",
			  mgr->sci_version);
		exit(1);
	} else {
		int error = /* Decompress from regular resource file */
			decompressors[mgr->sci_version](res, fh, mgr->sci_version);

		if (error) {
			sciprintf("Error %d occured while reading %s.%03d"
				  " from resource file: %s\n",
				  error, sci_resource_types[res->type], res->number,
				  sci_error_types[error]);

			if (protect)
				memcpy(res, &backup, sizeof(resource_t));

			res->data = NULL;
			res->status = SCI_STATUS_NOMALLOC;
			res->size = 0;
			if (!fh_cached)
				close(fh);   /* was leaked on this path */
			SCIR_RESTORE_CWD();
			return;
		}
	}

	if (!fh_cached)
		close(fh);
	SCIR_RESTORE_CWD();
#undef SCIR_SAVE_CWD
#undef SCIR_RESTORE_CWD
}

static void
_scir_load_resource(resource_mgr_t *mgr, resource_t *res, int protect)
{
#ifdef HAVE_PICO
	unsigned long long t0 = pico_perf_us(), dt;

	_scir_load_resource_inner(mgr, res, protect);
	dt = pico_perf_us() - t0;
	pico_resload_us += dt;
	pico_resload_us_total += dt;
	pico_resload_n++;
	pico_resload_n_total++;
#else
	_scir_load_resource_inner(mgr, res, protect);
#endif
}

resource_t *
scir_test_resource(resource_mgr_t *mgr, int type, int number)
{
	resource_t binseeker;
	binseeker.type = type;
	binseeker.number = number;
	return (resource_t *)
		bsearch(&binseeker, mgr->resources, mgr->resources_nr,
			sizeof(resource_t), resourcecmp);
}

int sci0_get_compression_method(int resh);

int
sci_test_view_type(resource_mgr_t *mgr)
{
	int fh;
	char filename[PATH_MAX];
	int compression;
	resource_t *res;
	int i;
	int fh_cached;   /* from the open-volume cache: do not close */

	mgr->sci_version = SCI_VERSION_AUTODETECT;

	for (i=0;i<1000;i++)
	{
		res = scir_test_resource(mgr, sci_view, i);

		if (!res) continue;

		if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_DIRECTORY ||
		    SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_AUDIO_DIRECTORY)
			continue;

		strcpy(filename, SCIR_SOURCE(res)->location.file.name);
		fh_cached = 0;
#ifdef SCIR_KEEP_VOLUMES_OPEN
		/* Once per view/pic resource at startup: 563 opens of the same few
		   volumes for KQ4 without the cache. */
		if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_VOLUME)
			fh = scir_volume_open(SCIR_SOURCE(res), filename, &fh_cached);
		else
#endif
		fh = open(filename, O_RDONLY | O_BINARY);

		if (!IS_VALID_FD(fh)) {
			char *raiser = filename;
			while (*raiser) {
				*raiser = toupper(*raiser); /* Uppercasify */
				++raiser;
			}
			fh = sci_open(filename, O_RDONLY|O_BINARY);
		}    /* Try case-insensitively name */
		
		if (!IS_VALID_FD(fh)) continue;
		lseek(fh, res->file_offset, SEEK_SET);

		compression = sci0_get_compression_method(fh);
		if (!fh_cached)
			close(fh);

		if (compression == 3)
			return (mgr->sci_version = SCI_VERSION_01_VGA);
	}

	/* Try the same thing with pics */
	for (i=0;i<1000;i++)
	{
		res = scir_test_resource(mgr, sci_pic, i);

		if (!res) continue;

		if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_DIRECTORY ||
		    SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_AUDIO_DIRECTORY)
			continue;

		strcpy(filename, SCIR_SOURCE(res)->location.file.name);
		fh_cached = 0;
#ifdef SCIR_KEEP_VOLUMES_OPEN
		/* Once per view/pic resource at startup: 563 opens of the same few
		   volumes for KQ4 without the cache. */
		if (SCIR_SOURCE(res)->source_type == RESSOURCE_TYPE_VOLUME)
			fh = scir_volume_open(SCIR_SOURCE(res), filename, &fh_cached);
		else
#endif
		fh = open(filename, O_RDONLY | O_BINARY);


		if (!IS_VALID_FD(fh)) {
			char *raiser = filename;
			while (*raiser) {
				*raiser = toupper(*raiser); /* Uppercasify */
				++raiser;
			}
			fh = sci_open(filename, O_RDONLY|O_BINARY);
		}    /* Try case-insensitively name */
		
		if (!IS_VALID_FD(fh)) continue;
		lseek(fh, res->file_offset, SEEK_SET);

		compression = sci0_get_compression_method(fh);
		if (!fh_cached)
			close(fh);

		if (compression == 3)
			return (mgr->sci_version = SCI_VERSION_01_VGA);
	}

	return mgr->sci_version;
}
	

		
int
scir_add_appropriate_sources(resource_mgr_t *mgr,
			     int allow_patches,
			     char *dir)
{
	char *trailing_slash = "";
	char path_separator;
	sci_dir_t dirent;
	char *name;
	resource_source_t *map;
	int fd;
	char fullname[PATH_MAX];

	if (dir[strlen(dir)-1] != G_DIR_SEPARATOR)
	{
		trailing_slash = G_DIR_SEPARATOR_S;
	}

	name = (char *)malloc(strlen(dir) + 1 +
		      strlen("RESOURCE.MAP") + 1);
	
	sprintf(fullname, "%s%s%s", dir, trailing_slash, "RESOURCE.MAP");
	fd = sci_open("RESOURCE.MAP", O_RDONLY | O_BINARY);
	if (!IS_VALID_FD(fd)) return 0;
	close(fd);
	map = scir_add_external_map(mgr, fullname);
	free(name);
	sci_init_dir(&dirent);
	name = sci_find_first(&dirent, "RESOURCE.0??");
	while (name != NULL)
	{
		char *dot = strrchr(name, '.');
		int number = atoi(dot + 1);

		sprintf(fullname, "%s%s%s", dir, G_DIR_SEPARATOR_S, name);
		scir_add_volume(mgr, map, fullname, number, 0);
		name = sci_find_next(&dirent);
	}
	sci_finish_find(&dirent);

	sci_finish_find(&dirent);
	sprintf(fullname, "%s%s", dir, G_DIR_SEPARATOR_S);
	scir_add_patch_dir(mgr, RESSOURCE_TYPE_DIRECTORY, fullname);

	return 1;
}

static int
_scir_scan_new_sources(resource_mgr_t *mgr, int *detected_version, resource_source_t *source)
{
	int preset_version = mgr->sci_version;
	int resource_error = 0;
	int dummy = mgr->sci_version;

	if (detected_version == NULL)
		detected_version = &dummy;

	*detected_version = mgr->sci_version;
	if (source->next)
		_scir_scan_new_sources(mgr, detected_version, source->next);

	if (!source->scanned)
	{
		source->scanned = 1;
		switch (source->source_type)
		{
		case RESSOURCE_TYPE_DIRECTORY:
			if (mgr->sci_version <= SCI_VERSION_01)
				sci0_read_resource_patches(source,
							   &mgr->resources,
							   &mgr->resources_nr);
			else
				sci1_read_resource_patches(source,
							   &mgr->resources,
							   &mgr->resources_nr);
			break;
		case RESSOURCE_TYPE_EXTERNAL_MAP:
			if (preset_version <= SCI_VERSION_01_VGA_ODD
			    /* || preset_version == SCI_VERSION_AUTODETECT -- subsumed by the above line */) {
				resource_error =
					sci0_read_resource_map(mgr, 
							       source,
							       &mgr->resources,
							       &mgr->resources_nr,
							       detected_version);
				
#if 0
				if (resource_error >= SCI_ERROR_CRITICAL) {
					sciprintf("Resmgr: Error while loading resource map: %s\n",
						  sci_error_types[resource_error]);
					if (resource_error == SCI_ERROR_RESMAP_NOT_FOUND)
						sciprintf("Running SCI games without a resource map is not supported ATM\n");
					sci_free(mgr);
					chdir(caller_cwd);
					free(caller_cwd);
					return NULL;
				}
				if (resource_error == SCI_ERROR_RESMAP_NOT_FOUND) {
					/* fixme: Try reading w/o resource.map */
					resource_error = SCI_ERROR_NO_RESOURCE_FILES_FOUND;
				}

				if (resource_error == SCI_ERROR_NO_RESOURCE_FILES_FOUND) {
					/* Initialize empty resource manager */
					_scir_init_trivial(mgr);
					resource_error = 0;
				}
#endif
			}
			
			if ((preset_version == SCI_VERSION_1_EARLY)||
			    (preset_version == SCI_VERSION_1_LATE)||
			    (preset_version == SCI_VERSION_1_1)||
			    ((*detected_version == SCI_VERSION_AUTODETECT)&&(preset_version == SCI_VERSION_AUTODETECT)))
			{
				resource_error =
					sci1_read_resource_map(mgr,
							       source,
							       scir_get_volume(mgr, source, 0),
							       &mgr->resources,
							       &mgr->resources_nr,
							       detected_version);
					
				if (resource_error == SCI_ERROR_RESMAP_NOT_FOUND) {
					/* fixme: Try reading w/o resource.map */
					resource_error = SCI_ERROR_NO_RESOURCE_FILES_FOUND;
				}
				
				if (resource_error == SCI_ERROR_NO_RESOURCE_FILES_FOUND) {
					/* Initialize empty resource manager */
					_scir_init_trivial(mgr);
					resource_error = 0;
				}
				
				*detected_version = SCI_VERSION_1;
			}
			
			mgr->sci_version = *detected_version;
			break;
		}
		qsort(mgr->resources, mgr->resources_nr, sizeof(resource_t),
		      resourcecmp); /* Sort resources */
	}
	return resource_error;
}

int
scir_scan_new_sources(resource_mgr_t *mgr, int *detected_version)
{
	_scir_scan_new_sources(mgr, detected_version, mgr->sources);
}

static void
_scir_free_resource_sources(resource_source_t *rss)
{
	if (rss) {
		_scir_free_resource_sources(rss->next);
		free(rss);
	}
}

#ifdef HAVE_PICO
/* Evicts the SCI01 script resource that holds the decompress scratch, when the
   next user takes the scratch (decompress0.c pico_scratch_take). */
static resource_mgr_t *s_scratch_mgr = NULL;
void scir_evict_resource_data(resource_mgr_t *mgr, resource_t *res);

static void
pico_scratch_evict(resource_t *res)
{
	if (s_scratch_mgr)
		scir_evict_resource_data(s_scratch_mgr, res);
}
#endif

resource_mgr_t *
scir_new_resource_manager(char *dir, int version,
			  char allow_patches, int max_memory)
{
	int resource_error = 0;
	resource_mgr_t *mgr = (resource_mgr_t*)sci_malloc(sizeof(resource_mgr_t));
	char *caller_cwd = sci_getcwd();
	int resmap_version = version;

	if (chdir(dir)) {
		sciprintf("Resmgr: Directory '%s' is invalid!\n", dir);
		free(caller_cwd);
		return NULL;
	}

	mgr->max_memory = max_memory;

	mgr->memory_locked = 0;
	mgr->memory_lru = 0;

	mgr->resource_path = dir;

	mgr->resources = NULL;
	mgr->resources_nr = 0;
	mgr->sources = NULL;
	mgr->sci_version = version;

	scir_add_appropriate_sources(mgr, allow_patches, dir);
	scir_scan_new_sources(mgr, &resmap_version);

	if (!mgr->resources || !mgr->resources_nr) {
		if (mgr->resources) {
			free(mgr->resources);
			mgr->resources = NULL;
		}
		sciprintf("Resmgr: Could not retreive a resource list!\n");
		_scir_free_resource_sources(mgr->sources);
		sci_free(mgr);
		chdir(caller_cwd);
		free(caller_cwd);
		return NULL;
	}

	mgr->lru_first = NULL;
	mgr->lru_last = NULL;

	mgr->allow_patches = allow_patches;

	qsort(mgr->resources, mgr->resources_nr, sizeof(resource_t),
	      resourcecmp); /* Sort resources */

	if (version == SCI_VERSION_AUTODETECT)
		switch (resmap_version) {
		case SCI_VERSION_0:
			if (scir_test_resource(mgr, sci_vocab,
					       VOCAB_RESOURCE_SCI0_MAIN_VOCAB)) {
				version = sci_test_view_type(mgr);
				if (version == SCI_VERSION_01_VGA)
				{
					sciprintf("Resmgr: Detected KQ5 or similar\n");
				} else {
					sciprintf("Resmgr: Detected SCI0\n");
					version = SCI_VERSION_0;
				}
			} else if (scir_test_resource(mgr, sci_vocab,
						      VOCAB_RESOURCE_SCI1_MAIN_VOCAB)) {
				version = sci_test_view_type(mgr);
				if (version == SCI_VERSION_01_VGA)
				{
					sciprintf("Resmgr: Detected KQ5 or similar\n");
				} else {
					if (scir_test_resource(mgr, sci_vocab, 912)) {
						sciprintf("Resmgr: Running KQ1 or similar, using SCI0 resource encoding\n");
						version = SCI_VERSION_0;
					} else {
						version = SCI_VERSION_01;
						sciprintf("Resmgr: Detected SCI01\n");
					}
				}
			} else {
				version = sci_test_view_type(mgr);
				if (version == SCI_VERSION_01_VGA)
				{
					sciprintf("Resmgr: Detected KQ5 or similar\n");
				} else {
					sciprintf("Resmgr: Warning: Could not find vocabulary; assuming SCI0 w/o parser\n");
					version = SCI_VERSION_0;
				}
			} break;
		case SCI_VERSION_01_VGA_ODD:
			version = resmap_version;
			sciprintf("Resmgr: Detected Jones/CD or similar\n");
			break;
		case SCI_VERSION_1:
		{
			resource_t *res = scir_test_resource(mgr, sci_script, 0);
			
			mgr->sci_version = version = SCI_VERSION_1_EARLY;
			_scir_load_resource(mgr, res, 1);
			
			if (res->status == SCI_STATUS_NOMALLOC)
			    mgr->sci_version = version = SCI_VERSION_1_LATE;

			/* No need to handle SCI 1.1 here - it was done in resource_map.c */
			break;
		}
		default:
			sciprintf("Resmgr: Warning: While autodetecting: Couldn't"
				  " determine SCI version!\n");
		}

	if (!resource_error)
	{
#if 0
		if (version <= SCI_VERSION_01)
			sci0_read_resource_patches(dir,
						   &mgr->resources,
						   &mgr->resources_nr);
		else
			sci1_read_resource_patches(dir,
						   &mgr->resources,
						   &mgr->resources_nr);
#endif

		qsort(mgr->resources, mgr->resources_nr, sizeof(resource_t),
		      resourcecmp); /* Sort resources */
	}

	mgr->sci_version = version;

	chdir(caller_cwd);
	free(caller_cwd);

	#ifdef HAVE_PICO
	s_scratch_mgr = mgr;
	g_pico_scratch_evict = pico_scratch_evict;
#endif
	return mgr;
}

static void
_scir_free_altsources(resource_altsource_t *dynressrc)
{
	if (dynressrc) {
		_scir_free_altsources(dynressrc->next);
		free(dynressrc);
	}
}

void
_scir_free_resources(resource_t *resources, int resources_nr)
{
	int i;

	for (i = 0; i < resources_nr; i++) {
		resource_t *res = resources + i;

#ifndef SCIR_PACKED
		_scir_free_altsources(res->alt_sources);
#endif

		if (res->status != SCI_STATUS_NOMALLOC
		    && !PICO_IS_DECOMPRESS_SCRATCH(res->data))
			sci_free(res->data);
	}

	sci_free(resources);
}

void
scir_free_resource_manager(resource_mgr_t *mgr)
{
	_scir_free_resources(mgr->resources, mgr->resources_nr);
#ifdef SCIR_KEEP_VOLUMES_OPEN
	scir_close_volume_fds();   /* before the sources they point at are freed */
#endif
	_scir_free_resource_sources(mgr->sources);
	mgr->resources = NULL;
#ifdef SCIR_PACKED
	scir_sources_nr = 0; /* the sources were just freed */
#endif

	sci_free(mgr);
}


static void
_scir_unalloc(resource_t *res)
{
	if (!PICO_IS_DECOMPRESS_SCRATCH(res->data))
		sci_free(res->data);
	res->data = NULL;
	res->status = SCI_STATUS_NOMALLOC;
}


static void
_scir_remove_from_lru(resource_mgr_t *mgr, resource_t *res)
{
	if (res->status != SCI_STATUS_ENQUEUED) {
		sciprintf("Resmgr: Oops: trying to remove resource that isn't"
			  " enqueued\n");
		return;
	}

	{
		resource_t *next = SCIR_LRU_NEXT(mgr, res);
		resource_t *prev = SCIR_LRU_PREV(mgr, res);

		if (next)
			SCIR_LRU_SET_PREV(mgr, next, prev);
		if (prev)
			SCIR_LRU_SET_NEXT(mgr, prev, next);
		if (mgr->lru_first == res)
			mgr->lru_first = next;
		if (mgr->lru_last == res)
			mgr->lru_last = prev;
	}

	mgr->memory_lru -= res->size;

	res->status = SCI_STATUS_ALLOCATED;
}

static void
_scir_add_to_lru(resource_mgr_t *mgr, resource_t *res)
{
	if (res->status != SCI_STATUS_ALLOCATED) {
		sciprintf("Resmgr: Oops: trying to enqueue resource with state"
			  " %d\n", res->status);
		return;
	}

	SCIR_LRU_SET_PREV(mgr, res, NULL);
	SCIR_LRU_SET_NEXT(mgr, res, mgr->lru_first);
	mgr->lru_first = res;
	if (!mgr->lru_last)
		mgr->lru_last = res;
	if (SCIR_LRU_NEXT(mgr, res))
		SCIR_LRU_SET_PREV(mgr, SCIR_LRU_NEXT(mgr, res), res);

	mgr->memory_lru += res->size;
#if (SCI_VERBOSE_RESMGR > 1)
	fprintf(stderr, "Adding %s.%03d (%d bytes) to lru control: %d bytes total\n",
		sci_resource_types[res->type], res->number, res->size,
		mgr->memory_lru);

#endif

	res->status = SCI_STATUS_ENQUEUED;
}

static void
_scir_print_lru_list(resource_mgr_t *mgr)
{
	int mem = 0;
	int entries = 0;
	resource_t *res = mgr->lru_first;

	while (res) {
		fprintf(stderr,"\t%s.%03d: %d bytes\n",
			sci_resource_types[res->type], res->number,
			res->size);
		mem += res->size;
		++entries;
		res = SCIR_LRU_NEXT(mgr, res);
	}

	fprintf(stderr,"Total: %d entries, %d bytes (mgr says %d)\n",
		entries, mem, mgr->memory_lru);
}

static void
_scir_free_old_resources(resource_mgr_t *mgr, int last_invulnerable)
{
	while (mgr->max_memory < mgr->memory_lru
	       && (!last_invulnerable || mgr->lru_first != mgr->lru_last)) {
		resource_t *goner = mgr->lru_last;
		if (!goner) {
			fprintf(stderr,"Internal error: mgr->lru_last is NULL!\n");
			fprintf(stderr,"LRU-mem= %d\n", mgr->memory_lru);
			fprintf(stderr,"lru_first = %p\n", (void *)mgr->lru_first);
			_scir_print_lru_list(mgr);
		}

		_scir_remove_from_lru(mgr, goner);
		_scir_unalloc(goner);
#ifdef SCI_VERBOSE_RESMGR
		sciprintf("Resmgr-debug: LRU: Freeing %s.%03d (%d bytes)\n",
			  sci_resource_types[goner->type], goner->number,
			  goner->size);
#endif
	}
}

resource_t *
scir_find_resource(resource_mgr_t *mgr, int type, int number, int lock)
{
	resource_t *retval;

	if (number >= sci_max_resource_nr[mgr->sci_version]) {
		int modded_number = number % sci_max_resource_nr[mgr->sci_version];
		sciprintf("[resmgr] Requested invalid resource %s.%d, mapped to %s.%d\n",
			  sci_resource_types[type], number,
			  sci_resource_types[type], modded_number);
		number = modded_number;
	}

	retval = scir_test_resource(mgr, type, number);

	if (!retval)
		return NULL;

	if (!retval->status)
		_scir_load_resource(mgr, retval, 0);

	else if (retval->status == SCI_STATUS_ENQUEUED)
		_scir_remove_from_lru(mgr, retval);
	/* Unless an error occured, the resource is now either
	** locked or allocated, but never queued or freed.  */

	if (lock) {
		if (retval->status == SCI_STATUS_ALLOCATED) {
			retval->status = SCI_STATUS_LOCKED;
			retval->lockers = 0;
			mgr->memory_locked += retval->size;
		}

		++retval->lockers;

	} else if (retval->status != SCI_STATUS_LOCKED) { /* Don't lock it */
		if (retval->status == SCI_STATUS_ALLOCATED)
			_scir_add_to_lru(mgr, retval);
	}

	/* Protect the just-added resource from immediate self-eviction.
	   After _scir_add_to_lru the status is ENQUEUED, not ALLOCATED, so we
	   check ENQUEUED here.  LOCKED resources are never in the LRU list, so
	   passing 0 for them is harmless. */
	_scir_free_old_resources(mgr, retval->status == SCI_STATUS_ENQUEUED);

	if (retval->data)
		return retval;
	else {
		sciprintf("Resmgr: Failed to read %s.%03d\n",
			  sci_resource_types[retval->type], retval->number);
		return NULL;
	}
}

#ifdef HAVE_PICO
void
scir_evict_resource_data(resource_mgr_t *mgr, resource_t *res)
{
	if (!res || !res->data)
		return;
	if (res->status == SCI_STATUS_ENQUEUED)
		_scir_remove_from_lru(mgr, res);
	if (!PICO_IS_DECOMPRESS_SCRATCH(res->data))
		sci_free(res->data);
	res->data = NULL;
	res->status = SCI_STATUS_NOMALLOC;
}

#if defined(PICO_STREAM_DECOMPRESS) && !defined(PICO_PSRAM_MAPPED)
/* Load a resource's decompressed bytes straight into PSRAM at 'addr' (a fixed
   staging slot of max_size bytes) instead of res->data: for a VGA view of up to ~35 KB that has to be decoded
   but never needs an SRAM copy (gfxr_draw_view1_psram reads it through a
   cache). Handles only volume-sourced resources of the SCI01 family (the
   decompress01 games) that are not already loaded, with method 0 or 2; the
   output is staged through the idle decompress scratch. Returns 0 and fills
   *size, or -1 when not handled -- the caller then loads it normally. */
int
scir_pico_load_to_psram(resource_mgr_t *mgr, int type, int number,
			uint32_t addr, unsigned int max_size, int *size)
{
	extern int pico_decompress01_to_psram(int resh, int method, unsigned int complength,
					      int size, uint32_t addr, guint8 *stage, int stage_size);
	resource_t *res = scir_test_resource(mgr, type, number);
	char filename[PATH_MAX];
	guint8 hdr[8];
	int fh, fh_cached = 0, rc;
	unsigned int clen, dsz, method, id;

	if (!res || res->status || !g_pico_decompress_scratch
	    || decompressors[mgr->sci_version] != &decompress01
	    || SCIR_SOURCE(res)->source_type != RESSOURCE_TYPE_VOLUME)
		return -1;

	strcpy(filename, SCIR_SOURCE(res)->location.file.name);
#ifdef SCIR_KEEP_VOLUMES_OPEN
	fh = scir_volume_open(SCIR_SOURCE(res), filename, &fh_cached);
#else
	fh = open(filename, O_RDONLY | O_BINARY);
#endif
	if (!IS_VALID_FD(fh))
		return -1;
	lseek(fh, res->file_offset, SEEK_SET);
	if (read(fh, hdr, 8) != 8) {
		if (!fh_cached)
			close(fh);
		return -1;
	}
	id = hdr[0] | (hdr[1] << 8);
	clen = hdr[2] | (hdr[3] << 8);
	dsz = hdr[4] | (hdr[5] << 8);
	method = hdr[6] | (hdr[7] << 8);
	if ((id >> 11) != (unsigned) type || (id & 0x7ff) != (unsigned) number
	    || clen <= 4 || (method != 0 && method != 2) || dsz > max_size) {
		if (!fh_cached)
			close(fh);
		return -1;
	}

	pico_scratch_take(); /* the scratch is the staging buffer below */
	rc = pico_decompress01_to_psram(fh, method, clen - 4, dsz, addr,
					g_pico_decompress_scratch, PICO_DECOMPRESS_SCRATCH_SIZE);
	if (!fh_cached)
		close(fh);
	if (rc) {
		sciprintf("Error %d occured while reading %s.%03d to PSRAM\n",
			  rc, sci_resource_types[type], number);
		return -1;
	}
	*size = (int) dsz;
	return 0;
}
#endif /* PICO_STREAM_DECOMPRESS && !PICO_PSRAM_MAPPED */

void
scir_free_all_lru(resource_mgr_t *mgr)
{
	while (mgr->lru_last) {
		resource_t *goner = mgr->lru_last;
		_scir_remove_from_lru(mgr, goner); /* sets status = ALLOCATED */
		if (!PICO_IS_DECOMPRESS_SCRATCH(goner->data))
			sci_free(goner->data);
		goner->data = NULL;
		goner->status = SCI_STATUS_NOMALLOC;
	}
}
#endif

void
scir_unlock_resource(resource_mgr_t *mgr, resource_t *res, int resnum, int restype)
{
	if (!res) {
		sciprintf("Resmgr: Warning: Attempt to unlock non-existant"
			  " resource %s.%03d!\n",
			  sci_resource_types[restype], resnum);
		return;
	}

	if (res->status != SCI_STATUS_LOCKED) {
		sciprintf("Resmgr: Warning: Attempt to unlock unlocked"
			  " resource %s.%03d\n",
			  sci_resource_types[res->type], res->number);
		return;
	}

	if (!--res->lockers) { /* No more lockers? */
		res->status = SCI_STATUS_ALLOCATED;
		mgr->memory_locked -= res->size;
		_scir_add_to_lru(mgr, res);
	}

	_scir_free_old_resources(mgr, 0);
}

