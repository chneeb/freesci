cmake_minimum_required(VERSION 3.16)

set(PLATFORM "desktop" CACHE STRING "Target platform: desktop or pico")

# Memory-mapped QMI PSRAM target (Pimoroni Pico Plus 2, RP2350B) instead of the
# PicoCalc's PIO-SPI PSRAM. Declared here (before project()) because it selects
# the board the SDK imports. Default OFF -> unchanged PicoCalc build. See
# src/platform/pico/psram_mapped.c and CLAUDE.md "Pimoroni mapped-PSRAM target".
option(PICO_PSRAM_MAPPED "Use memory-mapped QMI PSRAM (Pimoroni Pico Plus 2)" OFF)

# ── Pico SDK must be imported BEFORE project() so the cross-compiler is found ──
if(PLATFORM STREQUAL "pico")
    # Always build as pico2 (RP2350A config), even for the mapped-PSRAM target on
    # the Pimoroni Pico Plus 2 (RP2350B).  The device-tested frank-snes firmware
    # runs on the exact same board+PicoCalc built as pico2 and drives GPIO 47 for
    # the PSRAM chip-select at runtime (the register write works on the B silicon
    # regardless of the compile-time variant).  The pimoroni_pico_plus2_rp2350
    # board config pulls in a 16 MB-flash setup that does NOT boot cleanly here
    # (early crash, TFT noise, no serial) — pico2 is the known-good flash config
    # our PicoCalc build already boots with. See CLAUDE.md "Pimoroni mapped-PSRAM".
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

    # ── Memory census: provide our own pico_malloc so the SDK skips its malloc.c ──
    # pico-sdk's pico_malloc/CMakeLists.txt is guarded by `if(NOT TARGET pico_malloc)`,
    # so defining the target here preempts it: the SDK never compiles its
    # WRAPPER_FUNC __wrap_malloc/calloc/realloc/free (which would multiply-define ours,
    # since pico_malloc adds malloc.c as an INTERFACE source compiled straight into the
    # executable, not an archive). pico_mem_census.c then owns those symbols. We re-add
    # the --wrap linker flags pico_wrap_function would have set so __real_* still resolve
    # to picolibc's allocator. See src/platform/pico/pico_mem_census.c for rationale.
    # Mirror the SDK's two targets (pico_add_library would have made both): the
    # _headers target is referenced by the SDK's header aggregate, so it must exist.
    add_library(pico_malloc_headers INTERFACE)
    target_include_directories(pico_malloc_headers SYSTEM INTERFACE
        ${PICO_SDK_PATH}/src/rp2_common/pico_malloc/include)
    add_library(pico_malloc INTERFACE)
    target_link_libraries(pico_malloc INTERFACE pico_malloc_headers pico_sync)
    target_sources(pico_malloc INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/pico/pico_mem_census.c)
    target_link_options(pico_malloc INTERFACE
        "LINKER:--wrap=malloc"
        "LINKER:--wrap=calloc"
        "LINKER:--wrap=realloc"
        "LINKER:--wrap=free")

    # Make USB-CDC stdout non-blocking: default is a 500 ms per-write wait when the
    # host has enumerated the port but nothing is draining it (PicoCalc is
    # USB-powered, so the OS opens the CDC even with no terminal). FreeSCI prints
    # heavily during the intro, so that stalls the game until a console attaches and
    # drains the buffer. 0 = drop instead of wait (debug output is best-effort).
    # Set before pico_sdk_init() so the SDK's stdio_usb.c picks it up. Not
    # SDK-version-specific — the stdio_usb defaults/logic are identical in 2.2.0/2.3.0.
    add_compile_definitions(PICO_STDIO_USB_STDOUT_TIMEOUT_US=0)

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

# ── Diagnostic probe toggles ──────────────────────────────────────────────────
# Compile-time gates for the leftover instrumentation kept in the tree for
# bringing up OTHER games. Top-level (not Pico-scoped) so the desktop mirror
# probes ([dpcol]/[dpblit] in gfx_support.c/operations.c, desktop_mem_probe in
# kgraphics.c) are reachable on desktop builds too. Pico-only probes additionally
# require HAVE_PICO in their own #if. Default OFF for clean builds, EXCEPT the
# string-overflow probe which guards the still-open "aspb" heap-corruption bug.
#   FSCI_PROBE_STR        — [strprobe]: SCI string kernels writing past dest size
#   FSCI_PROBE_GFX        — [pcol]/[ctl]/[oc]/[pblit] + desktop [dpcol]/[dpblit]
#   FSCI_PROBE_MEM        — [mem] BREAKDOWN/PXM/room lines (+ desktop_mem_probe)
#   FSCI_PROBE_MEM_CENSUS — [mem] CENSUS/SITES + the --wrap malloc histogram
#                           (~27.6KB .bss; implies FSCI_PROBE_MEM for the dump)
#   FSCI_PROBE_PARSER     — [gnf] per-command GNF-rebuild transient size
#   FSCI_PROBE_ARENA      — [arenagrow] names the sci_* alloc that grows the
#                           heap arena (sbrk), pinpointing the restore-time
#                           +32KB/restore ratchet trigger (Pico only)
option(FSCI_PROBE_STR        "Probe: SCI string-kernel buffer overflows"        ON)
option(FSCI_PROBE_GFX        "Probe: priority/control/occlusion gfx tracing"    OFF)
option(FSCI_PROBE_MEM        "Probe: per-room SRAM/heap breakdown lines"        OFF)
option(FSCI_PROBE_MEM_CENSUS "Probe: live-alloc size histogram + call sites"    OFF)
option(FSCI_PROBE_PARSER     "Probe: per-command GNF-rebuild transient size"    OFF)
option(FSCI_PROBE_ARENA      "Probe: name the alloc that grows the heap arena"  OFF)
option(FSCI_PROBE_PERF       "Probe: [perf] per-room pic-decode timing (us)"    OFF)
if(FSCI_PROBE_MEM_CENSUS AND NOT FSCI_PROBE_MEM)
    message(STATUS "FSCI_PROBE_MEM_CENSUS implies FSCI_PROBE_MEM (census dumps in the breakdown) — enabling it")
    set(FSCI_PROBE_MEM ON)
endif()
if(FSCI_PROBE_STR)
    add_compile_definitions(FSCI_PROBE_STR=1)
endif()
if(FSCI_PROBE_GFX)
    add_compile_definitions(FSCI_PROBE_GFX=1)
endif()
if(FSCI_PROBE_MEM)
    add_compile_definitions(FSCI_PROBE_MEM=1)
endif()
if(FSCI_PROBE_MEM_CENSUS)
    add_compile_definitions(FSCI_PROBE_MEM_CENSUS=1)
endif()
if(FSCI_PROBE_PARSER)
    add_compile_definitions(FSCI_PROBE_PARSER=1)
endif()
if(FSCI_PROBE_ARENA)
    add_compile_definitions(FSCI_PROBE_ARENA=1)
endif()
if(FSCI_PROBE_PERF)
    add_compile_definitions(FSCI_PROBE_PERF=1)
endif()

if(PLATFORM STREQUAL "pico")
    add_compile_definitions(HAVE_PICO=1 HAVE_FNMATCH=1 HAVE_FNMATCH_H=1)

    # Decode the pic control map (collision detection + control-line scripts).
    # ON by default: needed for kCanBeHere collision (walls, control triggers).
    # The extra ~128KB transient decode peak can OOM tight rooms; if an unvisited
    # room OOMs, rebuild with -DPICO_CONTROL_MAP=OFF to keep playing (no collision)
    # while the peak is shrunk (bit-pack aux_map / offload priority map). See CLAUDE.md.
    option(PICO_CONTROL_MAP "Decode SCI control map on Pico (collision)" ON)
    if(PICO_CONTROL_MAP)
        add_compile_definitions(PICO_DECODE_CONTROL_MAP=1)
    endif()

    # Memory-mapped QMI PSRAM (Pimoroni Pico Plus 2). Option is declared at the top
    # of this file (it selects PICO_BOARD before pico_sdk_init); the compile define
    # is added here so psram_mapped.c / psram_qmi.h / pico_main.c see it. When ON,
    # src/platform/pico/CMakeLists.txt swaps the PIO PSRAM driver for the memcpy
    # backend. Default OFF = unchanged PicoCalc PIO PSRAM.
    if(PICO_PSRAM_MAPPED)
        add_compile_definitions(PICO_PSRAM_MAPPED=1)

        # Phase 2: route script_t.buf (the ~35-44KB/room VM bytecode+objvar+locals
        # working memory — the biggest single SRAM consumer, and the previously
        # "impossible" hot RW data) to the 6MB PSRAM heap. This is the make-or-break
        # perf test: the buf is read every VM instruction, so it exercises XIP
        # cache contention (flash code vs PSRAM data share the 16KB cache). Kept as
        # its own option so it can be A/B'd against SRAM-resident scripts
        # (-DPICO_PSRAM_SCRIPTS=OFF) to measure the slowdown, if any. Only meaningful
        # with PICO_PSRAM_MAPPED.
        option(PICO_PSRAM_SCRIPTS "Route script_t.buf to the PSRAM heap (mapped PSRAM only)" ON)
        if(PICO_PSRAM_SCRIPTS)
            add_compile_definitions(PICO_PSRAM_SCRIPTS=1)
        endif()

        # Give the mapped target a real STATIC visual buffer (the desktop
        # visual[2] analogue) instead of baking static draws into the PSRAM
        # background: kAddToPic picviews persist via an SRAM buffer, and the
        # room's background stays pristine so the add_to_pic overlay base is
        # never scribbled on. OFF falls back to the PIO behaviour (restore
        # straight from the PSRAM background), which is the A/B for deciding
        # whether a render regression belongs to this feature.
        option(PICO_STATIC_VISUAL "Use a dedicated static visual buffer (mapped PSRAM only)" ON)
        if(PICO_STATIC_VISUAL)
            add_compile_definitions(PICO_STATIC_VISUAL=1)
        endif()

        # Desktop-style per-frame priority maps. The PicoCalc keeps ONE priority
        # map, PSRAM-resident and aliased to static_priority_map, so the per-frame
        # copyback is a self-copy no-op and nothing ever writes sprite priority
        # back -- hence "last drawn wins" z-order, and the static-view bake that
        # stands a stopUpd view's priority in the map PERMANENTLY (which is what
        # over-occludes PQ2's officer and bleeds over its dialogs).
        #
        # With the engine moved to PSRAM there is SRAM for two real 320x200 maps
        # (128KB), which is what the desktop model needs: static_priority_map is
        # the clean plate, priority_map the working copy, restored from it every
        # frame so a view's priority is TRANSIENT. Supersedes the static-view
        # priority bake, so that routing is compiled out when this is on.
        option(PICO_WORKING_PRIORITY "Desktop-style per-frame priority maps (mapped PSRAM only)" ON)
        if(PICO_WORKING_PRIORITY)
            add_compile_definitions(PICO_WORKING_PRIORITY=1)
        endif()
    endif()

    # Static-view occlusion/persistence on the single Pico visual buffer. Two levels:
    #
    #   PICO_STATIC_VIEW_PRIORITY (default ON, the clean/free win): write a static
    #     view's PRIORITY into the PSRAM priority map so actors are occluded by it —
    #     kAddToPic picviews (PQ2 cars) AND settled stopUpd/NO_UPDATE dynviews (SQ3
    #     door/motivator). Zero new SRAM (reuses the row scratch the blit already
    #     reads priority through; +312 B flash). No color is persisted, so nothing
    #     ghosts: dynview color goes only to this frame's visual[0], never static_bg.
    #     Minor residual: a view that later animates leaves a stale (invisible)
    #     priority footprint (Roger slightly clipped by an opened door). ON because
    #     it is a strict occlusion improvement with no color regressions.
    #
    #   PICO_STATIC_VIEW_BAKE (default OFF, the regressing extra): additionally
    #     persist NO_UPDATE-dynview COLOR into static_bg so static items that never
    #     redraw stay visible (PQ2 glovebox registration/businessCard). The COLOR
    #     half is un-bakeable on one buffer — a baked view that later animates or is
    #     removed (SQ3 door opening, picked-up glovebox item, Pestulon-logo overlay)
    #     leaves a ghost, and baked foreground (car windshield / police-dept door)
    #     redraws over later dialog boxes. Needs save-unders we can't afford; kept as
    #     an opt-in experiment. Implies PRIORITY. See CLAUDE.md "Static-view occlusion".
    option(PICO_STATIC_VIEW_PRIORITY "Bake static-view priority on Pico (occlusion, no color)" ON)
    option(PICO_STATIC_VIEW_BAKE "Also persist NO_UPDATE-view color into static_bg (regresses dynamic scenes)" OFF)
    if(PICO_STATIC_VIEW_BAKE)
        add_compile_definitions(PICO_STATIC_VIEW_BAKE=1 PICO_STATIC_VIEW_PRIORITY=1)
    elseif(PICO_STATIC_VIEW_PRIORITY)
        add_compile_definitions(PICO_STATIC_VIEW_PRIORITY=1)
    endif()

    # THROWAWAY measurement build for roadmap #1 (text parser). When ON,
    # _init_vocabulary loads the full vocab once, prints its SRAM cost + the
    # GNF-build transient peak ([vocab] lines), then frees it and runs the game
    # parser-less as usual. OFF by default; flip on for one flash, read pico.log,
    # flip off. See CLAUDE.md "Pico roadmap #1".
    option(PICO_VOCAB_PROBE "Measure text-parser vocab SRAM cost on Pico" OFF)
    if(PICO_VOCAB_PROBE)
        add_compile_definitions(PICO_VOCAB_PROBE=1)
    endif()

    # Roadmap #1 Stage 2: pack the ~1489 per-word parser blocks into one
    # allocation (drops resident vocab ~53KB → ~33KB AND, more importantly, turns
    # ~1489-1843 scattered small allocs into one blob — a big fragmentation win;
    # an unpacked vocab shredded the free list badly enough to fail the 64KB
    # visual[0] alloc on a cross-game switch). Now DEFAULT ON — it is the shipping
    # config; the Stage-1 unpacked measurement is long done. -DPICO_PACK_VOCAB=OFF
    # to A/B. See CLAUDE.md "Pico roadmap #1".
    option(PICO_PACK_VOCAB "Pack parser word vocab into one allocation on Pico" ON)
    if(PICO_PACK_VOCAB)
        add_compile_definitions(PICO_PACK_VOCAB=1)
    endif()

    # PicoCalc PWM audio synth (tiny_agi waveform synth on GPIO 26/27).
    # OFF for now: FreeSCI sound is disabled (-q / SFX_STATE_FLAG_NOSOUND), so the
    # synth only ran an idle 22kHz IRQ outputting silence and reserved ~6.8KB SRAM
    # for its waveform table (audio/pwm_strings.h). OFF unlinks pwm_synth.c entirely
    # (full SRAM + flash recovery) and skips pwm_synth_init(). Flip ON to bring sound
    # back — it re-compiles the synth and re-arms init in one flag. See CLAUDE.md
    # "Sound on Pico (TODO)".
    # [snd] per-second production/consumption/underrun report. Diagnostic only;
    # it was what finally localised the starved sound poll.
    option(FSCI_PROBE_SND "Log PCM production vs consumption rates" OFF)
    if(FSCI_PROBE_SND)
        add_compile_definitions(FSCI_PROBE_SND=1)
    endif()

    option(PICO_PWM_AUDIO "Build + init PicoCalc PWM audio synth" OFF)
    # Which softsequencer drives the PCM feed. Empty = FreeSCI's default (opl2,
    # i.e. Adlib FM emulation -- the most expensive one). "SN76496" (Tandy 3-voice
    # square) and "pcspeaker" are far cheaper and are already linked; SCI0 sound
    # resources carry tracks for all of them.
    set(PICO_SOFTSEQ "" CACHE STRING "Softseq name on Pico: empty (opl2), SN76496, pcspeaker")
    if(PICO_PWM_AUDIO)
        add_compile_definitions(PICO_PWM_AUDIO=1)
        if(NOT PICO_SOFTSEQ STREQUAL "")
            add_compile_definitions(PICO_SOFTSEQ_NAME="${PICO_SOFTSEQ}")
        endif()
        # OPL2 lookup tables come from flash .rodata (fmopl_tables.c) instead of
        # SRAM malloc — zero-SRAM HQ tables. Gates fmopl.c's table provider and
        # adds the generated table file to scisoftseq (see src/sfx/softseq).
        add_compile_definitions(FMOPL_FLASH_TABLES=1)
    endif()
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
        ${CMAKE_SOURCE_DIR}/src/platform/pico   # fake dirent.h, fnmatch.h, psram_alloc.h
        ${CMAKE_BINARY_DIR}/src/platform/pico   # psram_spi.pio.h (generated by pico_generate_pio_header)
        ${PICOCALC_HW_DIR}/lcdspi               # lcdspi.h
        ${PICOCALC_HW_DIR}/i2ckbd               # i2ckbd.h
        ${FATFS_DIR}/ff15/source                # ff.h (needed by dirent.h and pico_io.c)
        ${FATFS_DIR}/sd_driver                  # sd_card.h, spi.h
    )
endif()

add_subdirectory(src)
