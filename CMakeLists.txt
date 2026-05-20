cmake_minimum_required(VERSION 3.16)
project(freesci C)

set(PLATFORM "desktop" CACHE STRING "Target platform: desktop or pico")

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckTypeSize)

# ── Platform setup ────────────────────────────────────────────────────────────
if(PLATFORM STREQUAL "desktop")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(HAVE_SDL 1)
    set(SCI_PLATFORM_DESKTOP 1)
elseif(PLATFORM STREQUAL "pico")
    set(SCI_PLATFORM_PICO 1)
    if(DEFINED ENV{PICO_SDK_PATH})
        include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)
        pico_sdk_init()
    else()
        message(WARNING "PICO_SDK_PATH not set — Pico build will be incomplete")
    endif()
else()
    message(FATAL_ERROR "Unknown PLATFORM '${PLATFORM}'. Use 'desktop' or 'pico'.")
endif()

# ── Feature detection ─────────────────────────────────────────────────────────
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

# ── Generate config.h ─────────────────────────────────────────────────────────
configure_file(cmake/config.h.in config.h)

# ── Global compile flags ──────────────────────────────────────────────────────
# HAVE_CONFIG_H must be set via compiler flag so the guard in scitypes.h works
add_compile_definitions(HAVE_CONFIG_H=1)
# Disable Xlib driver — SDL2 is the display backend
add_compile_definitions(X_DISPLAY_MISSING=1)
# Old code uses GNU C89-style inline (non-static inline emits external symbol)
add_compile_options(-fgnu89-inline -w)

# ── Global include directories ────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_BINARY_DIR}          # for generated config.h
)

add_subdirectory(src)
