cmake_minimum_required(VERSION 3.16)

set(PLATFORM "desktop" CACHE STRING "Target platform: desktop or pico")

# ── Pico SDK must be imported BEFORE project() so the cross-compiler is found ──
if(PLATFORM STREQUAL "pico")
    set(PICO_BOARD "pico2" CACHE STRING "")

    if(DEFINED PICO_SDK_PATH)
        # passed via -DPICO_SDK_PATH=...
    elseif(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH is not set. "
            "Pass -DPICO_SDK_PATH=/path/to/pico-sdk or set the environment variable.")
    endif()
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
endif()

project(freesci C CXX ASM)

# ── Platform setup ────────────────────────────────────────────────────────────
if(PLATFORM STREQUAL "desktop")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(HAVE_SDL 1)
    set(SCI_PLATFORM_DESKTOP 1)

elseif(PLATFORM STREQUAL "pico")
    set(SCI_PLATFORM_PICO 1)
    set(HAVE_PICO 1)
    pico_sdk_init()

    # ── FatFS + SD SPI driver (vendored under src/platform/pico/fatfs/) ─────────
    set(FATFS_DIR ${CMAKE_SOURCE_DIR}/src/platform/pico/fatfs)
    # ── PicoCalc hw (i2ckbd + lcdspi) vendored under src/platform/pico/hw/ ─────
    set(PICOCALC_HW_DIR ${CMAKE_SOURCE_DIR}/src/platform/pico/hw)

else()
    message(FATAL_ERROR "Unknown PLATFORM '${PLATFORM}'. Use 'desktop' or 'pico'.")
endif()

# ── Feature detection (skip for pico — cross-compiler has different libc) ─────
if(PLATFORM STREQUAL "desktop")
    include(CheckIncludeFile)
    include(CheckFunctionExists)
    include(CheckTypeSize)

    check_include_file("unistd.h"     HAVE_UNISTD_H)
    check_include_file("dirent.h"     HAVE_DIRENT_H)
    check_include_file("sys/time.h"   HAVE_SYS_TIME_H)
    check_include_file("sys/uio.h"    HAVE_SYS_UIO_H)
    check_include_file("limits.h"     HAVE_LIMITS_H)
    check_include_file("fcntl.h"      HAVE_FCNTL_H)
    check_include_file("getopt.h"     HAVE_GETOPT_H)
    check_include_file("setjmp.h"     HAVE_SETJMP_H)
    check_include_file("fnmatch.h"    HAVE_FNMATCH_H)
    check_include_file("obstack.h"    HAVE_OBSTACK_H)
    check_include_file("sys/stat.h"   HAVE_SYS_STAT_H)

    check_function_exists(fork         HAVE_FORK)
    check_function_exists(fnmatch      HAVE_FNMATCH)
    check_function_exists(usleep       HAVE_USLEEP)
    check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
    check_function_exists(setitimer    HAVE_SETITIMER)
    check_function_exists(pipe         HAVE_PIPE)
    check_function_exists(socketpair   HAVE_SOCKETPAIR)
    check_function_exists(sched_yield  HAVE_SCHED_YIELD)
    check_function_exists(getopt_long  HAVE_GETOPT_LONG)
    check_function_exists(isblank      HAVE_ISBLANK)
    check_function_exists(unlink       HAVE_UNLINK)
    check_function_exists(rmdir        HAVE_RMDIR)
    check_function_exists(ffs          HAVE_FFS)
    check_function_exists(memchr       HAVE_MEMCHR)
    check_function_exists(memfrob      HAVE_MEMFROB)

    check_type_size("short" SIZEOF_SHORT)
    check_type_size("int"   SIZEOF_INT)
    check_type_size("long"  SIZEOF_LONG)
else()
    # Hard-code values appropriate for arm-none-eabi / newlib on RP2350
    set(HAVE_PICO         1)
    set(SCI_PLATFORM_PICO 1)
    set(HAVE_UNISTD_H     1)
    set(HAVE_DIRENT_H     1)   # newlib dirent.h present; _opendir stub in pico_io.c
    set(HAVE_LIMITS_H     1)
    set(HAVE_FCNTL_H      1)
    set(HAVE_SETJMP_H     1)
    set(HAVE_SYS_STAT_H   1)
    set(HAVE_FNMATCH      1)   # pico_io.c provides _fnmatch or HAVE_FNMATCH_H path
    set(HAVE_FNMATCH_H    1)
    set(HAVE_UNLINK       1)   # pico_io.c provides _unlink → FatFS
    set(HAVE_RMDIR        1)   # pico_io.c provides _rmdir → FatFS
    set(HAVE_FFS          1)   # newlib provides ffs()
    set(HAVE_MEMCHR       1)
    set(SIZEOF_SHORT      2)
    set(SIZEOF_INT        4)
    set(SIZEOF_LONG       4)
endif()

# ── Generate config.h ─────────────────────────────────────────────────────────
configure_file(cmake/config.h.in config.h)

# ── Global compile flags ──────────────────────────────────────────────────────
# HAVE_CONFIG_H must be set via compiler flag so the guard in scitypes.h works
add_compile_definitions(HAVE_CONFIG_H=1)
# Disable Xlib driver
add_compile_definitions(X_DISPLAY_MISSING=1)
# Old code uses GNU C89-style inline; -Wno-implicit-function-declaration needed for GCC 14+
# where implicit declarations became errors (not just warnings) — -w alone is insufficient.
add_compile_options(-fgnu89-inline -w -Wno-implicit-function-declaration)

if(PLATFORM STREQUAL "pico")
    add_compile_definitions(HAVE_PICO=1 HAVE_FNMATCH=1 HAVE_FNMATCH_H=1)
endif()

# ── Global include directories ────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}          # for generated config.h
)

if(PLATFORM STREQUAL "pico")
    # Our fake dirent.h must shadow newlib's (which has #error "<dirent.h> not supported").
    # Put platform/pico first so it wins the header search before arm-none-eabi sysroot.
    include_directories(BEFORE
        ${CMAKE_SOURCE_DIR}/src/platform/pico   # fake dirent.h, fnmatch.h
        ${PICOCALC_HW_DIR}/lcdspi               # lcdspi.h
        ${PICOCALC_HW_DIR}/i2ckbd               # i2ckbd.h
        ${FATFS_DIR}/ff15/source                # ff.h (needed by dirent.h and pico_io.c)
        ${FATFS_DIR}/sd_driver                  # sd_card.h, spi.h
    )
endif()

add_subdirectory(src)
