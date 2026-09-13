/* decompdiff -- decompress EVERY resource of a game twice, once through the
   stock decompress0 and once through the streaming variant, and compare the
   output byte for byte.

   WHY THIS EXISTS BEFORE A SINGLE LINE OF STREAMING IS WRITTEN: decompress0 is
   on the path of every single resource in the game -- scripts, pics, views,
   vocab, sound. A bug here does not corrupt one picture, it corrupts
   everything, and it would surface on device as an unrelated-looking fault a
   long way from the cause. Every harness in this series has earned its keep
   (picodiff localised the KQ4 control-map flood entirely offline; drvdiff
   produced three FALSE PASSES from a scene that did not walk the paths under
   test), so the standard here is every resource of every game, not a sample.

   WHAT IS BEING PROVEN: that a sliding window over the fd feeds all three SCI0
   compression methods identically to one big contiguous read.
     method 0  straight copy
     method 1  decrypt1, strictly forward, <=2 bytes of lookahead
     method 2  decrypt2, forward bitstream + a Huffman node table that is the
               only random access and is bounded at 510 bytes (numnodes is
               src[0], a byte)

   HOW: the second copy of decompress0.c is compiled with
   -DPICO_STREAM_DECOMPRESS=1 and -include streamprefix.h, which renames its
   entry points to stream_*, so both implementations link into one binary and
   run against the same file offsets. Same trick as tests/picodiff.

   Note the streaming path is gated on PICO_STREAM_DECOMPRESS, NOT HAVE_PICO,
   precisely so it can be built and proven here on the desktop.

     cd tests/decompdiff
     cp ../../src/scicore/decompress0.c stream_decomp.c
     gcc -c -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
         -DPICO_STREAM_DECOMPRESS=1 -include streamprefix.h \
         -I. -I../../src/include -I../../build -I/usr/include/SDL2 -D_REENTRANT \
         -o stream_decomp.o stream_decomp.c
     LIBS=$(find ../../build -name '*.a' | tr '\n' ' ')
     gcc -fgnu89-inline -w -DHAVE_CONFIG_H=1 -DX_DISPLAY_MISSING=1 \
         -I../../src/include -I../../build -I/usr/include/SDL2 -D_REENTRANT \
         -o decompdiff decompdiff_main.c stream_decomp.o \
         -Wl,--start-group $LIBS -Wl,--end-group -lSDL2 -lm -lz -lpthread -ldl
     ./decompdiff ~/Downloads/quest/sq3 ~/Downloads/quest/pq2 ~/Downloads/quest/kq4

   Until the streaming path is written both copies are identical and this
   reports 0 differing -- which is the harness validating itself, exactly as
   visdiff did before the packing work began.
*/
#include <sciresource.h>
#include <sci_memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#ifndef O_BINARY
#  define O_BINARY 0
#endif

int decompress0(resource_t *result, int resh, int sci_version);
int stream_decompress0(resource_t *result, int resh, int sci_version);

/* Reproduce _scir_load_resource's open+lseek (resource.c ~line 290) for a
   VOLUME-sourced resource, then hand the fd to one decompressor. Patch-file
   sources are excluded by the caller: those go through
   _scir_load_from_patch_file and never reach decompress0, so including them
   would test a path that does not exist. */
static int
load_via(resource_t *res, int version, int use_stream,
	 unsigned char **data_out, unsigned int *size_out)
{
	resource_t work;
	char filename[PATH_MAX];
	int fh, rc;

	strcpy(filename, res->source->location.file.name);
	fh = open(filename, O_RDONLY | O_BINARY);
	if (fh < 0) {
		char *r = filename;
		while (*r) { *r = toupper(*r); ++r; }
		fh = open(filename, O_RDONLY | O_BINARY);
	}
	if (fh < 0)
		return -1;

	/* A fresh resource_t per run: decompress0 writes id/number/type/size/
	   status/data into it, and reusing one would let the first run's results
	   leak into the second and mask a difference. */
	memset(&work, 0, sizeof work);
	work.number = res->number;
	work.type = res->type;

	lseek(fh, res->file_offset, SEEK_SET);
	rc = use_stream ? stream_decompress0(&work, fh, version)
			: decompress0(&work, fh, version);
	close(fh);

	*data_out = work.data;
	*size_out = work.size;
	return rc;
}

static int
run_game(const char *dir, int *checked_out, int *skipped_out)
{
	resource_mgr_t *resmgr;
	int i, bad = 0, checked = 0, skipped = 0;
	long total_bytes = 0;
	int method_seen[8];
	int method_max[8], method_max_num[8], method_max_type[8];
	int last_method = -1;
	unsigned int last_clen = 0;

	memset(method_seen, 0, sizeof method_seen);
	memset(method_max, 0, sizeof method_max);
	memset(method_max_num, 0, sizeof method_max_num);
	memset(method_max_type, 0, sizeof method_max_type);

	if (chdir(dir)) { fprintf(stderr, "  !! cannot chdir %s\n", dir); return -1; }
	resmgr = scir_new_resource_manager(sci_getcwd(), SCI_VERSION_AUTODETECT,
					   1, 1024 * 128);
	if (!resmgr) { fprintf(stderr, "  !! no resources in %s\n", dir); return -1; }

	/* decompress0 is the SCI0 decompressor; on a later-version game the
	   resmgr dispatches elsewhere (decompress01/1/11) and this comparison
	   would be meaningless. */
	if (resmgr->sci_version != SCI_VERSION_0) {
		printf("  skipped: not SCI0 (version %d)\n", resmgr->sci_version);
		scir_free_resource_manager(resmgr);
		*checked_out = 0; *skipped_out = 0;
		return 0;
	}

	for (i = 0; i < resmgr->resources_nr; i++) {
		resource_t *res = resmgr->resources + i;
		unsigned char *a = NULL, *b = NULL;
		unsigned int sa = 0, sb = 0;
		int ra, rb;

		if (!res->source ||
		    res->source->source_type != RESSOURCE_TYPE_VOLUME) {
			skipped++;
			continue;
		}

		/* Record which compression method this resource uses. A pass means
		   nothing for a method the corpus never exercises -- drvdiff gave
		   three false passes for exactly that reason -- so the coverage is
		   reported, not assumed. Header layout per decompress0: id,
		   compressedLength, result_size, compressionMethod, all 16-bit LE. */
		{
			int hf = open(res->source->location.file.name, O_RDONLY | O_BINARY);
			unsigned char hdr[8];

			if (hf >= 0) {
				lseek(hf, res->file_offset, SEEK_SET);
				if (read(hf, hdr, 8) == 8) {
					int m = hdr[6] | (hdr[7] << 8);
					unsigned int clen = hdr[2] | (hdr[3] << 8);
					last_method = m;
					if (clen > 4) last_clen = clen - 4; else last_clen = 0;
					if (clen > 4) clen -= 4;
					if (m >= 0 && m < 8 && clen > (unsigned)method_max[m]) {
						method_max[m] = clen;
						method_max_num[m] = res->number;
						method_max_type[m] = res->type;
					}
					if (m >= 0 && m < 8)
						method_seen[m]++;
				}
				close(hf);
			}
		}

		if (getenv("METHODS"))
			printf("    %s.%03d method=%d clen=%u\n",
			       sci_resource_types[res->type], res->number,
			       last_method, last_clen);

		ra = load_via(res, resmgr->sci_version, 0, &a, &sa);
		rb = load_via(res, resmgr->sci_version, 1, &b, &sb);

		if (ra < 0 && rb < 0) { skipped++; continue; }

		checked++;
		total_bytes += sa;

		/* Return code, size and bytes all have to agree. Comparing only
		   the bytes would let a truncated decode pass whenever the tail
		   happened to be zero. */
		if (ra != rb) {
			printf("  %s.%03d  RC differs: stock=%d stream=%d\n",
			       sci_resource_types[res->type], res->number, ra, rb);
			bad++;
		} else if (sa != sb) {
			printf("  %s.%03d  SIZE differs: stock=%u stream=%u\n",
			       sci_resource_types[res->type], res->number, sa, sb);
			bad++;
		} else if (ra == 0 && a && b && memcmp(a, b, sa)) {
			unsigned int j, first = 0, ndiff = 0;
			for (j = 0; j < sa; j++)
				if (a[j] != b[j]) {
					if (!ndiff) first = j;
					ndiff++;
				}
			printf("  %s.%03d  DATA differs: %u/%u bytes, first at %u "
			       "(stock %02x vs stream %02x)\n",
			       sci_resource_types[res->type], res->number,
			       ndiff, sa, first, a[first], b[first]);
			bad++;
		} else if (ra == 0 && (!a || !b)) {
			printf("  %s.%03d  one side returned NULL data (stock=%p stream=%p)\n",
			       sci_resource_types[res->type], res->number,
			       (void *)a, (void *)b);
			bad++;
		}

		if (a) free(a);
		if (b) free(b);
	}

	printf("  %d resources compared (%ld bytes), %d skipped, %d differ\n",
	       checked, total_bytes, skipped, bad);
	printf("  methods exercised:");
	for (i = 0; i < 8; i++)
		if (method_seen[i])
			printf("  %d=%d", i, method_seen[i]);
	printf("\n");
	/* The LARGEST compressed block per method is what decides whether
	   streaming that method is worth anything: the point of the exercise is
	   removing a big CONTIGUOUS allocation, not a small one. */
	for (i = 0; i < 8; i++)
		if (method_seen[i])
			printf("  method %d: largest input %d bytes (%s.%03d)\n",
			       i, method_max[i],
			       sci_resource_types[method_max_type[i]], method_max_num[i]);
	scir_free_resource_manager(resmgr);
	*checked_out = checked;
	*skipped_out = skipped;
	return bad;
}

int
main(int argc, char **argv)
{
	int g, total = 0, total_bad = 0, total_skipped = 0;
	char cwd[PATH_MAX];

	setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: a crash must not eat the trace */
	if (argc < 2) {
		fprintf(stderr, "usage: %s <gamedir> [gamedir...]\n", argv[0]);
		return 2;
	}
	if (!getcwd(cwd, sizeof cwd)) { perror("getcwd"); return 1; }

	for (g = 1; g < argc; g++) {
		int checked = 0, skipped = 0, bad;

		printf("%s\n", argv[g]);
		if (chdir(cwd)) { perror("chdir"); return 1; }   /* paths are relative-safe */
		bad = run_game(argv[g], &checked, &skipped);
		if (bad < 0) continue;
		total += checked;
		total_skipped += skipped;
		total_bad += bad;
	}

	printf("\n== %d resources compared across %d games; %d differ ==\n",
	       total, argc - 1, total_bad);
	if (total_skipped)
		printf("   (%d skipped: patch-file sources, which never reach decompress0)\n",
		       total_skipped);
	printf("   %s\n", total_bad == 0
	       ? "streaming decompress is byte-identical to the stock path."
	       : "MISMATCH: the streaming path does not reproduce the stock output.");
	return total_bad != 0;
}
