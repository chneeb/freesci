/* pico_io.c — newlib syscall stubs backed by FatFS.
   FreeSCI uses open/read/lseek/close/stat via newlib.  The pico-sdk stubs
   these to -1/ENOSYS by default.  We override the low-level _open/_read/
   _lseek/_close/_fstat/_isatty hooks so that all C-library file I/O routes
   through FatFS automatically.

   Supported open flags: O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC.
   Limited fstat: st_size only (enough for FreeSCI resource loading).
*/

#include "pico/stdlib.h"   /* bool, true, false, putchar_raw */
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "ff.h"

#define MAX_FDS 16

/* fd 0/1/2 are stdin/stdout/stderr — we don't intercept those */
#define FD_OFFSET 3

static FIL   fat_files[MAX_FDS];
static bool  fat_open[MAX_FDS];

static int alloc_fd(void)
{
    for (int i = 0; i < MAX_FDS; i++)
        if (!fat_open[i]) return i;
    return -1;
}

int _open(const char *path, int flags, ...)
{
    int idx = alloc_fd();
    if (idx < 0) { errno = EMFILE; return -1; }

    BYTE mode = 0;
    if ((flags & O_ACCMODE) == O_RDONLY)  mode = FA_READ;
    else if ((flags & O_ACCMODE) == O_WRONLY) mode = FA_WRITE;
    else                                       mode = FA_READ | FA_WRITE;
    if (flags & O_CREAT)  mode |= FA_OPEN_ALWAYS;
    if (flags & O_TRUNC)  mode |= FA_CREATE_ALWAYS;

    FRESULT r = f_open(&fat_files[idx], path, mode);
    if (r != FR_OK) { errno = ENOENT; return -1; }

    fat_open[idx] = true;
    return idx + FD_OFFSET;
}

int _close(int fd)
{
    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_FDS || !fat_open[idx]) { errno = EBADF; return -1; }
    f_close(&fat_files[idx]);
    fat_open[idx] = false;
    return 0;
}

_READ_WRITE_RETURN_TYPE _read(int fd, void *buf, size_t len)
{
    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_FDS || !fat_open[idx]) { errno = EBADF; return -1; }
    UINT br = 0;
    FRESULT r = f_read(&fat_files[idx], buf, (UINT)len, &br);
    if (r != FR_OK) { errno = EIO; return -1; }
    return (int)br;
}

_READ_WRITE_RETURN_TYPE _write(int fd, const void *buf, size_t len)
{
    /* Pass stdout/stderr through to UART */
    if (fd == 1 || fd == 2) {
        const char *p = buf;
        for (size_t i = 0; i < len; i++) putchar_raw(p[i]);
        return (int)len;
    }
    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_FDS || !fat_open[idx]) { errno = EBADF; return -1; }
    UINT bw = 0;
    FRESULT r = f_write(&fat_files[idx], buf, (UINT)len, &bw);
    if (r != FR_OK) { errno = EIO; return -1; }
    return (int)bw;
}

off_t _lseek(int fd, off_t offset, int whence)
{
    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_FDS || !fat_open[idx]) { errno = EBADF; return -1; }

    FSIZE_t pos;
    switch (whence) {
    case SEEK_SET: pos = (FSIZE_t)offset; break;
    case SEEK_CUR: pos = f_tell(&fat_files[idx]) + (FSIZE_t)offset; break;
    case SEEK_END: pos = f_size(&fat_files[idx]) + (FSIZE_t)offset; break;
    default: errno = EINVAL; return -1;
    }
    FRESULT r = f_lseek(&fat_files[idx], pos);
    if (r != FR_OK) { errno = EIO; return -1; }
    return (off_t)f_tell(&fat_files[idx]);
}

int _fstat(int fd, struct stat *st)
{
    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_FDS || !fat_open[idx]) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)f_size(&fat_files[idx]);
    st->st_mode = S_IFREG;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)fno.fsize;
    st->st_mode = (fno.fattrib & AM_DIR) ? S_IFDIR : S_IFREG;
    return 0;
}

int _isatty(int fd)
{
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Directory / filesystem helpers                                       */
/* ------------------------------------------------------------------ */

int _unlink(const char *path)
{
    FRESULT r = f_unlink(path);
    if (r != FR_OK) { errno = (r == FR_NO_FILE) ? ENOENT : EIO; return -1; }
    return 0;
}

int _rmdir(const char *path)
{
    /* FatFS f_unlink removes empty directories too */
    return _unlink(path);
}

int _mkdir(const char *path, mode_t mode)
{
    (void)mode;
    FRESULT r = f_mkdir(path);
    if (r != FR_OK) { errno = (r == FR_EXIST) ? EEXIST : EIO; return -1; }
    return 0;
}

/* Directory iteration — pico_dir_t wraps FatFS DIR + current dirent.
   dirent.h (in this directory) defines DIR as FatFS's DIR, so callers
   that include <dirent.h> get our fake header and deal in FatFS DIR*.
   opendir() allocates a pico_dir_t (FatFS DIR is first member, so a
   plain DIR* cast is safe) and returns it as DIR*. */
#include <dirent.h>   /* our fake dirent.h — uses FatFS DIR */
#include <stdlib.h>

typedef struct {
    DIR            ff_dir;  /* FatFS DIR — MUST be first member */
    struct dirent  cur;
} pico_dir_t;

DIR *opendir(const char *path)
{
    pico_dir_t *pd = calloc(1, sizeof(*pd));
    if (!pd) { errno = ENOMEM; return NULL; }
    if (f_opendir(&pd->ff_dir, path) != FR_OK) { free(pd); errno = ENOENT; return NULL; }
    return (DIR *)pd;   /* safe: ff_dir is first member */
}

struct dirent *readdir(DIR *dirp)
{
    pico_dir_t *pd = (pico_dir_t *)dirp;
    FILINFO fno;
    if (f_readdir(&pd->ff_dir, &fno) != FR_OK || fno.fname[0] == '\0')
        return NULL;
    strncpy(pd->cur.d_name, fno.fname, sizeof(pd->cur.d_name) - 1);
    pd->cur.d_name[sizeof(pd->cur.d_name) - 1] = '\0';
    return &pd->cur;
}

int closedir(DIR *dirp)
{
    pico_dir_t *pd = (pico_dir_t *)dirp;
    f_closedir(&pd->ff_dir);
    free(pd);
    return 0;
}
int mkdir(const char *p, mode_t m) { return _mkdir(p, m); }
int unlink(const char *p)        { return _unlink(p); }
int rmdir(const char *p)         { return _rmdir(p); }

/* getcwd stub — FreeSCI uses it for the work dir */
char *getcwd(char *buf, size_t size)
{
    if (!buf || size < 2) { errno = EINVAL; return NULL; }
    /* Return the current FatFS working directory */
    if (f_getcwd(buf, (UINT)size) != FR_OK) {
        strncpy(buf, "0:", size);
        buf[size - 1] = '\0';
    }
    return buf;
}

int chdir(const char *path)
{
    FRESULT r = f_chdir(path);
    if (r != FR_OK) { errno = ENOENT; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Additional POSIX stubs                                               */
/* ------------------------------------------------------------------ */

int creat(const char *path, mode_t mode)
{
    (void)mode;
    return _open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

/* sleep()/usleep() — FreeSCI engine uses sleep() in scriptdebug */
unsigned int sleep(unsigned int seconds)
{
    sleep_ms(seconds * 1000);
    return 0;
}

int usleep(unsigned int usecs)
{
    sleep_us(usecs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* fnmatch — minimal glob matching for SCI resource scanning           */
/* Only needs to handle patterns like "*.sc?", "resource.map", etc.   */
/* ------------------------------------------------------------------ */
#include "fnmatch.h"

/* Case-insensitive single-char match used by our fnmatch */
static int ci_match(int c1, int c2)
{
    if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
    if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
    return c1 == c2;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    /* We ignore most flags; FNM_NOESCAPE, FNM_CASEFOLD etc. are nops here */
    (void)flags;
    for (;;) {
        switch (*pattern) {
        case '\0':
            return (*string == '\0') ? 0 : FNM_NOMATCH;
        case '*':
            /* skip consecutive stars */
            while (pattern[1] == '*') pattern++;
            pattern++;
            if (*pattern == '\0') return 0; /* trailing * matches everything */
            for (; *string; string++)
                if (fnmatch(pattern, string, flags) == 0) return 0;
            return FNM_NOMATCH;
        case '?':
            if (*string == '\0') return FNM_NOMATCH;
            pattern++; string++;
            break;
        case '[': {
            int matched = 0, negated = 0;
            const char *p = pattern + 1;
            if (*p == '!') { negated = 1; p++; }
            for (; *p && *p != ']'; p++) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if ((unsigned char)*string >= (unsigned char)p[0] &&
                        (unsigned char)*string <= (unsigned char)p[2])
                        matched = 1;
                    p += 2;
                } else if (ci_match((unsigned char)*p, (unsigned char)*string)) {
                    matched = 1;
                }
            }
            if (*p != ']') return FNM_NOMATCH; /* malformed */
            if (matched == negated) return FNM_NOMATCH;
            pattern = p + 1; string++;
            break;
        }
        default:
            if (!ci_match((unsigned char)*pattern, (unsigned char)*string))
                return FNM_NOMATCH;
            pattern++; string++;
            break;
        }
    }
}
