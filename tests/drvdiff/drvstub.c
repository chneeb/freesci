/* drvstub.c -- host stubs for the PicoCalc hardware pico_driver.c talks to,
   so the driver itself can be compiled and exercised on the desktop.

   The one that matters is hw_send_spi: that is where pixels leave for the
   panel, so capturing it gives the exact RGB bytes a user would see. Everything
   else is inert. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- captured panel output ------------------------------------------- */

#define CAP_MAX (4 * 1024 * 1024)
static uint8_t cap_buf[CAP_MAX];
static size_t  cap_len = 0;

void capture_reset(void) { cap_len = 0; }
size_t capture_len(void) { return cap_len; }
const uint8_t *capture_data(void) { return cap_buf; }

/* Region headers are captured too: a correct pixel stream sent to the WRONG
   window is still a wrong picture, and that is precisely the class of bug the
   16-bit LCD attempt hit. */
void define_region_spi(int x1, int y1, int x2, int y2, int set)
{
	int hdr[5];

	hdr[0] = x1; hdr[1] = y1; hdr[2] = x2; hdr[3] = y2; hdr[4] = set;
	if (cap_len + sizeof(hdr) <= CAP_MAX) {
		memcpy(cap_buf + cap_len, hdr, sizeof(hdr));
		cap_len += sizeof(hdr);
	}
}

void hw_send_spi(const uint8_t *buf, int len)
{
	if (len > 0 && cap_len + (size_t)len <= CAP_MAX) {
		memcpy(cap_buf + cap_len, buf, (size_t)len);
		cap_len += (size_t)len;
	}
}

/* ---- inert hardware --------------------------------------------------- */

void lcd_init(void) { }
void lcd_spi_raise_cs(void) { }
void spi_finish(void *unused) { (void)unused; }
void spi_set_baudrate(void *u, unsigned b) { (void)u; (void)b; }
void kbd_input_init(void) { }
int  kbd_read(void) { return 0; }
void sleep_ms(uint32_t ms) { (void)ms; }
void sleep_us(uint64_t us) { (void)us; }
uint64_t time_us_64(void) { static uint64_t t; return t += 1000; }

/* ---- PSRAM backed by plain RAM (same idea as picodiff's psram_stub) ---- */

#define PSRAM_SIZE (8u * 1024u * 1024u)
static uint8_t *psram;
static uint32_t psram_off;

static void psram_ensure(void)
{
	if (!psram) psram = calloc(PSRAM_SIZE, 1);
}

uint32_t psram_alloc(size_t bytes)
{
	uint32_t a;

	psram_ensure();
	a = psram_off;
	psram_off += (uint32_t)bytes;
	return a;
}

void psram_reset(void) { psram_off = 0; }

void psram_store(uint32_t addr, const uint8_t *src, size_t len)
{
	psram_ensure();
	if (addr + len <= PSRAM_SIZE) memcpy(psram + addr, src, len);
}

void psram_load(uint32_t addr, uint8_t *dst, size_t len)
{
	psram_ensure();
	if (addr + len <= PSRAM_SIZE) memcpy(dst, psram + addr, len);
	else memset(dst, 0, len);
}
