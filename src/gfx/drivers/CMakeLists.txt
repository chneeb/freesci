set(driver_sources
    gfx_drivers.c
    null_driver.c
)

if(PLATFORM STREQUAL "desktop")
    list(APPEND driver_sources sdl_driver.c)
endif()

add_library(scidrivers STATIC ${driver_sources})

if(PLATFORM STREQUAL "desktop")
    target_compile_options(scidrivers PRIVATE ${SDL2_CFLAGS_OTHER})
    target_include_directories(scidrivers PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(scidrivers PRIVATE HAVE_SDL=1)
endif()
