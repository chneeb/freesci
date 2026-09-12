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

#define PICO_PWM_RATE PICO_SND_RATE
/* The mixer produces AT MOST buf_size frames per call (mix_compute_buf_len caps
** demand at it), and it is polled from the main loop -- so sustained output rate
** is buf_size * actual_frame_rate. At 512 frames that is only 23ms of audio per
** poll, so any frame slower than ~43Hz starves the ring: the PWM IRQ then holds
** last_sample, which stretches AND chops the audio (heard on device as "way too
** slow" plus a broken-tractor buzz). Because the polled player is a PCM feed,
** the song tempo follows the sample clock and drags with it.
**
** buf_size is squeezed from BOTH SIDES. Device-measured on current master:
**
**   rate/11 (1002)  SQ3: NO SOUND AT ALL   PQ2: dies at vocab init
**   rate/30 ( 367)  SQ3: plays             PQ2: dies later, at decompress
**
** TOO SMALL starves the mixer; TOO LARGE does not fit. rate/11 was correct on
** 2026-09-06, but master has since gained the composed surface and more, and the
** extra ~7.6KB (compbuf 2*N*4 + feed ~4*N) no longer fits -- the SQ3 log shows
** the song's own "malloc 19008 failed", which SUCCEEDS on retry at rate/30. So
** rate/30 it is: inside the window, where rate/11 no longer is.
**
** The starvation floor below is real and still the reason not to go much lower;
** it simply was not the binding constraint. Both limits must be respected, and
** the window between them is narrow -- re-measure if the memory picture changes.
** Tried rate/30 on 2026-09-12 on the theory that polls run at ~60Hz post-fix, so
** the floor would be rate/60. The DEVICE SAYS OTHERWISE. soft.c's own
** "[sfx-mixer] Output starving: demand N > buf_size" lines give the real poll
** rate directly, and on SQ3 they read:
**
**     demand  402 -> 36ms frame  -> 28Hz   <- ORDINARY frames
**     demand  496 -> 44ms frame  -> 23Hz
**     demand 2400 -> 217ms frame -> 4.6Hz
**     demand 9339 -> 847ms frame -> 1.2Hz  <- song load / room decode
**
** So the steady-state poll rate is 22-28Hz, NOT 60Hz: the real floor is about
** rate/22 ~ 500, and rate/30 = 367 sits BELOW it -- ordinary frames starve, the
** PWM IRQ holds last_sample, and the audio sticks (heard on device). rate/11
** clears the ordinary case with ~2x margin.
**
** The multi-hundred-ms stalls cannot be covered by ANY sane buf_size (9339
** frames would be 112KB of compbuf); those are the ring's job, and starvation
** lines for them are expected, not a fault.
**
** Raising this cannot be replaced by polling more often: the mixer sizes each
** batch from elapsed WALL-CLOCK time, so back-to-back calls compute ~0 frames.
** Costs ~12 bytes/frame (compbuf 2*N*4 in mixer/soft.c plus feed buf ~4*N):
** 12.0KB at 11025, 24.0KB at 22050. That looks like the cheapest large saving
** available and it is NOT -- it is load-bearing. Override with
** -DPICO_SND_BUF_FRAMES=N only with the starving lines in hand. */
#ifdef PICO_SND_BUF_FRAMES
#  define PICO_PWM_BUF_FRAMES PICO_SND_BUF_FRAMES
#else
#  define PICO_PWM_BUF_FRAMES (PICO_SND_RATE / 30)
#endif

/* Diagnostic: compare PRODUCTION (pushed) against CONSUMPTION (IRQs). Both
   should sit at ~22050/s. Whichever one is low is the actual fault, which four
   rounds of reading the code failed to settle. */
extern volatile uint32_t pwm_irq_count;
extern volatile uint32_t pwm_underrun_count;
static uint32_t snd_pushed = 0, snd_dropped = 0;
/* Poll rate decides the minimum safe buf_size (the mixer emits at most
   buf_size per call, so buf_size >= rate / poll_rate). Steady-state totals
   alone cannot distinguish "few capped calls" from "many small ones", and that
   distinction is exactly what sizing the buffers depends on. */
static uint32_t snd_polls = 0;
/* Microseconds spent inside the softseq generating samples, and how many it
   produced. If this approaches 1,000,000 per report the synth is the wall. */
unsigned long long pico_seq_poll_us = 0;
unsigned long pico_seq_poll_frames = 0;
static unsigned long long snd_last_report_us = 0;
static long snd_report_secs = 0;

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
		if (!pwm_synth_push_sample((uint8_t) s)) {
			snd_dropped += (count - i);
			break; /* ring full: drop the rest, IRQ will catch up */
		}
		snd_pushed++;
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

	snd_polls++;
	pico_sfx_timer_callback(pico_sfx_timer_data);

	/* Once per second: the two rates that decide everything. */
#ifdef FSCI_PROBE_SND
	if (secs != snd_report_secs) {
		snd_report_secs = secs;
		{
			extern unsigned long long pico_perf_us(void);
			unsigned long long now_us = pico_perf_us();
			unsigned long long span = now_us - snd_last_report_us;
			if (!span) span = 1;
			snd_last_report_us = now_us;
			/* NB rates are per REPORT INTERVAL, which is only ~1s in
			   steady state -- span_ms makes a long interval obvious
			   instead of it looking like an impossible sample rate. */
			sciprintf("[snd] span=%lums polls=%u produced=%u consumed=%u"
				  " underrun=%u ring=%d | seq=%lums for %lu frames\n",
				  (unsigned long)(span / 1000), (unsigned)snd_polls,
				  (unsigned)snd_pushed, (unsigned)pwm_irq_count,
				  (unsigned)pwm_underrun_count,
				  pwm_synth_ring_pending(),
				  (unsigned long)(pico_seq_poll_us / 1000),
				  pico_seq_poll_frames);
		}
		snd_pushed = 0;
		snd_dropped = 0;
		snd_polls = 0;
		pwm_irq_count = 0;
		pwm_underrun_count = 0;
		pico_seq_poll_us = 0;
		pico_seq_poll_frames = 0;
	}
#else
	(void)snd_report_secs;
#endif
}

#endif /* PICO_PWM_AUDIO */
