/* pico_time.c — sci_gettime() and sci_get_current_time() for RP2350 */

#include <resource.h>
#include "hardware/timer.h"

void sci_gettime(long *seconds, long *useconds)
{
    uint64_t now = time_us_64();
    *seconds  = (long)(now / 1000000ULL);
    *useconds = (long)(now % 1000000ULL);
}

void sci_get_current_time(GTimeVal *val)
{
    long sec, usec;
    sci_gettime(&sec, &usec);
    val->tv_sec  = sec;
    val->tv_usec = usec;
}

/* Always available on Pico: used by the [perf] decode timer, the sound probe,
   and the startup resource-load timing in main.c. */
#ifdef HAVE_PICO
/* Free-running microsecond counter for perf A/B measurements (e.g. pic-decode
   timing to compare flash-cache-on vs XIP-cache-as-RAM builds). */
unsigned long long pico_perf_us(void)
{
    return (unsigned long long)time_us_64();
}
#endif
