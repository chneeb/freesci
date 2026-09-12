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
#ifdef PICO_PWM_AUDIO
#include "../../platform/pico/audio/pwm_synth.h"
#endif

/* define_region_spi is in lcdspi.c but not declared in lcdspi.h */
extern void define_region_spi(int xstart, int ystart, int xend, int yend, int rw);
/* hw_send_spi / spi_finish / lcd_spi_raise_cs declared in lcdspi.h */

#define TFT_Y_OFFSET   60    /* centre 320x200 in the 320x320 panel */
#define PICO_XSIZE     320
#define PICO_YSIZE     200

#define PICO_HANDLE_NORMAL  0
#define PICO_HANDLE_GRABBED 1

#define EVT_BUF_SIZE 8

/* The mapped target's static visual buffer is separately switchable so it can be
   A/B'd against the pre-static-buffer behaviour (-DPICO_STATIC_VISUAL=OFF), which
   falls back to restoring straight from the PSRAM background exactly as the PIO
   target does. Useful for deciding whether a render regression belongs to this
   feature or predates it. */
#if defined(PICO_PSRAM_MAPPED) && defined(PICO_STATIC_VISUAL)
#  define PICO_USE_STATIC_VISUAL 1
#endif

/* Buffer set. The PicoCalc PIO target has ONE visual buffer serving back+front
   (SRAM is the binding constraint there), and compensates with the static bake
   into PSRAM -- see pico_bake_static_region.

   The mapped-PSRAM target has ~300KB of SRAM headroom once engine allocations
   move to PSRAM, so it can afford a real STATIC buffer and follow the desktop
   model instead (sdl_driver.c: visual[2]=STATIC, visual[1]=BACK). There,
   GFX_BUFFER_STATIC draws land in the static buffer and every BACK restore
   copies FROM it, so kAddToPic picviews survive restores by construction rather
   than by baking them into the background. */
#ifdef PICO_USE_STATIC_VISUAL
#  define PICO_NVISUAL     2
#  define PICO_VIS_STATIC  1        /* analogue of SDL's visual[2] */
#else
#  define PICO_NVISUAL     1
#endif

/* ---- 4bpp visual buffer (PICO_PACK_VISUAL) --------------------------------
   With D16 dithering every pixel is one index 0..15 (proved across 343 pics by
   tests/d16check.c), so visual[0] can hold two pixels per byte: 64000 -> 32000,
   the whole point of the exercise on the SRAM-bound PIO target.

   Everything below goes through PICO_VIS_BYTES / VIS_GET / VIS_SET so the
   unpacked build is byte-identical -- the accessors compile to plain indexing.
   That matters because the 16-bit LCD attempt failed by converting ONE writer
   and leaving the rest: with accessors, a missed site is a compile-visible
   direct `visual[0][i]` rather than a silent shear. Verify with tests/drvdiff,
   which runs this file on the desktop and diffs the actual panel bytes. */
#ifdef PICO_PACK_VISUAL
#  define PICO_VIS_BYTES   ((PICO_XSIZE * PICO_YSIZE + 1) >> 1)
#  define PICO_VIS_PACKED  1
#  define VIS_GET(buf, i)  (((i) & 1) ? ((buf)[(i) >> 1] >> 4) \
				      : ((buf)[(i) >> 1] & 0x0f))
#  define VIS_SET(buf, i, v)                                            \
	do {                                                            \
		uint8_t *_p = (buf) + ((i) >> 1);                        \
		uint8_t _n = (uint8_t)((v) & 0x0f);                      \
		*_p = ((i) & 1) ? (uint8_t)((*_p & 0x0f) | (_n << 4))    \
				: (uint8_t)((*_p & 0xf0) | _n);          \
	} while (0)
#else
#  define PICO_VIS_BYTES   (PICO_XSIZE * PICO_YSIZE)
#  define PICO_VIS_PACKED  0
#  define VIS_GET(buf, i)  ((buf)[(i)])
#  define VIS_SET(buf, i, v) do { (buf)[(i)] = (uint8_t)(v); } while (0)
#endif

#ifdef PICO_PACK_VISUAL
/* Grabbed pixmaps stay 8bpp: their data is consumed elsewhere as plain bytes
   (and a grab can start at an odd x, where a packed copy would need partial-byte
   edges for no benefit). So the pack boundary is crossed HERE, on the way in and
   out, and grab/restore stay symmetric. */
static uint8_t s_vis_row[PICO_XSIZE];
#endif

struct _pico_state {
    uint8_t        *visual[PICO_NVISUAL]; /* [0]=back/front (drawing+display) */
    /* priority buffer removed — uses engine's priority_map via s_shared_priority */
    uint8_t         palette[256][3]; /* R,G,B for each colour index */
#ifdef PICO_LCD_16BIT
    /* Same palette pre-packed as RGB565, rebuilt whenever palette[] changes.
       Two wins per pixel in flush_region: one 16-bit load instead of three
       byte loads, and two bytes on the wire instead of three. */
    uint16_t        pal565[256];
#endif
    gfx_pixmap_t   *static_bg;       /* current room's visual_map (PSRAM-backed) */
#ifdef PICO_USE_STATIC_VISUAL
    int             static_dirty;    /* static buffer may predate static_bg */
#else
    /* PIO: the SECOND PSRAM surface. static_bg stays PRISTINE (the room's clean
       plate); `composed` holds background + baked static views and is what BACK
       restores read. Separating them is what makes a bake reversible -- baking
       into static_bg itself destroyed the clean plate, so a view that later
       animated away could never be erased (the documented BAKE ghosting), and it
       corrupted the add_to_pic overlay base. Both surfaces are PSRAM, so this
       costs 64KB of an 8MB chip and ZERO SRAM; the mapped target solves the same
       problem with an SRAM buffer (PICO_USE_STATIC_VISUAL) it can afford and PIO
       cannot. Allocated lazily on the first bake, so a room with no static views
       pays nothing and behaves exactly as before. */
    uint32_t        composed_addr;   /* PSRAM offset, valid iff composed_valid */
    int             composed_valid;
    gfx_pixmap_t    composed_pxm;    /* shallow mirror of *static_bg, addr swapped */
#endif

    /* keyboard event ring buffer */
    sci_event_t     evbuf[EVT_BUF_SIZE];
    int             ev_head, ev_tail;
};

/* Shared engine priority maps, set by pico_connect_engine_priority() after GFX
   init. s_static_priority is the clean plate and is NULL unless the mapped
   target's desktop-style per-frame maps are in use. */
static gfx_pixmap_t *s_shared_priority = NULL;
#ifdef PICO_WORKING_PRIORITY
static gfx_pixmap_t *s_static_priority = NULL;
#endif

void pico_connect_engine_priority(gfx_pixmap_t *priority_map,
                                  gfx_pixmap_t *static_priority_map)
{
    s_shared_priority = priority_map;
#ifdef PICO_WORKING_PRIORITY
    s_static_priority = static_priority_map;
#else
    /* PIO: static_priority_map is aliased to priority_map, so there is nothing
       to select between -- keep the target byte-identical. */
    (void)static_priority_map;
#endif
}

/* Set by widgets.c around a NO_UPDATE dynview's static-routed draw in priority-only
   mode (PICO_STATIC_VIEW_PRIORITY without PICO_STATIC_VIEW_BAKE): bake the view's
   priority but do NOT persist its color into static_bg (so it can't ghost).  Always
   0 for picviews and in full-bake mode, so their color still bakes. */
int pico_priority_only_static = 0;

void pico_free_visual(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (ps && ps->visual[0]) {
        sci_free(ps->visual[0]);
        ps->visual[0] = NULL;
    }
#ifdef PICO_USE_STATIC_VISUAL
    if (ps && ps->visual[PICO_VIS_STATIC]) {
        sci_free(ps->visual[PICO_VIS_STATIC]);
        ps->visual[PICO_VIS_STATIC] = NULL;
    }
#endif
}

void pico_alloc_visual(gfx_driver_t *drv)
{
    struct _pico_state *ps = (struct _pico_state *)drv->state;
    if (ps && !ps->visual[0]) {
        ps->visual[0] = (uint8_t *)sci_malloc_sram(PICO_VIS_BYTES);
        if (ps->visual[0])
            memset(ps->visual[0], 0, PICO_VIS_BYTES);
    }
#ifdef PICO_USE_STATIC_VISUAL
    /* The static buffer is best-effort: every use site falls back to the back
       buffer if it is missing, which degrades to the PIO single-buffer
       behaviour rather than failing. */
    if (ps && !ps->visual[PICO_VIS_STATIC]) {
        ps->visual[PICO_VIS_STATIC] =
            (uint8_t *)sci_malloc_sram(PICO_VIS_BYTES);
        if (ps->visual[PICO_VIS_STATIC])
            memset(ps->visual[PICO_VIS_STATIC], 0, PICO_XSIZE * PICO_YSIZE);
    }
#endif
}

/* Ensure visual[0] is allocated; returns 1 on success, 0 on OOM. */
static int pico_ensure_visual(gfx_driver_t *drv)
{
    pico_alloc_visual(drv);
    return ((struct _pico_state *)drv->state)->visual[0] != NULL;
}

/* Ensure visual[0] is allocated and return it, for the pic-decode visual[0]-reuse
   path (gfxop_new_pic borrows this 64KB display buffer as the decode-visual so the
   decode needs no fresh 64KB alloc). Returns NULL only if the alloc itself OOMs. */
byte *pico_get_visual(gfx_driver_t *drv)
{
    pico_alloc_visual(drv);
    return ((struct _pico_state *)drv->state)->visual[0];
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
    psram_store(PICO_PARSE_SCRATCH_ADDR, ps->visual[0], PICO_VIS_BYTES);
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
    ps->visual[0] = (uint8_t *)sci_malloc_sram(PICO_VIS_BYTES);
    psram_load(PICO_PARSE_SCRATCH_ADDR, ps->visual[0], PICO_VIS_BYTES);
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

#ifdef PICO_LCD_16BIT
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/* Rebuild the packed palette. Cheap (256 entries) and only on palette changes,
   never per frame. */
static void pico_rebuild_pal565(struct _pico_state *ps)
{
    for (int i = 0; i < 256; i++)
        ps->pal565[i] = rgb565(ps->palette[i][0], ps->palette[i][1],
                               ps->palette[i][2]);
}
#endif

static void flush_region(struct _pico_state *ps,
                          int x, int y, int w, int h)
{
    if (!ps->visual[0]) return; /* not yet allocated — nothing to display */
    define_region_spi(x, y + TFT_Y_OFFSET,
                      x + w - 1, y + TFT_Y_OFFSET + h - 1, 1);

    for (int row = 0; row < h; row++) {
#ifdef PICO_PACK_VISUAL
        /* HOTTEST loop in the driver: one nibble unpack per pixel. Kept as a
           separate block rather than routed through VIS_GET on a per-pixel
           index so the row base is computed once. */
        const int base = (y + row) * PICO_XSIZE + x;
        const uint8_t *packed = ps->visual[0];
#       define SRC_AT(col) ((uint8_t)VIS_GET(packed, base + (col)))
#else
        const uint8_t *src = ps->visual[0] + (y + row) * PICO_XSIZE + x;
#       define SRC_AT(col) (src[(col)])
#endif
#ifdef PICO_LCD_16BIT
        /* RGB565, big-endian on the wire (panel is set to 0x3A=0x65). */
        for (int col = 0; col < w; col++) {
            uint16_t p = ps->pal565[SRC_AT(col)];
            line_buf[col * 2    ] = (uint8_t)(p >> 8);
            line_buf[col * 2 + 1] = (uint8_t)(p & 0xff);
        }
        hw_send_spi(line_buf, w * 2);
#else
        for (int col = 0; col < w; col++) {
            uint8_t idx = SRC_AT(col);
            line_buf[col * 3    ] = ps->palette[idx][0];
            line_buf[col * 3 + 1] = ps->palette[idx][1];
            line_buf[col * 3 + 2] = ps->palette[idx][2];
        }
        hw_send_spi(line_buf, w * 3);
#endif
#       undef SRC_AT
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
        drv->state = sci_malloc_sram(sizeof(struct _pico_state));
        if (!drv->state) return GFX_FATAL;
        memset(drv->state, 0, sizeof(struct _pico_state));
    }

    /* Allocate one 320×200 palette-indexed visual buffer (back/front combined).
       Priority buffer is not allocated here — after GFX init, pico_connect_engine_priority()
       wires the engine's state->priority_map directly, saving 64KB of heap. */
    /* PICO_VIS_BYTES, not xsize*ysize: this and pico_alloc_visual MUST agree, or
       visual[0] starts full-size at boot and becomes half-size after the first
       realloc (a restore, or the parse-time borrow) -- which is why the device
       ran fine for a while and only faulted later, on leaving the PQ2 car. Two
       allocation sites with different sizes is a trap, not an optimisation. */
    S->visual[0] = (uint8_t *)sci_malloc_sram(PICO_VIS_BYTES);
    if (!S->visual[0]) {
        fprintf(stderr, "pico_driver: OOM allocating visual[0]\n");
        return GFX_FATAL;
    }
    memset(S->visual[0], 0, PICO_VIS_BYTES);
#ifdef PICO_USE_STATIC_VISUAL
    /* Static buffer (desktop visual[2] analogue). Best-effort: every use site
       falls back to the back buffer, so a failure degrades to the PIO
       single-buffer behaviour instead of failing init. */
    S->visual[PICO_VIS_STATIC] = (uint8_t *)sci_malloc_sram(xsize * ysize);
    if (S->visual[PICO_VIS_STATIC])
        memset(S->visual[PICO_VIS_STATIC], 0, xsize * ysize);
    else
        fprintf(stderr, "pico_driver: OOM allocating static visual buffer;"
                        " falling back to single-buffer behaviour\n");
#endif

    /* Default EGA palette */
    static const uint8_t ega16[16][3] = {
        {  0,  0,  0}, {  0,  0,168}, {  0,168,  0}, {  0,168,168},
        {168,  0,  0}, {168,  0,168}, {168, 84,  0}, {168,168,168},
        { 84, 84, 84}, { 84, 84,255}, { 84,255, 84}, { 84,255,255},
        {255, 84, 84}, {255, 84,255}, {255,255, 84}, {255,255,255},
    };
    for (i = 0; i < 16; i++)
        memcpy(S->palette[i], ega16[i], 3);
#ifdef PICO_LCD_16BIT
    pico_rebuild_pal565(S);
#endif

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
#ifdef PICO_USE_STATIC_VISUAL
    if (S->visual[PICO_VIS_STATIC]) {
        sci_free(S->visual[PICO_VIS_STATIC]);
        S->visual[PICO_VIS_STATIC] = NULL;
    }
#endif
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
#ifdef PICO_LCD_16BIT
    /* keep the packed palette in sync */
    pico_rebuild_pal565(ps);
#endif
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
    /* Index rather than pointer arithmetic: a packed buffer has no byte per
       pixel to point at. Same algorithm, same pixel sequence -- only the
       addressing changes, deliberately, since re-implementing a traversal is
       how the ctl_draw_line flood-fill gap happened. */
    int pixel = y1 * pitch + x1;
    int pixx = sx, pixy = sy * pitch;

    if (dx < dy) {
        int tmp;
        tmp = dx; dx = dy; dy = tmp;
        tmp = pixx; pixx = pixy; pixy = tmp;
    }

    for (; x < dx; x++, pixel += pixx) {
        VIS_SET(buf, pixel, color);
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
#ifdef PICO_PACK_VISUAL
            {
                /* Constant index, NOT a dither pair: by the time the driver
                   sees a colour the engine has already resolved it to one
                   palette slot, so both nibbles are the same value. */
                int i = row * PICO_XSIZE + rect.x, end = i + rect.xl;
                uint8_t pair = (uint8_t)((c & 0x0f) | ((c & 0x0f) << 4));

                if (i & 1) { VIS_SET(S->visual[0], i, c); i++; }
                if (end - i >= 2) {
                    memset(S->visual[0] + (i >> 1), pair, (end - i) >> 1);
                    i += ((end - i) >> 1) << 1;
                }
                if (i < end) VIS_SET(S->visual[0], i, c);
            }
#else
            memset(S->visual[0] + row * PICO_XSIZE + rect.x, c, rect.xl);
#endif
    }
    if ((color1.mask & GFX_MASK_PRIORITY) && s_shared_priority)
        gfx_draw_box_pixmap_i(s_shared_priority, rect, color1.priority);

    return GFX_OK;
}

/* ------------------------------------------------------------------ */
/* Indexed blit: translate index_data+colors directly to visual buf   */
/* Used when pxm->data is NULL (skipped in gfx_xlate_pixmap for Pico) */
/* ------------------------------------------------------------------ */

static uint8_t s_psram_row[PICO_XSIZE];
static uint8_t s_psram_packed[(PICO_XSIZE + 1) >> 1]; /* packed row staging */ /* scratch row for PSRAM reads (source index) */
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
                  uint8_t *destbuf,    /* BASE, not homed: a packed buffer has no
                                          byte to point at for an odd x */
                  int dest_index,      /* first pixel = dest.y*stride + dest.x */
                  int dest_packed,     /* destbuf is 2 px/byte */
                  int dest_stride,
                  uint8_t *pri_buf,    /* priority index_data, or NULL */
                  int pri_stride,
                  int bake_static_pri) /* 1: also write this cel's priority into the
                                          PSRAM priority map (GFX_BUFFER_STATIC only) */
{
    int xl = src.xl, yl = src.yl;
    /* color_key is an int (-1 == GFX_PIXMAP_COLOR_KEY_NONE); test has_alpha on the
       int BEFORE narrowing to a byte.  Truncating -1 to a byte yields 255, which
       != NONE, so a transparency-free pixmap (background visual_map, color_key=-1)
       was wrongly given has_alpha=1 with key 255 — every palette-index-255 (solid
       white) background pixel got dropped as transparent (device holes the desktop
       doesn't have).  Only narrow to a byte when there really is a key. */
    int has_alpha = (pxm->color_key != GFX_PIXMAP_COLOR_KEY_NONE);
    byte color_key = has_alpha ? (byte)pxm->color_key : 0;
    int use_psram = (!pxm->index_data && pxm->psram_valid);

    /* GUARD: s_psram_row is PICO_XSIZE bytes.  A cel whose index_xl exceeds
       that (or a bad src origin) overflows the scratch row and feeds psram_load
       a runaway length -> blocking DMA never returns.  Skip on out-of-range
       geometry rather than hang. */
    if (use_psram && (pxm->index_xl <= 0 || pxm->index_xl > PICO_XSIZE
                      || pxm->index_yl <= 0 || pxm->index_yl > PICO_YSIZE
                      || src.x < 0 || src.y < 0
                      || src.x + xl > pxm->index_xl
                      || src.y + yl > pxm->index_yl)) {
#ifdef FSCI_PROBE_GFX
        /* Diagnostic: a cel dropped here is invisible (desktop would clip it).
           Capture the geometry so a missing PQ2 cel can be matched. */
        sciprintf("[pblit] SKIP-GUARD cel idx=%dx%d src=(%d,%d %dx%d) pri=%d\n",
                  pxm->index_xl, pxm->index_yl, src.x, src.y, xl, yl, priority);
#endif
        return;
    }

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

    int row_dst = dest_index;
    uint8_t *row_pri = pri_buf;

/* One destination store for the whole blit. dest_packed is a RUNTIME flag, not
   a compile-time one, because this function serves several destinations at once:
   visual[0] (packed under PICO_PACK_VISUAL), the mapped static buffer, and the
   single-row compose scratch -- which are not all the same format. */
#define BLIT_PUT(idx_, v_)                                              \
	do {                                                            \
		if (dest_packed) {                                      \
			uint8_t *_p = destbuf + ((idx_) >> 1);          \
			uint8_t _n = (uint8_t)((v_) & 0x0f);            \
			*_p = ((idx_) & 1)                              \
			      ? (uint8_t)((*_p & 0x0f) | (_n << 4))     \
			      : (uint8_t)((*_p & 0xf0) | _n);           \
		} else destbuf[(idx_)] = (uint8_t)(v_);                 \
	} while (0)

    /* Priority source for occlusion gating.  The engine's priority_map holds the
       background's baked-in priorities, but on Pico its index_data is offloaded to
       PSRAM after decode (operations.c:2334), so the caller passes pri_buf=NULL.
       When that happens, read the priority row back from PSRAM per row.

       The PSRAM map plays the role of SDL's static_priority_map: background plus
       baked-in picviews, read-only to moving actors.  Actors (GFX_BUFFER_BACK) only
       READ it, so no cross-frame priority trail accumulates.  Static picviews
       (GFX_BUFFER_STATIC) additionally WRITE their priority back — see
       bake_static_pri below.  Inter-actor z-order is still not gated (that needs an
       SRAM working map we can't afford). */
    int psram_pri = (!row_pri && priority >= 0 && s_shared_priority
                     && !s_shared_priority->index_data && s_shared_priority->psram_valid);
    int pri_xl = psram_pri ? s_shared_priority->index_xl : 0;
    int pri_yl = psram_pri ? s_shared_priority->index_yl : 0;

    /* Bake this cel's priority into the PSRAM map, the analogue of the desktop
       _gfxop_draw_priority(static_priority_map, ...) (operations.c:380) that is
       skipped on Pico because priority_map->index_data is NULL.  Without it a
       kAddToPic/stopUpd picview (SQ3's door + motivator, PQ2's parking-lot cars)
       is drawn and colour-baked but leaves no priority, so actors paint over it.
       Only for GFX_BUFFER_STATIC, and only for the SCI0 0..15 range the packed
       nibble map can represent. */
#if defined(PICO_STATIC_VIEW_BAKE) || defined(PICO_STATIC_VIEW_PRIORITY)
    int bake_pri = (bake_static_pri && psram_pri && priority <= 15);
#else
    int bake_pri = 0;      /* feature off -> priority map stays the clean background */
    (void)bake_static_pri;
#endif

    /* [pblit] probe: per-sprite occlusion summary (throttled, strip later). */
    int pb_drawn = 0, pb_supp = 0, pb_min = 99, pb_max = -1, pb_opaque = 0;

    for (int y = 0; y < yl; y++) {
        const byte *row_src;
        if (use_psram) {
            if (pxm->nibble_packed) {
                /* 2 px/byte in PSRAM -- read the packed row and unpack, exactly
                   as the priority map below already does. Without this the load
                   reads index_xl BYTES of a row that is only half that long,
                   running past it and mis-indexing every pixel. This is how the
                   composed surface (which inherits nibble_packed from static_bg)
                   came back as garbage once the visual buffer was packed. */
                int rowbytes = (pxm->index_xl + 1) >> 1;
                uint32_t row_offset = (uint32_t)((src.y + y) * rowbytes);
                int c;

                psram_load(pxm->psram_addr + row_offset, s_psram_packed,
                           (size_t)rowbytes);
                for (c = 0; c < pxm->index_xl; c++)
                    s_psram_row[c] = (c & 1) ? (uint8_t)(s_psram_packed[c >> 1] >> 4)
                                             : (uint8_t)(s_psram_packed[c >> 1] & 0x0f);
            } else {
                uint32_t row_offset = (uint32_t)((src.y + y) * pxm->index_xl);
                psram_load(pxm->psram_addr + row_offset, s_psram_row,
                           (size_t)pxm->index_xl);
            }
            row_src = s_psram_row + src.x;
        } else {
            row_src = pxm->index_data + (src.y + y) * pxm->index_xl + src.x;
        }

        const uint8_t *pri_row = row_pri;  /* SRAM priority row, or NULL */
        /* Row-scope state for the optional bake write-back (see below). */
        int      pri_loaded = 0;    /* s_pri_row holds this row's PSRAM priorities */
        int      pri_dirty  = 0;    /* a bake modified s_pri_row -> store it back   */
        int      pri_pidx = 0, pri_byte0 = 0;
        size_t   pri_nbytes = 0;
        uint32_t pri_addr = 0;
        if (psram_pri) {
            int py = dest.y + y;
            if (py >= 0 && py < pri_yl && dest.x >= 0 && dest.x + xl <= pri_xl) {
                if (s_shared_priority->nibble_packed) {
                    /* Priority map is 2 px/byte in PSRAM (merged-decode packing).
                       Read the packed byte span covering this row and unpack the
                       nibbles into s_pri_row (mirrors _gfxop_scan_one_bitmask). */
                    int byteN;
                    pri_pidx  = py * pri_xl + dest.x;
                    pri_byte0 = pri_pidx >> 1;
                    byteN     = (pri_pidx + xl - 1) >> 1;
                    pri_nbytes = (size_t)(byteN - pri_byte0 + 1);
                    psram_load(s_shared_priority->psram_addr + (uint32_t)pri_byte0,
                               s_pri_pack, pri_nbytes);
                    for (int px = 0; px < xl; px++) {
                        int p = pri_pidx + px;
                        uint8_t b = s_pri_pack[(p >> 1) - pri_byte0];
                        s_pri_row[px] = (p & 1) ? (b >> 4) : (b & 0x0f);
                    }
                } else {
                    pri_addr = s_shared_priority->psram_addr
                               + (uint32_t)(py * pri_xl + dest.x);
                    psram_load(pri_addr, s_pri_row, (size_t)xl);
                }
                pri_row = s_pri_row;   /* gate against background base priority */
                pri_loaded = 1;
            }
            /* else: row off the priority map -> pri_row stays NULL -> write through */
        }

        for (int x = 0; x < xl; x++) {
            byte idx = row_src[x];
            if (!has_alpha || idx != color_key) {
                pb_opaque++;
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
                        BLIT_PUT(row_dst + x, lut[idx]);
                        if (row_pri)  /* SRAM working map, when one exists */
                            row_pri[x] = (uint8_t)priority;
                        else if (bake_pri && pri_loaded
                                 && (int)s_pri_row[x] < priority) {
                            /* Same "draw only lower priority" condition as the
                               desktop _gfxop_draw_priority DRAW_LOOP; the row is
                               stored back to PSRAM once, after the x loop. */
                            s_pri_row[x] = (uint8_t)priority;
                            pri_dirty = 1;
                        }
                        if (psram_pri) pb_drawn++;
                    } else if (psram_pri) {
                        pb_supp++;
                    }
                } else {
                    /* No priority map (background fill, text): always write. */
                    BLIT_PUT(row_dst + x, lut[idx]);
                }
            }
        }
        /* Store the baked picview priorities back.  Only the byte span that was
           loaded is written, and only nibbles inside [dest.x, dest.x+xl) were
           modified, so a pixel sharing an edge byte with a neighbour outside the
           cel keeps its original nibble. */
        if (pri_dirty) {
            if (s_shared_priority->nibble_packed) {
                for (int px = 0; px < xl; px++) {
                    int p = pri_pidx + px;
                    uint8_t *b = &s_pri_pack[(p >> 1) - pri_byte0];
                    uint8_t v = (uint8_t)(s_pri_row[px] & 0x0f);
                    *b = (p & 1) ? (uint8_t)((*b & 0x0f) | (v << 4))
                                 : (uint8_t)((*b & 0xf0) | v);
                }
                psram_store(s_shared_priority->psram_addr + (uint32_t)pri_byte0,
                            s_pri_pack, pri_nbytes);
            } else {
                psram_store(pri_addr, s_pri_row, (size_t)xl);
            }
        }

        row_dst += dest_stride;
        if (row_pri) row_pri += pri_stride;
    }

    /* [pblit] probe: report sprites whose pixels were occlusion-suppressed, so the
       cel priority can be compared against the background priority under it.
       Throttled to 1-in-8 to keep the serial log readable while walking. */
#ifdef FSCI_PROBE_GFX
    /* Diagnostic: log EVERY cel reaching the blit (not just suppressed ones) so a
       missing PQ2 cel can be classified from one capture:
         opaque=0          -> cel decoded empty/all-transparent (decode/offload bug)
         drawn=0 supp>0     -> fully occlusion-suppressed (priority)
         drawn>0            -> drew normally (look elsewhere: palette/clip)
       psram=0 means no bg-priority gate ran (opaque pixels written through).
       Restore the 1-in-8 throttle for routine walking once PQ2 is solved.

       TOP tag: cels landing in the upper screen band (dest.y < 100) get a
       trailing " <TOP>" so the SQ3-intro "Pirates of Pestulon" subtitle cel
       over the logo is greppable in one pass: grep '\[pblit\].*<TOP>'. */
    sciprintf("[pblit] cel pri=%d dest=(%d,%d %dx%d) psram=%d opaque=%d "
              "bgpri=%d..%d drawn=%d supp=%d%s\n",
              priority, dest.x, dest.y, xl, yl, psram_pri, pb_opaque,
              pb_min, pb_max, pb_drawn, pb_supp,
              (dest.y < 100) ? " <TOP>" : "");
#else
    (void)pb_drawn; (void)pb_supp; (void)pb_min; (void)pb_max; (void)pb_opaque;
#endif /* FSCI_PROBE_GFX */
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
    pico_blit_indexed(ps, bg, -1, full, dst, ps->visual[0],
                      dst.y * PICO_XSIZE + dst.x, PICO_VIS_PACKED,
                      PICO_XSIZE, NULL, 0, 0);
#ifdef PICO_USE_STATIC_VISUAL
    /* Seed the static buffer with the new room background. Picviews drawn after
       this (kAddToPic) composite on top of it, and every later BACK restore
       copies from here -- so the static buffer is the room's "clean plate".
       Copying from visual[0] rather than re-blitting from PSRAM keeps the two
       byte-identical and costs one memcpy. */
    if (ps->visual[PICO_VIS_STATIC]) {
        memcpy(ps->visual[PICO_VIS_STATIC], ps->visual[0],
               PICO_XSIZE * PICO_YSIZE);
        ps->static_dirty = 0;
    }
#endif
    /* Stage the new background in visual[0] but do NOT flush it to the LCD here.
       gfxop_new_pic runs this from inside kDrawPic, before kAnimate's open
       transition; an eager flush snapped the full new pic onto the screen, then
       animate_do_animation redrew the old screen and revealed the new one via the
       fade/curtain — the player saw "show, disappear, fade in again". Matching the
       desktop model (gfxop_new_pic only stages the static buffer, never the front),
       the reveal now comes solely from the normal pipeline (the open transition,
       FULL_REDRAW, or _reset_graphics_input on restore). visual[0] still holds the
       new pic so animate_do_animation's grab of newscreen reads it correctly. */
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

/* Bake a just-drawn GFX_BUFFER_STATIC region of visual[0] back into the
   PSRAM-resident static_bg, so subsequent GFX_BUFFER_BACK restores reproduce it.
   This is the Pico analogue of the SDL static buffer (visual[2]): there, static
   picviews (kAddToPic scene objects — PQ2's parking-lot cars, car-interior face,
   glovebox items) are drawn into visual[2] and sdl_update copies FROM visual[2]
   on every BACK restore. Pico has one visual[0]; without this bake-in the next
   BACK restore blits static_bg (the picview-less background) over the picview and
   erases it. visual[0] holds palette-slot bytes and static_bg->index_data is the
   identical palette slots (identity LUT for the 256-color background), so the
   region copies straight across. */
#ifndef PICO_USE_STATIC_VISUAL
/* Row staging for the PSRAM->PSRAM copies below: the PIO PSRAM is store/load
   only, with no block-copy primitive, so a copy has to land in SRAM in between.
   One row (320 B) keeps the SRAM cost negligible. */
static uint8_t s_compose_row[PICO_XSIZE];

/* Set by the widget layer around the FULLSCREEN-clipped static draw of a settled
   stopUpd dynview (widgets.c). It marks the one call whose dest rect is NOT
   port-clipped, and therefore the one that must never touch the displayed
   visual[0]: that out-of-port write is the PQ2 dialog bleed. kAddToPic picviews
   go through gfxop_draw_cel_static_CLIPPED, leave this 0, and keep drawing to
   visual[0] as before -- they draw exactly once (_gfxwop_pic_view_draw sets
   draw = _gfxwop_draw_nop), so routing them composed-only would make them wait
   on a later BACK restore to appear, which is very likely the original
   "static-only makes it invisible" observation. */
int pico_static_fullscreen = 0;

/* Bring the composed surface into existence for the CURRENT static_bg, seeded
   with a pristine copy of it. Lazy on purpose -- called from the bake path, so a
   room that never draws a static view never allocates it and never pays the
   copy. Re-allocated per room because psram_reset() rewinds the bump arena on
   every room change (gfxr_free_all_pics), which silently invalidates the old
   offset; composed_valid is cleared in pico_set_static_buffer to force that.
   Returns 0 if there is no PSRAM-backed background to compose from. */
static int pico_compose_ensure(struct _pico_state *ps)
{
    gfx_pixmap_t *bg = ps->static_bg;
    int bw, bh, y;
    uint32_t addr;

    if (!bg || bg->index_data || !bg->psram_valid) return 0;
    if (ps->composed_valid) return 1;

    bw = bg->index_xl;
    bh = bg->index_yl;
    if (bw <= 0 || bh <= 0 || bw > PICO_XSIZE) return 0;

    /* psram_alloc is an unchecked bump allocator -- it never returns a failure
       and offset 0 is a LEGITIMATE address, so "if (!addr)" would both reject a
       valid allocation and never catch exhaustion. Bound it explicitly against
       the fixed parse scratch instead, and decline to compose rather than let
       the arena run into it. */
    addr = psram_alloc((size_t)bw * (size_t)bh);
    if (addr + (uint32_t)(bw * bh) > PICO_PARSE_SCRATCH_ADDR)
        return 0;                   /* arena exhausted: fall back to pristine */

    {
        /* Row length is in BYTES, and a packed surface holds 2 px/byte -- copying
           bw bytes there would read past the row and leave the tail unwritten. */
        int rowbytes = bg->nibble_packed ? ((bw + 1) >> 1) : bw;

        for (y = 0; y < bh; y++) {
            psram_load(bg->psram_addr + (uint32_t)(y * rowbytes), s_compose_row,
                       (size_t)rowbytes);
            psram_store(addr + (uint32_t)(y * rowbytes), s_compose_row,
                        (size_t)rowbytes);
        }
    }

    /* Shallow mirror: pico_blit_indexed reads index dims, colors and psram_addr,
       none of which it owns, so sharing them with static_bg is safe as long as
       this is refreshed whenever static_bg changes -- which composed_valid
       guarantees, since it is cleared on every set_static_buffer. */
    ps->composed_pxm = *bg;
    ps->composed_pxm.psram_addr = addr;
    ps->composed_addr = addr;
    ps->composed_valid = 1;
    return 1;
}

/* PHASE 2b was a frame-level "re-bake or be erased" sweep over tracked rects.
   It is RETIRED: device-testing showed it erases exactly what should persist,
   because stopUpd means a settled view stops being redrawn, so its rect goes
   unmarked and the sweep wipes it (SQ3's door stopped closing). The
   settled-vs-gone distinction is not visible in rects at all. Phase 2c moves it
   to the widget layer (widgets.c), which observes move / resume-updating /
   dispose directly; pico_invalidate_static_region below is what it calls. */

/* Erase a baked static view: copy its rect back from the pristine static_bg into
   the composed surface, so the next BACK restore reproduces clean background
   there. This is the half that makes baking reversible -- WHO calls it (a
   stopUpd view resuming or being disposed) is engine-side and still to be
   wired; see CLAUDE.md. No-op until something has actually been composed. */
void pico_invalidate_static_region(struct _gfx_driver *drv, rect_t area)
{
    struct _pico_state *ps = drv ? (struct _pico_state *)drv->state : NULL;
    gfx_pixmap_t *bg;
    int bw, bh, x0, x1, w, row;

    if (!ps || !ps->composed_valid) return;
    bg = ps->static_bg;
    if (!bg || !bg->psram_valid) return;

    bw = bg->index_xl;
    bh = bg->index_yl;

    /* Bound the rect to the screen BEFORE looping. This is defence, not
       cosmetics: an out-of-range yl here means iterating that many times doing
       PSRAM I/O, which presents as a total freeze (screen and UART both dead)
       rather than a fault -- exactly what an uninitialised pico_has_baked
       produced. A bad rect must degrade to "restore nothing", never to a hang. */
    if (area.xl <= 0 || area.yl <= 0) return;
    if (area.xl > bw) area.xl = bw;
    if (area.yl > bh) area.yl = bh;

    x0 = area.x < 0 ? 0 : area.x;
    x1 = area.x + area.xl;
    if (x1 > bw) x1 = bw;
    w = x1 - x0;
    if (w <= 0) return;

    for (row = 0; row < area.yl; row++) {
        int by = area.y + row;
        if (by < 0 || by >= bh) continue;

        if (bg->nibble_packed) {
            /* Same shared-edge problem as the bake: at an odd x0 the first
               byte's low nibble belongs to x0-1, and at an odd end the last
               byte's high nibble belongs to x0+w. Copying those bytes whole
               would revert a neighbour that is not being invalidated. */
            int b0 = by * bw + x0, b1 = b0 + w;
            uint8_t src_edge, dst_edge;

            if (b0 & 1) {
                psram_load(bg->psram_addr + (uint32_t)(b0 >> 1), &src_edge, 1);
                psram_load(ps->composed_addr + (uint32_t)(b0 >> 1), &dst_edge, 1);
                dst_edge = (uint8_t)((dst_edge & 0x0f) | (src_edge & 0xf0));
                psram_store(ps->composed_addr + (uint32_t)(b0 >> 1), &dst_edge, 1);
                b0++;
            }
            if (b1 & 1) {
                b1--;
                psram_load(bg->psram_addr + (uint32_t)(b1 >> 1), &src_edge, 1);
                psram_load(ps->composed_addr + (uint32_t)(b1 >> 1), &dst_edge, 1);
                dst_edge = (uint8_t)((dst_edge & 0xf0) | (src_edge & 0x0f));
                psram_store(ps->composed_addr + (uint32_t)(b1 >> 1), &dst_edge, 1);
            }
            if (b1 > b0) {
                size_t nb = (size_t)((b1 - b0) >> 1);
                psram_load(bg->psram_addr + (uint32_t)(b0 >> 1), s_compose_row, nb);
                psram_store(ps->composed_addr + (uint32_t)(b0 >> 1), s_compose_row, nb);
            }
            continue;
        }

        psram_load(bg->psram_addr + (uint32_t)(by * bw + x0),
                   s_compose_row, (size_t)w);
        psram_store(ps->composed_addr + (uint32_t)(by * bw + x0),
                    s_compose_row, (size_t)w);
    }
}

#endif /* !PICO_USE_STATIC_VISUAL */

static void pico_bake_static_region(struct _pico_state *ps, rect_t dest)
{
    gfx_pixmap_t *bg = ps->static_bg;
    int bw, bh, x0, x1, w;

    /* Same gate as the PIO bake, deliberately ahead of the mirror: when there is
       no PSRAM-backed background (no pic set yet, or a window/closeup that never
       issued a kDrawPic) the PIO path persists NOTHING. Mirroring unconditionally
       would let the mapped target make things permanent that PIO never did --
       a dismissed dialog drawn in that state would be baked into the clean plate
       and then painted back by every subsequent BACK restore, so it could never
       be erased. Keep the two targets' persistence decisions identical. */
    if (!bg || bg->index_data || !bg->psram_valid) return;  /* not PSRAM-backed */

#ifdef PICO_USE_STATIC_VISUAL
    /* Mirror the drawn region into the SRAM static buffer. Two wins over baking
       into the PSRAM background: it is an SRAM memcpy rather than a PSRAM write,
       and it leaves static_bg PRISTINE -- baking altered the room's clean plate,
       which is what corrupted the add_to_pic overlay base (the SQ3 "Pirates of
       Pestulon" title). */
    if (ps->visual[PICO_VIS_STATIC] && ps->visual[0]) {
        int row, y, xs = dest.x < 0 ? 0 : dest.x;
        int xe = dest.x + dest.xl;
        if (xe > PICO_XSIZE) xe = PICO_XSIZE;
        if (xe > xs) {
            for (row = 0; row < dest.yl; row++) {
                y = dest.y + row;
                if (y < 0 || y >= PICO_YSIZE) continue;
                memcpy(ps->visual[PICO_VIS_STATIC] + y * PICO_XSIZE + xs,
                       ps->visual[0] + y * PICO_XSIZE + xs, (size_t)(xe - xs));
            }
        }
        return;
    }
#endif
    bw = bg->index_xl;
    bh = bg->index_yl;

    x0 = dest.x < 0 ? 0 : dest.x;
    x1 = dest.x + dest.xl;
    if (x1 > bw) x1 = bw;
    w = x1 - x0;
    if (w <= 0) return;

#ifndef PICO_USE_STATIC_VISUAL
    /* Bake into the COMPOSED surface, never into static_bg: the clean plate has
       to survive so pico_invalidate_static_region can undo this, and so the
       add_to_pic overlay path still finds an un-composited base. If the composed
       surface cannot be created (no PSRAM background, or arena exhausted) do
       NOTHING rather than fall back to writing the clean plate -- a bake that
       cannot be undone is the ghosting bug, and skipping it merely costs the
       view its persistence. */
    {
        uint32_t base;
        if (!pico_compose_ensure(ps)) return;
        base = ps->composed_addr;
        for (int row = 0; row < dest.yl; row++) {
            int by = dest.y + row;
            if (by < 0 || by >= bh) continue;
#ifdef PICO_PACK_VISUAL
            {
                /* Both surfaces are packed with the SAME pixel->byte mapping
                   (index = y*320 + x, and bw == PICO_XSIZE), so the interior is
                   a straight byte copy. Only the first and last bytes can be
                   SHARED with pixels outside [x0, x0+w) -- at an odd x0 the low
                   nibble belongs to x0-1, at an odd end the high nibble belongs
                   to x0+w -- and those need read-modify-write or the copy would
                   clobber a neighbour that is not being baked. */
                int b0 = by * PICO_XSIZE + x0;      /* first pixel  */
                int b1 = b0 + w;                    /* one past last */
                const uint8_t *vis = ps->visual[0];
                uint8_t edge;

                if (b0 & 1) {                       /* leading shared byte */
                    psram_load(base + (uint32_t)(b0 >> 1), &edge, 1);
                    edge = (uint8_t)((edge & 0x0f)
                                     | (uint8_t)(VIS_GET(vis, b0) << 4));
                    psram_store(base + (uint32_t)(b0 >> 1), &edge, 1);
                    b0++;
                }
                if (b1 & 1) {                       /* trailing shared byte */
                    b1--;
                    psram_load(base + (uint32_t)(b1 >> 1), &edge, 1);
                    edge = (uint8_t)((edge & 0xf0) | (uint8_t)VIS_GET(vis, b1));
                    psram_store(base + (uint32_t)(b1 >> 1), &edge, 1);
                }
                if (b1 > b0)                        /* aligned interior */
                    psram_store(base + (uint32_t)(b0 >> 1),
                                vis + (b0 >> 1), (size_t)((b1 - b0) >> 1));
            }
#else
            psram_store(base + (uint32_t)(by * bw + x0),
                        ps->visual[0] + by * PICO_XSIZE + x0, (size_t)w);
#endif
        }
    }
#else
    for (int row = 0; row < dest.yl; row++) {
        int by = dest.y + row;
        if (by < 0 || by >= bh) continue;
        psram_store(bg->psram_addr + (uint32_t)(by * bw + x0),
                    ps->visual[0] + by * PICO_XSIZE + x0, (size_t)w);
    }
#endif
}

static int pico_draw_pixmap(struct _gfx_driver *drv, gfx_pixmap_t *pxm,
                             int priority,
                             rect_t src, rect_t dest, gfx_buffer_t buffer)
{
    /* Every draw goes to the back buffer, including STATIC ones. Sending a
       STATIC draw ONLY to the static buffer (the literal desktop model) makes it
       invisible here: desktop follows it with update(BACK)+update(FRONT), but
       the Pico engine path does not reliably issue those, so the pixels never
       reach the panel. Persistence is handled by MIRRORING the drawn region into
       the static buffer afterwards instead -- same semantics as the PIO bake. */
    int bufnr = 0;

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
            for (int row = 0; row < dest.yl; row++) {
#ifdef PICO_PACK_VISUAL
                int b = (dest.y + row) * PICO_XSIZE + dest.x, c;
                psram_load(addr + (uint32_t)((src.y + row) * pxm->xl + src.x),
                           s_vis_row, (size_t)copy_w);
                for (c = 0; c < copy_w; c++)
                    VIS_SET(S->visual[bufnr], b + c, s_vis_row[c]);
#else
                psram_load(addr + (uint32_t)((src.y + row) * pxm->xl + src.x),
                           S->visual[bufnr] + (dest.y + row) * PICO_XSIZE + dest.x,
                           (size_t)copy_w);
#endif
            }
        } else if (pxm->data) {
            /* SRAM grab */
            for (int row = 0; row < dest.yl; row++) {
#ifdef PICO_PACK_VISUAL
                int b = (dest.y + row) * PICO_XSIZE + dest.x, c;
                const uint8_t *srow = pxm->data + (src.y + row) * pxm->xl + src.x;
                for (c = 0; c < copy_w; c++)
                    VIS_SET(S->visual[bufnr], b + c, srow[c]);
#else
                memcpy(S->visual[bufnr] + (dest.y + row) * PICO_XSIZE + dest.x,
                       pxm->data + (src.y + row) * pxm->xl + src.x, copy_w);
#endif
            }
        }
        return GFX_OK;
    }

#if !defined(PICO_USE_STATIC_VISUAL) && defined(PICO_STATIC_COMPOSED)
    /* Desktop model, finally expressible now that a second surface exists.
       Desktop never picks ONE clip for a static view -- it uses two
       destinations, each with its own clip: visual[2] (persistence) takes the
       FULLSCREEN-clipped draw, visual[1] (displayed) takes the port-clipped
       one. Pico collapsed both into visual[0], so one clip had to serve both
       roles -- which is why fullscreen bled over dialogs and the ambient clip
       lost the door: with a single destination there was no third answer.

       Here the fullscreen static draw goes to the COMPOSED surface ONLY. It
       never touches visual[0], so it cannot overpaint a dialog; the view still
       reaches the screen via the paired port-clipped gfxop_draw_cel and via
       BACK restores, which read composed (measured: a BACK restore covers
       100% of static draws in the SAME frame, and a FRONT flush 100%, so the
       old "the Pico engine path does not reliably issue those" no longer
       holds -- what was missing then was a surface worth restoring FROM).

       Row-at-a-time read-modify-write: the cel may be transparent, so the
       row must be preloaded with what composed already holds. Priority is
       unchanged -- the same bake_static_pri path runs, just per row. */
    if (buffer == GFX_BUFFER_STATIC && pico_static_fullscreen
        && !pxm->data && pico_compose_ensure(S)) {
        gfx_pixmap_t *bg = S->static_bg;
        int bw = bg->index_xl;
        gfx_pixmap_t *pmap = s_shared_priority;
        uint8_t *pridata = (pmap && pmap->index_data) ? pmap->index_data : NULL;
        int pri_stride = pridata ? pmap->index_xl : 0;
        int x0 = dest.x < 0 ? 0 : dest.x;
        int w  = dest.x + dest.xl > bw ? bw - x0 : dest.xl - (x0 - dest.x);

        if (w > 0) for (int row = 0; row < dest.yl; row++) {
            int dy = dest.y + row;
            if (dy < 0 || dy >= bg->index_yl) continue;
            psram_load(S->composed_addr + (uint32_t)(dy * bw + x0),
                       s_compose_row, (size_t)w);
            pico_blit_indexed(S, pxm, priority,
                              gfx_rect(src.x + (x0 - dest.x), src.y + row, w, 1),
                              gfx_rect(x0, dy, w, 1),
                              s_compose_row, 0, 0, w,
                              pridata ? pridata + dy * pri_stride + x0 : NULL,
                              pri_stride, 1);
            psram_store(S->composed_addr + (uint32_t)(dy * bw + x0),
                        s_compose_row, (size_t)w);
        }
        return GFX_OK;
    }
#endif

    /* Normal pixmap: blit to visual buffer.
       If pxm->data is NULL (gfx_xlate_pixmap skipped on Pico), use the
       direct indexed blit that translates index_data+colors on-the-fly. */
    uint8_t *destptr = S->visual[bufnr] + dest.y * PICO_XSIZE + dest.x;
    if (!pxm->data) {
        /* Mirror the desktop map choice (_gfxop_draw_pixmap: static_buf ?
           static_priority_map : priority_map). A STATIC draw (kAddToPic picview)
           must write the CLEAN PLATE, or the per-frame copyback would erase its
           priority and actors would stop being occluded by it. */
        gfx_pixmap_t *pmap = s_shared_priority;
#ifdef PICO_WORKING_PRIORITY
        if (buffer == GFX_BUFFER_STATIC && s_static_priority)
            pmap = s_static_priority;
#endif
        uint8_t *pridata = (pmap && pmap->index_data) ? pmap->index_data : NULL;
        uint8_t *priptr = pridata
                          ? (pridata + dest.y * pmap->index_xl + dest.x)
                          : NULL;
        int pri_stride = pridata ? pmap->index_xl : 0;
        pico_blit_indexed(S, pxm, priority, src, dest,
                          S->visual[bufnr], dest.y * PICO_XSIZE + dest.x,
                          bufnr == 0 ? PICO_VIS_PACKED : 0,
                          PICO_XSIZE, priptr, pri_stride,
                          buffer == GFX_BUFFER_STATIC);
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

    /* Static picviews (kAddToPic scene objects) must survive BACK restores: bake
       the drawn region into the PSRAM static_bg (see pico_bake_static_region).
       Skipped when a NO_UPDATE dynview is routed here in priority-only mode — it
       gets its priority baked (above) but must NOT persist color, or it ghosts. */
    if (buffer == GFX_BUFFER_STATIC && !pico_priority_only_static)
        pico_bake_static_region(S, dest);

#ifdef FSCI_PROBE_GFX
    /* [pbuf] probe: classify which buffer each cel targets and whether the static
       bake fired, to pin down the PQ2 glovebox-closeup miss. The companion [pblit]
       line lacks the buffer arg; this adds it.
         buf=STATIC + baked=1                  -> kAddToPic picview (face/cars path)
         buf=BACK                              -> window/control contents (bake never
                                                  fires; next BACK restore erases it)
       sbg/sbg_psram name the current static_bg so a stale/previous-room background
       at glovebox-open time is visible (bake would write into the wrong room).
       Strip with the other GFX probes once PQ2 is solved. */
    {
        const char *bn = buffer == GFX_BUFFER_STATIC ? "STATIC"
                       : buffer == GFX_BUFFER_BACK   ? "BACK"
                       : buffer == GFX_BUFFER_FRONT  ? "FRONT" : "?";
        sciprintf("[pbuf] buf=%s dest=(%d,%d %dx%d) baked=%d sbg=%p sbg_psram=%d\n",
                  bn, dest.x, dest.y, dest.xl, dest.yl,
                  (buffer == GFX_BUFFER_STATIC) ? 1 : 0,
                  (void *)S->static_bg,
                  (S->static_bg && S->static_bg->psram_valid) ? 1 : 0);
    }
#endif /* FSCI_PROBE_GFX */

    return GFX_OK;
}

/* Save-unders larger than this go to the PSRAM bump arena rather than SRAM.
   Raising it on the mapped target was tried (to dodge psram_reset clobbering a
   save-under held across a room change) and REVERTED: it did not fix the PQ2
   dialogs it was aimed at, and it is expensive -- the old_screen grab alone is
   320x190 = 60800 bytes on every pic transition, which together with the static
   visual buffer and the two priority maps drove the arena to 470628 of a 474728
   heap span and OOM'd. Keep save-unders in PSRAM on both targets. */
#define PICO_GRAB_SRAM_MAX  ((size_t)4096)

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
        if (sz > PICO_GRAB_SRAM_MAX) {
            /* Large grab: save row-by-row into PSRAM to avoid SRAM pressure.
               PSRAM address is bump-allocated and reclaimed on psram_reset(). */
            uint32_t addr = psram_alloc(sz);
            for (int row = 0; row < src.yl; row++) {
#ifdef PICO_PACK_VISUAL
                int b = (src.y + row) * PICO_XSIZE + src.x, c;
                for (c = 0; c < src.xl; c++)
                    s_vis_row[c] = (uint8_t)VIS_GET(S->visual[0], b + c);
                psram_store(addr + (uint32_t)(row * src.xl), s_vis_row, src.xl);
#else
                psram_store(addr + (uint32_t)(row * src.xl),
                            S->visual[0] + (src.y + row) * PICO_XSIZE + src.x,
                            src.xl);
#endif
            }
            pxm->data = (uint8_t *)(uintptr_t)addr;
            pxm->internal.info = (void *)(uintptr_t)1;
            pico_grab_psram_total++;
        } else {
            /* Small grab: SRAM */
            if (!pxm->data) {
                pxm->data = (uint8_t *)sci_malloc_sram(sz);
                if (!pxm->data) return GFX_FATAL;
                pico_grab_sram_live++;
                pico_grab_sram_bytes += sz;
                pico_grab_sram_total++;
            }
            for (int row = 0; row < src.yl; row++) {
#ifdef PICO_PACK_VISUAL
                int b = (src.y + row) * PICO_XSIZE + src.x, c;
                for (c = 0; c < src.xl; c++)
                    pxm->data[row * src.xl + c] =
                        (uint8_t)VIS_GET(S->visual[0], b + c);
#else
                memcpy(pxm->data + row * src.xl,
                       S->visual[0] + (src.y + row) * PICO_XSIZE + src.x,
                       src.xl);
#endif
            }
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
#ifdef FSCI_PROBE_GFX
        /* [pupd] probe: BACK restore region. This is the static_bg -> visual[0]
           erase. If a glovebox-item rect (e.g. 243,121 or 263,109) is covered by
           a BACK restore that is NOT followed by a redraw of that item before the
           next FRONT flush, the item is erased. Compare these rects against the
           [pblit]/[pbuf] item draws to see if an item draw is being undone. */
        sciprintf("[pupd] BACK restore src=(%d,%d %dx%d) dest=(%d,%d)\n",
                  src.x, src.y, src.xl, src.yl, dest.x, dest.y);
#endif
#ifdef PICO_USE_STATIC_VISUAL
        /* Desktop model (sdl_update: data_source = STATIC for a BACK restore):
           copy the dirty region straight out of the static buffer, which already
           holds the background WITH its kAddToPic picviews composited in. Both
           buffers are SRAM, so this is a plain per-row memcpy -- and it replaces
           a per-row PSRAM read, so the restore gets cheaper, not dearer. */
        if (S->visual[PICO_VIS_STATIC] && S->visual[0]) {
            int row;
            int w = src.xl, h = src.yl;
            /* Self-correcting refresh: if the background changed without going
               through pico_render_background, rebuild the whole clean plate from
               static_bg before serving restores out of it. The PIO path cannot
               go stale because it reads static_bg directly every time. */
            if (S->static_dirty && S->static_bg
                && (S->static_bg->index_data || S->static_bg->psram_valid)) {
                rect_t f = gfx_rect(0, 0, S->static_bg->index_xl,
                                    S->static_bg->index_yl);
                rect_t d = gfx_rect(0, 0, S->static_bg->index_xl,
                                    S->static_bg->index_yl);
                pico_blit_indexed(S, S->static_bg, -1, f, d,
                                  S->visual[PICO_VIS_STATIC], 0, 0, PICO_XSIZE,
                                  NULL, 0, 0);
                S->static_dirty = 0;
            }
            if (dest.x + w > PICO_XSIZE) w = PICO_XSIZE - dest.x;
            if (dest.y + h > PICO_YSIZE) h = PICO_YSIZE - dest.y;
            for (row = 0; row < h; row++)
                memcpy(S->visual[0] + (dest.y + row) * PICO_XSIZE + dest.x,
                       S->visual[PICO_VIS_STATIC]
                           + (src.y + row) * PICO_XSIZE + src.x,
                       (size_t)(w > 0 ? w : 0));
            break;
        }
        /* else fall through to the PSRAM restore below (static buffer missing) */
#endif
        /* Restore background from PSRAM into visual[0] for this dirty region.
           Reads the COMPOSED surface (background + baked static views) when one
           exists, which is the whole point of keeping it: a kAddToPic picview
           survives the restore instead of being erased by the picview-less
           plate. Falls back to the pristine static_bg when nothing has been
           baked this room -- byte-for-byte the previous behaviour. */
        if (S->static_bg && S->visual[0]) {
            uint8_t *destptr = S->visual[0] + dest.y * PICO_XSIZE + dest.x;
            rect_t bgsrc = gfx_rect(src.x, src.y, src.xl, src.yl);
            rect_t bgdst = gfx_rect(0, 0, src.xl, src.yl);
            gfx_pixmap_t *srcmap = S->static_bg;
#ifndef PICO_USE_STATIC_VISUAL
            if (S->composed_valid) srcmap = &S->composed_pxm;
#endif
            pico_blit_indexed(S, srcmap, -1, bgsrc, bgdst,
                              S->visual[0], dest.y * PICO_XSIZE + dest.x,
                              PICO_VIS_PACKED, PICO_XSIZE, NULL, 0, 0);
        }
        break;

    case GFX_BUFFER_FRONT:
#ifdef FSCI_PROBE_GFX
        /* [pupd] probe: FRONT flush region = exactly what reaches the LCD. If a
           glovebox-item rect is drawn to visual[0] but NO FRONT flush covers it,
           the item is in the back buffer but never pushed to the panel. */
        sciprintf("[pupd] FRONT flush dest=(%d,%d %dx%d)\n",
                  dest.x, dest.y, src.xl, src.yl);
#endif
        flush_region(S, dest.x, dest.y, src.xl, src.yl);
#ifdef FSCI_PROBE_FPS
        /* Frame rate == the sound poll rate (pico_sfx_poll is driven from here),
           and the poll rate sets the MINIMUM safe PICO_PWM_BUF_FRAMES, since the
           mixer emits at most buf_size per call: buf_size >= rate / poll_rate.
           Deliberately usable WITHOUT sound, so a target can be measured before
           deciding whether audio fits there -- the two Pico targets have quite
           different frame rates (PIO reads priority back over PIO-SPI per row;
           the mapped build reads SRAM), so each must be sized from its own
           number. Report the MINIMUM over the interval: the worst frame is what
           starves the ring, not the average. */
        {
            static unsigned fps_frames = 0, fps_min = 0xffffffffu;
            static uint64_t fps_last_us = 0, fps_prev_us = 0;
            uint64_t now_us = time_us_64();
            if (fps_prev_us) {
                unsigned dt = (unsigned)(now_us - fps_prev_us);
                if (dt < fps_min) fps_min = dt;
            }
            fps_prev_us = now_us;
            fps_frames++;
            if (now_us - fps_last_us >= 1000000u) {
                sciprintf("[fps] frames=%u in %lums (worst gap %ums ->"
                          " min buf_size = rate/%u)\n",
                          fps_frames,
                          (unsigned long)((now_us - fps_last_us) / 1000),
                          fps_min == 0xffffffffu ? 0 : fps_min / 1000,
                          fps_frames ? fps_frames : 1);
                fps_frames = 0;
                fps_min = 0xffffffffu;
                fps_last_us = now_us;
            }
        }
#endif
        /* Per-frame keyboard poll + pace. Covers animation loops that never
           call kGetEvent/kWait (e.g. the SQ3 intro), which otherwise run
           unpaced. Throttled inside poll_keyboard to one read per frame. */
        poll_keyboard(drv);
#ifdef PICO_PWM_AUDIO
        /* Feed the sound pipeline here too. Driving it ONLY from
           pico_get_event tied audio production to how often the game asks for
           input, which is sporadic -- measured at 0.3-7 polls/sec during the
           SQ3 intro, and 50 seconds with none at all at startup. The mixer
           produces at most buf_size per call, so a starved poll rate starves
           the ring and the PWM IRQ holds last_sample, stretching and chopping
           the audio. This is the same reasoning that already put poll_keyboard
           on the front flush. pico_sfx_poll self-limits to 60Hz, so the extra
           calls are near-free no-ops. */
        pico_sfx_poll();
#endif
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
#ifndef PICO_USE_STATIC_VISUAL
    /* Drop the composed surface: its PSRAM offset came from the bump arena,
       which psram_reset() rewound on this room change, so the old offset now
       aliases whatever the new room decoded into. It is re-created lazily on
       the next bake. Clearing it here is what keeps a previous room's composite
       from being served to BACK restores. */
    S->composed_valid = 0;
    S->composed_addr = 0;
#endif
#ifdef PICO_USE_STATIC_VISUAL
    /* The static buffer now predates the current background. It is refreshed
       lazily at the next BACK restore rather than here, because the pic's PSRAM
       content is not necessarily decoded yet at this point. Without this the
       buffer could keep a PREVIOUS room and BACK restores would paint that old
       room back over parts of the new one. */
    S->static_dirty = 1;
#endif
#ifdef FSCI_PROBE_GFX
    /* [pstat] probe: every static_bg swap. If opening the glovebox emits a [pstat]
       line, the closeup is a full kDrawPic (static_bg refreshed -> bake target is
       correct); if it does NOT, the closeup is an inset/window and static_bg stays
       the car interior (bake writes into the wrong room / BACK restore wipes it). */
    sciprintf("[pstat] set_static_buffer pic=%p %dx%d psram=%d\n",
              (void *)pic, pic ? pic->index_xl : 0, pic ? pic->index_yl : 0,
              (pic && pic->psram_valid) ? 1 : 0);
#endif /* FSCI_PROBE_GFX */
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
#ifdef PICO_LCD_16BIT
    /* keep the packed palette in sync */
    pico_rebuild_pal565(S);
#endif
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
#ifdef PICO_PWM_AUDIO
    /* Drive the SCI sound pipeline in normal (non-IRQ) context once per
       frame; the trivial ring-pop runs in the PWM IRQ (pwm_synth.c). */
    pico_sfx_poll();
#endif
    poll_keyboard(drv);
    return pop_event(S);
}

static int pico_usec_sleep(struct _gfx_driver *drv, long usecs)
{
    (void)drv;
    /* Pure sleep: keyboard polling/pacing now lives in the per-frame front
       flush (pico_update) and pico_get_event, so gfxop_usleep() times
       accurately instead of being floored at ~16ms by an embedded i2c read. */
#ifdef PICO_PWM_AUDIO
    {
        /* ...but sound cannot pause while the game waits. An animation delay
           spent entirely inside sleep_us() produces no samples at all, so the
           ring drains and the audio stalls. Break the sleep into slices and
           feed the pipeline between them; pico_sfx_poll's own 60Hz gate means
           most slices do nothing. */
        long remaining = usecs > 10000 ? 10000 : usecs;
        while (remaining > 0) {
            long slice = remaining > 2000 ? 2000 : remaining;
            sleep_us((uint64_t)slice);
            remaining -= slice;
            pico_sfx_poll();
        }
    }
#else
    sleep_us((uint64_t)(usecs > 10000 ? 10000 : usecs));
#endif
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
