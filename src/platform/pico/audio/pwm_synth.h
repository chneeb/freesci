#pragma once

#include <stdint.h>
#include <stddef.h>

/* PicoCalc PWM audio backend.
**
** Originally a 3-channel sine synth (tiny_agi). For the FreeSCI port it is a
** PCM sink: the SCI mixer pushes 8-bit unsigned samples into a lock-free ring
** (single producer = main-loop poll context, single consumer = the 22050Hz
** PWM IRQ, which pops one sample per tick).
*/

#define PWM_SYNTH_RING_SIZE 8192 /* power of two; ~372ms at 22050Hz. Sized to ride
				    through main-loop stalls (a room decode is ~250ms
				    and drained the old 186ms ring, which showed up as
				    underrun>0 on exactly those seconds). 8KB .bss. */

extern void pwm_synth_init(int pwm_pin_base);

/* --- PCM ring (see pico_pwm.c) --- */

/* Enqueue one sample. Returns 1 if stored, 0 if the ring is full. */
extern int pwm_synth_push_sample(uint8_t sample);

/* Number of samples currently queued (written, not yet played). */
extern int pwm_synth_ring_pending(void);

/* Drop all queued samples (called on device init/exit). */
extern void pwm_synth_ring_reset(void);

/* Output silence and clear the ring. */
extern void pwm_synth_silence_all_channels(void);

/* Drive the SCI sound pipeline; call once per frame from the main loop. */
extern void pico_sfx_poll(void);
