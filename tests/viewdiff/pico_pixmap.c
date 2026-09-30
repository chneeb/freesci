/* Compiled WITH -DHAVE_PICO (the pixmap layout pico_view1.o uses); everything
   else in viewdiff is a desktop compile, because other structs (resource_t)
   differ under HAVE_PICO too. Linked with -Wl,--wrap=gfx_new_pixmap: the desktop
   library allocates the shorter desktop pixmap, so grow each one to the Pico
   layout with a zeroed tail (the psram fields pico_view1.o writes). */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <gfx_system.h>

gfx_pixmap_t *__real_gfx_new_pixmap(int xl, int yl, int resid, int loop, int cel);

gfx_pixmap_t *
__wrap_gfx_new_pixmap(int xl, int yl, int resid, int loop, int cel)
{
	gfx_pixmap_t *p = __real_gfx_new_pixmap(xl, yl, resid, loop, cel);
	size_t off = offsetof(gfx_pixmap_t, psram_addr);

	p = (gfx_pixmap_t *) realloc(p, sizeof(gfx_pixmap_t));
	memset((char *) p + off, 0, sizeof(gfx_pixmap_t) - off);
	return p;
}

/* 1 and the PSRAM address if the cel was offloaded */
int
vd_cel_psram(gfx_pixmap_t *p, uint32_t *addr)
{
	*addr = p->psram_addr;
	return p->psram_valid;
}
