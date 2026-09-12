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
#   FSCI_PROBE_DIRTY      — [dirty] per-frame cel-draw / dirty-rect / buffer-
#                           propagate trace. Runs on DESKTOP to settle Pico
#                           single-buffer questions offline: the rect bookkeeping
#                           is shared code, so desktop executes it identically
#                           even though its 3 buffers hide the artifact. Env-gated
#                           at runtime by FREESCI_DIRTYPROBE=1.
#   FSCI_SIM_PICO_STATIC  — desktop-only: route settled stopUpd (NO_UPDATE)
#                           dynviews through the STATIC path, mirroring the PIO
#                           PICO_STATIC_VIEW_PRIORITY routing, so the same draw
#                           sequence can be traced offline. Inert to the desktop
#                           picture's correctness only in the sense that STATIC
#                           lands in the off-screen visual[2] — it DOES bake there,
#                           so expect desktop ghosting. Trace builds only.
option(FSCI_PROBE_STR        "Probe: SCI string-kernel buffer overflows"        ON)
option(FSCI_PROBE_GFX        "Probe: priority/control/occlusion gfx tracing"    OFF)
option(FSCI_PROBE_MEM        "Probe: per-room SRAM/heap breakdown lines"        OFF)
option(FSCI_PROBE_MEM_CENSUS "Probe: live-alloc size histogram + call sites"    OFF)
option(FSCI_PROBE_PARSER     "Probe: per-command GNF-rebuild transient size"    OFF)
option(FSCI_PROBE_ARENA      "Probe: name the alloc that grows the heap arena"  OFF)
option(FSCI_PROBE_PERF       "Probe: [perf] per-room pic-decode timing (us)"    OFF)
option(FSCI_PROBE_DIRTY      "Probe: [dirty] cel-draw/dirty-rect/propagate trace" OFF)
option(FSCI_SIM_PICO_STATIC  "Desktop: mirror the PIO stopUpd static routing"   OFF)
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
if(FSCI_PROBE_DIRTY)
    add_compile_definitions(FSCI_PROBE_DIRTY=1)
endif()
if(FSCI_SIM_PICO_STATIC)
    add_compile_definitions(FSCI_SIM_PICO_STATIC=1)
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
    # Route the FULLSCREEN-clipped static draw of a settled stopUpd dynview to
    # the composed PSRAM surface instead of the displayed visual[0] -- the
    # desktop two-destination model (visual[2] persistence + visual[1] display,
    # each with its own clip), expressible only now that the second PSRAM
    # surface exists. Targets the PQ2 dialog bleed AND the SQ3 door at once.
    # Default OFF: it also makes a stopUpd view's COLOUR persist, so it needs
    # the invalidation trigger (phase 2b) before it can be trusted not to ghost.
    # Default ON for the PIO target: device-validated on PQ2 (dialogs, glovebox
    # items, card pickup, lid, overlay, cars, HQ door all correct) and no worse
    # than baseline on SQ3, whose spaceship door does not visibly stay closed
    # either way -- priority-only never persisted a settled view's colour.
    # MEANINGLESS on the mapped target, which has PICO_STATIC_VISUAL and real
    # per-frame priority maps instead; the define is withheld there so the
    # widget-layer hooks cannot reference a driver function that target does not
    # build (pico_invalidate_static_region lives behind !PICO_USE_STATIC_VISUAL).
    if(PICO_PSRAM_MAPPED)
        option(PICO_STATIC_COMPOSED "Pico/PIO: fullscreen static draws -> composed surface" OFF)
    else()
        option(PICO_STATIC_COMPOSED "Pico/PIO: fullscreen static draws -> composed surface" ON)
    endif()
    if(PICO_STATIC_VIEW_BAKE)
        add_compile_definitions(PICO_STATIC_VIEW_BAKE=1 PICO_STATIC_VIEW_PRIORITY=1)
    elseif(PICO_STATIC_VIEW_PRIORITY)
        add_compile_definitions(PICO_STATIC_VIEW_PRIORITY=1)
    endif()
    if(PICO_STATIC_COMPOSED AND NOT PICO_PSRAM_MAPPED)
        add_compile_definitions(PICO_STATIC_COMPOSED=1)
    endif()

    # THROWAWAY measurement build for roadmap #1 (text parser). When ON,
    # _init_vocabulary loads the full vocab once, prints its SRAM cost + the
    # GNF-build transient peak ([vocab] lines), then frees it and runs the game
    # parser-less as usual. OFF by default; flip on for one flash, read pico.log,
    # flip off. See CLAUDE.md "Pico roadmap #1".
    # 4bpp groundwork. SCI0's visual buffer is 8bpp because each byte holds a
    # DITHER PAIR (two EGA indices blended by INTERCOL, hence the 256-entry
    # gfx_sci0_pic_colors). GFXR_DITHER_MODE_D16 -- "Sierra SCI style", what the
    # original hardware showed -- collapses that to ONE index per pixel with a
    # spatial checkerboard, so every value is 0..15 and the buffer can eventually
    # be nibble-packed (64KB -> 32KB, PIO-only value; see CLAUDE.md).
    #
    # FORCES the mode rather than defaulting it, deliberately: pic0_dither_mode is
    # a config-file option (config.l), and once the buffer is packed a stray
    # "pic0_dither_mode = d256" on the SD card would write values >15 into nibbles
    # and silently corrupt the display. A packed buffer and a runtime-selectable
    # dither mode cannot coexist.
    option(PICO_DITHER_D16 "Pico: force D16 dithering (one index per pixel, 0..15)" OFF)
    if(PICO_DITHER_D16)
        add_compile_definitions(PICO_DITHER_D16=1)
    endif()

    # Nibble-pack the visual buffer: 64000 -> 32000 bytes of SRAM, the point of
    # the whole D16 exercise on the SRAM-bound PIO target. REQUIRES D16 (a nibble
    # holds 0..15), so it forces it on rather than letting the two disagree.
    #
    # Default OFF and UNVERIFIED ON DEVICE. The decode half (visdiff) and the
    # driver half (drvdiff) are each verified offline, but nothing has yet
    # exercised them TOGETHER, and no hardware has run this at all -- PSRAM
    # timing, the real palette plumbing, and whether 16 slots suffice for text
    # and UI under D16 are all untested. Treat the first flash as an experiment.
    option(PICO_PACK_VISUAL "Pico/PIO: 4bpp nibble-packed visual buffer (needs D16)" OFF)
    if(PICO_PACK_VISUAL)
        if(PICO_PSRAM_MAPPED)
            message(FATAL_ERROR "PICO_PACK_VISUAL is PIO-only: the mapped target has SRAM to spare and a different render model")
        endif()
        add_compile_definitions(PICO_PACK_VISUAL=1 PICO_DITHER_D16=1)
        message(WARNING
            "PICO_PACK_VISUAL is KNOWN BROKEN and must not be flashed: the packed "
            "flood-fill aux produces wrong control maps (tests/picodiff: SQ3 pic 3 "
            "51917/64000 differ), so collision detection is wrong. See CLAUDE.md.")
    endif()

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
    # Frame rate == the sound poll rate, and works WITHOUT sound -- so a target
    # can be measured before deciding whether audio fits on it.
    option(FSCI_PROBE_FPS "Log per-frame rate (sets the minimum audio buf_size)" OFF)
    if(FSCI_PROBE_FPS)
        add_compile_definitions(FSCI_PROBE_FPS=1)
    endif()

    option(FSCI_PROBE_SND "Log PCM production vs consumption rates" OFF)
    if(FSCI_PROBE_SND)
        add_compile_definitions(FSCI_PROBE_SND=1)
    endif()

    # ---- Target-INDEPENDENT knobs. These must sit at pico-block level, NOT
    # inside if(PICO_PSRAM_MAPPED): they apply to both targets, and nesting them
    # there silently gave the PIO build the fallback values baked into the C
    # sources (133 MHz / 12500 kHz) while CMakeCache reported what was asked for.
    # The runtime "[clk] ... requested N MHz" line exists to catch exactly that.
    set(PICO_LCD_SPI_KHZ "25000" CACHE STRING "LCD SPI clock in kHz (25000 default)")
    add_compile_definitions(LCD_SPI_SPEED=${PICO_LCD_SPI_KHZ}*1000)

    # SD SPI clock in kHz. Resource loading includes SD reads, which the core
    # clock does NOT speed up -- an independent lever. frank-quest runs 30000 on
    # the same class of card without DMA (we have DMA). Init negotiates at 400 kHz
    # first, so a card that cannot sustain it fails at mount, not silently.
    set(PICO_SD_SPI_KHZ "30000" CACHE STRING "SD SPI clock in kHz (30000; 12500 was the old default)")
    add_compile_definitions(PICO_SD_SPI_KHZ=${PICO_SD_SPI_KHZ})

    # Core clock. 252 is device-confirmed on the MAPPED target (memory-mapped QMI
    # PSRAM re-derives its own divisor from clk_sys, so it re-caps itself safely).
    # PIO stays at 133: its bit-banged PIO-SPI PSRAM fails the boot smoke test at
    # 252 even with the divisor scaled to hold the SPI at its proven rate, and
    # again with an integer divisor to remove fractional-clock jitter. See
    # CLAUDE.md "Overclocking". Overridable either way.
    if(PICO_PSRAM_MAPPED)
        set(PICO_SYS_CLOCK_MHZ "252" CACHE STRING "Core clock in MHz")
    else()
        set(PICO_SYS_CLOCK_MHZ "133" CACHE STRING "Core clock in MHz (PIO: 252 fails, see CLAUDE.md)")
    endif()
    add_compile_definitions(PICO_SYS_CLOCK_MHZ=${PICO_SYS_CLOCK_MHZ})

    # Sound defaults ON for the mapped-PSRAM target (where it works: engine in
    # PSRAM leaves ~110KB of SRAM, and music plays correctly) and OFF for the
    # PicoCalc PIO target (where it is marginal -- see CLAUDE.md "Sound on the
    # PIO target"). Explicitly overridable either way.
    if(PICO_PSRAM_MAPPED)
        # Core clock in MHz. 133 is the RP2350 power-on default. Raising it scales
    # resource loading almost directly (decompression + decode + interpreter are
    # CPU-bound); it does NOT affect display bandwidth. 252 needs no voltage
    # change; >=378 needs vreg_set_voltage, which is not wired up here.
    # WARNING: on the PIO target the PSRAM clkdiv is hard-coded to 1.0 (runs at
    # full clk_sys), so raising this there overclocks the PSRAM chip -- scale
    # that divisor first. The mapped target re-derives PSRAM timing from clk_sys.
    # Display speedups, independent of the core clock (which does nothing for
    # display bandwidth): 16-bit RGB565 over SPI instead of 18-bit/3-byte, plus
    # 50 MHz SPI instead of 25. Together ~3x fewer bytes-per-second on the panel.
    # Failure mode is obvious and recoverable (garbled/blank display, reflash).
    # 16-bit RGB565 pixel format instead of 18-bit/3-byte. INCOMPLETE and OFF:
    # switching the panel changes the byte count for EVERY writer, and only
    # pico_driver.c's flush_region was converted -- pico_clear_screen_black still
    # sends XSIZE*3, and lcdspi.c has ~66 of its own writes (chooser menu, text,
    # the OOM/HardFault dumps). Device result: sheared/distorted display. The
    # panel itself is fine with 0x65 (proven by shapones and frank-quest); the
    # conversion just has to cover all writers before this is usable.
    option(PICO_LCD_16BIT "16-bit pixel format (INCOMPLETE -- distorts, see comment)" OFF)
    if(PICO_LCD_16BIT)
        add_compile_definitions(PICO_LCD_16BIT=1)
    endif()

    option(PICO_PWM_AUDIO "Build + init PicoCalc PWM audio synth" ON)
    else()
        option(PICO_PWM_AUDIO "Build + init PicoCalc PWM audio synth" OFF)
    endif()
    # Which softsequencer drives the PCM feed. Empty = FreeSCI's default (opl2,
    # i.e. Adlib FM emulation -- the most expensive one). "SN76496" (Tandy 3-voice
    # square) and "pcspeaker" are far cheaper and are already linked; SCI0 sound
    # resources carry tracks for all of them.
    set(PICO_SOFTSEQ "" CACHE STRING "Softseq name on Pico: empty (opl2), SN76496, pcspeaker")
    # Audio sample rate. 11025 halves both the OPL synth cost (33-47% CPU at
    # 22050) and every derived buffer, which is what a memory-tight target needs.
    set(PICO_SND_RATE "22050" CACHE STRING "Pico audio sample rate (22050 or 11025)")
    # Frames the mixer may emit per call. Default rate/11 (~91ms). The mixer is
    # capped at this per call, so it must exceed rate/poll_rate or the ring
    # starves; every 512 frames costs ~4KB of compbuf, so it is the main audio
    # memory knob on a tight target.
    set(PICO_SND_BUF_FRAMES "" CACHE STRING "Mixer frames per call (default: rate/11)")
    if(PICO_PWM_AUDIO)
        add_compile_definitions(PICO_PWM_AUDIO=1 PICO_SND_RATE=${PICO_SND_RATE})
        if(NOT PICO_SND_BUF_FRAMES STREQUAL "")
            add_compile_definitions(PICO_SND_BUF_FRAMES=${PICO_SND_BUF_FRAMES})
        endif()
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
