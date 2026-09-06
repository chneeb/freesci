#include <stdint.h>
/* Rename every externally-visible symbol so this second (Pico-configured) copy
   of sci_pic_0.c can link alongside the normal desktop one. */
#define gfxr_init_pic              pico_gfxr_init_pic
#define gfxr_clear_pic0            pico_gfxr_clear_pic0
#define gfxr_draw_pic01            pico_gfxr_draw_pic01
#define gfxr_draw_pic11            pico_gfxr_draw_pic11
#define gfxr_free_pic              pico_gfxr_free_pic
#define gfxr_init_static_palette   pico_gfxr_init_static_palette
#define gfxr_remove_artifacts_pic0 pico_gfxr_remove_artifacts_pic0
#define gfxr_dither_pic0           pico_gfxr_dither_pic0
#define gfx_sci0_pic_colors        pico_gfx_sci0_pic_colors
#define gfx_sci0_image_colors      pico_gfx_sci0_image_colors
#define embedded_view_colors       pico_embedded_view_colors
#define sci0_palette               pico_sci0_palette
#define pico_picdec_cache_begin    pico_pdc_begin_x
#define pico_picdec_cache_end      pico_pdc_end_x
