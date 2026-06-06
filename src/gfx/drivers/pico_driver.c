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

/* Fixed PSRAM scratch for the parse-time visual borrow. Placed at 7MB, far
   above the room bump arena (grows from 0, well under 1MB/room), below the 8MB
   top. Reused every parse — only one borrow is ever live at a time (kParse is
   synchronous and the game is paused while a typed command is parsed). */
#define PICO_PARSE_SCRATCH_ADDR  0x700000u

/* Borrow the 64KB visual back-buffer to PSRAM for the duration of a paused
   computation (GNF text parsing, which can transiently need ~90KB). The
   composited frame is saved so it can be restored intact — SCI redraws
   incrementally, so we must not lose it. Returns 1 if a buffer was offloaded,
   0 if there was nothing to borrow (already freed). */
int pico_borrow_visual(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (!ps || !ps->visual[0])
        return 0;
    psram_store(PICO_PARSE_SCRATCH_ADDR, ps->visual[0], PICO_XSIZE * PICO_YSIZE);
    sci_free(ps->visual[0]);
    ps->visual[0] = NULL;
    return 1;
}

/* Re-acquire the visual back-buffer freed by pico_borrow_visual and restore
   its saved contents. sci_malloc halts legibly on OOM (the residual risk: a
   command whose parse fragments the heap so badly the 64KB can't be reclaimed),
   rather than corrupting silently. */
void pico_return_visual(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (!ps || ps->visual[0])
        return;  /* nothing was borrowed, or already restored */
    ps->visual[0] = (uint8_t *)sci_malloc(PICO_XSIZE * PICO_YSIZE);
    psram_load(PICO_PARSE_SCRATCH_ADDR, ps->visual[0], PICO_XSIZE * PICO_YSIZE);
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
/* read_i2c_kbd() blocks ~16ms (i2c write, sleep_ms(16), i2c read), so a single
   poll already paces one frame at ~60Hz. Throttle to at most one real read per
   frame-time so multiple callers in one cycle (e.g. several front-buffer dirty
   rects) don't stack 16ms blocks. This is the tiny_agi model: one blocking
   keyboard read per visible cycle doubles as the frame pacer. */
#define KBD_POLL_INTERVAL_US 16000

static void poll_keyboard(struct _gfx_driver *drv)
{
    static uint64_t last_kbd_us = 0;
    uint64_t now = time_us_64();
    if (now - last_kbd_us < KBD_POLL_INTERVAL_US)
        return;
    last_kbd_us = now;

    int key = kbd_read();
    if (key <= 0) return;

    int sci_key = pico_map_key(key);
    if (!sci_key)
        return;

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

static uint8_t s_psram_row[PICO_XSIZE]; /* scratch row for PSRAM reads (source index) */
static uint8_t s_pri_row[PICO_XSIZE];   /* scratch row for PSRAM reads (priority) */
static uint8_t s_pri_pack[(PICO_XSIZE >> 1) + 1]; /* packed-nibble scratch for the priority readback */

/* Map an RGB triple to the closest entry in the Pico palette. */
static uint8_t
nearest_pal(struct _pico_state *ps, int r, int g, int b)
{
    int best = 0, best_d = 0x7fffffff;
    for (int i = 0; i < 256; i++) {
        int dr = r - ps->palette[i][0];
        int dg = g - ps->palette[i][1];
        int db = b - ps->palette[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; if (!d) break; }
    }
    return (uint8_t)best;
}

static void
pico_blit_indexed(struct _pico_state *ps, gfx_pixmap_t *pxm, int priority,
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

    /* GUARD: s_psram_row is PICO_XSIZE bytes.  A cel whose index_xl exceeds
       that (or a bad src origin) overflows the scratch row and feeds psram_load
       a runaway length -> blocking DMA never returns.  Skip on out-of-range
       geometry rather than hang. */
    if (use_psram && (pxm->index_xl <= 0 || pxm->index_xl > PICO_XSIZE
                      || pxm->index_yl <= 0 || pxm->index_yl > PICO_YSIZE
                      || src.x < 0 || src.y < 0
                      || src.x + xl > pxm->index_xl
                      || src.y + yl > pxm->index_yl))
        return;

    /* Pre-build lookup: local color index → palette slot in ps->palette[].
       - 256 colors (background pic): palette slot == color index (identity)
       - otherwise (view cels, text): match each local color's RGB to the
         nearest palette entry. For EGA view cels this resolves to slot i*17
         (pure EGA i); for text it resolves to the real requested color
         instead of mis-assuming local index i == EGA color i. */
    uint8_t lut[256];
    if (pxm->colors_nr == GFX_SCI0_PIC_COLORS_NR) {
        for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i;
    } else {
        for (int i = 0; i < pxm->colors_nr; i++)
            lut[i] = nearest_pal(ps, pxm->colors[i].r,
                                 pxm->colors[i].g, pxm->colors[i].b);
    }

    uint8_t *row_dst = destbuf;
    uint8_t *row_pri = pri_buf;

    /* Priority source for occlusion gating.  The engine's priority_map holds the
       background's baked-in priorities, but on Pico its index_data is offloaded to
       PSRAM after decode (operations.c:2334), so the caller passes pri_buf=NULL.
       When that happens, read the priority row back from PSRAM per row (read-only —
       we never write sprite priorities back, so the PSRAM map stays the clean
       background base and no cross-frame priority trail accumulates).  This gives
       correct background occlusion; inter-sprite z-order is not gated. */
    int psram_pri = (!row_pri && priority >= 0 && s_shared_priority
                     && !s_shared_priority->index_data && s_shared_priority->psram_valid);
    int pri_xl = psram_pri ? s_shared_priority->index_xl : 0;
    int pri_yl = psram_pri ? s_shared_priority->index_yl : 0;

    /* [pblit] probe: per-sprite occlusion summary (throttled, strip later). */
    int pb_drawn = 0, pb_supp = 0, pb_min = 99, pb_max = -1;

    for (int y = 0; y < yl; y++) {
        const byte *row_src;
        if (use_psram) {
            uint32_t row_offset = (uint32_t)((src.y + y) * pxm->index_xl);
            psram_load(pxm->psram_addr + row_offset, s_psram_row, (size_t)pxm->index_xl);
            row_src = s_psram_row + src.x;
        } else {
            row_src = pxm->index_data + (src.y + y) * pxm->index_xl + src.x;
        }

        const uint8_t *pri_row = row_pri;  /* SRAM priority row, or NULL */
        if (psram_pri) {
            int py = dest.y + y;
            if (py >= 0 && py < pri_yl && dest.x >= 0 && dest.x + xl <= pri_xl) {
                if (s_shared_priority->nibble_packed) {
                    /* Priority map is 2 px/byte in PSRAM (merged-decode packing).
                       Read the packed byte span covering this row and unpack the
                       nibbles into s_pri_row (mirrors _gfxop_scan_one_bitmask). */
                    int pidx  = py * pri_xl + dest.x;
                    int byte0 = pidx >> 1;
                    int byteN = (pidx + xl - 1) >> 1;
                    size_t nbytes = (size_t)(byteN - byte0 + 1);
                    psram_load(s_shared_priority->psram_addr + (uint32_t)byte0,
                               s_pri_pack, nbytes);
                    for (int px = 0; px < xl; px++) {
                        int p = pidx + px;
                        uint8_t b = s_pri_pack[(p >> 1) - byte0];
                        s_pri_row[px] = (p & 1) ? (b >> 4) : (b & 0x0f);
                    }
                } else {
                    psram_load(s_shared_priority->psram_addr
                               + (uint32_t)(py * pri_xl + dest.x),
                               s_pri_row, (size_t)xl);
                }
                pri_row = s_pri_row;   /* gate against background base priority */
            }
            /* else: row off the priority map -> pri_row stays NULL -> write through */
        }

        for (int x = 0; x < xl; x++) {
            byte idx = row_src[x];
            if (!has_alpha || idx != color_key) {
                if (pri_row && priority >= 0) {
                    /* Highest-priority-wins: background scenery whose baked-in
                       priority exceeds this cel's occludes it (matches the SDL
                       crossblit gating in gfx_crossblit.c). */
                    int bp = pri_row[x];
                    if (psram_pri) {
                        if (bp < pb_min) pb_min = bp;
                        if (bp > pb_max) pb_max = bp;
                    }
                    if ((int)pri_row[x] <= priority) {
                        row_dst[x] = lut[idx];
                        if (row_pri)  /* only the SRAM map is written back */
                            row_pri[x] = (uint8_t)priority;
                        if (psram_pri) pb_drawn++;
                    } else if (psram_pri) {
                        pb_supp++;
                    }
                } else {
                    /* No priority map (background fill, text): always write. */
                    row_dst[x] = lut[idx];
                }
            }
        }
        row_dst += dest_stride;
        if (row_pri) row_pri += pri_stride;
    }

    /* [pblit] probe: report sprites whose pixels were occlusion-suppressed, so the
       cel priority can be compared against the background priority under it.
       Throttled to 1-in-8 to keep the serial log readable while walking. */
    if (psram_pri && pb_supp > 0) {
        static unsigned pb_call = 0;
        if ((pb_call++ & 7) == 0)
            sciprintf("[pblit] cel pri=%d dest=(%d,%d %dx%d) bgpri=%d..%d "
                      "drawn=%d supp=%d\n",
                      priority, dest.x, dest.y, xl, yl,
                      pb_min, pb_max, pb_drawn, pb_supp);
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
    pico_blit_indexed(ps, bg, -1, full, dst, ps->visual[0], PICO_XSIZE, NULL, 0);
    /* Push the freshly decoded background to the display immediately. */
    flush_region(ps, 0, 0, PICO_XSIZE, PICO_YSIZE);
}

/* ------------------------------------------------------------------ */
/* Pixmap operations (no registry — engine owns pxm->data)            */
/* ------------------------------------------------------------------ */

/* Save-under grab/free accounting (diagnostic for the ~35KB/revisit leak hunt;
   printed on the [mem] BREAKDOWN line). SRAM grabs are sci_malloc'd and freed
   individually; PSRAM grabs are bump-allocated and only reclaimed at
   psram_reset() on room change. If sram_live fails to return to baseline across
   same-room revisits, save-unders are the leak. */
int    pico_grab_sram_live  = 0;   /* live SRAM grabs (grab++ / free--)        */
size_t pico_grab_sram_bytes = 0;   /* live SRAM grab bytes                     */
int    pico_grab_sram_total = 0;   /* cumulative SRAM grabs                    */
int    pico_free_sram_total = 0;   /* cumulative SRAM frees                    */
int    pico_grab_psram_total = 0;  /* cumulative PSRAM grabs (since reset)     */

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
            pico_grab_sram_live--;
            pico_grab_sram_bytes -= (size_t)pxm->xl * pxm->yl;
            pico_free_sram_total++;
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

    /* Grabbed pixmap: restore saved region.
       Honor the src sub-rect: the grab is pxm->xl wide, but the caller may
       request only a (src.x,src.y,dest.xl,dest.yl) slice of it (e.g. the pic
       open-animation draws narrow vertical/horizontal strips). Copying the
       full pxm->xl per row at dest.x would over-read PSRAM ~64x and clobber
       neighbouring columns. */
    if (pxm->internal.handle == PICO_HANDLE_GRABBED) {
        int copy_w = dest.xl;  /* == src.xl (no scaling on Pico) */
        /* GUARD: a bad grab geometry over-reads PSRAM / overruns visual[0] ->
           hang or corruption.  Skip instead. */
        if (copy_w <= 0 || copy_w > PICO_XSIZE
            || dest.x < 0 || dest.x + copy_w > PICO_XSIZE
            || dest.y < 0 || dest.y + dest.yl > PICO_YSIZE)
            return GFX_OK;
        if (pxm->internal.info) {
            /* PSRAM grab: restore row-by-row from PSRAM */
            uint32_t addr = (uint32_t)(uintptr_t)pxm->data;
            for (int row = 0; row < dest.yl; row++)
                psram_load(addr + (uint32_t)((src.y + row) * pxm->xl + src.x),
                           S->visual[bufnr] + (dest.y + row) * PICO_XSIZE + dest.x,
                           (size_t)copy_w);
        } else if (pxm->data) {
            /* SRAM grab */
            for (int row = 0; row < dest.yl; row++)
                memcpy(S->visual[bufnr] + (dest.y + row) * PICO_XSIZE + dest.x,
                       pxm->data + (src.y + row) * pxm->xl + src.x, copy_w);
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
        pico_blit_indexed(S, pxm, priority, src, dest, destptr, PICO_XSIZE,
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
            pico_grab_psram_total++;
        } else {
            /* Small grab: SRAM */
            if (!pxm->data) {
                pxm->data = (uint8_t *)sci_malloc(sz);
                if (!pxm->data) return GFX_FATAL;
                pico_grab_sram_live++;
                pico_grab_sram_bytes += sz;
                pico_grab_sram_total++;
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
            pico_blit_indexed(S, S->static_bg, -1, bgsrc, bgdst,
                              destptr, PICO_XSIZE, NULL, 0);
        }
        break;

    case GFX_BUFFER_FRONT:
        flush_region(S, dest.x, dest.y, src.xl, src.yl);
        /* Per-frame keyboard poll + pace. Covers animation loops that never
           call kGetEvent/kWait (e.g. the SQ3 intro), which otherwise run
           unpaced. Throttled inside poll_keyboard to one read per frame. */
        poll_keyboard(drv);
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
    poll_keyboard(drv);
    return pop_event(S);
}

static int pico_usec_sleep(struct _gfx_driver *drv, long usecs)
{
    (void)drv;
    /* Pure sleep: keyboard polling/pacing now lives in the per-frame front
       flush (pico_update) and pico_get_event, so gfxop_usleep() times
       accurately instead of being floored at ~16ms by an embedded i2c read. */
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
    GFX_CAPABILITY_FINE_LINES | GFX_CAPABILITY_PIXMAP_REGISTRY,
    /* no mouse. Registry cap is required so gfx_free_pixmap() calls
       pico_unregister_pixmap(), which clears pxm->data for grabbed pixmaps.
       Grabbed PSRAM pixmaps store a PSRAM bump address (not a heap pointer)
       in pxm->data; without unregister, the generic free() would free() that
       bogus pointer and corrupt the heap. */
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
