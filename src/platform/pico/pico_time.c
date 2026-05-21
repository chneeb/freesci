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
