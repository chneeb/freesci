add_subdirectory(resource)
add_subdirectory(drivers)

# alpha_mvi_crossblit.c must be compiled twice with different defines
add_library(alpha_mvi_32 OBJECT alpha_mvi_crossblit.c)
target_compile_definitions(alpha_mvi_32 PRIVATE FUNCT_NAME=alpha_mvi_crossblit_32)
target_include_directories(alpha_mvi_32 PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include ${CMAKE_BINARY_DIR})

add_library(alpha_mvi_32p OBJECT alpha_mvi_crossblit.c)
target_compile_definitions(alpha_mvi_32p PRIVATE PRIORITY=1 FUNCT_NAME=alpha_mvi_crossblit_32_P)
target_include_directories(alpha_mvi_32p PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include ${CMAKE_BINARY_DIR})

add_library(scigfx STATIC
    font.c
    gfx_resource.c
    gfx_support.c
    gfx_tools.c
    operations.c
    resmgr.c
    sbtree.c
    widgets.c
    menubar.c
    sci_widgets.c
    antialias.c
    gfx_console.c
    gfx_res_options.c
    font-5x8.c
    font-6x10.c
    $<TARGET_OBJECTS:alpha_mvi_32>
    $<TARGET_OBJECTS:alpha_mvi_32p>
)

if(PLATFORM STREQUAL "pico")
    target_link_libraries(scigfx PRIVATE scipicoplatform)
endif()
