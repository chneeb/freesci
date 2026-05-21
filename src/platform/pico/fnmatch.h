/* fnmatch.h — minimal POSIX fnmatch for FreeSCI on PicoCalc.
   Implementation is in pico_io.c. */
#ifndef _PICO_FNMATCH_H_
#define _PICO_FNMATCH_H_

#define FNM_NOMATCH  1
#define FNM_NOESCAPE 1
#define FNM_PATHNAME 2
#define FNM_PERIOD   4
#define FNM_CASEFOLD 16

int fnmatch(const char *pattern, const char *string, int flags);

#endif /* _PICO_FNMATCH_H_ */
