#ifdef HAVE_PICO

#include "psram_alloc.h"
#include "psram/psram_spi.h"
#include <string.h>
#include <stdio.h>

psram_spi_inst_t g_psram;

static uint32_t s_psram_offset = 0;

uint32_t
psram_alloc(size_t bytes)
{
    uint32_t addr = s_psram_offset;
    s_psram_offset += (uint32_t)bytes;
    return addr;
}

void
psram_reset(void)
{
    s_psram_offset = 0;
}

/* Song slots (see psram_alloc.h).  Deliberately NOT touched by psram_reset:
   music plays across room changes, which is exactly when the bump arena is
   rewound. */
/* Refcounted, not a plain in-use flag: _SIMSG_BASEMSG_CLONE memcpy's a whole
   iterator, so two iterators end up naming the same slot.  With a bare flag the
   first teardown would release it while the clone was still reading. */
static uint8_t s_song_slot_refs[PSRAM_SONG_SLOTS];

uint32_t
psram_song_alloc(size_t bytes)
{
    int i;

    if (bytes > PSRAM_SONG_SLOT_SIZE)
        return PSRAM_SONG_NONE;
    for (i = 0; i < PSRAM_SONG_SLOTS; i++)
        if (!s_song_slot_refs[i]) {
            s_song_slot_refs[i] = 1;
            return PSRAM_SONG_BASE + (uint32_t)i * PSRAM_SONG_SLOT_SIZE;
        }
    return PSRAM_SONG_NONE;   /* caller falls back to SRAM */
}

/* Slots in use, for the [mem] SOUND probe.  Without this a slot LEAK is
   invisible: leaked slots do not show up in refcnt (that counts SRAM copies
   only), they just silently push later large songs back into SRAM. */
int
psram_song_slots_used(void)
{
    int i, n = 0;

    for (i = 0; i < PSRAM_SONG_SLOTS; i++)
        if (s_song_slot_refs[i])
            n++;
    return n;
}

void
psram_song_incref(uint32_t addr)
{
    uint32_t i;

    if (addr == PSRAM_SONG_NONE || addr < PSRAM_SONG_BASE)
        return;
    i = (addr - PSRAM_SONG_BASE) / PSRAM_SONG_SLOT_SIZE;
    if (i < PSRAM_SONG_SLOTS && s_song_slot_refs[i] < 255)
        s_song_slot_refs[i]++;
}

void
psram_song_free(uint32_t addr)
{
    uint32_t i;

    if (addr == PSRAM_SONG_NONE || addr < PSRAM_SONG_BASE)
        return;
    i = (addr - PSRAM_SONG_BASE) / PSRAM_SONG_SLOT_SIZE;
    if (i < PSRAM_SONG_SLOTS && s_song_slot_refs[i])
        s_song_slot_refs[i]--;
}

/*
 * The PIO command protocol encodes "bits to write" and "bits to read" as
 * single uint8_t fields.  For a write: bits = (4 + data_bytes) * 8 must fit
 * in a uint8_t → max data_bytes = 27 ((4+27)*8 = 248 ≤ 255).
 * For a read:  bits = data_bytes * 8 must fit in a uint8_t → max = 31.
 * Larger transfers are handled by looping over same-sized chunks, each with
 * its own CS pulse, command, and (incremented) address.
 */
#define PSRAM_MAX_WRITE_CHUNK 27
#define PSRAM_MAX_READ_CHUNK  31

void
psram_store(uint32_t addr, const uint8_t *src, size_t len)
{
    static uint8_t wbuf[6 + PSRAM_MAX_WRITE_CHUNK];
    wbuf[1] = 0;      /* y = 0 (no read phase) */
    wbuf[2] = 0x02u;  /* SPI write command */
    while (len > 0) {
        size_t chunk = (len > PSRAM_MAX_WRITE_CHUNK) ? PSRAM_MAX_WRITE_CHUNK : len;
        wbuf[0] = (uint8_t)((4 + chunk) * 8); /* cmd(1)+addr(3)+data(chunk) bits */
        wbuf[3] = (uint8_t)(addr >> 16);
        wbuf[4] = (uint8_t)(addr >> 8);
        wbuf[5] = (uint8_t)(addr);
        memcpy(wbuf + 6, src, chunk);
        pio_spi_write_dma_blocking(&g_psram, wbuf, 6 + chunk);
        addr += (uint32_t)chunk;
        src  += chunk;
        len  -= chunk;
    }
}

void
psram_load(uint32_t addr, uint8_t *dst, size_t len)
{
    while (len > 0) {
        size_t chunk = (len > PSRAM_MAX_READ_CHUNK) ? PSRAM_MAX_READ_CHUNK : len;
        psram_read(&g_psram, addr, dst, chunk);
        addr += (uint32_t)chunk;
        dst  += chunk;
        len  -= chunk;
    }
}

#endif /* HAVE_PICO */
