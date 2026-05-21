set(driver_sources
    gfx_drivers.c
    null_driver.c
)

if(PLATFORM STREQUAL "desktop")
    list(APPEND driver_sources sdl_driver.c)
elseif(PLATFORM STREQUAL "pico")
    list(APPEND driver_sources pico_driver.c)
endif()

add_library(scidrivers STATIC ${driver_sources})

if(PLATFORM STREQUAL "desktop")
    target_compile_options(scidrivers PRIVATE ${SDL2_CFLAGS_OTHER})
    target_include_directories(scidrivers PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(scidrivers PRIVATE HAVE_SDL=1)
elseif(PLATFORM STREQUAL "pico")
    target_compile_definitions(scidrivers PRIVATE HAVE_PICO=1)
    target_include_directories(scidrivers PRIVATE
        ${PICOCALC_HW_DIR}/i2ckbd
        ${PICOCALC_HW_DIR}/lcdspi
        ${CMAKE_SOURCE_DIR}/src/platform/pico   # kbd_input.h
        ${FATFS_DIR}/ff15/source                # ff.h
    )
    target_link_libraries(scidrivers PRIVATE scipicoplatform)
endif()
