add_subdirectory(scicore)
add_subdirectory(engine)
add_subdirectory(gfx)
add_subdirectory(sfx)
add_subdirectory(menu)

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

if(PLATFORM STREQUAL "desktop")
    target_compile_options(freesci PRIVATE ${SDL2_CFLAGS_OTHER})
    target_include_directories(freesci PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(freesci ${SDL2_LIBRARIES})
    target_compile_definitions(freesci PRIVATE HAVE_SDL=1)
endif()
