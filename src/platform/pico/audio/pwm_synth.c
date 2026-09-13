#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#include "pwm_synth.h"

uint slice_num;

static const float sample_rate_hz = (float)PICO_SND_RATE;

/* PWM CARRIER OVERSAMPLING -- this is what silences the "shrill feep".
**
** The original divider made the PWM carrier EQUAL to the sample rate, so at
** PICO_SND_RATE=11025 the carrier was 11,025 Hz: squarely audible.  With the
** ring empty the IRQ holds last_sample=127, i.e. a 50% duty cycle, which is a
** clean 11 kHz SQUARE WAVE -- audible from the game chooser onward, with no
** game, no song and no underrun required.  (It was inaudible on the mapped
** target only because that runs at 22050, putting the carrier above hearing --
** not because anything there was better.)
**
** Fix: run the carrier CARRIER_MULT times faster and pop a sample only every
** CARRIER_MULT-th IRQ.  4x moves 11,025 -> 44,100 Hz, well out of hearing.
** `wrap` is untouched, so the 8-bit resolution is unchanged; the cost is that
** the IRQ fires 4x as often, and it was measured at ~1% CPU, so ~4%.
**
** NB the underrun/stretching problem is SEPARATE and this does not address it:
** that produces chopping, not a tone. */
#ifndef PICO_PWM_CARRIER_MULT
#  define PICO_PWM_CARRIER_MULT 4
#endif

/* The level the PWM sits at before any audio exists -- 0, NOT the 127 midpoint.
** THIS IS THE FIX FOR THE "SHRILL FEEP" (device-confirmed 2026-09-13).
**
** 127 is a 50% duty cycle, and on this hardware a sustained mid-level duty is
** AUDIBLE: the tone was present from the game chooser with no game, no song and
** no underrun.  Bisected by setting this to 0 -- feep gone, music unaffected --
** which proved the pin output is the source and the DUTY VALUE is what matters.
** Frequency is NOT the cause: moving the carrier 11,025 -> 44,100 Hz changed
** nothing (see PICO_PWM_CARRIER_MULT, which was built for a theory that turned
** out to be wrong).
**
** Safe because this is only the INITIAL/reset value.  During an underrun the
** IRQ holds the last REAL sample, not this, so the anti-click behaviour that
** 127 was chosen for is untouched; and during playback every sample comes from
** the ring.  The only cost is one DC step from 0 to the first real sample.
**
** STILL UNEXPLAINED: the mapped target at 22050 reportedly had no feep at idle
** 127.  The frequency theory that would have explained it is disproved, so the
** difference is not understood -- but 0 is silent on any hardware, so the fix
** is correct for both targets regardless. */
#ifndef PICO_PWM_IDLE_LEVEL
#  define PICO_PWM_IDLE_LEVEL 0
#endif

/* Lock-free SPSC ring. head = next write (producer, main loop),
** tail = next read (consumer, PWM IRQ). One slot is left empty to
** distinguish full from empty. */
static volatile uint8_t pcm_ring[PWM_SYNTH_RING_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint8_t last_sample = PICO_PWM_IDLE_LEVEL; /* held during underrun to avoid clicks */

#define RING_MASK (PWM_SYNTH_RING_SIZE - 1)

int pwm_synth_push_sample(uint8_t sample) {
    uint32_t head = ring_head;
    uint32_t next = (head + 1) & RING_MASK;
    if (next == ring_tail)
        return 0; /* full */
    pcm_ring[head] = sample;
    ring_head = next;
    return 1;
}

int pwm_synth_ring_pending(void) {
    return (int)((ring_head - ring_tail) & RING_MASK);
}

void pwm_synth_ring_reset(void) {
    ring_tail = ring_head;
    last_sample = PICO_PWM_IDLE_LEVEL;
}

void pwm_synth_silence_all_channels(void) {
    ring_tail = ring_head;
    last_sample = PICO_PWM_IDLE_LEVEL;
}

/* Diagnostic counters (see pwm_synth_get_stats). irq_count is the REAL output
   sample rate -- if it is not ~22050/s the hardware side is at fault; underruns
   count IRQs that found the ring empty and had to hold last_sample, which is
   what stretches and chops the audio. */
volatile uint32_t pwm_irq_count = 0;
volatile uint32_t pwm_underrun_count = 0;

void __not_in_flash_func(pih)() {
#if PICO_PWM_CARRIER_MULT > 1
    /* The carrier runs CARRIER_MULT times faster than the sample rate, so only
    ** every CARRIER_MULT-th IRQ advances the ring.  The intervening IRQs just
    ** re-assert the current level and clear the flag, which is what keeps the
    ** duty cycle steady between samples. */
    static uint32_t phase = 0;

    if (phase) {
        phase--;
        pwm_clear_irq(slice_num);
        return;
    }
    phase = PICO_PWM_CARRIER_MULT - 1;
#endif
    {
    uint32_t tail = ring_tail;
    pwm_irq_count++;
    if (tail == ring_head)
        pwm_underrun_count++;
    if (tail != ring_head) {
        last_sample = pcm_ring[tail];
        ring_tail = (tail + 1) & RING_MASK;
    }
    /* On underrun, hold last_sample (no new pop). */

    pwm_set_both_levels(slice_num, last_sample, last_sample);
    pwm_clear_irq(slice_num);
    }
}


void pwm_synth_init(int pwm_pin_base) {
    ring_head = ring_tail = 0;
    last_sample = PICO_PWM_IDLE_LEVEL;

    gpio_set_function(pwm_pin_base, GPIO_FUNC_PWM);
    gpio_set_function(pwm_pin_base + 1, GPIO_FUNC_PWM);

    // Find out which PWM slice is connected to pwm_pin_base
    slice_num = pwm_gpio_to_slice_num(pwm_pin_base);

    uint32_t clock_rate_hz = clock_get_hz(clk_sys);
    uint32_t wrap = 256;
    /* Carrier = sample_rate * CARRIER_MULT, NOT sample_rate -- see the comment
    ** on PICO_PWM_CARRIER_MULT. pwm_set_clkdiv tops out at ~255.94, and this
    ** only makes the divider SMALLER, so it stays in range. */
    float divider = (clock_rate_hz / (float)wrap)
                    / (sample_rate_hz * (float)PICO_PWM_CARRIER_MULT);

    pwm_set_clkdiv(slice_num, divider);
    pwm_set_wrap(slice_num, wrap);

    pwm_clear_irq(slice_num);
    pwm_set_irq_enabled(slice_num, true);

    irq_set_exclusive_handler(PWM_IRQ_WRAP, pih);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_set_enabled(slice_num, true);
}
