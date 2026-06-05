add_subdirectory(scicore)
add_subdirectory(engine)
add_subdirectory(gfx)
add_subdirectory(sfx)
add_subdirectory(menu)

if(PLATFORM STREQUAL "pico")
    add_subdirectory(platform/pico)

    # pico_mem_census.c (our __wrap_malloc/calloc/realloc/free) is injected via the
    # pico_malloc INTERFACE target preempted in the top-level CMakeLists.txt, NOT listed
    # here — listing it would compile it twice and re-trigger a multiple-definition clash
    # with the wrap symbols. See the file header for the leak-hunt rationale.
    add_executable(freesci main.c config.c)
    # PICO_MALLOC_PANIC=0: let pico-sdk's malloc wrapper return NULL on OOM instead
    # of panic()ing immediately, so FreeSCI's own sci_malloc can report the failing
    # allocation site + free heap to the LCD (see pico_oom_report in pico_main.c).
    target_compile_definitions(freesci PRIVATE HAVE_PICO=1 PICO_MALLOC_PANIC=0)
    target_include_directories(freesci PRIVATE
        ${CMAKE_SOURCE_DIR}/src/platform/pico
    )
    target_link_libraries(freesci
        sciengine
        scigfx
        scigfxresources
        scidrivers
        scisound
        scicore
        sciplayer
        sciseq
        scitimer
        scipcm
        scimixer
        scisoftseq
        scisoundlib
        scisounddevice
        scimenu
        # Platform library last — provides sci_gettime/sci_get_current_time
        # and all hw drivers (i2ckbd, lcdspi, FatFS) via PUBLIC link
        scipicoplatform
        m
    )
    pico_enable_stdio_usb(freesci 1)   # USB CDC serial for debugging
    pico_enable_stdio_uart(freesci 1)  # keep UART too
    pico_add_extra_outputs(freesci)    # generates .uf2 / .bin / .elf
else()
    add_executable(freesci main.c config.c)
    target_link_libraries(freesci
        sciengine
        scigfx
        scigfxresources
        scidrivers
        scisound
        scicore
        sciplayer
        sciseq
        scitimer
        scipcm
        scimixer
        scisoftseq
        scisoundlib
        scisounddevice
        scimenu
        m
    )
endif()

if(PLATFORM STREQUAL "desktop")
    target_compile_options(freesci PRIVATE ${SDL2_CFLAGS_OTHER})
    target_include_directories(freesci PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(freesci ${SDL2_LIBRARIES})
    target_compile_definitions(freesci PRIVATE HAVE_SDL=1)
endif()
