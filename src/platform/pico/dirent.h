/* dirent.h — POSIX dirent emulation for FreeSCI on PicoCalc.
   Shadows newlib's <dirent.h> (which is unsupported on arm-none-eabi).
   POSIX DIR is implemented as a thin wrapper around FatFS's DIR struct.
   opendir/readdir/closedir are defined in pico_io.c. */

#ifndef _PICO_DIRENT_H_
#define _PICO_DIRENT_H_

#include "ff.h"   /* provides FatFS DIR typedef */

struct dirent {
    char d_name[FF_LFN_BUF + 1];
};

/* Implemented in pico_io.c using a pico_dir_t wrapper that extends DIR */
DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);

#endif /* _PICO_DIRENT_H_ */
