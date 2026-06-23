/***************************************************************************
 pico_pwm.c Copyright (C) 2026 FreeSCI PicoCalc port

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

***************************************************************************/

/* PicoCalc PWM PCM output device.
**
** The SCI sound pipeline (player->maintenance + mixer->process) is driven
** POLLED from the main loop, not from a hardware IRQ: pico_sfx_poll() is
** called once per frame and, at ~60Hz, invokes the SCI timer callback in
** normal (non-IRQ) context. This keeps malloc/sciprintf — which the mixer
** may do while (re)allocating feed buffers — out of IRQ context, which
** matters on this fragmentation-prone single-heap device.
**
** output() converts the mixer's 16-bit signed mono frames to 8-bit unsigned
** samples and pushes them into the PWM synth's lock-free ring. A lightweight
** 22050Hz PWM IRQ (pwm_synth.c) pops one sample per tick.
*/

#include <sfx_pcm.h>

#ifdef PICO_PWM_AUDIO

#include "../../platform/pico/audio/pwm_synth.h"

#define PICO_PWM_RATE 22050
#define PICO_PWM_BUF_FRAMES 512 /* mixer compbuf = 2 * this * 4 bytes; halved
				    from 1024 to reclaim ~6KB of heap headroom on
				    the SRAM-tight Pico (sound build). At 60Hz poll
				    this still produces 23ms of audio/poll vs the
				    16.6ms poll interval, so the ring never starves. */

static int pico_pwm_active = 0;
static int pico_sfx_blocked = 0;

/* --- polled timer state --- */
static void (*pico_sfx_timer_callback)(void *data) = NULL;
static void *pico_sfx_timer_data = NULL;
static long pico_sfx_last_secs = 0, pico_sfx_last_usecs = 0;

#define PICO_SFX_INTERVAL_USEC (1000000 / 60)

/* Bias-converts and enqueues 'count' mono S16-native frames. */
static int
pcmout_pico_output(sfx_pcm_device_t *self, byte *buf, int count,
		   sfx_timestamp_t *ts)
{
	gint16 *src = (gint16 *) buf;
	int i;

	for (i = 0; i < count; i++) {
		/* S16 signed -> 8-bit unsigned (PWM wrap is 256). */
		int s = (src[i] >> 8) + 128;
		if (s < 0)
			s = 0;
		else if (s > 255)
			s = 255;
		if (!pwm_synth_push_sample((uint8_t) s))
			break; /* ring full: drop the rest, IRQ will catch up */
	}
	return SFX_OK;
}

static sfx_timestamp_t
pcmout_pico_output_timestamp(sfx_pcm_device_t *self)
{
	long secs, usecs;
	sci_gettime(&secs, &usecs);
	/* Next frame plays after everything still queued in the ring. */
	return sfx_timestamp_add(sfx_new_timestamp(secs, usecs, PICO_PWM_RATE),
				 pwm_synth_ring_pending());
}

static int
pcmout_pico_init(sfx_pcm_device_t *self)
{
	self->conf.rate = PICO_PWM_RATE;
	self->conf.stereo = SFX_PCM_MONO;
	self->conf.format = SFX_PCM_FORMAT_S16_NATIVE;
	self->buf_size = PICO_PWM_BUF_FRAMES;

	pwm_synth_ring_reset();
	pico_pwm_active = 1;

	sciprintf("[SND:pico-pwm] Initialised: %dHz mono 8-bit PWM\n",
		  PICO_PWM_RATE);
	return SFX_OK;
}

static void
pcmout_pico_exit(sfx_pcm_device_t *self)
{
	pico_pwm_active = 0;
	pwm_synth_ring_reset();
	pwm_synth_silence_all_channels();
}

static int
pcmout_pico_set_option(sfx_pcm_device_t *self, char *name, char *value)
{
	return SFX_ERROR; /* No options supported */
}

/* --- polled timer (attached to the device, preferred by core.c) --- */

static int
timer_pico_set_option(char *name, char *value)
{
	return SFX_ERROR;
}

static int
timer_pico_init(void (*callback)(void *data), void *data)
{
	pico_sfx_timer_callback = callback;
	pico_sfx_timer_data = data;
	sci_gettime(&pico_sfx_last_secs, &pico_sfx_last_usecs);
	return SFX_OK;
}

static int
timer_pico_stop(void)
{
	pico_sfx_timer_callback = NULL;
	return SFX_OK;
}

static int
timer_pico_block(void)
{
	pico_sfx_blocked++;
	return SFX_OK;
}

static int
timer_pico_unblock(void)
{
	if (pico_sfx_blocked > 0)
		pico_sfx_blocked--;
	return SFX_OK;
}

#define PICO_PWM_VERSION "0.1"

sfx_timer_t pcmout_pico_timer = {
	"pico-pwm-timer",
	PICO_PWM_VERSION,
	17, /* ~1000/60 ms; passed to the player as its tempo base */
	0,
	timer_pico_set_option,
	timer_pico_init,
	timer_pico_stop,
	timer_pico_block,
	timer_pico_unblock
};

sfx_pcm_device_t sfx_pcm_driver_pico_pwm = {
	"pico_pwm",
	PICO_PWM_VERSION,
	pcmout_pico_init,
	pcmout_pico_exit,
	pcmout_pico_set_option,
	pcmout_pico_output,
	pcmout_pico_output_timestamp,
	{0, 0, 0},
	0,
	&pcmout_pico_timer,
	NULL
};

/* Called once per frame from the Pico main loop. Drives the SCI sound
** pipeline in normal context at ~60Hz. */
void
pico_sfx_poll(void)
{
	long secs, usecs;
	long delta;

	if (!pico_pwm_active || pico_sfx_blocked || !pico_sfx_timer_callback)
		return;

	sci_gettime(&secs, &usecs);
	delta = (secs - pico_sfx_last_secs) * 1000000 + (usecs - pico_sfx_last_usecs);

	if (delta < PICO_SFX_INTERVAL_USEC)
		return;

	/* Advance the deadline by whole intervals; cap catch-up so a long
	** stall (e.g. a slow room load) doesn't spiral into a flood of
	** back-to-back maintenance passes. */
	if (delta > 4 * PICO_SFX_INTERVAL_USEC) {
		pico_sfx_last_secs = secs;
		pico_sfx_last_usecs = usecs;
	} else {
		pico_sfx_last_usecs += PICO_SFX_INTERVAL_USEC;
		if (pico_sfx_last_usecs >= 1000000) {
			pico_sfx_last_usecs -= 1000000;
			pico_sfx_last_secs++;
		}
	}

	pico_sfx_timer_callback(pico_sfx_timer_data);
}

#endif /* PICO_PWM_AUDIO */
