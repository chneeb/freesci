#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#include "pwm_synth.h"

uint slice_num;

static const float sample_rate_hz = (float)PICO_SND_RATE;

/* Lock-free SPSC ring. head = next write (producer, main loop),
** tail = next read (consumer, PWM IRQ). One slot is left empty to
** distinguish full from empty. */
static volatile uint8_t pcm_ring[PWM_SYNTH_RING_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint8_t last_sample = 127; /* held during underrun to avoid clicks */

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
    last_sample = 127;
}

void pwm_synth_silence_all_channels(void) {
    ring_tail = ring_head;
    last_sample = 127;
}

/* Diagnostic counters (see pwm_synth_get_stats). irq_count is the REAL output
   sample rate -- if it is not ~22050/s the hardware side is at fault; underruns
   count IRQs that found the ring empty and had to hold last_sample, which is
   what stretches and chops the audio. */
volatile uint32_t pwm_irq_count = 0;
volatile uint32_t pwm_underrun_count = 0;

void __not_in_flash_func(pih)() {
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


void pwm_synth_init(int pwm_pin_base) {
    ring_head = ring_tail = 0;
    last_sample = 127;

    gpio_set_function(pwm_pin_base, GPIO_FUNC_PWM);
    gpio_set_function(pwm_pin_base + 1, GPIO_FUNC_PWM);

    // Find out which PWM slice is connected to pwm_pin_base
    slice_num = pwm_gpio_to_slice_num(pwm_pin_base);

    uint32_t clock_rate_hz = clock_get_hz(clk_sys);
    uint32_t wrap = 256;
    float divider = (clock_rate_hz / (float)wrap) / sample_rate_hz;

    pwm_set_clkdiv(slice_num, divider);
    pwm_set_wrap(slice_num, wrap);

    pwm_clear_irq(slice_num);
    pwm_set_irq_enabled(slice_num, true);

    irq_set_exclusive_handler(PWM_IRQ_WRAP, pih);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_set_enabled(slice_num, true);
}
