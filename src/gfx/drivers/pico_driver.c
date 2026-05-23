/***************************************************************************
 pico_driver.c - FreeSCI gfx driver for PicoCalc (RP2350 + ILI9488 TFT)

 Renders SCI0 320x200 8bpp palette-indexed frames to the 320x320 ILI9488
 display (centered with 60-pixel top/bottom margins) via SPI.
 Keyboard input is read from the I2C PicoCalc keyboard via kbd_input.h.
 No mouse support (PicoCalc has no pointing device).
***************************************************************************/

#include <sci_memory.h>
#include <gfx_driver.h>

#ifdef HAVE_PICO

#include <gfx_tools.h>
#include <gfx_resource.h>
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/spi.h"
#include "lcdspi.h"
#include "kbd_input.h"
#include "psram_alloc.h"

/* define_region_spi is in lcdspi.c but not declared in lcdspi.h */
extern void define_region_spi(int xstart, int ystart, int xend, int yend, int rw);
/* hw_send_spi / spi_finish / lcd_spi_raise_cs declared in lcdspi.h */

#define TFT_Y_OFFSET   60    /* centre 320x200 in the 320x320 panel */
#define PICO_XSIZE     320
#define PICO_YSIZE     200

#define PICO_HANDLE_NORMAL  0
#define PICO_HANDLE_GRABBED 1

#define EVT_BUF_SIZE 8

struct _pico_state {
    uint8_t        *visual[1];       /* [0]=back/front (drawing+display) */
    /* priority buffer removed — uses engine's priority_map via s_shared_priority */
    uint8_t         palette[256][3]; /* R,G,B for each colour index */
    gfx_pixmap_t   *static_bg;       /* current room's visual_map (PSRAM-backed) */

    /* keyboard event ring buffer */
    sci_event_t     evbuf[EVT_BUF_SIZE];
    int             ev_head, ev_tail;
};

/* Shared engine priority_map set by pico_connect_engine_priority() after GFX init */
static gfx_pixmap_t *s_shared_priority = NULL;

void pico_connect_engine_priority(gfx_pixmap_t *priority_map)
{
    s_shared_priority = priority_map;
}

void pico_free_visual(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (ps && ps->visual[0]) {
        sci_free(ps->visual[0]);
        ps->visual[0] = NULL;
    }
}

void pico_alloc_visual(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (ps && !ps->visual[0]) {
        ps->visual[0] = (uint8_t *)sci_malloc(PICO_XSIZE * PICO_YSIZE);
        if (ps->visual[0])
            memset(ps->visual[0], 0, PICO_XSIZE * PICO_YSIZE);
    }
}

/* Ensure visual[0] is allocated; returns 1 on success, 0 on OOM. */
static int pico_ensure_visual(gfx_driver_t *drv)
{
    pico_alloc_visual(drv);
    return ((struct _pico_state *)drv->state)->visual[0] != NULL;
}

#define S  ((struct _pico_state *)(drv->state))

/* Scratch row for palette→RGB24 conversion (960 bytes) */
static uint8_t line_buf[PICO_XSIZE * 3];

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

static int pico_map_key(int key)
{
    if (key >= ' ' && key <= '~')
        return key;

    switch (key) {
    case 0x08: return SCI_K_BACKSPACE;
    case 0x09: return SCI_K_TAB;
    case 0x0A: return SCI_K_ENTER;
    case 0xB1: return SCI_K_ESC;
    case 0xB4: return SCI_K_LEFT;
    case 0xB5: return SCI_K_UP;
    case 0xB6: return SCI_K_DOWN;
    case 0xB7: return SCI_K_RIGHT;
    case 0xD1: return SCI_K_INSERT;
    case 0xD2: return SCI_K_HOME;
    case 0xD4: return SCI_K_DELETE;
    case 0x81: return SCI_K_F1;
    case 0x82: return SCI_K_F2;
    case 0x83: return SCI_K_F3;
    case 0x84: return SCI_K_F4;
    case 0x85: return SCI_K_F5;
    case 0x86: return SCI_K_F6;
    case 0x87: return SCI_K_F7;
    case 0x88: return SCI_K_F8;
    case 0x89: return SCI_K_F9;
    case 0x90: return SCI_K_F10;
    default:   return 0;
    }
}

static void push_event(struct _pico_state *ps, sci_event_t ev)
{
    int next = (ps->ev_head + 1) % EVT_BUF_SIZE;
    if (next != ps->ev_tail) {
        ps->evbuf[ps->ev_head] = ev;
        ps->ev_head = next;
    }
}

static sci_event_t pop_event(struct _pico_state *ps)
{
    sci_event_t ev = { SCI_EVT_NONE, 0, 0 };
    if (ps->ev_head != ps->ev_tail) {
        ev = ps->evbuf[ps->ev_tail];
        ps->ev_tail = (ps->ev_tail + 1) % EVT_BUF_SIZE;
    }
    return ev;
}

/* Poll the I2C keyboard once; push a sci_event_t if a mapped key arrived. */
static void poll_keyboard(struct _gfx_driver *drv)
{
    int key = kbd_read();
    if (key <= 0) return;

    printf("[kbd] raw=0x%02x\n", key); stdio_flush();

    int sci_key = pico_map_key(key);
    if (!sci_key) {
        printf("[kbd] unmapped\n"); stdio_flush();
        return;
    }

    printf("[kbd] sci_key=%d\n", sci_key); stdio_flush();

    sci_event_t ev;
    ev.type      = SCI_EVT_KEYBOARD;
    ev.data      = sci_key;
    ev.buckybits = 0;
    push_event(S, ev);
}

/* ------------------------------------------------------------------ */
/* Display flush                                                        */
/* ------------------------------------------------------------------ */

static void flush_region(struct _pico_state *ps,
                          int x, int y, int w, int h)
{
    if (!ps->visual[0]) return; /* not yet allocated — nothing to display */
    define_region_spi(x, y + TFT_Y_OFFSET,
                      x + w - 1, y + TFT_Y_OFFSET + h - 1, 1);

    for (int row = 0; row < h; row++) {
        const uint8_t *src = ps->visual[0] + (y + row) * PICO_XSIZE + x;
        for (int col = 0; col < w; col++) {
            uint8_t idx = src[col];
            line_buf[col * 3    ] = ps->palette[idx][0];
            line_buf[col * 3 + 1] = ps->palette[idx][1];
            line_buf[col * 3 + 2] = ps->palette[idx][2];
        }
        hw_send_spi(line_buf, w * 3);
    }
    spi_finish(spi1);
    lcd_spi_raise_cs();
}

/* ------------------------------------------------------------------ */
/* Driver callbacks                                                    */
/* ------------------------------------------------------------------ */

static int pico_set_parameter(struct _gfx_driver *drv,
                               char *attribute, char *value)
{
    (void)drv; (void)attribute; (void)value;
    return GFX_ERROR; /* no configurable parameters */
}

static int pico_init_specific(struct _gfx_driver *drv,
                               int xfact, int yfact, int bytespp)
{
    int xsize, ysize, i = 0;

    /* PicoCalc TFT is exactly 320×200 game area — only 1×1 makes sense */
    if (xfact != 1 || yfact != 1 || bytespp != 1) {
        fprintf(stderr, "pico_driver: only 1x1 @ 8bpp supported\n");
        return GFX_FATAL;
    }

    xsize = PICO_XSIZE;
    ysize = PICO_YSIZE;

    if (!drv->state) {
        drv->state = sci_malloc(sizeof(struct _pico_state));
        if (!drv->state) return GFX_FATAL;
        memset(drv->state, 0, sizeof(struct _pico_state));
    }

    /* Allocate one 320×200 palette-indexed visual buffer (back/front combined).
       Priority buffer is not allocated here — after GFX init, pico_connect_engine_priority()
       wires the engine's state->priority_map directly, saving 64KB of heap. */
    S->visual[0] = (uint8_t *)sci_malloc(xsize * ysize);
    if (!S->visual[0]) {
        fprintf(stderr, "pico_driver: OOM allocating visual[0]\n");
        return GFX_FATAL;
    }
    memset(S->visual[0], 0, xsize * ysize);

    /* Default EGA palette */
    static const uint8_t ega16[16][3] = {
        {  0,  0,  0}, {  0,  0,168}, {  0,168,  0}, {  0,168,168},
        {168,  0,  0}, {168,  0,168}, {168, 84,  0}, {168,168,168},
        { 84, 84, 84}, { 84, 84,255}, { 84,255, 84}, { 84,255,255},
        {255, 84, 84}, {255, 84,255}, {255,255, 84}, {255,255,255},
    };
    for (i = 0; i < 16; i++)
        memcpy(S->palette[i], ega16[i], 3);

    drv->mode = gfx_new_mode(1, 1, 1,
                              0, 0, 0, 0,  /* masks/shifts (palette mode) */
                              0, 0, 0, 0,
                              256, 0);
    return GFX_OK;
}

static void pico_clear_screen_black(void)
{
    static uint8_t black[PICO_XSIZE * 3];
    memset(black, 0, sizeof(black));
    define_region_spi(0, 0, PICO_XSIZE - 1, 319, 1);
    for (int row = 0; row < 320; row++)
        hw_send_spi(black, PICO_XSIZE * 3);
    spi_finish(spi1);
    lcd_spi_raise_cs();
}

static int pico_init(struct _gfx_driver *drv)
{
    lcd_init();
    spi_set_baudrate(spi1, 50000000);
    kbd_input_init();
    pico_clear_screen_black();
    return pico_init_specific(drv, 1, 1, 1);
}

static void pico_exit(struct _gfx_driver *drv)
{
    if (!S) return;

    if (S->visual[0]) { sci_free(S->visual[0]); S->visual[0] = NULL; }
    /* priority buffer is owned by the engine (s_shared_priority), not freed here */
    s_shared_priority = NULL;
    sci_free(drv->state);
    drv->state = NULL;
}

/* Called from gfxop_new_pic after gfxr_get_pic populates gfx_sci0_pic_colors[].
   Fills ps->palette[0..255] so flush_region produces correct RGB output. */
void pico_setup_sci0_palette(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (!ps) return;
    for (int i = 0; i < GFX_SCI0_PIC_COLORS_NR; i++) {
        ps->palette[i][0] = gfx_sci0_pic_colors[i].r;
        ps->palette[i][1] = gfx_sci0_pic_colors[i].g;
        ps->palette[i][2] = gfx_sci0_pic_colors[i].b;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static uint8_t pico_map_color(gfx_driver_t *drv, gfx_color_t color)
{
    (void)drv;
    return (uint8_t)color.visual.global_index;
}

/* Bresenham line — writes directly into a raw byte buffer */
static void draw_line_raw(uint8_t *buf, int pitch,
                           int x1, int y1, int x2, int y2, uint8_t color)
{
    int dx = x2 - x1, dy = y2 - y1;
    int sx = (dx >= 0) ? 1 : -1;
    int sy = (dy >= 0) ? 1 : -1;
    dx = sx * dx + 1;
    dy = sy * dy + 1;

    int x = 0, y = 0;
    uint8_t *pixel = buf + y1 * pitch + x1;
    int pixx = sx, pixy = sy * pitch;

    if (dx < dy) {
        int tmp;
        tmp = dx; dx = dy; dy = tmp;
        tmp = pixx; pixx = pixy; pixy = tmp;
    }

    for (; x < dx; x++, pixel += pixx) {
        *pixel = color;
        y += dy;
        if (y >= dx) { y -= dx; pixel += pixy; }
    }
}

static int pico_draw_line(struct _gfx_driver *drv,
                           point_t start, point_t end,
                           gfx_color_t color,
                           gfx_line_mode_t line_mode,
                           gfx_line_style_t line_style)
{
    (void)line_style;
    int xfact = (line_mode == GFX_LINE_MODE_FINE) ? 1 : 1; /* always 1x */
    int yfact = xfact;

    if (color.mask & GFX_MASK_VISUAL) {
        if (!pico_ensure_visual(drv)) return GFX_ERROR;
        uint8_t c = pico_map_color(drv, color);
        int xc, yc;
        for (xc = 0; xc < xfact; xc++)
            for (yc = 0; yc < yfact; yc++)
                draw_line_raw(S->visual[0], PICO_XSIZE,
                              start.x + xc, start.y + yc,
                              end.x   + xc, end.y   + yc, c);
    }
    if ((color.mask & GFX_MASK_PRIORITY) && s_shared_priority)
        gfx_draw_line_pixmap_i(s_shared_priority, start, end, color.priority);

    return GFX_OK;
}

static int pico_draw_filled_rect(struct _gfx_driver *drv, rect_t rect,
                                  gfx_color_t color1, gfx_color_t color2,
                                  gfx_rectangle_fill_t shade_mode)
{
    (void)color2; (void)shade_mode;

    if (color1.mask & GFX_MASK_VISUAL) {
        if (!pico_ensure_visual(drv)) return GFX_ERROR;
        uint8_t c = pico_map_color(drv, color1);
        for (int row = rect.y; row < rect.y + rect.yl; row++)
            memset(S->visual[0] + row * PICO_XSIZE + rect.x, c, rect.xl);
    }
    if ((color1.mask & GFX_MASK_PRIORITY) && s_shared_priority)
        gfx_draw_box_pixmap_i(s_shared_priority, rect, color1.priority);

    return GFX_OK;
}

/* ------------------------------------------------------------------ */
/* Indexed blit: translate index_data+colors directly to visual buf   */
/* Used when pxm->data is NULL (skipped in gfx_xlate_pixmap for Pico) */
/* ------------------------------------------------------------------ */

static uint8_t s_psram_row[PICO_XSIZE]; /* scratch row for PSRAM reads */

static void
pico_blit_indexed(gfx_pixmap_t *pxm, int priority,
                  rect_t src, rect_t dest,
                  uint8_t *destbuf,    /* already homed to (dest.x, dest.y) */
                  int dest_stride,
                  uint8_t *pri_buf,    /* priority index_data, or NULL */
                  int pri_stride)
{
    int xl = src.xl, yl = src.yl;
    byte color_key = pxm->color_key;
    int has_alpha = (color_key != GFX_PIXMAP_COLOR_KEY_NONE);
    int use_psram = (!pxm->index_data && pxm->psram_valid);

    /* Pre-build lookup: local color index → palette slot in ps->palette[].
       global_index is always -1 in SCI0 (never allocated via gfx_alloc_color),
       so we derive the mapping from colors_nr instead:
       - 256 colors (background pic): palette slot == color index (identity)
       - 16 colors (view cels): slot i*17 is the pure-EGA entry for color i,
         because gfx_sci0_pic_colors[i*17] = INTERCOL(ega[i], ega[i]) = ega[i] */
    uint8_t lut[256];
    if (pxm->colors_nr == GFX_SCI0_PIC_COLORS_NR) {
        for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i;
    } else {
        for (int i = 0; i < pxm->colors_nr; i++)
            lut[i] = (uint8_t)(i * 17);
    }

    uint8_t *row_dst = destbuf;
    uint8_t *row_pri = pri_buf;

    for (int y = 0; y < yl; y++) {
        const byte *row_src;
        if (use_psram) {
            uint32_t row_offset = (uint32_t)((src.y + y) * pxm->index_xl);
            psram_load(pxm->psram_addr + row_offset, s_psram_row, (size_t)pxm->index_xl);
            row_src = s_psram_row + src.x;
        } else {
            row_src = pxm->index_data + (src.y + y) * pxm->index_xl + src.x;
        }
        for (int x = 0; x < xl; x++) {
            byte idx = row_src[x];
            if (!has_alpha || idx != color_key) {
                row_dst[x] = lut[idx];
                if (row_pri && priority >= 0 && (int)row_pri[x] <= priority)
                    row_pri[x] = (uint8_t)priority;
            }
        }
        row_dst += dest_stride;
        if (row_pri) row_pri += pri_stride;
    }
}

/* Blit the full background from PSRAM into visual[0].
   Called from gfxop_new_pic after visual[0] is allocated.
   Does NOT flush to the display — the engine's first gfxop_update call does that. */
void pico_render_background(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (!ps || !ps->visual[0] || !ps->static_bg) return;
    gfx_pixmap_t *bg = ps->static_bg;
    rect_t full = gfx_rect(0, 0, bg->index_xl, bg->index_yl);
    rect_t dst  = gfx_rect(0, 0, bg->index_xl, bg->index_yl);
    pico_blit_indexed(bg, -1, full, dst, ps->visual[0], PICO_XSIZE, NULL, 0);
    /* Push the freshly decoded background to the display immediately. */
    flush_region(ps, 0, 0, PICO_XSIZE, PICO_YSIZE);
}

/* ------------------------------------------------------------------ */
/* Pixmap operations (no registry — engine owns pxm->data)            */
/* ------------------------------------------------------------------ */

static int pico_register_pixmap(struct _gfx_driver *drv, gfx_pixmap_t *pxm)
{
    (void)drv; (void)pxm;
    return GFX_OK;
}

static int pico_unregister_pixmap(struct _gfx_driver *drv, gfx_pixmap_t *pxm)
{
    (void)drv;
    if (pxm->internal.handle == PICO_HANDLE_GRABBED) {
        if (!pxm->internal.info && pxm->data) {
            /* SRAM grab: data was allocated by grab_pixmap */
            sci_free(pxm->data);
        }
        /* PSRAM grab (internal.info != NULL): bump-allocated, reclaimed on psram_reset */
        pxm->data = NULL;
        pxm->internal.info = NULL;
    }
    return GFX_OK;
}

static int pico_draw_pixmap(struct _gfx_driver *drv, gfx_pixmap_t *pxm,
                             int priority,
                             rect_t src, rect_t dest, gfx_buffer_t buffer)
{
    int bufnr = 0;  /* single visual buffer serves back and static */

    if (!pico_ensure_visual(drv)) return GFX_ERROR;

    if (dest.xl != src.xl || dest.yl != src.yl) {
        fprintf(stderr, "pico_driver: scaling not supported (%dx%d)->(%dx%d)\n",
                src.xl, src.yl, dest.xl, dest.yl);
        return GFX_ERROR;
    }

    /* Grabbed pixmap: restore saved region. */
    if (pxm->internal.handle == PICO_HANDLE_GRABBED) {
        if (pxm->internal.info) {
            /* PSRAM grab: restore row-by-row from PSRAM */
            uint32_t addr = (uint32_t)(uintptr_t)pxm->data;
            for (int row = 0; row < dest.yl; row++)
                psram_load(addr + (uint32_t)(row * pxm->xl),
                           S->visual[bufnr] + (dest.y + row) * PICO_XSIZE + dest.x,
                           (size_t)pxm->xl);
        } else if (pxm->data) {
            /* SRAM grab */
            for (int row = 0; row < dest.yl; row++)
                memcpy(S->visual[bufnr] + (dest.y + row) * PICO_XSIZE + dest.x,
                       pxm->data + row * pxm->xl, pxm->xl);
        }
        return GFX_OK;
    }

    /* Normal pixmap: blit to visual buffer.
       If pxm->data is NULL (gfx_xlate_pixmap skipped on Pico), use the
       direct indexed blit that translates index_data+colors on-the-fly. */
    uint8_t *destptr = S->visual[bufnr] + dest.y * PICO_XSIZE + dest.x;
    if (!pxm->data) {
        uint8_t *pridata = (s_shared_priority && s_shared_priority->index_data)
                           ? s_shared_priority->index_data : NULL;
        uint8_t *priptr = pridata
                          ? (pridata + dest.y * s_shared_priority->index_xl + dest.x)
                          : NULL;
        int pri_stride = pridata ? s_shared_priority->index_xl : 0;
        pico_blit_indexed(pxm, priority, src, dest, destptr, PICO_XSIZE,
                          priptr, pri_stride);
    } else if (s_shared_priority && s_shared_priority->index_data) {
        gfx_crossblit_pixmap(drv->mode, pxm, priority, src, dest,
                              destptr, PICO_XSIZE,
                              s_shared_priority->index_data,
                              s_shared_priority->index_xl, 1,
                              GFX_CROSSBLIT_FLAG_DATA_IS_HOMED);
    } else {
        gfx_crossblit_pixmap(drv->mode, pxm, GFX_NO_PRIORITY, src, dest,
                              destptr, PICO_XSIZE,
                              NULL, 0, 0,
                              GFX_CROSSBLIT_FLAG_DATA_IS_HOMED);
    }
    return GFX_OK;
}

static int pico_grab_pixmap(struct _gfx_driver *drv, rect_t src,
                             gfx_pixmap_t *pxm, gfx_map_mask_t map)
{
    if (!S->visual[0]) return GFX_OK; /* nothing drawn yet — grab returns empty */

    if (src.x < 0 || src.y < 0) {
        fprintf(stderr, "pico_driver: grab from invalid coords (%d,%d)\n",
                src.x, src.y);
        return GFX_ERROR;
    }

    switch (map) {
    case GFX_MASK_VISUAL: {
        pxm->xl = src.xl;
        pxm->yl = src.yl;
        pxm->internal.handle = PICO_HANDLE_GRABBED;
        pxm->internal.info = NULL;
        pxm->flags |= GFX_PIXMAP_FLAG_INSTALLED |
                      GFX_PIXMAP_FLAG_EXTERNAL_PALETTE |
                      GFX_PIXMAP_FLAG_PALETTE_SET;
        size_t sz = (size_t)src.xl * src.yl;
        if (sz > 4096) {
            /* Large grab: save row-by-row into PSRAM to avoid SRAM pressure.
               PSRAM address is bump-allocated and reclaimed on psram_reset(). */
            uint32_t addr = psram_alloc(sz);
            for (int row = 0; row < src.yl; row++)
                psram_store(addr + (uint32_t)(row * src.xl),
                            S->visual[0] + (src.y + row) * PICO_XSIZE + src.x,
                            src.xl);
            pxm->data = (uint8_t *)(uintptr_t)addr;
            pxm->internal.info = (void *)(uintptr_t)1;
        } else {
            /* Small grab: SRAM */
            if (!pxm->data) {
                pxm->data = (uint8_t *)sci_malloc(sz);
                if (!pxm->data) return GFX_FATAL;
            }
            for (int row = 0; row < src.yl; row++)
                memcpy(pxm->data + row * src.xl,
                       S->visual[0] + (src.y + row) * PICO_XSIZE + src.x,
                       src.xl);
        }
        return GFX_OK;
    }
    case GFX_MASK_PRIORITY:
        fprintf(stderr, "pico_driver: priority map grab not implemented\n");
        return GFX_ERROR;
    default:
        fprintf(stderr, "pico_driver: invalid map 0x%02x in grab\n", map);
        return GFX_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Buffer operations                                                   */
/* ------------------------------------------------------------------ */

static int pico_update(struct _gfx_driver *drv,
                        rect_t src, point_t dest, gfx_buffer_t buffer)
{
    switch (buffer) {
    case GFX_BUFFER_BACK:
        /* Restore background from PSRAM into visual[0] for this dirty region. */
        if (S->static_bg && S->visual[0]) {
            uint8_t *destptr = S->visual[0] + dest.y * PICO_XSIZE + dest.x;
            rect_t bgsrc = gfx_rect(src.x, src.y, src.xl, src.yl);
            rect_t bgdst = gfx_rect(0, 0, src.xl, src.yl);
            pico_blit_indexed(S->static_bg, -1, bgsrc, bgdst,
                              destptr, PICO_XSIZE, NULL, 0);
        }
        break;

    case GFX_BUFFER_FRONT:
        flush_region(S, dest.x, dest.y, src.xl, src.yl);
        break;

    default:
        fprintf(stderr, "pico_driver: invalid buffer %d\n", buffer);
        return GFX_ERROR;
    }

    return GFX_OK;
}

static int pico_set_static_buffer(struct _gfx_driver *drv,
                                   gfx_pixmap_t *pic, gfx_pixmap_t *priority)
{
    (void)priority;
    S->static_bg = pic;  /* save for BUFFER_BACK restoration */
    return GFX_OK;
}

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

static int pico_set_palette(struct _gfx_driver *drv,
                             int index, byte red, byte green, byte blue)
{
    /* global_index is always -1 in SCI0 (gfx_alloc_color never called).
       _gfxop_install_pixmap calls this for each color; silently succeeding
       lets it set GFX_PIXMAP_FLAG_PALETTE_SET without flooding UART with
       GFXWARN messages.  Palette is managed by pico_setup_sci0_palette(). */
    if (index < 0)
        return GFX_OK;
    if (index > 255)
        return GFX_ERROR;
    S->palette[index][0] = red;
    S->palette[index][1] = green;
    S->palette[index][2] = blue;
    return GFX_OK;
}

/* ------------------------------------------------------------------ */
/* Pointer (no hardware cursor — PicoCalc has no mouse)               */
/* ------------------------------------------------------------------ */

static int pico_set_pointer(struct _gfx_driver *drv, gfx_pixmap_t *pointer)
{
    (void)drv; (void)pointer;
    return GFX_OK;
}

/* ------------------------------------------------------------------ */
/* Event management                                                    */
/* ------------------------------------------------------------------ */

static sci_event_t pico_get_event(struct _gfx_driver *drv)
{
    static int s_evt_count = 0;
    if (++s_evt_count % 20 == 0) {
        printf("[evt] get_event calls: %d\n", s_evt_count); stdio_flush();
    }
    poll_keyboard(drv);
    return pop_event(S);
}

static int pico_usec_sleep(struct _gfx_driver *drv, long usecs)
{
    static int s_slp_count = 0;
    if (++s_slp_count % 100 == 0) {
        printf("[slp] sleep_count=%d\n", s_slp_count); stdio_flush();
    }
    poll_keyboard(drv);
    sleep_us((uint64_t)(usecs > 10000 ? 10000 : usecs));
    return GFX_OK;
}

/* ------------------------------------------------------------------ */
/* Driver descriptor                                                   */
/* ------------------------------------------------------------------ */

gfx_driver_t gfx_driver_pico = {
    "pico",
    "0.1",
    SCI_GFX_DRIVER_MAGIC,
    SCI_GFX_DRIVER_VERSION,
    NULL, /* mode */
    0, 0, /* pointer_x, pointer_y */
    GFX_CAPABILITY_FINE_LINES, /* no mouse, no pixmap registry */
    0,   /* debug_flags */
    pico_set_parameter,
    pico_init_specific,
    pico_init,
    pico_exit,
    pico_draw_line,
    pico_draw_filled_rect,
    pico_register_pixmap,
    pico_unregister_pixmap,
    pico_draw_pixmap,
    pico_grab_pixmap,
    pico_update,
    pico_set_static_buffer,
    pico_set_pointer,
    pico_set_palette,
    pico_get_event,
    pico_usec_sleep,
    NULL /* state */
};

#endif /* HAVE_PICO */
