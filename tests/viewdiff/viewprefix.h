/* Rename the Pico-configured copy of sci_view_1.c so it links next to the
   desktop one in libscigfxresources. -include'd on the command line. */
#ifndef VIEWPREFIX_H
#define VIEWPREFIX_H
#include <stdint.h>
#define gfxr_draw_cel1        pico_gfxr_draw_cel1
#define gfxr_draw_view1       pico_gfxr_draw_view1
#define gfxr_draw_view11      pico_gfxr_draw_view11
#define gfxr_draw_loop1       pico_gfxr_draw_loop1
#define gfxr_draw_cel11       pico_gfxr_draw_cel11
#define gfxr_draw_loop11      pico_gfxr_draw_loop11
#endif
